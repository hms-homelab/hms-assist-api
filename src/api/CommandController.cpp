#include "api/CommandController.h"
#include <iostream>
#include <chrono>
#include <future>
#include <vector>

CommandController::CommandController(std::shared_ptr<IntentClassifier> tier1,
                                     std::shared_ptr<IntentClassifier> tier2,
                                     std::shared_ptr<IntentClassifier> tier3,
                                     std::shared_ptr<DatabaseService>  db)
    : tier1_(tier1), tier2_(tier2), tier3_(tier3), db_(db) {}

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
    command.context   = (*bodyJson).get("context", Json::Value()).toStyledString();

    if (command.text.empty()) {
        cb(errorResponse(400, "text cannot be empty"));
        return;
    }

    // Log the incoming command
    int commandId = -1;
    try {
        commandId = db_->logVoiceCommand(command);
    } catch (const std::exception& e) {
        std::cerr << "[CommandController] DB log failed: " << e.what() << std::endl;
    }

    // Run the classification pipeline
    IntentResult result = runPipeline(command);

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

    // Run the Python sync tool as a background process (non-blocking).
    // Uses the same config file the API is running with.
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

IntentResult CommandController::runPipeline(const VoiceCommand& command) {
    // --- Tier 1: Deterministic (regex) ---
    IntentResult result = tier1_->classify(command);
    if (result.success) {
        std::cout << "[Pipeline] Tier1 hit: " << result.intent
                  << " (" << result.processing_time_ms << "ms)" << std::endl;
        return result;
    }

    // Compound commands ("X and Y"): LLM splits into sub-strings, each fragment
    // is routed through tier1→tier2. LLM needs no entity context — just text splitting.
    bool isCompound = (command.text.find(" and ") != std::string::npos);

    if (isCompound) {
        std::cout << "[Pipeline] Compound command — splitting via LLM" << std::endl;
        auto start = std::chrono::steady_clock::now();

        SplitResult split = tier3_->split(command);  // virtual: LLMClassifier overrides

        if (!split.sub_commands.empty()) {
            // Wave-based execution:
            //   sequential:false → launch in parallel with current wave
            //   sequential:true  → flush (wait for) current wave first, then start new wave
            // This handles both independent devices (parallel) and ordered operations
            // on the same device (e.g. restart: turn_off then turn_on).
            using FuturePair = std::pair<size_t, std::future<IntentResult>>;
            std::vector<FuturePair> wave;
            Json::Value executedCmds(Json::arrayValue);
            executedCmds.resize(split.sub_commands.size());
            bool allOk = true;
            std::string combinedResponse;

            auto flushWave = [&]() {
                for (auto& [idx, fut] : wave) {
                    IntentResult subResult = fut.get();
                    allOk = allOk && subResult.success;

                    Json::Value entry;
                    entry["text"]       = split.sub_commands[idx].text;
                    entry["sequential"] = split.sub_commands[idx].sequential;
                    entry["tier"]       = subResult.tier;
                    entry["intent"]     = subResult.intent;
                    entry["success"]    = subResult.success;
                    entry["entities"]   = subResult.entities;
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
                if (sc.sequential && !wave.empty()) {
                    flushWave();  // wait for current wave before starting this command
                }
                VoiceCommand sub;
                sub.text       = sc.text;
                sub.device_id  = command.device_id;
                sub.confidence = 1.0f;
                wave.push_back({i, std::async(std::launch::async,
                    [this, sub]() { return runSinglePipeline(sub); })});
            }
            flushWave();  // drain final wave

            if (!split.non_ha.empty()) {
                if (!combinedResponse.empty()) combinedResponse += " ";
                combinedResponse += split.non_ha;
            }

            auto end = std::chrono::steady_clock::now();
            IntentResult r;
            r.tier                = "tier3a";
            r.intent              = "multi_command";
            r.success             = allOk;
            r.confidence          = 1.0f;
            r.response_text       = combinedResponse;
            r.processing_time_ms  = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
            r.entities["commands"] = executedCmds;
            r.entities["non_ha"]   = split.non_ha;

            std::cout << "[Pipeline] Split: " << split.sub_commands.size()
                      << " sub-commands (" << r.processing_time_ms << "ms)" << std::endl;
            return r;
        }
        // Split returned empty (LLM failed) — fall through to tier2
    }

    // --- Tier 2: Vector search (semantic) ---
    result = tier2_->classify(command);
    if (result.success) {
        std::cout << "[Pipeline] Tier2 hit: " << result.intent
                  << " similarity=" << result.confidence
                  << " (" << result.processing_time_ms << "ms)" << std::endl;
        return result;
    }

    // --- Tier 3: LLM with pre-filtered entity context (single ambiguous command) ---
    result = tier3_->classify(command);
    std::cout << "[Pipeline] Tier3 " << result.tier << ": "
              << (result.success ? "ok" : "fail")
              << " (" << result.processing_time_ms << "ms)" << std::endl;
    return result;
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
