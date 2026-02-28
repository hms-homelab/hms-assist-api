#ifndef OLLAMA_CLIENT_H
#define OLLAMA_CLIENT_H

#include <string>
#include <vector>
#include <json/json.h>

class OllamaClient {
public:
    explicit OllamaClient(const std::string& baseUrl);

    // Generate embedding vector for text
    std::vector<float> embed(const std::string& text, const std::string& model);

    // Chat completion — returns raw response text
    std::string chat(const std::string& userPrompt,
                     const std::string& model,
                     const std::string& systemPrompt = "");

    // Chat completion — extracts and parses JSON object from model response
    Json::Value chatJson(const std::string& userPrompt,
                         const std::string& model,
                         const std::string& systemPrompt = "");

    bool isReachable();

private:
    std::string baseUrl_;

    std::string post(const std::string& endpoint, const std::string& body);
    static std::string extractJsonBlock(const std::string& raw);
};

#endif // OLLAMA_CLIENT_H
