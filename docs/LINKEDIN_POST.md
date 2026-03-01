# LinkedIn Post

---

**I kept losing commands every time I said more than one thing to my voice assistant. So I built something to fix it.**

The frustrating pattern: "Turn off the sala lights, restart the coffee maker, and play jazz in the living room."

One of those three things happens. Maybe two if I'm lucky. The rest just... disappear.

I tried Home Assistant's built-in Assist with an LLM backend. It's smarter, but it sends your entire entity list — 1,100+ devices in my case — to the model every single time. Too slow for a voice assistant. Too much resource pressure for daily use.

So I built **HMS-Assist**: a 3-tier intent classification API written in C++ that handles compound commands fully locally.

---

**How it works:**

**Tier 1 — Deterministic (regex, <5ms):** Common patterns like "turn on/off", "lock/unlock", "pause music" resolve instantly. No ML, no network call.

**Tier 2 — Vector search (~400ms):** Uses nomic-embed-text embeddings + pgvector to match natural language to the right entity out of 1,100+. "Brighten the sala" finds `light.sala_1` at 0.65 cosine similarity. The entity list never touches an LLM.

**Tier 3 — LLM as a splitter (7-15s):** For compound or ambiguous commands. Here's the key insight: **the LLM never sees your entity list.** It only splits text — "turn off X and turn on Y" → `["turn off X", "turn on Y"]`. Each fragment routes back through Tier 2. The LLM does what it's actually good at: understanding language structure, not searching through entity lists.

---

**The result:**

- Compound commands with parallel + sequential execution ("turn off coffee, then turn it back on, and turn on sala 1")
- Wave execution: independent commands run in parallel; dependent ones wait with a 500ms debounce
- Non-HA requests (jokes, questions) separated automatically into `non_ha` field
- Fully local — Ollama on an RTX 3050, pgvector on PostgreSQL, C++ binary at 6MB

The deterministic tiers handle 95% of commands under 500ms. The LLM is a fallback, not a dependency. Runs comfortably on a Raspberry Pi 5.

Next up: Docker image for one-command deployment, and connecting this as the brain behind a voice satellite (ReSpeaker Lite / Wyoming) — so the satellite handles audio I/O and HMS-Assist handles everything else.

The code is open source. Happy to share if you're building something similar.

#HomeAutomation #LocalAI #VoiceAssistant #C++ #OpenSource #HomeAssistant #RaspberryPi
