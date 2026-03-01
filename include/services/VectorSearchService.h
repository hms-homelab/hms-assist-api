#ifndef VECTOR_SEARCH_SERVICE_H
#define VECTOR_SEARCH_SERVICE_H

#include <string>
#include <vector>
#include <pqxx/pqxx>

struct EntityMatch {
    std::string entity_id;
    std::string domain;
    std::string friendly_name;
    std::string state;
    std::string attributes_json;
    float similarity{0.0f};
};

class VectorSearchService {
public:
    explicit VectorSearchService(const std::string& connStr);
    virtual ~VectorSearchService() = default;

    // Find top-K entities by cosine similarity to the given embedding
    virtual std::vector<EntityMatch> search(const std::vector<float>& embedding,
                                            float threshold,
                                            int limit = 5);

    // Upsert a single entity embedding
    void upsertEntity(const std::string& entityId,
                      const std::string& domain,
                      const std::string& friendlyName,
                      const std::string& state,
                      const std::string& attributesJson,
                      const std::vector<float>& embedding);

    // Remove entities not in the provided set (cleanup after full sync)
    void pruneEntities(const std::vector<std::string>& activeEntityIds);

    virtual int entityCount();

    // Public utility: formats a float vector as a pgvector literal "[a,b,c]"
    static std::string toVectorLiteral(const std::vector<float>& vec);

private:
    std::string connStr_;
};

#endif // VECTOR_SEARCH_SERVICE_H
