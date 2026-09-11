# psobb_patches

Patches for Phantasy Star Online: Blue Burst (2004) PC Game.

These patches are designed to work with the [MTethVer12513](https://github.com/anzz1/TethVer12513_Multi/releases/latest) (1.25.13) multilingual client.

To install, simply extract [psobb_patches.zip](https://github.com/anzz1/psobb_patches/releases/latest/download/psobb_patches.zip) archive to the game folder.

## BetterSleep
Significantly lowers CPU usage by replacing the frame limiter's busy loop with a smart sleep algorithm using the [precisesleep](https://github.com/anzz1/precisesleep) technique.

## MoreSaveSlots
Increases the number of save slots from 4 to 20. Configurable to up to 127 slots by changing the [SLOT_COUNT](https://github.com/anzz1/psobb_patches/blob/master/psobb_moresaveslots/dllmain.c#L14) definition and recompiling.
Requires a compatible server such as [newserv](https://github.com/fuzziqersoftware/newserv).

Credits to [fuzziqersoftware](https://github.com/fuzziqersoftware) for the patch.

## WideScreen
Runs the game in borderless fullscreen mode for improved compatibility with modern systems and multiple displays, allows the use of widescreen resolutions, and upgrades DirectX 8 to DirectX 9 for improved performance and shader support.

### Display mode
Set `DisplayMode` in `widescreen.cfg`, or pick it in the options menu:

- **`borderless`** (default) — a borderless window filling the monitor the game launches on, with a windowed D3D9 device behind it. Alt-tabs instantly and ignores `WindowWidth`/`WindowHeight`.
- **`fullscreen`** — switches the *display* to `WindowWidth` x `WindowHeight` (highest refresh rate available at that size) and fills it. Use it to run below your desktop resolution and let the monitor scale. The desktop is restored when you quit. If the adapter has no such mode, it stays at the desktop resolution, which is simply borderless. Note this is not an *exclusive-mode* device — see `notes/psobb-client-map.md` for why that cannot work on this client — so alt-tab is as safe here as anywhere else.
- **`windowed`** — a fixed-size titled window of `WindowWidth` x `WindowHeight`, centred.

The older `Windowed=0/1` key is still read for existing configs; `DisplayMode` wins when both are present.

Credits to [tofuman](https://github.com/tofuman0) for the offsets and [crosire](https://github.com/crosire) for the d3d8to9 project used as the base.

Several post-processing effects are implemented via shaders to improve graphical fidelity.  
These effects can be toggled by editing `widescreen.cfg`.

- **MSAA** (Multisampling Anti-Aliasing)  
Reduces aliasing, smoothing jagged edges.

- **SMAA** (Subpixel Morphological Anti-Aliasing)  
Further reduces aliasing at a subpixel level. More information in the [original research](https://www.iryoku.com/smaa/).

- **SSAO** (Screen Space Ambient Occlusion)  
Improves shadows by occluding ambient light according to the scene geometry.

- **Cel Shading**  
Improves the contrast of models and textures by emphasizing dark lines.

- **Depth of Field**  
Simulates a natural look by making far-away objects appear subtly out-of-focus while in contrast closer objects appear sharper.

- **High Dynamic Range Tone Mapping**  
Adjusts the color range of the scene to darken blacks and brighten whites, to more accurately replicate the deeper colors of a [CRT](https://en.wikipedia.org/wiki/Cathode-ray_tube) display for which the game's art was originally designed for. Colors will appear more vivid and less washed out on a modern LCD display. If you are using a CRT display, you might want to turn this feature off.

## LargeAssets
Increases the asset size limit from 0.59MB to 100MB. Allows loading large custom assets such as high definition texture packs and custom maps.

Credits to [Solybum](https://github.com/Solybum) for the patch.

## RightStickCamera
Adds right-stick camera control. Base PSO has no free camera — the right stick only navigates menus and the pad config's "Camera" binding is re-centre — so this is a camera being built, not a binding being exposed.

It does **not** rotate the view matrix. The client already has a full third-person follow camera with its own smoothing and map-geometry collision; this plugin hooks one call inside that camera's per-frame update and rotates the eye point the auto-camera just chose, about the point it is looking at. Everything downstream is derived from those two points, so the wall collision, the smoothing, the minimap heading, sprite billboarding, and camera-relative movement and lock-on all follow with no extra work.

It holds an **absolute world angle**, not an offset. The chase camera re-aims itself as you move, so an additive offset swings the view on its own -- harmful in combat. While the stick is deflected the plugin also rotates the eye directly onto that angle, **preserving whatever distance and height the client has chosen**, which is what makes it stop dead when you let go instead of rubber-banding. Distance, height and wall collision stay entirely the client's business.

Settings live in `widescreen.cfg`:

| Key | Default | Meaning |
|---|---|---|
| `RightStickCamera` | `0` | Master on/off. **Off by default** -- the right stick doing nothing is this client's existing behaviour, so players opt in. Exposed in the launcher as **Right-stick camera (modern controls)** and in `corellia-options.sh` on the Deck. Applied live. |
| `ChaseCam` | `0` | `0` Enabled -- eases back behind you as you move (25 deg/s, measured off Ephinea at 26). `1` Disabled -- holds the angle you give it. A stale `2` from the old three-mode list is migrated to Disabled. |
| `RightStickSensitivity` | `100` | Percent, scaling both turn speeds. |
| `RightStickSpeedSlow` / `RightStickSpeedFast` | `78` / `162` | Degrees/second below and above the split. |
| `RightStickSpeedSplit` | `50` | Percent deflection at which the speed steps up. |
| `RightStickDeadzone` | `10` | Percent of full stick travel ignored around centre. |
| `RightStickInvertX` / `RightStickInvertY` | `0` | Invert each axis. |
| `RightStickPitch` | `0` | Vertical look. Off by default -- see below. |
| `RightStickSnapWhileSteering` | `1` | Rotate `camera_source` directly while the stick is deflected, preserving the client's distance and height. This is what removes the lag; `0` falls back to commanding the desired point only, which trails ~25 deg behind the stick. |
| `RightStickSnapFrames` | `5` | Frames to ease onto the commanded angle when steering starts. Rotating the eye moves it sideways, so correcting the accumulated follow lag in one frame reads as a whip. |
| `RightStickRequireAttachedCamera` | `1` | Do not drive the camera when there is no character to follow -- the hook runs in **every** scene, so without this the stick swings the title screen, character select, ship select and the loading screen. |
| `RightStickAttachedFrames` | `10` | Frames the camera must have nothing attached before we believe it. Debounced so a transient null during a warp cannot drop the held angle mid-play. |
| `RightStickCameraLerp` | `0` | Override the client's own camera lerp (`+0x1BC`), percent per frame. **Leave at 0.** It is the distance/collision smoothing, not the angular lag; forcing it drives the eye into scenery. |
| `RightStickFollowNear` / `RightStickFollowFar` | `100` / `100` | Follow band, as a percentage of the chase camera's distance. Both `100` (the default, and the only configuration that has been measured) means the camera tracks rigidly at the client's own distance. |
| `RightStickFreezeChase` | `0` | Hold the eye distance and height while steering. Off -- the client owning distance is what fixed the framing jumping about between runs. |
| `RightStickReturnSpeed` | `25` | Degrees/second the camera drifts back behind you **while moving**. Set from `ChaseCam`; `0` holds the angle indefinitely. |
| `RightStickYawLimit` | `180` | Degrees the camera may sit from behind you. `180` (default) = no limit. |
| `RightStickAlwaysEngaged` | `0` | Take the camera immediately instead of on first stick touch. Set from `ChaseCam`. |
| `RightStickRecentreTrigger` | `1` | Which trigger recentres: `0` none, `1` LT, `2` RT, `3` either. |
| `RightStickRecentreMask` | `0` | Raw XInput button bitmask that also recentres, if a trigger is not what you want. |
| `RightStickSuppressMask` | `0x820` | Menu-state bits that mean "leave the camera alone". |

### ⛔ Known issue: PS5 (DualSense) controllers

**Reported from play 2026-09-10; not yet reproduced, no DualSense on hand.** The right stick does not
drive the camera correctly on a PS5 pad. The exact failure is not characterised yet -- "not handling it
properly" could be no response at all, wrong axes, or a stuck axis.

Leading hypothesis: **the plugin reads XInput directly**, and a DualSense in its native mode is not an
XInput device. `XInputGetState` would simply never see it, so the camera would never engage. Xidi ships
with our builds and presents an XInput pad *to the game's DirectInput*, which is why the client itself
works with such a pad -- but that does not make the physical controller an XInput device for our own
reads. Steam Input or DS4Windows would mask this by presenting the pad as an Xbox 360 controller, so
whether it works may depend on how the player launched the game.

⚠ The obvious fallback is barred: `g_joyState` is the Pad Config screen's binding-capture buffer, not
gameplay input, and it freezes at its last value when that screen closes. A DirectInput path would need
its own device enumeration.

**To diagnose, no hardware needed on our side:** a DualSense player runs the diagnostic build and sends
`corellia_rightstickcamera.log`. The startup line reports whether `XInputGetState` resolved, and each
sampled line carries `slot=`, `conn=` and the raw `rx=`/`ry=` values. `conn=0` confirms the hypothesis
outright; live `rx`/`ry` that move the camera wrongly points somewhere else entirely.

**Vertical look is off by default.** It works and is clamped, but stacked on the chase camera's own
pitch the two end up solving for height at the same time and it reads oddly in play. Set
`RightStickPitch=1` to try it.

**The Pad Button Config screen reflects it.** While the camera is enabled, the two **Right Analog**
rows are greyed out, lose their selection cursor, and read `-- Camera --` instead of their bindings —
because whatever they are bound to is not reaching the game. Turning the feature off restores them.

The greying is the client's own mechanism, not an invented one: it is exactly what the game does to
disable a menu entry elsewhere (colour `0xFF909090`, and the item's cursor bit cleared).

Nothing is written to your key config. The bindings are left exactly as you set them — which matters
on Blue Burst, where that config syncs to the server, so clearing it would persist after you switched
the camera back off.

**Turning is two fixed speeds, not proportional control.** Past the deadzone the camera turns at a
constant slow speed, stepping up to a constant fast speed past half deflection. That is measured from
Ephinea rather than invented: binning their camera's turn rate against actual stick deflection gives
two flat plateaus (~78°/s and ~162°/s) with a step near half, not a ramp.

It matters more than it sounds. Proportional control means a small nudge barely moves the camera and
the rate changes constantly under your thumb; two fixed speeds give a decisive, predictable rate for
fine aiming and a second one for spinning round.

**The toggle applies while the game is running.** Every setting here is re-read a few seconds after
`widescreen.cfg` changes, so switching between classic and modern controls in the launcher's Options
window takes effect without restarting the client and without reinstalling or re-patching anything.
Switched off, the plugin leaves the client's camera entirely alone — the hook stays in place so the
setting remains changeable, but it returns before reading or writing any camera state.

**The camera holds a world-space position, and only follows you loosely.** Inside the follow band it
does not move at all — you walk around inside a still frame, growing and shrinking, rather than being
welded to the middle of the screen. Only when you reach the near or far edge of the band does it move,
and only enough to put you back on the edge.

That distinction matters more than any of the rates. Recomputing the eye as *character + offset* every
frame looks identical while you stand still and is completely different in motion: it pins you to one
screen position and swings the whole world around you. `RightStickFollowNear`/`Far` set to `100` both
restores that older rigid behaviour if you want to compare.

## Two settings, not four

Only two decisions matter, and the launcher exposes exactly those:

- **Right-stick camera** — on or off. Off is classic PSO, and nothing is patched into the camera path.
- **Chase cam** — how the chase camera and you share control:

| Mode | Behaviour |
|---|---|
| **Enabled** | The chase camera stays in charge. You can swing the view, but it is reclaimed in under a second. Closest to stock PSO with a nudgeable camera. |
| **Hybrid** *(default)* | You aim it, and it eases back behind you over a few seconds of running. Holds while you stand still. |
| **Disabled** | The camera holds the angle you give it and never reclaims it. The chase camera still chooses distance and height. |

The individual keys (`RightStickReturnSpeed`, `RightStickAlwaysEngaged`, `RightStickFreezeChase`)
still work and **override** whatever the mode selected — the mode only supplies their defaults — so
tuning stays possible without adding more positions to the dropdown.

**The camera holds an absolute world angle, and it is not an offset from the chase camera.** Each
frame the plugin reads the angle the chase camera just chose and cancels it, so the view stays
exactly where you aimed it while you run and turn. The chase camera keeps full control of distance
and height; only yaw is taken over, and only once you have actually touched the right stick — until
then the client behaves exactly as it does without this plugin.

That is the point of a right-stick camera: aim the view with one thumb while positioning the
character with the other. An earlier build added a fixed *offset* to the chase camera's yaw instead,
which meant the view swung around on its own as the chase camera re-aimed itself — mildly odd while
exploring, actively harmful in combat.

`RightStickFreezeChase` (on by default) takes the chase camera out of the picture almost entirely:
once you take control, the eye holds the distance and height it had at that moment, so the camera
simply orbits your character. Measured over a play session, the chase camera moves the eye's height
barely at all (6–8 units) but swings its distance from 19 to 77 — that constant push-and-pull is
what makes the camera feel like it is fighting you, and this is what stops it.

Note what it does **not** turn off, deliberately: the look-at point still tracks your character, and
the client's own wall collision and camera smoothing still run downstream. Those are the parts of the
chase camera worth keeping.

`RightStickReturnSpeed` eases the held angle back behind you while you move, then hands the camera
to the chase cam once it arrives. It holds while you stand still, so looking around stationary works.
The default of 25°/s takes roughly 3–4 seconds to recover a 90° turn — matching the feel of Ephinea's
"Chase Cam: Hybrid". An earlier build shipped this at 90°/s, which was fast enough to fight the
player; the idea was sound, the rate was not. `0` disables the drift entirely.

`RightStickAlwaysEngaged=1` takes the camera on the first frame rather than waiting for you to touch
the stick, and re-frames on area changes. Combined with `RightStickFreezeChase=0` this approximates
Ephinea's "Chase Cam: Disabled" — a fixed world angle with the framing still chosen by the game.

**Recentring** hangs off the client's own Camera binding (`PAD BUTTON7` in the default pad config).
Because the offset is added on top of the chase camera, a recentre that does not also clear the
offset lands somewhere arbitrary; clearing it on the same press puts the camera where the player
expects. PSO sees Xidi's virtual pad, so "BUTTON7" is a Xidi mapper index rather than an XInput
button — under `StandardGamepad` that numbering puts the triggers at 7 and 8, which matches PSO's
defaults (Prev/Next Page on the shoulders), hence left trigger as the default.

Input comes from **XInput**, read directly, not from the game's own controller state. PSOBB's DirectInput device is opened `DISCL_EXCLUSIVE` so it cannot be shared, and the client's `DIJOYSTATE2` buffer turns out to be the Pad Button Config screen's binding-capture buffer — refreshed only while that screen is open, then frozen at its last value. Since this install ships Xidi (which exists to present an XInput pad to a DirectInput game), the physical controller is an XInput device by construction.

With no controller attached the plugin does nothing: XInput reports connection state explicitly, so "no pad" and "pad at rest" are different answers.
