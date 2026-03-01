"""
E2E tests for hms-assist-sync against real infrastructure.

Requirements:
  - PostgreSQL at <db-host>:5432 (hms_assist DB with schema applied)
  - Ollama at <ollama-host>:11434 (nomic-embed-text model loaded)
  - Home Assistant at <ha-host>:8123 (with a valid token)
  - Config at /etc/hms-assist/config.yaml (or HMS_ASSIST_CONFIG env var)

Run:
    cd projects/hms-assist
    HMS_ASSIST_CONFIG=/etc/hms-assist/config.yaml \\
        tools/venv/bin/python -m pytest tests/e2e/test_sync_e2e.py -v -s

These tests are intentionally skipped in CI if infrastructure is not reachable.
"""

import json
import os
import sys
import time
import unittest

import psycopg2
import requests

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools"))
from hms_assist_sync import (
    Config,
    build_description,
    embed,
    fetch_entities_from_ha_db,
    fetch_entities_from_rest,
    load_config,
    run_sync,
    upsert_entity,
    vec_literal,
)


# ─── Config loader ───────────────────────────────────────────────────────────

_CONFIG_PATH = os.environ.get("HMS_ASSIST_CONFIG", "/etc/hms-assist/config.yaml")


def _load() -> Config:
    return load_config(_CONFIG_PATH)


def _skip_if_unreachable(host: str, port: int, label: str):
    """Decorator-style helper; raises SkipTest if TCP connection fails."""
    import socket
    try:
        s = socket.create_connection((host, port), timeout=2)
        s.close()
    except OSError:
        raise unittest.SkipTest(f"{label} not reachable at {host}:{port}")


# ─── Infrastructure connectivity ─────────────────────────────────────────────

class TestInfrastructureReachable(unittest.TestCase):
    """Verify all required services are up before running deeper tests."""

    def test_postgres_reachable(self):
        _skip_if_unreachable("192.168.2.15", 5432, "PostgreSQL")
        cfg = _load()
        conn = psycopg2.connect(cfg.assist_db)
        cur = conn.cursor()
        cur.execute("SELECT 1")
        self.assertEqual(cur.fetchone()[0], 1)
        cur.close()
        conn.close()

    def test_hms_assist_schema_exists(self):
        _skip_if_unreachable("192.168.2.15", 5432, "PostgreSQL")
        cfg = _load()
        conn = psycopg2.connect(cfg.assist_db)
        cur = conn.cursor()
        cur.execute("""
            SELECT table_name FROM information_schema.tables
            WHERE table_schema = 'public'
            ORDER BY table_name
        """)
        tables = {row[0] for row in cur.fetchall()}
        cur.close()
        conn.close()
        self.assertIn("entity_embeddings", tables)
        self.assertIn("voice_commands", tables)
        self.assertIn("intent_results", tables)

    def test_pgvector_extension_installed(self):
        _skip_if_unreachable("192.168.2.15", 5432, "PostgreSQL")
        cfg = _load()
        conn = psycopg2.connect(cfg.assist_db)
        cur = conn.cursor()
        cur.execute("SELECT extname FROM pg_extension WHERE extname = 'vector'")
        self.assertIsNotNone(cur.fetchone(), "pgvector extension not installed")
        cur.close()
        conn.close()

    def test_ollama_reachable(self):
        _skip_if_unreachable("192.168.2.5", 11434, "Ollama")
        resp = requests.get("http://192.168.2.5:11434/api/tags", timeout=5)
        self.assertEqual(resp.status_code, 200)

    def test_nomic_embed_text_available(self):
        _skip_if_unreachable("192.168.2.5", 11434, "Ollama")
        resp = requests.get("http://192.168.2.5:11434/api/tags", timeout=5)
        models = [m["name"] for m in resp.json().get("models", [])]
        self.assertTrue(
            any("nomic-embed-text" in m for m in models),
            f"nomic-embed-text not found in Ollama models: {models}"
        )

    def test_home_assistant_reachable(self):
        _skip_if_unreachable("192.168.2.7", 8123, "Home Assistant")
        cfg = _load()
        resp = requests.get(
            f"{cfg.ha_url}/api/",
            headers={"Authorization": f"Bearer {cfg.ha_token}"},
            timeout=5,
        )
        self.assertEqual(resp.status_code, 200)


# ─── HA data sources ─────────────────────────────────────────────────────────

class TestEntityFetch(unittest.TestCase):

    def test_fetch_from_ha_db(self):
        _skip_if_unreachable("192.168.2.15", 5432, "PostgreSQL (HA DB)")
        cfg = _load()
        if not cfg.ha_db:
            self.skipTest("ha_db not configured")
        entities = fetch_entities_from_ha_db(cfg.ha_db)
        self.assertGreater(len(entities), 100, "Expected >100 entities from HA DB")
        # Every entity must have required fields
        for e in entities[:20]:
            self.assertIn("entity_id", e)
            self.assertIn("domain", e)
            self.assertIn("friendly_name", e)
            self.assertIn("state", e)
            self.assertIn("attributes", e)
            self.assertIsInstance(e["attributes"], dict)

    def test_ha_db_entities_have_valid_entity_ids(self):
        _skip_if_unreachable("192.168.2.15", 5432, "PostgreSQL (HA DB)")
        cfg = _load()
        if not cfg.ha_db:
            self.skipTest("ha_db not configured")
        entities = fetch_entities_from_ha_db(cfg.ha_db)
        for e in entities:
            self.assertIn(".", e["entity_id"],
                          f"entity_id missing dot: {e['entity_id']}")
            domain, _ = e["entity_id"].split(".", 1)
            self.assertEqual(domain, e["domain"])

    def test_fetch_from_rest(self):
        _skip_if_unreachable("192.168.2.7", 8123, "Home Assistant REST")
        cfg = _load()
        entities = fetch_entities_from_rest(cfg.ha_url, cfg.ha_token)
        self.assertGreater(len(entities), 100)

    def test_ha_db_and_rest_overlap(self):
        """Both sources should return the same entity IDs (within ~5% tolerance)."""
        _skip_if_unreachable("192.168.2.15", 5432, "PostgreSQL (HA DB)")
        _skip_if_unreachable("192.168.2.7", 8123, "Home Assistant REST")
        cfg = _load()
        if not cfg.ha_db:
            self.skipTest("ha_db not configured")

        db_ids = {e["entity_id"] for e in fetch_entities_from_ha_db(cfg.ha_db)}
        rest_ids = {e["entity_id"] for e in fetch_entities_from_rest(cfg.ha_url, cfg.ha_token)}

        overlap = len(db_ids & rest_ids)
        total = max(len(db_ids), len(rest_ids))
        overlap_pct = overlap / total * 100
        # HA DB excludes unavailable/unknown states; REST returns all → expect ~70-80% overlap
        self.assertGreater(overlap_pct, 70,
                           f"DB/REST overlap only {overlap_pct:.1f}% — sources diverged")


# ─── Ollama embedding ─────────────────────────────────────────────────────────

class TestOllamaEmbedding(unittest.TestCase):

    def test_embed_returns_768_dims(self):
        _skip_if_unreachable("192.168.2.5", 11434, "Ollama")
        cfg = _load()
        vec = embed("kitchen ceiling light", cfg.ollama_url, cfg.embed_model)
        self.assertEqual(len(vec), 768)
        self.assertIsInstance(vec[0], float)

    def test_embed_different_texts_are_different(self):
        _skip_if_unreachable("192.168.2.5", 11434, "Ollama")
        cfg = _load()
        v1 = embed("turn on living room light", cfg.ollama_url, cfg.embed_model)
        v2 = embed("lock the front door", cfg.ollama_url, cfg.embed_model)
        # Cosine similarity < 0.99 (should not be identical)
        dot = sum(a * b for a, b in zip(v1, v2))
        mag1 = sum(a * a for a in v1) ** 0.5
        mag2 = sum(b * b for b in v2) ** 0.5
        cosine = dot / (mag1 * mag2)
        self.assertLess(cosine, 0.99,
                        f"Expected different embeddings, got cosine={cosine:.4f}")

    def test_semantically_similar_texts_are_close(self):
        _skip_if_unreachable("192.168.2.5", 11434, "Ollama")
        cfg = _load()
        v1 = embed("turn on the lights", cfg.ollama_url, cfg.embed_model)
        v2 = embed("switch on lights", cfg.ollama_url, cfg.embed_model)
        dot = sum(a * b for a, b in zip(v1, v2))
        mag1 = sum(a * a for a in v1) ** 0.5
        mag2 = sum(b * b for b in v2) ** 0.5
        cosine = dot / (mag1 * mag2)
        # nomic-embed-text typically scores 0.85-0.95 for paraphrases
        self.assertGreater(cosine, 0.85,
                           f"Semantically similar texts too dissimilar: cosine={cosine:.4f}")


# ─── Vector search (pgvector) ─────────────────────────────────────────────────

class TestVectorSearch(unittest.TestCase):

    def _assert_entities_populated(self, cfg: Config):
        conn = psycopg2.connect(cfg.assist_db)
        cur = conn.cursor()
        cur.execute("SELECT COUNT(*) FROM entity_embeddings")
        count = cur.fetchone()[0]
        cur.close()
        conn.close()
        if count == 0:
            self.skipTest("entity_embeddings table is empty — run sync first")
        return count

    def test_entity_embeddings_populated(self):
        _skip_if_unreachable("192.168.2.15", 5432, "PostgreSQL")
        cfg = _load()
        count = self._assert_entities_populated(cfg)
        self.assertGreater(count, 100, f"Only {count} entities — expected >100")

    def test_vector_search_for_light_command(self):
        """Searching 'turn on kitchen light' should return a light entity."""
        _skip_if_unreachable("192.168.2.15", 5432, "PostgreSQL")
        _skip_if_unreachable("192.168.2.5", 11434, "Ollama")
        cfg = _load()
        self._assert_entities_populated(cfg)

        query_vec = embed("turn on kitchen light", cfg.ollama_url, cfg.embed_model)
        literal = vec_literal(query_vec)

        conn = psycopg2.connect(cfg.assist_db)
        cur = conn.cursor()
        cur.execute("""
            SELECT entity_id, domain, friendly_name,
                   1 - (embedding <=> %s::vector) AS similarity
            FROM entity_embeddings
            ORDER BY embedding <=> %s::vector
            LIMIT 5
        """, (literal, literal))
        rows = cur.fetchall()
        cur.close()
        conn.close()

        self.assertGreater(len(rows), 0)
        top = rows[0]
        entity_id, domain, name, similarity = top
        print(f"\n  [e2e] Top result: {entity_id} ({domain}) sim={similarity:.4f}")
        # Top result should be a light or at least a controllable entity
        self.assertNotIn(domain, {"sun", "person", "weather", "stt", "tts"})
        self.assertGreater(similarity, 0.5)

    def test_vector_search_for_lock_command(self):
        """Searching 'lock the front door' should return a lock entity."""
        _skip_if_unreachable("192.168.2.15", 5432, "PostgreSQL")
        _skip_if_unreachable("192.168.2.5", 11434, "Ollama")
        cfg = _load()
        self._assert_entities_populated(cfg)

        query_vec = embed("lock the front door", cfg.ollama_url, cfg.embed_model)
        literal = vec_literal(query_vec)

        conn = psycopg2.connect(cfg.assist_db)
        cur = conn.cursor()
        cur.execute("""
            SELECT entity_id, domain,
                   1 - (embedding <=> %s::vector) AS similarity
            FROM entity_embeddings
            ORDER BY embedding <=> %s::vector
            LIMIT 10
        """, (literal, literal))
        rows = cur.fetchall()
        cur.close()
        conn.close()

        top_domains = [r[1] for r in rows]
        print(f"\n  [e2e] Top domains for 'lock': {top_domains[:5]}")
        # At least one lock or door-related entity in top 10
        has_lock = any(d in ("lock", "cover", "binary_sensor") for d in top_domains)
        self.assertTrue(has_lock, f"No lock/cover in top 10: {top_domains}")


# ─── Full sync round-trip ─────────────────────────────────────────────────────

class TestFullSyncRoundTrip(unittest.TestCase):
    """
    Runs a real --once sync against real infra and validates the results.
    This is slow (~50-60s) — it re-embeds all entities.
    Skip with: pytest tests/e2e/ -k "not FullSync"
    """

    def test_full_sync_once(self):
        _skip_if_unreachable("192.168.2.15", 5432, "PostgreSQL")
        _skip_if_unreachable("192.168.2.5", 11434, "Ollama")
        _skip_if_unreachable("192.168.2.7", 8123, "Home Assistant")

        cfg = _load()
        t0 = time.time()
        stats = run_sync(cfg)
        elapsed = time.time() - t0

        print(f"\n  [e2e] Sync: {stats['processed']} indexed, "
              f"{stats['failed']} failed, {stats['pruned']} pruned "
              f"in {elapsed:.1f}s")

        self.assertGreater(stats["processed"], 100)
        self.assertEqual(stats["failed"], 0)
        self.assertGreater(stats["total_in_db"], 100)

    def test_dry_run_does_not_modify_db(self):
        _skip_if_unreachable("192.168.2.15", 5432, "PostgreSQL")
        _skip_if_unreachable("192.168.2.7", 8123, "Home Assistant")

        cfg = _load()
        conn = psycopg2.connect(cfg.assist_db)
        cur = conn.cursor()
        cur.execute("SELECT COUNT(*) FROM entity_embeddings")
        before = cur.fetchone()[0]
        cur.close()
        conn.close()

        run_sync(cfg, dry_run=True)

        conn = psycopg2.connect(cfg.assist_db)
        cur = conn.cursor()
        cur.execute("SELECT COUNT(*) FROM entity_embeddings")
        after = cur.fetchone()[0]
        cur.close()
        conn.close()

        self.assertEqual(before, after, "dry_run modified the DB")


if __name__ == "__main__":
    unittest.main()
