// GameSettings 1.25.13 (59NL)
//
// Remembers the last create-game selections -- play mode, difficulty, party name and password -- so
// the dialog comes up where you left it instead of resetting every time.
//
// The whole feature is OPT-IN and off by default, gated on RememberPartyInfo in widescreen.cfg, which
// the launcher's options window writes. That mirrors Ephinea's single SAVE_PARTY_INFO switch. The
// password is the part that earns the gate: a secret at rest, and a restore that goes wrong changes
// who can join a game.
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
#define ADDR_MENUBUILD_CALL 0x00733F7F   // `call 0x00734000` in the ctor -- builds the menu lines
#define ADDR_MENUBUILD      0x00734000   // reads +0x1E to pick the Play Mode label, once, forever
#define ADDR_CONFIRM_CALL   0x0079ADD7   // `call 0x007346CC` inside the confirm handler at 0x0079ADB8
#define ADDR_GET_NAME       0x007346CC   // accessor the confirm path calls first, with ecx = the object

// The constructor labels the Play Mode summary line with a hardcoded 0 rather than the field:
//   0x00733FE0  xor eax, eax        <-- always "Normal"
//   0x00733FE2  call 0x00734079     ; get_string(0x139 + eax)
// Patch those seven bytes to fetch the real mode. 0x00734079 takes its argument in AL and returns
// the string in EAX, so a stub can load the field and tail-jump straight to it.
#define ADDR_MODE_LABEL_ARG 0x00733FE0   // `xor eax,eax` + `call 0x00734079`, 7 bytes
#define ADDR_MODE_LABEL_FN  0x00734079   // get_string(0x139 + al)

#define OFF_MODE            0x1E         // 0 normal, 1 challenge, 2 battle, 3 one person
#define OFF_DIFFICULTY      0x1F         // 0..3; the accessor forces 0 when mode == challenge

// The exact bytes we expect to replace, little-endian: 88 42 1E 33 C9 88 42 1F.
#define CTOR_ZERO_ORIGINAL  0x1F4288C9331E4288ULL

// Restoring a difficulty the character has not unlocked is the one behaviour that is NOT yet
// established -- the dialog's own level gate has not been located, so it is unknown whether it
// re-clamps a pre-set value. Until that is tested in game, this switch turns the difficulty half off
// while leaving mode (which has no gating) working.
#define RESTORE_DIFFICULTY  1

// Text fields. Settled in game 2026-09-07 (see notes/psobb-client-map.md): creation site B builds
// the NAME widget (maxlen 0x0E, stores to +0x24) and site A the PASSWORD (maxlen 0x10, +0x28).
// 14 + the two-char "\tE" language marker is exactly the 16-wchar wire field.
#define OFF_NAME            0x24         // wchar_t*, FREED by the destructor -- see ADDR_CLIENT_MALLOC
#define OFF_PASSWORD        0x28         // wchar_t*, likewise
#define OFF_EDIT_WIDGET     0x34         // the text-entry widget, created on demand and destroyed after
#define FIELD_WCHARS        16           // pstring<UTF16,0x10>, matching C_CreateGame_BB_C1

#define ADDR_CREATE_NAME    0x0073432D   // mov eax,[ebp-0x14]; mov [eax+0x34],edx  (name widget)
#define ADDR_CREATE_PW      0x00734213   // same two instructions                   (password widget)
#define ADDR_WIDGET_SETTEXT 0x0078ED74   // __thiscall(ecx = widget, wchar_t*), CALLEE cleans
#define ADDR_SET_LINE_TEXT  0x00738A40   // __thiscall(ecx = menu widget at +0x2C), (string, line)
#define ADDR_CLIENT_MALLOC  0x008581C5   // __cdecl. MUST be this one: the dialog destructor frees
                                         // +0x24/+0x28, so a static buffer would corrupt the heap.
#define OFF_MENU_WIDGET     0x2C         // the menu whose lines the summary text belongs to
#define LINE_PARTY_NAME     0
#define LINE_PASSWORD       1

#define SETTINGS_FILE       "corellia_gamesettings.dat"
#define SETTINGS_MAGIC      0xC0
#define SETTINGS_VERSION    0x02         // v1 was mode+difficulty only; v1 files still load

// Cached across the hooks. Written by the confirm hook, read by the construct-time hooks.
static BYTE  g_mode = 0;
static BYTE  g_difficulty = 0;
static BYTE  g_have_saved = 0;
static WCHAR g_name[FIELD_WCHARS];
static WCHAR g_password[FIELD_WCHARS];
static BYTE  g_remember = 0;             // the launcher's RememberPartyInfo; gates ALL of it

static int gs_wlen(const WCHAR* w) {
  int n = 0;
  while (w && w[n] && n < 0x400) n++;
  return n;
}

// Copy at most FIELD_WCHARS-1 and always terminate. The wire field is 16 wide chars and the client
// truncates rather than overflows, so refusing to store more than fits keeps the two consistent.
static void gs_wcopy(WCHAR* dst, const WCHAR* src) {
  int i = 0;
  if (!src) { dst[0] = 0; return; }
  for (; i < FIELD_WCHARS - 1 && src[i]; i++) dst[i] = src[i];
  dst[i] = 0;
}

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
// The launcher writes RememberPartyInfo into widescreen.cfg beside psobb.exe. Absent or 0 means the
// text fields are neither captured nor restored -- the password is a secret at rest, and a restore
// that goes wrong changes who can join a game, so this defaults OFF and stays off until asked for.
static void load_remember_flag(void) {
  char path[MAX_PATH];
  HANDLE h;
  char buf[8192];
  DWORD got = 0, i;
  const char* key = "RememberPartyInfo";

  g_remember = 0;
  if (!gs_sibling_path(path, MAX_PATH, "widescreen.cfg"))
    return;
  h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                  FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE)
    return;
  if (ReadFile(h, buf, sizeof(buf) - 1, &got, NULL))
    buf[got] = 0;
  else
    got = 0;
  CloseHandle(h);

  for (i = 0; i < got; i++) {
    DWORD k = 0;

    // Only match at the start of a line, so a longer key ending in this name cannot match.
    if (i && buf[i - 1] != '\n' && buf[i - 1] != '\r')
      continue;
    while (key[k] && (i + k) < got && buf[i + k] == key[k]) k++;
    if (key[k])
      continue;                              // ran out of match before the end of the key

    k += i;
    while (k < got && (buf[k] == ' ' || buf[k] == '\t')) k++;
    if (k >= got || buf[k] != '=')
      continue;                              // the name, but not as a key
    k++;
    while (k < got && (buf[k] == ' ' || buf[k] == '\t')) k++;
    g_remember = (k < got && buf[k] == '1') ? 1 : 0;
    break;
  }
  gs_diag("config: RememberPartyInfo=%u", g_remember);
}

static void load_settings(void) {
  char path[MAX_PATH];
  HANDLE h;
  BYTE buf[4 + FIELD_WCHARS * 4];
  DWORD got = 0;

  if (!gs_sibling_path(path, MAX_PATH, SETTINGS_FILE))
    return;

  h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                  FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE)
    return;                                  // first run: nothing saved yet, which is not an error
  if (ReadFile(h, buf, sizeof(buf), &got, NULL) && got >= 4 &&
      buf[0] == SETTINGS_MAGIC && buf[1] <= SETTINGS_VERSION &&
      buf[2] <= 3 && buf[3] <= 3) {          // validate: a corrupt file must not select a bogus mode
    g_mode = buf[2];
    g_difficulty = buf[3];
    g_have_saved = 1;
    // v1 files hold only mode and difficulty; read them and leave the text empty rather than
    // rejecting a file written by an older build.
    if (buf[1] >= 2 && got >= sizeof(buf)) {
      gs_wcopy(g_name, (const WCHAR*)(buf + 4));
      gs_wcopy(g_password, (const WCHAR*)(buf + 4 + FIELD_WCHARS * 2));
    }
    gs_diag("load: v%u mode=%u difficulty=%u name=%d chars password=%d chars",
            buf[1], g_mode, g_difficulty, gs_wlen(g_name), gs_wlen(g_password));
  } else {
    gs_diag("load: settings file present but rejected (got %u bytes)", got);
  }
  CloseHandle(h);
}

static void store_settings(void) {
  char path[MAX_PATH];
  HANDLE h;
  BYTE buf[4 + FIELD_WCHARS * 4];
  DWORD put = 0;
  int i;

  if (!gs_sibling_path(path, MAX_PATH, SETTINGS_FILE))
    return;

  for (i = 0; i < (int)sizeof(buf); i++) buf[i] = 0;
  buf[0] = SETTINGS_MAGIC;
  buf[1] = SETTINGS_VERSION;
  buf[2] = g_mode;
  buf[3] = g_difficulty;
  gs_wcopy((WCHAR*)(buf + 4), g_name);
  gs_wcopy((WCHAR*)(buf + 4 + FIELD_WCHARS * 2), g_password);

  h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE)
    return;                                  // a read-only install dir just means it never persists
  WriteFile(h, buf, sizeof(buf), &put, NULL);
  CloseHandle(h);
}

// ---------------------------------------------------------------------------
// Calling into the client: allocation and the two text setters
// ---------------------------------------------------------------------------
// A copy of `text` on the CLIENT's heap. The dialog destructor frees +0x24/+0x28, so anything stored
// there has to come from the allocator that free() expects. Returns NULL if there is nothing to store.
static WCHAR* gs_client_dup(const WCHAR* text) {
  int n = gs_wlen(text);
  DWORD bytes;
  void* p = NULL;
  int i;

  if (n <= 0)
    return NULL;
  bytes = (DWORD)((n + 1) * 2);
  __asm {
    push bytes
    mov  eax, ADDR_CLIENT_MALLOC
    call eax
    add  esp, 4                                          // __cdecl: we clean
    mov  p, eax
  }
  if (!p)
    return NULL;
  for (i = 0; i < n; i++) ((WCHAR*)p)[i] = text[i];
  ((WCHAR*)p)[n] = 0;
  return (WCHAR*)p;
}

static void gs_widget_set_text(void* widget, const WCHAR* text) {
  __asm {
    push text
    mov  ecx, widget
    mov  eax, ADDR_WIDGET_SETTEXT
    call eax                                             // callee cleans its one argument
  }
}

static void gs_set_line_text(void* menu, void* text, int line) {
  __asm {
    push line
    push text
    mov  ecx, menu
    mov  eax, ADDR_SET_LINE_TEXT
    call eax                                             // callee cleans both
  }
}

// Put a remembered field back: the pointer the confirm path will read, and the summary line the
// player sees. Both, or neither -- a value that is sent but not shown is the failure this plugin
// already hit once with Play Mode.
static void gs_restore_text_field(BYTE* obj, int off, int line, const WCHAR* text) {
  WCHAR* dup;
  void* menu;

  if (!g_remember || !text || !text[0])
    return;
  dup = gs_client_dup(text);
  if (!dup) {
    gs_diag("text: allocation failed for +0x%02X", off);
    return;
  }
  *(WCHAR**)(obj + off) = dup;
  menu = *(void**)(obj + OFF_MENU_WIDGET);
  if (menu)
    gs_set_line_text(menu, dup, line);
  gs_diag("text: restored +0x%02X (%d chars) and line %d", off, gs_wlen(dup), line);
}

// ---------------------------------------------------------------------------
// Hook bodies (plain C; the naked stubs below just marshal to these)
// ---------------------------------------------------------------------------
// after_build distinguishes the two call sites: before the menu is built (+0x2C not yet set) and
// after the constructor clears the fields. Only the second can touch the label.
static void __cdecl on_dialog_write(BYTE* obj, int after_build) {
  BYTE mode = 0, difficulty = 0;

  if (!obj) {
    gs_diag("write: NULL object -- hook fired on something unexpected");
    return;
  }
  // Logged BEFORE the write: the constructor should have left both at 0, so a non-zero "was" here
  // would mean the hook is running somewhere other than where it was meant to.
  gs_diag("write: obj=%08X was mode=%u difficulty=%u episode=%u",
          (DWORD)obj, obj[OFF_MODE], obj[OFF_DIFFICULTY], *(DWORD*)(obj + 0x20));
  // ⚠ BOTH fields must be written on EVERY call, even with nothing saved. The two stores this hook
  // replaced were the only initialisation they get -- the object comes from a plain allocation that
  // does not zero -- so returning early here left difficulty holding heap garbage, which the dialog
  // could not resolve to a name and displayed as a literal "%s". Defaulting to 0 reproduces the
  // original behaviour exactly; a saved value merely overrides it.
  // ⚠ Both are written on EVERY call whether or not the feature is enabled. The stores this hook
  // replaced were the only initialisation these fields get, so skipping the write leaves heap garbage
  // that the dialog renders as a literal "%s". Disabled simply means writing the 0 the client would
  // have written itself.
  mode = (g_have_saved && g_remember) ? g_mode : 0;
#if RESTORE_DIFFICULTY
  difficulty = (g_have_saved && g_remember) ? g_difficulty : 0;
#endif
  obj[OFF_MODE] = mode;
  obj[OFF_DIFFICULTY] = difficulty;
  gs_diag("write: now mode=%u difficulty=%u (saved=%u, difficulty restore %s)",
          obj[OFF_MODE], obj[OFF_DIFFICULTY], g_have_saved, RESTORE_DIFFICULTY ? "on" : "OFF");
  if (after_build) {
    // +0x2C exists by now (set at 0x00733F87), and the constructor has not yet touched lines 0/1.
    gs_restore_text_field(obj, OFF_NAME, LINE_PARTY_NAME, g_name);
    gs_restore_text_field(obj, OFF_PASSWORD, LINE_PASSWORD, g_password);
  }
}

// Called as each text-entry widget is created, so the box opens holding the remembered value rather
// than blank -- otherwise opening a field just to look at it would clear it.
static void __cdecl on_edit_widget_created(void* widget, int is_name) {
  const WCHAR* text = is_name ? g_name : g_password;

  if (!g_remember || !widget || !text[0])
    return;
  gs_widget_set_text(widget, text);
  gs_diag("text: prefilled the %s widget %08X", is_name ? "NAME" : "PASSWORD", (DWORD)widget);
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
  if (!g_remember) {
    gs_diag("confirm: not saving, RememberPartyInfo is off");
    return;
  }
  g_mode = obj[OFF_MODE];
  g_difficulty = obj[OFF_DIFFICULTY];
  {
    // Stored verbatim, INCLUDING the leading "\tE" language marker the client puts there. Stripping
    // it would write back a subtly malformed name that renders oddly rather than failing.
    gs_wcopy(g_name, *(const WCHAR**)(obj + OFF_NAME));
    gs_wcopy(g_password, *(const WCHAR**)(obj + OFF_PASSWORD));
    gs_diag("confirm: captured name=%d chars password=%d chars",
            gs_wlen(g_name), gs_wlen(g_password));
  }
  g_have_saved = 1;
  store_settings();
}

// ---------------------------------------------------------------------------
// Naked stubs
// ---------------------------------------------------------------------------

// Each fires as its text-entry widget is created, reproducing the two instructions it replaces.
// ebp is the caller's frame and a naked callee leaves it alone, so the reproduced pair sees exactly
// what it would have.
void __declspec(naked) nameWidgetHook(void) {
  __asm {
    pushad
    push 1
    push edx
    call on_edit_widget_created
    add  esp, 8
    popad
    mov  eax, dword ptr [ebp - 0x14]
    mov  dword ptr [eax + OFF_EDIT_WIDGET], edx
    ret
  }
}

void __declspec(naked) passwordWidgetHook(void) {
  __asm {
    pushad
    push 0
    push edx
    call on_edit_widget_created
    add  esp, 8
    popad
    mov  eax, dword ptr [ebp - 0x14]
    mov  dword ptr [eax + OFF_EDIT_WIDGET], edx
    ret
  }
}

// Supplies the Play Mode summary label's argument, replacing the constructor's hardcoded zero.
// edx = the dialog object here; 0x00734079 wants the mode in AL and returns the string in EAX, so
// tail-jumping to it puts the right string in the right register for the caller.
void __declspec(naked) modeLabelHook(void) {
  __asm {
    movsx eax, byte ptr [edx + OFF_MODE]
    cmp   eax, 3
    jbe   ok
    xor   eax, eax                                       // out of range: behave exactly as before
  ok:
    mov   ecx, ADDR_MODE_LABEL_FN
    jmp   ecx                                            // its ret lands after our call site
  }
}

// Runs immediately before the constructor builds the dialog's menu lines, with ecx = the object.
// The Play Mode label is chosen here and never refreshed, so the value has to be in place BEFORE
// this call, not merely before the dialog is shown. The original call's return value (a widget,
// stored at +0x2C) must survive, so this tail-jumps to it.
void __declspec(naked) preBuildHook(void) {
  __asm {
    pushad
    push 0                                               // before the build: +0x2C is not set yet
    push ecx
    call on_dialog_write
    add  esp, 8
    popad
    mov  eax, ADDR_MENUBUILD
    jmp  eax
  }
}

// Replaces the constructor's two zeroing stores. edx = the object; eax is dead here (reloaded at
// 0x00733FA0), but ecx must come out zeroed because the `xor ecx, ecx` we overwrote did that.
void __declspec(naked) restoreHook(void) {
  __asm {
    pushad
    push 1                                               // after_build: widget exists, label can be set
    push edx
    call on_dialog_write
    add  esp, 8
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
// The six bytes both creation sites share: 8b 45 ec 89 50 34
static BOOL is_widget_store(ULONG_PTR at) {
  static const BYTE want[6] = { 0x8B, 0x45, 0xEC, 0x89, 0x50, 0x34 };
  int i;
  for (i = 0; i < 6; i++)
    if (*(BYTE*)(at + i) != want[i])
      return FALSE;
  return TRUE;
}

static void patch_widget_store(ULONG_PTR at, void* stub) {
  *(BYTE*)at = 0xE8;
  *(DWORD*)(at + 1) = calc_disp32(at + 1, (ULONG_PTR)stub);
  *(BYTE*)(at + 5) = 0x90;
}

static BOOL patch_gamesettings(void) {
  DWORD confirm_target;
  DWORD menubuild_target;
  DWORD modelabel_target;

  // Refuse to patch anything that is not byte-for-byte what was analysed. A wrong address here writes
  // a call into the middle of unrelated code, which would fault far away from the cause.
  if (*(ULONGLONG*)ADDR_CTOR_ZERO != CTOR_ZERO_ORIGINAL)
    return FALSE;

  if (*(BYTE*)ADDR_CONFIRM_CALL != 0xE8)
    return FALSE;
  confirm_target = (DWORD)(ADDR_CONFIRM_CALL + 5 + *(LONG*)(ADDR_CONFIRM_CALL + 1));
  if (confirm_target != ADDR_GET_NAME)
    return FALSE;                            // it is a call, but not the one we mean

  if (*(BYTE*)ADDR_MENUBUILD_CALL != 0xE8)
    return FALSE;
  menubuild_target = (DWORD)(ADDR_MENUBUILD_CALL + 5 + *(LONG*)(ADDR_MENUBUILD_CALL + 1));
  if (menubuild_target != ADDR_MENUBUILD)
    return FALSE;

  if (!is_widget_store(ADDR_CREATE_NAME) || !is_widget_store(ADDR_CREATE_PW))
    return FALSE;

  if (*(WORD*)ADDR_MODE_LABEL_ARG != 0xC033 ||           // xor eax, eax
      *(BYTE*)(ADDR_MODE_LABEL_ARG + 2) != 0xE8)
    return FALSE;
  modelabel_target = (DWORD)(ADDR_MODE_LABEL_ARG + 7 + *(LONG*)(ADDR_MODE_LABEL_ARG + 3));
  if (modelabel_target != ADDR_MODE_LABEL_FN)
    return FALSE;

  // Constructor: 8 bytes -> call rel32 + 3 nops.
  *(BYTE*)(ADDR_CTOR_ZERO) = 0xE8;
  *(DWORD*)(ADDR_CTOR_ZERO + 1) = calc_disp32(ADDR_CTOR_ZERO + 1, (ULONG_PTR)restoreHook);
  *(BYTE*)(ADDR_CTOR_ZERO + 5) = 0x90;
  *(BYTE*)(ADDR_CTOR_ZERO + 6) = 0x90;
  *(BYTE*)(ADDR_CTOR_ZERO + 7) = 0x90;

  // Menu build: same retarget idiom, so the labels are built from the restored values.
  *(DWORD*)(ADDR_MENUBUILD_CALL + 1) = calc_disp32(ADDR_MENUBUILD_CALL + 1, (ULONG_PTR)preBuildHook);

  // Play Mode label: 7 bytes -> call rel32 + 2 nops. The stub supplies the argument the
  // constructor hardcoded to zero.
  *(BYTE*)(ADDR_MODE_LABEL_ARG) = 0xE8;
  *(DWORD*)(ADDR_MODE_LABEL_ARG + 1) = calc_disp32(ADDR_MODE_LABEL_ARG + 1, (ULONG_PTR)modeLabelHook);
  *(BYTE*)(ADDR_MODE_LABEL_ARG + 5) = 0x90;
  *(BYTE*)(ADDR_MODE_LABEL_ARG + 6) = 0x90;

  // Text-entry widgets: prefill each as it is created, so opening a field to look at it does not
  // clear it.
  patch_widget_store(ADDR_CREATE_NAME, nameWidgetHook);
  patch_widget_store(ADDR_CREATE_PW, passwordWidgetHook);

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

  load_remember_flag();
  load_settings();

  if (patch_gamesettings()) {
    gs_log("patched ok (menubuild 0x%08X, ctor 0x%08X, label 0x%08X, confirm 0x%08X)%s",
           ADDR_MENUBUILD_CALL, ADDR_CTOR_ZERO, ADDR_MODE_LABEL_ARG, ADDR_CONFIRM_CALL,
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
