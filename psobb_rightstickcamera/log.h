// log.h -- tiny append-only logger for the right-stick camera plugin.
//
// Win32 only, no CRT: wsprintfA lives in USER32, so formatting costs no C runtime. Its output is
// capped at 1024 bytes by the API, which is far more than any line here needs.
//
// Two levels, because they answer different questions:
//
//   rsc_log()   always compiled in. Records ONE line per launch: the config that was read, and
//               whether the byte guard matched so the hook actually went in. That failure is
//               otherwise completely invisible -- an unmatched guard just means the stick does
//               nothing, with nothing anywhere to say why.
//   rsc_diag()  only in a diagnostic build. Used to settle the two things that CANNOT be determined
//               statically: which DIJOYSTATE2 axes Xidi puts the right stick on, and which
//               g_GenericMenuSubSelection bits are set when the camera should be left alone.
//               ⚠ The camera hook runs once per frame, so anything logged from it MUST be
//               throttled -- see RSC_DIAG_EVERY in dllmain.c. An unthrottled line there writes
//               ~30 lines a second and blows through RSC_LOG_MAX in under a minute.

#pragma once

#ifndef RSC_DIAGNOSTIC
#define RSC_DIAGNOSTIC 0
#endif

#define RSC_LOG_FILE   "corellia_rightstickcamera.log"
#define RSC_LOG_MAX    (64 * 1024)   // diagnostic builds log periodically; cap so a long session cannot grow it forever

// Build "<dir of psobb.exe>\<name>". Returns 0 on failure.
static int rsc_sibling_path(char* out, DWORD cch, const char* name) {
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

static void rsc_write_line(const char* text) {
  char path[MAX_PATH];
  char stamp[64];
  SYSTEMTIME st;
  HANDLE h;
  DWORD put = 0, size;

  if (!rsc_sibling_path(path, MAX_PATH, RSC_LOG_FILE))
    return;

  h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                  FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE)
    return;                                   // read-only install dir: no log, and no complaint

  // Start over rather than grow without bound. Losing old lines is fine; this is a diagnostic, and
  // the interesting event is always the most recent one.
  size = GetFileSize(h, NULL);
  if (size != INVALID_FILE_SIZE && size > RSC_LOG_MAX) {
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

static void rsc_log(const char* fmt, ...) {
  char line[1024];
  va_list ap;

  va_start(ap, fmt);
  wvsprintfA(line, fmt, ap);
  va_end(ap);
  rsc_write_line(line);
}

#if RSC_DIAGNOSTIC
#define rsc_diag rsc_log
#else
static void rsc_diag(const char* fmt, ...) { (void)fmt; }
#endif
