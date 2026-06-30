/*==========================================================================
  xbox_audio_directsound.cpp  --  Hardware DirectSound audio backend for the
  Flycast Xbox port.

  Implements Flycast's AudioBackend interface (audio/audiostream.h) on top of
  the original Xbox's native DirectSound. Flycast's AICA DSP renders Dreamcast
  audio and calls push() with SAMPLE_COUNT (512) stereo s16 frames at 44100 Hz;
  we hand those frames to a looping DirectSound buffer.

  Why not reuse core/audio/audiobackend_directsound.cpp?
    The desktop backend targets PC DirectSound8: it needs an HWND
    (SetCooperativeLevel) and a separate IDirectSoundNotify interface with
    position-notification events. Neither exists on Xbox -- no windowing,
    DirectSoundCreate takes no "8", IDirectSoundNotify is folded into the
    buffer. So we use the proven Xbox streaming pattern instead (matches
    Super-Mario-World-X / Zelda-A-Link-To-The-Past-X xbox_audio.cpp):
    one looping secondary buffer + a feeder thread that polls the play cursor
    and refills the region the hardware has consumed.

  Threading / pacing:
    - push()  : AICA/emu side (main thread). Writes frames into a lock-free
                SPSC FIFO. Blocks on s_pushWait when the FIFO is full IF the
                caller asked to wait (config::LimitFPS) -- this push-block is
                what frame-limits the emulator to realtime.
    - feeder  : polls IDirectSoundBuffer::GetCurrentPosition and refills the
                played-past region with whatever the FIFO holds, padding only
                the genuine shortfall with silence (see FEEDING below).

  FEEDING (why sub-block, not whole-block):
    The hardware play cursor advances continuously; each feeder pass we refill
    *exactly* the bytes it freed (frame-aligned), reading as much real audio as
    the FIFO currently has and zero-filling only the true remainder. Feeding in
    rigid 512-sample blocks instead would inject a whole block of silence
    whenever a block isn't 100% ready -- turning small emulator jitter into
    audible chunky dropouts. Sub-block feeding keeps glitches proportional to
    the actual underrun.
==========================================================================*/

#include "audio/audiostream.h"   // AudioBackend, SAMPLE_COUNT
#include "cfg/option.h"          // config::AudioBufferSize

#include <xtl.h>
#ifdef min
#  undef min
#endif
#ifdef max
#  undef max
#endif
#include <dsound.h>

// Dreamcast audio is stereo 16-bit @ 44100 Hz. Flycast always calls push()
// with exactly SAMPLE_COUNT frames.
static const DWORD AUDIO_SAMPLE_RATE = 44100;
static const DWORD AUDIO_CHANNELS    = 2;
static const DWORD AUDIO_BITS        = 16;
static const DWORD FRAME_BYTES       = AUDIO_CHANNELS * (AUDIO_BITS / 8); // 4
static const DWORD BLOCK_BYTES       = SAMPLE_COUNT * FRAME_BYTES;        // 2048

// The hardware ring DirectSound loops over. 8 blocks ~= 93 ms of cushion --
// enough headroom to ride out emulator frame-time jitter on xemu/hardware
// without the play cursor outrunning the feeder. (The emulator self-paces to
// this via the push-block frame limiter, so a larger buffer just means more
// jitter tolerance, not added latency drift.)
static const DWORD DS_BLOCKS    = 8;
static const DWORD DS_BUF_BYTES = DS_BLOCKS * BLOCK_BYTES;                // 16384

// Largest slice we Lock() at once while refilling (keeps each lock bounded).
static const DWORD FEED_CHUNK   = BLOCK_BYTES;

// Dynamic Rate Control bounds. The play rate is allowed to bend this far from
// nominal to match the emulator's actual speed. +-12% covers the observed
// 88-100% range; the worst fault-storm dips (sub-70%) still glitch briefly, but
// normal play stays smooth. 0.88x is ~2.2 semitones flat at the extreme --
// barely noticeable and far preferable to constant crackle.
static const double DRC_MIN_RATIO = 0.88;
static const double DRC_MAX_RATIO = 1.06;   // emu rarely exceeds realtime; cap chipmunking
static const double DRC_GAIN      = 0.20;   // proportional response to fill error
static const double DRC_SMOOTH    = 0.15;   // low-pass on freq changes (anti-wobble)

//--------------------------------------------------------------------------
// Lock-free single-producer / single-consumer byte FIFO.
//   producer = push() (emu thread), consumer = feeder thread.
// x86 has acquire/release ordering for aligned word loads/stores, and there is
// exactly one reader and one writer, so plain volatile cursors are safe here
// (same approach as the reference ports' ring buffers).
//--------------------------------------------------------------------------
class ByteFifo
{
	std::vector<u8> buf;
	volatile LONG   wpos = 0;   // byte write offset [0, size)
	volatile LONG   rpos = 0;   // byte read  offset [0, size)

	u32 cap() const { return (u32)buf.size(); }

public:
	void init(u32 bytes)
	{
		buf.assign(bytes, 0);
		wpos = 0;
		rpos = 0;
	}

	u32 readable() const
	{
		LONG w = wpos, r = rpos;
		return (u32)((w - r + (LONG)cap()) % (LONG)cap());
	}

	u32 writable() const { return cap() - 1 - readable(); }
	u32 capacity() const { return cap(); }

	// Returns false (writes nothing) if it won't all fit.
	bool write(const u8 *data, u32 n)
	{
		if (n > writable())
			return false;
		LONG w = wpos;
		u32 first = (n < cap() - (u32)w) ? n : cap() - (u32)w;
		memcpy(&buf[w], data, first);
		if (n > first)
			memcpy(&buf[0], data + first, n - first);
		wpos = (LONG)(((u32)w + n) % cap());
		return true;
	}

	// Reads up to n bytes (frame-aligned); returns bytes actually read.
	u32 read(u8 *data, u32 n)
	{
		u32 avail = readable();
		if (n > avail)
			n = avail;
		n &= ~(FRAME_BYTES - 1);   // never split a stereo frame
		if (n == 0)
			return 0;
		LONG r = rpos;
		u32 first = (n < cap() - (u32)r) ? n : cap() - (u32)r;
		memcpy(data, &buf[r], first);
		if (n > first)
			memcpy(data + first, &buf[0], n - first);
		rpos = (LONG)(((u32)r + n) % cap());
		return n;
	}
};

class XboxDirectSoundBackend : public AudioBackend
{
	LPDIRECTSOUND       dsound      = nullptr;
	LPDIRECTSOUNDBUFFER streamBuf   = nullptr;
	DWORD               dsWritePos  = 0;   // our running write cursor into streamBuf

	HANDLE              feederThread  = nullptr;
	volatile bool       feederRunning = false;
	HANDLE              pushWait      = nullptr;  // signalled when FIFO space frees

	ByteFifo            fifo;                     // push() -> feeder hand-off

	// --- Dynamic Rate Control (DRC) ------------------------------------------
	// The emulator can't quite hold 1x realtime (it runs ~82-98% and dips on JIT
	// fault storms), so the AICA produces audio slightly slower than DirectSound
	// drains it -> chronic underrun = crackle, regardless of buffer size. DRC
	// fixes this the way emulators do: instead of fighting the rate mismatch, we
	// retune the DirectSound playback frequency to TRACK the actual production
	// rate, holding the FIFO near half-full. Cost is a small, smooth pitch shift
	// (proportional to how far below realtime the core is) instead of crackle.
	double drcFreq      = (double)AUDIO_SAMPLE_RATE;  // current smoothed play rate
	int    drcTick      = 0;
	u32    fifoCapBytes = 0;

	static DWORD WINAPI feederTrampoline(LPVOID self)
	{
		((XboxDirectSoundBackend *)self)->feederMain();
		return 0;
	}

	// Refill everything the hardware has played past, frame-aligned: real audio
	// where the FIFO has it, silence only for the genuine shortfall.
	void feederMain()
	{
		BYTE scratch[FEED_CHUNK];
		while (feederRunning)
		{
			DWORD playCursor = 0, writeCursorUnused = 0;
			if (FAILED(streamBuf->GetCurrentPosition(&playCursor, &writeCursorUnused)))
			{
				Sleep(4);
				continue;
			}

			// Bytes freed since our last write position (= what to refill now).
			DWORD avail = (playCursor >= dsWritePos)
				? (playCursor - dsWritePos)
				: (DS_BUF_BYTES - dsWritePos + playCursor);
			avail &= ~(FRAME_BYTES - 1);

			bool drainedSome = false;
			while (avail > 0)
			{
				DWORD chunk = (avail < FEED_CHUNK) ? avail : FEED_CHUNK;

				u32 got = fifo.read(scratch, chunk);
				if (got > 0)
					drainedSome = true;
				if (got < chunk)
					memset(scratch + got, 0, chunk - got);   // pad real shortfall only

				void *p1 = nullptr, *p2 = nullptr;
				DWORD s1 = 0, s2 = 0;
				if (FAILED(streamBuf->Lock(dsWritePos, chunk, &p1, &s1, &p2, &s2, 0)))
					break;
				if (p1 && s1) memcpy(p1, scratch, s1);
				if (p2 && s2) memcpy(p2, scratch + s1, s2);
				streamBuf->Unlock(p1, s1, p2, s2);

				dsWritePos = (dsWritePos + chunk) % DS_BUF_BYTES;
				avail -= chunk;
			}

			if (drainedSome && pushWait)
				SetEvent(pushWait);   // wake a blocked push()

			// DRC: every ~32 ms, nudge the play rate toward what keeps the FIFO
			// half full. FIFO draining (emu slow) -> slow playback; filling (emu
			// fast) -> speed up. Smoothed + clamped so pitch glides, never jumps.
			if (++drcTick >= 8 && fifoCapBytes > 0)
			{
				drcTick = 0;
				double fillFrac = (double)fifo.readable() / (double)fifoCapBytes; // 0..1
				double ratio    = 1.0 + DRC_GAIN * (fillFrac - 0.5) * 2.0;
				if (ratio < DRC_MIN_RATIO) ratio = DRC_MIN_RATIO;
				if (ratio > DRC_MAX_RATIO) ratio = DRC_MAX_RATIO;
				double target = (double)AUDIO_SAMPLE_RATE * ratio;
				drcFreq += (target - drcFreq) * DRC_SMOOTH;
				streamBuf->SetFrequency((DWORD)(drcFreq + 0.5));
			}

			Sleep(4);
		}
	}

public:
	XboxDirectSoundBackend()
		: AudioBackend("directsound", "Xbox DirectSound") {}

	bool init() override
	{
		if (FAILED(DirectSoundCreate(NULL, &dsound, NULL)) || dsound == nullptr)
		{
			ERROR_LOG(AUDIO, "Xbox DirectSound: DirectSoundCreate failed");
			return false;
		}

		WAVEFORMATEX wfx;
		memset(&wfx, 0, sizeof(wfx));
		wfx.wFormatTag      = WAVE_FORMAT_PCM;
		wfx.nChannels       = (WORD)AUDIO_CHANNELS;
		wfx.nSamplesPerSec  = AUDIO_SAMPLE_RATE;
		wfx.wBitsPerSample  = (WORD)AUDIO_BITS;
		wfx.nBlockAlign     = (WORD)FRAME_BYTES;
		wfx.nAvgBytesPerSec = AUDIO_SAMPLE_RATE * FRAME_BYTES;

		DSBUFFERDESC desc;
		memset(&desc, 0, sizeof(desc));
		desc.dwSize        = sizeof(desc);
		desc.dwFlags       = DSBCAPS_CTRLVOLUME;
		desc.dwBufferBytes = DS_BUF_BYTES;
		desc.lpwfxFormat   = &wfx;

		if (FAILED(dsound->CreateSoundBuffer(&desc, &streamBuf, NULL)) || streamBuf == nullptr)
		{
			ERROR_LOG(AUDIO, "Xbox DirectSound: CreateSoundBuffer failed");
			dsound->Release();
			dsound = nullptr;
			return false;
		}

		// Start from silence.
		void *p1 = nullptr, *p2 = nullptr;
		DWORD s1 = 0, s2 = 0;
		if (SUCCEEDED(streamBuf->Lock(0, DS_BUF_BYTES, &p1, &s1, &p2, &s2, 0)))
		{
			if (p1 && s1) memset(p1, 0, s1);
			if (p2 && s2) memset(p2, 0, s2);
			streamBuf->Unlock(p1, s1, p2, s2);
		}

		// Staging FIFO. DRC parks this near half-full as the elastic cushion that
		// rides out fault-storm dips, so size it generously: ~3x the hardware
		// buffer (target backlog ~140 ms) means a ~120 ms JIT spike drains the
		// cushion without fully emptying it.
		u32 fifoBytes = (u32)config::AudioBufferSize * FRAME_BYTES;
		if (fifoBytes < DS_BUF_BYTES * 3)
			fifoBytes = DS_BUF_BYTES * 3;
		fifo.init(fifoBytes);
		fifoCapBytes = fifoBytes;

		dsWritePos = 0;
		drcFreq    = (double)AUDIO_SAMPLE_RATE;
		drcTick    = 0;
		pushWait = CreateEvent(NULL, FALSE, FALSE, NULL);   // auto-reset

		// Feeder up before play so the cursor never outruns us.
		feederRunning = true;
		feederThread = CreateThread(NULL, 0, feederTrampoline, this, 0, NULL);
		if (feederThread == nullptr)
		{
			ERROR_LOG(AUDIO, "Xbox DirectSound: feeder CreateThread failed");
			feederRunning = false;
			term();
			return false;
		}

		streamBuf->SetVolume(DSBVOLUME_MAX);   // no DS-side attenuation
		if (FAILED(streamBuf->Play(0, 0, DSBPLAY_LOOPING)))
		{
			ERROR_LOG(AUDIO, "Xbox DirectSound: Play failed");
			term();
			return false;
		}

		INFO_LOG(AUDIO, "Xbox DirectSound started (%lu Hz, %lu-byte hw buf, %lu-byte fifo)",
			(unsigned long)AUDIO_SAMPLE_RATE, (unsigned long)DS_BUF_BYTES, (unsigned long)fifoBytes);
		return true;
	}

	u32 push(const void *frame, u32 frames, bool wait) override
	{
		const u32 bytes = frames * FRAME_BYTES;
		// Block until the feeder frees space (LimitFPS path -> this is the frame
		// limiter). Time out so a stalled feeder can never deadlock the emulator;
		// if not waiting, drop the overflow rather than stall the AICA.
		while (!fifo.write((const u8 *)frame, bytes) && wait)
		{
			if (pushWait)
				WaitForSingleObject(pushWait, 100);
			else
				break;
		}
		return 1;
	}

	void term() override
	{
		feederRunning = false;
		if (feederThread)
		{
			WaitForSingleObject(feederThread, 2000);
			CloseHandle(feederThread);
			feederThread = nullptr;
		}
		if (streamBuf)
		{
			streamBuf->Stop();
			streamBuf->Release();
			streamBuf = nullptr;
		}
		if (dsound)
		{
			dsound->Release();
			dsound = nullptr;
		}
		if (pushWait)
		{
			CloseHandle(pushWait);
			pushWait = nullptr;
		}
		INFO_LOG(AUDIO, "Xbox DirectSound stopped");
	}
};

// Registered with AudioBackend at static-init time (ctor -> registerAudioBackend).
// Slug "directsound" sorts before "null", so "auto" selection picks it.
static XboxDirectSoundBackend xboxDirectSoundBackend;
