#ifndef DETERMINISTIC_CLASSIFIER_H
#define DETERMINISTIC_CLASSIFIER_H

#include "intent/IntentClassifier.h"
#include "clients/HomeAssistantClient.h"
#include <regex>
#include <vector>
#include <memory>

struct IntentPattern {
    std::string intent_name;
    std::regex pattern;
    std::string domain;  // light, switch, climate, etc.
    std::string action;  // turn_on, turn_off, set_temperature, etc.
    std::vector<int> capture_groups;  // Which regex groups to extract
};

class DeterministicClassifier : public IntentClassifier {
public:
    explicit DeterministicClassifier(std::shared_ptr<HomeAssistantClient> haClient);

    IntentResult classify(const VoiceCommand& command) override;

private:
    void initializePatterns();

    IntentResult processLightControl(const std::smatch& match, const VoiceCommand& command);
    IntentResult processThermostatControl(const std::smatch& match, const VoiceCommand& command);
    IntentResult processLockControl(const std::smatch& match, const VoiceCommand& command);
    IntentResult processMediaControl(const std::smatch& match, const VoiceCommand& command);
    IntentResult processDeviceControl(const std::smatch& match, const VoiceCommand& command);
    IntentResult processSceneControl(const std::smatch& match, const VoiceCommand& command);

    std::shared_ptr<HomeAssistantClient> haClient_;
    std::vector<IntentPattern> patterns_;
};

#endif // DETERMINISTIC_CLASSIFIER_H
