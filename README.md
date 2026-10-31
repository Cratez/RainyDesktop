# RainyDesktop

Rain on the glass of your monitor.

RainyDesktop draws water drops in a transparent, click-through window that sits on
top of everything. Each drop bends the light of whatever is really underneath it,
so it looks like the rain is between your eyes and the pixels. You can keep using
your desktop, browser and games normally while it rains.

Rain lands, splashes, beads up and merges; now and then a hard splash bursts and
throws drops off in every direction, which skid sideways and curve down the
pane. Drops cling to the glass until they are heavy enough to let go, then run
down, fast or slow depending on size, each with its own slightly ragged
outline. They meander, follow earlier wet paths, leave beads and thin trails
behind, and wipe the fine mist off the glass as they go. Wind slants the rain
and pushes the drops around, and its gusts swing both ways. Behind the glass,
rain falls past the window, showers come and go, and lightning lights up every
drop; close thunder rattles the pane and shakes drops loose. The light follows
the sun: grey daylight, a blue dusk, and at night the warm glow of streetlit
cloud with a tiny streetlight in every drop. Optional sound plays the taps of
drops on the pane, the rain outside heard through the glass, the gutter
dripping, the wind, the odd car swishing past and the thunder that follows each
flash. Hold Ctrl+Alt and move the mouse to wipe a finger through the water, or
let a wiper sweep the screen clean.

Everything is simulated from the physics of water on glass and of falling rain,
and everything can be tuned in the settings. The second half of this file
explains how it all works, with the maths.

## Contents

- [Requirements](#requirements)
- [Build](#build) ([signing](#signing))
- [Use](#use): [settings](#settings), [menu](#menu), [hotkeys](#hotkeys), [screenshots](#screenshots)
- [Limitations](#limitations), [Tuning the look](#tuning-the-look), [Troubleshooting](#troubleshooting)
- [How it works](#how-it-works)
  1. [The overlay](#1-the-overlay)
  2. [Water on the glass](#2-water-on-the-glass)
  3. [Beyond the glass: showers, lightning, daylight, rain outside](#3-beyond-the-glass)
  4. [Rendering](#4-rendering)
  5. [Sound](#5-sound)
  6. [References](#6-references)
- [Project layout](#project-layout)

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

### Signing

Every build signs `raindesk.exe` with your own code-signing certificate
(`sign.bat`, run by CMake after the link), so you can tell later whether the
file has been changed. Check it any time with:

```
sign.bat /check [path\to\raindesk.exe]
```

It prints **OK** if the exe is signed with your certificate and unchanged
since, or says it is **MODIFIED**, **NOT signed**, or signed by someone else.
Windows checks the signature's hash of the file, and any change to it breaks
the match; the check also insists on your certificate's thumbprint, because
anyone could strip the signature and sign the file with their own.

- The certificate is a self-signed one in your personal store
  (`Cert:\CurrentUser\My`), made once with
  `New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=raindesk" -CertStoreLocation Cert:\CurrentUser\My -KeyExportPolicy NonExportable`.
  Its thumbprint is at the top of `sign.bat`; set `RAINDESK_SIGN_THUMBPRINT`
  to use another.
- CMake runs `sign.bat` on every build, relinked or not. A freshly linked exe
  is signed; one already signed with your certificate and unchanged is left
  alone. It never signs over an exe that was changed after signing or signed by
  someone else, since that would vouch for it: the build stops and asks you to
  delete the exe and build again.
- On a machine without that certificate, or while raindesk is running and holds
  the exe open, the exe is left unsigned and the build carries on; the next
  build signs it. To turn signing off, configure with `-DRAINDESK_SIGN=OFF`.
- Other PCs (and SmartScreen) don't trust a self-signed certificate, so this
  proves the file is the one you built, not who you are to anyone else. Windows
  shows the signature as untrusted unless you add the certificate to your
  Trusted Root store; the hash is checked either way.
- The signature covers everything built into the exe, shaders included, but not
  `raindesk.ini` or a `rain.hlsl` you put next to it for tuning.
- The signature isn't timestamped, which only matters for trusting it after the
  certificate expires, and would need the network on every build.

## Use

Run `build\Release\raindesk.exe`. The glass starts dry and fills up over a few
minutes.

The exe is all you need: copy it anywhere, or share it on its own. It keeps
`raindesk.ini`, `raindesk.log` and `screenshots\` next to itself.

A blue raindrop icon appears in the system tray (it may be hidden under the **^**
overflow arrow).

- **Double-click** the icon to open **Settings**.
- **Right-click** it for the menu.

### Settings

Settings apply immediately and are saved in `raindesk.ini` next to the exe (the
key for each is in brackets). **Defaults** puts them all back.

**Rain**

| Setting | What it does |
| --- | --- |
| Rain amount (`rain`) | How hard it rains: how many raindrops hit the glass, and how heavy the rain outside is. |
| Fine mist (`mist`) | How much tiny misting settles between the drops. |
| Smallest / largest raindrop (`minDrop`, `maxDrop`) | Size range of raindrops when they land (diameters assume a 27" 1440p screen). |
| Splashes (`splash`) | How often big raindrops splash when they hit: a sheet thrown downward and a spray of micro droplets, then it pulls back into a bead, breaks into several drops, or shoots off down the glass with a trail. 0 means drops just appear. |
| Splashes that burst (`burst`) | How often a hard splash bursts and throws three to six drops off in every direction, sideways and even up; they skid along the glass and curve down as their weight takes over. Big raindrops and drops landing in water already on the glass burst most. About one every two seconds at the defaults; off means never. |

**On the glass**

| Setting | What it does |
| --- | --- |
| Glass grip (`friction`) | How hard the glass holds on to drops. Higher means bigger drops build up before they run, and running drops stutter more. |
| Running speed (`runSpeed`) | Scales how fast running drops travel. |
| Speed to leave a trail (`trailSpeed`) | How fast a running drop must go before it leaves a thin trail instead of a line of beads. Lower means more trails. |
| Streak life (`streakLife`) | How long those thin trails last before they break into droplets and drain away. |
| Bead-up speed (`beadSpeed`) | How quickly a broken trail pulls itself up into droplets once it has pinched apart. |
| Refraction (`refraction`) | How strongly the drops bend what is behind them. Higher shows a wider, more shrunken view in each drop. |

**Weather**

| Setting | What it does |
| --- | --- |
| Wind (`wind`) | Steady wind, left or right. Rain hits at an angle and skids, drops run at a slant, and the rain outside falls at a slant. |
| Gustiness (`gusts`) | How much the wind varies around the Wind setting: it swells and lulls, and swings, so now and then the water is pushed the other way for a moment (about 7% of the time at wind 1 and gustiness 1x). With no wind at all, gusts nudge the water both ways. Gusts also bring bursts of heavier rain. |
| Showers come and go (`weather`) | How much the rain builds and eases on its own over a minute or two. Off keeps it steady at the rain amount. |
| Rain outside (`outside`) | Rain falling past the window, behind the glass, slightly out of focus. In gusty wind, sheets of heavier rain sweep past. Off hides it. |
| Daylight (`daylight`) | **auto** (the default, the left end of the slider) follows the sun for your clock and the region set in Windows. Otherwise from night (0) to day (1). It changes the light in the drops and the rain outside, not your desktop. |

**Storm**

| Setting | What it does |
| --- | --- |
| Lightning (`lightning`) | How often lightning flashes (about one every 25 s at 1x, more in the heavy parts of a shower). Off by default. |
| Flash brightness (`flash`) | How bright the flashes are, on the screen and in the drops. Flashes stand out more at night. |
| Thunder shakes the glass (`shake`) | How hard close thunder (within 2 km or so) shakes the window: drops that were nearly ready to run let go together, and with sound on the pane rattles. Off turns both off. |

**Sound** (off by default)

| Setting | What it does |
| --- | --- |
| Volume (`volume`) | Master volume. Off closes the audio device. |
| Drops on the glass (`glassSound`) | Taps of raindrops hitting the pane, panned to where they land, no two alike. Splashes crackle as their droplets land again. |
| Rain outside (`outsideSound`) | Rain falling on the street, leaves and roofs outside: a soft wash with a patter of separate drops near and far, and now and then a bubble ringing in a puddle. |
| Thunder (`thunder`) | Thunder after each flash, later and darker the further away the strike. |
| Wind (`windSound`) | Wind that swells and dies with the gusts; in a gale it whistles at the window frame. Silent in calm weather. |
| Gutter dripping (`drips`) | The gutter dripping into puddles: faster the harder it rains, slowing to a stop a couple of minutes after the rain does. |
| Cars passing (`cars`) | Now and then a car swishes past on the wet street, about one a minute by day at 1x, far fewer at night. Half by default. |
| Window open (`windowOpen`) | **shut** (the default): everything outside is heard through the glass, which muffles the highs. Open lets it in bright and a little louder. |

Plus **Finger wipe**: hold Ctrl+Alt and move the mouse to drag a fingertip
through the water. It clears a path about 14 mm wide, pushes the water ahead of
it in a bulge and spills the rest off its sides; let go (or stop) and the bulge
stays behind as a drop, usually heavy enough to run. Later drops follow the wet
path it leaves. On keyboards where AltGr types characters, AltGr counts as
Ctrl+Alt; untick it if that gets in the way.

Plus **Pause rain**, which stops new rain (and lightning); water already on the
glass keeps behaving.

> Lightning flashes the whole screen briefly, several times in quick
> succession. If flashing light bothers you, leave **Lightning** off or turn
> **Flash brightness** down.

### Menu

- **Settings...**
- **Wipe the glass**: runs the wiper.
- **Auto wipe**: off, or every 1, 2, 5, 10 or 30 minutes.
- **Dry the glass instantly**: removes all water at once.
- **Pause rain**
- **Hide rain**: takes the overlay off the screen (and silences it) until you untick it.
- **Save screenshot**
- **Quit**

### Hotkeys

| Keys | Action |
| --- | --- |
| Ctrl+Alt+W | Run the wiper |
| Ctrl+Alt+Up / Down | Rain harder / softer |
| Ctrl+Alt+S | Save a screenshot with the rain |
| Ctrl+Alt+Q | Quit |
| Hold Ctrl+Alt, move the mouse | Wipe a finger through the water (if **Finger wipe** is ticked) |

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
- Drops refract the desktop, not the rain outside or the lightning drawn by
  raindesk itself.

## Tuning the look

The shaders (`shaders\rain.hlsl`) are built into the exe and compiled when it
starts. To tune them while raindesk runs, copy `shaders\rain.hlsl` next to the
exe: a `rain.hlsl` there takes the place of the built-in shaders, and raindesk
recompiles it whenever you save it. If an edit doesn't compile, the last working
version keeps running and the error goes to `raindesk.log` (at startup, a copy
that doesn't compile falls back to the built-in shaders). Delete the copy to go
back to the built-in shaders. Copy changes you want to keep back to
`shaders\rain.hlsl` and rebuild; each build clears out the copy in
`build\Release\`.

## Troubleshooting

Everything raindesk notices goes into `raindesk.log` next to the exe, including
fatal errors, shader compile errors and capture problems.

- **"Excluding the overlay from capture" failed**: Windows is older than 10
  version 2004.
- **No drops appear**: check the log for `DuplicateOutput` errors. Remote desktop
  sessions and some virtual machines don't support desktop capture.
- **A hotkey does nothing**: another app already owns it. The log lists any
  hotkey that couldn't be registered.
- **No sound**: turn **Volume** up (it starts off). raindesk plays through the
  default output device and retries every few seconds if there is none.

---

# How it works

raindesk has five parts:

| Part | Where | What it does |
| --- | --- | --- |
| Overlay | `src/main.cpp` | A see-through, click-through window that can read the desktop beneath itself. |
| Water on the glass | `src/rain_sim.cpp` | Where every drop, trail and speck of mist is, frame by frame. |
| Beyond the glass | `src/sky.cpp` | Showers, lightning, daylight, and the rain falling outside. |
| Rendering | `shaders/rain.hlsl` | Turns all of that into refracting water, streaks and flashes on the GPU. |
| Sound | `src/sound.cpp` | Synthesises the taps, the rain outside, the gutter, wind, cars and thunder as it plays. |

## 1. The overlay

### A window you can see but not touch

raindesk creates one borderless popup window covering the primary monitor, with
these extended styles:

| Style | Why |
| --- | --- |
| `WS_EX_TOPMOST` | Above normal windows. A one-second timer re-asserts it, because the taskbar and other topmost windows can get above it. |
| `WS_EX_LAYERED` + `WS_EX_TRANSPARENT` | All mouse input passes straight through to whatever is underneath. |
| `WS_EX_NOACTIVATE`, `WS_EX_TOOLWINDOW` | Never takes focus, never appears in the taskbar or Alt+Tab. |
| `WS_EX_NOREDIRECTIONBITMAP` | No GDI surface; content comes from DirectComposition. |

Pixels reach the screen through a DirectComposition visual holding a flip-model
swap chain with **premultiplied alpha**. Pixels with nothing on them are fully
transparent, so the desktop shows through untouched.

### Seeing what is underneath

Refraction needs the image behind each drop. raindesk captures the primary
monitor every frame with **DXGI Desktop Duplication** on the GPU that drives
that monitor, and copies the frame into a texture with a full mip chain. The
mips give cheap blur for out-of-focus refraction.

The capture would normally include raindesk's own window, which would feed the
drops back into themselves. `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)`
(Windows 10 2004+) hides the window from every capture API, so the capture sees
the real desktop. The side effect is that ordinary screenshots can't see the
rain either. That is why raindesk has its own screenshot command, which renders
desktop plus rain offscreen and saves a PNG through WIC.

When capture is lost (display mode change, secure desktop, fullscreen
switches), raindesk keeps the last good frame and recreates the duplication
every 500 ms until it works again.

### Threads and the frame loop

- The **window thread** owns every window (overlay, tray, settings) and only
  pumps messages. Dragging the settings window or opening the tray menu puts it
  in a modal loop, which is why nothing else runs on it. It hands the render
  thread a copy of the settings under a mutex, and wipe, dry and screenshot
  requests as atomic flags.
- The **render thread** runs the simulation and draws.
- The **sound thread** (only while the volume is above zero) synthesises audio
  and feeds it to the sound card.

The swap chain allows only one queued frame (`SetMaximumFrameLatency(1)` and its
waitable object), so the refracted desktop is never more than a frame old.
Each frame on the render thread:

1. Wait until the compositor can take a new frame.
2. Apply the settings and start an auto wipe if one is due.
3. `RainSim::Update(dt)`: the water on the glass.
4. `Sky::Update(dt)`: showers, lightning, the shaking from thunder, daylight,
   the rain outside. Every 10 s the render thread also tells the sky the date
   and time and roughly where you are, for the sun.
5. Hand this frame's raindrop hits, the rain rate, the wind, the daylight and
   any thunder to the sound. The finger (Ctrl+Alt and the mouse) is read on
   the render thread too, with `GetAsyncKeyState` and `GetCursorPos`, because
   the overlay never takes mouse input.
6. `Render()`: capture the desktop, run the GPU passes, present with vsync.

### Units

The simulation works in screen pixels. Sizes are written for a 1440p screen and
scaled by $s = \text{height} / 1440$. On a 27" 16:9 panel (33.6 cm tall) one
1440p pixel is about **0.233 mm**, which is how the settings show drop sizes
in millimetres and how physical lengths and speeds below turn into pixels.

## 2. Water on the glass

`src/rain_sim.h`, `src/rain_sim.cpp`

Each frame the simulation produces three lists of **splats**, ellipses with a
centre, radii and a water height, which the GPU turns into a water surface:

- **drops**: every drop and trail on the glass, redrawn from scratch each frame;
- **mist**: new tiny droplets, stamped once into a persistent texture;
- **erase**: where water was wiped off that texture this frame.

### Two kinds of water

Tracking every droplet is expensive, so water is split in two (as in Lucas
Bebber's rain experiments):

- **Drops** are simulated individually: position, radius, velocity, shape.
  There are a few thousand at most, found through a uniform spatial grid
  (`BuildGrid`, `ForNeighbors`).
- **Mist** is anything smaller than 1.6 px in radius. It is never simulated:
  it is stamped into a texture that dries over about a minute, and running
  drops and the wiper erase it where they pass.

### The glass

The pane has two properties on a grid of 6 px cells:

- **Affinity** $a \in [0, 1]$: fixed value noise standing in for dirt and
  coatings. High spots grip water harder and pull running drops toward them.
- **Wetness** $w \in [0, 1]$: how recently each cell was wetted. Fresh film dries
  over about two minutes. Film younger than 0.75 s is ignored, so a drop doesn't
  react to its own wake.

### Water is conserved

Water is counted by **volume**. A drop on glass keeps roughly the same contact
angle whatever its size, so it is a spherical cap of fixed shape and its volume
goes as $V \propto r^3$. Merging adds volumes:

$$r_\text{merged} = \sqrt[3]{r_1^3 + r_2^3}$$

Nothing on the glass creates or destroys water. Beads, trails, splashes and
spray all take their volume out of the drop that made them, and every piece of
a trail ends as a droplet holding exactly its water. Water leaves the glass only
when it runs off the screen, the wiper pushes it off, mist dries, or resting
drops evaporate. The test hook `WaterOnGlass()` sums everything on the glass;
with what ran off, dried and turned to mist it balances the starting total
exactly.

### Landing

Raindrops arrive at $10 \times$ rain amount per second (more in gusts and heavy
showers) at random spots, with radius

$$r = s\,\big(r_\text{min} + (r_\text{max} - r_\text{min})\,u^{k}\big), \quad u \sim U(0, 1), \qquad k = \text{clamp}\big(2.5\,I^{-0.35},\ 1.2,\ 6\big)$$

which makes small drops common and big ones rare. $I$ is how hard it is raining
right now relative to the default (the rain amount, the passing shower and any
gust burst). Heavier rain has bigger drops (Marshall and Palmer: the typical
size grows about as $R^{0.21}$, see [section 3](#rain-outside)), so in a
downpour $k$ falls and the sizes lean toward the large end; in drizzle they
lean toward the small end. At the default $k = 2.5$; at three times the rain
the mean radius is about 15% larger and the share of big drops half as large
again. A drop that lands on another merges into it.

Raindrops arrive along the glass as well as into it: part of their fall, and
in wind a sideways push. Big drops fall faster (see the fall speeds in
[section 3](#rain-outside)) and carry that motion into the glass, while fine
spray drifts in and lands softly, so the wind steers big drops far more. The
direction of travel along the glass is

$$\mathbf{d} \propto \big(W\,h\,U(0.6, 1.2) + U(-0.25, 0.25),\ U(0.4, 1)\big), \qquad h = \text{clamp}\left(\frac{r/s - 2}{6}, 0, 1\right)$$

where $W$ is the wind at that spot and $h$ is the drop's **heft**, 0 for a
2 px drop and 1 for an 8 px one. How steeply it comes in, the **glance**
$g \in [0, 1]$, is mostly small, with an occasional fast one, more in wind:
$g = \text{clamp}(u_1 u_2 (0.7 + 0.8 |W| h))$.

What happens next depends on size:

- **Big drops splash** (radius above 3.5 px, with probability
  $\min(0.9 \times \text{splashes}, 1)$). A raindrop that lands inside a drop
  already on the glass throws much of that drop's water out with its own (a
  crown splash: Josserand and Thoroddsen): the splash takes
  $\min\big(V_o\,U(0.3, 0.6),\ 2V_\text{rain}\big)$ of the drop's volume $V_o$
  with it, or all of it if what is left would be mist. The drop flattens into
  a thin sheet thrown out in 5 to 7 lobes: three in the direction of travel,
  $1.8$ to $3.2$ radii long and longer the steeper it came in
  ($\times(1 + 1.2g)$), and a short ruff backwards and to the sides. The sheet
  spreads fast and slows ($1 - (1-t)^2$ over the first 18% of the splash), and
  at 12% its rim breaks into a spray of $8$ to $22$ micro droplets flung past
  the tips. Then one of four things happens, with $b$ growing from 0 at a 4 px
  raindrop to 1 at 8 px:
  - **It shoots off**, with probability $p_s = (0.12 + 0.8g)\,d_y$: a drop that
    came in fast and steep keeps most of its momentum along the glass. After
    about a tenth of a second, 70 to 88% of the water leaves the end of the main
    lobe as a [flung drop](#flung-drops) at 0.2 to 0.35 m/s, drawing a trail
    until it slows; a small bead stays where it hit.
  - **It bursts**, with probability
    $p_b = (1 - p_s)\,\text{clamp}\big(B\,(0.02 + 0.12b + 0.25\,\omega),\ 0,\ 0.95\big)$,
    where $B$ is **Splashes that burst** and $\omega$ is how wet the spot is:
    half the film's wetness, or 1 if it landed in a drop. In high-speed footage
    of rain on windows, a big or hard drop, and above all one landing in
    standing water, sometimes throws its sheet out as splayed fingers whose
    rims break off and fly across the glass in every direction, sideways and
    even up, before their weight turns them down. Here the sheet goes out all
    round in 6 to 8 fingers at even angles (jittered by ±0.3 of the spacing),
    each $r\,U(1.6, 3)\,\big(1 + 0.4\cos(\alpha - \alpha_0)(0.5 + g)\big)$
    long, longest the way the raindrop was going ($\alpha_0$), with 1.8 times
    the usual spray. It breaks up early (at 35% of the usual time): the 3 to 5
    longest fingers (picked by length $\times U(0.6, 1.4)$; one more for a
    splash over 8 px) each throw a drop off their end. Those drops take 55 to
    85% of the water between them, shared by length $\times$ width
    $\times\,U(0.3, 1.7)$, and fly off along their finger ($\pm 0.15$ rad) as
    [flung drops](#flung-drops) at

    $$v = U(500, 1300)\,s\,\Big(0.5 + 0.7\,\frac{L}{L_\text{max}}\Big)(0.8 + 0.4g)\ \text{px/s}$$

    (about 7 to 44 cm/s), the long fingers throwing theirs fastest. The rest
    stays in the middle as a drop. In the offline tests with the default
    settings, a splash bursts about once every two seconds.
  - **It scatters**, with probability $(1 - p_s - p_b)(0.18 + 0.3b)$: a big
    drop hitting hard has more energy than the rim can hold together, so at
    full spread the sheet breaks into a drop or two at the end of each lobe
    (shared by lobe area), with some left in the middle.
  - **It pools** otherwise: surface tension pulls the sheet back (a smoothstep
    over the last 70% of the splash), the tips of long lobes tear off as small
    drops, and the rest gathers into one bead near where it hit.

  A splash lasts $0.22 + 0.012\,r/s + 0.1g$ seconds, a quarter to two fifths of
  a second, cut to 45% when it shoots off, 40% when it scatters and 35% when it
  bursts. A burst knocks resting drops within $3.5r$ (see Knocks below).
- **Medium drops skid** when they arrive at a glancing angle, with probability
  $(0.12 + 0.6\,\text{clamp}(|W|/1.5))(0.3 + 0.7h)$. They skid $r \cdot U(1, 2.6)$
  in the direction of travel, leave one to three fine droplets along the way,
  and settle as a head with a thinning tail.
- **Small drops simply appear**, and with probability $0.35 \times$ splashes
  throw two to five tiny satellites.

The pane also goes on above the screen, so now and then
($0.12 \times$ rain amount per second on a 2560 px wide screen) a drop that
gathered up there runs down into view from the top edge.

### Resting

A resting drop grows slowly as it gathers mist
($\dot r = 0.012\,s \times \text{rain} \times (0.5 + a)$ px/s), clears the mist
under itself every one to three seconds, and merges with any neighbour it
touches. It also evaporates: its area shrinks at a constant rate,

$$\frac{d(r^2)}{dt} = -0.015\,s^2 \ \text{px}^2/\text{s}$$

which hardly matters while it rains but takes the smallest drops first, over a
minute or two, once the rain stops. A drop drawn smaller than about 1.1 px is
gone.

**Shapes.** Photographs of rain on a window show hardly any round drops
bigger than a millimetre or so: the contact line catches on specks and
scratches as a drop spreads, so its outline is lumpy, and a drop that merged
or stopped running keeps whatever shape it was left in, because a pinned
contact line hardly moves. A resting drop is drawn as the union of up to six
blobs, and every outline is roughened as well in the shader (see
[Water textures](#water-textures)):

- A **new drop** bigger than 3 px is round 15% of the time. Otherwise it has
  one bump or two (two with probability $0.3 + 0.4b$, $b$ growing from 0 at
  3 px to 1 at 9 px), each of radius $r\,U(0.3, 0.55)$ at $r\,U(0.4, 0.7)$
  from the centre in a random direction. The main blob takes the rest of the
  area, so the blob areas add up to $r^2$, and the whole is re-centred on its
  centre of mass.
- Every drop gets its own **roughness**
  $\rho = U(0.15, 0.55)\,\text{clamp}\big((r/s - 1.2)/4\big)$ and a random
  phase $\sigma$, so specks under about 1.5 px are round and bigger drops each
  have their own irregular outline.
- When two drops **merge**, both drops' blobs are kept, re-centred on the
  combined centre of mass; while there are more than six, the two closest are
  folded into one (area-weighted). The merged drop keeps the rougher of the
  two outlines.
- A running drop that **stops** keeps its outline, lumps and all (rescaled so
  the blob areas still add up to $r^2$), becomes at least a little ragged
  ($\rho \ge U(0.2, 0.4)$), and pulls its tail back in over about 0.3 s
  (see [Running](#running)) instead of snapping round.

The contact line is pinned, so the drop only slowly pulls itself round (blob
offsets decay with a time constant of $60\,\text{s} \times \text{pin}$), and
merged drops stay peanut-shaped or ragged for a long time. Heavy drops sag
longer and narrower as they near letting go,
$\times\big(1 + 0.3\,\text{clamp}(r/r_\text{pin} - 0.4)\big)$ tall and the square
root of that narrower, and lean a little downwind.

### Letting go

A drop on a vertical pane is held by **contact angle hysteresis** along its
contact line (the Furmidge relation):

$$F_\text{grip} = \gamma\, w\, (\cos\theta_r - \cos\theta_a)$$

with surface tension $\gamma = 0.072$ N/m, contact width $w = 2a$, and advancing
and receding angles $\theta_a \approx 50$–$60°$ and $\theta_r \approx 20$–$30°$
on ordinary window glass, giving about 0.022 N per metre of width. Gravity on a
spherical cap with a contact angle near 40° is $\rho g V$ with $V \approx 0.6a^3$.
Grip grows with width ($\propto a$) and weight with volume ($\propto a^3$), so
they balance at one size:

$$\rho g \cdot 0.6a^3 = 0.022 \cdot 2a \quad\Rightarrow\quad a \approx 2.7\ \text{mm}$$

which is about 11 px at 1440p, `kPinRadius`. That is why drops hang on for a
long time and then go suddenly, and why drops bigger than 5 to 6 mm across
never last. Each drop lets go at its own **release radius**

$$r_\text{release} = r_\text{pin} \cdot \text{grip} \cdot p \cdot (0.85 + 0.3a) \cdot (1 - 0.3w)$$

where $p \in [0.85, 1.2]$ is a per-drop factor for how ragged its contact line
is (most drops let go near the typical size; a few hold on far longer), water-
loving spots grip harder, and wet film grips less.

Wind adds a sideways pull. Drag grows with the drop's frontal area ($r^2$) and
weight with its volume ($r^3$), so the wind's pull relative to the weight, the
**tilt**, is larger for small drops:

$$\tau = \frac{W}{2} \cdot \frac{r_\text{pin}}{\max(r,\ 0.3\,r_\text{pin})}$$

The contact line has to hold weight and wind together, so a drop lets go when
$r\,(1 + \tau^2)^{1/4} > r_\text{release}$, and heavy drops let go in a gust.

It goes from its lowest point: the blob furthest along the way it sets off
becomes its head, and the rest of its outline drains into it as it gets going
(see [Running](#running)), so a lumpy drop doesn't turn round the moment it
moves.

**Knocks and shaking.** Vibration and impacts help a drop past its hysteresis
(Noblin, Buguin and Brochard-Wyart; Brunet, Eggers and Deegan): for a moment
the contact line can move as if it had less grip. Two things do this:

- **A raindrop landing on or splashing into a drop.** On a vertical pane most
  of a raindrop's momentum is along the glass (it was falling), far more than
  the contact line can hold for that instant. Every resting drop within reach
  of the impact ($r$, or $2.5r$ for a splash) gets a **jolt**
  $j \mathrel{+}= (0.5 + g)\,\frac{V_\text{rain}}{V_\text{rain} + V_\text{drop}}$
  (at most 0.3), which decays with a time constant of 0.15 s. A small raindrop
  hardly moves a big drop; one of similar size can knock it loose.
- **Close thunder.** The pane vibrates for a second or two, with strength $S$
  (see [Lightning](#lightning)).

While either lasts, the release radius is lowered:

$$r_\text{release} \to r_\text{release}\,\big(1 - \min(j + 0.25\,S,\ 0.4)\big)$$

so drops that were within a few percent of letting go do, all at once. In the
offline tests a shake of 0.5 starts about 80 drops running within two seconds
on a 1080p screen, against a background of about two a second.

### Running

Above the threshold, the sliding speed of a small drop follows
$\text{Ca} \propto \text{Bo} - \text{Bo}_c$: the capillary number (speed ×
viscosity / surface tension) grows with how far the Bond number (weight
relative to surface tension, $\propto a^2$) exceeds its critical value. A
moving drop keeps going until it has shrunk to about 80% of its release size
(the receding threshold is lower than the advancing one), so with
$r_\text{slide} = 0.8\,r_\text{release}$:

$$v = v_0 \left(\frac{r^2}{r_\text{slide}^2}\sqrt{1 + \tau^2} - 1\right)(1 + 0.8\,w_\text{ahead}), \qquad v_0 = 340\,s \times \text{running speed}\ \text{px/s}$$

capped at about 40 cm/s. A drop that only just broke loose creeps; one that
swallowed its neighbours is gone almost at once. Viscous drag is strong, so the
speed settles within a fraction of a second (rate 8 /s on dry glass, 14 /s on
wet).

While running, a drop:

- **steers** (`Steer`) every drop-length or so by scoring five directions
  between ±0.6 rad of straight down:
  $1.2\cos\phi + 2w + 0.8a - 0.6|\phi - \text{heading}| + U(0, 0.5)$.
  It prefers wet glass, water-loving patches and its current heading, so it
  meanders and later drops reuse earlier paths. Fast drops wander less, and the
  wind leans the heading by $\arctan\tau$, always keeping it somewhat downhill.
- **sticks and slips** on dry glass: short stalls (0.05 to 0.35 s) at a rate of
  $1.2 \times \text{grip}$ per second. On wet glass it runs smoothly.
- **swallows** every drop and trail it touches. Momentum is conserved, so it
  lurches, then speeds up because it is heavier.
- **sheds water.** Behind the drop the receding contact line leaves a film.
  Below a critical speed the contact line keeps up and the film breaks into a
  line of beads (8 to 22% of the drop's radius, further apart the faster it
  goes). Above it the line can't keep up and a **rivulet** is drawn out
  (Podgorski, Flesselles and Limat). The critical speed is

  $$v_c = 400\,s \times \frac{\text{trail speed}}{\sqrt{\text{grip}}}(1.2 - 0.4a)\ \text{px/s} \quad (\approx 9\ \text{cm/s})$$

  and once a rivulet has started it continues down to $0.8\,v_c$, so it doesn't
  flicker. Faster drops draw wider rivulets: half-width
  $\max\big(r(0.12 + 0.14e),\ 1.6s\big)$ with $e = \text{clamp}(v/v_c - 1)$.
- **stops** once it has shed below its sliding size, unless it is on a wet
  path. It keeps the shape it stopped in (see [Resting](#resting)).

**Shape on the move.** A running drop is a teardrop: its head, stretched along
its path by $1 + 0.15\ell$ and narrowed across by $1 - 0.12\ell$, and two tail
sprites behind it, close enough to fuse with the head into one outline
($0.45 + 0.4\ell$ and $0.9 + 0.9\ell$ head radii back, 0.8 and 0.5 as wide).
In footage of drops sliding down glass the tail grows smoothly as a drop
speeds up and drains back slowly as it stops; it never appears at once. So
the tail length $\ell \in [0, 1]$ eases toward its target:

$$\ell \mathrel{+}= (\ell^* - \ell)\min\Big(1, \frac{\Delta t}{\tau}\Big), \qquad \ell^* = \text{clamp}\Big(\frac{v}{600\,s}\Big), \qquad \tau = \begin{cases} 0.06\ \text{s} & \ell^* > \ell \\ 0.3\ \text{s} & \text{otherwise} \end{cases}$$

and the tail sprites are drawn with weight $\text{smoothstep}(0, 0.25, \ell)$,
so the tail fades in and out instead of popping. The tail follows the path
rather than the heading: its direction turns toward the heading by
$\min(1, \text{step}/1.5r)$ of the difference each frame, so a drop that turns
has a curved tail. Resting and running drops are drawn by the same code (a
resting drop is one whose tail has drained, $\ell \to 0$ with $\tau = 0.3$ s),
so nothing jumps when a drop starts or stops.

Lumps left from letting go, and drops it has just swallowed (each shows for a
moment as a lump where it was), drain into the head: every frame their offsets
and radii shrink by $e^{-\Delta t/0.12\,\text{s}}$, the head's radius relaxes
toward $r$ at the same rate, and a lump under $0.12r$ is dropped. The water is
counted by $r$, not by the blobs, so this only changes the outline. The head
keeps 60% of its resting roughness, and the phase of its outline advances by
$\text{step}/4r$, so it wobbles slightly from side to side as it goes.

### Flung drops

`Launch`, the `flung` branch of the running update

A drop thrown off by a splash (shooting off or bursting) starts with a
velocity $\mathbf{V}$ in any direction, sideways and up included, and goes
where its momentum takes it. It doesn't steer or stick-slip; three forces act
on it, written as accelerations in px/s²:

$$\frac{d\mathbf{V}}{dt} = F_w\,(\tau,\ 1) - F_g\,\hat{\mathbf{V}} - \kappa\,\mathbf{V}$$

- its **weight**, with the wind's sideways pull (the tilt $\tau$ from
  [Letting go](#letting-go)):
  $F_w = \kappa v_0 \left(\frac{r}{0.8\,r_\text{pin}\,p}\right)^2$, which matches
  the running speed law: a drop at its sliding size is pulled just as hard as
  the grip holds it back;
- the **grip** of the contact line, a steady force against the motion (it
  comes from contact angle hysteresis, so it doesn't depend on speed),
  $F_g = \kappa v_0 (1 - 0.5\,w_\text{ahead})$, weaker on wet glass. It takes
  away at most the speed there is, so it can stop a drop but never push it
  backwards;
- **viscous drag**, $\kappa = 8$ /s, the same rate that settles a running drop
  at its speed.

A drop heavier than its grip ($F_w \ge F_g$) curves round under its weight and
is handed to the ordinary running model once its heading is within 0.3 rad of
downhill ($\arctan\tau$). A lighter one slows to below $15s$ px/s and stays
where it stopped, as a drop that will run later if it grows. So the flung
drops trace arcs that bend down the glass: heavy ones swing round and run on,
small ones stop short as beads. A flung drop that swallows another adds their
momenta as vectors.

### Rivulets

`LayRivulet`, `UpdateRivulets`, `StartBead`, `Gather`

A rivulet is kept as a chain of nodes half a width apart, each holding a volume
of water. It is a low ridge about a third as high as it is wide, a cross-section
of about $0.4w^2$, which in the same units as a drop's $r^3$ is $0.6w^2$ per
pixel of length. That is exactly what the drop loses as it lays the rivulet.
The nodes are drawn as small drop splats, so the rivulet refracts like any other
water. It meanders slightly and its width wanders by ±18%.

A rivulet left on the glass is unstable. With $L$ the **streak life**:

1. **Intact** for about $L$ seconds.
2. **Pinching** from $L$ to $4L$. Surface tension (the **Plateau–Rayleigh
   instability**) swells it into bulges and thins the necks between them, at a
   wavelength of 16 to 26 widths. Each node's width is
   $w = w_0 \big(1 + 0.8\,p(t)\,[\sin(2\pi x/\lambda + \phi) + 0.3\sin(2\pi x/0.43\lambda + 2\phi)]\big)\sqrt{V/V_0}$,
   where $p(t)$ ramps from 0 to 1. A neck thinner than $0.35w_0$ (or holding
   less than a quarter of its water) snaps and hands its water to the node
   below it, or above it if that is all it is still joined to.
3. **Draining** after $2.5L$: water flows down the line at a rate of
   $1/(2L)$ per second per node, so the top thins and runs dry while lower
   pieces fatten.
4. **Beading up.** A pinched-off piece shorter than two wavelengths beads up
   after $3L$; anything still joined up beads after $7L$, one droplet per
   wavelength. Beading is not instant: the piece's two ends retract toward its
   middle at about $70\,s \times \text{bead-up speed}$ px/s, faster as the bead
   grows ($\times(1 + 0.5\min(r_\text{bead}/w_0, 2))$). Each node that retracts
   hands its water inward, 60% into the swelling bead and 40% to the new end,
   which thickens like a retracting rim. When the ends meet, the bead becomes
   an ordinary resting drop. A typical piece takes about a quarter of a second.

The top of a rivulet is its oldest part, so the stages travel down it: the top
drains, the middle pinches into segments, and the newest part is still a
continuous line. What is left is a dotted line of droplets along the path,
which later drops tend to follow and sweep up.

### Wind

`UpdateWind`, `WindAt`, `Tilt`

The **Wind** setting $W_0$ is the general direction and strength of the wind
along the glass; **Gustiness** $G$ is its variability around that. Near
buildings the wind swells and lulls by about a third of its strength and
swings in direction by tens of degrees, and what pushes the water is only its
part along the glass. So a swing can carry the water across the window the
other way for a moment, even when the wind blows mostly one way, or straight
at the glass with no part along it on average.

Three Ornstein–Uhlenbeck processes (random walks pulled back to zero, with
unit variance),

$$dx = -\frac{x}{\tau}\,dt + \sqrt{\frac{2}{\tau}}\,dB$$

with time constants of 5 s (slow swells and lulls), 0.7 s (quick buffeting)
and 4 s (swings in direction), make up the wind now:

$$W = W_0\,\text{clamp}\big(1 + G(0.4\,x_\text{slow} + 0.2\,x_\text{fast}),\ 0.2,\ 2.2\big) + G\,\big(0.35 + 0.25\min(|W_0|, 3)\big)\big(0.85\,x_\text{swing} + 0.12\,x_\text{fast}\big)$$

The first term is the strength, which scales the steady wind and never
reverses it. The second is the swing, which adds to it and can. It grows
with how windy it is overall, but is at least a breeze in a storm, so even
with no steady wind the water drifts one way and then the other. How often
the water is pushed against the steady wind, from 10-minute offline runs:

| Wind $W_0$ | Gustiness $G$ | Time pushed the other way |
| --- | --- | --- |
| 0 | 1 | half (no side is preferred) |
| 1 | 1 | 7%, in spells of half a second or more about twice a minute |
| 1 | 2 | 18% |

Gust fronts sweep across the screen,

$$W(x, y) = W + G\,(0.1 + 0.2|W|)\sin\big(0.003x/s - \phi + 0.001y/s\big), \qquad \dot\phi = 1.7\,\text{clamp}(W, -1, 1)$$

so the wind is never the same everywhere at once, and the fronts travel the
way the wind is blowing now, slowing and turning back when it does. The slow
gust also brings bursts of heavier rain, more so the windier it is:
$\times\,\text{clamp}\big(1 + 0.5\,G\min(0.4 + 0.4|W_0|, 1)\,x_\text{slow},\ 0.4,\ 2.5\big)$.

### The wiper

`StartWipe`, `UpdateWiper`: a car-style wiper pivoting just below the bottom
centre of the screen, long enough to reach the top corners. One wipe is a
stroke to the right and back, about 1.1 s each, with eased motion and a slight
overshoot. Each frame, every drop in the wedge swept since the last frame is
moved to just ahead of the blade; pushed drops merge into a ridge that leaves
the screen at the end of the stroke. Behind the blade the mist is erased and
the glass is marked wet, with a few fine droplets left behind.

### Finger wipe

`SetFinger`, `UpdateFinger`

While Ctrl+Alt are held, the mouse pointer is a fingertip on the glass, $2h$
wide with $h = 30s$ px (about 14 mm). Each frame it moves from where it was,
$\mathbf{p}_0$, to where it is, $\mathbf{p}_1$, and:

- every drop whose centre is within $h + 0.8r$ of the segment
  $\mathbf{p}_0\mathbf{p}_1$ (a capsule) is picked up: its volume joins the
  water the finger **carries**;
- along the path, in steps of $h/2$, rivulets are soaked up into the carried
  water, the mist is erased and the glass is marked wet;
- the film left behind beads up: at each step, half the time, a fine droplet
  of radius $U(0.6, 1.4)s$ is left near the edges of the stroke, its water
  taken out of what the finger carries;
- a fingertip only holds so much, $V(1.5\,r_\text{pin})$: anything over that
  spills off its sides as drops of $U(0.5, 0.9)\,r_\text{pin}$;
- the carried water is drawn as a crescent bulging in front of the finger.

When the finger stops for 0.3 s, or lifts, the water it carries is left as one
drop in front of it, often big enough to run. No water is made or lost.

## 3. Beyond the glass

`src/sky.h`, `src/sky.cpp`

### Showers

Rain rarely holds steady: cells pass over, so it builds for a minute or two,
eases off and comes back. The log of the rain rate does an Ornstein–Uhlenbeck
random walk with a time constant of 75 s, and the rate is

$$R = R_0 \exp\!\left(\sigma x - \tfrac{\sigma^2}{2}\right), \qquad \sigma = 0.6 \times \text{showers}$$

The $-\sigma^2/2$ keeps the long-run average at the rain amount you picked
($E[e^{\sigma x}] = e^{\sigma^2/2}$ for a standard normal $x$). The default rain
amount is a moderate rain of about $R_0 = 6$ mm/h.

### Lightning

Flashes arrive as a Poisson process at
$\text{lightning} \times \text{clamp}(0.3 + 0.7(R/R_0)^2, 0.3, 3) / 25$ per
second, at least 1.5 s apart: about one every 25 s at 1x, more in the heavy part
of a shower. Each strike is somewhere in the storm out to 12 km,
$d = 0.5 + 11.5\,u^{0.8}$ km, and seen from under the cloud its brightness falls
off slowly, $1.4 / (1 + d/4)$, because the clouds spread its light over the
whole sky.

Each flash is a list of strokes, each a brightness that jumps to a peak and
fades, $P\,e^{-(t - t_k)/\delta}$:

- **Cloud flashes** (60%): most lightning never reaches the ground. 3 to 9
  softer pulses 30 to 120 ms apart, each fading over 40 to 120 ms, at 60% of the
  brightness: the irregular flicker of a cloud lit from inside.
- **Cloud-to-ground flashes** (40%): a train of return strokes down the same
  channel. About one in five has a single stroke; the rest have three or four on
  average (each further stroke with probability 0.6, up to 9). They are
  log-normally spaced around 55 ms apart (15 to 200 ms), each fades in 25 to
  50 ms, the first is the brightest, and 30% are followed by **continuing
  current**, a dimmer glow (15 to 35% of the peak) lasting 40 to 250 ms.

Strokes are shorter than a frame, so sampling them at the frame time would make
them flicker at random. Instead each frame shows the stroke's average brightness
over the frame, the exact integral:

$$\bar F = \frac{1}{\Delta t}\int_{t-\Delta t}^{t} P\,e^{-(\tau - t_k)/\delta}\,d\tau = \frac{P\delta}{\Delta t}\left(e^{-a/\delta} - e^{-c/\delta}\right)$$

with $a$ and $c$ the start and end of the frame measured from the stroke, which
is what the eye (or a camera) integrates too. Each flash also has a direction in
the sky (within ±70° of straight up), which sets where the drops catch its light.

Sound travels at 343 m/s, so the thunder is queued $d / 0.343$ seconds after the
flash: about 3 s per kilometre.

**Thunder shakes the window.** Only a close strike is loud enough to rattle a
pane, so each flash has a rattle strength $S = P\,e^{-d/1.5}$, about 0.5 at
1 km and 0.07 at 4 km. If it is over 0.05, the shake reaches the glass with the
thunder's boom, $d/0.343 + 0.3$ s after the flash, and dies away with a time
constant of 1.2 s. The **Thunder shakes the glass** setting scales it. The
simulation turns the shake into drops letting go (see
[Letting go](#letting-go)) and the sound into the pane rattling in its frame.

### Daylight

The light the drops refract is the sky's, so it follows the time of day.

**Where the sun is.** With **Daylight** on auto, the sun's elevation comes from
NOAA's general solar position formulas. With $\gamma$ the fractional year in
radians, $\gamma = \frac{2\pi}{365}\left(N - 1 + \frac{h - 12}{24}\right)$
for day $N$ and UTC hour $h$:

$$E_t = 229.18\,(0.000075 + 0.001868\cos\gamma - 0.032077\sin\gamma - 0.014615\cos 2\gamma - 0.040849\sin 2\gamma)\ \text{min}$$

$$\delta = 0.006918 - 0.399912\cos\gamma + 0.070257\sin\gamma - 0.006758\cos 2\gamma + 0.000907\sin 2\gamma - 0.002697\cos 3\gamma + 0.00148\sin 3\gamma$$

$E_t$ is the equation of time (the sun runs up to 16 minutes ahead of or behind
the clock over the year) and $\delta$ the declination. The hour angle is
$H = (60h + E_t + 4\lambda)/4 - 180°$ and the elevation

$$\sin e = \sin\phi\,\sin\delta + \cos\phi\,\cos\delta\,\cos H$$

raindesk doesn't ask where you are. The longitude $\lambda$ comes from the time
zone (15° per hour from Greenwich, which is close to where most people in a
zone live) and the latitude $\phi$ from the middle of the country set in
Windows' region settings (`GetGeoInfo`), or 40° if there is none. Either can be
off by several degrees, which moves sunrise and sunset by a few minutes, far
less than the twilight they fade over.

**How light it is.** Daylight is $D = \text{smoothstep}(-6°, 12°, e)$: night
until the end of civil twilight, full day once the sun is 12° up (under rain
cloud the sky is still dim near the horizon). Set by hand, $D$ is the slider
value. It follows changes over about half a second.

**What colour.** Under rain cloud the day sky is a flat, cool grey,
$(0.80, 0.84, 0.90)$. At night clouds over a town glow a dull orange from the
streetlights below, $(0.26, 0.21, 0.16)$. The sky colour is the mix of the two
by $D$, shifted toward blue in the blue hour (by
$4D(1 - D) \times (-0.03, -0.01, +0.05)$), and darkened under heavy rain, which
falls from thicker cloud: $\times \max\big(1/(1 + 0.1\max(R/6 - 1, 0)),\ 0.7\big)$.
How the drops use it is in [section 4](#composite-pscomposite).

### Rain outside

The screen is the window pane. The viewer sits $E = 0.65$ m in front of it, and
drops fall through the space behind it from 0.25 m to 3.5 m deep (further ones
are too small and blurred to see). A point $(X, Y)$ metres off the line of sight
at depth $z$ behind the glass lands on the screen at

$$x = c_x + X\,k, \quad y = c_y + Y\,k, \qquad k = \frac{P\,E}{E + z}$$

with $P$ pixels per metre at the glass (screen height / 0.336 m).

**How many and where.** The visible volume at depth $z$ grows as $(E + z)^2$, so
for drops spread evenly through space depth is sampled by inverting that cubic:

$$z = \sqrt[3]{(E + z_\text{near})^3 + u\big((E + z_\text{far})^3 - (E + z_\text{near})^3\big)} - E$$

About 1350 drops are kept in view at the default rain amount, scaled by
$\sqrt{R/6}$ and the **Rain outside** setting (at most 6000). Across, drops fill
1.25 times the width of the view at their depth, and the slice wraps around: a
drop blown out of one side comes back in at the other, so the wind never thins
out the rain on the upwind side.

**Sizes.** Raindrop diameters follow **Marshall and Palmer**:
$N(D) = N_0 e^{-\Lambda D}$ with $\Lambda = 4.1\,R^{-0.21}$ per mm at $R$ mm/h,
so heavier rain has bigger drops. Only drops over 0.5 mm are big enough to see,
and since the exponential is memoryless their diameters are simply
$0.5 + \text{Exp}(\Lambda)$ mm (capped at 6 mm).

**Speeds.** Each falls at its terminal speed (**Atlas et al. 1973**, fitted to
Gunn and Kinzer's measurements):

$$v(D) = 9.65 - 10.3\,e^{-0.6D}\ \ \text{m/s}$$

about 2 m/s for a 0.5 mm drop, 6.5 m/s for 2 mm and 9 m/s for the biggest. The
wind blows them sideways at $2W$ m/s, plus up to ±0.3 m/s of each drop's own
drift.

**Streaks.** A falling drop is seen as a streak, the path it covers while the
eye (or a camera's shutter) integrates, taken as $T = 1/50$ s. Each streak runs
from where the drop was $T$ ago to where it is now, so its on-screen length is
$\ell = v\,T\,k$.

Its width and opacity come from two effects:

- **Size.** Seen sharp, a drop is its own diameter wide, $D_\text{px} = D\,k$.
- **Focus.** The eye is focused on the glass, so a drop $z$ behind it blurs into
  a disc. With a pupil of $A = 3$ mm, rays from the drop to the pupil cross the
  glass in a circle of diameter

  $$b = A\,\frac{z}{E + z}\,P\ \text{px}$$

  The streak's width is $w = \sqrt{D_\text{px}^2 + b^2}$.
- **Opacity.** Each pixel on the path is covered for $D_\text{px}/(vk)$ of the
  exposure, so a sharp streak's opacity is $D_\text{px}/\ell$. The blur spreads
  the same light over $w$ instead of $D_\text{px}$:

  $$\alpha = G \cdot \frac{D_\text{px}^2}{\ell\,w}$$

  Physically that is tiny (a near drop is about 1.5%), yet rain is plainly
  visible in real life, because each drop is a wide-angle lens (field of view
  about 165°) showing the bright sky, far brighter than a monitor can show. The
  gain $G = 22 \times (0.6 + 0.4 \times \text{rain outside})$ makes up for that,
  capped at 35% opacity.

On the GPU each streak is a soft line with a Gaussian cross-section and round
ends, coloured mostly by the sky (Garg and Nayar showed that drops are almost
always brighter than what is behind them) and a little by the blurred desktop
behind it. In a lightning flash every streak lights up.

**Sheets of rain.** In gusty wind, rain comes in sheets: the density varies over
a few metres and the pattern blows past with the wind. Each streak's gain is
multiplied by

$$\text{clamp}\Big(1 + 1.6\,a\;n\big(\tfrac{X - s}{6\,\text{m}} + 0.15z,\ \tfrac{t}{7\,\text{s}}\big),\ 0.1,\ 2.2\Big), \qquad a = \min\big(0.5\,G\,\min(|v_w|/2, 1),\ 0.9\big)$$

where $n$ is smooth value noise in $-1..1$, $s$ is how far the wind (at $v_w$
m/s) has carried the pattern so far, and $G$ is **Gustiness**. With no wind
the rain falls evenly.

## 4. Rendering

`shaders/rain.hlsl`, `Render()` and `DrawOverlay()` in `src/main.cpp`

All shaders live in one file, built into the exe as a resource and compiled at
startup (or taken from a `rain.hlsl` next to the exe and recompiled whenever it
changes; see [Tuning the look](#tuning-the-look)). Each frame:

1. **Mist** (persistent texture): dry it a little (`PSDecay`), stamp new mist
   (`PSMist`, max blend), and wipe where drops passed (`PSErase`, multiply).
2. **Water** (cleared every frame): add every drop splat (`PSDrop`).
3. **Onto the screen**, each layer blended over the last with premultiplied
   alpha: the lightning flash (`PSFlash`), the rain outside (`VSStreak` +
   `PSStreak`), the water on the glass (`PSComposite`), and during a wipe the
   wiper (`PSWiper`).

### Water textures

Two full-screen `R16G16_FLOAT` targets hold, per pixel,
$\big(\sum h_i f_i,\ \sum f_i\big)$, where $h_i$ is a splat's water height
(half its radius) and $f_i = w_i (1 - q')^2$ its smooth falloff. $q$ is the
squared distance from the splat's centre in units of its radius, and
$w_i \in [0, 1]$ the splat's **weight**, which fades a sprite in or out (a
running drop's tail does) without changing its size.

No drop on real glass is a perfect ellipse, so each splat also has a
roughness $\rho$ and a phase $\sigma$, and a few low harmonics around the
outline push it in and out:

$$q' = q + \rho\,H(\theta)\,q(1 - q), \qquad H(\theta) = 0.6\cos(2\theta + \sigma) + 0.4\cos(3\theta + 2.3\sigma) + 0.25\cos(5\theta + 4.1\sigma)$$

where $\theta$ is the angle around the centre. The bump is largest near the
edge and vanishes at the centre and at the sprite's border, where the field
must still fall to zero. Since $|\rho H| \le 0.55 \times 1.25 < 1$,
$dq'/dq = 1 + \rho H (1 - 2q) > 0$, so the field still falls steadily outward
and the outline stays one piece; at the roughest it moves by up to about a
tenth of the radius. A drop on a vertical pane sags, so the profile is squashed above the centre ($\times 1.15$) and
stretched below ($\times 0.9$). Summed fields behave like **metaballs**: nearby
blobs fuse into one smooth outline, which is how lobes, tails, trails and
merging drops become single shapes.

### Composite (`PSComposite`)

For each pixel the water and mist values are added, then:

1. **Outline.** The drop edge is where the summed field crosses
   $k_\text{edge} = 0.12$, antialiased with `fwidth`.
2. **Shape.** Overlapping blobs push the field above 1, so it is first squashed
   smoothly into 0..1 (unchanged at the edge), $f' = 1 - (1 - k)^{F/k}$. For a
   lone splat $f = (1 - q)^2$, so $q = 1 - \sqrt{f'}$, rescaled so that $q = 1$
   at the edge. The thickness is then a **spherical cap** meeting the glass at
   the outline, $t = h\sqrt{1 - q}$. A sphere maps the view behind it smoothly,
   with no rings inside the drop.
3. **Normal** from the cap's gradient by central differences,
   $\mathbf{n} = \text{normalize}(-2\nabla t,\ 1)$.
4. **Refraction.** A drop on glass is a small plano-convex lens, so it shows a
   shrunken, upside-down view of a wide patch behind it. The shader samples the
   desktop at

   $$\mathbf{p} - \mathbf{n}_{xy} \cdot (2.6r + 2s) \cdot \text{refraction}$$

   Where the surface tilts down, the sample comes from above, and the image is
   flipped. The mip level, $\log_2(1 + 0.35r) + 1.5(1 - n_z)$, blurs it more for
   bigger drops and steeper surfaces, since the drop's image is out of focus.
5. **Shading.**
   - The lower half is lit slightly by light gathering from above,
     $\times(1 + 0.15\,\text{saturate}(1.5n_y) - 0.05)$.
   - The rim darkens where rays leave at grazing angles and reflect back inside,
     most strongly along the top edge. Tiny droplets have a dark band too narrow
     to resolve, so theirs is lighter.
   - Light from the sky above the window is flipped by the lens onto the
     bottom edge, giving the bright **crescent** that makes rain visible even
     against something dark. Its colour is
     $0.36\,C_\text{sky} + (1 - D)(0.08, 0.06, 0.04)$, with $C_\text{sky}$ and
     $D$ from [Daylight](#daylight): cool grey by day, a dimmer warm glow at
     night. It barely shows over bright content, which is already that bright.
   - By day, larger drops get a soft highlight from the bright sky above and to
     the left, $0.4D(\mathbf{n} \cdot \mathbf{h})^{80}$. Mist gets none; on tiny
     droplets it would read as noise.
   - By night each drop shows tiny upside-down images of the lights outside: a
     warm streetlight up to the left as a sharp point low on the drop's right,
     $(1 - D)(\mathbf{n} \cdot \mathbf{l}_1)^{150}(1.0, 0.78, 0.5)$, and a dimmer,
     cooler one up to the right low on its left,
     $(1 - D)(\mathbf{n} \cdot \mathbf{l}_2)^{220}(0.4, 0.45, 0.5)$.
   - **Lightning.** With $F$ the flash brightness and $\mathbf{f}$ its direction
     on the screen, every drop catches it: the view through the drop brightens
     ($\times(1 + 0.6F)$), the lens throws a bright crescent onto the side away
     from the flash ($0.8F\,\text{saturate}(-1.6\,\mathbf{n}_{xy} \cdot \mathbf{f})$
     on steep parts), and the side facing it glints
     ($0.7F(\mathbf{n} \cdot \mathbf{h}_f)^{40}$). Even the finest mist sparkles.
6. Output is premultiplied colour and alpha.

### Flash (`PSFlash`)

A full-screen layer, drawn only during a flash, that tints everything a cool
white with opacity $0.22F$, a little stronger toward the flash and toward the
top of the screen, where the sky is. A flash stands out far more against a
night sky than a grey day, so $F$ here is the sky's flash brightness times the
**Flash brightness** setting times $1.5 - 0.5D$.

### Streaks (`VSStreak`, `PSStreak`)

Each streak is a quad around its line, padded by three standard deviations of
its Gaussian ($\sigma = w / 2.355$, from the full width at half maximum).
Opacity is $\alpha\,e^{-v^2/2\sigma^2}$ across the line, with the same falloff
past each end. Colour is $0.7 \times$ the light $+\ 0.3 \times$ the blurred
desktop behind it, brightened by $(1 + 2.5F)$ in a flash. The light is the sky
colour plus, at night, the glow of streetlights and lit rooms on drops near the
window, $C_\text{sky} + (1 - D)(0.30, 0.24, 0.16)$, so the rain outside still
shows at night, warmer and dimmer.

### Wiper and screenshots

`PSWiper` draws the blade (a rubber edge in a frame lit along one side) and its
arm analytically from the pivot and angle. `SaveScreenshot` draws the plain
desktop (`PSDesktop`) and then the same layers as the screen into an offscreen
texture, reads it back and writes a PNG.

## 5. Sound

`src/sound.h`, `src/sound.cpp`

Everything is synthesised as it plays, at 48 kHz stereo; there are no recordings.
The render thread tells the synthesiser about every raindrop that hits the glass
this frame (where, how big, how steep, whether it splashed or burst), the rain
rate, the wind and the daylight, and any thunder. A sound thread renders
1024-sample buffers (21 ms) and keeps four queued with `waveOut`, so a tap is
heard within about 100 ms of its drop landing. The device is open only while
the volume is above zero.

Rain is a sound texture: we recognise it by its statistics, many separate
events at random moments, each a little different (McDermott and Simoncelli).
Anything regular, a fixed rate of events or the same few resonances every
time, is heard at once as synthetic. So every part below is a random stream
of events, and nothing repeats.

### Drops on the glass

A raindrop presses on the pane for its **contact time**, roughly its diameter
over its speed, a fraction of a millisecond. That force is a half-sine pulse of
length $T_c = 0.12 + 0.25\,r_\text{mm}$ ms (bigger drops press longer, so sound
lower), with an amplitude of $\min\big((r/1.2\,\text{mm})^{1.5}, 4\big)(0.6 + 0.6g)$,
$\times 1.5$ for a splash. Each tap starts at a random moment within the
buffer (it is about as long as a frame), so taps don't beat in time with the
frame rate, and is panned to where it hit with equal-power panning,
$(\cos\theta, \sin\theta)$ with $\theta = \frac{\pi}{4}(\text{pan} + 1)$.

A window pane has a great many bending modes: for a plate of area $A$, mass per
area $\rho h$ and bending stiffness $D$ there are about
$\frac{A}{2}\sqrt{\rho h / D}$ per hertz, which for 1.5 m² of 4 mm glass is
about 120 in every kilohertz. Which ones a drop excites depends on exactly
where it strikes, and the frame and putty damp them hard, so what you hear is
a short, dense, noise-like ring, never the same note twice. Each tap is:

- **the ring**: white noise through a one-pole low-pass, starting at 0.6 of the
  force and decaying over 1.5 to 4 ms ($\times(0.7 + 0.3\min(r_\text{mm}, 2))$).
  A half-sine has little energy above about 1.5 / its length, so the low-pass
  is at $0.6/T_c$, clamped to 0.9 to 6 kHz: a big, soft drop can't ring the
  high modes;
- **two body modes**, the nearer modes that happen to stand out where it
  struck, at random frequencies $220\,e^{2.2u}$ Hz (220 Hz to 2 kHz) ringing
  for $T_{60}$ = 8 to 30 ms. Each is a two-pole resonator,

  $$y_n = g\,x_n + 2\rho\cos\omega\,y_{n-1} - \rho^2 y_{n-2}, \qquad \rho = e^{-6.91/(T_{60} f_s)}, \quad \omega = \frac{2\pi f}{f_s}$$

  which decays by 60 dB in $T_{60}$;
- **spray**: the drop shattering, a burst of hiss at $0.6(0.5 + g)$ ($\times 1.4$
  for a splash) decaying over 1.2 ms (4 ms for a splash), which gives the tap
  its crisp edge.

The pulse and the spray go through a 1.5 kHz high-pass (the tick of the impact
itself); the ring, the body modes and a little of the pulse through a 120 Hz
one. A splash's droplets land again around it a moment later: 2 to 5 tiny
clicks (2 to 4 samples, at 5 to 25% of the tap) within 50 ms, or for a burst 6
to 12 within 120 ms, spread twice as wide.

### Rain outside

Rain outside is thousands of separate impacts a second, each a tiny burst of
noise coloured by what it hits, from near and far (Liu, Cheng and Tong).
Heard through a shut window there is little hiss left: the glass takes most of
the highs away and leaves a soft, full patter (see
[Through the window](#through-the-window)).

- **When.** Impacts arrive as a Poisson stream (exponential intervals, never
  on a beat) at $120\,R^{0.8}$ a second, with $R$ the rain rate in mm/h: about
  500 a second in moderate rain (6 mm/h), 1800 in a downpour (30 mm/h).
- **Where.** Raindrops fall evenly over the ground, so the number landing at
  distance $d$ grows as $d$, while each is quieter as $1/d$. The distance is
  drawn evenly by area from the street below the window (6 m) out to 30 m,
  $d = \sqrt{d_0^2 + u\,(d_1^2 - d_0^2)}$, and the amplitude is
  $\frac{4}{d}\,(0.25 + 0.75u^3)$ (most raindrops are small). That alone gives
  the patter its few louder, nearer ticks among many soft, far ones. Each is
  panned at random.
- **What it hits.** Each impact is white noise through a band-pass, decaying
  exponentially:

  | Surface | Share | Centre (Hz) | Q | Decay (ms) |
  | --- | --- | --- | --- | --- |
  | Paving, stone: a crisp tick | 55% | $900\,e^{1.9u}$ (0.9 to 6 k) | 0.9 | 0.3 to 1.2 |
  | Leaves, grass: a soft pat | 30% | $450\,e^{1.4u}$ (0.45 to 1.8 k) | 1.3 | 1 to 3 |
  | Roofs, cars, bins: rings a little | 15% | $1800\,e^{1.1u}$ (1.8 to 5.4 k) | 3.5 | 0.8 to 2.5 |

  With $f = (d - 6)/24$ from 0 at the nearest to 1 at the furthest, the centre
  is lowered by $\times(1 - 0.35f)$ (air and obstacles take the highs) and
  $0.1 + 0.6f$ of it goes to the echoes.
- **Bubbles.** 2% of impacts land in a puddle and trap a bubble, which rings
  at its Minnaert frequency

  $$f_0 = \frac{1}{2\pi a}\sqrt{\frac{3\gamma p_0}{\rho}} \approx \frac{3.26\ \text{m/s}}{a}$$

  for a bubble of radius $a$ (1.2 to 4.2 kHz here, bubbles of 0.8 to 2.7 mm).
  Following van den Doel, it is a sine whose amplitude decays at
  $d = 0.043f_0 + 0.0014f_0^{3/2}$ per second (viscous, thermal and radiation
  damping), rising in pitch as it settles, $f(t) = f_0(1 + 0.1\,d\,t)$.
- **The wash.** Rain far enough away is no longer separate impacts, just noise:
  pink noise (equal energy per octave, Kellet's filter) through a 120 Hz
  high-pass and a 4 kHz low-pass. Many independent impacts add in power, so
  its level goes as the square root of the rate, $0.12\sqrt{R/6}$, smoothed
  over 1.5 s. It breathes a little (up to ±40%, from noise smoothed at 6 Hz,
  and a little more in wind) as sheets of rain come and go.
- **The roar.** In a downpour rain drums on the roofs: pink noise through a
  250 Hz low-pass, fading in from 8 mm/h and full at 28 mm/h.

In the offline tests the old version of this, a hiss over a regular stream of
identical plinks, peaked at 2 to 8 kHz through the window; the new one is
roughly flat from 125 Hz to 4 kHz and 17 dB down at 8 kHz.

### The gutter

The gutter fills while it rains, toward $\text{clamp}(R/8)$ with a time constant
of 20 s, and drains with one of 45 s, so the dripping carries on for a minute
or two after the rain stops. Three outlets (into puddles, onto a sill), each
with its own place, loudness, drop radius (2 to 4.5 mm) and pace (a base
interval $T_0$ of 0.35 to 1.3 s), drip every $T_0\,g^{-0.8}\,U(0.8, 1.25)$
seconds, $g$ the gutter level: faster as it fills, never quite regularly. Each
drip is a soft plop (noise through a 500 to 900 Hz band-pass, decaying over
3 ms) and, three times in four, a bubble at its Minnaert frequency,
$3.26/r_\text{mm}$ kHz ±6%: a plink from a small drop, a bloop from a big one.

### Wind

The wind you hear is the gusting wind of the simulation (see
[Wind](#wind)), $|W|$ smoothed over 0.3 s: pink noise through a low-pass at
$160 + 260|W|$ Hz plus 0.6 of a band-pass at $300 + 140|W|$ Hz (Q 2), so gusts
are both louder and brighter, at a level of

$$\text{clamp}\Big(\frac{|W| - 0.25}{2.75},\ 0,\ 1.2\Big)^{1.5}$$

which is silent in a breeze and swells and dies with each gust. Above
$|W| = 1.7$ it **whistles** through a gap in the frame: noise through a narrow
band-pass (Q 30) at $520 + 160(|W| - 1.6)$ Hz, at a level of
$\min(|W| - 1.7, 1.3)^2$. The whistle is at the window itself, so the window
filter doesn't touch it.

### Cars

Now and then a car swishes past on the wet street, at exponential intervals
averaging $\big(35 + 120(1 - \text{daylight})\big)/C$ seconds, where $C$ is
**Cars passing**: at the default 0.5, about one every 70 s by day and one every
five minutes at night. Each passes at 8 to 16 m/s, 8 to 30 m away. With
$x = (t - t_c)\,v/d$ its time from the closest point, in units of the time it
takes to swing 45° off,

$$a \propto \frac{1}{d\,\sqrt{1 + x^2}}\,(0.3 + 0.7\,\text{wet}), \qquad \text{pan} = 0.85\,\frac{x}{\sqrt{1 + x^2}}$$

so it grows and fades as one over its distance and pans across. Its sound is
the tyres' hiss (noise through a band-pass at 0.8 to 1.5 kHz, Q 0.6) and a
rumble (noise below 110 Hz). It is brighter coming than going (a low-pass at
$3500 - 1500\tanh x$ Hz): the tyres throw their spray backwards, and the
Doppler shift. A wet road is louder: it gets wet over about a minute of rain
(time constant 60 s) and dries over a quarter of an hour (900 s). A pass lasts
$9\,d/v + 2$ seconds.

### Echoes

The far impacts, bubbles, drips and cars send part of their sound to a small
**feedback delay network** (Stautner and Puckette; Jot), standing in for the
buildings across the street: four delay lines of 1601, 1999, 2377 and 2843
samples (33 to 59 ms, no common factors), mixed through a 4 × 4 Hadamard
matrix (which keeps the energy and feeds every line into every other), each
with feedback gain $10^{-3L/(T_{60} f_s)}$ for $T_{60} = 0.9$ s and a 3 kHz
one-pole low-pass. It comes back at half level.

### Through the window

Everything outside is heard through the glass unless **Window open** is
turned up. Glass passes sound less the higher it goes, by about 6 dB an octave
(the **mass law**: transmission loss $\approx 20\log_{10}(m f) - 47$ dB for a
pane of $m$ kg/m²), except near its **coincidence frequency**, where bending
waves in the pane travel as fast as sound in the air and it lets much more
through: $f_c \approx 12/h_\text{mm}$ kHz for glass, about 3 kHz for a 4 mm
pane (Fahy). The shut window is a one-pole low-pass at 650 Hz followed by a
+8 dB peak at 3 kHz (Q 1.4). Window open blends toward the unfiltered sound,
$y = y_\text{shut} + o\,(y_\text{open} - y_\text{shut})$, smoothed over 0.3 s.
The taps on the glass and the thunder are not filtered (the taps are on the
pane; thunder has its own distance filter).

### Thunder

Thunder is the sound of the whole channel, kilometres long, so it arrives from
each part at a different time. After Farnell (*Designing Sound*) and Few
(*Acoustic radiations from lightning*), each thunder has:

- a **crack** if the strike is within 2.5 km: band-passed noise around 1.4 kHz
  decaying over 120 ms;
- a train of 60 to 140 **N-waves**, the shock waves from different parts of the
  channel: each a sharp rise and a straight fall from +1 to −1, 2 to 40 ms long
  (absorption stretches them with distance), bunched toward the start
  ($t = T u^{1.7}$) and fading ($e^{-t/0.45T}$), with the occasional loud one;
- a **rumble**: noise held for random stretches of 8 to 50 ms and smoothed by
  two low-passes at 90 to 210 Hz, which gives the irregular rolling of many
  overlapping arrivals;
- a **deep boom**: noise through three 60 Hz low-passes, swelling over the
  first second and dying away over most of the thunder.

Air absorbs high frequencies, so the further away, the darker it is: everything
passes through a low-pass at $150 + 4500(1 - d/12)$ Hz. One reflection off the
ground or buildings, 0.25 to 0.6 s later at 35%, adds space. The whole thunder
lasts $T = 3$ to $13$ s (longer when distant), and its level is
$0.9\,P/(1 + d/3)$.

**The window rattles.** A close strike's boom shakes the pane against its
frame, so it chatters in bursts while the boom and rumble are loudest. Knocks
come at random at up to $500\,S\,b(t)\,m(t)$ a second, where $b$ and $m$ are the
boom and rumble envelopes and $S$ the rattle strength, gated by the rumble's
held noise so they cluster into bursts (a twelfth of the rate between them).
Each knock is a sharp 3 to 8 sample pulse with a 0.6 ms buzz of glass on
frame, a 6 ms ring of the pane (low-passed at 5 kHz) and two modes of a loose
pane, at $500\,e^{2u}$ Hz ringing for 30 to 80 ms, so it is the pane itself you
hear. Like the taps it is on the glass, so the window filter doesn't touch it.
It is scaled by the Thunder and Drops on the glass levels.

A gentle `tanh` limiter on the output keeps a close clap from clipping.

## 6. References

- K. Kaneda, T. Kagawa, H. Yamashita, "Animation of Water Droplets on a Glass
  Plate", Computer Animation '93: critical weights, wet paths, water left behind.
  <https://home.hiroshima-u.ac.jp/kin/publications/CA93/wdroplet.html>
- N. Le Grand, A. Daerr, L. Limat, "Shape and motion of drops sliding down an
  inclined plane", J. Fluid Mech. 541 (2005); T. Podgorski, J.-M. Flesselles,
  L. Limat, "Corners, cusps, and pearls in running drops", PRL 87 (2001):
  sliding speed laws and the critical speed for trails.
  <https://arxiv.org/abs/physics/0503051>
- Stick-slip sliding of water drops on chemically heterogeneous surfaces.
  <https://arxiv.org/abs/1305.1162>
- New insights into the capillary retention force and the work of adhesion
  (the Furmidge relation). <https://arxiv.org/abs/2205.12180>
- K. Garg, S. K. Nayar, "Photometric model of a rain drop" (Columbia TR, 2004)
  and "Photorealistic rendering of rain streaks" (SIGGRAPH 2006): drops as
  165° wide-angle lenses, brighter than their background; streaks as motion
  blur. <https://www.cs.columbia.edu/CAVE/publications/pdfs/Garg_TR04.pdf>
- P. Rousseau, V. Jolivet, D. Ghazanfarpour, "Realistic real-time rain
  rendering", Computers & Graphics 30 (2006): refraction through a drop flips
  the scene. <https://faculty.digipen.edu/~gherron/references/References/Modeling/Realistic-real-time-rain-rendering.pdf>
- J. S. Marshall, W. M. Palmer, "The distribution of raindrops with size",
  J. Meteorology 5 (1948).
- D. Atlas, R. C. Srivastava, R. S. Sekhon, "Doppler radar characteristics of
  precipitation at vertical incidence", Rev. Geophys. 11 (1973): the terminal
  speed fit to Gunn and Kinzer (1949).
- V. A. Rakov, M. A. Uman, *Lightning: Physics and Effects* (2003), and "Lightning
  Parameters for Engineering Applications" (CIGRE): strokes per flash,
  interstroke intervals, continuing current.
  <https://peg.atis.org/wp-content/uploads/2018/09/Lightning-Parameters-for-Engineering-Applications-1.pdf>
- A. Farnell, *Designing Sound* (MIT Press, 2010), practical 17, "Thunder".
  <https://mitp-content-server.mit.edu/books/content/sectbyfn/books_pres_0/8375/designing_sound.zip/practical17.html>
- A. A. Few, "Acoustic radiations from lightning", in *Handbook of Atmospheric
  Electrodynamics* (1995).
- S. Liu, H. Cheng, Y. Tong, "Physically-based statistical simulation of rain
  sound", SIGGRAPH 2019: rain sound as many small impacts and bubbles.
- L. Bebber, "Rain & Water Effect Experiments", Codrops (2015): splitting large
  simulated drops from small stamped ones.
  <https://tympanus.net/codrops/2015/11/04/rain-water-effect-experiments/>
- NOAA Global Monitoring Laboratory, "General solar position calculations":
  the equation of time, declination and elevation.
  <https://gml.noaa.gov/grad/solcalc/solareqns.PDF>
- X. Noblin, A. Buguin, F. Brochard-Wyart, "Vibrated sessile drops: transition
  between pinned and mobile contact line oscillations", Eur. Phys. J. E 14
  (2004); P. Brunet, J. Eggers, R. D. Deegan, "Vibration-induced climbing of
  drops", PRL 99 (2007): vibration frees pinned contact lines.
- C. Josserand, S. T. Thoroddsen, "Drop impact on a solid surface", Annu.
  Rev. Fluid Mech. 48 (2016): spreading sheets, rim breakup, prompt and
  corona splashes, and splashes into a liquid film.
- K. van den Doel, "Physically based models for liquid sounds", ACM Trans.
  Applied Perception 2 (2005): bubble damping and the rising pitch of a
  bubble near the surface.
- M. Minnaert, "On musical air-bubbles and the sounds of running water",
  Phil. Mag. 16 (1933): the resonance of a bubble.
- J. H. McDermott, E. P. Simoncelli, "Sound texture perception via statistics
  of the auditory periphery", Neuron 71 (2011): rain and other textures are
  heard by their statistics, so random event streams, not loops or fixed
  resonances.
- F. Fahy, P. Gardonio, *Sound and Structural Vibration* (2nd ed., 2007): the
  mass law, the coincidence frequency and the modal density of plates.
- J. Stautner, M. Puckette, "Designing multi-channel reverberators", Computer
  Music J. 6 (1982); J.-M. Jot, A. Chaigne, "Digital delay networks for
  designing artificial reverberators", AES 90th Convention (1991): the
  feedback delay network.
- P. Kellet, "Filter to make pink noise from white" (musicdsp.org, 1999).
- Microsoft Docs: Desktop Duplication API, DirectComposition,
  `SetWindowDisplayAffinity`, waveOut.

## Project layout

| Path | Contents |
| --- | --- |
| `src/main.cpp` | Window, Direct3D 11, desktop capture, render passes, hotkeys, main loop |
| `src/rain_sim.h`, `src/rain_sim.cpp` | Water on the glass: drops, splashes, flung drops, trails, mist, wind, the wiper, the finger |
| `src/sky.h`, `src/sky.cpp` | Beyond the glass: showers, lightning and the shaking from thunder, daylight, the rain outside |
| `src/sound.h`, `src/sound.cpp` | Sound synthesis (glass taps, rain outside, gutter, wind, cars, thunder, the window) and playback |
| `src/controls.h`, `src/controls.cpp` | Tray icon, menu, settings window, `raindesk.ini` (one table drives the sliders and the ini) |
| `src/resource.h` | Resource ids |
| `res/` | App icon (`raindesk.ico`, a PNG copy) and the `.rc` that builds it and the shaders into the exe |
| `shaders/rain.hlsl` | All GPU shaders: drop splats, mist, composite, flash, streaks, wiper (built into the exe) |
| `CMakeLists.txt`, `build.bat` | Build |
| `sign.bat` | Signs the exe after each build; `sign.bat /check` checks it |
| `LICENSE` | MIT license |

## License

MIT. See [LICENSE](LICENSE).
