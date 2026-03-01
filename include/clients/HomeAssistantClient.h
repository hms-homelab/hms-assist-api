#ifndef HOME_ASSISTANT_CLIENT_H
#define HOME_ASSISTANT_CLIENT_H

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <chrono>
#include <json/json.h>

struct Entity {
    std::string entity_id;
    std::string state;
    std::string friendly_name;
    std::string domain;  // light, switch, climate, etc.
    Json::Value attributes;
};

class HomeAssistantClient {
public:
    HomeAssistantClient(const std::string& baseUrl, const std::string& bearerToken);
    virtual ~HomeAssistantClient() = default;

    // Get all entities
    virtual std::vector<Entity> getAllEntities();

    // Get specific entity state
    virtual Entity getEntityState(const std::string& entityId);

    // Find entities by fuzzy matching (e.g., "kitchen" -> "light.kitchen_ceiling")
    virtual std::vector<Entity> findEntities(const std::string& query, const std::string& domain = "");

    // Call Home Assistant service
    virtual bool callService(const std::string& domain, const std::string& service,
                    const std::string& entityId, const Json::Value& parameters = Json::Value());

    // Common service shortcuts
    virtual bool turnOn(const std::string& entityId, const Json::Value& parameters = Json::Value());
    virtual bool turnOff(const std::string& entityId);
    virtual bool toggle(const std::string& entityId);
    virtual bool setTemperature(const std::string& entityId, float temperature);

private:
    std::string makeRequest(const std::string& endpoint, const std::string& method = "GET",
                           const std::string& postData = "");

    std::string baseUrl_;
    std::string bearerToken_;

    mutable std::mutex entityCacheMutex_;
    std::vector<Entity> entityCache_;
    std::chrono::steady_clock::time_point entityCacheTime_;
    static constexpr int kEntityCacheTtlSeconds = 300;
};

#endif // HOME_ASSISTANT_CLIENT_H
