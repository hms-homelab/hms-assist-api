"""
E2E tests for hms-assist-api (HTTP layer).

Requires the API binary to be running on port 8894:
    cd projects/hms-assist/build
    HMS_ASSIST_CONFIG=/etc/hms-assist/config.yaml ./hms_assist &

Run:
    cd projects/hms-assist
    tools/venv/bin/pytest tests/e2e/test_api_e2e.py -v -s

Safe entities (not in the room):
    light.sala_1, light.sala_2_2, light.sala_3_2
    switch.dinner, switch.coffee
    sensor.awn_outdoor_temperature, sensor.awn_indoor_temperature,
    sensor.awn_outdoor_humidity, sensor.awn_max_daily_wind_gust,
    sensor.ups_back_ups_xs_1000m_ups_nominal_power
"""

import json
import os
import socket
import unittest

import requests


API_HOST = os.environ.get("HMS_ASSIST_HOST", "localhost")
API_PORT = int(os.environ.get("HMS_ASSIST_PORT", "8894"))
BASE_URL = f"http://{API_HOST}:{API_PORT}"


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


# ─── Device control: Tier1 catch-all ─────────────────────────────────────────

class TestDeviceControlTier1(unittest.TestCase):
    """
    Verify that 'turn on/off <name>' without 'light' suffix is caught by
    Tier1 catch-all device patterns. Uses dry_run=true for Tier1-only tests
    (no LLM needed, just regex + entity lookup).

    Safe entities: sala 1/2/3, dinner, coffee.
    """

    def _dry(self, text: str, timeout: int = 15) -> dict:
        if not _api_reachable():
            raise unittest.SkipTest(f"API not reachable at {BASE_URL}")
        resp = requests.post(
            f"{BASE_URL}/api/v1/command",
            json={"text": text, "device_id": "e2e_device", "dry_run": True},
            timeout=timeout,
        )
        return resp.json()

    def _assert_tier1(self, text, expected_entity, expected_action):
        body = self._dry(text)
        print(f"\n  [e2e] '{text}' -> tier={body['tier']} "
              f"entity={body.get('entities', {}).get('entity_id', '?')} "
              f"intent={body['intent']}")
        self.assertEqual(body["tier"], "tier1")
        self.assertTrue(body["success"])
        self.assertEqual(body["entities"]["entity_id"], expected_entity)
        self.assertIn(expected_action, body["entities"]["action"])

    @skip_if_api_down
    def test_turn_off_sala_1(self):
        self._assert_tier1("turn off sala 1", "light.sala_1", "off")

    @skip_if_api_down
    def test_turn_on_sala_1(self):
        self._assert_tier1("turn on sala 1", "light.sala_1", "on")

    @skip_if_api_down
    def test_turn_off_sala_2(self):
        body = self._dry("turn off sala 2")
        self.assertEqual(body["tier"], "tier1")
        self.assertTrue(body["success"])
        self.assertIn("sala", body["entities"]["entity_id"].lower())

    @skip_if_api_down
    def test_turn_off_sala_3(self):
        body = self._dry("turn off sala 3")
        self.assertEqual(body["tier"], "tier1")
        self.assertTrue(body["success"])
        self.assertIn("sala", body["entities"]["entity_id"].lower())

    @skip_if_api_down
    def test_turn_off_dinner_resolves_switch(self):
        body = self._dry("turn off dinner")
        self.assertEqual(body["tier"], "tier1")
        self.assertTrue(body["success"])
        self.assertIn("switch.", body["entities"]["entity_id"])

    @skip_if_api_down
    def test_turn_on_coffee(self):
        body = self._dry("turn on coffee")
        self.assertEqual(body["tier"], "tier1")
        self.assertTrue(body["success"])


# ─── Sensor rejection in Tier2 ──────────────────────────────────────────────

class TestSensorRejectionTier2(unittest.TestCase):
    """
    Verify Tier2 does NOT return sensor_query when the command has
    action words (turn on/off, toggle).
    """

    def _dry(self, text: str, timeout: int = 30) -> dict:
        if not _api_reachable():
            raise unittest.SkipTest(f"API not reachable at {BASE_URL}")
        resp = requests.post(
            f"{BASE_URL}/api/v1/command",
            json={"text": text, "device_id": "e2e_sensor_reject", "dry_run": True},
            timeout=timeout,
        )
        return resp.json()

    @skip_if_api_down
    def test_turn_off_sala_1_never_sensor_query(self):
        body = self._dry("turn off sala 1")
        print(f"\n  [e2e] sensor reject: tier={body['tier']} intent={body['intent']}")
        self.assertNotEqual(body.get("intent"), "sensor_query")

    @skip_if_api_down
    def test_turn_on_with_sensor_match_rejected(self):
        body = self._dry("turn on front door")
        self.assertNotEqual(body.get("intent"), "sensor_query")

    @skip_if_api_down
    def test_legit_sensor_query_still_works(self):
        body = self._dry("tell me the outdoor temperature")
        self.assertEqual(body["tier"], "tier2")
        self.assertEqual(body["intent"], "sensor_query")


# ─── Mixed integration: real calls, full pipeline ────────────────────────────

class TestMixedIntegration(unittest.TestCase):
    """
    6 real integration tests — full LLM pipeline, no dry_run.
    Mixes device actions, sensor queries, and non-HA content.

    Safe entities only: sala 1/2/3, dinner, coffee, sensors.
    """

    def _cmd(self, text: str, timeout: int = 120) -> dict:
        if not _api_reachable():
            raise unittest.SkipTest(f"API not reachable at {BASE_URL}")
        resp = requests.post(
            f"{BASE_URL}/api/v1/command",
            json={"text": text, "device_id": "e2e_mixed"},
            timeout=timeout,
        )
        return resp.json()

    # 1. Sensor query: indoor temperature -> tier2 sensor_query
    @skip_if_api_down
    def test_sensor_indoor_temperature(self):
        body = self._cmd("what is the indoor temperature")
        print(f"\n  [mix] indoor temp: tier={body['tier']} intent={body['intent']} "
              f"entity={body.get('entities', {}).get('entity_id', '?')} "
              f"conf={body['confidence']:.2f}")
        self.assertEqual(body["tier"], "tier2")
        self.assertEqual(body["intent"], "sensor_query")
        self.assertIn("indoor_temperature",
                       body["entities"]["entity_id"],
                       "Must resolve to awn_indoor_temperature sensor")
        self.assertGreater(body["confidence"], 0.65)

    # 2. Sensor query: UPS power -> tier2 sensor_query
    @skip_if_api_down
    def test_sensor_ups_power(self):
        body = self._cmd("how much power is the ups using")
        print(f"\n  [mix] ups power: tier={body['tier']} intent={body['intent']} "
              f"entity={body.get('entities', {}).get('entity_id', '?')} "
              f"conf={body['confidence']:.2f}")
        self.assertEqual(body["tier"], "tier2")
        self.assertEqual(body["intent"], "sensor_query")
        self.assertIn("ups", body["entities"]["entity_id"].lower())
        self.assertGreater(body["confidence"], 0.65)

    # 3. Device action + sensor query compound
    @skip_if_api_down
    def test_compound_action_plus_sensor(self):
        body = self._cmd("turn off sala 1 and what is the indoor temperature")
        cmds = body.get("entities", {}).get("commands", [])
        print(f"\n  [mix] action+sensor: tier={body['tier']} cmds={len(cmds)} "
              f"success={body['success']}")
        for i, c in enumerate(cmds):
            eid = (c.get("entities") or {}).get("entity_id", "?")
            print(f"    [{i}] '{c.get('text','')}' -> {eid} ok={c['success']}")

        self.assertTrue(body["success"], f"Both parts must succeed: {body}")
        sala_found = any("sala" in (c.get("entities") or {}).get("entity_id", "").lower()
                         for c in cmds)
        self.assertTrue(sala_found, "sala 1 must resolve to a sala entity")
        sensor_found = any("temperature" in (c.get("entities") or {}).get("entity_id", "").lower()
                           for c in cmds)
        self.assertTrue(sensor_found, "Indoor temperature sensor must resolve")

    # 4. Triple mix: non-HA + device action + sensor query
    @skip_if_api_down
    def test_compound_nonha_action_sensor(self):
        body = self._cmd("tell me a joke and turn off dinner and what is the wind speed")
        cmds = body.get("entities", {}).get("commands", [])
        non_ha = body.get("entities", {}).get("non_ha", "")
        print(f"\n  [mix] joke+dinner+wind: tier={body['tier']} cmds={len(cmds)} "
              f"non_ha='{non_ha[:60]}'")
        for i, c in enumerate(cmds):
            eid = (c.get("entities") or {}).get("entity_id", "?")
            print(f"    [{i}] '{c.get('text','')}' -> {eid} ok={c['success']}")

        # Dinner switch must resolve
        dinner_found = any("dinner" in (c.get("entities") or {}).get("entity_id", "").lower()
                           or "dinner" in c.get("text", "").lower()
                           for c in cmds if c.get("success"))
        self.assertTrue(dinner_found, "dinner switch must execute")

        # Wind sensor must resolve
        wind_found = any("wind" in (c.get("entities") or {}).get("entity_id", "").lower()
                         for c in cmds if c.get("success"))
        self.assertTrue(wind_found, "wind speed sensor must resolve")

        # Joke lands in non_ha when Tier3 LLM runs (tier2+tier3 or tier3).
        # When Tier2 handles all HA parts directly, joke may be dropped.
        if "tier3" in body["tier"]:
            has_non_ha = len(non_ha) > 5
            has_joke_cmd = any("joke" in c.get("text", "").lower() for c in cmds)
            self.assertTrue(has_non_ha or has_joke_cmd,
                            "Joke must appear in non_ha or sub_commands when Tier3 runs")

    # 5. Compound: two device actions + sensor
    @skip_if_api_down
    def test_compound_two_actions_plus_sensor(self):
        body = self._cmd(
            "turn on sala 2 and turn off sala 3 and tell me the outdoor humidity")
        cmds = body.get("entities", {}).get("commands", [])
        print(f"\n  [mix] 2 actions+sensor: tier={body['tier']} cmds={len(cmds)} "
              f"success={body['success']}")
        for i, c in enumerate(cmds):
            eid = (c.get("entities") or {}).get("entity_id", "?")
            print(f"    [{i}] '{c.get('text','')}' -> {eid} ok={c['success']}")

        self.assertTrue(body["success"], f"All parts must succeed: {body}")

        entity_ids = [(c.get("entities") or {}).get("entity_id", "").lower()
                      for c in cmds if c.get("success")]
        all_entities = " ".join(entity_ids)

        self.assertIn("sala", all_entities, "sala lights must resolve")
        self.assertIn("humidity", all_entities, "outdoor humidity sensor must resolve")

    # 6. Action words must NEVER resolve to sensor_query
    @skip_if_api_down
    def test_action_words_never_sensor_query(self):
        """Regression: action words (turn on/off) must never return sensor_query."""
        action_cmds = [
            "turn off sala 1",
            "turn on sala 2",
            "turn off sala 3",
            "turn off coffee",
            "turn off dinner",
        ]
        for text in action_cmds:
            body = self._cmd(text)
            print(f"\n  [mix] action guard: '{text}' -> tier={body['tier']} "
                  f"intent={body['intent']}")
            self.assertNotEqual(body.get("intent"), "sensor_query",
                                f"'{text}' must NOT be sensor_query, "
                                f"got: tier={body['tier']} entity="
                                f"{body.get('entities', {}).get('entity_id', '?')}")


# ─── Admin reindex ─────────────────────────────────────────────────────────────

class TestAdminReindex(unittest.TestCase):

    @skip_if_api_down
    def test_reindex_returns_success(self):
        resp = requests.post(f"{BASE_URL}/admin/reindex", timeout=10)
        self.assertEqual(resp.status_code, 200)
        body = resp.json()
        self.assertIn("success", body)
        self.assertIn("message", body)


# ─── DB persistence ───────────────────────────────────────────────────────────

class TestCommandPersistence(unittest.TestCase):
    """Verify commands are persisted to PostgreSQL."""

    @skip_if_api_down
    def test_total_commands_increases_after_request(self):
        before = requests.get(f"{BASE_URL}/health", timeout=5).json()["statistics"]["total_commands"]

        requests.post(
            f"{BASE_URL}/api/v1/command",
            json={"text": "turn on sala 1", "device_id": "persist_test"},
            timeout=60,
        )

        after = requests.get(f"{BASE_URL}/health", timeout=5).json()["statistics"]["total_commands"]
        self.assertGreater(after, before,
                           "total_commands did not increase after sending a command")


if __name__ == "__main__":
    unittest.main()
