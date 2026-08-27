// PHASE 3: actually scale the volume.
//
// Every buffer the game creates gets wrapped so its volume can be scaled by the player's setting.
// Two things make this simpler than the pre-build plan assumed, both measured by the phase-2
// census rather than guessed:
//
//   * The game already asks for DSBCAPS_CTRLVOLUME on every buffer it creates (6,362 of 6,362), so
//     the proxy does NOT have to rewrite descriptors to add it. SetVolume just works.
//   * Music and effects are cleanly separable: DSBCAPS_GETCURRENTPOSITION2 was set on all 24
//     streaming buffers and none of the 6,338 effect buffers, with the sample rate agreeing on
//     every single one. That is what makes separate sliders honest rather than aspirational.
//
// The census also sized this code: the client creates a NEW buffer per playback rather than reusing
// one -- 1,878 separate buffers for a single 0.239 s sound, about four a second. So the scalar has
// to be applied at creation, not only when a setting changes, and this path has to stay cheap.
//
// Volume is combined, never overwritten. The game drives its own fades through SetVolume, so the
// wrapper remembers what the game last asked for and applies (game + our offset); GetVolume hands
// back the game's own value, so a read-modify-write fade in the client cannot compound our scalar
// into itself and walk the volume down to silence.

#include "proxy.h"
#include <math.h>
#include <new>

// DirectSound volume is HUNDREDTHS OF A DECIBEL, not linear: 0 is full scale and DSBVOLUME_MIN
// (-10000) is silence. A linear 0-100% slider mapped straight onto that range would sound nearly
// silent at 50%, so convert properly.
static LONG FractionToDb(double fraction)
{
	if (fraction <= 0.0)
		return DSBVOLUME_MIN;
	if (fraction >= 1.0)
		return DSBVOLUME_MAX;  // 0 -- full scale, no attenuation
	double db = 2000.0 * log10(fraction);
	if (db <= (double)DSBVOLUME_MIN)
		return DSBVOLUME_MIN;
	return (LONG)db;
}

LONG VolumeOffsetFor(bool isMusic)
{
	VolumeConfig c = GetVolumeConfig();
	double master = c.master / 100.0;
	double category = (isMusic ? c.music : c.effects) / 100.0;
	return FractionToDb(master * category);
}

// The registry of buffers that exist right now.
//
// Phase 3 applied volume at creation and on the game's own SetVolume, which was enough while the
// only way to change a setting was to quit to the launcher. The in-game hotkey needs to reach
// buffers that already exist -- above all the music stream, which is created once and then plays
// for minutes -- so wrappers link themselves into this list for as long as they live.
//
// An intrusive doubly-linked list rather than a container: insert and remove are both O(1) and
// allocation-free, which matters because the client creates a new buffer per playback, about four a
// second, rather than reusing them.
static CRITICAL_SECTION g_listLock;
static LONG g_listReady = 0;

static void EnsureListLock(void)
{
	// Racy-looking but not: the first buffer is created long before any second audio thread exists,
	// and InitializeCriticalSection on an already-initialised section is the only thing being
	// guarded against.
	if (InterlockedCompareExchange(&g_listReady, 1, 0) == 0)
		InitializeCriticalSection(&g_listLock);
}

namespace { class SoundBufferProxy; }
static void RegisterBuffer(SoundBufferProxy *b);
static void UnregisterBuffer(SoundBufferProxy *b);

// A private interface id, so the proxy can recognise its own wrappers. Needed because the game can
// hand a buffer back to us -- DuplicateSoundBuffer takes one as an argument -- and passing our
// wrapper to the real DirectSound would be a correctness bug, not merely a volume one: the real
// implementation expects its own object. Recognising the wrapper lets us pass the real buffer
// through and copy the music/effect classification onto the duplicate.
// {6C4E1A52-9D3B-4E77-9E6B-1F0A2C5D8B31}
static const GUID kIID_PsobbBufferProxy =
	{ 0x6C4E1A52, 0x9D3B, 0x4E77, { 0x9E, 0x6B, 0x1F, 0x0A, 0x2C, 0x5D, 0x8B, 0x31 } };

namespace {

class SoundBufferProxy : public IDirectSoundBuffer {
public:
	SoundBufferProxy(IDirectSoundBuffer *real, bool isMusic)
		: m_real(real), m_refs(1), m_isMusic(isMusic), m_gameVolume(DSBVOLUME_MAX),
		  m_offset(VolumeOffsetFor(isMusic)), m_prev(nullptr), m_next(nullptr) {}

	// Recompute from the current settings and push the result at the buffer. Called on every open
	// buffer when the hotkey moves the master level.
	void RefreshVolume()
	{
		m_offset = VolumeOffsetFor(m_isMusic);
		m_real->SetVolume(Combine(m_gameVolume));
	}

	SoundBufferProxy *m_prev;
	SoundBufferProxy *m_next;

	// Apply the player's setting to a freshly created buffer. The game has not called SetVolume
	// yet, so its notional volume is full scale and the applied value is just our offset.
	void ApplyInitialVolume()
	{
		if (m_offset != DSBVOLUME_MAX)
			m_real->SetVolume(Combine(m_gameVolume));
	}

	IDirectSoundBuffer *Real() const { return m_real; }
	bool IsMusic() const { return m_isMusic; }

	// Linkage for the live-buffer registry below. Public because the registry is a pair of free
	// functions in this file rather than a class -- there is one list and it has one owner.
	SoundBufferProxy *m_prev;
	SoundBufferProxy *m_next;

	// --- IUnknown ---
	STDMETHOD(QueryInterface)(REFIID riid, LPVOID *ppv) override
	{
		if (ppv && memcmp(&riid, &kIID_PsobbBufferProxy, sizeof(GUID)) == 0) {
			AddRef();
			*ppv = this;
			return S_OK;
		}

		// Forwarded untouched. The interfaces the client actually asks for here are
		// IDirectSoundNotify (2,412 buffers set CTRLPOSITIONNOTIFY) and IDirectSound3DBuffer
		// (CTRL3D), and neither carries a volume control -- IDirectSound3DBuffer does distance
		// attenuation, which is the game's business and composes with our scalar rather than
		// fighting it. Pretending to implement them would be all risk and no benefit.
		return m_real->QueryInterface(riid, ppv);
	}

	STDMETHOD_(ULONG, AddRef)() override { return (ULONG)InterlockedIncrement(&m_refs); }

	STDMETHOD_(ULONG, Release)() override
	{
		LONG n = InterlockedDecrement(&m_refs);
		if (n == 0) {
			UnregisterBuffer(this);
			m_real->Release();
			delete this;
			return 0;
		}
		return (ULONG)n;
	}

	// --- the two methods that exist for this feature ---
	STDMETHOD(SetVolume)(LONG volume) override
	{
		// Remember what the GAME wanted, apply what the PLAYER should hear.
		m_gameVolume = volume;
		return m_real->SetVolume(Combine(volume));
	}

	STDMETHOD(GetVolume)(LPLONG volume) override
	{
		// Deliberately not m_real->GetVolume: hand back the game's own value so its fade logic
		// sees what it set, not what we set.
		if (!volume)
			return E_POINTER;
		*volume = m_gameVolume;
		return DS_OK;
	}

	// --- everything else: straight through ---
	STDMETHOD(GetCaps)(LPDSBCAPS c) override { return m_real->GetCaps(c); }
	STDMETHOD(GetCurrentPosition)(LPDWORD p, LPDWORD w) override
		{ return m_real->GetCurrentPosition(p, w); }
	STDMETHOD(GetFormat)(LPWAVEFORMATEX f, DWORD size, LPDWORD written) override
		{ return m_real->GetFormat(f, size, written); }
	STDMETHOD(GetPan)(LPLONG pan) override { return m_real->GetPan(pan); }
	STDMETHOD(GetFrequency)(LPDWORD f) override { return m_real->GetFrequency(f); }
	STDMETHOD(GetStatus)(LPDWORD s) override { return m_real->GetStatus(s); }
	STDMETHOD(Initialize)(LPDIRECTSOUND ds, LPCDSBUFFERDESC d) override
		{ return m_real->Initialize(ds, d); }
	STDMETHOD(Lock)(DWORD off, DWORD bytes, LPVOID *p1, LPDWORD b1, LPVOID *p2, LPDWORD b2,
		DWORD flags) override { return m_real->Lock(off, bytes, p1, b1, p2, b2, flags); }
	STDMETHOD(Play)(DWORD r1, DWORD pri, DWORD flags) override
		{ return m_real->Play(r1, pri, flags); }
	STDMETHOD(SetCurrentPosition)(DWORD pos) override { return m_real->SetCurrentPosition(pos); }
	STDMETHOD(SetFormat)(LPCWAVEFORMATEX f) override { return m_real->SetFormat(f); }
	STDMETHOD(SetPan)(LONG pan) override { return m_real->SetPan(pan); }
	STDMETHOD(SetFrequency)(DWORD f) override { return m_real->SetFrequency(f); }
	STDMETHOD(Stop)() override { return m_real->Stop(); }
	STDMETHOD(Unlock)(LPVOID p1, DWORD b1, LPVOID p2, DWORD b2) override
		{ return m_real->Unlock(p1, b1, p2, b2); }
	STDMETHOD(Restore)() override { return m_real->Restore(); }

private:
	LONG Combine(LONG gameVolume) const
	{
		LONG v = gameVolume + m_offset;   // both are attenuations in hundredths of a dB
		if (v < DSBVOLUME_MIN)
			return DSBVOLUME_MIN;
		if (v > DSBVOLUME_MAX)
			return DSBVOLUME_MAX;
		return v;
	}

	IDirectSoundBuffer *m_real;
	LONG m_refs;
	bool m_isMusic;
	LONG m_gameVolume;   // what the game last asked for, in hundredths of a dB
	LONG m_offset;       // the player's setting, same units, <= 0
};

static SoundBufferProxy *g_head = nullptr;

static void RegisterBuffer(SoundBufferProxy *b)
{
	EnsureListLock();
	EnterCriticalSection(&g_listLock);
	b->m_prev = nullptr;
	b->m_next = g_head;
	if (g_head)
		g_head->m_prev = b;
	g_head = b;
	LeaveCriticalSection(&g_listLock);
}

static void UnregisterBuffer(SoundBufferProxy *b)
{
	EnterCriticalSection(&g_listLock);
	if (b->m_prev)
		b->m_prev->m_next = b->m_next;
	else if (g_head == b)
		g_head = b->m_next;
	if (b->m_next)
		b->m_next->m_prev = b->m_prev;
	b->m_prev = b->m_next = nullptr;
	LeaveCriticalSection(&g_listLock);
}

}  // namespace

bool UnwrapSoundBuffer(IDirectSoundBuffer *maybeWrapper, IDirectSoundBuffer **real, bool *isMusic)
{
	if (!maybeWrapper)
		return false;
	void *p = nullptr;
	if (FAILED(maybeWrapper->QueryInterface(kIID_PsobbBufferProxy, &p)) || !p)
		return false;
	SoundBufferProxy *w = static_cast<SoundBufferProxy *>(p);
	if (real)
		*real = w->Real();
	if (isMusic)
		*isMusic = w->IsMusic();
	w->Release();  // the QueryInterface above took a reference; the caller borrows, it does not own
	return true;
}

IDirectSoundBuffer *WrapSoundBuffer(IDirectSoundBuffer *real, bool isMusic)
{
	if (!real)
		return real;

	SoundBufferProxy *p = new (std::nothrow) SoundBufferProxy(real, isMusic);
	if (!p) {
		// Hand back the unwrapped buffer rather than failing the creation: the player loses volume
		// control on this one sound, which is vastly better than the game losing the sound.
		ProxyLog(true, "could not allocate a buffer wrapper; this buffer plays unscaled");
		return real;
	}
	RegisterBuffer(p);
	p->ApplyInitialVolume();
	return p;
}

void ReapplyAllVolumes(void)
{
	EnsureListLock();
	EnterCriticalSection(&g_listLock);
	// SetVolume is called with the lock held. That is deliberate: releasing it to build a snapshot
	// would let a buffer be freed underneath us, and DirectSound's SetVolume is a cheap call on a
	// handful of live buffers, not something worth risking a use-after-free to avoid.
	for (SoundBufferProxy *b = g_head; b; b = b->m_next)
		b->RefreshVolume();
	LeaveCriticalSection(&g_listLock);
}
