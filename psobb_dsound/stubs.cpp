// The eleven exports the proxy does not care about.
//
// Each one makes sure the real dsound.dll is loaded and then JUMPS to it. A jmp leaves the
// caller's stack and return address exactly as they were, so the real function cleans up its own
// stdcall arguments and returns straight to the game -- which is why these can be
// behaviour-preserving without declaring a single real signature.
//
// That signature-free shape is also why this file must NOT see dsound.h: these functions carry the
// same names as the real exports, and the header's own prototypes for them would be a redefinition
// conflict. PROXY_NO_DSOUND says "I only need the loader plumbing, not the DirectSound types".
// Only DirectSoundCreate is a real typed function, and it lives in dllmain.cpp with the header.

#define PROXY_NO_DSOUND
#include "proxy.h"

// EnsureRealDsound is __cdecl with no arguments, so it needs no stack cleanup, and it preserves
// ebx/esi/edi/ebp per the C ABI. It may clobber eax/ecx/edx, which is harmless here: none of them
// carry stdcall arguments, and the callee we jump to sets eax itself.
#define PROXY_STUB(name, index)                          \
	extern "C" __declspec(naked) void name(void)         \
	{                                                    \
		__asm { call EnsureRealDsound }                  \
		__asm { jmp dword ptr [g_real + index * 4] }     \
	}

PROXY_STUB(DirectSoundEnumerateA, IDX_DirectSoundEnumerateA)
PROXY_STUB(DirectSoundEnumerateW, IDX_DirectSoundEnumerateW)
PROXY_STUB(DllCanUnloadNow, IDX_DllCanUnloadNow)
PROXY_STUB(DllGetClassObject, IDX_DllGetClassObject)
PROXY_STUB(DirectSoundCaptureCreate, IDX_DirectSoundCaptureCreate)
PROXY_STUB(DirectSoundCaptureEnumerateA, IDX_DirectSoundCaptureEnumerateA)
PROXY_STUB(DirectSoundCaptureEnumerateW, IDX_DirectSoundCaptureEnumerateW)
PROXY_STUB(GetDeviceID, IDX_GetDeviceID)
PROXY_STUB(DirectSoundFullDuplexCreate, IDX_DirectSoundFullDuplexCreate)
PROXY_STUB(DirectSoundCreate8, IDX_DirectSoundCreate8)
PROXY_STUB(DirectSoundCaptureCreate8, IDX_DirectSoundCaptureCreate8)
