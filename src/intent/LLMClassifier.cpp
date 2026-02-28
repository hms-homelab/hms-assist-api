#include "intent/LLMClassifier.h"
#include <iostream>
#include <future>
#include <chrono>
#include <sstream>

LLMClassifier::LLMClassifier(std::shared_ptr<OllamaClient> ollama,
                               std::shared_ptr<HomeAssistantClient> haClient,
                               std::shared_ptr<VectorSearchService> vectorSearch,
                               const std::string& embedModel,
                               const std::string& fastModel,
                               const std::string& smartModel,
                               float escalationThreshold)
    : ollama_(ollama), ha_(haClient), vectorSearch_(vectorSearch),
      embedModel_(embedModel), fastModel_(fastModel), smartModel_(smartModel),
      escalationThreshold_(escalationThreshold) {}

void LLMClassifier::refreshEntityCache(const std::vector<Entity>& entities) {
    Json::Value arr(Json::arrayValue);
    for (const auto& e : entities) {
        if (e.domain == "sun" || e.domain == "person" ||
            e.domain == "zone" || e.domain == "update") continue;
        Json::Value item;
        item["entity_id"]    = e.entity_id;
        item["name"]         = e.friendly_name;
        item["domain"]       = e.domain;
        item["state"]        = e.state;
        arr.append(item);
    }
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    entityCacheJson_ = Json::writeString(wb, arr);
    std::cout << "[LLMClassifier] Entity cache refreshed (" << arr.size() << " entities)" << std::endl;
}

IntentResult LLMClassifier::classify(const VoiceCommand& command) {
    auto start = std::chrono::steady_clock::now();

    // If entity cache is empty, populate it now
    if (entityCacheJson_.empty()) {
        try {
            auto entities = ha_->getAllEntities();
            refreshEntityCache(entities);
        } catch (const std::exception& e) {
            std::cerr << "[LLMClassifier] Could not populate entity cache: " << e.what() << std::endl;
        }
    }

    // Pre-filter entities: embed the command, grab the top 25 by cosine similarity.
    // This shrinks the LLM context from ~1100 entities to ~25, letting the 8b
    // model stay confident and avoid unnecessary escalation.
    std::string filteredEntities = preFilterEntities(command);

    // --- Tier 3a: fast model ---
    std::string modelUsed = fastModel_;
    Json::Value llmJson;
    try {
        llmJson = ollama_->chatJson(buildUserPrompt(command), fastModel_, buildSystemPrompt(filteredEntities));
    } catch (const std::exception& e) {
        std::cerr << "[LLMClassifier] Fast model failed: " << e.what() << std::endl;
        IntentResult r;
        r.success = false;
        r.tier = "tier3a";
        r.response_text = "I had trouble understanding that. Please try again.";
        return r;
    }

    LLMPlan plan = parsePlan(llmJson);

    // --- Tier 3b: escalate to smart model if needed ---
    if (plan.escalate || plan.confidence < escalationThreshold_) {
        std::cout << "[LLMClassifier] Escalating to smart model (escalate="
                  << plan.escalate << ", confidence=" << plan.confidence << ")" << std::endl;
        modelUsed = smartModel_;
        try {
            llmJson = ollama_->chatJson(buildUserPrompt(command), smartModel_, buildSystemPrompt(filteredEntities));
            plan = parsePlan(llmJson);
        } catch (const std::exception& e) {
            std::cerr << "[LLMClassifier] Smart model failed: " << e.what() << std::endl;
        }
    }

    IntentResult result = executePlan(plan, command, modelUsed);

    auto end = std::chrono::steady_clock::now();
    result.processing_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    return result;
}

LLMPlan LLMClassifier::parsePlan(const Json::Value& j) {
    LLMPlan plan;
    plan.escalate      = j.get("escalate", false).asBool();
    plan.confidence    = j.get("confidence", 0.5f).asFloat();
    plan.response_text = j.get("response_text", "").asString();

    const Json::Value& cmds = j["commands"];
    if (cmds.isArray()) {
        for (const auto& c : cmds) {
            LLMCommand cmd;
            cmd.intent    = c.get("intent", "").asString();
            cmd.entity_id = c.get("entity_id", "").asString();
            cmd.domain    = c.get("domain", "").asString();
            cmd.action    = c.get("action", "").asString();
            cmd.params    = c.get("params", Json::Value());
            if (!cmd.entity_id.empty() && !cmd.action.empty()) {
                plan.commands.push_back(cmd);
            }
        }
    }
    return plan;
}

IntentResult LLMClassifier::executePlan(const LLMPlan& plan,
                                         const VoiceCommand& command,
                                         const std::string& modelUsed) {
    IntentResult result;
    result.tier          = (modelUsed == fastModel_) ? "tier3a" : "tier3b";
    result.confidence    = plan.confidence;
    result.response_text = plan.response_text;

    if (plan.commands.empty()) {
        result.success       = false;
        result.response_text = plan.response_text.empty()
            ? "I understood you but couldn't find a matching device."
            : plan.response_text;
        return result;
    }

    // Execute all commands in parallel
    std::vector<std::future<bool>> futures;
    for (const auto& cmd : plan.commands) {
        futures.push_back(std::async(std::launch::async, [this, &cmd]() {
            try {
                return ha_->callService(cmd.domain, cmd.action, cmd.entity_id, cmd.params);
            } catch (const std::exception& e) {
                std::cerr << "[LLMClassifier] HA call failed for "
                          << cmd.entity_id << ": " << e.what() << std::endl;
                return false;
            }
        }));
    }

    // Collect results and build entities array
    Json::Value executedCmds(Json::arrayValue);
    bool allOk = true;
    for (size_t i = 0; i < plan.commands.size(); ++i) {
        bool ok = futures[i].get();
        allOk = allOk && ok;

        Json::Value cmdResult;
        cmdResult["intent"]    = plan.commands[i].intent;
        cmdResult["entity_id"] = plan.commands[i].entity_id;
        cmdResult["action"]    = plan.commands[i].action;
        cmdResult["success"]   = ok;

        // Get updated state
        try {
            Entity updated = ha_->getEntityState(plan.commands[i].entity_id);
            cmdResult["state"] = updated.state;
        } catch (...) {}

        executedCmds.append(cmdResult);
    }

    result.success               = allOk;
    result.intent                = plan.commands.size() == 1
                                    ? plan.commands[0].intent
                                    : "multi_command";
    result.entities["commands"]  = executedCmds;
    result.entities["model_used"] = modelUsed;

    if (result.response_text.empty()) {
        result.response_text = allOk ? "Done." : "Some commands could not be executed.";
    }

    return result;
}

std::string LLMClassifier::buildSystemPrompt(const std::string& entityJson) const {
    return R"(You are a smart home voice assistant. Parse voice commands into Home Assistant service calls.

Output ONLY a single JSON object. No prose, no explanation, no markdown.

Example — "turn off the coffee maker and turn on sala 1":
{"commands":[{"intent":"turn_off_coffee","entity_id":"switch.coffee","domain":"switch","action":"turn_off","params":{}},{"intent":"turn_on_sala_1","entity_id":"light.sala_1","domain":"light","action":"turn_on","params":{}}],"escalate":false,"confidence":0.95,"response_text":"Turned off the coffee maker and turned on Sala 1."}

Rules:
- commands: array of HA service calls. One entry per device/entity.
- entity_id: must be an exact match from the Available entities list below.
- action: one of turn_on | turn_off | toggle | lock | unlock | set_temperature | media_play_pause | media_next_track | media_previous_track | open_cover | close_cover | stop_cover
- params: {} unless action is set_temperature, then {"temperature": <number>}
- confidence: 0.9-1.0 when entities are clearly matched; 0.5-0.8 when uncertain
- escalate: true only if no entity matches or the command needs web/external data
- Non-HA requests (jokes, questions): return commands:[] with escalate:false and answer in response_text
- response_text: short natural language confirmation of what was done)" +
           std::string("\n\nAvailable entities:\n") + entityJson;
}

std::string LLMClassifier::preFilterEntities(const VoiceCommand& command) const {
    // Embed the command and retrieve top-25 candidates from pgvector.
    // This keeps the LLM context small enough for the 8b model to stay confident.
    try {
        std::vector<float> queryVec = ollama_->embed("search_query: " + command.text, embedModel_);
        // threshold=0.0 → return whatever the top-25 are regardless of score
        std::vector<EntityMatch> matches = vectorSearch_->search(queryVec, 0.0f, 25);

        Json::Value arr(Json::arrayValue);
        for (const auto& m : matches) {
            Json::Value item;
            item["entity_id"] = m.entity_id;
            item["name"]      = m.friendly_name;
            item["domain"]    = m.domain;
            item["state"]     = m.state;
            arr.append(item);
        }
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        return Json::writeString(wb, arr);
    } catch (const std::exception& e) {
        std::cerr << "[LLMClassifier] preFilterEntities failed: " << e.what()
                  << " — falling back to full entity cache" << std::endl;
        return entityCacheJson_;
    }
}

SplitResult LLMClassifier::split(const VoiceCommand& command) {
    static const std::string kSplitPrompt = R"(Split a compound voice command into individual smart home sub-commands.

Output ONLY a JSON object. No prose.

Each sub-command has:
- "text": the command string
- "sequential": true if this command must wait for the previous one to finish first

sequential:true when:
- same device (restart, power-cycle: off THEN on)
- explicit ordering ("first ... then ...", "after that ...")
- logical dependency (unlock door, then open it)

sequential:false when commands are on different/independent devices.

Example 1 — independent devices:
"turn off coffee and turn on sala 1 and tell me a joke about cows"
{"sub_commands":[{"text":"turn off coffee","sequential":false},{"text":"turn on sala 1","sequential":false}],"non_ha":"Why do cows wear bells? Because their horns don't work!"}

Example 2 — restart (same device, ordered):
"restart the router and turn on the TV"
{"sub_commands":[{"text":"turn off router","sequential":false},{"text":"turn on router","sequential":true},{"text":"turn on TV","sequential":false}],"non_ha":""}

Rules:
- sub_commands: only HA device/automation commands
- non_ha: answer non-HA parts (jokes, questions). Empty string if none.
- Keep each sub_command text close to the original wording.)";

    Json::Value llmJson;
    try {
        llmJson = ollama_->chatJson(
            "Voice command: \"" + command.text + "\"",
            fastModel_,
            kSplitPrompt
        );
    } catch (const std::exception& e) {
        std::cerr << "[LLMClassifier] split() failed: " << e.what() << std::endl;
        return {};
    }

    SplitResult result;
    result.non_ha = llmJson.get("non_ha", "").asString();
    const Json::Value& arr = llmJson["sub_commands"];
    if (arr.isArray()) {
        for (const auto& v : arr) {
            if (v.isString()) {
                // Backward-compat: plain string (old format)
                result.sub_commands.push_back({v.asString(), false});
            } else if (v.isObject()) {
                SubCommand sc;
                sc.text       = v.get("text", "").asString();
                sc.sequential = v.get("sequential", false).asBool();
                if (!sc.text.empty()) result.sub_commands.push_back(sc);
            }
        }
    }
    return result;
}

std::string LLMClassifier::buildUserPrompt(const VoiceCommand& command) const {
    std::ostringstream oss;
    oss << "Voice command: \"" << command.text << "\"\n";
    if (!command.context.empty()) {
        oss << "Context: " << command.context << "\n";
    }
    oss << "Device: " << command.device_id;
    return oss.str();
}
