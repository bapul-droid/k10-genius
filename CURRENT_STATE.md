# ESP32 PROJECT — CURRENT_STATE

Last updated: 2026-09-30
Scope: Hardware + firmware ESP32 devices only.
Primary devices covered now: DFRobot K10 and Minji ESP32-S3.

> READ THIS FILE FIRST when continuing the ESP32 project in a new chat/table.
> Server-side Genius V2 architecture belongs to the separate Genius project.
> This file may mention Genius V2 only where it affects ESP32 firmware/device behavior.

---

## 0. PROJECT BOUNDARY

This project owns:
- ESP32 hardware.
- ESP32 firmware.
- Board-specific GPIO/configuration.
- Audio, display, camera, wake/VAD, battery and device-side media.
- Device-side provisioning and connection to Genius V2.
- Experimental ESP32 branches such as ESP-Claw.

This project does NOT own:
- Genius V2 server internals.
- Server skill catalog/routing implementation.
- Telegram control/backend implementation.
- Home Assistant server integration.
- Server storage, proxy, console, or OpenKey backend.

Those belong to the separate Genius project.

---

# 1. DFRobot K10 — CURRENT STATE

## Identity

Device family: DFRobot UNIHIKER K10
Main repo: `bapul-droid/k10-genius`
Active branch: `feature/k10-lcd-auto-sleep`

Historical checkpoint:
- `80eddba` — `feat(genius): report playback lifecycle to v2 server` (2026-09-23) on the older `genius-v2-migration` line.

Important:
- Do not assume `genius-v2-migration` is the currently flashed branch.
- The active/tested branch on 2026-09-30 is `feature/k10-lcd-auto-sleep`.

## Firmware baseline

K10 is currently the main ESP32 test firmware for the Genius device architecture.

Working behavior:
- Starts Genius-side device integration only after network is available.
- Existing AP/captive-portal provisioning remains available.
- Genius domain is configurable through provisioning.
- Device exposes the unified `genius_tool` path rather than maintaining a separate per-skill device whitelist.
- Radio, music, weather, news, earthquake, knowledge, utility, smart-home and alarm flows have been exercised through the device/Genius path.
- Alarm create/list/cancel tested.
- Local timezone behavior fixed: system epoch remains UTC; `TZ=WIB-7` is used for localtime/LCD.
- Wake-word barge-in during Genius media playback works.
- Device playback lifecycle is reported to Genius V2 via `SendDeviceEvent()`.
- Proactive EWS path was tested end-to-end.
- Buttons:
  - A: volume down; hold ~1 s wake.
  - B: volume up; hold ~3 s Wi-Fi provisioning.
- Alarm volume: 100.

## Audio / wake

- AFE: `1MIC_V251128`
- Wake models: `wn9_jarvis_tts`, `wn9_alexa`
- AEC: `FD_LOW_COST`
- VAD: WebRTC
- Board config: `CONFIG_BOARD_TYPE_DF_K10=y`

## Device-side media architecture

Current active branch uses the Lily-derived split network/playback architecture:
- `ProducerTask` performs HTTP reads.
- `WorkerTask` decodes/plays from the stream buffer.
- Media stream buffer: 128 KiB.
- Initial prebuffer: 32 KiB.
- HTTP read chunk: 4096 bytes.
- Finite media retains byte-range resume semantics.
- Genius live radio is identified only by the stable Genius media URL carrying `?live=1`.
- Live radio never uses Range resume.
- Live reconnect is bounded to 3 attempts.

### Live-radio recovery — verified 2026-09-30

Problem:
Genius live radio can experience upstream stalls, TLS read failures, or a clean end of the current HTTP/Ogg response. Treating that as finite-media completion caused radio to stop after a few minutes.

Implemented behavior:
1. Producer keeps the playback lifecycle alive when a marked Genius live stream ends or its HTTP read fails.
2. Producer drains the old compressed stream buffer before reopening the same stable Genius media URL.
3. Each successful live reconnect increments `live_stream_generation_`.
4. Worker detects the generation boundary and creates a fresh `OggDemuxer` before consuming bytes from the new Ogg logical stream.
5. Playback ID/audio lifecycle remains the same across reconnects.
6. This avoids concatenating a fresh Ogg logical stream into an old demuxer, which previously caused `Unsupported Opus packet duration`.

Relevant active-branch commits:
- `f7e0616` — reconnect marked Genius live radio streams.
- `97ef1e3` — track live media stream generations.
- `e426ff8` — reset Ogg demuxer across live radio reconnects.

Real-device validation:
- JAK FM started normally with ~35 KB prebuffer.
- Failure #1: ESP AES allocation/TLS read failure after ~230 KB; K10 reconnected 1/3, changed Ogg generation 0 -> 1, and continued playing.
- Failure #2: 15-second HTTP content receive timeout after ~4.14 MB; K10 reconnected 2/3, changed Ogg generation 1 -> 2, and continued playing.
- No `Unsupported Opus packet duration` after either generation reset.
- No notification playback completion/failure during those recoveries.
- Playback remained alive beyond 18 minutes in the captured test.
- Minimum SRAM observed ~10 KB. SRAM is tight, but the test recovered without reboot/OOM.

Conclusion:
The Lily-style producer reconnect pattern is valid for K10 when combined with an explicit fresh-Ogg generation boundary. Preserve this behavior unless later evidence shows a regression.

## Camera — latest important state

Camera work became a dedicated K10 experiment after severe outdoor overexposure.

Observed during debugging:
- Outdoor image could become almost completely white from overexposure.
- Camera backend used the DFRobot-compatible `esp32-camera` path.
- Logs included GC2145/RGB565/QVGA work and sensor-detection experiments.
- A tested camera configuration was eventually considered good enough indoors/outdoors and the user requested that experiment to be committed as a K10 baseline.
- Current active branch also contains K10 camera-to-Genius vision work.

Important:
- Do NOT return to the earlier “camera is simply broken outdoors” assumption without checking the active branch/current source.

## K10 case / enclosure — active item

Current physical task:
- User wants a printable/protective K10 case.
- An STL named `CAse.stl` was supplied and separated/prepared for a 3D-print shop.
- Currently waiting for the print shop to confirm whether they can print/use the prepared files.
- Do not redesign unnecessarily until the shop responds.

Alternatives already identified:
1. DFRobot official K10 protector/case.
2. Community CosmicBee K10 top/bottom snap-fit case.
3. Other community STL/3MF designs.

Cost decision:
- User remembers an original DFRobot case around Rp200k in an online shop.
- Compare the actual delivered original-case price against the print-shop quote before choosing.
- If 3D printing approaches the price of a ready-made original case, evaluate the original case instead of assuming printing is cheaper.

---

# 2. K10 ESP-CLAW EXPERIMENT — CURRENT STATE

Workspace:
`D:\ESP-PROJECTS\ACTIVE\k10-claw`

Branch:
`experiment/esp-claw-agent`

Origin:
branched from the earlier K10 Genius work / `genius-v2-migration`.

Purpose:
- Experiment with ESP-Claw as a natural-language “translator/agent” layer.
- Avoid requiring magic words such as explicitly saying “YouTube”.
- Examples motivating the experiment:
  - “putar surat Yasin”
  - “putar ayat kursi”
  - knowledge queries such as “apa itu tembiluk”

Architecture direction:
- XiaoZhi remains the voice I/O path.
- Genius remains the broader service/backend path.
- ESP-Claw is experimental and must NOT silently replace the known-good K10 baseline.

Toolchain:
- XiaoZhi 2.5.0 experiment required ESP-IDF 6.1.
- Build uses Ninja.
- Initial `claw_utils` manifest/path issue was repaired by moving modules into `claw_components` and using relative paths.
- Build succeeded.

Known included components:
- `claw_core`
- `claw_utils`
- `claw_event_router`
- `claw_manager`
- `claw_cap`
- `claw_skill`
- `http_reuse`

Example resource figures from a successful build:
- Flash code ~1.47 MB
- IRAM ~59.6 KB (~45%)
- DRAM ~55.7 KB (~31%)

Status:
EXPERIMENTAL. Keep separate from production K10 firmware unless explicitly promoted.

---

# 3. MINJI — CURRENT STATE

## Identity

Device: Minji
Board: ESP32-S3 `bread-compact-wifi-lcd`
Display: ST7735 1.8", 128×160

Repo:
`bapul-droid/minji-xiaozhi-2.5`

Primary branch:
`genius-device-core`

Known latest recorded HEAD:
`bbac9f2` — finalize provisioning & media playback.

Important historical commits:
- `e91f2c0` baseline
- `e38a87a` connect V2
- `b39fee8` play URL
- `fa2b219` dynamic skill proxy
- `8bee5e2` runtime
- `bbac9f2` finalize provisioning & media playback

Toolchain:
- ESP-IDF 5.5.5
- Historically/currently flashed via COM6 at last recorded checkpoint.

## LCD

ST7735 1.8", 128×160.

Rubell VN LCD pin mapping:
- SDA: GPIO47
- RES: GPIO45
- DC: GPIO40
- CS: GPIO41
- BLK: GPIO42

History:
- Earlier blank/white-line display issue was traced to socket/contact rather than a fundamental firmware/display incompatibility.
- UI concept includes blinking eyes.
- Indonesian language is active.

Planned LCD idle behavior:
- 75%
- 50%
- 25%
- 10%
- OFF

Requirement:
- LCD sleep must NOT disable wake word or Wi-Fi.

## Audio / Bluetooth

WROOM A2DP integration version recorded as V2.1.

I2S:
- GPIO15 -> BCLK
- GPIO16 -> WS
- GPIO7 -> DATA

UART:
- GPIO18 <- TX2
- GPIO3 -> RX2

Working behavior:
- Bluetooth connect/disconnect works.
- Volume control works.
- Reconnecting Bluetooth restores media.
- Internal radio can be muted with internal-speaker fallback behavior.
- Historical device volume tests included 45 and 75.
- Speaker noise was noted; media/radio locking/ducking remains an area to preserve carefully.

## Wake / VAD

Wake models:
- `wn9_jarvis_tts`
- `wn9_alexa`

AFE:
- `1MIC_V251128`

VAD:
- WebRTC

Important historical failure:
- “Minji bisu” on 2026-08-21:
  - wake worked
  - `no_speech=1.00`
  - listening repeatedly re-armed
- Do not confuse that historical failure with the current baseline unless reproduced.

## Battery / power

Battery:
- Li-ion 18650, 3100 mAh.

Recorded switch behavior:
- LEFT switch = active/valid.
- RIGHT switch reading considered invalid.

Example telemetry:
- GPIO11 ~3.1 V
- GPIO12 ~0.01 V
- `Charging=YES`

Battery discharge behavior has not been characterized as thoroughly as charging.

## Genius device-core behavior

Minji contains the device-side Genius V2 client integration.

Known implementation points:
- `genius_v2_client`
- `ActivationTask` waits for the V2 catalog (~10 s) before XiaoZhi protocol continuation.
- XiaoZhi MCP path uses `mcp_server.*` with `tools/call` and `tools/list`.
- Unified `genius_tool` direction replaces exposing many independent MCP tools.
- Dynamic skill proxy/runtime work exists in the commit history.
- Media URL playback and provisioning were completed in the known `bbac9f2` baseline.

Historical issues worth remembering:
- `INT_WDT` around `http_client` / `esp_tcp`
- media wake behavior
- VAD/TTS bleed
- heartbeat / ADC battery telemetry
- watchdog/crash reporting

## Minji live-radio follow-up

Minji has shown the same user-visible symptom as K10: radio audio stops while Minji itself remains alive.

Next device-side task:
- inspect Minji's current player implementation;
- recognize Genius live media via `?live=1`;
- reconnect the same stable Genius session URL on transient read failure/end;
- keep the playback lifecycle alive while reconnecting;
- reset/recreate Ogg demuxing for each fresh logical stream;
- use bounded reconnect attempts;
- preserve finite-media behavior.

Do not copy the K10 implementation blindly because Minji's player/task/buffer architecture may differ.

---

# 4. OTHER ESP32 DEVICES — CONTEXT ONLY

## XiaoZhi VN CAM

Separate ESP32-S3 CAM device. NOT Minji.

Workspace:
`D:\ESP-PROJECTS\ACTIVE\xiaozhi-vn-cam`

Upstream:
`TienHuyIoT/xiaozhi-esp32_vietnam`

Custom board:
`BOARD_TYPE_XIAOZHI_VN_CAM`

Hardware:
- OV5640 camera
- headless
- DVP camera
- `NoAudioCodecSimplexPdm`
- boot button toggles chat / Wi-Fi reset

Status:
- provisioning portal/domain displayed successfully.
- GPIO provisioning target remained unfinished at last checkpoint.

## Mochi / Chronchi ESP32-C3

Workspace:
`D:\ESP-PROJECTS\ACTIVE\genius-c3`

Branch:
`genius-v2-c3`

Baseline:
`4d275c9`

Safety tag:
`before-genius-v2-c3-port`

Important:
- ESP-IDF 5.5.5-dirty baseline.
- Do NOT accidentally use Minji/experimental IDF 6.1 assumptions.
- Preserve existing XiaoZhi AI/chat + Mochi/Chronchi modes.
- Genius is a shared device layer, NOT a third user-facing mode.
- Preserve native YouTubeAudioPoller Ogg/Opus player.

---

# 5. NEXT ACTIONS

K10:
1. Preserve the verified live-radio reconnect + Ogg-generation behavior.
2. Continue normal real-device testing; investigate SRAM only if evidence shows a failure.
3. Wait for the 3D-print shop response and compare its quote with a ready-made original case.
4. Keep ESP-Claw experimental unless explicitly promoted.

Minji:
1. Port the proven K10 live-radio recovery semantics after inspecting Minji's current player.
2. Preserve working LCD/audio/wake/device-core behavior.
3. Continue battery discharge characterization when relevant.
4. Do not mix Minji hardware with the separate XiaoZhi VN CAM device.

---

# 6. CONTINUITY / MIRROR RULE

When a new chat/table begins:
1. Read `CURRENT_STATE.md`.
2. Read `REFERENCES.md`.
3. Verify the active Git branch/current source before changing firmware.
4. Update this diary after a meaningful firmware/hardware milestone.

Storage rule:
- ESP32 Library copy and ESP32 Git copy are mirrors of the same logical document.
- Do not maintain divergent “Git version” and “Library version”.
- After a meaningful update, synchronize both copies with identical content.
- Apply the same mirror rule to `REFERENCES.md`.
