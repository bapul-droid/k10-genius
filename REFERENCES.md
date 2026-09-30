# ESP32 PROJECT — REFERENCES

Last updated: 2026-09-30
Companion file: `CURRENT_STATE.md`

> Stable reference sheet for ESP32 hardware/firmware work.
> Server-side Genius V2 details belong in the separate Genius project.

---

# 1. WORKSPACE CONVENTIONS

Primary Windows ESP workspace root:
`D:\ESP-PROJECTS\ACTIVE`

Known serial ports:
- Minji: COM6 at latest recorded checkpoint.
- Historical other/right-side port: COM10.

---

# 2. K10 REFERENCES

## Main repository

GitHub:
`bapul-droid/k10-genius`

Active branch:
`feature/k10-lcd-auto-sleep`

Older architecture branch:
`genius-v2-migration`

Historical checkpoint:
`80eddba` — playback lifecycle reporting to Genius V2.

Current live-radio recovery commits:
- `f7e0616` — reconnect marked Genius live radio streams.
- `97ef1e3` — track live media stream generations.
- `e426ff8` — reset Ogg demuxer across live radio reconnects.
- `892297c` — reset live-radio retry budget after successful recovery.

Board:
DFRobot UNIHIKER K10

Board sdkconfig:
`CONFIG_BOARD_TYPE_DF_K10=y`

## Audio

AFE:
`1MIC_V251128`

Wake:
- `wn9_jarvis_tts`
- `wn9_alexa`

AEC:
`FD_LOW_COST`

VAD:
WebRTC

## Current media implementation

Lily-derived producer/consumer architecture:
- Producer: HTTP/network.
- Worker: decode/play.
- Stream buffer: 128 KiB.
- Initial prebuffer: 32 KiB.
- HTTP read chunk: 4096 bytes.
- Genius live URL marker: `?live=1`.
- Live reconnect limit: 3.
- Fresh Ogg logical stream requires a fresh Ogg demuxer generation.

## Controls

- Button A: volume down; hold ~1 second for wake.
- Button B: volume up; hold ~3 seconds for Wi-Fi provisioning.
- Alarm volume: 100.

## Camera

Relevant implementation/debug terms:
- DFRobot-compatible `esp32-camera` backend
- RGB565
- QVGA
- GC2145 experiments
- sensor PID detection
- PSRAM DMA behavior
- indoor/outdoor exposure tuning
- Genius vision route on active branch

Important reference rule:
Check the active branch/current source before redoing camera tuning.

## K10 enclosure

Official hardware family:
DFRobot / UNIHIKER K10

Official protector identifier previously found:
`FIT1020`

Known community alternative:
CosmicBee K10 case:
- top + bottom
- snap-fit
- screen close to flush
- openings for K10 hardware
- PETG-tested by designer
- later revision aimed to avoid button support

Previously supplied design:
`CAse.stl`

Current workflow:
prepared/separated file -> print-shop feasibility check -> quote comparison -> choose print vs ready-made case.

---

# 3. K10 ESP-CLAW REFERENCES

Workspace:
`D:\ESP-PROJECTS\ACTIVE\k10-claw`

Branch:
`experiment/esp-claw-agent`

Upstream ESP-Claw workspace:
`D:\ESP-PROJECTS\ACTIVE\esp-claw-upstream`

Upstream project:
Espressif `esp-claw`

Relevant components:
- `claw_core`
- `claw_utils`
- `claw_event_router`
- `claw_manager`
- `claw_cap`
- `claw_skill`
- `http_reuse`

Toolchain note:
XiaoZhi 2.5.0 experiment used ESP-IDF 6.1.

Do not generalize this IDF requirement to all K10/Minji branches.

---

# 4. MINJI REFERENCES

## Main repository

GitHub:
`bapul-droid/minji-xiaozhi-2.5`

Branch:
`genius-device-core`

Known recorded HEAD:
`bbac9f2`

Commit landmarks:
- `e91f2c0` baseline
- `e38a87a` connect V2
- `b39fee8` play URL
- `fa2b219` dynamic skill proxy
- `8bee5e2` runtime
- `bbac9f2` finalize provisioning & media playback

ESP-IDF:
5.5.5

## Board/display

Board:
`bread-compact-wifi-lcd`

LCD:
ST7735 1.8", 128×160

LCD pins:
- SDA = GPIO47
- RES = GPIO45
- DC = GPIO40
- CS = GPIO41
- BLK = GPIO42

## Audio / WROOM A2DP V2.1

I2S:
- GPIO15 = BCLK
- GPIO16 = WS
- GPIO7 = DATA

UART:
- GPIO18 <- TX2
- GPIO3 -> RX2

## Wake

AFE:
`1MIC_V251128`

Wake models:
- `wn9_jarvis_tts`
- `wn9_alexa`

VAD:
WebRTC

## Battery

Cell:
18650 Li-ion, 3100 mAh

Recorded ADC examples:
- GPIO11 ~3.1 V
- GPIO12 ~0.01 V

Recorded charging state:
`Charging=YES`

Switch:
- LEFT = valid/active
- RIGHT = invalid reading

## Verified live-radio recovery

Minji now implements and has physically verified the same live-radio recovery semantics as K10:
- Genius marker `?live=1`.
- stable session URL reconnect.
- 3 consecutive reconnect failures maximum; successful live reads reset the retry budget.
- playback lifecycle remains alive.
- fresh Ogg demuxer per fresh logical stream.
- finite media semantics remain unchanged.

Relevant Minji commits:
- `fdee8eb` — live radio recovery.
- `947fb13` — live stream generation tracking.

## MultiNet6 custom wake assets

Verified Minji wake baseline:
- model: MultiNet6 Chinese / `mn6_cn`
- runtime model: `rnnt_ctc_1.0`
- wake command: `min ji`
- recognition duration: 3000 ms
- sensitivity: 20 / runtime threshold `0.200000` (generator default)
- repo-local binary path: `custom_assets/assets.bin`

Build behavior:
- Minji board config selects custom assets.
- `main/CMakeLists.txt` forces repo-root `custom_assets/assets.bin` for `CONFIG_BOARD_TYPE_MINJI_S3_LCD`, preventing an old local sdkconfig from silently regenerating/flashing MultiNet5.
- Missing custom Minji assets is a build error rather than a silent fallback.
- `assets.bin` is local/generated and is not stored in Git; copy it separately to a fresh clone.

Relevant commits:
- `1b2df44` — select custom assets.
- `1dd006a` — correct custom-assets path.
- `aefb896` — always flash tested MultiNet6 assets for Minji.

---

# 5. XIAOZHI VN CAM REFERENCES

Purpose:
Separate headless ESP32-S3 camera device.

Workspace:
`D:\ESP-PROJECTS\ACTIVE\xiaozhi-vn-cam`

Upstream:
`TienHuyIoT/xiaozhi-esp32_vietnam`

Custom board symbol:
`BOARD_TYPE_XIAOZHI_VN_CAM`

Camera:
OV5640 / DVP

Audio:
`NoAudioCodecSimplexPdm`

Never treat this board as Minji or borrow Minji LCD assumptions.

---

# 6. MOCHI / CHRONCHI ESP32-C3 REFERENCES

Workspace:
`D:\ESP-PROJECTS\ACTIVE\genius-c3`

Branch:
`genius-v2-c3`

Baseline:
`4d275c9`

Safety tag:
`before-genius-v2-c3-port`

Upstream:
`Franklnir/xiaozhi-x-Chronchi-esp32c3-mini-`

Toolchain:
ESP-IDF 5.5.5-dirty at recorded baseline.

Architecture constraint:
- XiaoZhi AI/chat and Mochi/Chronchi notification/features remain the two modes.
- Genius device integration is shared infrastructure, not a third mode.

---

# 7. DEVICE ↔ GENIUS BOUNDARY

ESP32 side may contain:
- device identity
- network/provisioning
- Genius client/channel
- unified tool call transport
- playback lifecycle
- device telemetry
- media playback
- board hardware actions

Keep in Genius project:
- server routing logic
- skill registry/catalog internals
- Telegram natural-language routing
- server-side Home Assistant integration
- OpenKey backend
- server console/storage/proxy

---

# 8. UPDATE / MIRROR POLICY

After a meaningful milestone, update BOTH files when appropriate.

`CURRENT_STATE.md`:
What is true now? What works? What is broken? What are we waiting for? What is next?

`REFERENCES.md`:
Where is it? Which repo/branch/workspace? Which pins/toolchain/commit/link/component names should remain easy to recover?

Storage rule:
- Git and Library copies are mirrors.
- Synchronize identical content after meaningful changes.
- Do not create a separate abbreviated diary in one location.
- Do not turn either file into a full chronological chat log.
