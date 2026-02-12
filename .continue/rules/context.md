# Project Context: NHL Scoreboard (ESP32 + Hub75)

## Summary
- Real time NHL scoreboard for an ESP32-S3 driving a 64x32 Hub75 RGB matrix.
- Pulls NHL schedule and play-by-play data, renders a scoreboard scene, and shows a goal animation.
- Provides a small REST API and a web UI stored on LittleFS.

## Hardware + Build
- Target: ESP32-S3 (Freenove ESP32-S3 WROOM) with Hub75 64x32 panel.
- PlatformIO env: freenove_esp32_s3_wroom with Arduino framework.
- Filesystem: LittleFS (for web UI + logos + config).
- Key libs: ArduinoJson, ESP32-HUB75-MatrixPanel-DMA, Adafruit GFX.

## Runtime Flow
- Startup: init Serial, LittleFS, WiFi STA, NTP time sync, mDNS, API server, display.
- Loop: `apiServerLoop()` handles HTTP; `displayTick()` renders frames.

## Core Modules
### API server
- [src/api_server.cpp](src/api_server.cpp) hosts HTTP server and wires services.
- Persists selected game to `/scoreboard.json` in LittleFS.
- Endpoints:
	- `GET /` and `/index.html` -> web UI.
	- `GET /api/schedule` -> schedule snapshot.
	- `POST /api/select-game` -> set selected gameId.
	- `GET /api/selected-game` -> current selection.
	- `GET|POST /api/display-power` -> query / set display enabled.
	- `POST /api/preview-goal` -> trigger goal animation preview.
	- `GET /api/playbyplay` -> latest play-by-play snapshot.

### Schedule service
- [src/schedule_service.cpp](src/schedule_service.cpp) polls `https://api-web.nhle.com/v1/scoreboard/now`.
- Runs a FreeRTOS task every 30s, backs off on errors.
- Pauses polling when a game is selected (PBP takes over).
- Produces a simplified JSON array of games by date.

### Play-by-play service
- [src/playbyplay_service.cpp](src/playbyplay_service.cpp) polls NHL PBP when a game is selected.
- Detects new goals by `sortOrder`, builds roster cache for name lookups.
- Updates the shared data model and exposes a summary JSON.

### Data model
- [src/display/data_model.cpp](src/display/data_model.cpp) holds `GameSnapshot` with mutex protection.
- Updated by schedule and PBP services.
- `goalIsNew` flag triggers goal animation and is cleared after use.

### Display system
- [src/display/display_manager.cpp](src/display/display_manager.cpp) owns the HUB75 panel.
- Renders scoreboard scene or goal scene; goal animation lasts ~17s.
- `displayTriggerGoalPreview()` uses mock goal data for testing.
- Scenes:
	- [src/display/scoreboard_scene.cpp](src/display/scoreboard_scene.cpp): main scoreboard layout.
	- [src/display/goal_scene.cpp](src/display/goal_scene.cpp): animated goal overlay.

### Logo cache
- [src/display/logo_cache.cpp](src/display/logo_cache.cpp) loads `/logos/*.rgb565` from LittleFS.
- Cache size: 6 entries, with negative cache to avoid repeated misses.
- Supports 20x20 and 25x25 RGB565 files; adjusts colors for low bit depth.

## Web UI
- [data/index.html](data/index.html) calls the REST endpoints to list games and select one.
- UI + assets are uploaded to LittleFS via `pio run --target uploadfs`.

## Key Data and Files
- `/scoreboard.json` (LittleFS): selected game id.
- `data/logos/*.rgb565`: team logos (20x20 or 25x25 RGB565).
- `include/secrets.h`: WiFi credentials (copy from template).

## Build / Upload
- Upload filesystem: `pio run --target uploadfs`.
- Build + upload firmware: `pio run --target upload`.

## Logo Builder Tools
- [tools/logo_builder](tools/logo_builder) contains Python scripts to build logos.
- Outputs are copied into `data/logos/`.
