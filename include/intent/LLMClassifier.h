#ifndef LLM_CLASSIFIER_H
#define LLM_CLASSIFIER_H

#include "intent/IntentClassifier.h"
#include "clients/OllamaClient.h"
#include "clients/HomeAssistantClient.h"
#include "services/VectorSearchService.h"
#include <memory>
#include <string>
#include <vector>

struct LLMCommand {
    std::string intent;
    std::string entity_id;
    std::string domain;
    std::string action;
    Json::Value params;  // temperature, media_content_id, etc.
};

struct LLMPlan {
    std::vector<LLMCommand> commands;
    bool escalate{false};          // 8b sets this → route to smart model
    std::string response_text;
    float confidence{0.0f};
};

// Tier 3: LLM-based intent parsing and command splitting.
// Routes: fast_model (8b) first. If escalate=true → smart_model (120b).
class LLMClassifier : public IntentClassifier {
public:
    LLMClassifier(std::shared_ptr<OllamaClient> ollama,
                  std::shared_ptr<HomeAssistantClient> haClient,
                  std::shared_ptr<VectorSearchService> vectorSearch,
                  const std::string& embedModel,
                  const std::string& fastModel,
                  const std::string& smartModel,
                  float escalationThreshold);

    IntentResult classify(const VoiceCommand& command) override;

    // Refresh the entity snapshot used in the system prompt (called after ingest)
    void refreshEntityCache(const std::vector<Entity>& entities);

    // Split a compound command into individual sub-commands (no entity context needed).
    SplitResult split(const VoiceCommand& command) override;

private:
    std::shared_ptr<OllamaClient> ollama_;
    std::shared_ptr<HomeAssistantClient> ha_;
    std::shared_ptr<VectorSearchService> vectorSearch_;
    std::string embedModel_;
    std::string fastModel_;
    std::string smartModel_;
    float escalationThreshold_;
    std::string entityCacheJson_;  // Fallback: full entity list (used if vector pre-filter fails)

    LLMPlan parsePlan(const Json::Value& llmJson);
    IntentResult executePlan(const LLMPlan& plan, const VoiceCommand& command,
                             const std::string& modelUsed);

    std::string buildSystemPrompt(const std::string& entityJson) const;
    std::string buildUserPrompt(const VoiceCommand& command) const;
    std::string preFilterEntities(const VoiceCommand& command) const;
};

#endif // LLM_CLASSIFIER_H
