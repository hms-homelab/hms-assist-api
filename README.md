# hms-assist-api

[![Docker](https://ghcr-badge.egpl.dev/hms-homelab/hms-assist-api/size)](https://github.com/hms-homelab/hms-assist-api/pkgs/container/hms-assist-api)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

**100% local voice assistant API with 3-tier intent classification and Home Assistant integration.**

Part of the [maestro-hub](https://github.com/hms-homelab) home automation platform.

---

## Docker Quick Start

```bash
docker pull ghcr.io/hms-homelab/hms-assist-api:latest

docker run -d \
  -p 8894:8894 \
  -v /etc/hms-assist/config.yaml:/etc/hms-assist/config.yaml:ro \
  ghcr.io/hms-homelab/hms-assist-api:latest
```

See [Configuration](#configuration) for the config file format.

---

## What It Does

Accepts a natural-language voice command (text) via HTTP POST, classifies intent through a 3-tier pipeline, executes the corresponding Home Assistant action, and returns a structured result — all locally, no cloud required.

```
POST /api/v1/command
{"text": "can you brighten up the sala", "device_id": "satellite_1"}

→ {"tier": "tier2", "intent": "light_turn_on", "confidence": 0.65,
   "response_text": "Turned on the Sala 1.", "success": true, ...}
```

## Architecture

```
Voice Command (HTTP POST)
        │
        ▼
┌──────────────────────────────────────────────────────┐
│               3-Tier Classification Pipeline          │
│                                                      │
│  Tier 1 — DeterministicClassifier        <5ms, ~70%  │
│  ├─ 16 regex patterns                               │
│  └─ Exact entity match via HA REST API              │
│                                                      │
│  Tier 2 — EmbeddingClassifier          ~300ms, ~20%  │
│  ├─ nomic-embed-text (768-dim, search_query prefix) │
│  ├─ pgvector cosine search (1115 entities, HNSW)    │
│  └─ Action inferred from command text               │
│                                                      │
│  Tier 3 — LLMClassifier              1–30s, ~10%    │
│  ├─ Tier 3a: llama3.1:8b (fast)                    │
│  └─ Tier 3b: 120b cloud model (smart fallback)     │
│                                                      │
└──────────────────────────────────────────────────────┘
        │
        ▼
  Home Assistant REST API  →  PostgreSQL logging
```

## Supported Commands

### Tier 1 — Regex patterns (deterministic, always <5ms)

| Intent | Example phrases |
|--------|----------------|
| Light on/off/toggle | "turn on the kitchen light", "switch off sala 2", "toggle hallway" |
| Thermostat set | "set the living room thermostat to 72" |
| Thermostat warmer/cooler | "make it warmer", "make it cooler" |
| Lock/unlock | "lock the front door", "unlock the entryway" |
| Media pause | "pause the music", "pause playback" |
| Media next/prev | "skip to the next song", "previous track" |
| Media play | "play music", "resume playback" |
| Scene activate | "activate movie night scene" |
| Scene mode | "set night mode" |

### Tier 2 — Vector search (semantic paraphrases, ~300ms)

Any phrasing that references a known HA entity by name, even colloquially:

- "can you brighten up the sala" → `light.sala_1`
- "sala lights on" → `light.sala_1`
- "dim the sala" → `light.sala_1`

1115 entities indexed with `nomic-embed-text` + voice command examples. Threshold: 0.58 cosine similarity.

#### Sensor queries (instant, no LLM)

Tier 2 also handles read-only sensor queries by returning the current HA state directly — no LLM call:

- "tell me the outdoor temperature" → "AWN Outdoor Temperature is 71.10 °F"
- "what's the humidity outside" → "AWN Outdoor Humidity is 58%"
- "how's the air quality" → sensor state returned instantly

The `sensor` domain is detected at classification time; `getEntityState()` is called instead of `callService()`.

### Tier 3 — LLM (complex / ambiguous, 1–30s)

Open-ended requests that don't match any pattern or entity name:

- "make the living room cozy"
- "I'm heading to bed"

## Quick Start

### Prerequisites

```bash
# C++ dependencies
sudo apt install cmake g++ libdrogon-dev libjsoncpp-dev \
    libpqxx-dev libcurl4-openssl-dev libyaml-cpp-dev \
    libgtest-dev libgmock-dev postgresql-client python3 python3-venv

# PostgreSQL: database hms_assist must exist
# Ollama: nomic-embed-text + llama3.1:8b must be pulled on your Ollama server
```

### Install

```bash
git clone https://github.com/hms-homelab/hms-assist-api.git
cd hms-assist-api

# Copy and edit config
sudo mkdir -p /etc/hms-assist
sudo cp config/config.yaml.example /etc/hms-assist/config.yaml
sudo nano /etc/hms-assist/config.yaml   # fill in HA token, DB password, Ollama URL

# Build, test, and install
./build_and_install.sh
```

`build_and_install.sh` will:
1. Initialize the PostgreSQL schema
2. Build the C++ binary and run all unit tests
3. Install `/usr/local/bin/hms_assist`
4. Set up the Python entity-sync venv
5. Register and start both systemd services

### Verify

```bash
curl http://localhost:8894/health
# → {"status":"healthy","components":{"entity_count":1115,...}}

curl -X POST http://localhost:8894/api/v1/command \
  -H "Content-Type: application/json" \
  -d '{"text": "turn on the patio light", "device_id": "test"}'
# → {"tier":"tier1","intent":"light_control","confidence":0.95,...}
```

## Services

| Service | Unit | Description |
|---------|------|-------------|
| API | `hms-assist.service` | C++ HTTP server on port 8894 |
| Sync | `hms-assist-sync.service` | Python entity indexer (runs every 60 min) |

```bash
# API
sudo systemctl status hms-assist
sudo journalctl -u hms-assist -f

# Entity sync
sudo systemctl status hms-assist-sync
sudo journalctl -u hms-assist-sync -f

# Manual re-index via API
curl -X POST http://localhost:8894/admin/reindex
```

## API Reference

### `GET /health`

```json
{
  "status": "healthy",
  "service": "hms-assist",
  "version": "2.0.0",
  "components": {
    "database": "connected",
    "vector_db": "connected",
    "entity_count": 1115
  },
  "statistics": {
    "total_commands": 42,
    "successful_intents": 39
  }
}
```

### `POST /api/v1/command`

**Request:**
```json
{
  "text": "turn on the patio light",
  "device_id": "satellite_1",
  "confidence": 1.0,
  "media_player_entity_id": "media_player.living_room_speaker"
}
```

> **TTS note:** Include `media_player_entity_id` to have the response spoken via a Home Assistant media player. The `response_text` field is always returned regardless.

**Response (success):**
```json
{
  "success": true,
  "tier": "tier1",
  "intent": "light_control",
  "confidence": 0.95,
  "response_text": "Turned on the Patio Light.",
  "processing_time_ms": 3,
  "entities": {
    "entity_id": "light.patio",
    "domain": "light",
    "friendly_name": "Patio Light",
    "action": "turn_on",
    "state": "on"
  }
}
```

**Response (no match):**
```json
{
  "success": false,
  "tier": "tier3b",
  "intent": "",
  "confidence": 0.0,
  "response_text": "I'm not sure how to help with that.",
  "processing_time_ms": 8420
}
```

HTTP status: `200` on success, `400` on bad request, `422` if all tiers fail.

### `POST /admin/reindex`

Triggers a background entity re-sync from Home Assistant. Returns immediately.

```json
{"success": true, "message": "Sync started — tail /tmp/hms_assist_sync.log"}
```

## Configuration

Config file: `/etc/hms-assist/config.yaml`

```yaml
homeassistant:
  url: http://<ha-host>:8123
  token: <long-lived token>

database:
  host: <db-host>
  port: 5432
  name: hms_assist
  user: maestro
  password: <password>
  ha_db_name: homeassistant    # optional: direct HA recorder DB for faster sync

ollama:
  url: http://<ollama-host>:11434
  embed_model: nomic-embed-text
  fast_model: llama3.2:3b
  smart_model: llama3.1:70b-instruct-q4_K_M
  escalation_threshold: 0.7

wyoming:
  piper_host: <wyoming-host>
  piper_port: 10200
  whisper_host: <wyoming-host>
  whisper_port: 10300

service:
  port: 8894
  vector_similarity_threshold: 0.58
  vector_search_limit: 5
```

## Entity Sync

The sync tool (`tools/hms_assist_sync.py`) populates pgvector with HA entity embeddings:

- Fetches entities from the HA recorder DB (fast) or REST API (fallback)
- Builds a rich description for each entity including voice command examples
- Embeds with `nomic-embed-text` using `search_document:` asymmetric prefix
- Upserts into `entity_embeddings` table (768-dim HNSW index)
- Prunes stale entities no longer in HA

```bash
# Manual one-shot sync
HMS_ASSIST_CONFIG=/etc/hms-assist/config.yaml \
  tools/venv/bin/python tools/hms_assist_sync.py --once

# Dry-run (shows descriptions, no DB writes)
HMS_ASSIST_CONFIG=/etc/hms-assist/config.yaml \
  tools/venv/bin/python tools/hms_assist_sync.py --dry-run
```

## Testing

```bash
# C++ unit tests (no network/DB required)
cd build && ./hms_assist_tests

# Python sync unit tests
tools/venv/bin/python -m pytest tools/tests/ -v

# Sync e2e tests (requires Ollama + PostgreSQL)
HMS_ASSIST_CONFIG=/etc/hms-assist/config.yaml \
  tools/venv/bin/python -m pytest tests/e2e/test_sync_e2e.py -v -k "not FullSync"

# API e2e tests (requires running API on port 8894)
HMS_ASSIST_CONFIG=/etc/hms-assist/config.yaml \
  tools/venv/bin/python -m pytest tests/e2e/test_api_e2e.py -v
```

**Test counts:** 77 C++ unit · 30 Python sync unit · 16 sync e2e · 23 API e2e = **146 total**

## Project Structure

```
hms-assist-api/
├── CMakeLists.txt
├── hms-assist.service            # API systemd unit
├── build_and_install.sh          # Build + install script
├── init_database.sql             # PostgreSQL schema
├── config/
│   └── config.yaml.example       # Config template
├── include/
│   ├── api/CommandController.h
│   ├── clients/HomeAssistantClient.h
│   ├── intent/
│   │   ├── IntentClassifier.h    # Abstract base class
│   │   ├── DeterministicClassifier.h
│   │   ├── EmbeddingClassifier.h
│   │   └── LLMClassifier.h
│   ├── services/
│   │   ├── DatabaseService.h
│   │   ├── OllamaClient.h
│   │   └── VectorSearchService.h
│   └── utils/ConfigManager.h
├── src/
│   ├── main.cpp
│   ├── api/CommandController.cpp
│   ├── clients/HomeAssistantClient.cpp
│   ├── intent/{Deterministic,Embedding,LLM}Classifier.cpp
│   └── services/{Database,Ollama,VectorSearch}Service.cpp
├── tools/
│   ├── hms_assist_sync.py        # Entity indexer
│   ├── hms-assist-sync.service   # Sync systemd unit
│   ├── requirements.txt
│   └── tests/                    # Python unit tests
└── tests/
    ├── unit/                     # C++ unit tests (GoogleTest + GMock)
    └── e2e/                      # Python e2e tests (pytest)
```

## Performance

| Scenario | Tier | Latency |
|----------|------|---------|
| "turn on the patio light" | tier1 | ~3ms |
| "sala lights on" | tier2 | ~300ms |
| "can you brighten up the sala" | tier2 | ~300ms |
| "make the living room cozy" | tier3a | ~5–15s |
| Complex multi-entity request | tier3b | ~20–40s |

## Stack

- **C++ API:** Drogon HTTP, libpqxx, libcurl, jsoncpp, yaml-cpp
- **pgvector:** PostgreSQL 17 extension, 768-dim HNSW cosine index
- **Embeddings:** nomic-embed-text via Ollama (asymmetric search_query/search_document prefixes)
- **LLM:** llama3.1:8b-instruct-q4_K_M (tier3a) + cloud model (tier3b) via Ollama
- **Python sync:** psycopg2, requests, PyYAML

## License

MIT
---

## Support

[\![Buy Me A Coffee](https://www.buymeacoffee.com/assets/img/custom_images/orange_img.png)](https://www.buymeacoffee.com/aamat09)
