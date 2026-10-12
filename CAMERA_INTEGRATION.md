# K10 Camera integration â€” verified baseline (2026-10-12)

## Proven on physical K10
- Standalone workspace: `D:\ESP-PROJECTS\ACTIVE\k10-camera-test`
- Official DFRobot `unihiker_k10/examples/Photo/Photo.ino` compiled with `platform-unihiker` + Arduino framework.
- Standalone v2 successfully flashed and physically tested: fullscreen smooth live preview, A saves BMP to TF card, B shows saved BMP, B again clears canvas and resumes live preview; A can repeat.
- Green LED on during slow save, off when ready (user observation).
- Restored existing XiaoZhi launcher v0.16 from `k10-menu` build; physical test confirms Alexa/menu work. Its `esp_vfs_fat_sdspi_mount` still fails 0x108; do not call it fixed.

## Actual differences
- Official standalone uses `k10.begin()`, `initScreen`, `initBgCamerImage`, `creatCanvas`, `initSDFile()` (Arduino `SD.begin()`), `photoSaveToTFCard("S:/photo.bmp")`, and `canvasDrawImage`; full preview uses official camera display task.
- XiaoZhi launcher currently uses `EspVideo::Capture()` scheduled every 500 ms, LCD `SetK10CameraMode`, and ESP-IDF SDSPI mount on SPI3 sharing LCD. Photo uses `SaveK10Jpeg()`; SD fails `ESP_ERR_INVALID_RESPONSE`.
- Official Arduino variant default SPI: SCK12, MOSI21, MISO41, SS40. This does NOT prove that an already-initialized ESP-IDF LCD SPI3 bus is compatible with Arduino SD driver.
- Button mapping in XiaoZhi: A short = retake, B short = freeze/save, B long = exit. Preserve launcher navigation and exit behavior unless deliberately changed.

## Integration constraints
1. Keep XiaoZhi, Genius, launcher and stable workspace untouched during initial port; use `k10-menu` only.
2. Port complete standalone camera lifecycle (including display task and SD) with exclusive Camera mode; don't just copy `SD.begin()` into an ESP-IDF project (Arduino runtime dependency).
3. Do not initialize a second SPI bus on pins already owned by LCD. Determine SPI ownership and camera task shutdown/restart first.
4. Restore XiaoZhi LCD/UI and release camera/SD resources when leaving Camera.
5. Build before flashing; verify LCD/menu/Alexa after each step. Save a known-good flash image first.

## Current status
Research/port design only. No Camera integration patch applied or flashed yet.

## Critical source discovery (12 Oct 2026)
- `framework-arduinounihiker/libraries/SD/src/SD.h`: `SDFS::begin(uint8_t ssPin=SS, SPIClass &spi=SPI1, ...)` — **SD defaults to SPI1 (HSPI), not the main SPI object**.
- `libraries/SPI/src/SPI.cpp`: `SPIClass SPI1(HSPI)`; on ESP32-S3 `SPI1.begin()` without pins does NOT assign default pins for HSPI.
- `libraries/unihiker_k10/src/DFRobot_GT30L24A3W.cpp`, `GT_Font_Init()`: **`SPI1.begin(44,41,42,-1)`**; sets 20MHz. The SD driver subsequently uses this already-initialized SPI1 bus with SD CS40 (Arduino variant `SS=40`).
- Thus the working Arduino SD physical wiring is **SCLK GPIO44, MISO GPIO41, MOSI GPIO42, CS GPIO40**, NOT LCD SPI GPIO12/21/41. This explains why previous shared SPI3 and SPI2 pin guesses failed.
- ESP-IDF port should test a **dedicated SPI2_HOST** initialized with SCLK44/MISO41/MOSI42/CS40, while keeping LCD SPI3 untouched. Verify GPIO41 sharing with LCD does not conflict in practice; SD initialization only in exclusive Camera mode.
- `photoSaveToTFCard()` writes RGB565 BMP two bytes per pixel, locking SPI for each pixel; explains long save time and offers future buffered-write optimization.

