// psobb_dsound -- a proxy dsound.dll for PSOBB (59NL).
//
// PHASE 1: pure pass-through. Every export forwards to the real system dsound.dll and nothing
// else happens. The point is to de-risk the LOAD PATH on its own: if the game starts and audio is
// completely unchanged, the proxy is in place and later phases can wrap buffers with confidence.
//
// Why a proxy DLL at all: psobb.exe imports exactly ONE function from dsound.dll -- ordinal #1,
// DirectSoundCreate (verified by parsing the import directory of our own client, not assumed).
// Both the CRI ADX music stream and the effect buffers are created off that single device, so one
// proxy owns the entire audio path with no reverse engineering of the game. The client already
// establishes this pattern three times over: d3d8.dll (our widescreen wrapper + ASI loader) and
// dinput.dll / dinput8.dll (Xidi).
//
// Forwarding is done with naked jmp stubs rather than typed wrappers. A jmp leaves the caller's
// stack and return address exactly as they were, so the real function cleans up its own stdcall
// arguments and returns straight to the game. That makes phase 1 provably behaviour-preserving for
// all twelve exports without declaring twelve signatures. Phase 3 replaces the DirectSoundCreate
// stub with a real typed wrapper; the other eleven stay as they are forever.
//
// Ordinals matter: the game imports BY ORDINAL (#1), so the .def must reproduce the system DLL's
// ordinal layout exactly. Those ordinals were read out of C:\Windows\SysWOW64\dsound.dll -- note
// DllCanUnloadNow / DllGetClassObject are @4 / @5 here, which is not the order you would guess.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

// Export slots, in ordinal order (index = ordinal - 1). Populated on first use.
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

static const char *const kExportNames[IDX_COUNT] = {
	"DirectSoundCreate",
	"DirectSoundEnumerateA",
	"DirectSoundEnumerateW",
	"DllCanUnloadNow",
	"DllGetClassObject",
	"DirectSoundCaptureCreate",
	"DirectSoundCaptureEnumerateA",
	"DirectSoundCaptureEnumerateW",
	"GetDeviceID",
	"DirectSoundFullDuplexCreate",
	"DirectSoundCreate8",
	"DirectSoundCaptureCreate8",
};

static FARPROC g_real[IDX_COUNT];
static HMODULE g_realModule;
static LONG g_loadState;  // 0 = untried, 1 = loading, 2 = loaded

// ---------------------------------------------------------------------------------------------
// Logging. Same philosophy as the widescreen wrapper's RecoveryLog: a normal session must leave no
// file at all, because this ships to every player. So failures are always logged, and success is
// logged only if someone opted in by creating the log file first (an empty dsound_proxy.log next
// to the game executable turns it on). That gives a tester positive evidence the proxy is live
// without making a log file appear on hundreds of machines that will never be looked at.
static void ProxyLog(bool isFailure, const char *fmt, ...)
{
	char path[MAX_PATH + 1];
	if (!GetModuleFileNameA(nullptr, path, MAX_PATH))
		return;
	path[MAX_PATH] = 0;
	char *slash = strrchr(path, '\\');
	if (!slash)
		return;
	strcpy(slash + 1, "dsound_proxy.log");

	if (!isFailure && GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
		return;  // not opted in, and nothing is wrong -- stay silent

	FILE *f = fopen(path, "a");
	if (!f)
		return;

	SYSTEMTIME st;
	GetLocalTime(&st);
	fprintf(f, "%02d:%02d:%02d.%03d  ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

	va_list args;
	va_start(args, fmt);
	vfprintf(f, fmt, args);
	va_end(args);

	fputc('\n', f);
	fclose(f);
}

// A resolution failure means there is no audio API at all; there is no graceful degradation to
// offer from a naked stub (the correct stdcall cleanup differs per export, so a generic "return an
// error" stub would corrupt the stack). Fail loudly and deterministically instead.
static void FatalProxyError(const char *what)
{
	ProxyLog(true, "FATAL: %s (GetLastError=%lu)", what, GetLastError());
	MessageBoxA(nullptr,
		"Corellia: the sound proxy could not load the system dsound.dll.\n\n"
		"Details were written to dsound_proxy.log next to the game executable.",
		"PSOBB sound", MB_ICONERROR | MB_OK);
	ExitProcess(1);
}

// Loads the REAL dsound.dll out of the system directory. Never LoadLibrary("dsound.dll") -- the
// game folder is searched first, so that would load this proxy again.
//
// Deliberately not done in DllMain: LoadLibrary under the loader lock is how proxy DLLs deadlock.
// The first export call happens well after DllMain has returned, on the thread that initialises
// audio, so lazy resolution here is both safe and early enough.
extern "C" void EnsureRealDsound(void)
{
	if (g_loadState == 2)
		return;

	// First caller wins and does the work; any other thread spins until it is done. In practice
	// the client initialises sound from one thread, but the audio path is not ours to assume.
	if (InterlockedCompareExchange(&g_loadState, 1, 0) != 0) {
		while (g_loadState != 2)
			Sleep(1);
		return;
	}

	char path[MAX_PATH + 1];
	UINT n = GetSystemDirectoryA(path, MAX_PATH);
	if (n == 0 || n > MAX_PATH - 12)
		FatalProxyError("GetSystemDirectoryA failed");
	strcpy(path + n, "\\dsound.dll");

	g_realModule = LoadLibraryA(path);
	if (!g_realModule)
		FatalProxyError("LoadLibraryA on the system dsound.dll failed");

	int missing = 0;
	for (int i = 0; i < IDX_COUNT; i++) {
		g_real[i] = GetProcAddress(g_realModule, kExportNames[i]);
		if (!g_real[i]) {
			ProxyLog(true, "export %s is missing from %s", kExportNames[i], path);
			missing++;
		}
	}
	if (!g_real[IDX_DirectSoundCreate])
		FatalProxyError("the system dsound.dll has no DirectSoundCreate");

	ProxyLog(false, "psobb_dsound proxy active; forwarding to %s (%d of %d exports resolved)",
		path, IDX_COUNT - missing, IDX_COUNT);

	InterlockedExchange(&g_loadState, 2);
}

// Each export: make sure the real DLL is loaded, then jump. EnsureRealDsound is __cdecl with no
// arguments, so it needs no stack cleanup, and it preserves ebx/esi/edi/ebp per the C ABI. It may
// clobber eax/ecx/edx, which is harmless: none of them carry stdcall arguments, and the callee we
// jump to sets eax itself.
#define PROXY_STUB(name, index)                          \
	extern "C" __declspec(naked) void name(void)         \
	{                                                    \
		__asm { call EnsureRealDsound }                  \
		__asm { jmp dword ptr [g_real + index * 4] }     \
	}

PROXY_STUB(DirectSoundCreate, IDX_DirectSoundCreate)
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

BOOL WINAPI DllMain(HINSTANCE hInstDLL, DWORD dwReason, LPVOID lpReserved)
{
	UNREFERENCED_PARAMETER(lpReserved);

	if (dwReason == DLL_PROCESS_ATTACH)
		DisableThreadLibraryCalls(hInstDLL);

	return TRUE;
}
