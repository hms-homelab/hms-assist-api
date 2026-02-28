#ifndef EMBEDDING_CLASSIFIER_H
#define EMBEDDING_CLASSIFIER_H

#include "intent/IntentClassifier.h"
#include "clients/OllamaClient.h"
#include "clients/HomeAssistantClient.h"
#include "services/VectorSearchService.h"
#include <memory>
#include <string>

// Tier 2: semantic vector search.
// Embeds the voice command, finds the closest entity in the vector DB,
// then infers the intended action from the command text.
class EmbeddingClassifier : public IntentClassifier {
public:
    EmbeddingClassifier(std::shared_ptr<OllamaClient> ollama,
                        std::shared_ptr<HomeAssistantClient> haClient,
                        std::shared_ptr<VectorSearchService> vectorSearch,
                        const std::string& embedModel,
                        float similarityThreshold,
                        int searchLimit);

    IntentResult classify(const VoiceCommand& command) override;

private:
    std::shared_ptr<OllamaClient> ollama_;
    std::shared_ptr<HomeAssistantClient> ha_;
    std::shared_ptr<VectorSearchService> vectorSearch_;
    std::string embedModel_;
    float threshold_;
    int limit_;

    // Infer HA service action from command text + entity domain
    static std::string inferAction(const std::string& text, const std::string& domain);

    // Build response text for a successful match
    static std::string buildResponse(const std::string& action,
                                     const std::string& friendlyName);
};

#endif // EMBEDDING_CLASSIFIER_H
