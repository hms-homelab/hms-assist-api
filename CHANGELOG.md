# Changelog

All notable changes to hms-assist-api will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [2.3.0] - 2026-02-28

### Changed
- **Compound command architecture** (LLM-as-splitter): commands containing ` and ` now route
  to a lightweight LLM split step (no entity context) that returns individual sub-command
  strings and an optional non-HA answer (jokes, questions). Each sub-command is then
  independently routed through tier1 → tier2. This replaces the previous full-entity-context
  approach, cutting compound command latency from ~16s to ~7s.

### Added
- `IntentClassifier::split()` virtual method (default no-op) so any tier3 implementation
  can optionally handle compound splitting
- `LLMClassifier::split()` — tiny prompt, no entity list, outputs `sub_commands[]` + `non_ha`
- Pre-filtered entity context for single ambiguous tier3 calls: embeds the command and
  passes only the top-25 most relevant entities to the LLM (prevents 0.5 confidence
  saturation from 1147-entity context)
- `format: "json"` in Ollama API requests for `chatJson()` — enforces JSON output at the
  API level regardless of model tendency to generate prose

## [2.2.0] - 2026-02-28

### Changed
- **Compound command routing**: commands containing ` and ` now bypass tier2 (single-entity
  vector search) and route directly to tier3 (LLM). This ensures "turn off X and turn on Y"
  executes both intents instead of silently resolving only the best-matching entity.

## [2.1.0] - 2026-02-28

### Added
- **Restart/reboot/cycle support** in EmbeddingClassifier (tier2): detects restart/reboot/cycle
  keywords and executes turn_off → 1s sleep → turn_on sequentially against the matched entity
- **VERSION file** and build number injected at compile time via CMake
- **CHANGELOG.md** following Keep a Changelog format

### Fixed
- **Word-boundary matching** in `EmbeddingClassifier::inferAction`: replaced `t.find("off")`
  with `\boff\b` regex so entity names containing "off" as substring (e.g. "coffee", "offset")
  no longer incorrectly trigger `turn_off`. Same fix applied across all domains.
- **Systemd WorkingDirectory**: added `WorkingDirectory=/var/lib/hms-assist` so Drogon's
  upload temp dirs are created in a writable location. Removed `PrivateTmp=true` which conflicted.
- **Drogon log path**: changed from `"./"` to `/var/log/hms-assist` so the binary works from
  any working directory under systemd.

## [2.0.0] - 2026-02-28

### Added
- **Tier 2 — EmbeddingClassifier**: pgvector cosine search (768-dim HNSW, nomic-embed-text)
  - Asymmetric embeddings: `search_document:` prefix at index time, `search_query:` at query time
  - Voice command examples baked into entity descriptions for better semantic recall
  - Similarity threshold: 0.58 (calibrated against real queries)
- **Entity sync tool** (`tools/hms_assist_sync.py`): indexes 1115+ HA entities into pgvector
  - Fetches from HA recorder DB (fast path) or REST API (fallback)
  - Systemd service (`hms-assist-sync.service`), runs every 60 minutes
- **Tier 3 — LLMClassifier**: llama3.1:8b (tier3a) → 120b cloud model (tier3b) via Ollama
  - Multi-command execution (parallel HA calls)
  - Escalation threshold for routing between fast and smart model
- **Test suite** — 128 tests total:
  - 59 C++ unit tests (GoogleTest + GMock): ConfigManager, DeterministicClassifier, CommandController
  - 30 Python sync unit tests (unittest.mock)
  - 16 sync e2e tests (Ollama + pgvector)
  - 23 API e2e tests (live API on :8894)
- **`/admin/reindex` endpoint**: triggers background entity re-sync
- **`build_and_install.sh`**: full install script (binary + venv + both systemd services)
- **`config/config.yaml.example`**: config template with all fields documented

### Fixed
- `DatabaseService::logVoiceCommand`: column `command_text` → `text` (schema mismatch)
- `DatabaseService::logIntentResult`: entities serialization ARRAY → JSONB
- `HomeAssistantClient::callService`: success check `root.isArray()` not `root.size() > 0`
  (HA returns `[]` for idempotent calls — already on/off)
- HA recorder DB query: join `states_meta` via `metadata_id` (modern HA schema)
- `EmbeddingClassifier`: tier field `"embedding"` → `"tier2"`

### Changed
- `vector_similarity_threshold`: 0.82 → 0.58 (calibrated with asymmetric embeddings)
- `CommandController`: constructor accepts `shared_ptr<IntentClassifier>` base class
- All public methods in `HomeAssistantClient` and `DatabaseService` marked `virtual`
  (enables GoogleMock subclassing in tests)

## [1.0.0] - 2026-02-21

### Added
- Initial release — Tier 1 deterministic classifier (16 regex patterns)
- Home Assistant REST API client
- PostgreSQL logging (voice_commands + intent_results)
- Health check endpoint (`GET /health`)
- Command endpoint (`POST /api/v1/command`)
- YAML config (`/etc/hms-assist/config.yaml`)
- Drogon HTTP framework
- Supports: light on/off/toggle, thermostat set/warmer/cooler, lock/unlock,
  media pause/next/prev/play, scene activate/mode
