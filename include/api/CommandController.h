#ifndef COMMAND_CONTROLLER_H
#define COMMAND_CONTROLLER_H

#include "intent/DeterministicClassifier.h"
#include "intent/EmbeddingClassifier.h"
#include "intent/LLMClassifier.h"
#include "services/DatabaseService.h"
#include "services/EntityIngestService.h"
#include <memory>
#include <drogon/drogon.h>

// Orchestrates the full classification pipeline:
// POST /api/v1/command → Tier1 → Tier2 → Tier3a → Tier3b → HA → response
class CommandController {
public:
    CommandController(std::shared_ptr<DeterministicClassifier> tier1,
                      std::shared_ptr<EmbeddingClassifier> tier2,
                      std::shared_ptr<LLMClassifier> tier3,
                      std::shared_ptr<DatabaseService> db,
                      std::shared_ptr<EntityIngestService> ingest);

    // POST /api/v1/command
    // Body: { text, device_id, confidence, context? }
    void handleCommand(const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)>&& cb);

    // POST /admin/reindex  — manual entity re-index trigger
    void handleReindex(const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)>&& cb);

private:
    std::shared_ptr<DeterministicClassifier> tier1_;
    std::shared_ptr<EmbeddingClassifier>     tier2_;
    std::shared_ptr<LLMClassifier>           tier3_;
    std::shared_ptr<DatabaseService>         db_;
    std::shared_ptr<EntityIngestService>     ingest_;

    IntentResult runPipeline(const VoiceCommand& command);
    static drogon::HttpResponsePtr errorResponse(int status, const std::string& message);
    static Json::Value buildResponse(const IntentResult& result);
};

#endif // COMMAND_CONTROLLER_H
