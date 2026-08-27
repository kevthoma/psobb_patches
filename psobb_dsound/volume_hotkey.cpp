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
// ⚠ WHY THE KEYBOARD SIDE IS A HOOK AND NOT A POLL, AND WHY THE DEFAULT IS NOT A FUNCTION KEY:
// F1-F12 are ALREADY BOUND in this client. Blue Burst has a per-player "Function key setting"
// choosing whether they drive menu shortcuts or chat shortcuts (newserv documents the bit in
// SaveFileFormats.hh), so the function keys are in use either way -- they were the worst possible
// default and the first version of this file had them. Polling with GetAsyncKeyState observes but
// does not consume, so the game would have received the key as well and done both things at once.
//
// A WH_KEYBOARD_LL hook can return 1 to swallow the key, so the binding is ours alone. Two things
// follow from that, both handled:
//   * The default requires a MODIFIER (Alt) so that nothing is swallowed while the player is typing
//     in chat -- nobody holds Alt to type. Without a modifier, binding a key that produces text
//     would eat that character out of chat messages.
//   * The hook callback does almost nothing: it records the request and returns. Windows silently
//     drops a low-level hook whose thread does not answer within LowLevelHooksTimeout, and applying
//     a volume change walks live buffers and rewrites widescreen.cfg -- far too much to do inside
//     the callback. The polling thread drains the request instead.

#include "proxy.h"
#include <xinput.h>

// Read from widescreen.cfg by LoadConfigOnce in dllmain.cpp.
extern LONG g_hotkeyEnabled;
extern LONG g_hotkeyDown;
extern LONG g_hotkeyUp;
extern LONG g_hotkeyMod;
extern LONG g_hotkeyStep;

typedef DWORD(WINAPI *PFN_XInputGetState)(DWORD, XINPUT_STATE *);
static PFN_XInputGetState g_xinput;

// Steps requested by the hook and not yet applied. Signed: negative is quieter.
static LONG g_pendingSteps;

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
// would quietly change the volume of a game running behind it -- and, worse for a hook, would eat
// the keystroke out of whatever the player was actually typing into.
static bool GameHasFocus(void)
{
	HWND fg = GetForegroundWindow();
	if (!fg)
		return false;
	DWORD pid = 0;
	GetWindowThreadProcessId(fg, &pid);
	return pid == GetCurrentProcessId();
}

static bool ModifierHeld(void)
{
	switch (g_hotkeyMod) {
	case 1: return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
	case 2: return (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
	case 3: return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
	default: return true;  // no modifier configured
	}
}

static LRESULT CALLBACK KeyboardHook(int code, WPARAM wParam, LPARAM lParam)
{
	if (code == HC_ACTION) {
		const KBDLLHOOKSTRUCT *k = (const KBDLLHOOKSTRUCT *)lParam;
		bool isOurs = (k->vkCode == (DWORD)g_hotkeyDown || k->vkCode == (DWORD)g_hotkeyUp);

		if (isOurs && GameHasFocus() && ModifierHeld()) {
			// Alt-modified keys arrive as WM_SYSKEY*, so both forms have to be matched.
			bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
			bool isUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);

			// Auto-repeat is fine here: holding the key ramps the volume, which is what a player
			// expects from a control with no on-screen readout.
			if (isDown)
				InterlockedExchangeAdd(&g_pendingSteps,
					k->vkCode == (DWORD)g_hotkeyUp ? 1 : -1);

			// Swallow the release as well, or the game sees a key-up it never saw a key-down for.
			if (isDown || isUp)
				return 1;
		}
	}
	return CallNextHookEx(nullptr, code, wParam, lParam);
}

// The hook has to live on a thread that pumps messages, so this thread owns both the hook and the
// message loop. The controller is polled from the same loop via a timer, which keeps the whole
// feature on one thread.
static DWORD WINAPI VolumeInputThread(LPVOID)
{
	LoadXInput();

	HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHook, GetModuleHandleW(nullptr), 0);
	if (!hook)
		ProxyLog(true, "could not install the keyboard hook (GetLastError=%lu); the volume key is "
			"unavailable, the controller chord still works", GetLastError());

	UINT_PTR timer = SetTimer(nullptr, 0, 50, nullptr);
	bool prevPadDown = false, prevPadUp = false;

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
		if (msg.message != WM_TIMER) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
			continue;
		}

		int steps = (int)InterlockedExchange(&g_pendingSteps, 0);

		if (g_xinput && GameHasFocus()) {
			bool padDown = false, padUp = false;
			XINPUT_STATE st;
			for (DWORD pad = 0; pad < 4; pad++) {
				if (g_xinput(pad, &st) != ERROR_SUCCESS)
					continue;
				WORD b = st.Gamepad.wButtons;
				if (b & XINPUT_GAMEPAD_BACK) {
					if (b & XINPUT_GAMEPAD_DPAD_DOWN)
						padDown = true;
					if (b & XINPUT_GAMEPAD_DPAD_UP)
						padUp = true;
				}
			}
			// Edge-triggered on the pad: the D-pad has no auto-repeat to inherit, so holding it
			// would otherwise slam the volume to an end stop in a second.
			if (padDown && !prevPadDown)
				steps--;
			if (padUp && !prevPadUp)
				steps++;
			prevPadDown = padDown;
			prevPadUp = padUp;
		}

		if (steps)
			SetMasterVolume(GetVolumeConfig().master + steps * (int)g_hotkeyStep);
	}

	if (timer)
		KillTimer(nullptr, timer);
	if (hook)
		UnhookWindowsHookEx(hook);
	return 0;
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

	HANDLE h = CreateThread(nullptr, 0, VolumeInputThread, nullptr, 0, nullptr);
	if (!h) {
		ProxyLog(true, "could not start the volume input thread (GetLastError=%lu)",
			GetLastError());
		return;
	}
	CloseHandle(h);

	static const char *const kMods[] = { "no modifier", "Ctrl", "Alt", "Shift" };
	ProxyLog(false, "volume hotkey active: %s + key 0x%02X down / 0x%02X up (swallowed, the game "
		"does not see them), or hold BACK and press D-pad down/up on a controller; %d%% per press",
		kMods[g_hotkeyMod & 3], (int)g_hotkeyDown, (int)g_hotkeyUp, (int)g_hotkeyStep);
}
