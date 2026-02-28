#ifndef INTENT_CLASSIFIER_H
#define INTENT_CLASSIFIER_H

#include <string>
#include <json/json.h>

struct VoiceCommand {
    std::string device_id;
    std::string text;
    float confidence;
};

struct IntentResult {
    bool success;
    std::string intent;
    std::string tier;  // "deterministic", "embedding", "llm"
    float confidence;
    std::string response_text;
    std::string tts_url;
    int processing_time_ms;
    Json::Value entities;  // Additional intent data
};

class IntentClassifier {
public:
    virtual ~IntentClassifier() = default;

    /**
     * @brief Classify voice command and extract intent
     *
     * @param command Voice command from STT
     * @return IntentResult with classification result
     */
    virtual IntentResult classify(const VoiceCommand& command) = 0;

    /**
     * @brief Get classifier tier name
     */
    virtual std::string getTier() const = 0;
};

#endif // INTENT_CLASSIFIER_H
