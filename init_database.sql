-- HMS-Assist Voice Assistant Database Schema
-- Creates database and tables for storing voice commands and intent results

-- Create database (run as postgres superuser)
-- CREATE DATABASE hms_assist;

-- Connect to the database
\c hms_assist

-- Voice commands table
CREATE TABLE IF NOT EXISTS voice_commands (
    id SERIAL PRIMARY KEY,
    device_id VARCHAR(100) NOT NULL,
    command_text TEXT NOT NULL,
    confidence FLOAT,
    timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    CONSTRAINT valid_confidence CHECK (confidence >= 0 AND confidence <= 1)
);

-- Intent results table
CREATE TABLE IF NOT EXISTS intent_results (
    id SERIAL PRIMARY KEY,
    command_id INTEGER REFERENCES voice_commands(id) ON DELETE CASCADE,
    intent VARCHAR(100),
    tier VARCHAR(20) CHECK (tier IN ('deterministic', 'embedding', 'llm')),
    confidence FLOAT,
    response_text TEXT,
    processing_time_ms INTEGER,
    success BOOLEAN DEFAULT FALSE,
    entities TEXT[],
    timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    CONSTRAINT valid_confidence CHECK (confidence >= 0 AND confidence <= 1)
);

-- Indexes for faster queries
CREATE INDEX IF NOT EXISTS idx_commands_device ON voice_commands(device_id);
CREATE INDEX IF NOT EXISTS idx_commands_timestamp ON voice_commands(timestamp DESC);
CREATE INDEX IF NOT EXISTS idx_results_intent ON intent_results(intent);
CREATE INDEX IF NOT EXISTS idx_results_tier ON intent_results(tier);
CREATE INDEX IF NOT EXISTS idx_results_success ON intent_results(success);
CREATE INDEX IF NOT EXISTS idx_results_timestamp ON intent_results(timestamp DESC);

-- View for command statistics
CREATE OR REPLACE VIEW command_statistics AS
SELECT
    COUNT(*) as total_commands,
    COUNT(CASE WHEN ir.success = TRUE THEN 1 END) as successful_commands,
    ROUND(100.0 * COUNT(CASE WHEN ir.success = TRUE THEN 1 END) / COUNT(*), 2) as success_rate,
    AVG(ir.processing_time_ms) as avg_processing_time_ms
FROM voice_commands vc
LEFT JOIN intent_results ir ON vc.id = ir.command_id;

-- View for intent distribution
CREATE OR REPLACE VIEW intent_distribution AS
SELECT
    intent,
    tier,
    COUNT(*) as count,
    AVG(confidence) as avg_confidence,
    AVG(processing_time_ms) as avg_processing_time_ms,
    ROUND(100.0 * COUNT(CASE WHEN success = TRUE THEN 1 END) / COUNT(*), 2) as success_rate
FROM intent_results
GROUP BY intent, tier
ORDER BY count DESC;

-- View for device activity
CREATE OR REPLACE VIEW device_activity AS
SELECT
    device_id,
    COUNT(*) as total_commands,
    MAX(timestamp) as last_activity,
    COUNT(CASE WHEN ir.success = TRUE THEN 1 END) as successful_commands
FROM voice_commands vc
LEFT JOIN intent_results ir ON vc.id = ir.command_id
GROUP BY device_id
ORDER BY last_activity DESC;

-- Sample queries
COMMENT ON VIEW command_statistics IS 'Overall statistics for voice commands and intents';
COMMENT ON VIEW intent_distribution IS 'Distribution of intents by type and tier';
COMMENT ON VIEW device_activity IS 'Activity summary per voice assistant device';

COMMENT ON TABLE voice_commands IS 'Stores all voice commands received from voice assistant devices';
COMMENT ON TABLE intent_results IS 'Stores intent classification results and execution outcomes';

-- Grant permissions to maestro user
GRANT ALL PRIVILEGES ON DATABASE hms_assist TO maestro;
GRANT ALL PRIVILEGES ON ALL TABLES IN SCHEMA public TO maestro;
GRANT ALL PRIVILEGES ON ALL SEQUENCES IN SCHEMA public TO maestro;
GRANT SELECT ON ALL TABLES IN SCHEMA public TO maestro;

-- Example usage queries
-- Get recent commands:
-- SELECT * FROM voice_commands ORDER BY timestamp DESC LIMIT 10;

-- Get command success rate by intent:
-- SELECT * FROM intent_distribution;

-- Get recent successful commands:
-- SELECT vc.command_text, ir.intent, ir.response_text, ir.processing_time_ms
-- FROM voice_commands vc
-- JOIN intent_results ir ON vc.id = ir.command_id
-- WHERE ir.success = TRUE
-- ORDER BY vc.timestamp DESC
-- LIMIT 10;

-- Get tier performance:
-- SELECT tier, COUNT(*) as count, AVG(processing_time_ms) as avg_time,
--        ROUND(100.0 * COUNT(CASE WHEN success = TRUE THEN 1 END) / COUNT(*), 2) as success_rate
-- FROM intent_results
-- GROUP BY tier;
