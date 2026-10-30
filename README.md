# raindesk

Rain on the glass of your monitor.

raindesk draws water drops in a transparent, click-through window that sits on
top of everything. Each drop bends the light of whatever is really underneath it,
so it looks like the rain is between your eyes and the pixels. You can keep using
your desktop, browser and games normally while it rains.

Rain lands, beads up and merges. Drops cling to the glass until they are heavy
enough to let go, then run down, fast or slow depending on size. They meander,
follow earlier wet paths, leave beads and streaks behind, and wipe the fine mist
off the glass as they go. A wiper can sweep the screen clean.

## Requirements

- Windows 10 version 2004 (May 2020) or newer, or Windows 11.
- A DirectX 11 GPU.
- To build: Visual Studio 2022 with the **Desktop development with C++** workload
  (includes CMake and the Windows SDK).

## Build

Double-click `build.bat`. It finds Visual Studio, configures CMake and builds
`build\Release\raindesk.exe`.

You can also open this folder in Visual Studio (**File > Open > Folder**) and
build the `raindesk` CMake target.

## Use

Run `build\Release\raindesk.exe`. The glass starts dry and fills up over a few
minutes.

A blue raindrop icon appears in the system tray (it may be hidden under the **^**
overflow arrow).

- **Double-click** the icon to open **Settings**.
- **Right-click** it for the menu.

### Settings

| Setting | What it does |
| --- | --- |
| Rain amount | How many raindrops hit the glass. |
| Fine mist | How much tiny misting settles between the drops. |
| Glass grip (friction) | How hard the glass holds on to drops. Higher means bigger drops build up before they run, and running drops stutter more. |
| Running speed | Scales how fast running drops travel. |
| Smallest / largest raindrop | Size range of raindrops when they land (diameters assume a 27" 1440p screen). |
| Streak life | How long the thin trails behind running drops last before they break into droplets and drain away. |
| Wind | Steady wind along the glass, left or right. Rain hits at an angle and skids, and drops run at a slant. |
| Gustiness | How much the wind gusts and lulls around the Wind setting. Gusts also bring bursts of heavier rain. |
| Pause rain | Stops new rain. Water already on the glass keeps behaving. |

Settings apply immediately and are saved in `raindesk.ini` next to the exe.

### Menu

- **Settings...**
- **Wipe the glass**: runs the wiper.
- **Auto wipe**: off, or every 1, 2, 5, 10 or 30 minutes.
- **Dry the glass instantly**: removes all water at once.
- **Pause rain**
- **Hide rain**: takes the overlay off the screen until you untick it.
- **Save screenshot**
- **Quit**

### Hotkeys

| Keys | Action |
| --- | --- |
| Ctrl+Alt+W | Run the wiper |
| Ctrl+Alt+Up / Down | Rain harder / softer |
| Ctrl+Alt+S | Save a screenshot with the rain |
| Ctrl+Alt+Q | Quit |

### Screenshots

Normal screenshot tools cannot see raindesk. The overlay hides itself from
screen capture, because that is how it reads the real desktop underneath without
seeing its own drops. Use **Ctrl+Alt+S** or **Save screenshot** instead. Images
go to `screenshots\` next to the exe.

## Limitations

- Primary monitor only.
- Games in exclusive fullscreen draw over everything, so the rain cannot appear
  over them. Borderless or windowed fullscreen works.
- The secure desktop (UAC prompts, the lock screen) cannot be captured, so drops
  keep showing the last image they saw until it goes away.

## Tuning the look

`rain.hlsl` is copied next to the exe on every build. raindesk recompiles it
whenever the file changes, so you can edit the copy in `build\Release\` and see
the result while it runs. If an edit doesn't compile, the last working version
keeps running and the error goes to `raindesk.log`. Copy changes you want to keep
back to `shaders\rain.hlsl`.

## Troubleshooting

Everything raindesk notices goes into `raindesk.log` next to the exe, including
fatal errors, shader compile errors and capture problems.

- **"Excluding the overlay from capture" failed**: Windows is older than 10
  version 2004.
- **No drops appear**: check the log for `DuplicateOutput` errors. Remote desktop
  sessions and some virtual machines don't support desktop capture.
- **A hotkey does nothing**: another app already owns it. The log lists any
  hotkey that couldn't be registered.

## Project layout

| Path | Contents |
| --- | --- |
| `src/main.cpp` | Window, Direct3D 11, desktop capture, render passes, hotkeys, main loop |
| `src/rain_sim.h`, `src/rain_sim.cpp` | The rain simulation and the wiper |
| `src/controls.h`, `src/controls.cpp` | Tray icon, menu, settings window, `raindesk.ini` |
| `src/resource.h` | Resource ids |
| `res/` | App icon (`raindesk.ico`, a PNG copy, and the `.rc` that embeds it in the exe) |
| `shaders/rain.hlsl` | All GPU shaders: drop splats, mist, composite (refraction and shading), wiper |
| `docs/how-it-works.md` | How the overlay, simulation and rendering work, and the physics behind the drops |
| `CMakeLists.txt`, `build.bat` | Build |
