#include "services/VectorSearchService.h"
#include <sstream>
#include <iostream>
#include <stdexcept>

VectorSearchService::VectorSearchService(const std::string& connStr) : connStr_(connStr) {}

std::vector<EntityMatch> VectorSearchService::search(const std::vector<float>& embedding,
                                                       float threshold,
                                                       int limit) {
    pqxx::connection conn(connStr_);
    pqxx::work txn(conn);

    std::string vecLiteral = toVectorLiteral(embedding);

    std::string sql =
        "SELECT entity_id, domain, friendly_name, state, attributes_json, "
        "       1 - (embedding <=> " + txn.quote(vecLiteral) + "::vector) AS similarity "
        "FROM entity_embeddings "
        "WHERE 1 - (embedding <=> " + txn.quote(vecLiteral) + "::vector) >= " + std::to_string(threshold) + " "
        "ORDER BY similarity DESC "
        "LIMIT " + std::to_string(limit);

    pqxx::result rows = txn.exec(sql);

    std::vector<EntityMatch> results;
    for (const auto& row : rows) {
        EntityMatch m;
        m.entity_id       = row["entity_id"].as<std::string>();
        m.domain          = row["domain"].as<std::string>();
        m.friendly_name   = row["friendly_name"].as<std::string>("");
        m.state           = row["state"].as<std::string>("");
        m.attributes_json = row["attributes_json"].as<std::string>("{}");
        m.similarity      = row["similarity"].as<float>();
        results.push_back(m);
    }

    txn.commit();
    return results;
}

void VectorSearchService::upsertEntity(const std::string& entityId,
                                        const std::string& domain,
                                        const std::string& friendlyName,
                                        const std::string& state,
                                        const std::string& attributesJson,
                                        const std::vector<float>& embedding) {
    pqxx::connection conn(connStr_);
    pqxx::work txn(conn);

    std::string vecLiteral = toVectorLiteral(embedding);

    txn.exec(
        "INSERT INTO entity_embeddings "
        "  (entity_id, domain, friendly_name, state, attributes_json, embedding, last_updated) "
        "VALUES (" +
        txn.quote(entityId) + ", " +
        txn.quote(domain) + ", " +
        txn.quote(friendlyName) + ", " +
        txn.quote(state) + ", " +
        txn.quote(attributesJson) + ", " +
        txn.quote(vecLiteral) + "::vector, NOW()) "
        "ON CONFLICT (entity_id) DO UPDATE SET "
        "  domain = EXCLUDED.domain, "
        "  friendly_name = EXCLUDED.friendly_name, "
        "  state = EXCLUDED.state, "
        "  attributes_json = EXCLUDED.attributes_json, "
        "  embedding = EXCLUDED.embedding, "
        "  last_updated = NOW()"
    );

    txn.commit();
}

void VectorSearchService::pruneEntities(const std::vector<std::string>& activeEntityIds) {
    if (activeEntityIds.empty()) return;

    pqxx::connection conn(connStr_);
    pqxx::work txn(conn);

    std::string inList;
    for (size_t i = 0; i < activeEntityIds.size(); ++i) {
        if (i > 0) inList += ",";
        inList += txn.quote(activeEntityIds[i]);
    }

    txn.exec("DELETE FROM entity_embeddings WHERE entity_id NOT IN (" + inList + ")");
    txn.commit();
}

int VectorSearchService::entityCount() {
    pqxx::connection conn(connStr_);
    pqxx::work txn(conn);
    auto row = txn.exec("SELECT COUNT(*) FROM entity_embeddings").one_row();
    txn.commit();
    return row[0].as<int>();
}

std::string VectorSearchService::toVectorLiteral(const std::vector<float>& vec) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < vec.size(); ++i) {
        if (i > 0) oss << ",";
        oss << vec[i];
    }
    oss << "]";
    return oss.str();
}
