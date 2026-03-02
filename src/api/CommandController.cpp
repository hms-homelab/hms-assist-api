#include "api/CommandController.h"
#include <iostream>
#include <chrono>
#include <future>
#include <thread>
#include <vector>
#include <set>
#include <regex>

CommandController::CommandController(std::shared_ptr<IntentClassifier>    tier1,
                                     std::shared_ptr<IntentClassifier>    tier2,
                                     std::shared_ptr<IntentClassifier>    tier3,
                                     std::shared_ptr<DatabaseService>     db,
                                     std::shared_ptr<HomeAssistantClient> ha,
                                     const std::string&                   ttsEntity)
    : tier1_(tier1), tier2_(tier2), tier3_(tier3), db_(db), ha_(ha), ttsEntity_(ttsEntity) {}

void CommandController::handleCommand(const drogon::HttpRequestPtr& req,
                                       std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    auto bodyJson = req->getJsonObject();
    if (!bodyJson) {
        cb(errorResponse(400, "Request body must be valid JSON"));
        return;
    }

    if (!bodyJson->isMember("text") || !bodyJson->isMember("device_id")) {
        cb(errorResponse(400, "Required fields: text, device_id"));
        return;
    }

    VoiceCommand command;
    command.text      = (*bodyJson)["text"].asString();
    command.device_id = (*bodyJson)["device_id"].asString();
    command.confidence = (*bodyJson).get("confidence", 1.0f).asFloat();
    command.dry_run   = (*bodyJson).get("dry_run", false).asBool();
    command.context   = (*bodyJson).get("context", Json::Value()).toStyledString();

    if (command.text.empty()) {
        cb(errorResponse(400, "text cannot be empty"));
        return;
    }

    // Optional: media player entity for TTS spoken response
    std::string mediaPlayerEntityId =
        (*bodyJson).get("media_player_entity_id", "").asString();

    // Log the incoming command
    int commandId = -1;
    try {
        commandId = db_->logVoiceCommand(command);
    } catch (const std::exception& e) {
        std::cerr << "[CommandController] DB log failed: " << e.what() << std::endl;
    }

    // Run the classification pipeline
    IntentResult result = runPipeline(command);

    // Speak via TTS: HA confirmation first, then non_ha (joke/answer) separately
    if (!mediaPlayerEntityId.empty()) {
        std::string nonHaText = result.entities.get("non_ha", "").asString();

        // Build HA-only text by stripping the non_ha suffix from combined response
        std::string haText = result.response_text;
        if (!nonHaText.empty()) {
            size_t pos = haText.rfind(nonHaText);
            if (pos != std::string::npos) {
                haText = haText.substr(0, pos);
                while (!haText.empty() && haText.back() == ' ') haText.pop_back();
            }
        }

        auto ttsSpeak = [&](const std::string& text) {
            if (text.empty()) return;
            Json::Value p;
            p["message"]                = text;
            p["media_player_entity_id"] = mediaPlayerEntityId;
            try { ha_->callService("tts", "speak", ttsEntity_, p); }
            catch (const std::exception& e) {
                std::cerr << "[CommandController] TTS speak failed: " << e.what() << std::endl;
            }
        };

        ttsSpeak(haText);     // TTS 1: HA confirmation ("Turned on sala 1.")
        ttsSpeak(nonHaText);  // TTS 2: joke/answer (if present)
    }

    // Log the result
    if (commandId >= 0) {
        try {
            db_->logIntentResult(commandId, result);
        } catch (const std::exception& e) {
            std::cerr << "[CommandController] DB result log failed: " << e.what() << std::endl;
        }
    }

    Json::Value response = buildResponse(result);
    auto httpResp = drogon::HttpResponse::newHttpJsonResponse(response);
    httpResp->setStatusCode(result.success ? drogon::k200OK : drogon::k422UnprocessableEntity);
    cb(httpResp);
}

void CommandController::handleReindex(const drogon::HttpRequestPtr& req,
                                       std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    std::cout << "[CommandController] Manual re-index triggered — shelling out to hms-assist-sync" << std::endl;

    const char* configEnv = std::getenv("HMS_ASSIST_CONFIG");
    std::string configPath = configEnv ? configEnv : "/etc/hms-assist/config.yaml";

    std::string syncScript =
        "/home/aamat/maestro_hub/projects/hms-assist/tools/venv/bin/python "
        "/home/aamat/maestro_hub/projects/hms-assist/tools/hms_assist_sync.py "
        "--config " + configPath + " --once > /tmp/hms_assist_sync.log 2>&1 &";

    int ret = std::system(syncScript.c_str());

    Json::Value resp;
    resp["success"] = (ret == 0);
    resp["message"] = "Sync started in background — tail /tmp/hms_assist_sync.log";
    cb(drogon::HttpResponse::newHttpJsonResponse(resp));
}

IntentResult CommandController::runSinglePipeline(const VoiceCommand& command) {
    IntentResult result = tier1_->classify(command);
    if (result.success) return result;
    return tier2_->classify(command);
}

// Wave-based parallel execution of sub-commands from a SplitResult.
//   wait_for_previous:false → launch in parallel with current wave
//   wait_for_previous:true  → flush current wave first (500ms gap), then start new wave
IntentResult CommandController::executeSplit(const SplitResult& split,
                                              const VoiceCommand& origin,
                                              const std::string& initialResponse,
                                              const std::string& tierLabel) {
    auto start = std::chrono::steady_clock::now();

    using FuturePair = std::pair<size_t, std::future<IntentResult>>;
    std::vector<FuturePair> wave;
    Json::Value executedCmds(Json::arrayValue);
    executedCmds.resize(split.sub_commands.size());
    bool allOk = true;
    std::string combinedResponse = initialResponse;

    auto flushWave = [&]() {
        for (auto& [idx, fut] : wave) {
            IntentResult subResult = fut.get();
            allOk = allOk && subResult.success;

            Json::Value entry;
            entry["text"]              = split.sub_commands[idx].text;
            entry["wait_for_previous"] = split.sub_commands[idx].wait_for_previous;
            entry["tier"]              = subResult.tier;
            entry["intent"]            = subResult.intent;
            entry["success"]           = subResult.success;
            entry["entities"]          = subResult.entities;
            executedCmds[static_cast<int>(idx)] = entry;

            if (!subResult.response_text.empty() && subResult.success) {
                if (!combinedResponse.empty()) combinedResponse += " ";
                combinedResponse += subResult.response_text;
            }
        }
        wave.clear();
    };

    for (size_t i = 0; i < split.sub_commands.size(); ++i) {
        const auto& sc = split.sub_commands[i];
        if (sc.wait_for_previous && !wave.empty()) {
            flushWave();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        VoiceCommand sub;
        sub.text       = sc.text;
        sub.device_id  = origin.device_id;
        sub.confidence = 1.0f;
        wave.push_back({i, std::async(std::launch::async,
            [this, sub]() { return runSinglePipeline(sub); })});
    }
    flushWave();

    if (!split.non_ha.empty()) {
        if (!combinedResponse.empty()) combinedResponse += " ";
        combinedResponse += split.non_ha;
    }

    IntentResult r;
    r.tier               = tierLabel;
    r.intent             = split.sub_commands.size() == 1 ? "single_command" : "multi_command";
    r.success            = allOk;
    r.confidence         = 1.0f;
    r.response_text      = combinedResponse;
    r.processing_time_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count());
    r.entities["commands"] = executedCmds;
    r.entities["non_ha"]   = split.non_ha;
    return r;
}

// Split a command text on hard dividers (and/then/followed by/after that).
// Fuzzy fallback: if 2+ action-verb phrases found with no divider, split before the second.
// Returns a single-element vector if no compound structure detected.
std::vector<std::string> CommandController::regexSplitCompound(const std::string& text) {
    static const std::regex dividerRe(
        R"(\s+(?:and|then|followed\s+by|after\s+that)\s+)",
        std::regex::icase);

    std::vector<std::string> parts;
    std::sregex_token_iterator it(text.begin(), text.end(), dividerRe, -1);
    for (; it != std::sregex_token_iterator{}; ++it) {
        std::string part = it->str();
        auto first = part.find_first_not_of(" \t");
        auto last  = part.find_last_not_of(" \t");
        if (first != std::string::npos)
            parts.push_back(part.substr(first, last - first + 1));
    }

    if (parts.size() > 1) return parts;

    // Fuzzy fallback: detect 2+ action-verb phrases → split before the second occurrence
    static const std::regex actionRe(
        R"(\b(turn\s+on|turn\s+off|switch\s+on|switch\s+off|lock\b|unlock\b|pause\b|play\b|set\s+\w|restart\b|reboot\b|toggle\b))",
        std::regex::icase);

    std::vector<std::smatch> verbMatches;
    for (auto vi = std::sregex_iterator(text.begin(), text.end(), actionRe);
         vi != std::sregex_iterator{}; ++vi) {
        verbMatches.push_back(*vi);
    }

    if (verbMatches.size() >= 2) {
        size_t splitPos = static_cast<size_t>(verbMatches[1].position());
        size_t spacePos = text.rfind(' ', splitPos > 0 ? splitPos - 1 : 0);
        if (spacePos != std::string::npos) {
            std::string p1 = text.substr(0, spacePos);
            std::string p2 = text.substr(spacePos + 1);
            if (p1.size() >= 3 && p2.size() >= 3)
                return {p1, p2};
        }
    }

    return {text};
}

IntentResult CommandController::runPipeline(const VoiceCommand& command) {
    // --- Tier 1: Deterministic (regex) ---
    IntentResult result = tier1_->classify(command);
    if (result.success) {
        std::cout << "[Pipeline] Tier1 hit: " << result.intent
                  << " (" << result.processing_time_ms << "ms)" << std::endl;
        return result;
    }

    auto t0 = std::chrono::steady_clock::now();

    // --- Detect compound command ---
    std::vector<std::string> parts = regexSplitCompound(command.text);
    bool isCompound = parts.size() > 1;

    if (isCompound) {
        std::cout << "[Pipeline] Compound detected: " << parts.size() << " part(s)" << std::endl;

        // Phase 1 (parallel): Tier2 on each regex-split part + Tier3 split on full command
        std::vector<std::future<IntentResult>> tier2Futures;
        for (const auto& part : parts) {
            VoiceCommand sub;
            sub.text      = part;
            sub.device_id = command.device_id;
            sub.confidence = 1.0f;
            tier2Futures.push_back(std::async(std::launch::async,
                [this, sub]() { return tier2_->classify(sub); }));
        }
        auto splitFuture = std::async(std::launch::async,
            [this, command]() { return tier3_->split(command); });

        // Collect Tier2 results → build executed entity set
        std::set<std::string> executedEntityIds;
        std::string tier2ResponseText;
        Json::Value tier2Commands(Json::arrayValue);

        for (size_t i = 0; i < tier2Futures.size(); ++i) {
            IntentResult r = tier2Futures[i].get();
            if (r.success) {
                std::string eid = r.entities.get("entity_id", "").asString();
                if (!eid.empty()) executedEntityIds.insert(eid);
                if (!r.response_text.empty()) {
                    if (!tier2ResponseText.empty()) tier2ResponseText += " ";
                    tier2ResponseText += r.response_text;
                }
                Json::Value entry;
                entry["text"]     = parts[i];
                entry["tier"]     = "tier2";
                entry["intent"]   = r.intent;
                entry["success"]  = true;
                entry["entities"] = r.entities;
                tier2Commands.append(entry);
            }
        }

        // Collect Tier3 split result
        SplitResult split = splitFuture.get();

        if (split.sub_commands.empty() && split.non_ha.empty()) {
            // LLM returned nothing — Tier2 handled everything (or nothing)
            IntentResult r;
            r.tier          = "tier2";
            r.intent        = parts.size() == 1 ? "single_command" : "multi_command";
            r.success       = !executedEntityIds.empty();
            r.confidence    = 1.0f;
            r.response_text = tier2ResponseText;
            r.entities["commands"] = tier2Commands;
            r.processing_time_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0).count());
            std::cout << "[Pipeline] Compound Tier2-only: " << executedEntityIds.size()
                      << " entities (" << r.processing_time_ms << "ms)" << std::endl;
            return r;
        }

        // Phase 2 (parallel): dry-run resolve each Tier3 sub-command to get entity_id for dedup
        std::vector<std::future<IntentResult>> dryFutures;
        for (const auto& sc : split.sub_commands) {
            VoiceCommand dryCmd;
            dryCmd.text      = sc.text;
            dryCmd.device_id = command.device_id;
            dryCmd.dry_run   = true;
            dryFutures.push_back(std::async(std::launch::async,
                [this, dryCmd]() { return tier2_->classify(dryCmd); }));
        }

        SplitResult filteredSplit;
        filteredSplit.non_ha     = split.non_ha;
        filteredSplit.confidence = split.confidence;
        int skipped = 0;
        for (size_t i = 0; i < split.sub_commands.size(); ++i) {
            IntentResult dryResult = dryFutures[i].get();
            std::string eid = dryResult.entities.get("entity_id", "").asString();
            if (!eid.empty() && executedEntityIds.count(eid)) {
                ++skipped;
                std::cout << "[Pipeline] Compound: skip '" << split.sub_commands[i].text
                          << "' (entity " << eid << " already executed)" << std::endl;
            } else {
                filteredSplit.sub_commands.push_back(split.sub_commands[i]);
            }
        }

        if (filteredSplit.sub_commands.empty() && filteredSplit.non_ha.empty()) {
            // All Tier3 sub-commands deduplicated — Tier2 handled everything
            IntentResult r;
            r.tier          = "tier2";
            r.intent        = "multi_command";
            r.success       = !executedEntityIds.empty();
            r.confidence    = 1.0f;
            r.response_text = tier2ResponseText;
            r.entities["commands"] = tier2Commands;
            r.processing_time_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0).count());
            std::cout << "[Pipeline] Compound: all " << skipped
                      << " sub-cmd(s) deduplicated (" << r.processing_time_ms << "ms)" << std::endl;
            return r;
        }

        // Phase 3: execute non-deduplicated sub-commands
        std::string tierLabel = executedEntityIds.empty() ? "tier3" : "tier2+tier3";
        IntentResult r = executeSplit(filteredSplit, command, tier2ResponseText, tierLabel);

        // Prepend Tier2 commands to the front of the commands array
        if (tier2Commands.size() > 0) {
            Json::Value allCmds(Json::arrayValue);
            for (const auto& cmd : tier2Commands) allCmds.append(cmd);
            for (const auto& cmd : r.entities["commands"]) allCmds.append(cmd);
            r.entities["commands"] = allCmds;
        }

        std::cout << "[Pipeline] Compound " << tierLabel << ": "
                  << executedEntityIds.size() << " Tier2 + "
                  << filteredSplit.sub_commands.size() << " remaining ("
                  << r.processing_time_ms << "ms)" << std::endl;
        return r;
    }

    // --- Non-compound path ---

    // Tier 2: Vector search
    result = tier2_->classify(command);
    if (result.success) {
        std::cout << "[Pipeline] Tier2 hit: " << result.intent
                  << " similarity=" << result.confidence
                  << " (" << result.processing_time_ms << "ms)" << std::endl;
        return result;
    }

    // --- Tier 3: LLM splits the whole command ---
    std::cout << "[Pipeline] Tier3: parsing via LLM" << std::endl;

    SplitResult split = tier3_->split(command);

    // Non-HA only (joke, question, etc.)
    if (split.sub_commands.empty()) {
        IntentResult r;
        r.tier         = "tier3";
        r.intent       = "non_ha";
        r.success      = !split.non_ha.empty();
        r.response_text = split.non_ha.empty() ? "I couldn't understand that command." : split.non_ha;
        r.entities["non_ha"] = split.non_ha;
        r.processing_time_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count());
        std::cout << "[Pipeline] Tier3: non_ha only (" << r.processing_time_ms << "ms)" << std::endl;
        return r;
    }

    IntentResult r = executeSplit(split, command, "", "tier3");
    r.processing_time_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count());

    std::cout << "[Pipeline] Tier3: " << split.sub_commands.size()
              << " sub-command(s) (" << r.processing_time_ms << "ms)" << std::endl;
    return r;
}

drogon::HttpResponsePtr CommandController::errorResponse(int status, const std::string& message) {
    Json::Value body;
    body["success"] = false;
    body["error"]   = message;
    auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
    resp->setStatusCode(static_cast<drogon::HttpStatusCode>(status));
    return resp;
}

Json::Value CommandController::buildResponse(const IntentResult& result) {
    Json::Value r;
    r["success"]            = result.success;
    r["tier"]               = result.tier;
    r["intent"]             = result.intent;
    r["confidence"]         = result.confidence;
    r["response_text"]      = result.response_text;
    r["processing_time_ms"] = result.processing_time_ms;
    r["entities"]           = result.entities;
    return r;
}
