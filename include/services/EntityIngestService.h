#ifndef ENTITY_INGEST_SERVICE_H
#define ENTITY_INGEST_SERVICE_H

#include "clients/HomeAssistantClient.h"
#include "clients/OllamaClient.h"
#include "services/VectorSearchService.h"
#include <string>
#include <atomic>
#include <thread>
#include <pqxx/pqxx>

class EntityIngestService {
public:
    // haDbConnStr: connection string for the 'homeassistant' PostgreSQL DB
    // (direct DB query — faster than REST API)
    EntityIngestService(std::shared_ptr<HomeAssistantClient> haClient,
                        std::shared_ptr<OllamaClient> ollamaClient,
                        std::shared_ptr<VectorSearchService> vectorSearch,
                        const std::string& embedModel,
                        const std::string& haDbConnStr = "");

    ~EntityIngestService();

    // Run a full sync: fetch entities from HA, embed, upsert into vector DB
    // Returns number of entities processed
    int ingest();

    // Start background hourly sync thread
    void startBackgroundSync();

    void stop();

private:
    std::shared_ptr<HomeAssistantClient> ha_;
    std::shared_ptr<OllamaClient> ollama_;
    std::shared_ptr<VectorSearchService> vectorSearch_;
    std::string embedModel_;
    std::string haDbConnStr_;

    std::atomic<bool> running_{false};
    std::thread syncThread_;

    // Fetch entities directly from the homeassistant PostgreSQL DB
    std::vector<Entity> fetchEntitiesFromDb();

    // Build a text description of an entity for embedding
    static std::string buildEntityDescription(const Entity& entity);
};

#endif // ENTITY_INGEST_SERVICE_H
