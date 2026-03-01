#ifndef COMMAND_CONTROLLER_H
#define COMMAND_CONTROLLER_H

#include "intent/DeterministicClassifier.h"
#include "intent/EmbeddingClassifier.h"
#include "intent/LLMClassifier.h"
#include "clients/HomeAssistantClient.h"
#include "services/DatabaseService.h"
#include <memory>
#include <string>
#include <vector>
#include <drogon/drogon.h>

// Orchestrates the full classification pipeline:
// POST /api/v1/command → Tier1 → Tier2 → Tier3 → HA → TTS response
class CommandController {
public:
    CommandController(std::shared_ptr<IntentClassifier>    tier1,
                      std::shared_ptr<IntentClassifier>    tier2,
                      std::shared_ptr<IntentClassifier>    tier3,
                      std::shared_ptr<DatabaseService>     db,
                      std::shared_ptr<HomeAssistantClient> ha,
                      const std::string&                   ttsEntity);

    // POST /api/v1/command
    // Body: { text, device_id, confidence, context?, media_player_entity_id? }
    void handleCommand(const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)>&& cb);

    // POST /admin/reindex  — manual entity re-index trigger
    void handleReindex(const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)>&& cb);

private:
    std::shared_ptr<IntentClassifier>    tier1_;
    std::shared_ptr<IntentClassifier>    tier2_;
    std::shared_ptr<IntentClassifier>    tier3_;
    std::shared_ptr<DatabaseService>     db_;
    std::shared_ptr<HomeAssistantClient> ha_;
    std::string                          ttsEntity_;

    IntentResult runPipeline(const VoiceCommand& command);
    IntentResult runSinglePipeline(const VoiceCommand& command);  // tier1 → tier2 only
    // Execute a SplitResult's sub-commands (wave-based parallel execution).
    // initialResponse is prepended to the combined response text (e.g. from prior Tier2 hits).
    IntentResult executeSplit(const SplitResult& split,
                              const VoiceCommand& origin,
                              const std::string& initialResponse,
                              const std::string& tierLabel);
    // Split a command text on hard dividers (and/then/followed by) or 2+ action verbs.
    // Returns a single-element vector if no compound detected.
    static std::vector<std::string> regexSplitCompound(const std::string& text);

    static drogon::HttpResponsePtr errorResponse(int status, const std::string& message);
    static Json::Value buildResponse(const IntentResult& result);
};

#endif // COMMAND_CONTROLLER_H
