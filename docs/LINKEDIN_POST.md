# LinkedIn Post

---

**I built a voice assistant API that does what Alexa still can't do in 2026: run multiple commands in one sentence, fully locally, on a Raspberry Pi.**

"Turn off the sala lights, restart the coffee maker, and play jazz in the living room."

Alexa hears that and picks one. Maybe two if you're lucky. Home Assistant's built-in Assist sends your entire entity list — 1,100+ devices — to an LLM every single time, making it too slow and too expensive for real use.

So I built something different.

**HMS-Assist** is a 3-tier intent classification API written in C++:

**Tier 1 — Deterministic (regex, <5ms):** Common patterns like "turn on/off", "lock/unlock", "pause music" resolve instantly. No ML, no network call. Just fast.

**Tier 2 — Vector search (~400ms):** Uses nomic-embed-text embeddings + pgvector to match natural language to the right entity out of 1,100+. "Brighten the sala" finds `light.sala_1` at 0.65 cosine similarity. No LLM needed.

**Tier 3 — LLM splitter + executor (7-15s):** For compound or ambiguous commands. Here's the key insight: **the LLM never sees your entity list.** It only splits text — "turn off X and turn on Y" → `["turn off X", "turn on Y"]`. Each fragment routes back through Tier 2. The LLM handles what it's actually good at: understanding language structure, not entity resolution.

The result:
- Compound commands with parallel + sequential execution ("turn off coffee, then turn it back on, and turn on sala 1")
- Sequential wave execution with 500ms debounce between dependent steps
- Non-HA requests (jokes, questions) separated automatically into `non_ha` field
- Fully local — Ollama on an RTX 3050, pgvector on PostgreSQL, C++ binary at 6MB

The architecture runs comfortably on a Raspberry Pi 5. The deterministic tiers handle 95% of commands under 500ms. The LLM is a fallback, not a dependency.

Next: Docker image for one-command deployment, and integrating this as a fallback for voice satellites (ReSpeaker Lite / Wyoming) when local intent matching isn't enough.

The code is open source. Happy to share if you're building something similar.

#HomeAutomation #LocalAI #VoiceAssistant #C++ #OpenSource #HomeAssistant #RaspberryPi
