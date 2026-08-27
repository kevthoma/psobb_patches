// PHASE 2: the buffer census.
//
// The whole feature turns on one question that no amount of reading can answer: when the client
// creates its sound buffers, is the ADX music stream distinguishable from the one-shot effects?
// If it is, separate BGM and SE sliders are possible; if it is not, the honest deliverable is a
// single master slider. So this wraps IDirectSound and logs every CreateSoundBuffer -- flags,
// size, and wave format -- and otherwise changes nothing at all. The real buffer is handed back
// to the game unwrapped, so audio behaviour in phase 2 is still bit-for-bit what it was.
//
// Wrapping the interface (rather than patching the vtable the system DLL hands out) keeps the
// change local to the objects we created: a vtable patch would be process-global and would also
// affect any other DirectSound user in the process.
//
// One thing logged here is aimed squarely at phase 3: whether the game asks for DSBCAPS_CTRLVOLUME.
// It has no reason to, and SetVolume simply fails on a buffer that did not request it -- so phase 3
// will have to ADD that flag at creation. The census turns that from an assumption into a measured
// fact before any code depends on it.

#include "proxy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <new>

// Defined locally so this DLL needs no dxguid.lib. Values are from the DirectX headers.
static const GUID kIID_IUnknown =
	{ 0x00000000, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
static const GUID kIID_IDirectSound =
	{ 0x279AFA83, 0x4981, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } };

static bool SameGuid(REFIID a, const GUID &b)
{
	return memcmp(&a, &b, sizeof(GUID)) == 0;
}

// ---------------------------------------------------------------------------------------------
// Census formatting

static void DescribeFlags(DWORD f, char *out, size_t cap)
{
	struct { DWORD bit; const char *name; } kNames[] = {
		{ DSBCAPS_PRIMARYBUFFER,       "PRIMARYBUFFER" },
		{ DSBCAPS_STATIC,              "STATIC" },
		{ DSBCAPS_LOCHARDWARE,         "LOCHARDWARE" },
		{ DSBCAPS_LOCSOFTWARE,         "LOCSOFTWARE" },
		{ DSBCAPS_CTRL3D,              "CTRL3D" },
		{ DSBCAPS_CTRLFREQUENCY,       "CTRLFREQUENCY" },
		{ DSBCAPS_CTRLPAN,             "CTRLPAN" },
		{ DSBCAPS_CTRLVOLUME,          "CTRLVOLUME" },
		{ DSBCAPS_CTRLPOSITIONNOTIFY,  "CTRLPOSITIONNOTIFY" },
		{ DSBCAPS_STICKYFOCUS,         "STICKYFOCUS" },
		{ DSBCAPS_GLOBALFOCUS,         "GLOBALFOCUS" },
		{ DSBCAPS_GETCURRENTPOSITION2, "GETCURRENTPOSITION2" },
		{ DSBCAPS_MUTE3DATMAXDISTANCE, "MUTE3DATMAXDISTANCE" },
		{ DSBCAPS_LOCDEFER,            "LOCDEFER" },
	};

	out[0] = 0;
	size_t used = 0;
	for (int i = 0; i < _countof(kNames); i++) {
		if (!(f & kNames[i].bit))
			continue;
		size_t n = strlen(kNames[i].name);
		if (used + n + 2 >= cap)
			break;
		if (used) {
			out[used++] = '|';
			out[used] = 0;
		}
		memcpy(out + used, kNames[i].name, n + 1);
		used += n;
	}
	if (!used && cap)
		strcpy(out, "(none)");
}

static void LogBufferDesc(const char *what, LPCDSBUFFERDESC d, HRESULT hr, unsigned index)
{
	if (!CensusEnabled())
		return;

	if (!d) {
		ProxyLog(false, "%s #%u: (null descriptor) hr=0x%08lX", what, index, hr);
		return;
	}

	char flags[512];
	DescribeFlags(d->dwFlags, flags, sizeof(flags));

	const WAVEFORMATEX *w = d->lpwfxFormat;
	if (w) {
		// Seconds of audio the buffer holds. This is the most telling number in the census: a
		// streaming music buffer is a small rolling window (a fraction of a second to a couple of
		// seconds) refilled continuously, while a static effect buffer holds an entire short sound.
		double seconds = w->nAvgBytesPerSec ? (double)d->dwBufferBytes / w->nAvgBytesPerSec : 0.0;
		ProxyLog(false,
			"%s #%u: bytes=%lu (%.3f s) fmt=tag%u %luHz %uch %ubit avg=%lu/s "
			"CTRLVOLUME=%s flags=0x%08lX [%s] descSize=%lu hr=0x%08lX",
			what, index, d->dwBufferBytes, seconds,
			w->wFormatTag, w->nSamplesPerSec, w->nChannels, w->wBitsPerSample, w->nAvgBytesPerSec,
			(d->dwFlags & DSBCAPS_CTRLVOLUME) ? "yes" : "NO",
			d->dwFlags, flags, d->dwSize, hr);
	} else {
		// The primary buffer is created without a format; the game then sets one on it.
		ProxyLog(false,
			"%s #%u: bytes=%lu (no format -- primary buffer) CTRLVOLUME=%s flags=0x%08lX [%s] "
			"descSize=%lu hr=0x%08lX",
			what, index, d->dwBufferBytes,
			(d->dwFlags & DSBCAPS_CTRLVOLUME) ? "yes" : "NO",
			d->dwFlags, flags, d->dwSize, hr);
	}
}

// ---------------------------------------------------------------------------------------------
// The wrapper

namespace {

class DirectSoundProxy : public IDirectSound {
public:
	explicit DirectSoundProxy(IDirectSound *real) : m_real(real), m_refs(1), m_buffers(0) {}

	// --- IUnknown ---
	STDMETHOD(QueryInterface)(REFIID riid, LPVOID *ppv) override
	{
		if (!ppv)
			return E_POINTER;

		// Asked for what we are: hand back the wrapper, or the game would keep a raw pointer and
		// bypass the census entirely.
		if (SameGuid(riid, kIID_IDirectSound) || SameGuid(riid, kIID_IUnknown)) {
			AddRef();
			*ppv = static_cast<IDirectSound *>(this);
			return S_OK;
		}

		// Anything else (IDirectSound8, a private interface) is forwarded untouched. The client is
		// not known to do this -- it only ever calls DirectSoundCreate -- and passing the real
		// object through is safer than pretending to implement something we do not.
		HRESULT hr = m_real->QueryInterface(riid, ppv);
		ProxyLog(false, "QueryInterface for an interface we do not wrap -> hr=0x%08lX "
			"(census will miss buffers created through it)", hr);
		return hr;
	}

	STDMETHOD_(ULONG, AddRef)() override
	{
		return (ULONG)InterlockedIncrement(&m_refs);
	}

	STDMETHOD_(ULONG, Release)() override
	{
		LONG n = InterlockedDecrement(&m_refs);
		if (n == 0) {
			ProxyLog(false, "IDirectSound released; %u buffers created this session", m_buffers);
			m_real->Release();
			delete this;
			return 0;
		}
		return (ULONG)n;
	}

	// --- IDirectSound ---
	STDMETHOD(CreateSoundBuffer)(LPCDSBUFFERDESC desc, LPDIRECTSOUNDBUFFER *ppBuffer,
		LPUNKNOWN pUnkOuter) override
	{
		// The descriptor is still passed through byte for byte. The census proved the game already
		// requests DSBCAPS_CTRLVOLUME on every buffer, so there is nothing we need to add to it --
		// which keeps this the observation-only path it was in phase 2, plus a wrapper on the way
		// out.
		HRESULT hr = m_real->CreateSoundBuffer(desc, ppBuffer, pUnkOuter);
		unsigned index = (unsigned)InterlockedIncrement(&m_buffers);
		LogBufferDesc("CreateSoundBuffer", desc, hr, index);

		if (SUCCEEDED(hr) && ppBuffer && *ppBuffer && desc) {
			// The primary buffer is the mixer's own output, not a sound: scaling it would apply
			// the setting a second time on top of every already-scaled buffer. It is also the one
			// buffer the game creates WITHOUT CTRLVOLUME, so SetVolume on it would fail anyway.
			if (!(desc->dwFlags & DSBCAPS_PRIMARYBUFFER)) {
				bool isMusic = (desc->dwFlags & DSBCAPS_GETCURRENTPOSITION2) != 0;
				*ppBuffer = WrapSoundBuffer(*ppBuffer, isMusic);
			}
		}
		return hr;
	}

	STDMETHOD(GetCaps)(LPDSCAPS caps) override { return m_real->GetCaps(caps); }

	STDMETHOD(DuplicateSoundBuffer)(LPDIRECTSOUNDBUFFER original,
		LPDIRECTSOUNDBUFFER *duplicate) override
	{
		// The census recorded no calls to this in a 26-minute session, so this path is written for
		// correctness rather than because it is known to run. `original` will be one of OUR
		// wrappers, and handing that to the real DirectSound would be a bug in its own right --
		// the real implementation expects its own object, not a stand-in. Unwrap first, and carry
		// the original's music/effect classification onto the duplicate, which needs it: a
		// duplicate shares the source's memory but carries its own independent volume.
		IDirectSoundBuffer *realOriginal = original;
		bool isMusic = false;
		bool wasOurs = UnwrapSoundBuffer(original, &realOriginal, &isMusic);

		HRESULT hr = m_real->DuplicateSoundBuffer(realOriginal, duplicate);

		if (SUCCEEDED(hr) && duplicate && *duplicate && wasOurs)
			*duplicate = WrapSoundBuffer(*duplicate, isMusic);

		ProxyLog(!wasOurs && SUCCEEDED(hr),
			"DuplicateSoundBuffer -> hr=0x%08lX (source %s ours, %s)",
			hr, wasOurs ? "was" : "was NOT",
			wasOurs ? (isMusic ? "music" : "effect") : "duplicate plays unscaled");
		return hr;
	}

	STDMETHOD(SetCooperativeLevel)(HWND hwnd, DWORD level) override
	{
		if (CensusEnabled())
			ProxyLog(false, "SetCooperativeLevel(hwnd=0x%p, level=%lu)", hwnd, level);
		return m_real->SetCooperativeLevel(hwnd, level);
	}

	STDMETHOD(Compact)() override { return m_real->Compact(); }
	STDMETHOD(GetSpeakerConfig)(LPDWORD config) override { return m_real->GetSpeakerConfig(config); }
	STDMETHOD(SetSpeakerConfig)(DWORD config) override { return m_real->SetSpeakerConfig(config); }
	STDMETHOD(Initialize)(LPCGUID device) override { return m_real->Initialize(device); }

private:
	IDirectSound *m_real;
	LONG m_refs;
	LONG m_buffers;
};

}  // namespace

IDirectSound *WrapDirectSound(IDirectSound *real)
{
	if (!real)
		return real;

	DirectSoundProxy *p = new (std::nothrow) DirectSoundProxy(real);
	if (!p) {
		// Out of memory this early means the process is doomed anyway, but degrade to the real
		// object rather than returning null: the game keeps working, it just is not observed.
		ProxyLog(true, "could not allocate the IDirectSound wrapper; census disabled this session");
		return real;
	}
	return p;
}
