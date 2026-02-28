-- HMS-Assist v2.0 Database Schema
-- Run as postgres superuser to create DB, then as maestro for the rest.

-- CREATE DATABASE hms_assist;  -- run once as superuser
\c hms_assist

-- pgvector extension (requires pgvector to be installed)
CREATE EXTENSION IF NOT EXISTS vector;

-- -----------------------------------------------------------------------
-- Voice commands — every STT result received
-- -----------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS voice_commands (
    id         SERIAL PRIMARY KEY,
    device_id  VARCHAR(100) NOT NULL,
    text       TEXT NOT NULL,
    confidence FLOAT CHECK (confidence >= 0 AND confidence <= 1),
    context    TEXT,  -- JSON context from request
    timestamp  TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX IF NOT EXISTS idx_commands_device    ON voice_commands(device_id);
CREATE INDEX IF NOT EXISTS idx_commands_timestamp ON voice_commands(timestamp DESC);

-- -----------------------------------------------------------------------
-- Intent results — one row per classification attempt
-- -----------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS intent_results (
    id                 SERIAL PRIMARY KEY,
    command_id         INTEGER REFERENCES voice_commands(id) ON DELETE CASCADE,
    intent             VARCHAR(100),
    tier               VARCHAR(10) CHECK (tier IN ('tier1','tier2','tier3a','tier3b')),
    confidence         FLOAT CHECK (confidence >= 0 AND confidence <= 1),
    response_text      TEXT,
    processing_time_ms INTEGER,
    success            BOOLEAN DEFAULT FALSE,
    entities           JSONB,  -- flexible KV: entity_id, state, commands array, etc.
    timestamp          TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX IF NOT EXISTS idx_results_command   ON intent_results(command_id);
CREATE INDEX IF NOT EXISTS idx_results_intent    ON intent_results(intent);
CREATE INDEX IF NOT EXISTS idx_results_tier      ON intent_results(tier);
CREATE INDEX IF NOT EXISTS idx_results_success   ON intent_results(success);
CREATE INDEX IF NOT EXISTS idx_results_timestamp ON intent_results(timestamp DESC);

-- -----------------------------------------------------------------------
-- Entity embeddings — vector index of HA entities for Tier 2 search
-- nomic-embed-text outputs 768 dimensions
-- -----------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS entity_embeddings (
    id              SERIAL PRIMARY KEY,
    entity_id       VARCHAR(255) UNIQUE NOT NULL,
    domain          VARCHAR(100),
    friendly_name   TEXT,
    state           TEXT,
    attributes_json JSONB,
    embedding       vector(768),
    last_updated    TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- HNSW index for fast cosine similarity (better than IVFFlat for <10k vectors)
CREATE INDEX IF NOT EXISTS idx_entity_embeddings_hnsw
    ON entity_embeddings USING hnsw (embedding vector_cosine_ops)
    WITH (m = 16, ef_construction = 64);

CREATE INDEX IF NOT EXISTS idx_entity_domain ON entity_embeddings(domain);

-- -----------------------------------------------------------------------
-- Views
-- -----------------------------------------------------------------------
CREATE OR REPLACE VIEW command_statistics AS
SELECT
    COUNT(*)                                                        AS total_commands,
    COUNT(CASE WHEN ir.success = TRUE THEN 1 END)                  AS successful_commands,
    ROUND(100.0 * COUNT(CASE WHEN ir.success THEN 1 END) / NULLIF(COUNT(*),0), 2) AS success_rate,
    AVG(ir.processing_time_ms)                                     AS avg_processing_time_ms
FROM voice_commands vc
LEFT JOIN intent_results ir ON vc.id = ir.command_id;

CREATE OR REPLACE VIEW intent_distribution AS
SELECT
    intent,
    tier,
    COUNT(*)                                                              AS count,
    AVG(confidence)                                                       AS avg_confidence,
    AVG(processing_time_ms)                                               AS avg_processing_time_ms,
    ROUND(100.0 * COUNT(CASE WHEN success THEN 1 END) / COUNT(*), 2)     AS success_rate
FROM intent_results
GROUP BY intent, tier
ORDER BY count DESC;

CREATE OR REPLACE VIEW device_activity AS
SELECT
    vc.device_id,
    COUNT(*)                                           AS total_commands,
    MAX(vc.timestamp)                                  AS last_activity,
    COUNT(CASE WHEN ir.success THEN 1 END)             AS successful_commands,
    ROUND(100.0 * COUNT(CASE WHEN ir.success THEN 1 END) / COUNT(*), 2) AS success_rate
FROM voice_commands vc
LEFT JOIN intent_results ir ON vc.id = ir.command_id
GROUP BY vc.device_id
ORDER BY last_activity DESC;

CREATE OR REPLACE VIEW tier_performance AS
SELECT
    tier,
    COUNT(*)                                                          AS total,
    ROUND(100.0 * COUNT(CASE WHEN success THEN 1 END) / COUNT(*), 2) AS success_rate,
    AVG(processing_time_ms)                                           AS avg_ms,
    MIN(processing_time_ms)                                           AS min_ms,
    MAX(processing_time_ms)                                           AS max_ms
FROM intent_results
GROUP BY tier
ORDER BY tier;

-- -----------------------------------------------------------------------
-- Permissions
-- -----------------------------------------------------------------------
GRANT ALL PRIVILEGES ON ALL TABLES    IN SCHEMA public TO maestro;
GRANT ALL PRIVILEGES ON ALL SEQUENCES IN SCHEMA public TO maestro;
