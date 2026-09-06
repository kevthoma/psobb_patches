// GameSettings 1.25.13 (59NL)
//
// Remembers the last create-game selections -- difficulty and mode -- so the dialog comes up where you
// left it instead of resetting to Normal every time.
//
// WHY THIS IS SMALL: the client does not "fail to save" these; it actively clears them. The party
// creation menu object is constructed fresh each time the dialog opens, and its constructor zeroes the
// two fields:
//
//   0x00733F98  88 42 1E   mov byte ptr [edx+0x1E], al   ; mode       = 0   (al is 0 here)
//   0x00733F9B  33 C9      xor ecx, ecx
//   0x00733F9D  88 42 1F   mov byte ptr [edx+0x1F], al   ; difficulty = 0
//   0x00733FA0  8B 42 20   mov eax, [edx+0x20]           ; eax reloaded -> dead across our hook
//
// So the whole feature is: write two bytes back after the constructor has cleared them, and capture
// them again when the player confirms.
//
// Episode is deliberately NOT restored. It comes from the constructor's third argument -- i.e. from
// which menu entry the player chose -- so it is re-stated every time and there is nothing to remember.
//
// Full derivation, and how each address was confirmed against our own psobb.exe rather than taken from
// the psobb.io decompilation, is in notes/psobb-client-map.md ("Create-game dialog").

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
#define ADDR_CTOR_ZERO      0x00733F98   // the two stores above, 8 bytes through 0x00733F9F
#define ADDR_CONFIRM_CALL   0x0079ADD7   // `call 0x007346CC` inside the confirm handler at 0x0079ADB8
#define ADDR_GET_NAME       0x007346CC   // accessor the confirm path calls first, with ecx = the object

#define OFF_MODE            0x1E         // 0 normal, 1 challenge, 2 battle, 3 one person
#define OFF_DIFFICULTY      0x1F         // 0..3; the accessor forces 0 when mode == challenge

// The exact bytes we expect to replace, little-endian: 88 42 1E 33 C9 88 42 1F.
#define CTOR_ZERO_ORIGINAL  0x1F4288C9331E4288ULL

// Restoring a difficulty the character has not unlocked is the one behaviour that is NOT yet
// established -- the dialog's own level gate has not been located, so it is unknown whether it
// re-clamps a pre-set value. Until that is tested in game, this switch turns the difficulty half off
// while leaving mode (which has no gating) working.
#define RESTORE_DIFFICULTY  1

#define SETTINGS_FILE       "corellia_gamesettings.dat"
#define SETTINGS_MAGIC      0xC0
#define SETTINGS_VERSION    0x01

// Cached across the two hooks. Written by the confirm hook, read by the constructor hook.
static BYTE g_mode = 0;
static BYTE g_difficulty = 0;
static BYTE g_have_saved = 0;

// ---------------------------------------------------------------------------
// Persistence
//
// Stored next to psobb.exe rather than in the registry. Each instance (prod, canary, legacy) is its
// own install directory, so a file beside the client is scoped correctly for free -- whereas a
// registry write would have to work out which leaf THIS build owns (PSOBC / PSOBA / PSOBL, stamped at
// build time) to avoid canary settings leaking into prod. Same isolation, none of the detection.
//
// Win32 only, no CRT: these plugins link without one (see util.h re-implementing memset).
// ---------------------------------------------------------------------------
static void load_settings(void) {
  char path[MAX_PATH];
  HANDLE h;
  BYTE buf[4];
  DWORD got = 0;

  if (!gs_sibling_path(path, MAX_PATH, SETTINGS_FILE))
    return;

  h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                  FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE)
    return;                                  // first run: nothing saved yet, which is not an error
  if (ReadFile(h, buf, sizeof(buf), &got, NULL) && got == sizeof(buf) &&
      buf[0] == SETTINGS_MAGIC && buf[1] == SETTINGS_VERSION &&
      buf[2] <= 3 && buf[3] <= 3) {          // validate: a corrupt file must not select a bogus mode
    g_mode = buf[2];
    g_difficulty = buf[3];
    g_have_saved = 1;
    gs_diag("load: mode=%u difficulty=%u", g_mode, g_difficulty);
  } else {
    gs_diag("load: settings file present but rejected (got %u bytes)", got);
  }
  CloseHandle(h);
}

static void store_settings(void) {
  char path[MAX_PATH];
  HANDLE h;
  BYTE buf[4];
  DWORD put = 0;

  if (!gs_sibling_path(path, MAX_PATH, SETTINGS_FILE))
    return;

  buf[0] = SETTINGS_MAGIC;
  buf[1] = SETTINGS_VERSION;
  buf[2] = g_mode;
  buf[3] = g_difficulty;

  h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE)
    return;                                  // a read-only install dir just means it never persists
  WriteFile(h, buf, sizeof(buf), &put, NULL);
  CloseHandle(h);
}

// ---------------------------------------------------------------------------
// Hook bodies (plain C; the naked stubs below just marshal to these)
// ---------------------------------------------------------------------------
static void __cdecl on_dialog_constructed(BYTE* obj) {
  BYTE mode = 0, difficulty = 0;

  if (!obj) {
    gs_diag("construct: NULL object -- hook fired on something unexpected");
    return;
  }
  // Logged BEFORE the write: the constructor should have left both at 0, so a non-zero "was" here
  // would mean the hook is running somewhere other than where it was meant to.
  gs_diag("construct: obj=%08X was mode=%u difficulty=%u episode=%u",
          (DWORD)obj, obj[OFF_MODE], obj[OFF_DIFFICULTY], *(DWORD*)(obj + 0x20));
  // ⚠ BOTH fields must be written on EVERY call, even with nothing saved. The two stores this hook
  // replaced were the only initialisation they get -- the object comes from a plain allocation that
  // does not zero -- so returning early here left difficulty holding heap garbage, which the dialog
  // could not resolve to a name and displayed as a literal "%s". Defaulting to 0 reproduces the
  // original behaviour exactly; a saved value merely overrides it.
  mode = g_have_saved ? g_mode : 0;
#if RESTORE_DIFFICULTY
  difficulty = g_have_saved ? g_difficulty : 0;
#endif
  obj[OFF_MODE] = mode;
  obj[OFF_DIFFICULTY] = difficulty;
  gs_diag("construct: wrote mode=%u difficulty=%u (saved=%u, difficulty restore %s)",
          obj[OFF_MODE], obj[OFF_DIFFICULTY], g_have_saved, RESTORE_DIFFICULTY ? "on" : "OFF");
}

static void __cdecl on_dialog_confirmed(BYTE* obj) {
  if (!obj) {
    gs_diag("confirm: NULL object");
    return;
  }
  gs_diag("confirm: obj=%08X mode=%u difficulty=%u episode=%u",
          (DWORD)obj, obj[OFF_MODE], obj[OFF_DIFFICULTY], *(DWORD*)(obj + 0x20));
  if (obj[OFF_MODE] > 3 || obj[OFF_DIFFICULTY] > 3) {
    // Out of range means the offsets are wrong, not that the player did something odd -- so say so
    // rather than failing silently.
    gs_diag("confirm: values out of range, NOT saving (offsets may be wrong)");
    return;
  }
  g_mode = obj[OFF_MODE];
  g_difficulty = obj[OFF_DIFFICULTY];
  g_have_saved = 1;
  store_settings();
}

// ---------------------------------------------------------------------------
// Naked stubs
// ---------------------------------------------------------------------------

// Replaces the constructor's two zeroing stores. edx = the object; eax is dead here (reloaded at
// 0x00733FA0), but ecx must come out zeroed because the `xor ecx, ecx` we overwrote did that.
void __declspec(naked) restoreHook(void) {
  __asm {
    pushad
    push edx
    call on_dialog_constructed
    add  esp, 4
    popad
    xor  ecx, ecx                                        // reproduce the overwritten `xor ecx, ecx`
    ret
  }
}

// Replaces `call 0x007346CC` on the confirm path. ecx = the object, and the original call's return
// value (the name pointer) must still arrive in eax -- so this tail-jumps to the real accessor, which
// sets eax itself and returns straight to 0x0079ADDC.
void __declspec(naked) saveHook(void) {
  __asm {
    pushad
    push ecx
    call on_dialog_confirmed
    add  esp, 4
    popad
    mov  eax, ADDR_GET_NAME
    jmp  eax
  }
}

// ---------------------------------------------------------------------------
// Patching
//
// No VirtualProtect: this client's .text is already RWX (checked in the shipped binary), which is why
// no other plugin in this repo unprotects either. If that ever changes, every plugin here breaks
// together and loudly, not just this one.
// ---------------------------------------------------------------------------
static BOOL patch_gamesettings(void) {
  DWORD confirm_target;

  // Refuse to patch anything that is not byte-for-byte what was analysed. A wrong address here writes
  // a call into the middle of unrelated code, which would fault far away from the cause.
  if (*(ULONGLONG*)ADDR_CTOR_ZERO != CTOR_ZERO_ORIGINAL)
    return FALSE;

  if (*(BYTE*)ADDR_CONFIRM_CALL != 0xE8)
    return FALSE;
  confirm_target = (DWORD)(ADDR_CONFIRM_CALL + 5 + *(LONG*)(ADDR_CONFIRM_CALL + 1));
  if (confirm_target != ADDR_GET_NAME)
    return FALSE;                            // it is a call, but not the one we mean

  // Constructor: 8 bytes -> call rel32 + 3 nops.
  *(BYTE*)(ADDR_CTOR_ZERO) = 0xE8;
  *(DWORD*)(ADDR_CTOR_ZERO + 1) = calc_disp32(ADDR_CTOR_ZERO + 1, (ULONG_PTR)restoreHook);
  *(BYTE*)(ADDR_CTOR_ZERO + 5) = 0x90;
  *(BYTE*)(ADDR_CTOR_ZERO + 6) = 0x90;
  *(BYTE*)(ADDR_CTOR_ZERO + 7) = 0x90;

  // Confirm path: retarget the existing call to us; we tail-jump to where it went.
  *(DWORD*)(ADDR_CONFIRM_CALL + 1) = calc_disp32(ADDR_CONFIRM_CALL + 1, (ULONG_PTR)saveHook);

  return TRUE;
}

__declspec(dllexport) void __stdcall load(void) {
  if (GetImageSize(0) < 0x00762000 || *(DWORD*)0x00B613FA != 0x4C4E3935) { // 59NL
    gs_log("wrong client version -- expected MTethVer12513 (1.25.13)");
    MessageBoxA(0, "GameSettings: Wrong client version, expected MTethVer12513 (1.25.13)",
                "Error", MB_ICONERROR);
    return;
  }

  load_settings();

  if (patch_gamesettings()) {
    gs_log("patched ok (ctor 0x%08X, confirm 0x%08X)%s",
           ADDR_CTOR_ZERO, ADDR_CONFIRM_CALL,
           GAMESETTINGS_DIAGNOSTIC ? "  [DIAGNOSTIC BUILD]" : "");
  } else {
    // No dialog: a cosmetic feature must not interrupt every launch. But it must not be silent
    // either -- an unmatched guard means settings quietly stop being remembered, and without this
    // line there is nothing anywhere to say why.
    gs_log("NOT patched: hook sites did not match (ctor=%08X%08X confirm=%02X) -- feature disabled",
           *(DWORD*)(ADDR_CTOR_ZERO + 4), *(DWORD*)ADDR_CTOR_ZERO, *(BYTE*)ADDR_CONFIRM_CALL);
    OutputDebugStringA("GameSettings: hook sites did not match; feature disabled\n");
  }
}

int __stdcall DllMain(HINSTANCE hInstDLL, DWORD dwReason, LPVOID lpReserved) {
  if (dwReason == DLL_PROCESS_ATTACH)
    DisableThreadLibraryCalls(hInstDLL);

  return TRUE;
}
