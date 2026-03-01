#include "services/EntityIngestService.h"
#include <iostream>
#include <chrono>
#include <sstream>
#include <algorithm>
#include <json/json.h>

EntityIngestService::EntityIngestService(std::shared_ptr<HomeAssistantClient> haClient,
                                          std::shared_ptr<OllamaClient> ollamaClient,
                                          std::shared_ptr<VectorSearchService> vectorSearch,
                                          const std::string& embedModel,
                                          const std::string& haDbConnStr)
    : ha_(haClient), ollama_(ollamaClient), vectorSearch_(vectorSearch),
      embedModel_(embedModel), haDbConnStr_(haDbConnStr) {}

EntityIngestService::~EntityIngestService() {
    stop();
}

int EntityIngestService::ingest() {
    std::cout << "[EntityIngest] Starting entity sync from Home Assistant..." << std::endl;

    std::vector<Entity> entities;
    if (!haDbConnStr_.empty()) {
        try {
            entities = fetchEntitiesFromDb();
            std::cout << "[EntityIngest] Fetched " << entities.size()
                      << " entities from HA PostgreSQL DB" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[EntityIngest] DB fetch failed, falling back to REST API: "
                      << e.what() << std::endl;
        }
    }
    if (entities.empty()) {
        try {
            entities = ha_->getAllEntities();
            std::cout << "[EntityIngest] Fetched " << entities.size()
                      << " entities via HA REST API" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[EntityIngest] Failed to fetch entities: " << e.what() << std::endl;
            return 0;
        }
    }

    // Skip domains that aren't voice-controllable
    static const std::vector<std::string> skipDomains = {
        "sun", "persistent_notification", "update", "event",
        "zone", "person", "weather", "device_tracker"
    };

    std::vector<std::string> activeIds;
    int processed = 0;

    for (const auto& entity : entities) {
        // Skip non-controllable domains
        bool skip = false;
        for (const auto& d : skipDomains) {
            if (entity.domain == d) { skip = true; break; }
        }
        if (skip) continue;

        activeIds.push_back(entity.entity_id);

        try {
            std::string description = buildEntityDescription(entity);
            std::vector<float> embedding = ollama_->embed(description, embedModel_);

            // Serialize attributes to JSON string
            Json::StreamWriterBuilder wb;
            std::string attrJson = Json::writeString(wb, entity.attributes);

            vectorSearch_->upsertEntity(
                entity.entity_id,
                entity.domain,
                entity.friendly_name,
                entity.state,
                attrJson,
                embedding
            );

            processed++;

            if (processed % 20 == 0) {
                std::cout << "[EntityIngest] Processed " << processed << "/" << entities.size() << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "[EntityIngest] Failed to embed " << entity.entity_id << ": " << e.what() << std::endl;
        }
    }

    // Remove stale entities no longer in HA
    try {
        vectorSearch_->pruneEntities(activeIds);
    } catch (const std::exception& e) {
        std::cerr << "[EntityIngest] Prune failed: " << e.what() << std::endl;
    }

    std::cout << "[EntityIngest] Sync complete. " << processed << " entities indexed." << std::endl;
    return processed;
}

void EntityIngestService::startBackgroundSync() {
    running_ = true;
    syncThread_ = std::thread([this]() {
        // Initial sync on startup
        ingest();

        // Then sync every hour
        while (running_) {
            for (int i = 0; i < 3600 && running_; ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            if (running_) ingest();
        }
    });
    std::cout << "[EntityIngest] Background sync started (hourly)" << std::endl;
}

void EntityIngestService::stop() {
    running_ = false;
    if (syncThread_.joinable()) syncThread_.join();
}

std::vector<Entity> EntityIngestService::fetchEntitiesFromDb() {
    // Query the latest state for every entity directly from the HA recorder DB.
    // Much faster than the REST API and avoids network overhead.
    pqxx::connection conn(haDbConnStr_);
    pqxx::work txn(conn);

    // Get latest state per entity_id using DISTINCT ON (PostgreSQL specific)
    pqxx::result rows = txn.exec(R"(
        SELECT DISTINCT ON (s.entity_id)
               s.entity_id,
               s.state,
               COALESCE(sa.shared_attrs, '{}') AS attributes
        FROM   states s
        LEFT JOIN state_attributes sa ON s.attributes_id = sa.attributes_id
        WHERE  s.state IS NOT NULL
        ORDER  BY s.entity_id, s.last_updated_ts DESC
    )");
    txn.commit();

    std::vector<Entity> entities;
    Json::CharReaderBuilder rb;

    for (const auto& row : rows) {
        Entity e;
        e.entity_id = row["entity_id"].as<std::string>();
        e.state     = row["state"].as<std::string>("");

        // Extract domain from entity_id (e.g. "light.kitchen" → "light")
        auto dot = e.entity_id.find('.');
        e.domain = (dot != std::string::npos) ? e.entity_id.substr(0, dot) : e.entity_id;

        // Parse attributes JSON
        std::string attrStr = row["attributes"].as<std::string>("{}");
        std::string errs;
        std::istringstream ss(attrStr);
        Json::parseFromStream(rb, ss, &e.attributes, &errs);

        // friendly_name lives in attributes
        if (e.attributes.isMember("friendly_name")) {
            e.friendly_name = e.attributes["friendly_name"].asString();
        } else {
            // Fall back to entity_id without domain prefix
            e.friendly_name = (dot != std::string::npos)
                ? e.entity_id.substr(dot + 1) : e.entity_id;
            // Replace underscores with spaces for readability
            std::replace(e.friendly_name.begin(), e.friendly_name.end(), '_', ' ');
        }

        entities.push_back(e);
    }

    return entities;
}

std::string EntityIngestService::buildEntityDescription(const Entity& entity) {
    // Build a rich text description that captures what this entity is and does.
    // This is what gets embedded — richer = better semantic matches.
    std::ostringstream oss;

    oss << entity.friendly_name;
    if (!entity.domain.empty()) oss << " (" << entity.domain << ")";
    if (!entity.state.empty())  oss << ", currently " << entity.state;

    // Add area/location from attributes if available
    if (entity.attributes.isMember("area") && !entity.attributes["area"].asString().empty()) {
        oss << ", area: " << entity.attributes["area"].asString();
    }
    if (entity.attributes.isMember("room") && !entity.attributes["room"].asString().empty()) {
        oss << ", room: " << entity.attributes["room"].asString();
    }

    // Device class gives semantic meaning (e.g. "motion", "door", "temperature")
    if (entity.attributes.isMember("device_class") &&
        !entity.attributes["device_class"].asString().empty()) {
        oss << ", device class: " << entity.attributes["device_class"].asString();
    }

    // For media players, add supported features context
    if (entity.domain == "media_player") {
        oss << ", controllable: play pause stop volume next previous";
    }

    // Entity ID often encodes location (e.g. light.kitchen_ceiling)
    oss << ", entity_id: " << entity.entity_id;

    return oss.str();
}
