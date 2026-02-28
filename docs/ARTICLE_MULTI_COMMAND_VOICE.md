# Why Every Voice Assistant Gets Multi-Command Wrong — and How to Fix It

*A deep dive into HMS-Assist: a fully local, 3-tier voice command API built for Home Assistant*

---

## The Problem Nobody Has Solved

"Turn off the sala lights, restart the coffee maker, and play some jazz."

Say that to Alexa. She'll pick one — usually the last thing you said. Say it to Siri. She'll ask you to confirm each action individually. Say it to Home Assistant's built-in Assist with an LLM backend. You'll wait 30 seconds while it processes your 1,100-entity context window, and it'll probably still get one wrong.

Multi-command voice execution is a solved problem in demos. In production, with real devices, real latency, and real edge cases, nobody has actually shipped it well. Here's why — and how a 3-tier architecture changes the game.

---

## Why Alexa Can't Do It

Alexa's intent model is fundamentally single-intent. Each utterance maps to one skill, one intent, one set of slots. Amazon has bolted on "routines" as a workaround — you manually define that "good morning" means turn on lights + start coffee + play news. But routines are static. You configure them in advance. They don't generalize.

"Turn off the sala and restart the coffee" isn't a routine you've set up. Alexa hears it, picks the most confident intent match, executes one command, and calls it done. The rest of your sentence disappears.

The deeper issue is architectural. Alexa was designed for a world where voice commands are simple, atomic, and map 1:1 to skills written by third parties. That model doesn't compose. Multiple intents in a single utterance require a fundamentally different design — one where the system understands that a sentence can contain multiple independent requests.

---

## Why Home Assistant's LLM Assist Is Too Slow

Home Assistant introduced LLM-based Assist as a way to handle natural language. The intent is right. The implementation has a scaling problem.

Every request sends your entire entity list to the LLM. If you have 1,100 entities — lights, switches, locks, media players, climate zones, sensors, cameras — all of that goes into the context window on every single command. You're asking a language model to needle-in-a-haystack search through thousands of tokens to find `light.sala_1` every time someone says "turn on the sala."

This creates three problems:

1. **Latency.** A 7B model processing 15,000 tokens of entity context takes 10-30 seconds. That's not a voice assistant, that's a slow web form.

2. **Cost and resource pressure.** Running a capable LLM locally requires significant GPU. Sending full context on every command burns memory bandwidth and thermal budget constantly.

3. **Accuracy degrades with scale.** LLMs lose precision in very long contexts. The more entities you have, the more likely the model confuses `switch.coffee` with `switch.coffee_led` or picks the wrong `sala`.

The fix isn't a better LLM. It's using the LLM only for what LLMs are actually good at.

---

## The 3-Tier Architecture

HMS-Assist solves this with a tiered pipeline where each layer handles what it's best suited for, and only escalates to the next when necessary.

### Tier 1: Deterministic Classification (<5ms)

The fastest tier uses regex pattern matching against a set of pre-compiled intents. "Turn on/off [entity]", "lock/unlock [entity]", "pause/play [media player]" — these patterns cover roughly 60-70% of real-world home automation commands. When a pattern matches and the entity exists in Home Assistant, the command executes immediately.

No embeddings. No network call. No GPU. Just fast.

This is the tier that handles your most common phrases — the ones you say every day. Tier 1 should feel instant, because it is.

### Tier 2: Semantic Vector Search (~400ms)

When Tier 1 doesn't match — paraphrases, translated phrases, entity names the regex doesn't know — Tier 2 kicks in.

Every Home Assistant entity is indexed as a 768-dimensional embedding using `nomic-embed-text` with asymmetric retrieval prefixes (`search_document:` at index time, `search_query:` at query time). The embeddings are stored in pgvector on PostgreSQL with an HNSW index for fast cosine similarity search.

When a command comes in, it gets embedded and compared against all 1,100+ entities in the vector database. The closest match above a similarity threshold (0.58) wins. "Brighten the sala" finds `light.sala_1` at 0.65 similarity. "Can you dim the room a bit?" finds the right light. "Apaga el café" — in Spanish — finds `switch.coffee`.

The critical difference from HA's LLM approach: the entity list is never sent to any language model. The similarity search is a database operation, not an inference operation. It takes 200-400ms regardless of how many entities you have.

### Tier 3: LLM as a Splitter, Not a Resolver (7-15s)

This is where the architecture gets interesting, and where the key insight lives.

When a compound command comes in — something with "and" in it — the system routes to Tier 3. But not in the way you'd expect.

**The LLM never sees your entity list.**

Instead, the LLM receives a tiny, focused prompt: "Split this compound voice command into individual smart home sub-commands and answer any non-HA parts." That's it. No entities. No context. Just text understanding.

Input: `"turn off coffee and then turn it back on and tell me a joke about switches"`

LLM output:
```json
{
  "sub_commands": [
    {"text": "turn off coffee", "wait_for_previous": false},
    {"text": "turn on coffee",  "wait_for_previous": true}
  ],
  "non_ha": "Why do switches get anxious? Because they're always feeling a little 'off'!"
}
```

Each sub-command then routes back through Tier 1 → Tier 2 independently. Entity resolution happens in the vector search layer where it belongs. The LLM contributes its actual strength: understanding that "turn it back on" refers to the same device as "turn off coffee", and that the second command must wait for the first.

The `wait_for_previous` flag drives wave-based parallel execution:
- Commands with `wait_for_previous: false` launch in parallel
- Commands with `wait_for_previous: true` wait for the current wave to complete, then start a new wave with a 500ms debounce (for physical switch settling time)

This means "turn off sala 1 and sala 2 and sala 3 and restart the coffee" executes as:
```
Wave 1 (parallel): turn_off sala_1, turn_off sala_2, turn_off sala_3, turn_off coffee
500ms pause
Wave 2: turn_on coffee
```

Three sala lights go off simultaneously. The coffee cycles cleanly.

---

## Why a Small LLM Can Do the Splitting

A 1-3B model is good enough for this role. It doesn't need to know what `light.sala_1` is. It doesn't need to understand your home layout. It needs to parse: "Does this sentence have multiple requests? Which ones depend on each other? Is there a non-device request mixed in?"

That's basic syntactic and semantic understanding — exactly what small, fast models excel at. The 8B `llama3.1` model running on a local RTX 3050 handles it in 6-8 seconds. A purpose-fine-tuned 1B model could probably do it in under 1 second.

By contrast, entity resolution — finding the right device out of 1,100+ options — is what vector search is built for. It's a database retrieval problem, not a language understanding problem. Using an LLM for it is like using a chainsaw to cut butter.

---

## Running on a Raspberry Pi

The system is designed to scale down, not just up.

The C++ binary is 6MB. PostgreSQL with pgvector runs comfortably on a Pi 5 with 8GB RAM. The embedding model (`nomic-embed-text`) is 274MB and runs on CPU in ~2-3 seconds per embed on a Pi 5 — acceptable for Tier 2 since entities are indexed in batch, not per-command.

For the LLM tier, a Pi can run a quantized 1-2B model in reasonable time. Or, more practically, the Pi can point Tier 3 at a remote Ollama server on a more capable machine while Tiers 1 and 2 run fully locally. The architecture supports this natively through configuration.

The tiered design means most commands never touch the LLM. A household that uses 50 common commands regularly will have all of them handled by Tier 1 or Tier 2 — under 500ms, no GPU required, no external API.

---

## The Voice Satellite Integration Problem

Here's the next frontier.

Modern voice satellite hardware — devices like the ReSpeaker Lite running ESP32-S3, or any Wyoming-compatible satellite — handles the hard perceptual problems locally: wake word detection, microphone array processing, noise cancellation. They stream audio to a transcription service (Whisper) and get back text.

But what happens to that text? Today, satellites send it directly to Home Assistant's Assist pipeline. If the command is simple, it works. If it's compound or ambiguous, it either fails or takes 30+ seconds waiting for an LLM.

The better architecture: the satellite sends the transcribed text to HMS-Assist first. HMS-Assist handles the classification, entity resolution, and HA execution. It returns a structured response including `response_text` for the TTS to speak back.

This creates a fallback chain:
1. Satellite handles wake word + STT locally
2. HMS-Assist handles intent classification + HA execution
3. For truly complex requests that even Tier 3 can't handle, escalate to a capable cloud or local model

The satellite never needs to know about your entity list. It never needs to implement intent classification. It's a pure audio I/O device that speaks a simple REST API.

This separation of concerns is what makes the system work at scale. The satellite is dumb by design. The intelligence lives in the API where it can be updated, tested, and monitored independently of the firmware.

---

## Next Steps: Production-Ready Docker Deployment

The current implementation runs as a native systemd service. The next step is a production Docker image that:

1. **Bundles everything**: C++ binary + PostgreSQL + pgvector + Python sync tool in a single compose stack
2. **Auto-syncs entities on startup**: Runs `hms_assist_sync.py` before starting the API, ensuring the vector DB is current
3. **Handles migrations**: Schema versioning so updates don't require manual DB intervention
4. **Exposes the full API** on a single configurable port

The target deployment experience:
```bash
docker compose up -d
# That's it. Point your satellite at port 8894.
```

On a Pi 5, the full stack (excluding Ollama, which runs on a separate machine) should fit comfortably in under 500MB RAM. On any x86 machine with a discrete GPU, Ollama can run alongside it.

The Docker image also solves the satellite integration story completely. A voice satellite operator installs the container, provides their HA URL and token, and gets a fully functional multi-command voice API with no further configuration.

---

## The Broader Point

The voice assistant problem isn't fundamentally hard. It's been made hard by architectures that try to solve everything with one model and one approach.

Regex is fast and deterministic. Use it for what it's good at.
Vector search is accurate and scalable. Use it for entity resolution.
LLMs understand language structure. Use them for that — not for searching through entity lists.

When each layer does its job, the system is fast enough to be a voice assistant, accurate enough to be reliable, and light enough to run on hardware that costs $80.

That's the design. Everything else is implementation detail.

---

*HMS-Assist is open source. The full implementation — including the C++ API, Python sync tool, e2e tests, and ESP32 satellite firmware — is available on GitHub.*
