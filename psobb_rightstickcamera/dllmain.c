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

// Degrees per frame at full stick deflection, before RightStickSensitivity scales it. The camera
// update runs once per frame and the client targets 30fps, so 3.3333 is 100 degrees/second.
// Raised from 3.0 (90 deg/s) after the first in-game session, which read as slightly sluggish.
#define BASE_DEGREES        3.3333333f

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
static int g_deadzone    = 20;           // RightStickDeadzone, percent of full scale
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
// ⚠ Which XInput control that is cannot be read off the client: PSO sees Xidi's virtual DirectInput
// pad, and "PAD BUTTON7" is a Xidi mapper index, not an XInput button. Under StandardGamepad the
// numbering puts the triggers at 7 and 8, which fits PSO's defaults exactly (Prev Page/Camera =
// BUTTON7, Next Page = BUTTON8 -- i.e. the shoulder triggers page through). Hence left trigger as
// the default. A diagnostic build logs the raw button word and both triggers, so if it is wrong the
// log names the right one and it is a config line, not a rebuild.
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
static float g_yaw_offset = 0.0f;        // radians
static float g_pitch_offset = 0.0f;      // radians
static DWORD g_frames = 0;

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
  g_invert_x    = cfg_int(buf, got, "RightStickInvertX", g_invert_x) ? 1 : 0;
  g_invert_y    = cfg_int(buf, got, "RightStickInvertY", g_invert_y) ? 1 : 0;
  g_allow_pitch = cfg_int(buf, got, "RightStickPitch", g_allow_pitch) ? 1 : 0;
  g_suppress    = cfg_int(buf, got, "RightStickSuppressMask", g_suppress);
  g_recentre_trigger = cfg_int(buf, got, "RightStickRecentreTrigger", g_recentre_trigger);
  g_recentre_mask    = cfg_int(buf, got, "RightStickRecentreMask", g_recentre_mask);

  if (g_sensitivity < 1) g_sensitivity = 1;
  if (g_sensitivity > 1000) g_sensitivity = 1000;
  if (g_deadzone < 0) g_deadzone = 0;
  if (g_deadzone > 95) g_deadzone = 95;
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
  WORD  buttons;                         // raw XInput wButtons
  BYTE  lt, rt;                          // triggers, 0..255
} pad_state;

static void decode_pad(const BYTE* st, pad_state* p) {
  p->rx      = (float)(*(short*)(st + XI_OFF_THUMB_RX)) / XI_THUMB_SCALE;
  p->ry      = (float)(*(short*)(st + XI_OFF_THUMB_RY)) / XI_THUMB_SCALE;
  p->buttons = *(WORD*)(st + XI_OFF_BUTTONS);
  p->lt      = st[XI_OFF_TRIGGER_L];
  p->rt      = st[XI_OFF_TRIGGER_R];
}

static int read_pad(pad_state* p) {
  BYTE st[XI_STATE_BYTES];
  DWORD i;

  p->rx = p->ry = 0.0f;
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

// Deadzone and rescale, so the first movement past the deadzone starts from zero rather than
// jumping. Input is already -1..1 about centre.
static float shape_axis(float v, int invert) {
  float dz = (float)g_deadzone / 100.0f;
  float sign, mag;

  if (v > 1.0f) v = 1.0f;
  if (v < -1.0f) v = -1.0f;
  mag = f_abs(v);
  if (mag <= dz)
    return 0.0f;
  sign = (v < 0.0f) ? -1.0f : 1.0f;
  if (invert) sign = -sign;
  return sign * ((mag - dz) / (1.0f - dz));
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
  float dyaw, dpitch, s, c, nx, nz, nh, ny;
  float rate = BASE_DEGREES * DEG2RAD * ((float)g_sensitivity / 100.0f);
  pad_state pad;
  int have_pad, recentre;

  g_frames++;
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
    g_yaw_offset = 0.0f;
    g_pitch_offset = 0.0f;
  }
  g_recentre_held = recentre;

#if RSC_DIAGNOSTIC
  if ((g_frames % RSC_DIAG_EVERY) == 0) {
    // Whether a pad is seen at all, what it reads, and the live menu mask -- the three things that
    // distinguish "not connected" from "connected but suppressed" from "working".
    rsc_diag("xinput slot=%d connected=%d rx=%d/1000 ry=%d/1000 btn=%04X lt=%d rt=%d "
             "recentre=%d | menuflags=%08X | yaw=%d/1000 pitch=%d/1000",
             g_xi_user, have_pad, f_toint(pad.rx * 1000.0f), f_toint(pad.ry * 1000.0f),
             pad.buttons, pad.lt, pad.rt, recentre,
             flags, f_toint(g_yaw_offset * 1000.0f), f_toint(g_pitch_offset * 1000.0f));
  }
#endif

  // Cutscenes, teleports and menu states where the client snaps or freezes the camera. Recentre
  // rather than merely pausing: coming out of a cutscene holding a stale offset would look like the
  // camera had drifted on its own.
  if (flags & (DWORD)g_suppress) {
    g_yaw_offset = 0.0f;
    g_pitch_offset = 0.0f;
    return;
  }

  // Accumulate only when there is a pad to read. With none, the offset simply holds where it is --
  // that is "no new input", not "recentre the camera".
  if (have_pad) {
    // Negated after the first in-game session: pushing the stick right must swing the view right,
    // and the unnegated sense read backwards. RightStickInvertX now means "the other way from the
    // one that felt natural", which is what an invert switch should mean.
    dyaw = shape_axis(-pad.rx, g_invert_x) * rate;
    g_yaw_offset = wrap_angle(g_yaw_offset + dyaw);

    if (g_allow_pitch) {
      // XInput's Y is positive-UP, and a positive pitch here RAISES the eye (i.e. looks further
      // down). Negating makes stick-forward look up, which is the un-inverted convention;
      // RightStickInvertY then means what its name says.
      dpitch = shape_axis(-pad.ry, g_invert_y) * rate;
      g_pitch_offset += dpitch;
      if (g_pitch_offset >  PITCH_LIMIT_RAD) g_pitch_offset =  PITCH_LIMIT_RAD;
      if (g_pitch_offset < -PITCH_LIMIT_RAD) g_pitch_offset = -PITCH_LIMIT_RAD;
    }
  }

  if (g_yaw_offset == 0.0f && g_pitch_offset == 0.0f)
    return;                              // centred: leave the auto-camera's point byte-identical

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

  // Yaw: rotate about the vertical axis. Length preserved, so the auto-camera keeps full control of
  // how far back the camera sits.
  s = f_sin(g_yaw_offset);
  c = f_cos(g_yaw_offset);
  nx = vx * c + vz * s;
  nz = -vx * s + vz * c;

  // Pitch: the same rotation applied to the (horizontal distance, height) pair, then folded back
  // into x/z by scaling. Doing it this way needs no atan2 or asin -- only the sqrt above.
  ny = vy;
  if (g_pitch_offset != 0.0f && h > 0.0f) {
    s = f_sin(g_pitch_offset);
    c = f_cos(g_pitch_offset);
    nh = h * c - vy * s;
    ny = h * s + vy * c;
    // Refuse to pass the hard limit or to cross the axis. The accumulator clamp bounds only OUR
    // contribution; the auto-camera's own pitch rides underneath it, so the total needs its own
    // stop. Rejecting the whole pitch step leaves yaw working, which is the half players notice.
    if (nh <= 0.0f || f_abs(ny) > PITCH_SIN_LIMIT * r) {
      ny = vy;
    } else {
      float k = nh / h;
      nx *= k;
      nz *= k;
    }
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
  if (!g_enabled) {
    rsc_log("disabled (RightStickCamera=0)");
    return;
  }

  if (patch_camera()) {
    rsc_log("patched ok (call %08X -> hook) sens=%d%% deadzone=%d%% pitch=%s invX=%d invY=%d "
            "recentre(trig=%d mask=%04X) suppress=%03X xinput=%s%s",
            ADDR_UPDATE_CALL, g_sensitivity, g_deadzone, g_allow_pitch ? "on" : "off",
            g_invert_x, g_invert_y, g_recentre_trigger, g_recentre_mask, g_suppress,
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
