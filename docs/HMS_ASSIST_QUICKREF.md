# HMS-Assist API — Quick Reference

**100% local voice assistant API, 3-tier intent classification, Home Assistant integration.**

- **Status:** Deployed as systemd service (`hms-assist.service`)
- **Port:** 8894
- **Binary:** `/usr/local/bin/hms_assist`
- **Config:** `/etc/hms-assist/config.yaml`
- **Repo:** https://github.com/hms-homelab/hms-assist-api

---

## Service Management

```bash
# Status
sudo systemctl status hms-assist
sudo systemctl status hms-assist-sync

# Logs
sudo journalctl -u hms-assist -f
sudo journalctl -u hms-assist-sync -f

# Restart
sudo systemctl restart hms-assist

# Health check
curl http://localhost:8894/health

# Force entity re-index
curl -X POST http://localhost:8894/admin/reindex
```

## Architecture

```
HTTP POST /api/v1/command
        │
        ▼
Tier 1: DeterministicClassifier   <5ms    16 regex patterns → HA REST
        │ (miss)
        ▼
Tier 2: EmbeddingClassifier      ~300ms   nomic-embed-text → pgvector → HA REST
        │ (miss)
        ▼
Tier 3: LLMClassifier            1–30s    llama3.1:8b → 120b cloud model
        │
        ▼
PostgreSQL log → HTTP response
```

## API Quick Reference

```bash
# Send a command
curl -X POST http://localhost:8894/api/v1/command \
  -H "Content-Type: application/json" \
  -d '{"text": "turn on the patio light", "device_id": "test"}'

# Health
curl http://localhost:8894/health

# Re-index entities
curl -X POST http://localhost:8894/admin/reindex
```

## Tier 1 Patterns

| Pattern | Example |
|---------|---------|
| Light on | "turn on / switch on the {room} light" |
| Light off | "turn off / switch off the {room} light" |
| Light toggle | "toggle the {room} light" |
| Thermostat set | "set the {room} thermostat to {N}" |
| Warmer/cooler | "make it warmer / cooler" |
| Lock | "lock the {room} door" |
| Unlock | "unlock the {room} door" |
| Pause | "pause the music / playback" |
| Next track | "skip to the next song" |
| Prev track | "previous track / go back" |
| Play | "play music / resume playback" |
| Scene | "activate {name} scene" |
| Mode | "set {name} mode" |

## Entity Sync

```bash
# Manual one-shot sync (re-embeds all 1115 entities)
HMS_ASSIST_CONFIG=/etc/hms-assist/config.yaml \
  tools/venv/bin/python tools/hms_assist_sync.py --once

# Sync runs automatically every 60 min via hms-assist-sync.service
# Threshold: 0.58 cosine similarity  |  Dimensions: 768 (nomic-embed-text)
# Asymmetric: search_document: prefix at index time, search_query: at query time
```

## Testing

```bash
cd /home/aamat/maestro_hub/projects/hms-assist

# All C++ unit tests (59)
build/hms_assist_tests

# Python sync unit tests (30)
tools/venv/bin/python -m pytest tools/tests/ -v

# Sync e2e tests (16) — needs Ollama + PostgreSQL
HMS_ASSIST_CONFIG=/etc/hms-assist/config.yaml \
  tools/venv/bin/python -m pytest tests/e2e/test_sync_e2e.py -v -k "not FullSync"

# API e2e tests (23) — needs running API
HMS_ASSIST_CONFIG=/etc/hms-assist/config.yaml \
  tools/venv/bin/python -m pytest tests/e2e/test_api_e2e.py -v
```

## PostgreSQL

```bash
psql -h localhost -U maestro -d hms_assist

-- Entity index
SELECT COUNT(*) FROM entity_embeddings;
SELECT entity_id, friendly_name, domain FROM entity_embeddings WHERE entity_id ILIKE '%sala%';

-- Command history
SELECT text, confidence FROM voice_commands ORDER BY created_at DESC LIMIT 10;

-- Intent results
SELECT intent, tier, confidence, success FROM intent_results ORDER BY created_at DESC LIMIT 10;

-- Success rate
SELECT tier, COUNT(*) total, COUNT(*) FILTER (WHERE success) ok
FROM intent_results GROUP BY tier ORDER BY tier;
```

## Config Fields

```yaml
homeassistant.url:                  # HA instance (http://192.168.2.7:8123)
homeassistant.token:                # Long-lived access token
database.ha_db_name:                # HA recorder DB for fast entity sync (optional)
ollama.embed_model:                 # nomic-embed-text (768-dim)
ollama.fast_model:                  # Tier 3a LLM
ollama.smart_model:                 # Tier 3b fallback LLM
ollama.escalation_threshold:        # Confidence below which tier3a escalates to 3b
service.vector_similarity_threshold: # Min cosine sim for tier2 match (0.58)
service.vector_search_limit:        # Max pgvector candidates (5)
```

## Rebuild After Code Changes

```bash
cd /home/aamat/maestro_hub/projects/hms-assist/build
make -j$(nproc) hms_assist
sudo cp hms_assist /usr/local/bin/hms_assist
sudo systemctl restart hms-assist
```
