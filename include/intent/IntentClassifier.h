#ifndef INTENT_CLASSIFIER_H
#define INTENT_CLASSIFIER_H

#include <string>
#include <vector>
#include <json/json.h>

struct VoiceCommand {
    std::string device_id;
    std::string text;
    float confidence{1.0f};
    std::string context;          // JSON string: room, last_intent, etc.
    std::string already_executed; // reserved, no longer used by pipeline
    bool dry_run{false};          // if true: resolve entity_id but do NOT call HA service
};

struct IntentResult {
    bool success{false};
    std::string intent;
    std::string tier;  // "tier1", "tier2", "tier3a", "tier3b"
    float confidence{0.0f};
    std::string response_text;
    int processing_time_ms{0};
    Json::Value entities;  // Flexible KV bag: entity_id, state, commands array, etc.
};

struct SubCommand {
    std::string text;
    bool wait_for_previous{false};  // true = wait for all currently running commands first
};

struct SplitResult {
    std::vector<SubCommand> sub_commands;
    std::string non_ha;       // answer for non-HA parts (jokes, questions)
    float confidence{1.0f};   // fast model confidence in the non_ha answer
    bool escalate{false};     // fast model requests smart model escalation
};

class IntentClassifier {
public:
    virtual ~IntentClassifier() = default;
    virtual IntentResult classify(const VoiceCommand& command) = 0;
    // Split a compound command into sub-commands. Default: no-op (returns empty).
    virtual SplitResult split(const VoiceCommand&) { return {}; }
};

#endif // INTENT_CLASSIFIER_H
