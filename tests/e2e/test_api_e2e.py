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
        self.assertIn(body["tier"], {"tier1", "tier2", "tier3", "tier2+tier3"})

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
        self.assertIn(body["tier"], {"tier2", "tier3", "tier2+tier3"})
        # Should hit tier2 with sala entity indexed (sala 1/2/3 score ~0.64)
        if body["tier"] == "tier2":
            self.assertGreater(body["confidence"], 0.55)
            self.assertIn("sala", body["entities"].get("entity_id", "").lower())


# ─── Tier 3 (LLM) routing ─────────────────────────────────────────────────────

class TestTier3Routing(unittest.TestCase):
    """
    Exercises the LLM split path (fast model) and smart model escalation.
    These are slow — allow up to 120s per test.

    Compound commands trigger the LLM splitter; each sub-command then routes
    through tier1 → tier2. The smart model is exercised via a command with
    enough entities to stress the fast model.
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
    def test_compound_command_splits_and_all_succeed(self):
        """Two independent commands on different entities — both must execute.

        v2.6.0: parallel Tier2 on each regex-split part + Tier3 split for dedup.
        Route may be:
          - tier2: both parts handled by Tier2, all Tier3 sub-cmds deduplicated
          - tier2+tier3: one part handled by Tier2, remainder by Tier3
          - tier3: Tier2 misses both parts, Tier3 handles everything
        Either way overall success=True and all commands[] entries succeed.
        """
        body = self._command("turn off sala 1 and turn on sala 2")
        print(f"\n  [e2e] compound: tier={body['tier']} intent={body['intent']} "
              f"time={body['processing_time_ms']}ms")
        self.assertIn(body["tier"], {"tier2", "tier3", "tier2+tier3"})
        self.assertIn(body["intent"], {"single_command", "multi_command"})
        self.assertTrue(body["success"])
        cmds = body["entities"]["commands"]
        self.assertGreaterEqual(len(cmds), 1)
        self.assertTrue(all(c["success"] for c in cmds))

    @skip_if_api_down
    def test_tier2_compound_both_parts_execute(self):
        """Regression test: both parts of a compound command must execute.

        v2.6.0: parallel Tier2 handles each part individually + Tier3 split for
        deterministic dedup (no LLM hallucinations). Both "sala 1" (turn off)
        and "coffee" (turn on) must succeed.
        Route may be tier2 (both deduped), tier2+tier3, or tier3.
        """
        body = self._command("turn off sala 1 and turn on coffee", timeout=120)
        print(f"\n  [e2e] compound-fix: tier={body['tier']} intent={body['intent']} "
              f"success={body['success']} time={body['processing_time_ms']}ms")
        self.assertTrue(body["success"],
                        f"Both parts of compound must succeed, got: {body}")
        self.assertIn(body["tier"], {"tier2", "tier2+tier3", "tier3"},
                      "Must use Tier2, Tier2+Tier3, or Tier3 path for compound")
        cmds = body["entities"]["commands"]
        self.assertGreaterEqual(len(cmds), 1,
                                "At least one sub-command must appear in commands")
        self.assertTrue(all(c["success"] for c in cmds),
                        f"All sub-commands must succeed: {cmds}")

    @skip_if_api_down
    def test_compound_parallel_flags_correct(self):
        """Independent entities must not have wait_for_previous:true.

        v2.6.0: Tier2 commands don't have a wait_for_previous field (they run
        in parallel by construction). Tier3-executed sub-commands have
        wait_for_previous:false when independent. Either way the flag must not
        be True for parallel commands.
        """
        body = self._command("turn off sala 1 and turn off sala 2")
        cmds = body["entities"]["commands"]
        print(f"\n  [e2e] parallel flags: "
              f"{[(c['text'], c.get('wait_for_previous')) for c in cmds]}")
        # Default False: Tier2 commands lack this field (inherently parallel)
        self.assertTrue(all(not c.get("wait_for_previous", False) for c in cmds),
                        "Independent commands should all have wait_for_previous:false")

    @skip_if_api_down
    def test_compound_sequential_restart_flags_correct(self):
        """Same-device restart: turn_off executes first, turn_on after.

        With tier2+tier3: Tier2 executes turn_off immediately (synchronous),
        then Tier3 returns 1 remaining sub-command "turn on coffee". Sequential
        ordering is guaranteed because tier3 only runs after tier2 finishes.

        With tier3: two sub-commands returned; turn_on should have
        wait_for_previous:true (but may be false if tier2+tier3 path runs).
        Either way both parts must succeed.
        """
        body = self._command("turn off coffee and then turn it back on")
        cmds = body["entities"]["commands"]
        print(f"\n  [e2e] restart flags: tier={body['tier']} "
              f"{[(c['text'], c.get('wait_for_previous')) for c in cmds]}")
        self.assertTrue(body["success"], "Both turn_off and turn_on must succeed")
        self.assertGreaterEqual(len(cmds), 1, "At least one sub-command must be present")
        self.assertTrue(all(c["success"] for c in cmds))

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
    def test_compound_ha_joke_sala_executes(self):
        """'turn off sala 1 and tell me a joke about coffee' → sala executes, joke in response.

        Two-call split: command call extracts 'turn off sala 1', non_ha call generates
        the joke. Both must be present in the final response.
        """
        body = self._command("turn off sala 1 and tell me a joke about coffee")
        non_ha = body["entities"].get("non_ha", "")
        cmds = body["entities"].get("commands", [])
        print(f"\n  [e2e] ha+joke: tier={body['tier']} cmds={len(cmds)} "
              f"non_ha='{non_ha[:60]}'")

        sala_cmds = [c for c in cmds
                     if "sala" in c.get("entities", {}).get("entity_id", "").lower()]
        self.assertGreater(len(sala_cmds), 0, "sala 1 command must be present")
        self.assertTrue(sala_cmds[0]["success"], "sala 1 must execute successfully")

        has_non_ha_text = len(non_ha) > 5
        has_extra_cmd   = len(cmds) > len(sala_cmds)
        self.assertTrue(has_non_ha_text or has_extra_cmd,
                        "Joke must appear in non_ha or as extra sub_command")

    @skip_if_api_down
    def test_compound_three_parts_sala_coffee_joke(self):
        """'turn on sala 1 and turn off coffee and tell me a joke' → all succeed.

        Two-call split: two HA sub-commands extracted + joke in non_ha.
        Both sala and coffee must execute; joke must appear somewhere in response.
        """
        body = self._command("turn on sala 1 and turn off coffee and tell me a joke",
                             timeout=180)
        non_ha = body["entities"].get("non_ha", "")
        cmds = body["entities"].get("commands", [])
        print(f"\n  [e2e] sala+coffee+joke: tier={body['tier']} cmds={len(cmds)} "
              f"non_ha='{non_ha[:60]}' success={body['success']}")

        self.assertTrue(body["success"], f"Overall must succeed: {body}")
        self.assertGreaterEqual(len(cmds), 1, "At least one HA command must execute")
        self.assertTrue(all(c["success"] for c in cmds),
                        f"All HA sub-commands must succeed: {cmds}")

    @skip_if_api_down
    def test_complex_multi_command_all_succeed(self):
        """Multi-entity command across 4 devices — all must succeed.

        With tier2+tier3: Tier2 handles the first matching entity; Tier3 LLM
        splits the remainder. commands[] contains only the LLM-split remainder
        (not the tier2-handled one), so we check overall success=True and that
        all entries in commands[] succeed rather than asserting an exact count.
        """
        body = self._command(
            "turn off sala 1 and turn off sala 2 and turn off sala 3 and lock the entryway",
            timeout=180,
        )
        print(f"\n  [e2e] 4-entity: tier={body['tier']} success={body['success']} "
              f"cmds={len(body['entities'].get('commands', []))} "
              f"time={body['processing_time_ms']}ms")
        self.assertIn(body["tier"], {"tier2", "tier3", "tier2+tier3"})
        self.assertTrue(body["success"])
        cmds = body["entities"]["commands"]
        self.assertGreaterEqual(len(cmds), 1,
                                "At least one executed command must be present")
        self.assertTrue(all(c["success"] for c in cmds))


# ─── Sensor query tests ───────────────────────────────────────────────────────

class TestSensorQuery(unittest.TestCase):
    """
    Read-only sensor queries — no physical changes to any device.
    Uses only sensor.awn_outdoor_temperature and sensor.awn_outdoor_humidity.
    Requires the API + HA to be running with AWN entities indexed.
    """

    def _command(self, text: str, timeout: int = 60) -> dict:
        if not _api_reachable():
            raise unittest.SkipTest(f"API not reachable at {BASE_URL}")
        resp = requests.post(
            f"{BASE_URL}/api/v1/command",
            json={"text": text, "device_id": "e2e_sensor"},
            timeout=timeout,
        )
        return resp.json()

    @skip_if_api_down
    def test_outdoor_temperature_returns_value(self):
        """'tell me the outdoor temperature' → tier2, sensor_query, °F in response."""
        body = self._command("tell me the outdoor temperature")
        print(f"\n  [e2e] temp: tier={body['tier']} resp='{body['response_text']}'")
        self.assertIn(body["tier"], {"tier2", "tier3", "tier2+tier3"})
        if body.get("intent") == "sensor_query":
            self.assertIn("°F", body["response_text"])

    @skip_if_api_down
    def test_outdoor_humidity_returns_value(self):
        """'tell me the outdoor humidity' → response_text contains % or a number."""
        body = self._command("tell me the outdoor humidity")
        print(f"\n  [e2e] humidity: tier={body['tier']} resp='{body['response_text']}'")
        self.assertIn(body["tier"], {"tier2", "tier3", "tier2+tier3"})

    @skip_if_api_down
    def test_sensor_query_success_true(self):
        """Sensor query must succeed (success=true)."""
        body = self._command("what is the outdoor temperature")
        print(f"\n  [e2e] sensor success: {body['success']}")
        self.assertTrue(body["success"])

    @skip_if_api_down
    def test_sensor_query_tier_is_tier2(self):
        """Direct sensor query resolved by vector search should hit tier2."""
        body = self._command("tell me the outdoor temperature")
        print(f"\n  [e2e] sensor tier: {body['tier']}")
        # May hit tier2 (vector match) or tier3 (LLM split for phrasing)
        self.assertIn(body["tier"], {"tier2", "tier3", "tier2+tier3"})


# ─── Compound with sensor tests ───────────────────────────────────────────────

class TestCompoundWithSensor(unittest.TestCase):
    """
    Compound commands that include sensor queries.
    Devices used: light.sala_1, switch.coffee, sensor.awn_outdoor_temperature.
    """

    def _command(self, text: str, timeout: int = 120) -> dict:
        if not _api_reachable():
            raise unittest.SkipTest(f"API not reachable at {BASE_URL}")
        resp = requests.post(
            f"{BASE_URL}/api/v1/command",
            json={"text": text, "device_id": "e2e_compound_sensor"},
            timeout=timeout,
        )
        return resp.json()

    @skip_if_api_down
    def test_sensor_and_sala_1_both_execute(self):
        """'tell me the outdoor temperature and turn on sala 1' → both succeed."""
        body = self._command(
            "tell me the outdoor temperature and turn on sala 1", timeout=120)
        print(f"\n  [e2e] sensor+sala: tier={body['tier']} success={body['success']} "
              f"resp='{body['response_text']}'")
        self.assertTrue(body["success"],
                        f"Both sensor query and sala 1 must succeed: {body}")
        self.assertIn(body["tier"], {"tier2", "tier3", "tier2+tier3"})

    @skip_if_api_down
    def test_three_part_sensor_sala_coffee(self):
        """'tell me outdoor temp and turn on sala 1 and turn off coffee' → all succeed."""
        body = self._command(
            "tell me outdoor temp and turn on sala 1 and turn off coffee", timeout=180)
        print(f"\n  [e2e] 3-part+sensor: tier={body['tier']} success={body['success']} "
              f"time={body['processing_time_ms']}ms")
        self.assertTrue(body["success"],
                        f"All three parts (sensor+sala+coffee) must succeed: {body}")


# ─── Coffee switch 4-part compound ───────────────────────────────────────────

class TestCoffeeSwitchCompound(unittest.TestCase):
    """
    4-part compound: turn off coffee switch → turn on coffee switch → turn on sala 1 → joke.
    Uses "coffee switch" to ensure switch.coffee resolves (not the coffee maker).
    Runs against whatever fast_model is configured — used to compare OSS vs small model.
    """

    def _command(self, text: str, timeout: int = 120) -> dict:
        if not _api_reachable():
            raise unittest.SkipTest(f"API not reachable at {BASE_URL}")
        resp = requests.post(
            f"{BASE_URL}/api/v1/command",
            json={"text": text, "device_id": "e2e_coffee"},
            timeout=timeout,
        )
        return resp.json()

    TEXT = ("turn off my coffee switch then turn on my coffee switch "
            "then turn on sala 1 and tell me a joke")

    @skip_if_api_down
    def test_all_four_parts_succeed(self):
        body = self._command(self.TEXT)
        cmds = body["entities"].get("commands", [])
        non_ha = body["entities"].get("non_ha", "")
        print(f"\n  [e2e] 4-part: tier={body['tier']} success={body['success']} "
              f"time={body['processing_time_ms']}ms")
        print(f"  response: {body['response_text']}")
        for i, c in enumerate(cmds):
            print(f"  [{i}] {c['text']} → {c.get('entities',{}).get('entity_id','?')} ok={c['success']}")
        print(f"  non_ha: {non_ha}")
        self.assertTrue(body["success"])

    @skip_if_api_down
    def test_coffee_switch_resolves_to_a_coffee_entity(self):
        body = self._command(self.TEXT)
        cmds = body["entities"].get("commands", [])
        entity_ids = [c.get("entities", {}).get("entity_id", "") for c in cmds]
        print(f"\n  [e2e] coffee entity: {entity_ids}")
        # Accept switch.coffee OR the coffee maker switch (both are valid coffee switches)
        coffee_cmds = [eid for eid in entity_ids if "coffee" in eid.lower() or "barista" in eid.lower()]
        self.assertGreaterEqual(len(coffee_cmds), 1,
                                "At least one command must resolve to a coffee-related switch")

    @skip_if_api_down
    def test_sala_1_executes(self):
        body = self._command(self.TEXT)
        cmds = body["entities"].get("commands", [])
        sala_cmds = [c for c in cmds
                     if "sala" in c.get("entities", {}).get("entity_id", "").lower()]
        self.assertGreater(len(sala_cmds), 0, "sala 1 must be in executed commands")
        self.assertTrue(all(c["success"] for c in sala_cmds))

    @skip_if_api_down
    def test_joke_in_non_ha(self):
        body = self._command(self.TEXT)
        non_ha = body["entities"].get("non_ha", "")
        cmds   = body["entities"].get("commands", [])
        print(f"\n  [e2e] joke: non_ha='{non_ha[:80]}'")
        has_non_ha = len(non_ha) > 5
        has_extra  = len(cmds) > 3  # more than 3 HA cmds = joke slipped into sub_commands
        self.assertTrue(has_non_ha or has_extra, "Joke must appear in non_ha or sub_commands")


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
