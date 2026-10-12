# K10 XiaoZhi app launcher — experimental worktree

Baseline: 37ad771 (stable camera indoor/outdoor, LCD auto-sleep, robot GIF, SerpAPI vision).
Source reference: D:\ESP-PROJECTS\ACTIVE\k10-muse\esp32\components\muse\muse_menu.c and muse_input.c.

## Agreed controls
- Hold A while launcher closed: open menu.
- Tap A while launcher open: next item, wrap around.
- Tap B while launcher open: select item.
- Select Back to go up one level; select Exit to return to XiaoZhi robot GIF.
- While launcher closed, retain existing XiaoZhi volume/media/Wi-Fi button behavior except the agreed A-hold shortcut.
- Never activate microSD in this phase: SD CS GPIO39 conflicts with audio I2S DIN GPIO39.

## First stage
- Launcher list: XiaoZhi, Recorder (placeholder), Camera (placeholder), Tools (placeholder), Exit.
- GUI must use the existing LVGL/display lock and restore the robot GIF when closing.
- Menu must not initialize new camera, audio, SD, or network drivers.
- Preserve auto-sleep and wake behavior; prevent menu selection from also changing volume.
- Do not flash until build and review are complete.

## Current state
Worktree created on feature/k10-app-launcher. Implementation and build pending.
