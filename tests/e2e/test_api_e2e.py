"""
E2E tests for hms-assist-api (HTTP layer).

Requires the API binary to be running on port 8894:
    cd projects/hms-assist/build
    HMS_ASSIST_CONFIG=/etc/hms-assist/config.yaml ./hms_assist &

Run:
    cd projects/hms-assist
    HMS_ASSIST_CONFIG=/etc/hms-assist/config.yaml \\
        tools/venv/bin/python -m pytest tests/e2e/test_api_e2e.py -v -s

Tests that send real voice commands (and exercise full tier pipeline) are
grouped under TestCommandEndpointLive and are skipped when the API is not up.
"""

import json
import os
import socket
import time
import unittest

import requests


API_HOST = os.environ.get("HMS_ASSIST_HOST", "localhost")
API_PORT = int(os.environ.get("HMS_ASSIST_PORT", "8894"))
BASE_URL  = f"http://{API_HOST}:{API_PORT}"


def _api_reachable() -> bool:
    try:
        s = socket.create_connection((API_HOST, API_PORT), timeout=2)
        s.close()
        return True
    except OSError:
        return False


def skip_if_api_down(fn):
    import functools
    @functools.wraps(fn)
    def wrapper(self, *args, **kwargs):
        if not _api_reachable():
            raise unittest.SkipTest(
                f"hms-assist API not reachable at {BASE_URL} — "
                "start it with: HMS_ASSIST_CONFIG=/etc/hms-assist/config.yaml ./hms_assist"
            )
        return fn(self, *args, **kwargs)
    return wrapper


# ─── Health endpoint ─────────────────────────────────────────────────────────

class TestHealthEndpoint(unittest.TestCase):

    @skip_if_api_down
    def test_health_returns_200(self):
        resp = requests.get(f"{BASE_URL}/health", timeout=5)
        self.assertEqual(resp.status_code, 200)

    @skip_if_api_down
    def test_health_response_structure(self):
        data = requests.get(f"{BASE_URL}/health", timeout=5).json()
        self.assertEqual(data["status"], "healthy")
        self.assertEqual(data["service"], "hms-assist")
        self.assertIn("version", data)
        self.assertIn("components", data)
        self.assertIn("statistics", data)

    @skip_if_api_down
    def test_health_components_present(self):
        data = requests.get(f"{BASE_URL}/health", timeout=5).json()
        components = data["components"]
        self.assertIn("database", components)
        self.assertIn("vector_db", components)
        self.assertIn("entity_count", components)

    @skip_if_api_down
    def test_health_database_connected(self):
        data = requests.get(f"{BASE_URL}/health", timeout=5).json()
        self.assertEqual(data["components"]["database"], "connected")

    @skip_if_api_down
    def test_health_entity_count_nonzero(self):
        data = requests.get(f"{BASE_URL}/health", timeout=5).json()
        count = data["components"]["entity_count"]
        self.assertGreater(count, 100,
                           f"Expected >100 entities in vector DB, got {count}")

    @skip_if_api_down
    def test_health_statistics_keys(self):
        data = requests.get(f"{BASE_URL}/health", timeout=5).json()
        stats = data["statistics"]
        self.assertIn("total_commands", stats)
        self.assertIn("successful_intents", stats)


# ─── Request validation ───────────────────────────────────────────────────────

class TestCommandValidation(unittest.TestCase):

    def _post(self, body, *, content_type="application/json"):
        return requests.post(
            f"{BASE_URL}/api/v1/command",
            data=json.dumps(body) if isinstance(body, dict) else body,
            headers={"Content-Type": content_type},
            timeout=10,
        )

    @skip_if_api_down
    def test_missing_text_field_returns_400(self):
        resp = self._post({"device_id": "test_device"})
        self.assertEqual(resp.status_code, 400)
        self.assertFalse(resp.json()["success"])

    @skip_if_api_down
    def test_missing_device_id_returns_400(self):
        resp = self._post({"text": "turn on the light"})
        self.assertEqual(resp.status_code, 400)
        self.assertFalse(resp.json()["success"])

    @skip_if_api_down
    def test_empty_text_returns_400(self):
        resp = self._post({"text": "", "device_id": "dev1"})
        self.assertEqual(resp.status_code, 400)
        self.assertFalse(resp.json()["success"])

    @skip_if_api_down
    def test_invalid_json_returns_400(self):
        resp = self._post("not-json-at-all")
        self.assertEqual(resp.status_code, 400)

    @skip_if_api_down
    def test_error_response_has_error_field(self):
        resp = self._post({"device_id": "dev1"})  # missing text
        body = resp.json()
        self.assertFalse(body["success"])
        self.assertIn("error", body)
        self.assertIn("text", body["error"].lower())


# ─── Response structure ───────────────────────────────────────────────────────

class TestCommandResponseStructure(unittest.TestCase):
    """
    Uses a known pattern that always hits Tier 1 (regex match + real entity).
    Entity 'patio' maps to light.patio which exists in this HA instance.
    """

    def _command(self, text: str, timeout: int = 60) -> tuple:
        if not _api_reachable():
            raise unittest.SkipTest(f"API not reachable at {BASE_URL}")
        resp = requests.post(
            f"{BASE_URL}/api/v1/command",
            json={"text": text, "device_id": "test_satellite"},
            timeout=timeout,
        )
        return resp.status_code, resp.json()

    @skip_if_api_down
    def test_response_has_all_required_fields(self):
        _, body = self._command("turn on the patio light")
        self.assertIn("success", body)
        self.assertIn("tier", body)
        self.assertIn("intent", body)
        self.assertIn("confidence", body)
        self.assertIn("response_text", body)
        self.assertIn("processing_time_ms", body)

    @skip_if_api_down
    def test_tier_field_is_valid(self):
        _, body = self._command("turn on the patio light")
        self.assertIn(body["tier"], {"tier1", "tier2", "tier3a", "tier3b"})

    @skip_if_api_down
    def test_processing_time_is_positive(self):
        _, body = self._command("turn on the patio light")
        self.assertGreater(body["processing_time_ms"], 0)

    @skip_if_api_down
    def test_confidence_in_range(self):
        _, body = self._command("turn on the patio light")
        # When tier1 succeeds, confidence is 0.95. When it falls through,
        # tier3 may return 0.0 on failure — both are valid in range.
        self.assertGreaterEqual(body["confidence"], 0.0)
        self.assertLessEqual(body["confidence"], 1.0)


# ─── Tier routing ─────────────────────────────────────────────────────────────

class TestTierRouting(unittest.TestCase):
    """
    Verify the tier pipeline routing with real HA + Ollama.
    Tier 3 calls may be slow (1-30s for LLM inference).

    Entity names used are confirmed to exist in this HA instance:
      - light.patio   (lights)
      - lock.entryway_lock
      - media_player.denon
    """

    def _command(self, text: str, timeout: int = 60) -> dict:
        if not _api_reachable():
            raise unittest.SkipTest(f"API not reachable at {BASE_URL}")
        resp = requests.post(
            f"{BASE_URL}/api/v1/command",
            json={"text": text, "device_id": "e2e_test"},
            timeout=timeout,
        )
        return resp.json()

    @skip_if_api_down
    def test_deterministic_light_pattern_hits_tier1(self):
        """'turn on the patio light' matches regex and light.patio exists → tier1."""
        body = self._command("turn on the patio light")
        print(f"\n  [e2e] tier={body['tier']} intent={body['intent']} "
              f"conf={body['confidence']:.2f} resp='{body['response_text']}'")
        self.assertEqual(body["tier"], "tier1")
        self.assertEqual(body["intent"], "light_control")
        self.assertTrue(body["success"])

    @skip_if_api_down
    def test_deterministic_lock_pattern_tier1(self):
        """'lock the entryway door' matches regex and lock.entryway_lock exists → tier1."""
        body = self._command("lock the entryway door")
        print(f"\n  [e2e] lock: tier={body['tier']} resp='{body['response_text']}'")
        self.assertEqual(body["tier"], "tier1")
        self.assertEqual(body["intent"], "lock_control")

    @skip_if_api_down
    def test_deterministic_media_pause_tier1(self):
        """'pause the music' matches regex + media_player.denon exists → tier1."""
        body = self._command("pause the music")
        print(f"\n  [e2e] media: tier={body['tier']} resp='{body['response_text']}'")
        self.assertEqual(body["tier"], "tier1")
        self.assertEqual(body["intent"], "media_control")

    @skip_if_api_down
    def test_non_deterministic_command_bypasses_tier1(self):
        """A phrase with no regex match must not be resolved by tier1."""
        body = self._command("make the living room cozy", timeout=120)
        print(f"\n  [e2e] cozy: tier={body['tier']} intent={body.get('intent','')}")
        # No regex pattern matches this → must fall through
        self.assertNotEqual(body["tier"], "tier1")

    @skip_if_api_down
    def test_paraphrase_handled_by_tier2_or_tier3(self):
        """Semantic paraphrase should be handled by tier2 (vector) or tier3 (LLM).
        'sala' entities (sala 1/2/3) are indexed — 'brighten the sala' should
        resolve via tier2 embedding search with confidence ≥ 0.58.
        """
        body = self._command("can you brighten up the sala", timeout=120)
        print(f"\n  [e2e] brighten sala: tier={body['tier']} "
              f"conf={body['confidence']:.2f} entity={body.get('entities', {}).get('entity_id', '?')}")
        self.assertIn(body["tier"], {"tier2", "tier3a", "tier3b"})
        # Should hit tier2 with sala entity indexed (sala 1/2/3 score ~0.64)
        if body["tier"] == "tier2":
            self.assertGreater(body["confidence"], 0.55)
            self.assertIn("sala", body["entities"].get("entity_id", "").lower())


# ─── Tier 3 (LLM) routing ─────────────────────────────────────────────────────

class TestTier3Routing(unittest.TestCase):
    """
    Exercises the LLM split path (tier3a fast model) and smart model escalation
    (tier3b). These are slow — allow up to 120s per test.

    Compound commands trigger the LLM splitter; each sub-command then routes
    through tier1 → tier2. The smart model is exercised via a command with
    enough entities to stress the 8b model.
    """

    def _command(self, text: str, timeout: int = 120) -> dict:
        if not _api_reachable():
            raise unittest.SkipTest(f"API not reachable at {BASE_URL}")
        resp = requests.post(
            f"{BASE_URL}/api/v1/command",
            json={"text": text, "device_id": "e2e_tier3"},
            timeout=timeout,
        )
        return resp.json()

    @skip_if_api_down
    def test_compound_command_routes_to_tier3a_split(self):
        """Two independent commands on different entities → tier3a split, both succeed."""
        body = self._command("turn off sala 1 and turn on sala 2")
        print(f"\n  [e2e] compound: tier={body['tier']} intent={body['intent']} "
              f"time={body['processing_time_ms']}ms")
        self.assertEqual(body["tier"], "tier3a")
        self.assertEqual(body["intent"], "multi_command")
        self.assertTrue(body["success"])
        cmds = body["entities"]["commands"]
        self.assertEqual(len(cmds), 2)
        self.assertTrue(all(c["success"] for c in cmds))

    @skip_if_api_down
    def test_compound_parallel_flags_correct(self):
        """Independent entities must have wait_for_previous:false."""
        body = self._command("turn off sala 1 and turn off sala 2")
        cmds = body["entities"]["commands"]
        print(f"\n  [e2e] parallel flags: "
              f"{[(c['text'], c.get('wait_for_previous')) for c in cmds]}")
        self.assertTrue(all(not c.get("wait_for_previous", True) for c in cmds),
                        "Independent commands should all have wait_for_previous:false")

    @skip_if_api_down
    def test_compound_sequential_restart_flags_correct(self):
        """Same-device restart: turn_off → wait_for_previous:false, turn_on → true."""
        body = self._command("turn off coffee and then turn it back on")
        cmds = body["entities"]["commands"]
        print(f"\n  [e2e] restart flags: "
              f"{[(c['text'], c.get('wait_for_previous')) for c in cmds]}")
        self.assertEqual(len(cmds), 2)
        self.assertFalse(cmds[0].get("wait_for_previous", True),
                         "First command must not wait")
        self.assertTrue(cmds[1].get("wait_for_previous", False),
                        "turn_on after turn_off on same device must have wait_for_previous:true")

    @skip_if_api_down
    def test_compound_with_joke_separates_non_ha(self):
        """Joke should land in non_ha; sala must execute successfully regardless.

        Note: the 8b model occasionally routes jokes containing entity words
        (e.g. 'joke about lights') into sub_commands instead of non_ha. The
        HA execution (sala 1) must always succeed either way.
        """
        body = self._command("turn off sala 1 and tell me a joke about lights")
        non_ha = body["entities"].get("non_ha", "")
        cmds = body["entities"]["commands"]
        print(f"\n  [e2e] joke: cmds={len(cmds)} non_ha='{non_ha[:60]}'")

        # At least one command must be the sala entity
        sala_cmds = [c for c in cmds
                     if "sala" in c.get("entities", {}).get("entity_id", "").lower()]
        self.assertGreater(len(sala_cmds), 0, "sala 1 command must be present")
        self.assertTrue(sala_cmds[0]["success"], "sala 1 must execute successfully")

        # Best case: joke in non_ha. Acceptable fallback: joke slipped into sub_commands.
        has_non_ha_joke = len(non_ha) > 5
        has_extra_cmd   = len(cmds) > len(sala_cmds)
        self.assertTrue(has_non_ha_joke or has_extra_cmd,
                        "Joke must appear in non_ha or as an extra sub_command")

    @skip_if_api_down
    def test_tier3b_smart_model_escalation(self):
        """4-entity command: should succeed via tier3a or escalate to tier3b."""
        body = self._command(
            "turn off sala 1 and turn off sala 2 and turn off sala 3 and lock the entryway",
            timeout=180,
        )
        print(f"\n  [e2e] 4-entity: tier={body['tier']} success={body['success']} "
              f"time={body['processing_time_ms']}ms")
        self.assertIn(body["tier"], {"tier3a", "tier3b"})
        self.assertEqual(body["intent"], "multi_command")
        self.assertTrue(body["success"])
        cmds = body["entities"]["commands"]
        self.assertEqual(len(cmds), 4)
        self.assertTrue(all(c["success"] for c in cmds))


# ─── Admin reindex ─────────────────────────────────────────────────────────────

class TestAdminReindex(unittest.TestCase):

    @skip_if_api_down
    def test_reindex_returns_success(self):
        resp = requests.post(f"{BASE_URL}/admin/reindex", timeout=10)
        self.assertEqual(resp.status_code, 200)
        body = resp.json()
        self.assertIn("success", body)
        self.assertIn("message", body)

    @skip_if_api_down
    def test_reindex_response_mentions_log(self):
        body = requests.post(f"{BASE_URL}/admin/reindex", timeout=10).json()
        # Message should tell the user where to find logs
        self.assertIn("log", body["message"].lower())


# ─── DB persistence ───────────────────────────────────────────────────────────

class TestCommandPersistence(unittest.TestCase):
    """Verify commands are persisted to PostgreSQL."""

    @skip_if_api_down
    def test_total_commands_increases_after_request(self):
        before = requests.get(f"{BASE_URL}/health", timeout=5).json()["statistics"]["total_commands"]

        # Use a tier1 command (fast) so it doesn't time out
        requests.post(
            f"{BASE_URL}/api/v1/command",
            json={"text": "turn on the patio light", "device_id": "persist_test"},
            timeout=60,
        )

        after = requests.get(f"{BASE_URL}/health", timeout=5).json()["statistics"]["total_commands"]
        self.assertGreater(after, before,
                           "total_commands did not increase after sending a command")


if __name__ == "__main__":
    unittest.main()
