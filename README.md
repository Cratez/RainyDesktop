# raindesk

Rain on the glass of your monitor. A transparent, click-through overlay that
refracts the real desktop through water drops.

## Build and run (Windows 10 2004+ / Windows 11)

1. Double-click `build.bat` (needs Visual Studio 2022 with "Desktop development with C++").
   Or open this folder in Visual Studio (File > Open > Folder) and build the CMake target.
2. Run `build\Release\raindesk.exe`.
3. Hotkeys: **Ctrl+Alt+Q** quit, **Ctrl+Alt+W** wipe the glass,
   **Ctrl+Alt+Up / Down** rain harder / softer (each press is 1.5x),
   **Ctrl+Alt+S** save a screenshot with the rain (to `screenshots\` next to the exe;
   normal screenshots cannot see the overlay).

`rain.hlsl` next to the exe is recompiled every time you save it, so the look
can be tuned while it runs. Problems are written to `raindesk.log` next to the exe.

## How it works

- A topmost `WS_EX_LAYERED | WS_EX_TRANSPARENT` window covers the primary
  monitor, so all input passes through to whatever is underneath.
- DirectComposition presents a premultiplied-alpha swap chain: pixels without
  water are fully transparent.
- `src/rain_sim.cpp` simulates the drops on the CPU: rain lands and merges,
  drops past a size limit break loose and run down with stick-slip motion,
  leaving droplet trails and wiping the fine mist off the glass.
- The GPU splats drops into a water-thickness texture (and mist into a
  persistent one), then `PSComposite` turns thickness into a surface normal
  and refracts the desktop through it.
- Desktop Duplication captures the screen each frame. The overlay excludes itself
  from capture (`WDA_EXCLUDEFROMCAPTURE`), so drops refract the real desktop.

Works over normal and borderless-fullscreen apps. Games in exclusive fullscreen
draw over everything and cannot be overlaid.
