#ifndef COMMAND_CONTROLLER_H
#define COMMAND_CONTROLLER_H

#include "intent/DeterministicClassifier.h"
#include "intent/EmbeddingClassifier.h"
#include "intent/LLMClassifier.h"
#include "services/DatabaseService.h"
#include <memory>
#include <drogon/drogon.h>

// Orchestrates the full classification pipeline:
// POST /api/v1/command → Tier1 → Tier2 → Tier3a → Tier3b → HA → response
class CommandController {
public:
    // Tiers are accepted as the base IntentClassifier so test doubles can be injected.
    CommandController(std::shared_ptr<IntentClassifier> tier1,
                      std::shared_ptr<IntentClassifier> tier2,
                      std::shared_ptr<IntentClassifier> tier3,
                      std::shared_ptr<DatabaseService> db);

    // POST /api/v1/command
    // Body: { text, device_id, confidence, context? }
    void handleCommand(const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)>&& cb);

    // POST /admin/reindex  — manual entity re-index trigger
    void handleReindex(const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)>&& cb);

private:
    std::shared_ptr<IntentClassifier> tier1_;
    std::shared_ptr<IntentClassifier> tier2_;
    std::shared_ptr<IntentClassifier> tier3_;
    std::shared_ptr<DatabaseService>  db_;

    IntentResult runPipeline(const VoiceCommand& command);
    static drogon::HttpResponsePtr errorResponse(int status, const std::string& message);
    static Json::Value buildResponse(const IntentResult& result);
};

#endif // COMMAND_CONTROLLER_H
