#include "services/VoiceService.h"
#include <json/json.h>
#include <iostream>
#include <chrono>

VoiceService::VoiceService(std::shared_ptr<MqttClient> mqttClient,
                           std::shared_ptr<HomeAssistantClient> haClient,
                           std::shared_ptr<DatabaseService> dbService)
    : mqttClient_(mqttClient), haClient_(haClient), dbService_(dbService), running_(false) {

    // Initialize deterministic classifier
    deterministicClassifier_ = std::make_shared<DeterministicClassifier>(haClient_);
}

void VoiceService::start() {
    std::cout << "[VoiceService] Starting service..." << std::endl;

    running_ = true;

    // Set MQTT message callback
    mqttClient_->setMessageCallback([this](const std::string& topic, const std::string& payload) {
        onMqttMessage(topic, payload);
    });

    // Subscribe to STT result topics (wildcard for all devices)
    mqttClient_->subscribe("hms_assist/voice/+/stt_result", 1);

    std::cout << "[VoiceService] Service started and listening for voice commands" << std::endl;
}

void VoiceService::stop() {
    std::cout << "[VoiceService] Stopping service..." << std::endl;
    running_ = false;
}

void VoiceService::onMqttMessage(const std::string& topic, const std::string& payload) {
    std::cout << "[VoiceService] Received message on topic: " << topic << std::endl;

    // Parse topic to extract device ID
    // Format: hms_assist/voice/{device_id}/stt_result
    size_t start = topic.find("/voice/") + 7;
    size_t end = topic.find("/", start);
    std::string deviceId = topic.substr(start, end - start);

    // Parse JSON payload
    Json::Reader reader;
    Json::Value root;

    if (reader.parse(payload, root)) {
        VoiceCommand command;
        command.device_id = deviceId;
        command.text = root["text"].asString();
        command.confidence = root["confidence"].asFloat();

        std::cout << "[VoiceService] Voice command from " << deviceId << ": \"" << command.text << "\"" << std::endl;

        processVoiceCommand(deviceId, command);
    } else {
        std::cerr << "[VoiceService] Failed to parse JSON payload" << std::endl;
    }
}

void VoiceService::processVoiceCommand(const std::string& deviceId, const VoiceCommand& command) {
    // Log command to database
    int commandId = dbService_->logVoiceCommand(command);

    // Classify intent using deterministic classifier (Tier 1)
    IntentResult result = deterministicClassifier_->classify(command);

    if (!result.success) {
        // TODO: Fallback to Tier 2 (embeddings) and Tier 3 (LLM) when implemented
        std::cout << "[VoiceService] Deterministic classification failed, no fallback available yet" << std::endl;

        // Return error response
        result.success = false;
        result.response_text = "Sorry, I didn't understand that command";
        result.tts_url = "";  // TODO: Generate TTS for error message
    }

    // Log intent result
    if (commandId > 0) {
        dbService_->logIntentResult(commandId, result);
    }

    // Publish result back to MQTT
    publishIntentResult(deviceId, result);
}

void VoiceService::publishIntentResult(const std::string& deviceId, const IntentResult& result) {
    // Build JSON response
    Json::Value response;
    response["success"] = result.success;
    response["intent"] = result.intent;
    response["tier"] = result.tier;
    response["confidence"] = result.confidence;
    response["response_text"] = result.response_text;
    response["tts_url"] = result.tts_url;
    response["processing_time_ms"] = result.processing_time_ms;

    // Convert entities to JSON
    response["entities"] = result.entities;

    Json::StreamWriterBuilder writer;
    std::string jsonStr = Json::writeString(writer, response);

    // Publish to MQTT
    std::string topic = "hms_assist/voice/" + deviceId + "/intent_result";
    mqttClient_->publish(topic, jsonStr, 1, false);

    std::cout << "[VoiceService] Published intent result to " << topic << std::endl;
}
