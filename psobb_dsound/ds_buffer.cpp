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
	const VolumeConfig &c = GetVolumeConfig();
	double master = c.master / 100.0;
	double category = (isMusic ? c.music : c.effects) / 100.0;
	return FractionToDb(master * category);
}

// A private interface id, so the proxy can recognise its own wrappers. Needed because the game can
// hand a buffer back to us -- DuplicateSoundBuffer takes one as an argument -- and passing our
// wrapper to the real DirectSound would be a correctness bug, not merely a volume one: the real
// implementation expects its own object. Recognising the wrapper lets us pass the real buffer
// through and copy the music/effect classification onto the duplicate.
// {6C4E1A52-9D3B-4E77-9E6B-1F0A2C5D8B31}
static const GUID kIID_PsobbBufferProxy =
	{ 0x6C4E1A52, 0x9D3B, 0x4E77, { 0x9E, 0x6B, 0x1F, 0x0A, 0x2C, 0x5D, 0x8B, 0x31 } };

// Defined locally so this DLL needs no dxguid.lib, same as in ds_wrapper.cpp. Values from the
// DirectX headers: IDirectSound is ...83, IDirectSoundBuffer is ...85.
static const GUID kIID_IUnknown =
	{ 0x00000000, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
static const GUID kIID_IDirectSoundBuffer =
	{ 0x279AFA85, 0x4981, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } };

namespace {

class SoundBufferProxy : public IDirectSoundBuffer {
public:
	SoundBufferProxy(IDirectSoundBuffer *real, bool isMusic)
		: m_real(real), m_refs(1), m_isMusic(isMusic), m_gameVolume(DSBVOLUME_MAX),
		  m_offset(VolumeOffsetFor(isMusic)) {}

	// Apply the player's setting to a freshly created buffer. The game has not called SetVolume
	// yet, so its notional volume is full scale and the applied value is just our offset.
	void ApplyInitialVolume()
	{
		if (m_offset != DSBVOLUME_MAX)
			m_real->SetVolume(Combine(m_gameVolume));
	}

	// --- IUnknown ---
	IDirectSoundBuffer *Real() const { return m_real; }
	bool IsMusic() const { return m_isMusic; }

	STDMETHOD(QueryInterface)(REFIID riid, LPVOID *ppv) override
	{
		if (!ppv)
			return E_POINTER;

		if (memcmp(&riid, &kIID_PsobbBufferProxy, sizeof(GUID)) == 0) {
			AddRef();
			*ppv = this;
			return S_OK;
		}

		// Asked for what we ARE: hand back the wrapper. This was previously forwarded to the real
		// buffer along with everything else, which was wrong twice over:
		//
		//   * It breaks COM identity. QueryInterface(IID_IUnknown) must return the SAME pointer for
		//     the same object, and returning the real buffer's IUnknown returns a different one.
		//     Anything comparing pointers to decide "is this the buffer I already have" gets a
		//     wrong answer. The device wrapper next door always did this correctly; the buffer
		//     wrapper did not, and the inconsistency was the bug.
		//   * A caller that asked for IID_IDirectSoundBuffer received the raw buffer and from then
		//     on bypassed the volume scaling entirely.
		if (memcmp(&riid, &kIID_IDirectSoundBuffer, sizeof(GUID)) == 0 ||
			memcmp(&riid, &kIID_IUnknown, sizeof(GUID)) == 0) {
			AddRef();
			*ppv = static_cast<IDirectSoundBuffer *>(this);
			return S_OK;
		}

		// Everything else is forwarded. The interfaces the client actually asks for here are
		// IDirectSoundNotify (2,412 buffers set CTRLPOSITIONNOTIFY) and IDirectSound3DBuffer
		// (CTRL3D), and neither carries a volume control -- IDirectSound3DBuffer does distance
		// attenuation, which is the game's business and composes with our scalar rather than
		// fighting it. Implementing them would be all risk and no benefit.
		//
		// Note what this means: a caller holding one of those DOES talk to the real buffer
		// directly. That is fine for what they do, and their reference keeps the real object
		// alive on its own refcount, independent of ours.
		return m_real->QueryInterface(riid, ppv);
	}

	STDMETHOD_(ULONG, AddRef)() override { return (ULONG)InterlockedIncrement(&m_refs); }

	STDMETHOD_(ULONG, Release)() override
	{
		LONG n = InterlockedDecrement(&m_refs);
		if (n == 0) {
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
	p->ApplyInitialVolume();
	return p;
}
