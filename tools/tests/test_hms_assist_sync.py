"""
Unit tests for hms_assist_sync.py

Run from project root:
    cd projects/hms-assist
    tools/venv/bin/python -m pytest tools/tests/ -v

No network or DB connections are made — all external calls are mocked.
"""

import json
import sys
import types
import unittest
from unittest.mock import MagicMock, patch, call

# ── make the tools package importable without installing it ──────────────────
import os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from hms_assist_sync import (
    SKIP_DOMAINS,
    build_description,
    fetch_entities_from_rest,
    prune_stale,
    upsert_entity,
    vec_literal,
    run_sync,
    Config,
)


# ─── Helpers ─────────────────────────────────────────────────────────────────

def _entity(entity_id: str, state: str = "on", attrs: dict | None = None) -> dict:
    domain = entity_id.split(".")[0]
    a = attrs or {}
    return {
        "entity_id": entity_id,
        "domain": domain,
        "friendly_name": a.get("friendly_name", entity_id.split(".", 1)[-1].replace("_", " ").title()),
        "state": state,
        "attributes": a,
    }


_MINIMAL_CONFIG = Config(
    assist_db="fake-assist-conn",
    ha_db="",
    ha_url="http://ha.local:8123",
    ha_token="tok",
    ollama_url="http://ollama.local:11434",
    embed_model="nomic-embed-text",
)


# ─── vec_literal ─────────────────────────────────────────────────────────────

class TestVecLiteral(unittest.TestCase):
    def test_basic(self):
        self.assertEqual(vec_literal([0.1, 0.2, 0.3]), "[0.1,0.2,0.3]")

    def test_empty(self):
        self.assertEqual(vec_literal([]), "[]")

    def test_negative(self):
        result = vec_literal([-0.5, 1.0])
        self.assertIn("-0.5", result)
        self.assertIn("1.0", result)


# ─── build_description ───────────────────────────────────────────────────────

class TestBuildDescription(unittest.TestCase):

    def test_light_contains_controllable_keywords(self):
        e = _entity("light.kitchen_ceiling", "on", {"friendly_name": "Kitchen Ceiling"})
        desc = build_description(e, {}, {})
        self.assertIn("Kitchen Ceiling", desc)
        self.assertIn("light", desc)
        self.assertIn("on off", desc)

    def test_switch_keywords(self):
        e = _entity("switch.outlet_living_room", "off")
        desc = build_description(e, {}, {})
        self.assertIn("toggle", desc)

    def test_media_player_keywords(self):
        e = _entity("media_player.living_room_tv", "playing",
                    {"source_list": ["Netflix", "YouTube", "Plex"]})
        desc = build_description(e, {}, {})
        self.assertIn("pause", desc)
        self.assertIn("Netflix", desc)

    def test_climate_keywords(self):
        e = _entity("climate.thermostat", "heat",
                    {"current_temperature": 21, "hvac_modes": ["off", "heat", "cool"]})
        desc = build_description(e, {}, {})
        self.assertIn("temperature", desc)
        self.assertIn("heat", desc)

    def test_lock_keywords(self):
        e = _entity("lock.front_door", "locked")
        desc = build_description(e, {}, {})
        self.assertIn("lock", desc)
        self.assertIn("unlock", desc)

    def test_scene_keywords(self):
        e = _entity("scene.movie_night", "2024-01-01")
        desc = build_description(e, {}, {})
        self.assertIn("scene", desc)
        self.assertIn("activatable", desc)

    def test_vacuum_keywords(self):
        e = _entity("vacuum.roomba", "docked")
        desc = build_description(e, {}, {})
        self.assertIn("dock", desc)

    def test_cover_keywords(self):
        e = _entity("cover.garage_door", "closed")
        desc = build_description(e, {}, {})
        self.assertIn("open", desc)
        self.assertIn("close", desc)

    def test_automation_pulls_alias_and_description(self):
        e = _entity("automation.good_morning", "on")
        auto_details = {
            "good_morning": {
                "alias": "Good Morning Routine",
                "description": "Turns on lights and coffee maker at 7 AM",
            }
        }
        desc = build_description(e, auto_details, {})
        self.assertIn("Good Morning Routine", desc)
        self.assertIn("coffee maker", desc)
        self.assertIn("triggerable automation", desc)

    def test_script_pulls_alias(self):
        e = _entity("script.goodnight", "off")
        script_details = {
            "goodnight": {"alias": "Goodnight Sequence", "description": ""}
        }
        desc = build_description(e, {}, script_details)
        self.assertIn("Goodnight Sequence", desc)
        self.assertIn("runnable script", desc)

    def test_entity_id_always_included(self):
        e = _entity("binary_sensor.motion_hallway", "off")
        desc = build_description(e, {}, {})
        self.assertIn("binary_sensor.motion_hallway", desc)

    def test_device_class_included(self):
        e = _entity("binary_sensor.window_sensor", "off",
                    {"device_class": "window"})
        desc = build_description(e, {}, {})
        self.assertIn("window", desc)

    def test_area_included_when_present(self):
        e = _entity("light.bedroom_lamp", "off",
                    {"area": "Bedroom", "friendly_name": "Bedroom Lamp"})
        desc = build_description(e, {}, {})
        self.assertIn("Bedroom", desc)

    def test_unavailable_state_skipped(self):
        e = _entity("sensor.temp", "unavailable")
        desc = build_description(e, {}, {})
        self.assertNotIn("currently unavailable", desc)

    def test_media_player_source_list_capped_at_5(self):
        sources = ["S1", "S2", "S3", "S4", "S5", "S6", "S7"]
        e = _entity("media_player.tv", "on", {"source_list": sources})
        desc = build_description(e, {}, {})
        self.assertNotIn("S6", desc)
        self.assertNotIn("S7", desc)


# ─── SKIP_DOMAINS ────────────────────────────────────────────────────────────

class TestSkipDomains(unittest.TestCase):
    SHOULD_SKIP = [
        "sun", "persistent_notification", "update", "event",
        "zone", "person", "weather", "device_tracker",
        "stt", "tts", "recorder", "logbook",
    ]
    SHOULD_KEEP = [
        "light", "switch", "media_player", "climate",
        "lock", "cover", "fan", "vacuum", "scene",
        "automation", "script", "button", "select", "sensor",
        "binary_sensor", "input_boolean",
    ]

    def test_skip_domains_excluded(self):
        for d in self.SHOULD_SKIP:
            self.assertIn(d, SKIP_DOMAINS, f"{d} should be in SKIP_DOMAINS")

    def test_controllable_domains_kept(self):
        for d in self.SHOULD_KEEP:
            self.assertNotIn(d, SKIP_DOMAINS, f"{d} should NOT be in SKIP_DOMAINS")


# ─── fetch_entities_from_rest ─────────────────────────────────────────────────

class TestFetchEntitiesFromRest(unittest.TestCase):

    def _mock_response(self, data):
        resp = MagicMock()
        resp.json.return_value = data
        resp.raise_for_status = MagicMock()
        return resp

    @patch("hms_assist_sync.requests.get")
    def test_parses_entities(self, mock_get):
        mock_get.return_value = self._mock_response([
            {
                "entity_id": "light.kitchen",
                "state": "on",
                "attributes": {"friendly_name": "Kitchen Light"},
            },
            {
                "entity_id": "switch.fan",
                "state": "off",
                "attributes": {},
            },
        ])
        entities = fetch_entities_from_rest("http://ha:8123", "tok")
        self.assertEqual(len(entities), 2)
        self.assertEqual(entities[0]["entity_id"], "light.kitchen")
        self.assertEqual(entities[0]["domain"], "light")
        self.assertEqual(entities[0]["friendly_name"], "Kitchen Light")
        self.assertEqual(entities[1]["domain"], "switch")

    @patch("hms_assist_sync.requests.get")
    def test_friendly_name_fallback(self, mock_get):
        mock_get.return_value = self._mock_response([
            {"entity_id": "binary_sensor.motion_front", "state": "off", "attributes": {}}
        ])
        entities = fetch_entities_from_rest("http://ha:8123", "tok")
        self.assertEqual(entities[0]["friendly_name"], "Motion Front")

    @patch("hms_assist_sync.requests.get")
    def test_authorization_header_sent(self, mock_get):
        mock_get.return_value = self._mock_response([])
        fetch_entities_from_rest("http://ha:8123", "mytoken")
        _, kwargs = mock_get.call_args
        self.assertEqual(kwargs["headers"]["Authorization"], "Bearer mytoken")


# ─── upsert_entity ───────────────────────────────────────────────────────────

class TestUpsertEntity(unittest.TestCase):

    def test_executes_insert_with_correct_params(self):
        cur = MagicMock()
        entity = _entity("light.test", "on",
                         {"friendly_name": "Test Light", "brightness": 200})
        desc = "Test Light, light, on"
        embedding = [0.1] * 768

        upsert_entity(cur, entity, desc, embedding)
        self.assertTrue(cur.execute.called)
        args = cur.execute.call_args[0]
        sql, params = args[0], args[1]
        self.assertIn("INSERT INTO entity_embeddings", sql)
        self.assertIn("ON CONFLICT", sql)
        self.assertEqual(params[0], "light.test")
        self.assertEqual(params[1], "light")
        self.assertEqual(params[2], "Test Light")
        self.assertEqual(params[3], "on")
        self.assertIn('"brightness": 200', params[4])
        self.assertEqual(params[5], desc)


# ─── prune_stale ─────────────────────────────────────────────────────────────

class TestPruneStale(unittest.TestCase):

    def test_executes_delete(self):
        cur = MagicMock()
        cur.rowcount = 3
        result = prune_stale(cur, ["light.a", "switch.b"])
        self.assertTrue(cur.execute.called)
        sql, params = cur.execute.call_args[0]
        self.assertIn("DELETE FROM entity_embeddings", sql)
        self.assertEqual(result, 3)

    def test_skips_when_empty_list(self):
        cur = MagicMock()
        prune_stale(cur, [])
        cur.execute.assert_not_called()


# ─── run_sync ────────────────────────────────────────────────────────────────

class TestRunSync(unittest.TestCase):
    """Integration-style unit tests: mock all I/O, verify the orchestration."""

    def _make_config(self):
        return _MINIMAL_CONFIG

    @patch("hms_assist_sync.prune_stale")
    @patch("hms_assist_sync.upsert_entity")
    @patch("hms_assist_sync.embed")
    @patch("hms_assist_sync.fetch_automation_details", return_value={})
    @patch("hms_assist_sync.fetch_script_details", return_value={})
    @patch("hms_assist_sync.fetch_entities_from_rest")
    @patch("hms_assist_sync.psycopg2.connect")
    def test_run_sync_happy_path(
        self, mock_connect, mock_fetch_rest,
        mock_fetch_scripts, mock_fetch_autos,
        mock_embed, mock_upsert, mock_prune,
    ):
        # Two controllable entities
        mock_fetch_rest.return_value = [
            _entity("light.living_room", "on"),
            _entity("switch.fan", "off"),
        ]
        mock_embed.return_value = [0.0] * 768

        mock_conn = MagicMock()
        mock_cur = MagicMock()
        mock_cur.fetchone.return_value = (2,)
        mock_conn.cursor.return_value = mock_cur
        mock_connect.return_value = mock_conn

        stats = run_sync(self._make_config())

        self.assertEqual(stats["processed"], 2)
        self.assertEqual(stats["failed"], 0)
        self.assertEqual(mock_embed.call_count, 2)
        self.assertEqual(mock_upsert.call_count, 2)
        mock_prune.assert_called_once()
        mock_conn.commit.assert_called()

    @patch("hms_assist_sync.prune_stale")
    @patch("hms_assist_sync.upsert_entity")
    @patch("hms_assist_sync.embed")
    @patch("hms_assist_sync.fetch_automation_details", return_value={})
    @patch("hms_assist_sync.fetch_script_details", return_value={})
    @patch("hms_assist_sync.fetch_entities_from_rest")
    @patch("hms_assist_sync.psycopg2.connect")
    def test_skip_domains_filtered_in_run_sync(
        self, mock_connect, mock_fetch_rest,
        mock_fetch_scripts, mock_fetch_autos,
        mock_embed, mock_upsert, mock_prune,
    ):
        mock_fetch_rest.return_value = [
            _entity("light.living_room", "on"),
            _entity("sun.sun", "above_horizon"),          # should be skipped
            _entity("person.aamat", "home"),              # should be skipped
            _entity("weather.home", "sunny"),             # should be skipped
        ]
        mock_embed.return_value = [0.0] * 768
        mock_conn = MagicMock()
        mock_cur = MagicMock()
        mock_cur.fetchone.return_value = (1,)
        mock_conn.cursor.return_value = mock_cur
        mock_connect.return_value = mock_conn

        stats = run_sync(self._make_config())
        # Only light.living_room should be indexed
        self.assertEqual(stats["processed"], 1)
        self.assertEqual(mock_embed.call_count, 1)

    @patch("hms_assist_sync.prune_stale")
    @patch("hms_assist_sync.upsert_entity")
    @patch("hms_assist_sync.embed", side_effect=RuntimeError("Ollama down"))
    @patch("hms_assist_sync.fetch_automation_details", return_value={})
    @patch("hms_assist_sync.fetch_script_details", return_value={})
    @patch("hms_assist_sync.fetch_entities_from_rest")
    @patch("hms_assist_sync.psycopg2.connect")
    def test_embed_failure_counted_as_failed(
        self, mock_connect, mock_fetch_rest,
        mock_fetch_scripts, mock_fetch_autos,
        mock_embed, mock_upsert, mock_prune,
    ):
        mock_fetch_rest.return_value = [_entity("light.x", "on")]
        mock_conn = MagicMock()
        mock_cur = MagicMock()
        mock_cur.fetchone.return_value = (0,)
        mock_conn.cursor.return_value = mock_cur
        mock_connect.return_value = mock_conn

        stats = run_sync(self._make_config())
        self.assertEqual(stats["processed"], 0)
        self.assertEqual(stats["failed"], 1)
        mock_upsert.assert_not_called()

    @patch("hms_assist_sync.embed")
    @patch("hms_assist_sync.fetch_automation_details", return_value={})
    @patch("hms_assist_sync.fetch_script_details", return_value={})
    @patch("hms_assist_sync.fetch_entities_from_rest")
    def test_dry_run_does_not_connect_to_db(
        self, mock_fetch_rest, mock_fetch_scripts, mock_fetch_autos, mock_embed
    ):
        mock_fetch_rest.return_value = [_entity("light.x", "on")]

        with patch("hms_assist_sync.psycopg2.connect") as mock_connect:
            run_sync(self._make_config(), dry_run=True)
            mock_connect.assert_not_called()

        mock_embed.assert_not_called()


if __name__ == "__main__":
    unittest.main()
