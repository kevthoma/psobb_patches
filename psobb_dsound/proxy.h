// Shared internals of the proxy dsound.dll. See dllmain.cpp for the why of the whole thing.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// The naked forwarding stubs (stubs.cpp) define functions with the same NAMES as the real
// dsound exports but a deliberately signature-free `void f(void)` shape -- they jump rather than
// call, so they never need the real prototypes. That collides with dsound.h's own declarations of
// DirectSoundEnumerateA, DllCanUnloadNow and friends, so stubs.cpp defines PROXY_NO_DSOUND and
// skips the header it does not need. Everything that actually touches DirectSound types leaves it
// undefined and gets the real declarations.
//
// mmsystem.h comes first because WIN32_LEAN_AND_MEAN excludes it, and dsound.h needs WAVEFORMATEX
// from it -- without it dsound.h fails to compile with a wall of "missing type specifier".
#ifndef PROXY_NO_DSOUND
#include <mmsystem.h>
#include <dsound.h>
#endif

// Export slots, in ordinal order (index = ordinal - 1). The ordinals are the system DLL's, not a
// guess: the game imports DirectSoundCreate BY ORDINAL, and CI diffs our export table against
// SysWOW64\dsound.dll on every build.
enum {
	IDX_DirectSoundCreate = 0,
	IDX_DirectSoundEnumerateA,
	IDX_DirectSoundEnumerateW,
	IDX_DllCanUnloadNow,
	IDX_DllGetClassObject,
	IDX_DirectSoundCaptureCreate,
	IDX_DirectSoundCaptureEnumerateA,
	IDX_DirectSoundCaptureEnumerateW,
	IDX_GetDeviceID,
	IDX_DirectSoundFullDuplexCreate,
	IDX_DirectSoundCreate8,
	IDX_DirectSoundCaptureCreate8,
	IDX_COUNT
};

extern FARPROC g_real[IDX_COUNT];

// Resolves the real system dsound.dll on first use. Safe to call from any export, any thread.
extern "C" void EnsureRealDsound(void);

// Writes to dsound_proxy.log next to the game executable. Failures are always recorded; anything
// else only when the file already exists, so a normal session leaves nothing behind. Creating an
// empty dsound_proxy.log is how a tester opts in.
void ProxyLog(bool isFailure, const char *fmt, ...);

// True once the census has been opted into (the log file exists). Cached after the first check so
// the per-buffer path does not stat the disk.
bool CensusEnabled(void);

#ifndef PROXY_NO_DSOUND
// Wraps a real IDirectSound so CreateSoundBuffer can be observed (phase 2) and, later, scaled
// (phase 3). Returns the wrapper, or the original pointer if wrapping is not possible.
IDirectSound *WrapDirectSound(IDirectSound *real);
#endif
