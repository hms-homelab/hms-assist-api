#ifndef VOICE_SERVICE_H
#define VOICE_SERVICE_H

#include "clients/MqttClient.h"
#include "clients/HomeAssistantClient.h"
#include "intent/DeterministicClassifier.h"
#include "services/DatabaseService.h"
#include <memory>
#include <string>

class VoiceService {
public:
    VoiceService(std::shared_ptr<MqttClient> mqttClient,
                 std::shared_ptr<HomeAssistantClient> haClient,
                 std::shared_ptr<DatabaseService> dbService);

    void start();
    void stop();

private:
    void onMqttMessage(const std::string& topic, const std::string& payload);
    void processVoiceCommand(const std::string& deviceId, const VoiceCommand& command);
    void publishIntentResult(const std::string& deviceId, const IntentResult& result);

    std::shared_ptr<MqttClient> mqttClient_;
    std::shared_ptr<HomeAssistantClient> haClient_;
    std::shared_ptr<DatabaseService> dbService_;
    std::shared_ptr<DeterministicClassifier> deterministicClassifier_;

    bool running_;
};

#endif // VOICE_SERVICE_H
