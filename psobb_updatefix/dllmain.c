// UpdateFix 1.25.13 (59NL)
//
// Lets the patch server update a file the game already has loaded -- every patches/*.asi.
//
// WHY PLUGIN UPDATES NEVER LANDED (corellia-build#7). The patch client runs inside psobb.exe, and
// psobb.exe loads every plugin before it patches anything: its entry point (0x00B60000) calls
// patch.dll's `patch` export, which LoadLibraryA's .\*.asi and .\patches\*.asi, and only then jumps
// to the CRT. When a download finishes, the close-file handler (command 08, 0x0070BB78) does
//
//     0x0070BD73  call remove(name)                 ; DeleteFileA
//     0x0070BD7A  call rename(name.patch, name)     ; MoveFileA
//
// and checks neither result. Windows refuses to delete a mapped image (ERROR_ACCESS_DENIED), so the
// delete fails, the move then fails because the destination still exists (ERROR_ALREADY_EXISTS), and
// <name>.patch is left behind with the correct bytes. Next launch: same file, same download, forever.
// That one fact explains every case we saw -- CREATING a plugin works (nothing to delete), updating
// Corellia.exe works (the game never loads it), and dsound.dll could never be updated either.
//
// THE FIX. Windows does allow RENAMING a loaded image. So the delete is replaced: try it, and if the
// file exists but cannot be deleted, rename it aside to <name>.corellia-old. The client's own move
// then succeeds. This session keeps running the old code under the new name; the next launch loads
// the new file and sweeps the leftovers.
//
// This plugin has to reach an install BEFORE the update it fixes. Shipping it is safe on its own:
// it is a new file, and creating files in patches/ always worked.

#define WINVER 0x0501
#define _WIN32_WINNT 0x0501

#define _CRT_NONSTDC_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define _NO_CRT_STDIO_INLINE
#include <windows.h>
#include "util.h"

#define ADDR_REMOVE_CALL    0x0070BD73   // `call remove` in the close-file handler, rename path
#define ADDR_RENAME_CALL    0x0070BD7A   // `call rename` right after it -- guard only, not patched
#define ADDR_CRT_REMOVE     0x0085A1A6   // CRT remove(): push [esp+4]; call [DeleteFileA]
#define ADDR_CRT_RENAME     0x0085A1D0

#define ASIDE_SUFFIX        ".corellia-old"
#define LOG_FILE            "corellia_updatefix.log"
#define LOG_MAX             (64 * 1024)

// Build "<dir of psobb.exe>\<name>". Returns 0 on failure.
static int sibling_path(char* out, DWORD cch, const char* name) {
  DWORD n = GetModuleFileNameA(NULL, out, cch);
  DWORD i;

  if (n == 0 || n >= cch) {
    out[0] = 0;
    return 0;
  }
  while (n > 0 && out[n - 1] != '\\' && out[n - 1] != '/')
    n--;
  for (i = 0; name[i] && (n + i + 1) < cch; i++)
    out[n + i] = name[i];
  out[n + i] = 0;
  return 1;
}

// One line per event that matters: patched or not, and every file set aside or swept. Without it a
// guard mismatch just means updates quietly keep looping, with nothing anywhere to say why.
static void uf_log(const char* fmt, ...) {
  char path[MAX_PATH];
  char line[1024];
  SYSTEMTIME st;
  HANDLE h;
  DWORD put = 0, size;
  int n;
  va_list ap;

  if (!sibling_path(path, MAX_PATH, LOG_FILE))
    return;

  GetLocalTime(&st);
  n = wsprintfA(line, "%04d-%02d-%02d %02d:%02d:%02d ", st.wYear, st.wMonth, st.wDay, st.wHour,
                st.wMinute, st.wSecond);
  va_start(ap, fmt);
  n += wvsprintfA(line + n, fmt, ap);
  va_end(ap);
  if (n > (int)sizeof(line) - 3)
    n = (int)sizeof(line) - 3;
  line[n++] = '\r';
  line[n++] = '\n';

  h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                  NULL);
  if (h == INVALID_HANDLE_VALUE)
    return;                                   // read-only install dir: no log, and no complaint

  size = GetFileSize(h, NULL);
  if (size != INVALID_FILE_SIZE && size > LOG_MAX) {
    CloseHandle(h);
    h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                    NULL);
    if (h == INVALID_HANDLE_VALUE)
      return;
  }
  WriteFile(h, line, (DWORD)n, &put, NULL);
  CloseHandle(h);
}

// Replaces the client's remove(name). Same contract as the CRT call it stands in for: 0 when the
// name is free for the rename that follows, -1 when it is not. The caller ignores the result anyway.
static int __cdecl remove_or_set_aside(const char* name) {
  char aside[MAX_PATH];
  DWORD err;
  int i, j;

  if (DeleteFileA(name))
    return 0;
  err = GetLastError();
  if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
    return -1;                                // a brand-new file: nothing in the way

  for (i = 0; name[i] && i < MAX_PATH - (int)sizeof(ASIDE_SUFFIX); i++)
    aside[i] = name[i];
  for (j = 0; ASIDE_SUFFIX[j]; j++)
    aside[i + j] = ASIDE_SUFFIX[j];
  aside[i + j] = 0;

  // REPLACE_EXISTING: a leftover from an earlier session is not mapped by this one, so it can go.
  if (MoveFileExA(name, aside, MOVEFILE_REPLACE_EXISTING)) {
    uf_log("set aside %s (delete failed, error %lu) so the update can land", name, err);
    return 0;
  }
  uf_log("FAILED to set aside %s: delete error %lu, rename error %lu -- this update will retry",
         name, err, GetLastError());
  return -1;
}

// Delete <dir>\*.corellia-old. Nothing this process loaded carries that name, so all of it is free.
static void sweep(const char* subdir) {
  char pattern[MAX_PATH], path[MAX_PATH];
  WIN32_FIND_DATAA fd;
  HANDLE f;
  int base;

  if (!sibling_path(pattern, MAX_PATH, subdir))
    return;
  lstrcatA(pattern, "*" ASIDE_SUFFIX);
  f = FindFirstFileA(pattern, &fd);
  if (f == INVALID_HANDLE_VALUE)
    return;

  sibling_path(path, MAX_PATH, subdir);
  base = lstrlenA(path);
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
      continue;
    if (base + lstrlenA(fd.cFileName) >= MAX_PATH)
      continue;
    path[base] = 0;
    lstrcatA(path, fd.cFileName);
    if (DeleteFileA(path))
      uf_log("swept %s%s", subdir, fd.cFileName);
  } while (FindNextFileA(f, &fd));
  FindClose(f);
}

static BOOL patch_remove(void) {
  // push ebx; call remove; push ebx; push ebp; call rename -- the rename path of the close-file
  // handler, and nowhere else. The flag==1 path has its own remove at 0x0070BC35 that deletes the
  // .patch file itself; that one must stay a plain delete.
  if (*(BYTE*)(ADDR_REMOVE_CALL - 1) != 0x53 || *(BYTE*)ADDR_REMOVE_CALL != 0xE8)
    return FALSE;
  if ((DWORD)(ADDR_REMOVE_CALL + 5 + *(LONG*)(ADDR_REMOVE_CALL + 1)) != ADDR_CRT_REMOVE)
    return FALSE;
  if (*(WORD*)(ADDR_RENAME_CALL - 2) != 0x5553 || *(BYTE*)ADDR_RENAME_CALL != 0xE8)
    return FALSE;
  if ((DWORD)(ADDR_RENAME_CALL + 5 + *(LONG*)(ADDR_RENAME_CALL + 1)) != ADDR_CRT_RENAME)
    return FALSE;
  // The CRT remove itself: push [esp+4]; call [DeleteFileA]. Confirms the stand-in's contract.
  if (*(DWORD*)ADDR_CRT_REMOVE != 0x042474FF || *(WORD*)(ADDR_CRT_REMOVE + 4) != 0x15FF)
    return FALSE;

  // No VirtualProtect: this client's .text is already RWX, as every plugin in this repo relies on.
  *(DWORD*)(ADDR_REMOVE_CALL + 1) = calc_disp32(ADDR_REMOVE_CALL + 1, (ULONG_PTR)remove_or_set_aside);
  return TRUE;
}

__declspec(dllexport) void __stdcall load(void) {
  if (GetImageSize(0) < 0x00762000 || *(DWORD*)0x00B613FA != 0x4C4E3935) { // 59NL
    MessageBoxA(0, "UpdateFix: Wrong client version, expected MTethVer12513 (1.25.13)", "Error", MB_ICONERROR);
    return;
  }

  sweep("");
  sweep("patches\\");

  if (patch_remove())
    uf_log("patched ok (call %08X -> remove_or_set_aside)", ADDR_REMOVE_CALL);
  else
    uf_log("NOT patched: close-file handler did not match at %08X -- plugin updates will still loop",
           ADDR_REMOVE_CALL);
}

int __stdcall DllMain(HINSTANCE hInstDLL, DWORD dwReason, LPVOID lpReserved) {
  if (dwReason == DLL_PROCESS_ATTACH)
    DisableThreadLibraryCalls(hInstDLL);

  return TRUE;
}
