# ESP32 Devices — Current State

Last updated: 2026-09-30

This file is the compact hand-off for ESP32 device firmware work. Read it together with the relevant repository history before changing device code.

## K10 Genius

Repository: bapul-droid/k10-genius
Active branch: feature/k10-lcd-auto-sleep
Current tested architecture:
- Lily-derived split network/playback design: ProducerTask feeds a PSRAM stream buffer while WorkerTask decodes/plays.
- Media stream buffer: 128 KiB.
- Initial prebuffer: 32 KiB.
- HTTP read chunk: 4096 bytes.
- Genius live radio is recognized only by the stable Genius media URL carrying ?live=1.
- Finite media keeps its existing byte-range semantics; live radio never uses Range resume.
- Live radio reconnect is bounded to 3 attempts.

## K10 live-radio recovery — verified 2026-09-30

Problem:
Genius live radio can experience upstream stalls, TLS read failures, or a clean end of the current HTTP/Ogg response. Treating that as finite-media completion caused radio to stop after a few minutes.

Implemented behavior:
1. Producer keeps the playback lifecycle alive when a marked Genius live stream ends or its HTTP read fails.
2. Producer drains the old compressed stream buffer before reopening the same stable Genius media URL.
3. Each successful live reconnect increments live_stream_generation_.
4. Worker detects the generation boundary and creates a fresh OggDemuxer before consuming bytes from the new Ogg logical stream.
5. Playback ID/audio lifecycle remains the same across reconnects.
6. This avoids concatenating a fresh Ogg logical stream into an old demuxer, which previously caused Unsupported Opus packet duration.

Relevant commits on feature/k10-lcd-auto-sleep:
- f7e0616 reconnect marked Genius live radio streams
- 97ef1e3 track live media stream generations
- e426ff8 reset Ogg demuxer across live radio reconnects

Real-device validation:
- JAK FM started normally with ~35 KB prebuffer.
- Failure #1: esp-aes allocation failure / TLS read -132 after ~230 KB.
- K10 logged reconnect 1/3, reopened HTTPS, then Ogg generation 0 -> 1 and continued playing.
- Failure #2: 15-second HTTP content receive timeout after ~4.14 MB.
- K10 logged reconnect 2/3, reopened HTTPS, then Ogg generation 1 -> 2 and continued playing.
- No Unsupported Opus packet duration after either generation reset.
- No notification playback completion/failure during those recoveries.
- Playback remained alive beyond 18 minutes in the captured test.
- Minimum SRAM observed ~10,160 bytes. SRAM is tight, but the test recovered without reboot/OOM.

Conclusion:
The Lily-style producer reconnect pattern is valid for K10 when combined with an explicit fresh-Ogg generation boundary. Preserve this behavior unless later evidence shows a regression.

## Minji follow-up

Minji has shown the same user-visible radio symptom: audio stops while Minji itself remains alive.

Minji should receive the same semantics after inspecting its current player implementation:
- recognize Genius live media via ?live=1;
- reconnect the same stable Genius session URL on transient read failure/end;
- keep the playback lifecycle alive while reconnecting;
- reset/recreate Ogg demuxing for each fresh logical stream;
- use bounded reconnect attempts;
- preserve finite-media behavior.

Do not copy K10 implementation blindly because Minji's player/task/buffer architecture may differ.

## Handoff rule

For a new ESP32 conversation, read this file first, then inspect the active branch and current source before patching. Never assume an older branch is the device currently flashed.
