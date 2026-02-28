#include "intent/EmbeddingClassifier.h"
#include <iostream>
#include <algorithm>
#include <chrono>
#include <regex>
#include <thread>

// Returns true when `word` appears as a whole word in `text`.
static bool hasWord(const std::string& text, const std::string& word) {
    return std::regex_search(text, std::regex("\\b" + word + "\\b"));
}

EmbeddingClassifier::EmbeddingClassifier(std::shared_ptr<OllamaClient> ollama,
                                          std::shared_ptr<HomeAssistantClient> haClient,
                                          std::shared_ptr<VectorSearchService> vectorSearch,
                                          const std::string& embedModel,
                                          float similarityThreshold,
                                          int searchLimit)
    : ollama_(ollama), ha_(haClient), vectorSearch_(vectorSearch),
      embedModel_(embedModel), threshold_(similarityThreshold), limit_(searchLimit) {}

IntentResult EmbeddingClassifier::classify(const VoiceCommand& command) {
    auto start = std::chrono::steady_clock::now();
    IntentResult result;
    result.tier = "tier2";

    try {
        // Embed the command text with asymmetric retrieval prefix so that
        // nomic-embed-text aligns query space with the document space used
        // during indexing (search_document: prefix in hms_assist_sync.py).
        std::vector<float> queryVec = ollama_->embed("search_query: " + command.text, embedModel_);

        // Search vector DB
        std::vector<EntityMatch> matches = vectorSearch_->search(queryVec, threshold_, limit_);

        if (matches.empty()) {
            result.success = false;
            return result;
        }

        const EntityMatch& best = matches[0];

        // "restart" = turn_off, wait 1s, turn_on (sequential, not in inferAction)
        std::string tLower = command.text;
        std::transform(tLower.begin(), tLower.end(), tLower.begin(), ::tolower);
        if (hasWord(tLower, "restart") || hasWord(tLower, "reboot") || hasWord(tLower, "cycle")) {
            ha_->callService(best.domain, "turn_off", best.entity_id, {});
            std::this_thread::sleep_for(std::chrono::seconds(1));
            bool haOk = ha_->callService(best.domain, "turn_on", best.entity_id, {});
            std::this_thread::sleep_for(std::chrono::milliseconds(1200));
            Entity updated = ha_->getEntityState(best.entity_id);
            result.success       = haOk;
            result.intent        = best.domain + "_restart";
            result.confidence    = best.similarity;
            result.response_text = "Restarted the " + best.friendly_name + ".";
            result.entities["entity_id"]     = best.entity_id;
            result.entities["domain"]        = best.domain;
            result.entities["friendly_name"] = best.friendly_name;
            result.entities["action"]        = "restart";
            result.entities["state"]         = updated.state;
            result.entities["similarity"]    = best.similarity;
            auto end = std::chrono::steady_clock::now();
            result.processing_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
            return result;
        }

        std::string action = inferAction(command.text, best.domain);

        if (action.empty()) {
            result.success = false;
            return result;
        }

        // Execute against HA
        Json::Value params;
        if (best.domain == "climate" && action == "set_temperature") {
            // Try to extract temperature from command text
            std::regex tempRe(R"(\b(\d{2})\b)");
            std::smatch m;
            if (std::regex_search(command.text, m, tempRe)) {
                params["temperature"] = std::stof(m[1].str());
            }
        }

        bool haOk = ha_->callService(best.domain, action, best.entity_id, params);

        // Brief wait for HA to process the state change before reading it back
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));

        // Get updated state
        Entity updated = ha_->getEntityState(best.entity_id);

        result.success       = haOk;
        result.intent        = best.domain + "_" + action;
        result.confidence    = best.similarity;
        result.response_text = buildResponse(action, best.friendly_name);

        result.entities["entity_id"]    = best.entity_id;
        result.entities["domain"]       = best.domain;
        result.entities["friendly_name"] = best.friendly_name;
        result.entities["action"]       = action;
        result.entities["state"]        = updated.state;
        result.entities["similarity"]   = best.similarity;

    } catch (const std::exception& e) {
        std::cerr << "[EmbeddingClassifier] Error: " << e.what() << std::endl;
        result.success = false;
    }

    auto end = std::chrono::steady_clock::now();
    result.processing_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    return result;
}

std::string EmbeddingClassifier::inferAction(const std::string& text, const std::string& domain) {
    std::string t = text;
    std::transform(t.begin(), t.end(), t.begin(), ::tolower);

    if (domain == "light" || domain == "switch" || domain == "fan") {
        if (hasWord(t, "off") || hasWord(t, "disable")) return "turn_off";
        if (hasWord(t, "toggle"))                        return "toggle";
        return "turn_on"; // default for lights/switches
    }
    if (domain == "lock") {
        if (hasWord(t, "unlock") || hasWord(t, "open")) return "unlock";
        return "lock";
    }
    if (domain == "cover") {
        if (hasWord(t, "close") || hasWord(t, "down")) return "close_cover";
        if (hasWord(t, "stop"))                         return "stop_cover";
        return "open_cover";
    }
    if (domain == "media_player") {
        if (hasWord(t, "pause") || hasWord(t, "stop")) return "media_pause";
        if (hasWord(t, "next")  || hasWord(t, "skip")) return "media_next_track";
        if (hasWord(t, "prev")  || hasWord(t, "back")) return "media_previous_track";
        if (hasWord(t, "mute"))                         return "volume_mute";
        return "media_play_pause";
    }
    if (domain == "climate") {
        if (hasWord(t, "off"))                                    return "turn_off";
        if (hasWord(t, "cool") || hasWord(t, "cold"))             return "set_temperature";
        if (hasWord(t, "heat") || hasWord(t, "warm"))             return "set_temperature";
        return "set_temperature";
    }
    if (domain == "scene")        return "turn_on";
    if (domain == "script")       return "turn_on";
    if (domain == "input_boolean") {
        if (hasWord(t, "off")) return "turn_off";
        return "turn_on";
    }

    return "";
}

std::string EmbeddingClassifier::buildResponse(const std::string& action,
                                                const std::string& friendlyName) {
    if (action == "turn_on")            return "Turned on the " + friendlyName + ".";
    if (action == "turn_off")           return "Turned off the " + friendlyName + ".";
    if (action == "toggle")             return "Toggled the " + friendlyName + ".";
    if (action == "lock")               return "Locked the " + friendlyName + ".";
    if (action == "unlock")             return "Unlocked the " + friendlyName + ".";
    if (action == "open_cover")         return "Opened the " + friendlyName + ".";
    if (action == "close_cover")        return "Closed the " + friendlyName + ".";
    if (action == "media_pause")        return "Paused " + friendlyName + ".";
    if (action == "media_play_pause")   return "Toggled playback on " + friendlyName + ".";
    if (action == "media_next_track")   return "Skipping to next track on " + friendlyName + ".";
    if (action == "media_previous_track") return "Going back a track on " + friendlyName + ".";
    if (action == "set_temperature")    return "Adjusting temperature on " + friendlyName + ".";
    if (action == "restart")            return "Restarted the " + friendlyName + ".";
    return "Done.";
}
