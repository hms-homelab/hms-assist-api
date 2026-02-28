#include "clients/HomeAssistantClient.h"
#include <curl/curl.h>
#include <iostream>
#include <algorithm>
#include <cctype>

// Callback for CURL to write response data
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

HomeAssistantClient::HomeAssistantClient(const std::string& baseUrl, const std::string& bearerToken)
    : baseUrl_(baseUrl), bearerToken_(bearerToken) {
}

std::string HomeAssistantClient::makeRequest(const std::string& endpoint, const std::string& method, const std::string& postData) {
    CURL* curl = curl_easy_init();
    std::string response;

    if (curl) {
        std::string url = baseUrl_ + endpoint;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

        // Set headers
        struct curl_slist* headers = NULL;
        std::string auth_header = "Authorization: Bearer " + bearerToken_;
        headers = curl_slist_append(headers, auth_header.c_str());
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        // Set method
        if (method == "POST") {
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postData.c_str());
        }

        // Set callback
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

        // Perform request
        CURLcode res = curl_easy_perform(curl);

        if (res != CURLE_OK) {
            std::cerr << "[HA Client] Request failed: " << curl_easy_strerror(res) << std::endl;
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }

    return response;
}

std::vector<Entity> HomeAssistantClient::getAllEntities() {
    std::vector<Entity> entities;

    std::string response = makeRequest("/api/states");

    Json::Reader reader;
    Json::Value root;

    if (reader.parse(response, root) && root.isArray()) {
        for (const auto& item : root) {
            Entity entity;
            entity.entity_id = item["entity_id"].asString();
            entity.state = item["state"].asString();
            entity.attributes = item["attributes"];

            if (item["attributes"].isMember("friendly_name")) {
                entity.friendly_name = item["attributes"]["friendly_name"].asString();
            }

            // Extract domain from entity_id (e.g., "light.kitchen" -> "light")
            size_t dot_pos = entity.entity_id.find('.');
            if (dot_pos != std::string::npos) {
                entity.domain = entity.entity_id.substr(0, dot_pos);
            }

            entities.push_back(entity);
        }
    }

    std::cout << "[HA Client] Fetched " << entities.size() << " entities" << std::endl;
    return entities;
}

Entity HomeAssistantClient::getEntityState(const std::string& entityId) {
    Entity entity;

    std::string response = makeRequest("/api/states/" + entityId);

    Json::Reader reader;
    Json::Value root;

    if (reader.parse(response, root)) {
        entity.entity_id = root["entity_id"].asString();
        entity.state = root["state"].asString();
        entity.attributes = root["attributes"];

        if (root["attributes"].isMember("friendly_name")) {
            entity.friendly_name = root["attributes"]["friendly_name"].asString();
        }

        size_t dot_pos = entity.entity_id.find('.');
        if (dot_pos != std::string::npos) {
            entity.domain = entity.entity_id.substr(0, dot_pos);
        }
    }

    return entity;
}

std::vector<Entity> HomeAssistantClient::findEntities(const std::string& query, const std::string& domain) {
    std::vector<Entity> results;
    std::vector<Entity> allEntities = getAllEntities();

    // Convert query to lowercase for case-insensitive matching
    std::string lowerQuery = query;
    std::transform(lowerQuery.begin(), lowerQuery.end(), lowerQuery.begin(), ::tolower);

    for (const auto& entity : allEntities) {
        // Filter by domain if specified
        if (!domain.empty() && entity.domain != domain) {
            continue;
        }

        // Check if query matches entity_id or friendly_name
        std::string lowerEntityId = entity.entity_id;
        std::string lowerFriendlyName = entity.friendly_name;
        std::transform(lowerEntityId.begin(), lowerEntityId.end(), lowerEntityId.begin(), ::tolower);
        std::transform(lowerFriendlyName.begin(), lowerFriendlyName.end(), lowerFriendlyName.begin(), ::tolower);

        if (lowerEntityId.find(lowerQuery) != std::string::npos ||
            lowerFriendlyName.find(lowerQuery) != std::string::npos) {
            results.push_back(entity);
        }
    }

    std::cout << "[HA Client] Found " << results.size() << " entities matching '" << query << "'" << std::endl;
    return results;
}

bool HomeAssistantClient::callService(const std::string& domain, const std::string& service,
                                     const std::string& entityId, const Json::Value& parameters) {
    Json::Value payload;
    payload["entity_id"] = entityId;

    // Merge parameters
    for (const auto& key : parameters.getMemberNames()) {
        payload[key] = parameters[key];
    }

    Json::StreamWriterBuilder writer;
    std::string postData = Json::writeString(writer, payload);

    std::string endpoint = "/api/services/" + domain + "/" + service;
    std::string response = makeRequest(endpoint, "POST", postData);

    // Check if response is valid JSON array (success)
    Json::Reader reader;
    Json::Value root;
    bool success = reader.parse(response, root) && root.isArray() && root.size() > 0;

    if (success) {
        std::cout << "[HA Client] Service call successful: " << domain << "." << service
                  << " on " << entityId << std::endl;
    } else {
        std::cerr << "[HA Client] Service call failed: " << domain << "." << service
                  << " on " << entityId << std::endl;
    }

    return success;
}

bool HomeAssistantClient::turnOn(const std::string& entityId, const Json::Value& parameters) {
    size_t dot_pos = entityId.find('.');
    if (dot_pos == std::string::npos) {
        return false;
    }
    std::string domain = entityId.substr(0, dot_pos);
    return callService(domain, "turn_on", entityId, parameters);
}

bool HomeAssistantClient::turnOff(const std::string& entityId) {
    size_t dot_pos = entityId.find('.');
    if (dot_pos == std::string::npos) {
        return false;
    }
    std::string domain = entityId.substr(0, dot_pos);
    return callService(domain, "turn_off", entityId);
}

bool HomeAssistantClient::toggle(const std::string& entityId) {
    size_t dot_pos = entityId.find('.');
    if (dot_pos == std::string::npos) {
        return false;
    }
    std::string domain = entityId.substr(0, dot_pos);
    return callService(domain, "toggle", entityId);
}

bool HomeAssistantClient::setTemperature(const std::string& entityId, float temperature) {
    Json::Value params;
    params["temperature"] = temperature;
    return callService("climate", "set_temperature", entityId, params);
}
