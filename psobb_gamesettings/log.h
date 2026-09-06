// log.h -- tiny append-only logger for the gamesettings plugin.
//
// Win32 only, no CRT: wsprintfA lives in USER32, so formatting costs no C runtime. Its output is
// capped at 1024 bytes by the API, which is far more than any line here needs.
//
// Two levels, because they answer different questions:
//
//   gs_log()    always compiled in. Records ONE line per launch: whether the byte guards matched and
//               the patch actually applied. That failure is otherwise completely invisible -- a
//               mismatched guard just means settings quietly stop being remembered, with nothing
//               anywhere to say why.
//   gs_diag()   only in a diagnostic build. Per-event detail used to confirm the hooks fire and the
//               offsets are the ones intended.

#pragma once

#ifndef GAMESETTINGS_DIAGNOSTIC
#define GAMESETTINGS_DIAGNOSTIC 0
#endif

#define GS_LOG_FILE   "corellia_gamesettings.log"
#define GS_LOG_MAX    (64 * 1024)   // a dialog reopen logs a line; cap so a long session cannot grow it forever

// Build "<dir of psobb.exe>\<name>". Returns 0 on failure.
static int gs_sibling_path(char* out, DWORD cch, const char* name) {
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

static void gs_write_line(const char* text) {
  char path[MAX_PATH];
  char stamp[64];
  SYSTEMTIME st;
  HANDLE h;
  DWORD put = 0, size;

  if (!gs_sibling_path(path, MAX_PATH, GS_LOG_FILE))
    return;

  h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                  FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE)
    return;                                   // read-only install dir: no log, and no complaint

  // Start over rather than grow without bound. Losing old lines is fine; this is a diagnostic, and
  // the interesting event is always the most recent one.
  size = GetFileSize(h, NULL);
  if (size != INVALID_FILE_SIZE && size > GS_LOG_MAX) {
    CloseHandle(h);
    h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
      return;
  }

  GetLocalTime(&st);
  wsprintfA(stamp, "%04u-%02u-%02u %02u:%02u:%02u  ",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

  SetFilePointer(h, 0, NULL, FILE_END);
  WriteFile(h, stamp, lstrlenA(stamp), &put, NULL);
  WriteFile(h, text, lstrlenA(text), &put, NULL);
  WriteFile(h, "\r\n", 2, &put, NULL);
  CloseHandle(h);
}

static void gs_log(const char* fmt, ...) {
  char line[1024];
  va_list ap;

  va_start(ap, fmt);
  wvsprintfA(line, fmt, ap);
  va_end(ap);
  gs_write_line(line);
}

#if GAMESETTINGS_DIAGNOSTIC
#define gs_diag gs_log
#else
static void gs_diag(const char* fmt, ...) { (void)fmt; }
#endif
