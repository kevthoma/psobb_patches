// PHASE 3B: change the volume without leaving the game.
//
// The options sliders are the persistence layer and stay the place you set music and effects. This
// adjusts MASTER only, which is the thing anyone actually reaches for mid-session ("too loud right
// now"), and it is deliberately the whole scope: per-category mixing during combat is not a real
// need, and one live number is far easier to reason about on the audio path.
//
// CONTROLLER PARITY IS THE POINT, not a nicety. Every other convenience in this stack is typed --
// $bank, $save, the whole chat-command surface -- and PSO's pad text input is Word Select, which
// emits canned phrases and cannot produce arbitrary text. A keyboard-only volume key would have
// landed straight in that same trap, on a build where the Steam Deck made pad-only play a real
// configuration. So the chord is here from the first commit rather than promised for later.
//
// A polling thread rather than a hook: the proxy has no message loop and no frame tick of its own,
// and a low-level keyboard hook would be both heavier and far more intrusive than reading key state
// twenty times a second.
//
// ⚠ THE GAME STILL SEES THE INPUT. GetAsyncKeyState observes, it does not consume, so whatever the
// bindings are bound to in game ALSO happens. That is why the defaults are F11/F12 -- function keys
// the client has no use for -- and why every binding is configurable in widescreen.cfg rather than
// baked in.

#include "proxy.h"
#include <xinput.h>

// Read from widescreen.cfg by LoadConfigOnce in dllmain.cpp.
extern LONG g_hotkeyEnabled;
extern LONG g_hotkeyDown;
extern LONG g_hotkeyUp;
extern LONG g_hotkeyStep;

typedef DWORD(WINAPI *PFN_XInputGetState)(DWORD, XINPUT_STATE *);
static PFN_XInputGetState g_xinput;

// The controller chord: BACK held, then D-pad up or down. BACK is a modifier here rather than an
// action, so a pad player cannot nudge the volume by accident while moving, and the D-pad keeps the
// stick free. Loaded dynamically because the XInput DLL version varies across Windows releases and
// this must not become a hard load-time dependency -- least of all under Proton on the Deck.
static void LoadXInput(void)
{
	static const char *const kNames[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
	for (int i = 0; i < _countof(kNames) && !g_xinput; i++) {
		HMODULE m = LoadLibraryA(kNames[i]);
		if (m)
			g_xinput = (PFN_XInputGetState)GetProcAddress(m, "XInputGetState");
	}
	if (!g_xinput)
		ProxyLog(false, "no XInput DLL found; the controller volume chord is unavailable");
}

// Only react while the game is the foreground window. Without this, pressing the key in a browser
// would quietly change the volume of a game running behind it.
static bool GameHasFocus(void)
{
	HWND fg = GetForegroundWindow();
	if (!fg)
		return false;
	DWORD pid = 0;
	GetWindowThreadProcessId(fg, &pid);
	return pid == GetCurrentProcessId();
}

static DWORD WINAPI VolumeHotkeyThread(LPVOID)
{
	LoadXInput();

	bool prevDown = false, prevUp = false;
	for (;;) {
		Sleep(50);

		if (!GameHasFocus()) {
			// Drop the edge state, so releasing a key while alt-tabbed away cannot register as a
			// press when the player comes back.
			prevDown = prevUp = false;
			continue;
		}

		bool down = (GetAsyncKeyState((int)g_hotkeyDown) & 0x8000) != 0;
		bool up = (GetAsyncKeyState((int)g_hotkeyUp) & 0x8000) != 0;

		if (g_xinput) {
			XINPUT_STATE st;
			for (DWORD pad = 0; pad < 4; pad++) {
				if (g_xinput(pad, &st) != ERROR_SUCCESS)
					continue;
				WORD b = st.Gamepad.wButtons;
				if (b & XINPUT_GAMEPAD_BACK) {
					if (b & XINPUT_GAMEPAD_DPAD_DOWN)
						down = true;
					if (b & XINPUT_GAMEPAD_DPAD_UP)
						up = true;
				}
			}
		}

		// Edge-triggered: one step per press. Holding the key does not ramp, which is the right
		// call for a control with no on-screen readout -- the player adjusts, listens, adjusts.
		if (down && !prevDown)
			SetMasterVolume(GetVolumeConfig().master - (int)g_hotkeyStep);
		if (up && !prevUp)
			SetMasterVolume(GetVolumeConfig().master + (int)g_hotkeyStep);

		prevDown = down;
		prevUp = up;
	}
}

void StartVolumeHotkeyThread(void)
{
	static LONG started = 0;
	if (InterlockedCompareExchange(&started, 1, 0) != 0)
		return;

	// Reading the config here also forces it to load before the thread races for it.
	VolumeConfig c = GetVolumeConfig();
	if (!g_hotkeyEnabled) {
		ProxyLog(false, "volume hotkey disabled by config (master stays at %d%%)", c.master);
		return;
	}

	HANDLE h = CreateThread(nullptr, 0, VolumeHotkeyThread, nullptr, 0, nullptr);
	if (!h) {
		ProxyLog(true, "could not start the volume hotkey thread (GetLastError=%lu)",
			GetLastError());
		return;
	}
	CloseHandle(h);
	ProxyLog(false, "volume hotkey active: key 0x%02X down / 0x%02X up, or hold BACK and press "
		"D-pad down/up on a controller; %d%% per press",
		(int)g_hotkeyDown, (int)g_hotkeyUp, (int)g_hotkeyStep);
}
