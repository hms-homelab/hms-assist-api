#include "intent/DeterministicClassifier.h"
#include <iostream>
#include <chrono>
#include <algorithm>

DeterministicClassifier::DeterministicClassifier(std::shared_ptr<HomeAssistantClient> haClient)
    : haClient_(haClient) {
    initializePatterns();
}

void DeterministicClassifier::initializePatterns() {
    // Light control patterns
    patterns_.push_back({
        "light_on",
        std::regex(R"(turn\s+on\s+(?:the\s+)?(.+?)\s+light)", std::regex::icase),
        "light", "turn_on", {1}
    });

    patterns_.push_back({
        "light_off",
        std::regex(R"(turn\s+off\s+(?:the\s+)?(.+?)\s+light)", std::regex::icase),
        "light", "turn_off", {1}
    });

    patterns_.push_back({
        "light_on",
        std::regex(R"(switch\s+on\s+(?:the\s+)?(.+?)\s+light)", std::regex::icase),
        "light", "turn_on", {1}
    });

    patterns_.push_back({
        "light_off",
        std::regex(R"(switch\s+off\s+(?:the\s+)?(.+?)\s+light)", std::regex::icase),
        "light", "turn_off", {1}
    });

    patterns_.push_back({
        "light_toggle",
        std::regex(R"(toggle\s+(?:the\s+)?(.+?)\s+light)", std::regex::icase),
        "light", "toggle", {1}
    });

    // Thermostat control patterns
    patterns_.push_back({
        "thermostat_set",
        std::regex(R"(set\s+(?:the\s+)?(.+?)\s+(?:thermostat|temperature)\s+to\s+(\d+))", std::regex::icase),
        "climate", "set_temperature", {1, 2}
    });

    patterns_.push_back({
        "thermostat_warmer",
        std::regex(R"(make\s+it\s+(warmer|hotter))", std::regex::icase),
        "climate", "increase_temperature", {}
    });

    patterns_.push_back({
        "thermostat_cooler",
        std::regex(R"(make\s+it\s+(cooler|colder))", std::regex::icase),
        "climate", "decrease_temperature", {}
    });

    // Lock control patterns
    patterns_.push_back({
        "lock_door",
        std::regex(R"(lock\s+(?:the\s+)?(.+?)\s+door)", std::regex::icase),
        "lock", "lock", {1}
    });

    patterns_.push_back({
        "unlock_door",
        std::regex(R"(unlock\s+(?:the\s+)?(.+?)\s+door)", std::regex::icase),
        "lock", "unlock", {1}
    });

    // Media control patterns
    patterns_.push_back({
        "media_play",
        std::regex(R"(play\s+(.+?)\s+on\s+spotify)", std::regex::icase),
        "media_player", "play_media", {1}
    });

    patterns_.push_back({
        "media_pause",
        std::regex(R"(pause\s+(?:the\s+)?music)", std::regex::icase),
        "media_player", "media_pause", {}
    });

    patterns_.push_back({
        "media_next",
        std::regex(R"(skip\s+(?:to\s+)?(?:the\s+)?next\s+(?:song|track))", std::regex::icase),
        "media_player", "media_next_track", {}
    });

    patterns_.push_back({
        "media_previous",
        std::regex(R"((?:go\s+to\s+)?previous\s+(?:song|track))", std::regex::icase),
        "media_player", "media_previous_track", {}
    });

    // Scene/mode control patterns
    patterns_.push_back({
        "scene_activate",
        std::regex(R"(activate\s+(.+?)\s+scene)", std::regex::icase),
        "scene", "turn_on", {1}
    });

    patterns_.push_back({
        "scene_activate",
        std::regex(R"(set\s+(.+?)\s+mode)", std::regex::icase),
        "scene", "turn_on", {1}
    });

    std::cout << "[Deterministic] Initialized " << patterns_.size() << " intent patterns" << std::endl;
}

IntentResult DeterministicClassifier::classify(const VoiceCommand& command) {
    auto start_time = std::chrono::high_resolution_clock::now();

    IntentResult result;
    result.success = false;
    result.tier = "deterministic";
    result.confidence = 0.0f;

    // Try to match against all patterns
    for (const auto& pattern : patterns_) {
        std::smatch match;
        if (std::regex_search(command.text, match, pattern.pattern)) {
            std::cout << "[Deterministic] Matched pattern: " << pattern.intent_name << std::endl;

            // Route to appropriate handler
            if (pattern.domain == "light") {
                if (pattern.action == "turn_on") {
                    result = processLightControl(match, command);
                } else if (pattern.action == "turn_off") {
                    result = processLightControl(match, command);
                } else if (pattern.action == "toggle") {
                    result = processLightControl(match, command);
                }
            } else if (pattern.domain == "climate") {
                result = processThermostatControl(match, command);
            } else if (pattern.domain == "lock") {
                result = processLockControl(match, command);
            } else if (pattern.domain == "media_player") {
                result = processMediaControl(match, command);
            } else if (pattern.domain == "scene") {
                result = processSceneControl(match, command);
            }

            if (result.success) {
                break;  // Stop at first successful match
            }
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    result.processing_time_ms = duration.count();

    return result;
}

IntentResult DeterministicClassifier::processLightControl(const std::smatch& match, const VoiceCommand& command) {
    IntentResult result;
    result.tier = "deterministic";

    // Extract location from regex match (group 1)
    std::string location = match[1].str();

    // Find matching light entity
    auto entities = haClient_->findEntities(location, "light");

    if (entities.empty()) {
        result.success = false;
        result.response_text = "I couldn't find a light matching '" + location + "'";
        result.tts_url = "";  // TODO: Generate TTS URL
        return result;
    }

    // Use first matching entity
    Entity light = entities[0];
    std::cout << "[Deterministic] Found light: " << light.entity_id << " (" << light.friendly_name << ")" << std::endl;

    // Determine action from command
    bool success = false;
    std::string action_str;

    if (command.text.find("turn on") != std::string::npos || command.text.find("switch on") != std::string::npos) {
        success = haClient_->turnOn(light.entity_id);
        action_str = "turned on";
    } else if (command.text.find("turn off") != std::string::npos || command.text.find("switch off") != std::string::npos) {
        success = haClient_->turnOff(light.entity_id);
        action_str = "turned off";
    } else if (command.text.find("toggle") != std::string::npos) {
        success = haClient_->toggle(light.entity_id);
        action_str = "toggled";
    }

    result.success = success;
    result.intent = "light_control";
    result.confidence = 0.95f;
    result.entities["entity_id"] = light.entity_id;
    result.entities["location"] = location;
    result.entities["action"] = action_str;

    if (success) {
        result.response_text = "I've " + action_str + " the " + light.friendly_name;
    } else {
        result.response_text = "Sorry, I couldn't control the " + light.friendly_name;
    }

    result.tts_url = "";  // TODO: Generate TTS URL with Wyoming Piper

    return result;
}

IntentResult DeterministicClassifier::processThermostatControl(const std::smatch& match, const VoiceCommand& command) {
    IntentResult result;
    result.tier = "deterministic";
    result.intent = "thermostat_control";

    // Extract location and temperature
    std::string location = match.size() > 1 ? match[1].str() : "main";
    int temperature = match.size() > 2 ? std::stoi(match[2].str()) : 0;

    // Find matching climate entity
    auto entities = haClient_->findEntities(location, "climate");

    if (entities.empty()) {
        result.success = false;
        result.response_text = "I couldn't find a thermostat matching '" + location + "'";
        return result;
    }

    Entity thermostat = entities[0];

    bool success = false;
    if (temperature > 0) {
        success = haClient_->setTemperature(thermostat.entity_id, static_cast<float>(temperature));
        result.response_text = "I've set the " + thermostat.friendly_name + " to " + std::to_string(temperature) + " degrees";
    } else if (command.text.find("warmer") != std::string::npos || command.text.find("hotter") != std::string::npos) {
        // Get current temperature and increase by 2 degrees
        Entity current = haClient_->getEntityState(thermostat.entity_id);
        float current_temp = current.attributes["temperature"].asFloat();
        success = haClient_->setTemperature(thermostat.entity_id, current_temp + 2.0f);
        result.response_text = "I've increased the temperature to " + std::to_string(static_cast<int>(current_temp + 2.0f)) + " degrees";
    } else if (command.text.find("cooler") != std::string::npos || command.text.find("colder") != std::string::npos) {
        Entity current = haClient_->getEntityState(thermostat.entity_id);
        float current_temp = current.attributes["temperature"].asFloat();
        success = haClient_->setTemperature(thermostat.entity_id, current_temp - 2.0f);
        result.response_text = "I've decreased the temperature to " + std::to_string(static_cast<int>(current_temp - 2.0f)) + " degrees";
    }

    result.success = success;
    result.confidence = 0.92f;
    result.entities["entity_id"] = thermostat.entity_id;

    return result;
}

IntentResult DeterministicClassifier::processLockControl(const std::smatch& match, const VoiceCommand& command) {
    IntentResult result;
    result.tier = "deterministic";
    result.intent = "lock_control";

    std::string location = match[1].str();
    auto entities = haClient_->findEntities(location, "lock");

    if (entities.empty()) {
        result.success = false;
        result.response_text = "I couldn't find a lock matching '" + location + "'";
        return result;
    }

    Entity lock = entities[0];

    bool success = false;
    std::string action_str;

    if (command.text.find("lock") != std::string::npos && command.text.find("unlock") == std::string::npos) {
        success = haClient_->callService("lock", "lock", lock.entity_id);
        action_str = "locked";
    } else if (command.text.find("unlock") != std::string::npos) {
        success = haClient_->callService("lock", "unlock", lock.entity_id);
        action_str = "unlocked";
    }

    result.success = success;
    result.confidence = 0.90f;
    result.entities["entity_id"] = lock.entity_id;
    result.entities["action"] = action_str;

    if (success) {
        result.response_text = "I've " + action_str + " the " + lock.friendly_name;
    } else {
        result.response_text = "Sorry, I couldn't control the " + lock.friendly_name;
    }

    return result;
}

IntentResult DeterministicClassifier::processMediaControl(const std::smatch& match, const VoiceCommand& command) {
    IntentResult result;
    result.tier = "deterministic";
    result.intent = "media_control";

    // Find media_player entity (default to first available)
    auto entities = haClient_->findEntities("", "media_player");

    if (entities.empty()) {
        result.success = false;
        result.response_text = "I couldn't find any media players";
        return result;
    }

    Entity media_player = entities[0];  // Use first available

    bool success = false;
    std::string action_str;

    if (command.text.find("pause") != std::string::npos) {
        success = haClient_->callService("media_player", "media_pause", media_player.entity_id);
        action_str = "paused";
    } else if (command.text.find("next") != std::string::npos) {
        success = haClient_->callService("media_player", "media_next_track", media_player.entity_id);
        action_str = "skipped to next track";
    } else if (command.text.find("previous") != std::string::npos) {
        success = haClient_->callService("media_player", "media_previous_track", media_player.entity_id);
        action_str = "went to previous track";
    }

    result.success = success;
    result.confidence = 0.88f;
    result.entities["entity_id"] = media_player.entity_id;

    if (success) {
        result.response_text = "I've " + action_str;
    } else {
        result.response_text = "Sorry, I couldn't control the media player";
    }

    return result;
}

IntentResult DeterministicClassifier::processSceneControl(const std::smatch& match, const VoiceCommand& command) {
    IntentResult result;
    result.tier = "deterministic";
    result.intent = "scene_control";

    std::string scene_name = match[1].str();
    auto entities = haClient_->findEntities(scene_name, "scene");

    if (entities.empty()) {
        result.success = false;
        result.response_text = "I couldn't find a scene matching '" + scene_name + "'";
        return result;
    }

    Entity scene = entities[0];
    bool success = haClient_->turnOn(scene.entity_id);

    result.success = success;
    result.confidence = 0.93f;
    result.entities["entity_id"] = scene.entity_id;

    if (success) {
        result.response_text = "I've activated the " + scene.friendly_name + " scene";
    } else {
        result.response_text = "Sorry, I couldn't activate the scene";
    }

    return result;
}
