// RightStickCamera 1.25.13 (59NL)
//
// Adds right-stick camera control. Base PSO has no free camera at all -- the right stick only
// navigates menus, and the pad config's "Camera" binding is re-centre -- so this is not a binding
// being exposed, it is a camera being built.
//
// WHY THIS IS SMALL, AND WHY IT IS NOT A VIEW-MATRIX HACK
// ------------------------------------------------------
// The obvious version -- rotate the view matrix in the d3d8 wrapper -- looks right for about five
// seconds and is wrong: it is visual only. Movement is camera-relative and lock-on is defined in
// terms of camera direction, so the game would still believe the camera never moved and the
// character would walk somewhere other than where the player is looking.
//
// The client already HAS a complete third-person follow camera, with its own smoothing, its own
// map-geometry collision, and a cached yaw that the minimap and every billboarded sprite read. Its
// per-frame update is:
//
//   UpdateDefaultNPCCameraState @ 0x004D3ABC
//     0x004D3B12  call 0x004D1FF4   ; the auto-camera writes desired_source / desired_target
//     0x004D3B17  <-- WE RUN HERE
//                 ...collision ray, then lerp current -> desired, then shake, then projection
//
// So the whole feature is: after the auto-camera has decided where it wants the eye, rotate that
// point about the look-at point. Everything downstream is *derived* from those two points and
// therefore follows for free -- the lerp, the wall collision, the cached yaw, the minimap heading,
// sprite billboarding, and camera-relative movement and lock-on. We are not fighting the
// auto-camera; we are transforming its output inside its own update, before anything reads it.
//
// ⚠ Do NOT try to drive the camera by writing y_rotation (+0x194). It is an OUTPUT: the client
// derives it from the source->target vector every frame via 0x004D6448. Writing it does nothing but
// desynchronise the minimap for one frame.
//
// Full derivation, and how every address below was confirmed against our own psobb.exe rather than
// taken from the psobb.io decompilation, is in notes/psobb-client-map.md ("Camera").

#define WINVER 0x0501
#define _WIN32_WINNT 0x0501

#define _CRT_NONSTDC_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define _NO_CRT_STDIO_INLINE
#include <windows.h>
#include "util.h"
#include "log.h"

// ---------------------------------------------------------------------------
// Addresses (59NL). psobb.exe has DYNAMICBASE off and its relocations stripped,
// so it can only load at 0x00400000 and these are absolute, not RVAs.
// ---------------------------------------------------------------------------
#define ADDR_UPDATE_CALL    0x004D3B12   // `call 0x004D1FF4` inside UpdateDefaultNPCCameraState
#define ADDR_SET_POINTS     0x004D1FF4   // set_global_camera_source_and_target(camera behaviour obj)
#define ADDR_CAMERA_STATE   0x00A48A54   // -> camera_state_struct*, 0x1D4 bytes
// Pad Button Config menu. The row loop's common tail runs once per row for all 16 rows, with the
// row index in EBX and that row's text widget in ECX, AFTER the row's value label has been written:
//
//   00790D9A  mov ecx, [edi+0x2C]      ; the row's text widget
//   00790D9D  call 0x0072E0E4          ; <- retargeted; we overwrite rows 2 and 3 first
//   00790DA8  cmp ebx, 0x10            ; 16 rows
//
// Hooking the shared tail rather than either label branch means one call site instead of two, and it
// does not matter which branch (axis names at 0x0097B264, button names at 0x0097B2E4) produced the
// text -- we simply replace it afterwards.
#define ADDR_PADROW_CALL    0x00790D9D   // `call 0x0072E0E4` -- the per-row tail
#define ADDR_TEXT_SPACE     0x0072E0E4   // __thiscall(ecx = widget), what that call went to
#define ADDR_TEXT_SETTEXT   0x0072DB60   // __thiscall(ecx = widget, wchar_t*, int) -- CALLEE cleans

// Which rows are the right analog axes. Row order is the menu's own: 0 Move L/R, 1 Move F/B,
// 2 Right Analog L/R, 3 Right Analog F/B, then the buttons. Confirmed against the in-game screen.
// Greying a row out, using the client's OWN mechanism rather than an invented one.
// ChatShortcutMenuYesNoWindow_SetItemColorById @ 0x00738A9C is how the client disables a menu entry:
//
//   *(u32*)(item + 0x24) = 0xFF909090;   // grey
//   *(u16*)(item + 4)   &= 0xFFFE;       // clear bit 0
//
// Both halves are corroborated by ListWindowObject_AddListItem @ 0x00735C90, which initialises
// +0x24 to 0xFFFFFFFF (so +0x24 is the colour) and sets bit 0 of the same flags word when the
// caller passes has_cursor (so bit 0 is the cursor/selectable bit).
//
// We replicate it inline rather than calling 0x00738A9C, because that function also requires bit 2
// of the flags word to be set and it is not known whether the pad config's rows have it -- a
// precondition that fails silently is worse than three field writes.
#define LIST_LINES_PTR      0x28         // list window -> array of line objects
#define LIST_NUM_LINES      0x8A         // short
#define ITEM_FLAGS          0x04         // ushort; bit 0 = has cursor / selectable
#define ITEM_VALUE          0x18         // the item_index passed to AddListItem
#define ITEM_COLOR          0x24         // ARGB, 0xFFFFFFFF when added
#define ITEM_GREY           0xFF909090   // the client's own disabled grey
#define OFF_MENU_LIST       0x24         // menu object -> the 16-row list window

#define PAD_ROW_RSTICK_X    2
#define PAD_ROW_RSTICK_Y    3

#define ADDR_MENU_FLAGS     0x00A489FC   // g_GenericMenuSubSelection
// ⛔ NOT USED, and deliberately so. g_joyState (DIJOYSTATE2, 0x110 bytes) lives at 0x00ADCC80, and
// the first two builds read the right stick from it. It does not work: it is the PAD CONFIG SCREEN'S
// BINDING-CAPTURE BUFFER, not gameplay input. Proof in our own binary -- the only three references to
// 0x00ADCC80 anywhere (0x842485, 0x8424D2, 0x8424EC) are inside the two poll routines themselves, and
// each poll routine has exactly one caller, both inside the config UI at 0x00790E16 / 0x007915E1.
// (PollJoystickStateWithFrameDelta compares per-axis deltas against 4096.0f -- "which axis did you
// just move?", i.e. binding capture.) So it is refreshed ONLY while that screen is open, and then
// FREEZES at the last value: measured in game, Z=34205 Rz=18100 unchanged for three minutes while
// the camera span. A stale axis is indistinguishable from a held stick, so this can never be a safe
// input source. Read the pad ourselves instead -- see the XInput section below.

// camera_state_struct offsets. Confirmed: 0x004D1FF4 itself writes +0x1A0/+0x1A4/+0x1A8 through the
// pointer at ADDR_CAMERA_STATE, and 0x004D2158 reads +0x184 minus +0x178 as target-minus-source.
#define OFF_SOURCE          0x178        // vec3f, current eye (lerped toward desired)
#define OFF_TARGET          0x184        // vec3f, current look-at
#define OFF_Y_ROTATION      0x194        // uint, PSO angle units -- OUTPUT ONLY, see header note
// ⭐ TWO lerp factors sit here, and ONLY +0x1BC governs camera_source. Setting +0x1B8 to 1.0 leaves
// the trailing completely unchanged; +0x1BC is the one whose value shows up in the motion.
// 📏 Proof, 6 clean stick releases in open ground: the post-release yaw error decays as a clean
// first-order lag whose fitted constant is 0.289/frame across 24 fits (spread 0.28-0.37), and a live
// probe read +0x1BC = 0.2890 bit-exact while +0x1B8 held our own 1.0. Same lag drags the distance
// (actual/commanded 0.894 while rotating, 1.000 idle) -- one lag, both axes.
#define OFF_LERP_A          0x1B8        // float; we set it too, but it is NOT what moves the eye
#define OFF_LERP_SOURCE     0x1BC        // float; THE one -- camera_source chases desired_source
#define OFF_DESIRED_SOURCE  0x1A0        // vec3f  <-- the one field this plugin writes
#define OFF_DESIRED_TARGET  0x1AC        // vec3f, the pivot we rotate about

// ---------------------------------------------------------------------------
// Input: XInput, read directly
//
// PSOBB holds its DirectInput device with DISCL_EXCLUSIVE, so we cannot open that. XInput is a
// separate API and is not affected. This install ships Xidi (Xidi.32.dll, Mapper Type =
// StandardGamepad), whose entire purpose is to present an XInput pad to a DirectInput game -- so the
// physical controller IS an XInput device, by construction, and XInputGetState will see it.
//
// This also removes the stale-value hazard that made the g_joyState approach unsafe: XInput reports
// connection state explicitly, so "no pad" and "pad at rest" are different answers rather than the
// same all-zero buffer.
//
// Loaded dynamically. A static import of xinput1_4.dll would refuse to start the client on a machine
// that only has an older one, which is a bad trade for a camera nicety.
// ---------------------------------------------------------------------------
#define XI_MAX_USERS        4
#define XI_RESCAN_FRAMES    120          // ~4s between scans while no pad is connected
#define XI_THUMB_SCALE      32767.0f     // sThumbRX/RY are SHORT, centred at 0

// XINPUT_STATE is {DWORD dwPacketNumber; XINPUT_GAMEPAD Gamepad;} and XINPUT_GAMEPAD is
// {WORD wButtons; BYTE bLeftTrigger; BYTE bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;}
// -- 16 bytes total. Declared by offset rather than including XInput.h, which would drag in a
// different SDK surface for two SHORTs.
#define XI_STATE_BYTES      16
#define XI_OFF_BUTTONS       4           // WORD
#define XI_OFF_TRIGGER_L     6           // BYTE 0..255
#define XI_OFF_TRIGGER_R     7           // BYTE 0..255
#define XI_OFF_THUMB_LX      8           // left stick -- used only to tell "is the player moving"
#define XI_OFF_THUMB_LY     10
#define XI_OFF_THUMB_RX     12
#define XI_OFF_THUMB_RY     14
#define XI_TRIGGER_ON       30           // XINPUT_GAMEPAD_TRIGGER_THRESHOLD

// Bits of g_GenericMenuSubSelection that mean "leave the camera alone".
//
// 0x820 is the mask the camera update itself tests to skip collision and smoothing entirely, so it
// is the client's own statement of "hands off".
//
// ⚠ This was 0x82C in the first build, which also suppressed on 0x0C. That was wrong. 0x0C merely
// selects the branch inside 0x004D1FF4 that writes the CURRENT points as well as the desired ones;
// it does not mean the camera is off limits. Measured in game 2026-09-07, bit 0x4 is set during
// ordinary play (live masks: 0x604, 0x204, 0x600), so 0x82C disabled the feature most of the time.
// Still a config key: if cutscenes turn out to need protecting, bits go back without a rebuild.
#define DEFAULT_SUPPRESS    0x820

// ⭐ TWO SPEEDS, NOT A RAMP. Measured off Ephinea while stationary, binning camera turn rate against
// actual stick deflection (628 samples, 22s):
//
//     |stick|  0.05 -> 0 deg/s        (deadzone, their CAMERA_DEADZONE = 10)
//              0.15 -> 81
//              0.35 -> 78             } flat, about 78 deg/s
//              0.50 -> 116            (transition)
//              0.55 -> 163
//              1.00 -> 161            } flat, about 162 deg/s
//
// Two plateaus with a step near half deflection -- NOT proportional control. That is very likely
// why theirs reads as smoother: a constant, decisive speed for fine aiming and a second constant
// speed for spinning round, with no need to modulate thumb pressure to hold a rate.
//
// Ours was a straight line from 0 to 100 deg/s with a 20% deadzone, so a small nudge of 0.2 turned
// at 20 deg/s where theirs turns at 78. That is the difference the player feels.
#define SPEED_SLOW_DEG      78           // deg/s below the split
#define SPEED_FAST_DEG      162          // deg/s at or above it
#define SPEED_SPLIT_PCT     50           // % deflection where it steps up

#define PI                  3.14159265358979323846f
#define DEG2RAD             (PI / 180.0f)

// How far the accumulated pitch may travel from wherever the auto-camera put it. Kept well short of
// straight up/down: the client itself special-cases a purely vertical source->target vector as a
// degenerate case it has to nudge out of, so there is no reason to steer into it.
#define PITCH_LIMIT_RAD     (55.0f * DEG2RAD)

// Hard stop on the FINAL vector, applied after the auto-camera's own pitch is included. The
// accumulator clamp above bounds our contribution; this bounds the total. sin(70 degrees).
#define PITCH_SIN_LIMIT     0.9397f

#define RSC_DIAG_EVERY      300          // frames between diagnostic lines -- see log.h

// How often to notice that widescreen.cfg changed, so the launcher's checkbox takes effect without
// restarting the client. 90 frames is ~3s at 30fps. The check itself is one GetFileAttributesEx --
// the file is only re-read when its write time actually moves, so the common case costs a syscall
// every three seconds and nothing else.
#define RSC_CONFIG_POLL     90

typedef struct { float x, y, z; } vec3f;

// ⚠ Required because this is the first plugin in the repo to use floating point. MSVC emits a
// reference to __fltused from any object file that touches a float -- it is not a function, just a
// marker the CRT defines so its startup code knows to initialise FP support. This project links no
// CRT (/kernel + OmitDefaultLibName), so nothing defines it and the link fails with LNK2001 even
// though every line compiled cleanly. On x86 the compiler prefixes C symbols with an underscore, so
// `_fltused` here is the `__fltused` the linker is asking for. The value is never read.
int _fltused = 0;

// ---------------------------------------------------------------------------
// Configuration (widescreen.cfg, same file and parser style as the other plugins)
// ---------------------------------------------------------------------------
static int g_enabled     = 1;            // RightStickCamera
static int g_sensitivity = 100;          // RightStickSensitivity, percent
// 10% matches Ephinea's measured deadzone; 20 was our own guess and made small nudges useless.
static int g_deadzone    = 10;           // RightStickDeadzone, percent of full scale
static int g_speed_slow  = SPEED_SLOW_DEG;   // RightStickSpeedSlow
static int g_speed_fast  = SPEED_FAST_DEG;   // RightStickSpeedFast
static int g_speed_split = SPEED_SPLIT_PCT;  // RightStickSpeedSplit
static int g_invert_x    = 0;            // RightStickInvertX
// ⚠ Which way is "up" is a coin flip until it is tried. DirectInput's Y axes are positive-down, and
// a positive pitch here raises the eye, so pushing the stick forward probably lowers the camera --
// i.e. what most players would call inverted. Left at 0 rather than guessed at: it is one config
// line either way, and shipping a wrong "fix" is worse than shipping the raw axis.
static int g_invert_y    = 0;            // RightStickInvertY
// ⚠ Vertical look is OFF by default. It works, but riding on top of the chase camera's own pitch it
// reads as strange in game -- the two are solving for height at the same time. Left in behind a
// switch rather than deleted: the maths and the clamps are the tested part, the feel is not.
static int g_allow_pitch = 0;            // RightStickPitch
static int g_suppress    = DEFAULT_SUPPRESS;

// Recentring. The client's own Camera binding (PAD BUTTON7 in the default pad config) re-aims the
// chase camera behind the character -- but our offset is added on top of that, so without clearing
// it the recentre lands somewhere arbitrary. Zero the offset on the same press and it lands where
// the player expects.
//
// ✅ CONFIRMED left trigger (2026-09-07): all 56 recentre presses in a play session logged lt=255.
// It could not be read off the client -- PSO sees Xidi's virtual DirectInput pad, so "PAD BUTTON7"
// is a Xidi mapper index, not an XInput button -- but StandardGamepad puts the triggers at 7 and 8,
// which fits PSO's defaults exactly (Prev Page/Camera = BUTTON7, Next Page = BUTTON8, i.e. the
// shoulders page through). Still config keys: a different mapper would land elsewhere.
// Automatic return to centre WHILE MOVING, in degrees/second. 0 holds the offset indefinitely,
// which was the original behaviour.
//
// Why moving specifically: a fixed offset means the camera is never behind you, so the chase camera
// pulls one way while the offset holds the other and the result reads as the camera "doing strange
// things" as you run. Standing still it is fine -- and looking around while stationary is the case
// where holding the offset is exactly what the player wants. So: decay while moving, hold while
// still, and never decay while the player is actively steering.
// Drift back behind the character while moving, in degrees/second. 0 holds the angle indefinitely.
//
// 📏 Calibrated against Ephinea's "Chase Cam: Hybrid" (observed 2026-09-08): after releasing the
// stick, their camera takes 3-5 seconds to swing back behind you from roughly 90 degrees off, i.e.
// about 20-30 deg/s. The first attempt here shipped 90 deg/s -- three to four times too fast -- and
// that is why it read as fighting the player rather than helping. The idea was fine; the rate was
// not. Their drift also does NOT run while standing still (nine seconds of observed idle, camera
// perfectly static), which is the rule this already implements.
#define BASE_RETURN_SPEED   25
static int g_return_speed = BASE_RETURN_SPEED;   // RightStickReturnSpeed, degrees/second

// Freeze the chase camera's remaining influence: hold the eye DISTANCE and HEIGHT at whatever they
// were when the player took control, instead of letting the chase camera keep choosing them.
//
// With yaw already held absolutely, distance and height are all the chase camera still decides about
// where the eye sits, so this turns it into a plain orbit camera around the character. What it does
// NOT disable, deliberately: the look-at target still tracks the character (that is the part you
// want), and the client's own wall collision and smoothing still run downstream of us.
// ✅ ON by default, from measurement. Over 81 in-game samples the chase camera moved the eye's
// HEIGHT almost not at all (6..8) while swinging its DISTANCE from 19 to 77 -- a 4x spread, and up
// to 45 units away from what the player had framed. That constant push-and-pull is what made the
// camera feel like it was fighting back; holding the framing is what stopped it.
// ---------------------------------------------------------------------------
// ChaseCam: the one setting that decides how the chase camera and the player share the camera.
//
// Everything below it (always-engaged, return speed, and what recentre does) is a consequence of
// this choice, so it is expressed once here rather than as four knobs a player has to reason about.
// The three positions are the same ones Ephinea offers, and are calibrated against it:
//
//   ENABLED   the chase camera stays in charge. You can swing the view, and it eases back behind
//             you while you move. Measured off Ephinea at 26 deg/s, and it does NOT drift while
//             the character stands still.
//   DISABLED  the camera holds the angle you give it and never reclaims. The chase camera still
//             chooses distance and height -- theirs visibly does, so ours must not freeze them.
//
// ⛔ There used to be a third mode, HYBRID, distinct from ENABLED by reclaiming faster (we used
// 120 deg/s). Measuring Ephinea killed it: their Enabled reclaims at 24.3 deg/s and their Hybrid at
// 26.1 -- the same within noise, so the two are not distinguishable in the client we were copying,
// and our 120 was invented rather than observed. Two modes, and the surviving one uses the measured
// rate.
//
// An explicit low-level key in widescreen.cfg still overrides whatever the mode selected, because
// the mode only supplies the DEFAULT for each. That keeps tuning possible without adding positions.
// ---------------------------------------------------------------------------
#define CHASE_ENABLED       0
#define CHASE_DISABLED      1

static int g_chase_mode  = CHASE_ENABLED; // ChaseCam

// Take the camera on the first frame and keep it, instead of waiting for the player to touch the
// right stick. OFF by default.
//
// ⚠ Tried once as the DEFAULT and rejected as "objectively worse" -- but that attempt also FROZE the
// framing, and observation of Ephinea's "Chase Cam: Disabled" (2026-09-08) shows theirs keeps the
// distance dynamic: their character visibly changes on-screen size, and drifts around the frame
// rather than being welded to its centre. So the untested combination is this ON with
// RightStickFreezeChase OFF, which is what their Disabled mode actually looks like.
// ⭐ How far the held angle may sit from directly behind the character, in degrees. 180 = no limit.
//
// This is a different mechanism from RightStickReturnSpeed, and a better fit for what PSO is:
// inside the cone NOTHING pulls at the camera, so there is no drift to fight. Only at the edge does
// the character's own turning drag the camera along -- so it always ends up following eventually,
// without ever tugging while you are aiming.
//
// Why a limit exists at all: with the camera in FRONT of the character, three things go wrong at
// once. Movement is camera-relative, so forward walks you toward the camera. Your own character
// occludes what you are attacking. And the audio listener is camera-relative -- PSO uses DS3D
// positional buffers, so combat audio audibly muffles when the enemy ends up far from the camera
// (observed in game 2026-09-08). None of that is fixable by tuning a rate; the camera simply should
// not go there.
// ⭐ THE FOLLOW BAND -- how far the eye may drift from the character before the camera moves at all.
//
// Percentages of the distance the chase camera currently wants. Inside the band the eye is left in
// WORLD SPACE, completely untouched: the character walks around inside a still frame, growing and
// shrinking, exactly as Ephinea's "Chase Cam: Disabled" does. Only when they reach the near or far
// edge does the camera move, and only enough to put them back on the edge.
//
// ⚠ This replaces rigid tracking, which was the real defect. Pinning the eye at target + offset
// every frame welds the character to one screen position and pivots the entire world around them --
// that is what made our fixed-angle mode "decidedly worse" than theirs despite having the same
// fixed angle. Setting NEAR and FAR both to 100 restores that old rigid behaviour exactly.
#define DEFAULT_FOLLOW_NEAR 60           // % of the chase camera's distance
#define DEFAULT_FOLLOW_FAR  180

#define DEFAULT_YAW_LIMIT   180          // no cone: dragging the camera IS the snapping to avoid
static int g_yaw_limit = DEFAULT_YAW_LIMIT;      // RightStickYawLimit
// ⭐ Smoothing on the distance we ask for, in percent-toward-target per frame. 100 = no smoothing.
//
// 📏 Measured over matched laps: Ephinea's camera distance lives in 24..54, ours in 11..96. The
// cause is what we ASK for -- we passed the chase camera's live preferred distance straight through,
// and that itself swings 32..77, so the client's lerp was chasing a target moving as fast as the
// camera. Ephinea holds a stable ~45.4 (one of their fixed CameraZoom levels).
//
// Smoothing rather than freezing: a held value has to be captured at some moment and is then wrong
// after a zoom or an area change, which is exactly how RightStickFreezeChase misbehaved. A slow
// filter tracks those changes without passing the jitter through. 3%/frame is a ~1s time constant.
#define DEFAULT_DIST_SMOOTH 3
// Look-at movement per frame below which we treat the character as at rest, for the purpose of
// learning the resting camera distance. Running measures ~1.5 units/frame.
#define DIST_LEARN_SPEED    0.5f
// ⭐ How fast the client's own lerp drags the camera to where we put it, in percent per frame.
// 0 leaves the client's value alone.
//
// 📏 Measured: releasing the stick while stationary, our camera keeps turning ~23 deg over ~700 ms;
// Ephinea's stops after 1.9 deg. The cause is not overshoot and not wall collision -- it is a plain
// first-order lag at 0.289/frame. While the stick is held the camera sits a steady 25.8 deg and 11%
// of its distance BEHIND what we command, and on release it unwinds. That is the "rubber band", and
// it is one lag showing up in two axes at once.
//
// ⛔ Two dead ends, both refuted by measurement, do not revisit them:
//   - "the camera is being displaced by map geometry" -- the trace above was taken standing still in
//     the middle of an open Forest room with nothing to collide with, and character speed was 0.00
//     for all 396 samples. Same 23 deg tail.
//   - "our zoom level sits further out than theirs" -- ours is 50.3 at Zoom 2 against their 45.4.
//     4.9 units cannot produce a 12x settling difference. See the zoom table in the client map.
//
// ⚠ The first version of this override wrote +0x1B8 and only appeared to help (35.8 -> 23.6 deg,
// which was really run-to-run variation). Writing +0x1BC is what actually addresses it.
#define DEFAULT_CAMERA_LERP 100
static int   g_camera_lerp = DEFAULT_CAMERA_LERP;  // RightStickCameraLerp

static int   g_dist_smooth = DEFAULT_DIST_SMOOTH;  // RightStickDistanceSmooth
static float g_smooth_h = 0.0f;
static int   g_smooth_h_valid = 0;

static int g_follow_near = DEFAULT_FOLLOW_NEAR;  // RightStickFollowNear
static int g_follow_far  = DEFAULT_FOLLOW_FAR;   // RightStickFollowFar

// The eye, in WORLD space, while we are holding it. Not an offset from the character -- that is the
// whole point: it stays put while they move.
static vec3f g_eye = { 0.0f, 0.0f, 0.0f };
static int   g_eye_valid = 0;

static int g_always_engaged = 0;         // RightStickAlwaysEngaged

static int g_freeze_chase = 1;           // RightStickFreezeChase

static int g_recentre_trigger = 1;       // RightStickRecentreTrigger: 0 none, 1 LT, 2 RT, 3 either
static int g_recentre_mask    = 0;       // RightStickRecentreMask: raw XInput wButtons bitmask
static int g_recentre_held    = 0;       // edge detection -- fire on press, not every held frame

// XInput, resolved at load. NULL means no usable XInput DLL and therefore no camera control.
typedef DWORD (WINAPI *PFN_XInputGetState)(DWORD dwUserIndex, void* pState);
static PFN_XInputGetState g_xinput_get_state = NULL;
static int   g_xi_user   = -1;           // the connected slot, or -1 if not known
static DWORD g_xi_rescan = 0;            // frames left before scanning slots again

// Accumulated offset from wherever the auto-camera would have put the eye. Persisted only in
// memory: a camera angle is not worth a file, and starting each session centred is the behaviour
// players already expect from the game.
// ⭐ The camera yaw we are HOLDING, as an absolute world angle -- not an offset from the chase
// camera. g_have_yaw says whether we are holding one at all; while it is 0 the chase camera is left
// completely untouched, so a player who never uses the right stick gets stock behaviour.
static float g_camera_yaw = 0.0f;        // radians, absolute
static int   g_have_yaw   = 0;
// Eye framing captured when control was taken, used only when g_freeze_chase is on.
static float g_hold_h     = 0.0f;        // horizontal distance from target to eye
static float g_hold_y     = 0.0f;        // height of the eye above the target
static float g_pitch_offset = 0.0f;      // radians
static DWORD g_frames = 0;

// Previous frame's look-at point. Only consulted when g_always_engaged is on: holding an angle
// indefinitely means noticing when the framing has gone stale, or the lobby's framing follows you
// into a dungeon. A one-frame jump above WARP_UNITS is a teleport, area change or respawn.
// 📏 Running measures about 1.5 units/frame in our own logs, so 100 is ~60x clear of it.
#define WARP_UNITS          100.0f
static vec3f g_prev_target = { 0.0f, 0.0f, 0.0f };
static float g_target_move = 0.0f;       // this frame's look-at movement, computed ONCE per frame
static int   g_have_prev_target = 0;
static int   g_warped = 0;               // set when that movement looks like a teleport

// ⚠ Recentring is NOT instantaneous, and treating it as such is a bug.
//
// The chase camera swings behind the character gradually. In always-engaged mode we used to release
// and re-engage in the same frame, which pinned the camera to however far that swing had got --
// typically less than half way -- so recentring took two or three presses to actually end up behind
// the character. Measured: one press moved the held angle 48.7 degrees, the next another 25.2.
//
// So after a recentre we stay OUT of the way and let the chase camera finish, re-engaging only once
// it has settled. "Settled" is its own yaw changing by less than SETTLE_DEG per frame for
// SETTLE_FRAMES consecutive frames.
#define SETTLE_DEG          0.5f
#define SETTLE_FRAMES       10
static int   g_recentring = 0;
static int   g_settle_count = 0;
static float g_prev_auto_yaw = 0.0f;
static int   g_have_prev_auto = 0;

// Last-seen write time of widescreen.cfg, for live config reloads.
static FILETIME g_cfg_mtime = { 0, 0 };
static int      g_cfg_mtime_valid = 0;

// Find "<key>" at the start of a line in buf, and return the integer after '='. Returns `fallback`
// if the key is absent or malformed. Deliberately not strtol: this plugin links no CRT.
static int cfg_int(const char* buf, DWORD got, const char* key, int fallback) {
  DWORD i;

  for (i = 0; i < got; i++) {
    DWORD k = 0;
    int sign = 1, digits = 0, value = 0;

    // Only match at the start of a line, so a longer key ending in this name cannot match.
    if (i && buf[i - 1] != '\n' && buf[i - 1] != '\r')
      continue;
    while (key[k] && (i + k) < got && buf[i + k] == key[k]) k++;
    if (key[k])
      continue;                          // ran out of match before the end of the key

    k += i;
    while (k < got && (buf[k] == ' ' || buf[k] == '\t')) k++;
    if (k >= got || buf[k] != '=')
      continue;                          // the name, but not as a key
    k++;
    while (k < got && (buf[k] == ' ' || buf[k] == '\t')) k++;
    if (k < got && (buf[k] == '-' || buf[k] == '+')) {
      sign = (buf[k] == '-') ? -1 : 1;
      k++;
    }
    if (k + 1 < got && buf[k] == '0' && (buf[k + 1] == 'x' || buf[k + 1] == 'X')) {
      k += 2;                            // hex, for the suppress mask
      while (k < got) {
        int d;
        if (buf[k] >= '0' && buf[k] <= '9') d = buf[k] - '0';
        else if (buf[k] >= 'a' && buf[k] <= 'f') d = buf[k] - 'a' + 10;
        else if (buf[k] >= 'A' && buf[k] <= 'F') d = buf[k] - 'A' + 10;
        else break;
        value = value * 16 + d;
        digits++;
        k++;
      }
    } else {
      while (k < got && buf[k] >= '0' && buf[k] <= '9') {
        value = value * 10 + (buf[k] - '0');
        digits++;
        k++;
      }
    }
    return digits ? sign * value : fallback;
  }
  return fallback;
}

static void load_config(void) {
  char path[MAX_PATH];
  char buf[8192];
  HANDLE h;
  DWORD got = 0;

  if (!rsc_sibling_path(path, MAX_PATH, "widescreen.cfg"))
    return;                              // no config: the defaults above stand
  h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                  FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE)
    return;
  if (ReadFile(h, buf, sizeof(buf) - 1, &got, NULL))
    buf[got] = 0;
  else
    got = 0;
  CloseHandle(h);

  g_enabled     = cfg_int(buf, got, "RightStickCamera", g_enabled) ? 1 : 0;
  g_sensitivity = cfg_int(buf, got, "RightStickSensitivity", g_sensitivity);
  g_deadzone    = cfg_int(buf, got, "RightStickDeadzone", g_deadzone);
  g_speed_slow  = cfg_int(buf, got, "RightStickSpeedSlow", g_speed_slow);
  g_speed_fast  = cfg_int(buf, got, "RightStickSpeedFast", g_speed_fast);
  g_speed_split = cfg_int(buf, got, "RightStickSpeedSplit", g_speed_split);
  if (g_speed_slow < 1) g_speed_slow = 1;
  if (g_speed_fast < 1) g_speed_fast = 1;
  if (g_speed_split < 1) g_speed_split = 1;
  if (g_speed_split > 100) g_speed_split = 100;
  g_invert_x    = cfg_int(buf, got, "RightStickInvertX", g_invert_x) ? 1 : 0;
  g_invert_y    = cfg_int(buf, got, "RightStickInvertY", g_invert_y) ? 1 : 0;
  g_allow_pitch = cfg_int(buf, got, "RightStickPitch", g_allow_pitch) ? 1 : 0;
  g_suppress    = cfg_int(buf, got, "RightStickSuppressMask", g_suppress);
  // The mode first: it sets the defaults that the individual keys below may then override.
  g_chase_mode = cfg_int(buf, got, "ChaseCam", g_chase_mode);
  // ⚠ 2 was "Disabled" in the old three-mode list (Enabled/Hybrid/Disabled). Migrate it rather than
  // clamping, or an existing config silently flips the player from Disabled to Enabled. The
  // launcher does the same mapping when it loads.
  if (g_chase_mode == 2)
    g_chase_mode = CHASE_DISABLED;
  if (g_chase_mode < CHASE_ENABLED || g_chase_mode > CHASE_DISABLED)
    g_chase_mode = CHASE_ENABLED;
  // ⚠ freeze OFF in both: the resting-distance hold supersedes it, and freezing distance is what
  // made the first attempt at Disabled worse rather than better.
  if (g_chase_mode == CHASE_DISABLED) {
    g_always_engaged = 1; g_return_speed = 0; g_freeze_chase = 0;
  } else {
    g_always_engaged = 0; g_return_speed = BASE_RETURN_SPEED; g_freeze_chase = 0;
  }

  g_freeze_chase     = cfg_int(buf, got, "RightStickFreezeChase", g_freeze_chase) ? 1 : 0;
  g_always_engaged   = cfg_int(buf, got, "RightStickAlwaysEngaged", g_always_engaged) ? 1 : 0;
  g_camera_lerp      = cfg_int(buf, got, "RightStickCameraLerp", g_camera_lerp);
  if (g_camera_lerp < 0) g_camera_lerp = 0;
  if (g_camera_lerp > 100) g_camera_lerp = 100;
  g_dist_smooth      = cfg_int(buf, got, "RightStickDistanceSmooth", g_dist_smooth);
  if (g_dist_smooth < 1) g_dist_smooth = 1;
  if (g_dist_smooth > 100) g_dist_smooth = 100;
  g_follow_near      = cfg_int(buf, got, "RightStickFollowNear", g_follow_near);
  g_follow_far       = cfg_int(buf, got, "RightStickFollowFar", g_follow_far);
  if (g_follow_near < 10) g_follow_near = 10;
  if (g_follow_far < g_follow_near) g_follow_far = g_follow_near;
  g_yaw_limit        = cfg_int(buf, got, "RightStickYawLimit", g_yaw_limit);
  if (g_yaw_limit < 10) g_yaw_limit = 10;
  if (g_yaw_limit > 180) g_yaw_limit = 180;
  g_return_speed     = cfg_int(buf, got, "RightStickReturnSpeed", g_return_speed);
  if (g_return_speed < 0) g_return_speed = 0;
  g_recentre_trigger = cfg_int(buf, got, "RightStickRecentreTrigger", g_recentre_trigger);
  g_recentre_mask    = cfg_int(buf, got, "RightStickRecentreMask", g_recentre_mask);

  if (g_sensitivity < 1) g_sensitivity = 1;
  if (g_sensitivity > 1000) g_sensitivity = 1000;
  if (g_deadzone < 0) g_deadzone = 0;
  if (g_deadzone > 95) g_deadzone = 95;
}

// Has widescreen.cfg been written since we last looked? Cheap enough to call a few times a second:
// GetFileAttributesEx stats the file without opening it.
static int config_changed(void) {
  WIN32_FILE_ATTRIBUTE_DATA fad;
  char path[MAX_PATH];

  if (!rsc_sibling_path(path, MAX_PATH, "widescreen.cfg"))
    return 0;
  if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad))
    return 0;                            // missing or unreadable: keep whatever we already have
  if (g_cfg_mtime_valid &&
      fad.ftLastWriteTime.dwLowDateTime == g_cfg_mtime.dwLowDateTime &&
      fad.ftLastWriteTime.dwHighDateTime == g_cfg_mtime.dwHighDateTime)
    return 0;
  g_cfg_mtime = fad.ftLastWriteTime;
  g_cfg_mtime_valid = 1;
  return 1;
}

// ---------------------------------------------------------------------------
// Float maths without a C runtime
//
// This project links with /kernel and OmitDefaultLibName, so there is no sinf/cosf/sqrtf to call.
// x87 provides all three as single instructions, which is both smaller and exactly what the
// compiler would have emitted anyway. FSIN/FCOS are only defined for |x| < 2^63; every angle here
// is a bounded accumulator well inside that, but wrap_angle keeps it tidy regardless.
// ---------------------------------------------------------------------------
static float f_sin(float a) {
  float r = 0.0f;
  __asm {
    fld   dword ptr [a]
    fsin
    fstp  dword ptr [r]
  }
  return r;
}

static float f_cos(float a) {
  float r = 0.0f;
  __asm {
    fld   dword ptr [a]
    fcos
    fstp  dword ptr [r]
  }
  return r;
}

static float f_sqrt(float a) {
  float r = 0.0f;
  __asm {
    fld   dword ptr [a]
    fsqrt
    fstp  dword ptr [r]
  }
  return r;
}

// ⚠ Not a plain `(int)f` cast. On x86 MSVC compiles a float-to-int cast into a call to the CRT
// helper __ftol2_sse, and this project links no CRT -- so the cast compiles cleanly and then fails
// at LINK time, in the diagnostic build only, which is exactly where it would be least expected.
// FISTP does it in one instruction. It rounds to nearest rather than truncating; for a log line
// that is if anything the better answer.
static int f_toint(float a) {
  int r = 0;
  __asm {
    fld   dword ptr [a]
    fistp dword ptr [r]
  }
  return r;
}

// atan2 via x87. FPATAN computes the angle of (st(0), st(1)) with the correct quadrant, so pushing
// y then x gives atan2(y, x). Needed in every build now that the camera holds an ABSOLUTE yaw: each
// frame we have to know what angle the chase camera just chose in order to cancel it.
static float f_atan2(float y, float x) {
  float r = 0.0f;
  __asm {
    fld   dword ptr [y]
    fld   dword ptr [x]
    fpatan
    fstp  dword ptr [r]
  }
  return r;
}

#if RSC_DIAGNOSTIC
// Previous frame's look-at point, so the periodic line can report how fast the chase camera's
// target is actually travelling. Purely observational.
#endif

static float f_abs(float a) {
  return a < 0.0f ? -a : a;
}

// Keep the yaw accumulator in (-PI, PI]. Unbounded growth would eventually cost FSIN its precision,
// and a bounded value is far easier to read in a log line.
static float wrap_angle(float a) {
  while (a > PI)  a -= 2.0f * PI;
  while (a < -PI) a += 2.0f * PI;
  return a;
}

// Resolve XInputGetState from whichever runtime is present, newest first. 1_4 ships with Win8+,
// 1_3 with the old DirectX redist, 9_1_0 is the legacy always-present one.
static void xinput_init(void) {
  static const char* const dlls[3] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
  HMODULE h;
  int i;

  for (i = 0; i < 3; i++) {
    h = LoadLibraryA(dlls[i]);
    if (!h)
      continue;
    g_xinput_get_state = (PFN_XInputGetState)GetProcAddress(h, "XInputGetState");
    if (g_xinput_get_state) {
      rsc_log("xinput: using %s", dlls[i]);
      return;
    }
  }
  // Worth a log line rather than silence: with no XInput the stick simply does nothing, which looks
  // exactly like a failed hook.
  rsc_log("xinput: no usable XInput DLL -- right stick unavailable");
}

// Everything we need from the pad in one read. Unlike the g_joyState buffer this replaced, "not
// connected" is an explicit answer, so a missing pad can never be mistaken for a held stick.
typedef struct {
  float rx, ry;                          // right stick, -1..1 about centre
  float lx, ly;                          // left stick -- movement input, for auto-recentring
  WORD  buttons;                         // raw XInput wButtons
  BYTE  lt, rt;                          // triggers, 0..255
} pad_state;

static void decode_pad(const BYTE* st, pad_state* p) {
  p->rx      = (float)(*(short*)(st + XI_OFF_THUMB_RX)) / XI_THUMB_SCALE;
  p->ry      = (float)(*(short*)(st + XI_OFF_THUMB_RY)) / XI_THUMB_SCALE;
  p->lx      = (float)(*(short*)(st + XI_OFF_THUMB_LX)) / XI_THUMB_SCALE;
  p->ly      = (float)(*(short*)(st + XI_OFF_THUMB_LY)) / XI_THUMB_SCALE;
  p->buttons = *(WORD*)(st + XI_OFF_BUTTONS);
  p->lt      = st[XI_OFF_TRIGGER_L];
  p->rt      = st[XI_OFF_TRIGGER_R];
}

static int read_pad(pad_state* p) {
  BYTE st[XI_STATE_BYTES];
  DWORD i;

  p->rx = p->ry = p->lx = p->ly = 0.0f;
  p->buttons = 0;
  p->lt = p->rt = 0;
  if (!g_xinput_get_state)
    return 0;

  // Fast path: the slot we already know about.
  if (g_xi_user >= 0) {
    if (g_xinput_get_state((DWORD)g_xi_user, st) == ERROR_SUCCESS) {
      decode_pad(st, p);
      return 1;
    }
    g_xi_user = -1;                      // unplugged
  }

  // Scanning every frame is wasteful when nothing is plugged in, so back off between sweeps.
  if (g_xi_rescan) {
    g_xi_rescan--;
    return 0;
  }
  for (i = 0; i < XI_MAX_USERS; i++) {
    if (g_xinput_get_state(i, st) == ERROR_SUCCESS) {
      g_xi_user = (int)i;
      decode_pad(st, p);
      return 1;
    }
  }
  g_xi_rescan = XI_RESCAN_FRAMES;
  return 0;
}

// How hard the LEFT stick is pushed, 0..1, deadzone removed. This is the "is the player moving"
// signal. Using the movement INPUT rather than the character's world velocity keeps it honest when
// the character is blocked by a wall or staggered -- the player is still asking to move, and that
// is when they want the camera back behind them.
static float move_magnitude(const pad_state* p) {
  float dz = (float)g_deadzone / 100.0f;
  float m = f_sqrt(p->lx * p->lx + p->ly * p->ly);

  if (m > 1.0f) m = 1.0f;
  if (m <= dz)
    return 0.0f;
  return (m - dz) / (1.0f - dz);
}

// Move `v` toward zero by at most `step`, landing exactly on zero rather than creeping.
static float decay_toward_zero(float v, float step) {
  if (v > step)  return v - step;
  if (v < -step) return v + step;
  return 0.0f;
}

// Is the recentre control down this frame?
static int recentre_down(const pad_state* p) {
  if (g_recentre_mask && (p->buttons & (WORD)g_recentre_mask))
    return 1;
  if ((g_recentre_trigger & 1) && p->lt >= XI_TRIGGER_ON)
    return 1;
  if ((g_recentre_trigger & 2) && p->rt >= XI_TRIGGER_ON)
    return 1;
  return 0;
}

// Turn rate for a stick position, in RADIANS PER FRAME, signed.
//
// Deliberately NOT proportional: outside the deadzone the rate is one of two constants, stepping up
// at RightStickSpeedSplit. See the measurement above the SPEED_ defines -- this reproduces how
// Ephinea's camera actually behaves, and proportional control is what ours had while feeling worse.
//
// The threshold is tested against the RAW deflection, not a post-deadzone rescale, so that the split
// sits where the player's thumb feels it rather than moving with the deadzone setting.
static float shape_axis(float v, int invert) {
  float dz = (float)g_deadzone / 100.0f;
  float split = (float)g_speed_split / 100.0f;
  float sign, mag, deg;

  if (v > 1.0f) v = 1.0f;
  if (v < -1.0f) v = -1.0f;
  mag = f_abs(v);
  if (mag <= dz)
    return 0.0f;
  sign = (v < 0.0f) ? -1.0f : 1.0f;
  if (invert) sign = -sign;

  deg = (mag < split) ? (float)g_speed_slow : (float)g_speed_fast;
  deg = deg * (float)g_sensitivity / 100.0f;
  return sign * deg * DEG2RAD / 30.0f;         // deg/s -> rad/frame at 30fps
}

// Hand the camera back to the client. Used by the recentre control, by the suppressed states, and
// by the optional drift-back once it arrives -- all of which mean the same thing: stop holding an
// angle, and let the chase camera do exactly what it would have done without this plugin.
// Take the camera at the angle and framing the client currently has, so taking over is invisible.
static void engage_camera(float auto_yaw, float h, float vy) {
  g_camera_yaw = auto_yaw;
  g_hold_h = h;
  g_hold_y = vy;
  g_have_yaw = 1;
  g_eye_valid = 0;                       // seeded from the client's own eye on the next frame
  g_smooth_h_valid = 0;                  // and the distance filter starts from what it has now
}

static void release_camera(void) {
  g_have_yaw = 0;
  g_pitch_offset = 0.0f;
}

// ---------------------------------------------------------------------------
// Pad Button Config: show the right analog rows as taken, while the camera owns them
//
// While the camera owns the right stick, the two Right Analog rows are greyed out, lose their
// cursor, and report what has taken them. Whatever they are bound to is not reaching the game, so
// showing them as live bindings is a lie.
//
// The greying is the client's OWN mechanism, not an invented one -- see ITEM_COLOR/ITEM_FLAGS above.
//
// Nothing is written to the character's key config. The bindings are left exactly as the player set
// them, which matters because on Blue Burst that config syncs to the server: clearing it would
// persist after the camera was switched off again.
// ---------------------------------------------------------------------------
static const wchar_t RSC_ROW_TEXT[] = L"-- Camera --";

// Safe to hand the client a static string: its own callers pass static globals here (the axis and
// button name tables at 0x0097B264 / 0x0097B2E4), so this setter cannot be taking ownership.
static void pad_row_set_text(void* widget, const wchar_t* text) {
  __asm {
    push 0x20
    push text
    mov  ecx, widget
    mov  eax, ADDR_TEXT_SETTEXT
    call eax                                             // ret 8: callee cleans both arguments
  }
}

// Grey a text object and take its cursor away, exactly as 0x00738A9C does.
static void grey_item(BYTE* item) {
  if (!item)
    return;
  *(DWORD*)(item + ITEM_COLOR) = ITEM_GREY;
  *(WORD*)(item + ITEM_FLAGS) = (WORD)(*(WORD*)(item + ITEM_FLAGS) & 0xFFFE);
}

// The left-hand column is a list item, found by the value AddListItem was given -- which for this
// menu is the row index.
static void grey_list_row(BYTE* list, int value) {
  BYTE** lines;
  int n, i;

  if (!list)
    return;
  lines = *(BYTE***)(list + LIST_LINES_PTR);
  n = (int)*(short*)(list + LIST_NUM_LINES);
  if (!lines || n <= 0 || n > 256)
    return;                              // not the shape we expect: do nothing rather than scribble
  for (i = 0; i < n; i++) {
    BYTE* item = lines[i];
    if (item && *(int*)(item + ITEM_VALUE) == value)
      grey_item(item);
  }
}

static void __cdecl on_pad_row(void* widget, int row, void* menu) {
  if (!g_enabled || !widget)
    return;                              // classic controls: leave the menu exactly as it was
  if (row != PAD_ROW_RSTICK_X && row != PAD_ROW_RSTICK_Y)
    return;

  // Right-hand column: say what has taken the binding, and grey it.
  pad_row_set_text(widget, RSC_ROW_TEXT);
  grey_item((BYTE*)widget);

  // Left-hand column: grey the row name and drop its cursor, so it reads as unavailable and the
  // selection no longer highlights it.
  if (menu)
    grey_list_row(*(BYTE**)((BYTE*)menu + OFF_MENU_LIST), row);
}

// Replaces `call 0x0072E0E4`. ECX is the row's text widget and EBX the row index; both survive
// pushad/popad, and the original is tail-jumped so its ret lands after our call site.
void __declspec(naked) padRowHook(void) {
  __asm {
    pushad
    push esi                                             // the menu object (rows are esi+row*4+0x2C)
    push ebx                                             // row index
    push ecx                                             // this row's text widget
    call on_pad_row
    add  esp, 0xC
    popad
    mov  eax, ADDR_TEXT_SPACE
    jmp  eax
  }
}

// ---------------------------------------------------------------------------
// The hook body
//
// Runs immediately after the auto-camera has written desired_source and desired_target, and before
// anything reads them. Rotates desired_source about desired_target; touches nothing else.
// ---------------------------------------------------------------------------
static void __cdecl on_camera_updated(void) {
  BYTE* cam = *(BYTE**)ADDR_CAMERA_STATE;
  DWORD flags = *(DWORD*)ADDR_MENU_FLAGS;
  vec3f* src;
  vec3f* tgt;
  float vx, vy, vz, h, r;
  float nx, nz, ny;
  pad_state pad;
  float auto_yaw;
  int have_pad, recentre;

  g_frames++;

  // Pick up a changed widescreen.cfg while the game is running, so toggling the launcher's
  // checkbox takes effect without restarting the client -- or reinstalling anything.
  if ((g_frames % RSC_CONFIG_POLL) == 0 && config_changed()) {
    int was = g_enabled;
    load_config();
    if (was != g_enabled) {
      rsc_log("config reloaded: right-stick camera %s", g_enabled ? "ON" : "OFF");
      if (!g_enabled)
        release_camera();                // do not strand a held angle when switching off
    }
  }

  // Disabled at runtime: behave exactly as if the plugin were not installed. The hook is still in
  // place -- that is what makes the toggle live -- but it returns before reading or writing any of
  // the client's camera state, so the chase camera is left completely alone.
  if (!g_enabled)
    return;

  if (!cam)
    return;                              // before the camera exists, or after it is torn down

  src = (vec3f*)(cam + OFF_DESIRED_SOURCE);
  tgt = (vec3f*)(cam + OFF_DESIRED_TARGET);

  // Read the pad exactly ONCE per frame. read_pad has side effects -- it caches the connected slot
  // and counts down the rescan backoff -- so calling it a second time just for the log would make
  // diagnostic builds behave differently from release ones.
  have_pad = read_pad(&pad);

  // Recentre on the PRESS, not every frame the control is held: holding it would otherwise pin the
  // offset at zero and make the stick appear dead.
  recentre = have_pad && recentre_down(&pad);
  if (recentre && !g_recentre_held) {
    // Logged on the EDGE, not sampled: a press lasts a few frames, so the periodic line below will
    // essentially never catch one. This is what confirms which control is actually bound.
    rsc_diag("recentre fired (btn=%04X lt=%d rt=%d) -- released (was holding=%d yaw=%d/1000)",
             pad.buttons, pad.lt, pad.rt, g_have_yaw, f_toint(g_camera_yaw * 1000.0f));
    release_camera();
    g_recentring = 1;                    // hands off until the chase camera has finished the swing
    g_settle_count = 0;
    g_have_prev_auto = 0;
  }
  g_recentre_held = recentre;

  // How far the look-at point moved since last frame. Feeds both the diagnostic line and the warp
  // check below, and is computed exactly once so those two can never disagree. Done BEFORE any early
  // return, or a skipped frame would make the next delta an accumulated distance.
  {
    float mx = tgt->x - g_prev_target.x;
    float my = tgt->y - g_prev_target.y;
    float mz = tgt->z - g_prev_target.z;
    g_target_move = f_sqrt(mx * mx + my * my + mz * mz);
    g_warped = g_have_prev_target && (g_target_move > WARP_UNITS);
    g_prev_target = *tgt;
    g_have_prev_target = 1;
  }

#if RSC_DIAGNOSTIC
  if ((g_frames % RSC_DIAG_EVERY) == 0) {
    // Left half: is a pad seen, what does it read, is the state suppressed. Right half: what the
    // CHASE CAMERA itself is doing before we touch it -- its own yaw, how far back it is sitting,
    // and how fast its target is moving. If the camera still misbehaves, the cause is far more
    // likely to be visible in those three than in our offset.
    float ax = src->x - tgt->x, ay = src->y - tgt->y, az = src->z - tgt->z;
    rsc_diag("slot=%d conn=%d rx=%d/1000 ry=%d/1000 move=%d/1000 btn=%04X lt=%d rt=%d | "
             "menuflags=%08X | holding=%d yaw=%d/1000 pitch=%d/1000 | "
             "auto: yaw=%d/1000 dist=%d height=%d tgtspeed=%d | frozen=%d held h=%d y=%d",
             g_xi_user, have_pad, f_toint(pad.rx * 1000.0f), f_toint(pad.ry * 1000.0f),
             f_toint(move_magnitude(&pad) * 1000.0f), pad.buttons, pad.lt, pad.rt,
             flags, g_have_yaw, f_toint(g_camera_yaw * 1000.0f),
             f_toint(g_pitch_offset * 1000.0f),
             f_toint(f_atan2(ax, az) * 1000.0f), f_toint(f_sqrt(ax * ax + az * az)), f_toint(ay),
             f_toint(g_target_move * 100.0f),
             (g_freeze_chase && g_have_yaw) ? 1 : 0, f_toint(g_hold_h), f_toint(g_hold_y));
  }
#endif

  // Cutscenes, teleports and menu states where the client snaps or freezes the camera. Hand
  // control straight back rather than trying to hold an angle through them.
  if (flags & (DWORD)g_suppress) {
    release_camera();
    return;
  }

  // The eye relative to the look-at point. Y is the vertical axis here: the client's own degenerate
  // case at 0x004D2158 is "X and Z deltas are both zero", which it treats as looking straight
  // up or down.
  vx = src->x - tgt->x;
  vy = src->y - tgt->y;
  vz = src->z - tgt->z;

  h = f_sqrt(vx * vx + vz * vz);
  r = f_sqrt(h * h + vy * vy);
  if (r <= 0.0f)
    return;                              // source and target coincide; the client fixes this itself

  // The chase camera's OWN yaw this frame -- where it wants the eye, before we say anything.
  auto_yaw = (h > 0.0f) ? f_atan2(vx, vz) : g_camera_yaw;

  // Re-frame on a warp as well as on first engage: a new area is framed differently (measured,
  // distance ~100 in the lobby against ~50 in a dungeon), so a held framing goes stale across one.
  // Watch the chase camera settle after a recentre.
  if (g_recentring) {
    float moved = g_have_prev_auto ? f_abs(wrap_angle(auto_yaw - g_prev_auto_yaw)) : 999.0f;

    g_prev_auto_yaw = auto_yaw;
    g_have_prev_auto = 1;
    if (moved < SETTLE_DEG * DEG2RAD) {
      if (++g_settle_count >= SETTLE_FRAMES) {
        g_recentring = 0;                // it has arrived; take the camera back at that angle
        rsc_diag("recentre: chase camera settled at yaw=%d/1000", f_toint(auto_yaw * 1000.0f));
      }
    } else {
      g_settle_count = 0;
    }
  }

  // Re-engage only when we are not waiting for a recentre to finish.
  if (g_always_engaged && !g_recentring && (!g_have_yaw || g_warped))
    engage_camera(auto_yaw, h, vy);

  if (have_pad) {
    // Negated after the first in-game session: pushing the stick right must swing the view right,
    // and the unnegated sense read backwards. RightStickInvertX now means "the other way from the
    // one that felt natural", which is what an invert switch should mean.
    float steer_x = shape_axis(-pad.rx, g_invert_x);   // already rad/frame
    // XInput's Y is positive-UP, and a positive pitch here RAISES the eye (i.e. looks further
    // down). Negating makes stick-forward look up, which is the un-inverted convention;
    // RightStickInvertY then means what its name says.
    float steer_y = g_allow_pitch ? shape_axis(-pad.ry, g_invert_y) : 0.0f;

    if (steer_x != 0.0f || steer_y != 0.0f) {
      g_recentring = 0;                  // the player wants it now; stop waiting for the chase cam
      // Take control on first touch, seeded from wherever the chase camera currently is, so
      // engaging never produces a jump.
      if (!g_have_yaw)
        engage_camera(auto_yaw, h, vy);  // seeded from the chase camera, so this never jumps
      g_camera_yaw = wrap_angle(g_camera_yaw + steer_x);

      if (g_allow_pitch) {
        g_pitch_offset += steer_y;
        if (g_pitch_offset >  PITCH_LIMIT_RAD) g_pitch_offset =  PITCH_LIMIT_RAD;
        if (g_pitch_offset < -PITCH_LIMIT_RAD) g_pitch_offset = -PITCH_LIMIT_RAD;
      }
    } else if (g_return_speed > 0 && g_have_yaw) {
      // Optional drift back to whatever the chase camera wants, while moving. OFF by default: in
      // combat it fights both the player and the chase camera, which is exactly what a held camera
      // is supposed to prevent.
      float move = move_magnitude(&pad);

      if (move > 0.0f) {
        float step = (float)g_return_speed * DEG2RAD / 30.0f * move;  // deg/s -> rad/frame at 30fps
        float delta = wrap_angle(g_camera_yaw - auto_yaw);

        if (f_abs(delta) <= step && g_pitch_offset == 0.0f) {
          release_camera();              // arrived: let the chase camera have it back outright
        } else {
          g_camera_yaw = wrap_angle(auto_yaw + decay_toward_zero(delta, step));
          g_pitch_offset = decay_toward_zero(g_pitch_offset, step);
        }
      }
    }
  }

  // Not holding an angle and no pitch applied: leave the chase camera's point byte-identical, so a
  // player who never touches the right stick gets stock behaviour.
  if (!g_have_yaw && g_pitch_offset == 0.0f)
    return;

  // Drag the held angle along once it falls outside the cone. Applied AFTER steering and drift, and
  // every frame -- so as the character turns, an angle sitting at the edge is carried with them,
  // while anything inside the cone is left completely alone.
  if (g_have_yaw && g_yaw_limit < 180) {
    float lim = (float)g_yaw_limit * DEG2RAD;
    float off = wrap_angle(g_camera_yaw - auto_yaw);

    if (off > lim)
      g_camera_yaw = wrap_angle(auto_yaw + lim);
    else if (off < -lim)
      g_camera_yaw = wrap_angle(auto_yaw - lim);
  }

  // ⭐ The eye lives in WORLD space, not as an offset from the character.
  //
  // Each frame it is left exactly where it was, and only two things move it: the player steering
  // (which orbits it about the character), and the follow band (which pulls it in or lets it out
  // when the character gets too close or too far). Between those, the camera is genuinely still and
  // the character moves around inside the frame.
  //
  // The rigid version this replaces recomputed the eye as target + offset every frame, which looks
  // identical while standing still and is completely different in motion: it welds the character to
  // one screen position and swings the world around them.
  if (!g_eye_valid) {
    g_eye = *src;                        // start from wherever the client had the eye: no jump
    g_eye_valid = 1;
  }

  // ⭐ Learn the RESTING distance and hold it while moving.
  //
  // 📏 Measured across matched laps: the distance our chase camera asks for jumps from 51.9 standing
  // still to 77.1 while running -- it pulls back with speed. Ephinea's does not: 45.4 still, 46.6
  // moving, because their CameraZoom2 pins "distance behind the player" at 45.42 and holds it. That
  // pull-back, not lag, is why our camera sat ~50% further out than theirs ever gets. (Lag is the
  // other way round: their actual/commanded ratio while moving is 0.80, ours 0.92.)
  //
  // So only let the filter track while the character is essentially stationary. Running measures
  // ~1.5 units/frame, so DIST_LEARN_SPEED at 0.5 admits standing and shuffling but not running. The
  // effect is that zooming or changing area re-learns within a second of stopping, while a sprint
  // across a map holds the framing the player last saw at rest.
  if (!g_smooth_h_valid) {
    g_smooth_h = h;
    g_smooth_h_valid = 1;
  } else if (g_target_move < DIST_LEARN_SPEED) {
    g_smooth_h += (h - g_smooth_h) * ((float)g_dist_smooth / 100.0f);
  }

  {
    float ex = g_eye.x - tgt->x;
    float ez = g_eye.z - tgt->z;
    float ey = g_eye.y - tgt->y;
    float elen = f_sqrt(ex * ex + ez * ez);
    float near_d = g_smooth_h * (float)g_follow_near / 100.0f;
    float far_d = g_smooth_h * (float)g_follow_far / 100.0f;
    float want = wrap_angle(g_camera_yaw - ((elen > 0.0f) ? f_atan2(ex, ez) : g_camera_yaw));

    // Steering orbits the eye about the character, preserving its distance.
    if (want != 0.0f && elen > 0.0f) {
      float cs = f_cos(want), sn = f_sin(want);
      float rx = ex * cs + ez * sn;
      float rz = -ex * sn + ez * cs;
      ex = rx; ez = rz;
    }

    // The band. Inside it, nothing happens at all -- this is the part that makes the camera feel
    // fixed rather than glued to the character.
    if (elen > 0.0f) {
      float clamped = elen;
      if (elen < near_d) clamped = near_d;
      else if (elen > far_d) clamped = far_d;
      if (clamped != elen) {
        float k = clamped / elen;
        ex *= k; ez *= k;
      }
    } else {
      ex = 0.0f; ez = near_d;            // degenerate: put it somewhere sane
    }

    // Height tracks the chase camera's, which barely varies (measured 6..8) and is not worth
    // holding: letting it follow keeps the character correctly framed vertically on slopes.
    ey = g_freeze_chase ? g_hold_y : vy;

    g_eye.x = tgt->x + ex;
    g_eye.y = tgt->y + ey;
    g_eye.z = tgt->z + ez;
    nx = ex; ny = ey; nz = ez;
  }

  // Pitch: rotate the (horizontal distance, height) pair, then fold back into x/z by scaling.
  if (g_pitch_offset != 0.0f) {
    float hh2 = f_sqrt(nx * nx + nz * nz);

    if (hh2 > 0.0f) {
      float rr = f_sqrt(hh2 * hh2 + ny * ny);
      float sn = f_sin(g_pitch_offset), cs = f_cos(g_pitch_offset);
      float nh2 = hh2 * cs - ny * sn;
      float ny2 = hh2 * sn + ny * cs;

      if (nh2 > 0.0f && f_abs(ny2) <= PITCH_SIN_LIMIT * rr) {
        float k = nh2 / hh2;
        nx *= k; nz *= k; ny = ny2;
        g_eye.x = tgt->x + nx; g_eye.y = tgt->y + ny; g_eye.z = tgt->z + nz;
      }
    }
  }

  // Only while we are actually driving. Left alone, a player who never touches the right stick gets
  // the client's own smoothing exactly as before.
  if (g_camera_lerp > 0) {
    float k = (float)g_camera_lerp / 100.0f;
    *(float*)(cam + OFF_LERP_SOURCE) = k;   // +0x1BC -- the one that matters
    *(float*)(cam + OFF_LERP_A) = k;        // +0x1B8 -- harmless, and kept so the pair stays coherent
  }

  src->x = tgt->x + nx;
  src->y = tgt->y + ny;
  src->z = tgt->z + nz;
}

// ---------------------------------------------------------------------------
// Hook stub
//
// Replaces `call 0x004D1FF4`. The original has to run first -- it is what fills in the points we
// then rotate -- so this calls it rather than tail-jumping to it. ecx is already loaded by the
// instruction before our call site; the original is __fastcall and returns void.
// ---------------------------------------------------------------------------
void __declspec(naked) cameraHook(void) {
  __asm {
    mov  eax, ADDR_SET_POINTS
    call eax                                             // auto-camera writes desired source/target
    pushad
    call on_camera_updated
    popad
    ret
  }
}

// ---------------------------------------------------------------------------
// Patching
//
// No VirtualProtect: this client's .text is already RWX, which is why no other plugin in this repo
// unprotects either. If that ever changes, every plugin here breaks together and loudly.
// ---------------------------------------------------------------------------
// Separate from the camera patch on purpose: this one is cosmetic, and a guard failure here must
// not cost the actual feature.
static BOOL patch_pad_menu(void) {
  DWORD target;

  // mov ecx, [edi+0x2C] -- the widget load immediately before the call we are replacing.
  if (*(BYTE*)(ADDR_PADROW_CALL - 3) != 0x8B ||
      *(BYTE*)(ADDR_PADROW_CALL - 2) != 0x4F ||
      *(BYTE*)(ADDR_PADROW_CALL - 1) != 0x2C)
    return FALSE;
  // cmp ebx, 0x10 -- the 16-row loop bound, 11 bytes past the call. Confirms this is the row loop
  // and not some other site that happens to load a widget the same way.
  if (*(BYTE*)(ADDR_PADROW_CALL + 11) != 0x83 ||
      *(BYTE*)(ADDR_PADROW_CALL + 12) != 0xFB ||
      *(BYTE*)(ADDR_PADROW_CALL + 13) != 0x10)
    return FALSE;
  if (*(BYTE*)ADDR_PADROW_CALL != 0xE8)
    return FALSE;
  target = (DWORD)(ADDR_PADROW_CALL + 5 + *(LONG*)(ADDR_PADROW_CALL + 1));
  if (target != ADDR_TEXT_SPACE)
    return FALSE;

  *(DWORD*)(ADDR_PADROW_CALL + 1) = calc_disp32(ADDR_PADROW_CALL + 1, (ULONG_PTR)padRowHook);
  return TRUE;
}

static BOOL patch_camera(void) {
  DWORD target;

  // Refuse to patch anything that is not byte-for-byte what was analysed. A wrong address here
  // retargets some unrelated call, which would fault far away from the cause.
  if (*(WORD*)(ADDR_UPDATE_CALL - 2) != 0xCD8B)          // mov ecx, ebp -- sets up the argument
    return FALSE;
  if (*(BYTE*)ADDR_UPDATE_CALL != 0xE8)
    return FALSE;
  target = (DWORD)(ADDR_UPDATE_CALL + 5 + *(LONG*)(ADDR_UPDATE_CALL + 1));
  if (target != ADDR_SET_POINTS)
    return FALSE;                                        // it is a call, but not the one we mean

  // Retarget the existing call to us; we call through to where it went.
  *(DWORD*)(ADDR_UPDATE_CALL + 1) = calc_disp32(ADDR_UPDATE_CALL + 1, (ULONG_PTR)cameraHook);
  return TRUE;
}

__declspec(dllexport) void __stdcall load(void) {
  if (GetImageSize(0) < 0x00762000 || *(DWORD*)0x00B613FA != 0x4C4E3935) { // 59NL
    rsc_log("wrong client version -- expected MTethVer12513 (1.25.13)");
    MessageBoxA(0, "RightStickCamera: Wrong client version, expected MTethVer12513 (1.25.13)",
                "Error", MB_ICONERROR);
    return;
  }

  load_config();
  xinput_init();
  config_changed();                      // prime the mtime so the first poll is not a false change

  // ⚠ Patch even when the feature is switched OFF. The hook has to be in place for the setting to
  // be changeable at runtime; it checks g_enabled every frame and returns immediately when off,
  // which costs a compare and leaves the client's camera untouched. Skipping the patch here would
  // make turning the feature on require a client restart.
  if (!patch_pad_menu())
    rsc_log("pad menu NOT patched: row-loop signature did not match at %08X "
            "(right analog rows will still show their bindings)", ADDR_PADROW_CALL);

  if (patch_camera()) {
    static const char* const mode_name[2] = { "enabled", "disabled" };
    rsc_log("patched ok (call %08X -> hook) enabled=%d chasecam=%s sens=%d%% deadzone=%d%% pitch=%s "
            "invX=%d invY=%d speed=%d/%d@%d%% dsmooth=%d%% lerp=%d%% freeze=%d always=%d return=%ddeg/s yawlimit=%d recentre(trig=%d mask=%04X) suppress=%03X "
            "xinput=%s%s",
            ADDR_UPDATE_CALL, g_enabled, mode_name[g_chase_mode], g_sensitivity, g_deadzone,
            g_allow_pitch ? "on" : "off",
            g_invert_x, g_invert_y, g_speed_slow, g_speed_fast, g_speed_split, g_dist_smooth, g_camera_lerp,
            g_freeze_chase, g_always_engaged, g_return_speed, g_yaw_limit, g_recentre_trigger,
            g_recentre_mask, g_suppress,
            g_xinput_get_state ? "ok" : "MISSING",
            RSC_DIAGNOSTIC ? "  [DIAGNOSTIC BUILD]" : "");
  } else {
    // No dialog: a camera feature must not interrupt every launch. But it must not be silent
    // either -- an unmatched guard means the stick quietly does nothing, and without this line
    // there is nothing anywhere to say why.
    rsc_log("NOT patched: hook site did not match (setup=%04X op=%02X) -- feature disabled",
            *(WORD*)(ADDR_UPDATE_CALL - 2), *(BYTE*)ADDR_UPDATE_CALL);
    OutputDebugStringA("RightStickCamera: hook site did not match; feature disabled\n");
  }
}

int __stdcall DllMain(HINSTANCE hInstDLL, DWORD dwReason, LPVOID lpReserved) {
  if (dwReason == DLL_PROCESS_ATTACH)
    DisableThreadLibraryCalls(hInstDLL);

  return TRUE;
}
