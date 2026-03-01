#ifndef LLM_CLASSIFIER_H
#define LLM_CLASSIFIER_H

#include "intent/IntentClassifier.h"
#include "clients/OllamaClient.h"
#include <memory>
#include <string>

// Tier 3: LLM-based command parser.
// Parses any voice command (single or compound) into sub-commands with sequencing info.
// No entity context is passed — entity resolution is handled by Tier1/Tier2 after split.
// Non-HA responses are answered by fast_model; escalated to smart_model when confidence
// is below threshold or the model sets escalate:true.
class LLMClassifier : public IntentClassifier {
public:
    LLMClassifier(std::shared_ptr<OllamaClient> ollama,
                  const std::string& fastModel,
                  const std::string& smartModel,
                  float escalationThreshold);

    // Not used — Tier3 always goes through split().
    IntentResult classify(const VoiceCommand&) override { return {}; }

    // Parse command into sub-commands. Non-HA parts returned in non_ha.
    SplitResult split(const VoiceCommand& command) override;

private:
    std::shared_ptr<OllamaClient> ollama_;
    std::string fastModel_;
    std::string smartModel_;
    float escalationThreshold_;
};

#endif // LLM_CLASSIFIER_H
