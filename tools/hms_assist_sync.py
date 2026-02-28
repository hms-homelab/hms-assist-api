#!/usr/bin/env python3
"""
hms-assist-sync

Standalone sync tool that indexes Home Assistant entities, automations,
and scripts into pgvector for semantic search by hms-assist-api (Tier 2).

Usage:
  hms_assist_sync.py                       # continuous mode (runs hourly)
  hms_assist_sync.py --once                # run once and exit (cron/systemd)
  hms_assist_sync.py --dry-run             # show what would be indexed
  hms_assist_sync.py --config /path/to/config.yaml
"""

import argparse
import json
import logging
import os
import sys
import time
from dataclasses import dataclass, field
from typing import Optional

import psycopg2
import requests
import yaml

log = logging.getLogger("hms-assist-sync")

# ─── Domains to skip — not voice-controllable ───────────────────────────────
SKIP_DOMAINS = {
    "sun", "persistent_notification", "update", "event",
    "zone", "person", "weather", "device_tracker", "conversation",
    "stt", "tts", "wake_word", "recorder", "logbook",
    "history_stats", "counter", "timer", "system_health",
}


# ─── Config ─────────────────────────────────────────────────────────────────

@dataclass
class Config:
    # hms_assist DB (pgvector target)
    assist_db: str
    # HA recorder DB (entity source)
    ha_db: str
    # HA REST API (automation/script details)
    ha_url: str
    ha_token: str
    # Ollama
    ollama_url: str
    embed_model: str


def load_config(path: str) -> Config:
    with open(path) as f:
        cfg = yaml.safe_load(f)

    db = cfg["database"]
    base = f"host={db['host']} port={db.get('port', 5432)} user={db['user']} password={db['password']}"

    return Config(
        assist_db=f"{base} dbname={db['name']}",
        ha_db=f"{base} dbname={db['ha_db_name']}" if db.get("ha_db_name") else "",
        ha_url=cfg["homeassistant"]["url"],
        ha_token=cfg["homeassistant"]["token"],
        ollama_url=cfg["ollama"]["url"],
        embed_model=cfg["ollama"]["embed_model"],
    )


# ─── Fetch entities ──────────────────────────────────────────────────────────

def fetch_entities_from_ha_db(conn_str: str) -> list[dict]:
    """Fast path: query HA recorder DB directly for latest state of every entity.

    HA recorder (2023+) stores entity_id in states_meta; states.entity_id is
    a legacy character(1) column.  Join via metadata_id instead.
    """
    conn = psycopg2.connect(conn_str)
    cur = conn.cursor()
    cur.execute("""
        SELECT DISTINCT ON (sm.entity_id)
               sm.entity_id,
               s.state,
               COALESCE(sa.shared_attrs, '{}') AS attributes
        FROM   states s
        JOIN   states_meta sm ON s.metadata_id = sm.metadata_id
        LEFT JOIN state_attributes sa ON s.attributes_id = sa.attributes_id
        WHERE  s.state IS NOT NULL
          AND  s.state NOT IN ('unavailable', 'unknown')
        ORDER  BY sm.entity_id, s.last_updated_ts DESC
    """)
    rows = cur.fetchall()
    cur.close()
    conn.close()

    entities = []
    for entity_id, state, attrs_str in rows:
        try:
            attrs = json.loads(attrs_str) if attrs_str else {}
        except json.JSONDecodeError:
            attrs = {}

        domain = entity_id.split(".")[0] if "." in entity_id else entity_id
        friendly_name = (
            attrs.get("friendly_name")
            or entity_id.split(".", 1)[-1].replace("_", " ").title()
        )
        entities.append({
            "entity_id": entity_id,
            "domain": domain,
            "friendly_name": friendly_name,
            "state": state,
            "attributes": attrs,
        })

    return entities


def fetch_entities_from_rest(ha_url: str, ha_token: str) -> list[dict]:
    """Fallback: fetch entities via HA REST API."""
    headers = {"Authorization": f"Bearer {ha_token}"}
    resp = requests.get(f"{ha_url}/api/states", headers=headers, timeout=30)
    resp.raise_for_status()

    entities = []
    for item in resp.json():
        entity_id = item["entity_id"]
        domain = entity_id.split(".")[0] if "." in entity_id else entity_id
        attrs = item.get("attributes", {})
        friendly_name = (
            attrs.get("friendly_name")
            or entity_id.split(".", 1)[-1].replace("_", " ").title()
        )
        entities.append({
            "entity_id": entity_id,
            "domain": domain,
            "friendly_name": friendly_name,
            "state": item.get("state", ""),
            "attributes": attrs,
        })

    return entities


# ─── Fetch automation / script details from REST ─────────────────────────────

def fetch_automation_details(ha_url: str, ha_token: str) -> dict:
    """Fetch automation configs (alias + description) for richer embeddings."""
    headers = {"Authorization": f"Bearer {ha_token}"}
    try:
        resp = requests.get(
            f"{ha_url}/api/config/automation/config",
            headers=headers, timeout=15
        )
        if resp.ok:
            return {
                a.get("id", ""): a
                for a in resp.json()
                if isinstance(a, dict) and a.get("id")
            }
    except Exception as e:
        log.warning(f"Could not fetch automation details: {e}")
    return {}


def fetch_script_details(ha_url: str, ha_token: str) -> dict:
    """Fetch script configs (alias + description) for richer embeddings."""
    headers = {"Authorization": f"Bearer {ha_token}"}
    try:
        resp = requests.get(
            f"{ha_url}/api/config/script/config",
            headers=headers, timeout=15
        )
        if resp.ok:
            return resp.json()
    except Exception as e:
        log.warning(f"Could not fetch script details: {e}")
    return {}


# ─── Build embedding description ─────────────────────────────────────────────

def _voice_hints(name: str, domain: str) -> str:
    """
    Generate natural-language voice command examples for a given entity name
    and domain.  These are embedded alongside the description so that
    conversational queries (e.g. "brighten the sala") score higher in cosine
    similarity against the right entity.
    """
    n = name.lower()
    if domain == "light":
        return (f"turn on {n}, turn off {n}, brighten {n}, dim {n}, "
                f"toggle {n}, switch on {n}, switch off {n}")
    if domain == "switch":
        return f"turn on {n}, turn off {n}, toggle {n}, switch on {n}, switch off {n}"
    if domain == "media_player":
        return (f"play {n}, pause {n}, stop {n}, mute {n}, "
                f"next song {n}, skip {n}, volume up {n}")
    if domain == "climate":
        return f"set {n} temperature, heat {n}, cool {n}, adjust {n} thermostat"
    if domain == "cover":
        return f"open {n}, close {n}, raise {n}, lower {n}"
    if domain == "lock":
        return f"lock {n}, unlock {n}, secure {n}"
    if domain == "fan":
        return f"turn on {n} fan, turn off {n} fan, speed up {n}, slow down {n}"
    if domain == "vacuum":
        return f"start {n}, clean {n}, dock {n}, return {n}"
    if domain == "scene":
        return f"activate {n}, set {n} scene, run {n}"
    if domain == "script":
        return f"run {n}, execute {n}, trigger {n}"
    return ""


def build_description(entity: dict,
                       automation_details: dict,
                       script_details: dict) -> str:
    """
    Build a rich natural-language description of an entity.
    This text is what gets embedded — the richer and more specific it is,
    the better Tier 2 semantic search will perform.

    The returned string does NOT include the 'search_document:' prefix;
    the caller (run_sync) prepends that before embedding so that
    nomic-embed-text asymmetric retrieval is applied correctly.
    """
    parts = [entity["friendly_name"]]
    domain = entity["domain"]
    attrs = entity["attributes"]
    state = entity["state"]

    parts.append(f"({domain})")

    if state not in ("unavailable", "unknown", ""):
        parts.append(f"currently {state}")

    # Location context (area/room populated by HA area registry)
    for key in ("area", "room", "floor"):
        if attrs.get(key):
            parts.append(f"{key}: {attrs[key]}")

    # Device class: motion, door, temperature, humidity, etc.
    if attrs.get("device_class"):
        parts.append(f"device class: {attrs['device_class']}")

    # Domain-specific context
    if domain == "light":
        parts.append("controllable: on off toggle brightness color")
        if attrs.get("supported_color_modes"):
            parts.append(f"color modes: {' '.join(attrs['supported_color_modes'])}")

    elif domain == "switch":
        parts.append("controllable: on off toggle")

    elif domain == "media_player":
        parts.append("controllable: play pause stop volume next previous shuffle mute")
        if attrs.get("source_list"):
            sources = attrs["source_list"][:5]  # cap to avoid huge descriptions
            parts.append(f"sources: {' '.join(sources)}")

    elif domain == "climate":
        parts.append("controllable: temperature heat cool fan mode")
        if attrs.get("current_temperature") is not None:
            parts.append(f"current temperature: {attrs['current_temperature']}")
        if attrs.get("hvac_modes"):
            parts.append(f"modes: {' '.join(attrs['hvac_modes'])}")

    elif domain == "cover":
        parts.append("controllable: open close stop position blinds curtains garage")

    elif domain == "lock":
        parts.append("controllable: lock unlock")

    elif domain == "fan":
        parts.append("controllable: on off speed percentage")

    elif domain == "vacuum":
        parts.append("controllable: start stop pause return dock clean")

    elif domain == "scene":
        parts.append("activatable scene routine preset mood")

    elif domain == "automation":
        # Fetch alias + description from automation config
        auto_id = entity["entity_id"].replace("automation.", "", 1)
        auto = automation_details.get(auto_id, {})
        if auto.get("alias"):
            parts.append(f"alias: {auto['alias']}")
        if auto.get("description"):
            parts.append(f"description: {auto['description']}")
        parts.append("triggerable automation routine")

    elif domain == "script":
        script_id = entity["entity_id"].replace("script.", "", 1)
        script = script_details.get(script_id, {})
        if isinstance(script, dict):
            if script.get("alias"):
                parts.append(f"alias: {script['alias']}")
            if script.get("description"):
                parts.append(f"description: {script['description']}")
        parts.append("runnable script sequence")

    elif domain == "input_boolean":
        parts.append("toggle switch on off")

    elif domain == "input_select":
        if attrs.get("options"):
            parts.append(f"options: {' '.join(attrs['options'][:8])}")

    # Entity ID often encodes location (e.g. light.kitchen_ceiling)
    parts.append(f"id: {entity['entity_id']}")

    # Append voice command examples for controllable domains
    hints = _voice_hints(entity["friendly_name"], domain)
    if hints:
        parts.append(f"voice commands: {hints}")

    return ", ".join(parts)


# ─── Ollama embedding ─────────────────────────────────────────────────────────

def embed(text: str, ollama_url: str, model: str) -> list[float]:
    resp = requests.post(
        f"{ollama_url}/api/embeddings",
        json={"model": model, "prompt": text},
        timeout=30,
    )
    resp.raise_for_status()
    return resp.json()["embedding"]


# ─── pgvector upsert ──────────────────────────────────────────────────────────

def vec_literal(embedding: list[float]) -> str:
    return "[" + ",".join(str(v) for v in embedding) + "]"


def upsert_entity(cur, entity: dict, description: str, embedding: list[float]):
    cur.execute("""
        INSERT INTO entity_embeddings
            (entity_id, domain, friendly_name, state, attributes_json,
             description, embedding, last_updated)
        VALUES (%s, %s, %s, %s, %s, %s, %s::vector, NOW())
        ON CONFLICT (entity_id) DO UPDATE SET
            domain          = EXCLUDED.domain,
            friendly_name   = EXCLUDED.friendly_name,
            state           = EXCLUDED.state,
            attributes_json = EXCLUDED.attributes_json,
            description     = EXCLUDED.description,
            embedding       = EXCLUDED.embedding,
            last_updated    = NOW()
    """, (
        entity["entity_id"],
        entity["domain"],
        entity["friendly_name"],
        entity["state"],
        json.dumps(entity["attributes"]),
        description,
        vec_literal(embedding),
    ))


def prune_stale(cur, active_ids: list[str]):
    if not active_ids:
        return
    cur.execute(
        "DELETE FROM entity_embeddings WHERE entity_id != ALL(%s)",
        (active_ids,)
    )
    return cur.rowcount


# ─── Main sync logic ──────────────────────────────────────────────────────────

def run_sync(config: Config, dry_run: bool = False) -> dict:
    """
    Run a full sync. Returns a stats dict.
    """
    t_start = time.time()
    stats = {"processed": 0, "failed": 0, "pruned": 0, "total_in_db": 0}

    # 1. Fetch entities
    entities = []
    if config.ha_db:
        try:
            entities = fetch_entities_from_ha_db(config.ha_db)
            log.info(f"Fetched {len(entities)} entities from HA PostgreSQL DB")
        except Exception as e:
            log.warning(f"HA DB fetch failed ({e}), falling back to REST API")

    if not entities:
        entities = fetch_entities_from_rest(config.ha_url, config.ha_token)
        log.info(f"Fetched {len(entities)} entities via HA REST API")

    # 2. Fetch richer details for automations and scripts
    automation_details = fetch_automation_details(config.ha_url, config.ha_token)
    script_details = fetch_script_details(config.ha_url, config.ha_token)
    log.info(f"  Automation configs: {len(automation_details)}, Script configs: {len(script_details)}")

    # 3. Filter non-controllable domains
    before = len(entities)
    entities = [e for e in entities if e["domain"] not in SKIP_DOMAINS]
    log.info(f"Filtered {before - len(entities)} non-controllable → {len(entities)} to index")

    if dry_run:
        log.info("─── DRY RUN — sample descriptions ───")
        for e in entities[:10]:
            desc = build_description(e, automation_details, script_details)
            log.info(f"  [{e['domain']:15s}] {e['entity_id']}")
            log.info(f"               → {desc[:120]}")
        log.info(f"─── Would index {len(entities)} entities ───")
        stats["processed"] = len(entities)
        return stats

    # 4. Connect to assist DB
    conn = psycopg2.connect(config.assist_db)
    cur = conn.cursor()

    active_ids = []
    failed = []

    for i, entity in enumerate(entities):
        try:
            description = build_description(entity, automation_details, script_details)
            embedding = embed("search_document: " + description, config.ollama_url, config.embed_model)
            upsert_entity(cur, entity, description, embedding)
            active_ids.append(entity["entity_id"])
            stats["processed"] += 1

            if stats["processed"] % 25 == 0:
                conn.commit()
                elapsed = time.time() - t_start
                rate = stats["processed"] / elapsed
                eta = (len(entities) - stats["processed"]) / rate if rate > 0 else 0
                log.info(
                    f"  {stats['processed']}/{len(entities)} "
                    f"({rate:.1f}/s, ETA {eta:.0f}s)"
                )
        except Exception as e:
            log.warning(f"  ✗ {entity['entity_id']}: {e}")
            failed.append(entity["entity_id"])
            stats["failed"] += 1

    conn.commit()

    # 5. Prune stale entities
    pruned = prune_stale(cur, active_ids)
    stats["pruned"] = pruned or 0
    conn.commit()

    # 6. Total count
    cur.execute("SELECT COUNT(*) FROM entity_embeddings")
    stats["total_in_db"] = cur.fetchone()[0]

    cur.close()
    conn.close()

    elapsed = time.time() - t_start
    log.info(
        f"Sync complete in {elapsed:.1f}s — "
        f"{stats['processed']} indexed, {stats['failed']} failed, "
        f"{stats['pruned']} pruned, {stats['total_in_db']} total in DB"
    )
    if failed:
        log.warning(f"Failed: {failed[:10]}{'...' if len(failed) > 10 else ''}")

    return stats


# ─── Entry point ──────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Sync HA entities/automations/scripts into pgvector for hms-assist"
    )
    parser.add_argument(
        "--config", default=os.environ.get("HMS_ASSIST_CONFIG", "/etc/hms-assist/config.yaml"),
        help="Path to config.yaml (default: /etc/hms-assist/config.yaml)"
    )
    parser.add_argument("--once", action="store_true",
                        help="Run once and exit — for cron or systemd oneshot")
    parser.add_argument("--dry-run", action="store_true",
                        help="Show descriptions without writing to DB")
    parser.add_argument("--interval", type=int, default=3600,
                        help="Sync interval in seconds for continuous mode (default: 3600)")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s [%(levelname)s] %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    )

    try:
        config = load_config(args.config)
    except Exception as e:
        log.error(f"Failed to load config from {args.config}: {e}")
        sys.exit(1)

    log.info(f"Config loaded — DB: {config.assist_db.split('dbname=')[-1].split()[0]}, "
             f"Ollama: {config.ollama_url}, model: {config.embed_model}")

    if args.once or args.dry_run:
        stats = run_sync(config, dry_run=args.dry_run)
        sys.exit(0 if stats["processed"] > 0 else 1)

    # Continuous mode
    log.info(f"Continuous mode — syncing every {args.interval}s")
    while True:
        try:
            run_sync(config)
        except Exception as e:
            log.error(f"Sync run failed: {e}")
        log.info(f"Next sync in {args.interval}s")
        time.sleep(args.interval)


if __name__ == "__main__":
    main()
