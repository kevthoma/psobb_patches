// psobb_dsound -- a proxy dsound.dll for PSOBB (59NL).
//
// Why a proxy DLL at all: psobb.exe imports exactly ONE function from dsound.dll -- ordinal #1,
// DirectSoundCreate (verified by parsing the import directory of our own client, not assumed).
// Both the CRI ADX music stream and the effect buffers are created off that single device, so one
// proxy owns the entire audio path with no reverse engineering of the game. The client already
// establishes this pattern three times over: d3d8.dll (our widescreen wrapper + ASI loader) and
// dinput.dll / dinput8.dll (Xidi).
//
// PHASE 1 (done, tested in game): pure pass-through, to de-risk the load path on its own.
// PHASE 2 (here): DirectSoundCreate returns a wrapper that LOGS every CreateSoundBuffer and
//   changes nothing else -- see ds_wrapper.cpp for what the census is trying to settle.
// PHASE 3: that same wrapper scales buffer volume.
//
// Eleven of the twelve exports are still naked jmp stubs. A jmp leaves the caller's stack and
// return address exactly as they were, so the real function cleans up its own stdcall arguments
// and returns straight to the game -- behaviour-preserving without declaring twelve signatures.
// Only DirectSoundCreate is a real typed function, because only it has to wrap what it returns.
//
// Ordinals matter: the game imports BY ORDINAL, so the .def must reproduce the system DLL's
// ordinal layout exactly, and CI diffs the built export table against SysWOW64\dsound.dll to
// enforce it. Note DllCanUnloadNow / DllGetClassObject are @4 / @5, which is not the order you
// would guess.

#include "proxy.h"
#include <stdio.h>
#include <stdarg.h>

FARPROC g_real[IDX_COUNT];

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

static HMODULE g_realModule;
static LONG g_loadState;  // 0 = untried, 1 = loading, 2 = loaded

// ---------------------------------------------------------------------------------------------
// Logging. Same philosophy as the widescreen wrapper's RecoveryLog: a normal session must leave no
// file at all, because this ships to every player. So failures are always logged, and everything
// else only if someone opted in by creating the log file first (an empty dsound_proxy.log next to
// the game executable turns it on). That gives a tester the census without making a log file
// appear on hundreds of machines that will never be looked at.

static bool LogPath(char *path, size_t cap)
{
	if (cap < MAX_PATH + 1)
		return false;
	if (!GetModuleFileNameA(nullptr, path, MAX_PATH))
		return false;
	path[MAX_PATH] = 0;
	char *slash = strrchr(path, '\\');
	if (!slash)
		return false;
	strcpy(slash + 1, "dsound_proxy.log");
	return true;
}

// Cached: the census writes a line per buffer, and hitting the filesystem to ask "is logging on"
// each time would be a silly cost inside the game's audio path.
bool CensusEnabled(void)
{
	static int cached = -1;
	if (cached < 0) {
		char path[MAX_PATH + 1];
		cached = (LogPath(path, sizeof(path)) &&
			GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) ? 1 : 0;
	}
	return cached == 1;
}

void ProxyLog(bool isFailure, const char *fmt, ...)
{
	char path[MAX_PATH + 1];
	if (!LogPath(path, sizeof(path)))
		return;

	if (!isFailure && !CensusEnabled())
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
//
// (GetSystemDirectoryA reports C:\Windows\system32 even in a 32-bit process; the WOW64 filesystem
// redirector maps that to SysWOW64, so this does load the 32-bit DLL. The phase-1 log line saying
// "system32" is expected, not a bug.)
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

	VolumeConfig vc = GetVolumeConfig();
	ProxyLog(false, "psobb_dsound proxy active; forwarding to %s (%d of %d exports resolved)%s; "
		"volume master=%d%% music=%d%% effects=%d%%",
		path, IDX_COUNT - missing, IDX_COUNT,
		CensusEnabled() ? "; buffer census ON" : "",
		vc.master, vc.music, vc.effects);

	InterlockedExchange(&g_loadState, 2);
}

// ---------------------------------------------------------------------------------------------
// The player's settings.
//
// widescreen.cfg is chosen over a file of our own because it is already the one place the launcher
// writes and the client-side wrapper reads -- a second config file would be a second thing for a
// player to lose. Values are percentages; anything missing or unparseable falls back to 100, which
// means "sound exactly as it was before this feature existed".

static int ReadCfgInt(const char *text, const char *key, int dflt, int lo, int hi)
{
	size_t keyLen = strlen(key);
	for (const char *p = text; *p; ) {
		while (*p == ' ' || *p == '\t')
			p++;
		if (_strnicmp(p, key, keyLen) == 0) {
			const char *q = p + keyLen;
			while (*q == ' ' || *q == '\t')
				q++;
			if (*q == '=') {
				int v = atoi(q + 1);
				if (v < lo)
					v = lo;
				if (v > hi)
					v = hi;
				return v;
			}
		}
		while (*p && *p != '\n')
			p++;
		if (*p)
			p++;
	}
	return dflt;
}

static LONG g_master = 100;   // live: the hotkey moves this
static LONG g_music = 100;
static LONG g_effects = 100;
static LONG g_cfgLoaded = 0;

// The hotkey's own configuration, read from the same file. VolumeHotkey=0 turns the whole thing off
// for anyone who would rather the keys did nothing.
LONG g_hotkeyEnabled = 1;
// Arrow keys, not the numpad: the Steam Deck has no numpad at all and plenty of laptops do not
// either, and the swallow means the arrow never reaches the game to move the character.
LONG g_hotkeyDown = VK_DOWN;
LONG g_hotkeyUp = VK_UP;
LONG g_hotkeyMod = 2;              // 0 = none, 1 = Ctrl, 2 = Alt, 3 = Shift
LONG g_hotkeyStep = 5;

static void LoadConfigOnce(void)
{
	if (InterlockedCompareExchange(&g_cfgLoaded, 1, 0) != 0)
		return;

	char path[MAX_PATH + 1];
	if (!GetModuleFileNameA(nullptr, path, MAX_PATH))
		return;
	path[MAX_PATH] = 0;
	char *slash = strrchr(path, '\\');
	if (!slash)
		return;
	strcpy(slash + 1, "widescreen.cfg");

	FILE *f = fopen(path, "rb");
	if (!f) {
		ProxyLog(false, "no widescreen.cfg; volume stays at 100%% on all three");
		return;
	}
	char buf[8192];
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = 0;

	g_master  = ReadCfgInt(buf, "MasterVolume", 100, 0, 100);
	g_music   = ReadCfgInt(buf, "MusicVolume", 100, 0, 100);
	g_effects = ReadCfgInt(buf, "EffectVolume", 100, 0, 100);

	g_hotkeyEnabled = ReadCfgInt(buf, "VolumeHotkey", 1, 0, 1);
	g_hotkeyDown = ReadCfgInt(buf, "VolumeKeyDown", VK_DOWN, 0, 255);
	g_hotkeyUp = ReadCfgInt(buf, "VolumeKeyUp", VK_UP, 0, 255);
	g_hotkeyMod = ReadCfgInt(buf, "VolumeKeyModifier", 2, 0, 3);
	g_hotkeyStep = ReadCfgInt(buf, "VolumeStep", 5, 1, 50);
}

VolumeConfig GetVolumeConfig(void)
{
	LoadConfigOnce();
	VolumeConfig c;
	c.master  = (int)InterlockedCompareExchange(&g_master, 0, 0);
	c.music   = (int)g_music;
	c.effects = (int)g_effects;
	return c;
}

// Rewrites one key in widescreen.cfg in place, preserving every other line. Called from the hotkey
// thread so the level the player settles on survives a restart. The options window is the only
// other writer and cannot be open at the same time as the game in any normal flow.
static void PersistMasterVolume(int percent)
{
	char path[MAX_PATH + 1];
	if (!GetModuleFileNameA(nullptr, path, MAX_PATH))
		return;
	path[MAX_PATH] = 0;
	char *slash = strrchr(path, '\\');
	if (!slash)
		return;
	strcpy(slash + 1, "widescreen.cfg");

	char buf[8192];
	size_t n = 0;
	FILE *f = fopen(path, "rb");
	if (f) {
		n = fread(buf, 1, sizeof(buf) - 1, f);
		fclose(f);
	}
	buf[n] = 0;

	char out[8192 + 64];
	size_t used = 0;
	bool replaced = false;
	const char *p = buf;
	while (*p) {
		const char *eol = p;
		while (*eol && *eol != '\n')
			eol++;
		size_t len = (size_t)(eol - p) + (*eol ? 1 : 0);

		const char *k = p;
		while (*k == ' ' || *k == '	')
			k++;
		if (_strnicmp(k, "MasterVolume", 12) == 0) {
			int w = _snprintf_s(out + used, sizeof(out) - used, _TRUNCATE,
				"MasterVolume=%d\n", percent);
			if (w > 0)
				used += w;
			replaced = true;
		} else if (used + len < sizeof(out)) {
			memcpy(out + used, p, len);
			used += len;
		}
		p = eol + (*eol ? 1 : 0);
	}
	if (!replaced) {
		int w = _snprintf_s(out + used, sizeof(out) - used, _TRUNCATE,
			"MasterVolume=%d\n", percent);
		if (w > 0)
			used += w;
	}

	f = fopen(path, "wb");
	if (!f)
		return;
	fwrite(out, 1, used, f);
	fclose(f);
}

void SetMasterVolume(int percent)
{
	if (percent < 0)
		percent = 0;
	if (percent > 100)
		percent = 100;
	if (InterlockedExchange(&g_master, percent) == percent)
		return;

	ReapplyAllVolumes();
	PersistMasterVolume(percent);
	ProxyLog(false, "master volume -> %d%%", percent);
}

// ---------------------------------------------------------------------------------------------
// The one export that is not a pass-through: it has to wrap the device it returns.

typedef HRESULT(WINAPI *PFN_DirectSoundCreate)(LPCGUID, LPDIRECTSOUND *, LPUNKNOWN);

extern "C" HRESULT WINAPI DirectSoundCreate(LPCGUID pcGuidDevice, LPDIRECTSOUND *ppDS,
	LPUNKNOWN pUnkOuter)
{
	EnsureRealDsound();

	HRESULT hr = ((PFN_DirectSoundCreate)g_real[IDX_DirectSoundCreate])(
		pcGuidDevice, ppDS, pUnkOuter);

	if (SUCCEEDED(hr) && ppDS && *ppDS) {
		*ppDS = WrapDirectSound(*ppDS);
		// Started here rather than in DllMain: there is no point watching for a volume key before
		// there is any audio to change, and DllMain is the wrong place to create threads.
		StartVolumeHotkeyThread();
	}
	else
		ProxyLog(false, "DirectSoundCreate returned hr=0x%08lX; nothing to wrap", hr);

	return hr;
}

// The other eleven exports are naked jmp stubs in stubs.cpp, which deliberately does not include
// dsound.h -- see the comment at the top of that file.

BOOL WINAPI DllMain(HINSTANCE hInstDLL, DWORD dwReason, LPVOID lpReserved)
{
	UNREFERENCED_PARAMETER(lpReserved);

	if (dwReason == DLL_PROCESS_ATTACH)
		DisableThreadLibraryCalls(hInstDLL);

	return TRUE;
}
