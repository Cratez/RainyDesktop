# How raindesk works

This document covers the three parts of raindesk: the overlay window that
can see through itself, the simulation that decides where water is, and the
renderer that turns that water into refracting drops. It also explains the
physics the simulation is built on.

## 1. The overlay

`src/main.cpp`

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
swap chain with **premultiplied alpha**. Pixels with no water are written as
fully transparent, so the desktop shows through untouched.

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

### Threads and frame loop

The window thread owns every window (overlay, tray, settings) and only pumps
messages. Dragging the settings window or opening the tray menu puts that
thread in a modal loop, so the simulation and rendering run on a separate
render thread that never waits on window messages. The window thread hands it
a copy of the settings under a mutex, and wipe, dry and screenshot requests as
atomic flags.

The swap chain allows only one queued frame (`SetMaximumFrameLatency(1)` and
its waitable object). Each frame on the render thread:

1. Wait until the compositor can take a new frame.
2. Apply settings to the simulation and start an auto wipe if one is due.
3. `RainSim::Update(dt)` advances the simulation and produces splat lists.
4. `Render()` captures the desktop, runs the GPU passes below and presents with
   vsync.

Capturing right after the wait keeps the refracted image as fresh as
possible. It is still one refresh behind the real screen, because Desktop
Duplication only hands over a frame after the compositor has shown it.

## 2. The simulation

`src/rain_sim.h`, `src/rain_sim.cpp`

The simulation runs on the CPU in screen pixels. Sizes are written for a 1440p
screen, about 0.233 mm per pixel on a 27" panel, and scaled by
`s_ = height / 1440`. Each frame it produces three lists of **splats**, which
are ellipses with a centre, radii and a water height:

- **drops**: every drop on the glass, redrawn from scratch each frame.
- **mist**: new tiny droplets, stamped once into a persistent texture.
- **erase**: where water was wiped off that persistent texture this frame.

### Two kinds of water

Tracking every droplet individually is expensive, so water is split in two
(the same split as the Codrops rain experiments):

- **Drops** are simulated individually: position, radius, velocity, shape.
  There are a few thousand of them, found through a uniform spatial grid
  (`BuildGrid`, `ForNeighbors`).
- **Mist** is anything smaller than about 1.6 px in radius. It is never
  simulated. It is stamped into a texture that slowly
  dries (about a minute), and running drops and the wiper erase it where they
  pass.

### The glass

The pane itself has two properties on a coarse grid of 6 px cells:

- **Affinity** (`Affinity`): fixed value noise standing in for dirt and
  coatings. Some spots grip water harder and pull running drops toward them.
- **Wetness** (`Wetness`, `Wet`): when each cell was last wetted. Fresh film
  dries over about two minutes. Film younger than 0.75 s is ignored, so a drop
  doesn't react to its own wake.

### Lifecycle of a drop

**Landing.** Rain arrives at `10 × rain amount` drops per second, with radius
between the smallest and largest raindrop settings, biased toward small drops.
About a third of impacts throw a spray of small satellites. A drop that lands
touching another drop merges into it.

**Resting.** A resting drop:
- grows slowly as it gathers mist around it, and clears the mist under itself
  every few seconds;
- merges with any resting neighbour it touches;
- keeps the shapes of the drops that merged into it (see Shapes below);
- sags longer and narrower as it gets close to letting go.

**Letting go.** Each drop lets go at its own **release radius**:

```
release = pinRadius × pin × (0.85 + 0.3 × affinity) × (1 − 0.3 × wetness)
```

`pin` is a per-drop factor between 0.85 and 1.2 that stands in for how
ragged its contact line is. Most drops let go near the typical size, and a few
hold on noticeably longer. Water-loving spots grip harder, and wet film grips
less.

**Running.** Speed depends steeply on how far the drop is past its sliding size:

```
slideR = 0.8 × release
v      = v0 × ((r / slideR)² − 1) × (1 + 0.8 × wetness ahead)    capped at ~40 cm/s
```

A drop that only just broke loose creeps along. One that swallowed its
neighbours is gone almost instantly. Speed settles within a fraction of a second,
because viscous drag on a water drop is strong.

While running, a drop:
- **steers** (`Steer`) by scoring five downhill directions. It prefers wet
  glass, water-loving patches and its current heading, with some randomness,
  so it meanders, and later drops reuse earlier paths. Fast drops wander less.
- **sticks and slips** on dry glass: random short stalls, more often with
  higher glass grip. On wet glass it runs smoothly.
- **swallows** every drop it touches. Momentum is conserved, so it lurches,
  then speeds up because it is heavier.
- **sheds water.** On dry glass the trailing film breaks into a line of beads.
  Fast drops leave them further apart, and drops on a wet path leave fewer.
  About half of runs (`streaky`) also draw out a thin **rivulet** (see
  below), which slowly drains the drop.
- **stops** once it has shed below its sliding size, unless it is on a wet
  path, and becomes a resting drop again.
- leaves the simulation when it runs off the screen.

**Shapes.** A resting drop is drawn as the union of up to four blobs (`Lobe`).
When two drops merge, both drops' blobs are kept, re-centred on the combined
centre of mass, and folded together if there are more than four. The pinned
contact line only slowly lets the drop pull itself round (time constant
`60 s × pin`), so merged drops stay peanut-shaped or ragged for a long time.
New drops often get a small bulge. Running drops are drawn as teardrops: a
head plus two tail blobs that stretch with speed.

**Size cap.** A merged drop's radius is capped at 2.2× the pin radius. Resting
drops never get that far, because they let go well before it.

### Rivulets

`LayRivulet`, `UpdateRivulets`, `EraseRivulets`

A rivulet is a thin line of water about half a drop wide, kept as a chain of
nodes about a pixel apart. It meanders slightly and its width wanders. Each
frame the nodes are drawn as small drop splats, so the rivulet refracts like
any other water. A rivulet left on the glass is unstable and goes through
these stages, each scaled by the **Streak life** setting:

1. **Intact** for about a second after it is laid down.
2. **Pinching.** Surface tension (the Plateau-Rayleigh instability) swells
   the rivulet into bulges about twenty widths apart and thins the necks
   between them. A neck that gets too thin snaps.
3. **Beading up.** A segment that has pinched off pulls itself into one round
   droplet of the same water, which then stays on the glass like any other
   drop.
4. **Draining.** From about 3.5 s on, what is left thins out and disappears.

Because the top of a rivulet is the oldest part, the stages travel down it:
the top drains, the middle pinches into segments, and the newest part is still
a continuous line. Later drops and the wiper soak up any rivulet they cross.

### The wiper

`StartWipe` and `UpdateWiper` model a car-style wiper pivoting just below the
bottom centre of the screen, long enough to reach the top corners. One wipe is
a stroke to the right and back, about 1.1 s each, with eased motion and a slight
overshoot past horizontal.

Each frame, every drop in the wedge swept since the last frame is moved to just
ahead of the blade. Pushed drops touch each other and merge into a ridge. At the
end of a stroke the ridge is past the bottom edge and leaves the screen. Behind
the blade the mist is erased and the glass is marked wet. A few fine droplets are
left behind, because real blades aren't perfect.

## 3. Rendering

`shaders/rain.hlsl`, `Render()` in `src/main.cpp`

All shaders live in one file that is compiled at runtime and recompiled
whenever it changes.

### Water textures

Two full-screen `R16G16_FLOAT` render targets hold, per pixel,
`(Σ height × field, Σ field)`. Here `field = (1 − q)²` is each splat's smooth
falloff and `q` is the squared distance from its centre.

- **Water** is cleared every frame, and drop splats are **added** (`PSDrop`,
  additive blend). Summed fields behave like metaballs: nearby blobs fuse into
  one smooth outline, which is how lobes, tails and merging drops become a
  single shape.
- **Mist** is persistent. Each frame it is dried a little (`PSDecay`), new mist
  and streaks are stamped with a **max** blend (`PSMist`), and wiped areas are
  multiplied down (`PSErase`).

### Composite (`PSComposite`)

For each screen pixel the two textures are added, then:

1. **Outline.** The drop edge is where the summed field crosses `kEdge = 0.12`,
   antialiased with `fwidth`.
2. **Shape.** The field is squashed smoothly into 0..1, so heavily overlapping
   blobs don't plateau into a flat disc. From it, the shader recovers how far
   the pixel is from the drop's centre and builds a **spherical cap** that meets
   the glass at the outline. A sphere keeps the refracted image smooth, with no
   rings inside the drop.
3. **Normal** from the cap's gradient (central differences).
4. **Refraction.** Each drop acts as a small lens: it shows a shrunken,
   upside-down, out-of-focus view of a patch about 2.6 drop radii wide. The
   shader samples the desktop at `pixel − normal.xy × reach`, at a mip level
   that grows with drop size and surface steepness.
5. **Shading.** The lower half is lit slightly by light gathering from above. The
   rim darkens where light reflects inside the drop, most strongly along the
   top edge. Larger drops get a small highlight from a light above-left; mist
   gets none, because on tiny droplets it reads as noise.
6. Output is premultiplied colour and alpha.

### Wiper (`PSWiper`)

A full-screen pass, run only during a wipe, that draws the blade (a rubber edge
in a frame lit along one side) and its arm analytically from the pivot and the
angle. It is blended over the composite.

### Screenshot

`SaveScreenshot` draws the plain desktop (`PSDesktop`), then the composite and
the wiper over it, into an offscreen texture. It reads that back and writes a
PNG.

## Physics notes

**When a drop lets go.** A drop on a vertical pane is held by contact angle
hysteresis along its contact line, as in Furmidge's relation:

```
F_grip = γ · w · (cos θ_receding − cos θ_advancing)
```

For water on ordinary window glass, θ_a is about 50–60° and θ_r about 20–30°,
so the grip is about 0.022 N per metre of drop width. Gravity on a spherical cap
with a contact angle near 40° is `ρ g V` with `V ≈ 0.6 a³` and `w = 2a`. The
two balance at a contact radius `a ≈ 2.7 mm`. Drops wider than about 5–6 mm
can't stay put on a vertical pane, which is the source of `kPinRadius`. Grip
grows with width (∝ a) while weight grows with volume (∝ a³). That is why drops
hang on for a long time and then go suddenly, and why big drops never last.

**How fast it slides.** Above the threshold, the sliding speed of small drops
follows `Ca ∝ Bo − Bo_c`. Ca is the capillary number (speed × viscosity / surface
tension) and Bo is the Bond number (weight relative to surface tension). Since
`Bo ∝ a²`, speed grows roughly as `(a / a_c)² − 1`. This gives the slow creep
just past the threshold and the very fast fall of large drops. Stick-slip on dry
glass comes from the contact line catching on heterogeneities, as in the striped
surface experiments.

**Paths and trails.** The lattice model of Kaneda et al. (1993) introduced the
ideas used here: a static critical weight before a drop runs, a lower dynamic
one where it stops, preference for already-wet paths, and water left behind
along the way. The trail of beads is the receding contact line leaving film that
breaks up behind the drop.

**Optics.** A drop on glass is a small plano-convex lens. Scenery far behind
it appears shrunken and upside down inside the drop. Rays that leave the glass
at grazing angles reflect back inside, which is why drop edges look dark.

## References

- K. Kaneda, T. Kagawa, H. Yamashita, "Animation of Water Droplets on a Glass
  Plate", Computer Animation '93.
  <https://home.hiroshima-u.ac.jp/kin/publications/CA93/wdroplet.html>
- Stick-slip sliding of water drops on chemically heterogeneous surfaces.
  <https://arxiv.org/abs/1305.1162>
- New insights into the capillary retention force and the work of adhesion
  (Furmidge relation). <https://arxiv.org/abs/2205.12180>
- L. Bebber, "Rain & Water Effect Experiments", Codrops (2015): splitting large
  simulated drops from small stamped ones.
  <https://tympanus.net/codrops/2015/11/04/rain-water-effect-experiments/>
- Microsoft Docs: Desktop Duplication API, DirectComposition,
  `SetWindowDisplayAffinity`.
