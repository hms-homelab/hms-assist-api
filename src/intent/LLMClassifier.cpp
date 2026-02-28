#include "intent/LLMClassifier.h"
#include <iostream>
#include <future>
#include <chrono>
#include <sstream>

LLMClassifier::LLMClassifier(std::shared_ptr<OllamaClient> ollama,
                               std::shared_ptr<HomeAssistantClient> haClient,
                               const std::string& fastModel,
                               const std::string& smartModel,
                               float escalationThreshold)
    : ollama_(ollama), ha_(haClient),
      fastModel_(fastModel), smartModel_(smartModel),
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

    // --- Tier 3a: fast model ---
    std::string modelUsed = fastModel_;
    Json::Value llmJson;
    try {
        llmJson = ollama_->chatJson(buildUserPrompt(command), fastModel_, buildSystemPrompt());
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
            llmJson = ollama_->chatJson(buildUserPrompt(command), smartModel_, buildSystemPrompt());
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

std::string LLMClassifier::buildSystemPrompt() const {
    return R"(You are a voice assistant for a smart home. Your job is to parse voice commands into structured Home Assistant service calls.

You must respond with a single valid JSON object — nothing else, no prose, no markdown.

Response format:
{
  "commands": [
    {
      "intent": "<descriptive intent name>",
      "entity_id": "<exact entity_id from the list>",
      "domain": "<entity domain>",
      "action": "<HA service action: turn_on|turn_off|toggle|lock|unlock|set_temperature|media_play_pause|media_next_track|media_previous_track|open_cover|close_cover|turn_on>",
      "params": {}
    }
  ],
  "escalate": false,
  "confidence": 0.95,
  "response_text": "Human-friendly confirmation of what was done or will be done."
}

Rules:
- Split compound commands into multiple entries in "commands".
- Only use entity_ids from the provided entity list.
- Set "escalate": true if the command is ambiguous, requires web information, or needs complex reasoning.
- Set "confidence" between 0.0 and 1.0.
- For set_temperature, include {"temperature": <number>} in params.
- If no matching entity is found, return commands: [] and escalate: true.
- response_text should be natural and conversational, confirming what you did.)" +
           std::string("\n\nAvailable entities:\n") + entityCacheJson_;
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
