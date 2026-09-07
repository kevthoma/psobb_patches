// probe.h -- TEMPORARY, DIAGNOSTIC BUILDS ONLY. Delete once the question below is answered.
//
// THE QUESTION: the create-game dialog builds its text-entry widget at two sites, and it is not
// established which one is Party Name and which is Password. Static reading is ambiguous and points
// both ways:
//
//   site A  0x00734213  constructs with maxlen 0x10 (16)
//   site B  0x0073432D  constructs with maxlen 0x0E (14), plus two extra arguments
//
// 14 + the two-character "\tE" language marker = the 16-wchar wire field, which argues site B is the
// NAME -- the opposite of what address proximity suggests, and site B's extra arguments hint at a
// different widget configuration (masking?). Guessing wrong puts the password in the name box.
//
// THE METHOD: the two consuming paths ARE unambiguous -- one stores to +0x28 (password) and the other
// to +0x24 (name), and each calls get_text on whatever widget is at +0x34 immediately beforehand. So
// log the widget pointer at both creation sites and at both consuming sites, and the pointers
// correlate the two halves with no inference at all.
//
// Nothing here changes behaviour: the creation hooks reproduce the instructions they replace, and the
// get_text hooks tail-jump to the real function. Deliberately no string contents are logged -- the
// pointer alone answers the question, and one of these fields is a password.

#pragma once

#if GAMESETTINGS_DIAGNOSTIC

#define PROBE_CREATE_A      0x00734213   // mov eax,[ebp-0x14]; mov [eax+0x34],edx   (maxlen 0x10)
#define PROBE_CREATE_B      0x0073432D   // same two instructions                    (maxlen 0x0E)
#define PROBE_GETTEXT_PW    0x007342C1   // call get_text, result stored to +0x28
#define PROBE_GETTEXT_NAME  0x007343E6   // call get_text, result stored to +0x24
#define ADDR_GET_TEXT       0x0078ED4C

static void __cdecl probe_created(void* widget, int site) {
  gs_log("probe: widget %08X created at site %c (maxlen %s)",
         (DWORD)widget, site, site == 'A' ? "0x10" : "0x0E");
}

static void __cdecl probe_consumed(void* widget, int which) {
  gs_log("probe: widget %08X read by the %s path (stores to +0x%02X)",
         (DWORD)widget, which ? "NAME" : "PASSWORD", which ? 0x24 : 0x28);
}

// Reproduce `mov eax,[ebp-0x14]` / `mov [eax+0x34],edx` after logging. ebp is the caller's frame and
// is untouched by a naked callee, so the reproduced instructions see exactly what they would have.
void __declspec(naked) probeCreateAHook(void) {
  __asm {
    pushad
    push 'A'
    push edx
    call probe_created
    add  esp, 8
    popad
    mov  eax, dword ptr [ebp - 0x14]
    mov  dword ptr [eax + 0x34], edx
    ret
  }
}

void __declspec(naked) probeCreateBHook(void) {
  __asm {
    pushad
    push 'B'
    push edx
    call probe_created
    add  esp, 8
    popad
    mov  eax, dword ptr [ebp - 0x14]
    mov  dword ptr [eax + 0x34], edx
    ret
  }
}

// ecx = the widget get_text is about to read. Tail-jump so the real return value still arrives.
void __declspec(naked) probeGetTextPwHook(void) {
  __asm {
    pushad
    push 0
    push ecx
    call probe_consumed
    add  esp, 8
    popad
    mov  eax, ADDR_GET_TEXT
    jmp  eax
  }
}

void __declspec(naked) probeGetTextNameHook(void) {
  __asm {
    pushad
    push 1
    push ecx
    call probe_consumed
    add  esp, 8
    popad
    mov  eax, ADDR_GET_TEXT
    jmp  eax
  }
}

// The six bytes both creation sites share: 8b 45 ec 89 50 34
// Compared byte by byte on purpose -- a packed little-endian constant here is easy to write
// backwards, and a guard that is wrong in the safe direction just silently disables the probe.
static BOOL probe_site_is_create(ULONG_PTR at) {
  static const BYTE want[6] = { 0x8B, 0x45, 0xEC, 0x89, 0x50, 0x34 };
  int i;
  for (i = 0; i < 6; i++)
    if (*(BYTE*)(at + i) != want[i])
      return FALSE;
  return TRUE;
}

static BOOL probe_site_is_gettext(ULONG_PTR at) {
  return *(BYTE*)at == 0xE8 &&
         (DWORD)(at + 5 + *(LONG*)(at + 1)) == ADDR_GET_TEXT;
}

static void probe_patch_create(ULONG_PTR at, void* stub) {
  *(BYTE*)at = 0xE8;
  *(DWORD*)(at + 1) = calc_disp32(at + 1, (ULONG_PTR)stub);
  *(BYTE*)(at + 5) = 0x90;
}

static void install_probe(void) {
  if (!probe_site_is_create(PROBE_CREATE_A) || !probe_site_is_create(PROBE_CREATE_B) ||
      !probe_site_is_gettext(PROBE_GETTEXT_PW) || !probe_site_is_gettext(PROBE_GETTEXT_NAME)) {
    gs_log("probe: sites did not match; probe NOT installed");
    return;
  }
  probe_patch_create(PROBE_CREATE_A, probeCreateAHook);
  probe_patch_create(PROBE_CREATE_B, probeCreateBHook);
  *(DWORD*)(PROBE_GETTEXT_PW + 1) = calc_disp32(PROBE_GETTEXT_PW + 1, (ULONG_PTR)probeGetTextPwHook);
  *(DWORD*)(PROBE_GETTEXT_NAME + 1) = calc_disp32(PROBE_GETTEXT_NAME + 1, (ULONG_PTR)probeGetTextNameHook);
  gs_log("probe: installed (create A/B + get_text name/password)");
}

#else
static void install_probe(void) { }
#endif
