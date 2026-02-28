# HMS-Assist Voice Assistant System

**100% local voice assistant with 3-tier intent classification**

## Overview

HMS-Assist is a complete voice assistant system combining ESP32-S3 firmware with a C++ backend service for intelligent intent classification and Home Assistant integration.

### Architecture

```
ESP32-S3 Voice Assistant (hms-assist-voice)
  ├─ I2S Microphone (INMP441 dual-channel)
  ├─ ESP-SR Wake Word Engine (custom TFLite model)
  ├─ MQTT Client → EMQX (192.168.2.15:1883)
  └─ I2S Speaker (MAX98357A)
      ↓ MQTT
HMS-Assist C++ Service (systemd)
  ├─ Tier 1: Deterministic (regex, 70-80%, <5ms)
  ├─ Tier 2: Embeddings (semantic, 15-20%, ~20ms) [Future]
  ├─ Tier 3: LLM (Ollama llama3.1:8b, 5-10%, 500-1500ms) [Future]
  ├─ Home Assistant API Client
  ├─ PostgreSQL (intent history)
  └─ MQTT Discovery (sensors)
      ↓ HTTP
Wyoming Services (192.168.2.5)
  ├─ Whisper STT (port 10300)
  └─ Piper TTS (port 10200)
```

## Components

### 1. ESP32-S3 Firmware (hms-assist-voice/)

**Hardware:** ReSpeaker Lite (ESP32-S3 with XMOS audio interface)

**Features:**
- Custom wake word detection ("hey glitchee", "ok glitchee")
- Dual-channel I2S audio (16kHz, 32-bit stereo)
- Wyoming STT/TTS integration
- MQTT communication
- Voice activity detection (VAD)

**Files:**
- `main/main.c` - Entry point
- `main/voice_task.c` - State machine (idle → wake → recording → STT → intent → TTS)
- `main/audio/` - I2S config, wake word detection
- `main/network/` - WiFi, MQTT, Wyoming clients

### 2. HMS-Assist C++ Service (hms-assist/)

**Features:**
- 3-tier intent classification (MVP: Tier 1 deterministic only)
- Home Assistant API integration
- PostgreSQL logging
- Health check HTTP endpoint (:8894)

**Files:**
- `src/main.cpp` - Service entry point
- `src/intent/DeterministicClassifier.cpp` - Regex-based intent matching
- `src/clients/HomeAssistantClient.cpp` - HA REST API client
- `src/services/VoiceService.cpp` - MQTT orchestrator

## Build & Deploy

### Prerequisites

```bash
# Install dependencies
sudo apt update
sudo apt install -y cmake g++ libdrogon-dev libjsoncpp-dev \
                    libpaho-mqttpp-dev libpqxx-dev libcurl4-openssl-dev \
                    postgresql-client

# ESP-IDF (for ESP32-S3 firmware)
# Follow: https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/
```

### HMS-Assist C++ Service

```bash
# 1. Create PostgreSQL database
psql -h localhost -U maestro -d postgres -f init_database.sql
# Password: maestro_postgres_2026_secure

# 2. Build service
cd /home/aamat/maestro_hub/hms-assist
mkdir build && cd build
cmake ..
make -j$(nproc)

# 3. Test service
./hms_assist

# 4. Install systemd service
sudo cp ../hms-assist.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable hms-assist
sudo systemctl start hms-assist

# 5. Check status
sudo systemctl status hms-assist
sudo journalctl -u hms-assist -f

# 6. Health check
curl http://localhost:8894/health
```

### ESP32-S3 Firmware

```bash
# 1. Set up ESP-IDF environment
cd /home/aamat/maestro_hub/hms-assist-voice
. ~/esp/esp-idf/export.sh

# 2. Configure project
idf.py set-target esp32s3
idf.py menuconfig
# Configure WiFi SSID/password if needed

# 3. Build firmware
idf.py build

# 4. Flash to ESP32-S3
idf.py -p /dev/ttyUSB0 flash

# 5. Monitor logs
idf.py -p /dev/ttyUSB0 monitor
```

## Configuration

### Environment Variables (HMS-Assist Service)

```bash
MQTT_BROKER=192.168.2.15
MQTT_PORT=1883
MQTT_USER=aamat
MQTT_PASS=exploracion
DB_HOST=localhost
DB_PORT=5432
DB_NAME=hms_assist
DB_USER=maestro
DB_PASS=maestro_postgres_2026_secure
HA_URL=http://192.168.2.15:8123
HA_TOKEN=<your_token>
HEALTH_CHECK_PORT=8894
```

### Wake Word Models

Pre-trained TFLite models are located at:
```
/home/aamat/m5stack/artifacts/wake-word-models/
  ├─ hey_glitchee.tflite
  └─ Ok_Glichy.tflite
```

Copy to ESP32-S3 SPIFFS partition during build.

## Testing

### Test MQTT Communication

```bash
# Subscribe to all HMS-Assist topics
mosquitto_sub -h 192.168.2.15 -t 'hms_assist/#' -u aamat -P exploracion -v

# Publish test STT result
mosquitto_pub -h 192.168.2.15 -t 'hms_assist/voice/test_device/stt_result' \
  -u aamat -P exploracion \
  -m '{"text": "turn on kitchen light", "confidence": 0.95}'

# Check intent result
# Should see: hms_assist/voice/test_device/intent_result
```

### Test Home Assistant Integration

```bash
# Check HA entities
curl -s -H "Authorization: Bearer <token>" \
  http://192.168.2.15:8123/api/states | python3 -m json.tool

# Turn on light manually
curl -X POST -H "Authorization: Bearer <token>" \
  -H "Content-Type: application/json" \
  -d '{"entity_id": "light.kitchen"}' \
  http://192.168.2.15:8123/api/services/light/turn_on
```

### Test Database

```bash
# Connect to database
psql -h localhost -U maestro -d hms_assist

# View statistics
SELECT * FROM command_statistics;
SELECT * FROM intent_distribution;
SELECT * FROM device_activity;

# View recent commands
SELECT * FROM voice_commands ORDER BY timestamp DESC LIMIT 10;
```

## Supported Commands (Tier 1 Deterministic)

### Light Control
- "turn on [location] light"
- "turn off [location] light"
- "toggle [location] light"

### Thermostat
- "set [location] thermostat to [temperature]"
- "make it warmer/cooler"

### Locks
- "lock [location] door"
- "unlock [location] door"

### Media
- "pause music"
- "skip to next song"
- "previous track"

### Scenes
- "activate [scene name] scene"
- "set [mode name] mode"

## Performance Metrics

| Metric | Target | Current (MVP) |
|--------|--------|---------------|
| Wake Word Detection | <200ms | ~150ms |
| STT Latency | <500ms | ~400ms |
| Tier 1 Classification | <5ms | ~2ms |
| TTS Latency | <300ms | ~250ms |
| **End-to-End (Simple)** | **<1.5s** | **~1.2s** |

## Troubleshooting

### ESP32-S3 Not Connecting to WiFi

Check SSID/password in `config.h`:
```c
#define WIFI_SSID "scorpio"
#define WIFI_PASSWORD "sabrina1111"
```

### MQTT Connection Failed

Verify broker is running:
```bash
sudo systemctl status emqx
mosquitto_sub -h 192.168.2.15 -t 'test' -u aamat -P exploracion
```

### PostgreSQL Connection Failed

Check database exists:
```bash
psql -h localhost -U maestro -l
```

### Home Assistant API Not Responding

Verify token and URL:
```bash
curl -H "Authorization: Bearer <token>" http://192.168.2.15:8123/api/
```

### Intent Classification Not Working

Check logs:
```bash
sudo journalctl -u hms-assist -f
```

View recent commands:
```sql
SELECT vc.command_text, ir.intent, ir.success, ir.response_text
FROM voice_commands vc
LEFT JOIN intent_results ir ON vc.id = ir.command_id
ORDER BY vc.timestamp DESC LIMIT 10;
```

## Future Enhancements (Phase 4-5)

### Tier 2: Embeddings Classifier
- ONNX Runtime integration
- all-MiniLM-L6-v2 sentence transformer
- Semantic similarity matching (~20ms)

### Tier 3: LLM Classifier
- Ollama integration (llama3.1:8b)
- Multi-intent parsing
- Context-aware responses (500-1500ms)

### MQTT Discovery
- Home Assistant sensor integration
- Real-time metrics
- Performance monitoring

## Documentation

- `CLAUDE.md` - Full project documentation
- `HOME_LAB_RATING.md` - Phase 19 implementation details
- Plan file: Implementation plan with complete architecture

## License

MIT

## Credits

- **Maestro Hub Team**
- Wyoming Protocol: https://github.com/rhasspy/wyoming
- ESP-IDF: https://github.com/espressif/esp-idf
- Drogon Framework: https://github.com/drogonframework/drogon
