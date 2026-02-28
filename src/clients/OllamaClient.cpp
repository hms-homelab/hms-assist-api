#include "clients/OllamaClient.h"
#include <curl/curl.h>
#include <iostream>
#include <stdexcept>

namespace {
size_t writeCallback(void* contents, size_t size, size_t nmemb, std::string* out) {
    out->append(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}
} // namespace

OllamaClient::OllamaClient(const std::string& baseUrl) : baseUrl_(baseUrl) {}

std::vector<float> OllamaClient::embed(const std::string& text, const std::string& model) {
    Json::Value req;
    req["model"] = model;
    req["prompt"] = text;

    Json::StreamWriterBuilder wb;
    std::string body = Json::writeString(wb, req);

    std::string resp = post("/api/embeddings", body);

    Json::Value root;
    Json::CharReaderBuilder rb;
    std::string errs;
    std::istringstream ss(resp);
    if (!Json::parseFromStream(rb, ss, &root, &errs)) {
        throw std::runtime_error("OllamaClient::embed parse error: " + errs);
    }

    std::vector<float> result;
    const Json::Value& emb = root["embedding"];
    result.reserve(emb.size());
    for (const auto& v : emb) {
        result.push_back(v.asFloat());
    }
    return result;
}

std::string OllamaClient::chat(const std::string& userPrompt,
                                const std::string& model,
                                const std::string& systemPrompt) {
    Json::Value req;
    req["model"] = model;
    req["stream"] = false;

    Json::Value messages(Json::arrayValue);
    if (!systemPrompt.empty()) {
        Json::Value sys;
        sys["role"] = "system";
        sys["content"] = systemPrompt;
        messages.append(sys);
    }
    Json::Value user;
    user["role"] = "user";
    user["content"] = userPrompt;
    messages.append(user);
    req["messages"] = messages;

    Json::StreamWriterBuilder wb;
    std::string body = Json::writeString(wb, req);

    std::string resp = post("/api/chat", body);

    Json::Value root;
    Json::CharReaderBuilder rb;
    std::string errs;
    std::istringstream ss(resp);
    if (!Json::parseFromStream(rb, ss, &root, &errs)) {
        throw std::runtime_error("OllamaClient::chat parse error: " + errs);
    }

    return root["message"]["content"].asString();
}

Json::Value OllamaClient::chatJson(const std::string& userPrompt,
                                    const std::string& model,
                                    const std::string& systemPrompt) {
    std::string raw = chat(userPrompt, model, systemPrompt);
    std::string jsonStr = extractJsonBlock(raw);

    Json::Value result;
    Json::CharReaderBuilder rb;
    std::string errs;
    std::istringstream ss(jsonStr);
    if (!Json::parseFromStream(rb, ss, &result, &errs)) {
        throw std::runtime_error("OllamaClient::chatJson failed to parse JSON from model response: " + errs + "\nRaw: " + raw);
    }
    return result;
}

bool OllamaClient::isReachable() {
    try {
        post("/api/tags", "");
        return true;
    } catch (...) {
        return false;
    }
}

std::string OllamaClient::post(const std::string& endpoint, const std::string& body) {
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("OllamaClient: curl_easy_init failed");

    std::string response;
    std::string url = baseUrl_ + endpoint;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L); // LLM calls can be slow

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    if (!body.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body.size());
    } else {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    }

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        throw std::runtime_error(std::string("OllamaClient HTTP error: ") + curl_easy_strerror(res));
    }
    return response;
}

std::string OllamaClient::extractJsonBlock(const std::string& raw) {
    // Find the first '{' and last '}' to extract JSON object
    auto start = raw.find('{');
    auto end   = raw.rfind('}');
    if (start == std::string::npos || end == std::string::npos || end < start) {
        throw std::runtime_error("No JSON object found in model response: " + raw);
    }
    return raw.substr(start, end - start + 1);
}
