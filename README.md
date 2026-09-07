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

The rotation is an *offset* on top of the auto-camera, which keeps full control of distance and height. It re-centres itself in cutscenes and wherever the client snaps the camera on its own.

Settings live in `widescreen.cfg`:

| Key | Default | Meaning |
|---|---|---|
| `RightStickCamera` | `1` | Master on/off. |
| `RightStickSensitivity` | `100` | Percent. 100 = 100°/second at full deflection. |
| `RightStickDeadzone` | `20` | Percent of full stick travel ignored around centre. |
| `RightStickInvertX` / `RightStickInvertY` | `0` | Invert each axis. |
| `RightStickPitch` | `0` | Vertical look. Off by default — see below. |
| `RightStickRecentreTrigger` | `1` | Which trigger recentres: `0` none, `1` LT, `2` RT, `3` either. |
| `RightStickRecentreMask` | `0` | Raw XInput button bitmask that also recentres, if a trigger is not what you want. |
| `RightStickSuppressMask` | `0x820` | Menu-state bits that mean "leave the camera alone". |

**Vertical look is off by default.** It works and is clamped, but stacked on the chase camera's own
pitch the two end up solving for height at the same time and it reads oddly in play. Set
`RightStickPitch=1` to try it.

**Recentring** hangs off the client's own Camera binding (`PAD BUTTON7` in the default pad config).
Because the offset is added on top of the chase camera, a recentre that does not also clear the
offset lands somewhere arbitrary; clearing it on the same press puts the camera where the player
expects. PSO sees Xidi's virtual pad, so "BUTTON7" is a Xidi mapper index rather than an XInput
button — under `StandardGamepad` that numbering puts the triggers at 7 and 8, which matches PSO's
defaults (Prev/Next Page on the shoulders), hence left trigger as the default.

Input comes from **XInput**, read directly, not from the game's own controller state. PSOBB's DirectInput device is opened `DISCL_EXCLUSIVE` so it cannot be shared, and the client's `DIJOYSTATE2` buffer turns out to be the Pad Button Config screen's binding-capture buffer — refreshed only while that screen is open, then frozen at its last value. Since this install ships Xidi (which exists to present an XInput pad to a DirectInput game), the physical controller is an XInput device by construction.

With no controller attached the plugin does nothing: XInput reports connection state explicitly, so "no pad" and "pad at rest" are different answers.
