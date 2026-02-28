# Patio Lights - Home Assistant Assist Integration

## Problem
Home Assistant Assist can't control your patio lights because they're defined as **button** entities, not **light** entities. Buttons are momentary actions (like pressing a key), while lights need to be stateful (on/off with brightness).

## Solution
Create a template light entity that wraps your RF button entities and tracks state using helper entities.

## Installation Steps

### 1. Copy Configuration File

Copy `ha_patio_lights_template.yaml` to your Home Assistant config directory:

```bash
# Via SMB (recommended)
source .secrets
smbclient //$HA_SMB_HOST/config -U $HA_SMB_USER%$HA_SMB_PASS -c "put ha_patio_lights_template.yaml"

# OR via SSH if you have access
scp ha_patio_lights_template.yaml homeassistant@192.168.2.7:/config/
```

### 2. Update configuration.yaml

Add these lines to your `/config/configuration.yaml`:

```yaml
# Include patio lights template configuration
input_boolean: !include ha_patio_lights_template.yaml
input_select: !include ha_patio_lights_template.yaml
light: !include ha_patio_lights_template.yaml
automation: !include ha_patio_lights_template.yaml
```

**OR** if you already have these sections, merge the contents manually or use includes like:

```yaml
input_boolean: !include_dir_merge_named input_booleans/
input_select: !include_dir_merge_named input_selects/
light: !include_dir_merge_list lights/
automation: !include_dir_merge_list automations/
```

### 3. Restart Home Assistant

```bash
# Via HA web UI
# Settings → System → Restart

# OR via API
curl -X POST -H "Authorization: Bearer $HA_BEARER_TOKEN" \
  http://192.168.2.7:8123/api/services/homeassistant/restart
```

### 4. Verify the Light Entity

After restart, check that the light entity exists:

```bash
curl -s -H "Authorization: Bearer $HA_BEARER_TOKEN" \
  "http://192.168.2.7:8123/api/states/light.patio" | python3 -m json.tool
```

You should see a light entity called `light.patio` with brightness support.

## How It Works

### Components Created

1. **input_boolean.patio_lights_state**
   - Tracks whether lights are on or off
   - Toggled when you use ON/OFF button

2. **input_select.patio_lights_brightness**
   - Tracks current brightness level
   - Options: Off, 25%, 50%, 75%, 100%

3. **light.patio**
   - Main template light entity
   - Maps brightness levels (0-255) to your RF buttons
   - Triggers the appropriate `button.patio_lights_patio_*` entity

4. **Automations (5 total)**
   - Sync state when physical remote buttons are pressed
   - Keeps HA state in sync with actual lights

### Brightness Mapping

| HA Brightness | RF Button | Entity |
|---------------|-----------|--------|
| 0 (off) | OFF | `button.patio_lights_patio_on_off` |
| 1-63 | 25% | `button.patio_lights_patio_25` |
| 64-127 | 50% | `button.patio_lights_patio_50` |
| 128-191 | 75% | `button.patio_lights_patio_75` |
| 192-255 | 100% | `button.patio_lights_patio_100` |

## Using with Home Assistant Assist

After setup, you can say:

- **"Turn on the patio lights"** → Turns on at 100%
- **"Turn off the patio lights"** → Turns off
- **"Set patio lights to 50%"** → Sets to 50% brightness
- **"Dim the patio lights"** → Reduces brightness
- **"Brighten the patio lights"** → Increases brightness

Assist will now recognize `light.patio` as a controllable light entity!

## Troubleshooting

### Light entity not showing up

1. Check configuration.yaml syntax:
   ```bash
   # Via HA CLI
   ha core check
   ```

2. Check logs for errors:
   ```bash
   # Via web UI: Settings → System → Logs
   ```

3. Verify entity IDs match your actual buttons

### State not syncing with physical remote

- Automations should sync state when you press physical remote buttons
- If not working, check automation traces in HA UI
- Verify binary sensor entities are triggering correctly

### Brightness levels not working

- Check that button entities are pressing correctly
- Test manually in Developer Tools → Services:
  ```yaml
  service: button.press
  target:
    entity_id: button.patio_lights_patio_50
  ```

## Current Button Entities

Your RF gateway exposes these button entities:

| Entity ID | Function | Friendly Name |
|-----------|----------|---------------|
| `button.patio_lights_patio_on_off` | Toggle ON/OFF | Patio Lights Remote Patio ON/OFF |
| `button.patio_lights_patio_25` | 25% Brightness | Patio Lights Remote Patio 25% |
| `button.patio_lights_patio_50` | 50% Brightness | Patio Lights Remote Patio 50% |
| `button.patio_lights_patio_75` | 75% Brightness | Patio Lights Remote Patio 75% |
| `button.patio_lights_patio_100` | 100% Brightness | Patio Lights Remote Patio 100% |
| `button.patio_lights_patio_timer_3h` | 3 Hour Timer | Patio Lights Remote Patio Timer 3H |
| `button.patio_lights_patio_timer_5h` | 5 Hour Timer | Patio Lights Remote Patio Timer 5H |
| `button.patio_lights_patio_lighting` | Lighting Mode | Patio Lights Remote Patio Lighting |
| `button.patio_lights_patio_flash` | Flash Mode | Patio Lights Remote Patio Flash |
| `button.patio_lights_patio_breath` | Breath Mode | Patio Lights Remote Patio Breath |
| `button.patio_lights_patio_plus` | Brightness + | Patio Lights Remote Patio Plus |
| `button.patio_lights_patio_minus` | Brightness - | Patio Lights Remote Patio Minus |

## Binary Sensor Entities (Physical Remote Detection)

These detect when you press the physical remote:

| Entity ID | Purpose |
|-----------|---------|
| `binary_sensor.takt_rf_gateway_patio_remote_on_off_button` | Detects ON/OFF button press |
| `binary_sensor.takt_rf_gateway_patio_remote_25_button` | Detects 25% button press |
| `binary_sensor.takt_rf_gateway_patio_remote_50_button` | Detects 50% button press |
| `binary_sensor.takt_rf_gateway_patio_remote_75_button` | Detects 75% button press |
| `binary_sensor.takt_rf_gateway_patio_remote_100_button` | Detects 100% button press |

These are used in the automations to sync state when you use the physical remote.

## Next Steps

### Optional Enhancements

1. **Add lighting modes to HA**
   - Create scripts for Flash, Breath, Lighting modes
   - Add as scenes for easy voice control

2. **Timer Integration**
   - Create automations that use the timer buttons
   - "Set patio lights timer for 3 hours"

3. **Advanced Brightness Control**
   - Add support for Plus/Minus buttons
   - "Increase patio lights brightness"

4. **Group with Other Lights**
   - Create light group if you have other outdoor lights
   - "Turn on all outdoor lights"

## Files

- `ha_patio_lights_template.yaml` - Main configuration file
- `PATIO_LIGHTS_ASSIST_SETUP.md` - This documentation

## Achievement Unlocked 🎉

**THE NEIGHBOR HORNER GETS VOICE CONTROL!**

Your legendary 433MHz RF reverse-engineering project now has full Home Assistant Assist integration. From accidentally honking neighbor's cars to "Hey Google, dim the patio lights" - you've come full circle!

**Status**: Voice control ready, zero cloud dependency, 100% local! 🔥
