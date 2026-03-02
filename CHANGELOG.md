# Changelog

All notable changes to hms-assist-api will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

---

## [2.7.0] - 2026-03-02

### Added
- **Tier1 catch-all device patterns**: 5 new regex patterns (`turn on/off <name>`,
  `switch on/off <name>`, `toggle <name>`) that match commands without a "light" suffix.
  New `processDeviceControl` handler searches `light` domain first, then `switch`, enabling
  commands like "turn off sala 1" and "turn off dinner" to resolve in <5ms via Tier1.
- **E2E integration tests** (`TestMixedIntegration`, 6 tests): real LLM pipeline tests
  mixing device actions, sensor queries, and non-HA content (jokes). Full Tier1/Tier2/Tier3
  coverage with safe entities only (sala 1/2/3, dinner, coffee, sensors).
- **E2E device control tests** (`TestDeviceControlTier1`, 6 tests): verify Tier1 catch-all
  resolves sala 1/2/3, dinner, coffee correctly.
- **E2E sensor rejection tests** (`TestSensorRejectionTier2`, 3 tests): verify action words
  never return `sensor_query`.

### Fixed
- **Tier2 sensor false positive**: `inferAction()` now rejects sensor domain when the
  command contains action words (on/off/toggle/enable/disable/restart/reboot). Previously
  "turn off sala 1" could match a sensor entity at 0.71 similarity and return a sensor
  reading instead of controlling the device.
- **`dry_run` not parsed from HTTP request**: `CommandController::handleCommand()` was not
  reading `dry_run` from the request JSON body, so all API calls executed live even when
  `dry_run: true` was sent. Now correctly parsed via `bodyJson->get("dry_run", false)`.
- **Tier1 pattern loop**: specific patterns (light, climate, lock) now break on match
  regardless of handler success, preventing fallthrough to catch-all device patterns when
  the entity simply doesn't exist.

### Changed
- **`VectorSearchService::entityCount()`** is now `virtual` — enables mocking in unit tests.
- **`VectorSearchService::toVectorLiteral()`** moved from `private` to `public static`.

### Tests Added
- `test_vector_search_service.cpp` (8 tests): `toVectorLiteral` formatting, `entityCount()`.
- `test_database_service.cpp` (8 tests): not-connected guards, `disconnect()` idempotency.
- `test_deterministic_classifier.cpp` (+6 tests): device catch-all patterns.
- `test_embedding_classifier.cpp` (+2 tests): sensor rejection on action words.
- `test_api_e2e.py`: rewritten with safe entities, 26 total e2e tests.

## [2.6.0] - 2026-03-01

### Added
- **`dry_run` mode** (`VoiceCommand::dry_run`): classifiers resolve `entity_id` but skip all
  HA service calls and state fetches. Used internally by the compound dedup phase to probe
  which entity a Tier3 sub-command resolves to without executing it.
- **`regexSplitCompound()`** static helper in `CommandController`: splits command text on hard
  dividers (`and`, `then`, `followed by`, `after that`) via regex; fuzzy fallback detects 2+
  action-verb phrases (turn on/off, lock, unlock, pause, toggle, etc.) and splits before the
  second occurrence. Returns a single-element vector when no compound structure found.
- **TTS spoken response**: `POST /api/v1/command` now accepts optional
  `"media_player_entity_id"` field. When present and the pipeline produces a non-empty
  `response_text`, the API calls `ha.callService("tts","speak","<tts_entity>",{message,
  media_player_entity_id})` so the satellite device hears the response. TTS entity configured
  via `service.tts_entity` in config (default `"tts.piper"`).
- **`service.tts_entity`** config field (`ConfigManager::ServiceConfig::tts_entity`).

### Changed
- **Compound command architecture rewrite** (parallel Tier2 + dedup):
  - Tier1 miss → detect compound with `regexSplitCompound()`
  - If compound: launch Tier2 on each regex-split part in parallel AND Tier3 `split()` on
    the full command in parallel (Phase 1, ~3s wall time dominated by Tier3)
  - Collect Tier2 results → build `executedEntityIds` set
  - Dry-run resolve each Tier3 sub-command via Tier2 in parallel (Phase 2)
  - Any Tier3 sub-command whose resolved `entity_id` is already in `executedEntityIds` is
    **skipped deterministically** (no LLM asked to filter, no hallucination risk)
  - Execute remaining sub-commands via `executeSplit()` (Phase 3)
  - Tier label: `"tier2"` if all handled by Tier2 or all deduped; `"tier2+tier3"` if some
    remainder executed; `"tier3"` if Tier2 was empty and Tier3 handled everything
- **`CommandController` constructor** now accepts `shared_ptr<HomeAssistantClient>` and
  `ttsEntity` string (needed for TTS response delivery).
- **`DeterministicClassifier`** all `process*` methods: HA calls gated on `!command.dry_run`.
  Entity resolution still runs in dry_run mode; `success=true` returned without HA execution.
- **`EmbeddingClassifier`**: HA `callService` + `sleep` + `getEntityState` block gated on
  `!command.dry_run`. Same for the restart path. `response_text` is empty in dry_run mode.
- **Model switch**: `fast_model` changed from `qwen2.5:3b` → `llama3.2:3b` in config.
  `qwen2.5:3b` deleted from Ollama server (hallucinated wrong entities when given context hints).

### Fixed
- **DB schema**: `intent_results.tier` column `VARCHAR(10)` → `VARCHAR(20)`, CHECK constraint
  updated to allow `'tier2+tier3'` (was rejected as 11 chars, causing DB log errors).
  Existing `tier3a`/`tier3b` rows migrated to `tier3`.

## [2.5.0] - 2026-03-01

### Fixed
- **Compound command Tier2+Tier3 partial execution bug**: when Tier2 matched the first
  sub-command of a compound ("turn off sala 1 and turn on coffee"), it returned immediately
  leaving the second part unexecuted. Tier2 now passes `already_executed` context to Tier3
  split — the LLM excludes the already-handled sub-command and returns only the remainder.
  Both parts now execute correctly.

### Changed
- **`CommandController::executeSplit()` extracted as private helper**: eliminates the
  duplicated wave-execution code that existed in both the Tier2+Tier3 and Tier3-only paths.
  Single implementation handles `wait_for_previous` wave logic, parallel `std::async`
  dispatch, response concatenation, and JSON entity building.
- **Tier3 split is now sequential after Tier2** (not parallel): previously Tier3 was
  launched in parallel via `std::async` before Tier2 finished, so it could not receive
  `already_executed` context and `command` was captured by reference (dangling-ref risk).
  The parallel pre-launch is removed; Tier3 is called only after Tier2 confirms a hit.

## [2.4.0] - 2026-03-01

### Changed
- **Tier3 redesign — pure text parser, zero entity context**: `LLMClassifier::classify()`
  and all entity-related infrastructure removed (`preFilterEntities`, `buildSystemPrompt`,
  `parsePlan`, `executePlan`, `entityCacheJson`, `refreshEntityCache`, `LLMCommand`,
  `LLMPlan`). Tier3 is now exclusively a text splitter — no entity IDs are ever passed to
  the LLM. Entity resolution always happens in Tier1/Tier2 after the split.
- **Unified pipeline**: `isCompound` gate removed from `CommandController::runPipeline()`.
  Tier1 fail → Tier2 fail → Tier3 `split()` for ALL commands (single or compound). Each
  sub-command routes back through `runSinglePipeline()` (tier1 → tier2).
- **Non-HA escalation**: fast model (`llama3.1:8b`) answers jokes/questions directly. If
  `confidence < escalation_threshold` or `escalate:true` is set, re-calls smart model
  (`gpt-oss:120b-cloud`) for current events, deep reasoning, or complex answers.
- **Ollama `num_ctx` pinned to 1024**: prevents KV cache reallocation on context switches
  between `split()` calls (was using default oversized context, spilling to system RAM on
  RTX 3050 with only 584 MB VRAM free after model weights). Warm call latency: ~8s → ~2s.
- **`num_predict: 200`, `temperature: 0`** added to all `chatJson()` calls: caps generation,
  removes sampling overhead, deterministic JSON output.

### Fixed
- **Entity cache added to `HomeAssistantClient::getAllEntities()`**: 5-minute TTL with
  mutex. Was fetching all 1185 HA entities on every request (including Tier1 hits).
- **Plural "lights" regex**: all light patterns changed from `\s+light` to `\s+lights?\b`.
  Previously "turn off the lights" captured `"the"` as location → `findEntities("the")`
  → 0 results → fell through to compound/LLM path unnecessarily.

### Removed
- `LLMClassifier` dependencies on `HomeAssistantClient`, `VectorSearchService`, `embedModel`
  — Tier3 no longer touches HA or the vector DB directly.

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
