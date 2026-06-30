/*==========================================================================
  xbox_audio_directsound.cpp  --  Hardware DirectSound audio backend for the
  Flycast Xbox port.  Clean rewrite: fixed-rate, no pitch/rate control.

  Implements Flycast's AudioBackend interface (audio/audiostream.h) on top of
  the original Xbox's native DirectSound. The AICA DSP renders Dreamcast audio
  and calls push() with SAMPLE_COUNT (512) stereo s16 frames @ 44100 Hz.

  DESIGN -- audio is the master clock, fixed 44.1 kHz, NO rate control:
    A looping DirectSound buffer plays at a hard 44100 Hz (the hardware audio
    crystal). A feeder thread keeps it filled from a lock-free FIFO. push()
    writes into the FIFO and BLOCKS when it's full -- so the emulator is paced
    to exactly the rate DirectSound drains, i.e. true realtime, whenever the
    SH-4 can keep up. There is deliberately no Dynamic Rate Control and no
    SetFrequency(): the playback rate never changes, so the pitch is always
    correct. The trade is that if the emulator can't sustain realtime, the
    buffer underruns (a brief silence/glitch) instead of the audio being
    pitch-bent to hide it. That keeps pitch rock-steady and makes any CPU
    shortfall audible as a dropout exactly where it happens -- the honest
    behaviour, and the diagnostic we want.

  Because audio is now the sole pacing clock, the render loop in main_xbox.cpp
  no longer runs its own wall-clock frame limiter.
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

// Dreamcast audio: stereo 16-bit @ 44100 Hz. push() is always SAMPLE_COUNT frames.
static const DWORD AUDIO_SAMPLE_RATE = 44100;
static const DWORD AUDIO_CHANNELS    = 2;
static const DWORD AUDIO_BITS        = 16;
static const DWORD FRAME_BYTES       = AUDIO_CHANNELS * (AUDIO_BITS / 8); // 4
static const DWORD BLOCK_BYTES       = SAMPLE_COUNT * FRAME_BYTES;        // 2048 (~11.6 ms)

// Looping hardware buffer DirectSound plays through. 4 blocks ~= 46 ms: enough
// to ride feeder-thread scheduling jitter, small enough for low latency.
static const DWORD DS_BLOCKS    = 4;
static const DWORD DS_BUF_BYTES = DS_BLOCKS * BLOCK_BYTES;                // 8192

// Largest slice we Lock() at once while refilling.
static const DWORD FEED_CHUNK   = BLOCK_BYTES;

//--------------------------------------------------------------------------
// Lock-free single-producer / single-consumer byte FIFO.
//   producer = push() (emu thread); consumer = feeder thread.
// One reader, one writer, aligned word cursors on x86 -> plain volatile is safe.
//--------------------------------------------------------------------------
class ByteFifo
{
	std::vector<u8> buf;
	volatile LONG   wpos = 0;
	volatile LONG   rpos = 0;

	u32 cap() const { return (u32)buf.size(); }

public:
	void init(u32 bytes) { buf.assign(bytes, 0); wpos = 0; rpos = 0; }

	u32 readable() const
	{
		LONG w = wpos, r = rpos;
		return (u32)((w - r + (LONG)cap()) % (LONG)cap());
	}
	u32 writable() const { return cap() - 1 - readable(); }

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

	// Read up to n bytes (frame-aligned); returns bytes actually read.
	u32 read(u8 *data, u32 n)
	{
		u32 avail = readable();
		if (n > avail)
			n = avail;
		n &= ~(FRAME_BYTES - 1);
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
	LPDIRECTSOUND       dsound     = nullptr;
	LPDIRECTSOUNDBUFFER buffer     = nullptr;
	DWORD               writePos   = 0;   // our running write cursor into the buffer

	HANDLE              feederThread  = nullptr;
	volatile bool       feederRunning = false;
	HANDLE              roomEvent     = nullptr;   // signalled when FIFO drains

	ByteFifo            fifo;

	static DWORD WINAPI feederTrampoline(LPVOID self)
	{
		((XboxDirectSoundBackend *)self)->feederMain();
		return 0;
	}

	// Refill whatever the hardware has played past: real audio from the FIFO,
	// silence for any shortfall (so an underrun is a clean gap, never a replay
	// of stale buffer contents). Fixed rate -- no SetFrequency anywhere.
	void feederMain()
	{
		BYTE scratch[FEED_CHUNK];
		while (feederRunning)
		{
			DWORD play = 0, write = 0;
			if (FAILED(buffer->GetCurrentPosition(&play, &write)))
			{
				Sleep(2);
				continue;
			}

			DWORD avail = (play >= writePos) ? (play - writePos)
			                                 : (DS_BUF_BYTES - writePos + play);
			avail &= ~(FRAME_BYTES - 1);

			bool drained = false;
			while (avail > 0)
			{
				DWORD chunk = (avail < FEED_CHUNK) ? avail : FEED_CHUNK;

				u32 got = fifo.read(scratch, chunk);
				if (got > 0)
					drained = true;
				if (got < chunk)
					memset(scratch + got, 0, chunk - got);

				void *p1 = nullptr, *p2 = nullptr;
				DWORD s1 = 0, s2 = 0;
				if (FAILED(buffer->Lock(writePos, chunk, &p1, &s1, &p2, &s2, 0)))
					break;
				if (p1 && s1) memcpy(p1, scratch, s1);
				if (p2 && s2) memcpy(p2, scratch + s1, s2);
				buffer->Unlock(p1, s1, p2, s2);

				writePos = (writePos + chunk) % DS_BUF_BYTES;
				avail -= chunk;
			}

			if (drained && roomEvent)
				SetEvent(roomEvent);   // wake a blocked push()

			Sleep(2);
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

		if (FAILED(dsound->CreateSoundBuffer(&desc, &buffer, NULL)) || buffer == nullptr)
		{
			ERROR_LOG(AUDIO, "Xbox DirectSound: CreateSoundBuffer failed");
			dsound->Release();
			dsound = nullptr;
			return false;
		}

		// Silence the buffer to start.
		void *p1 = nullptr, *p2 = nullptr;
		DWORD s1 = 0, s2 = 0;
		if (SUCCEEDED(buffer->Lock(0, DS_BUF_BYTES, &p1, &s1, &p2, &s2, 0)))
		{
			if (p1 && s1) memset(p1, 0, s1);
			if (p2 && s2) memset(p2, 0, s2);
			buffer->Unlock(p1, s1, p2, s2);
		}

		// FIFO for push() -> feeder. Sized so push blocks promptly (tight pacing)
		// but with a little give for scheduling jitter: ~4 blocks (~46 ms).
		u32 fifoBytes = DS_BUF_BYTES;
		fifo.init(fifoBytes);

		writePos  = 0;
		roomEvent = CreateEvent(NULL, FALSE, FALSE, NULL);   // auto-reset

		feederRunning = true;
		feederThread  = CreateThread(NULL, 0, feederTrampoline, this, 0, NULL);
		if (feederThread == nullptr)
		{
			ERROR_LOG(AUDIO, "Xbox DirectSound: feeder CreateThread failed");
			feederRunning = false;
			term();
			return false;
		}

		buffer->SetVolume(DSBVOLUME_MAX);
		if (FAILED(buffer->Play(0, 0, DSBPLAY_LOOPING)))
		{
			ERROR_LOG(AUDIO, "Xbox DirectSound: Play failed");
			term();
			return false;
		}

		INFO_LOG(AUDIO, "Xbox DirectSound started: %lu Hz fixed-rate, %lu-byte buffer",
			(unsigned long)AUDIO_SAMPLE_RATE, (unsigned long)DS_BUF_BYTES);
		return true;
	}

	u32 push(const void *frame, u32 frames, bool wait) override
	{
		const u32 bytes = frames * FRAME_BYTES;
		// Block until the feeder frees FIFO space -> this is what paces the
		// emulator to the audio clock (true realtime) when it can keep up.
		// SAFETY: bound the wait to ~500 ms of no progress, then drop rather than
		// hang -- audio can never freeze the emulator.
		int timeouts = 0;
		while (!fifo.write((const u8 *)frame, bytes) && wait)
		{
			if (!roomEvent)
				break;
			if (WaitForSingleObject(roomEvent, 100) == WAIT_OBJECT_0)
				timeouts = 0;
			else if (++timeouts >= 5)
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
		if (buffer)
		{
			buffer->Stop();
			buffer->Release();
			buffer = nullptr;
		}
		if (dsound)
		{
			dsound->Release();
			dsound = nullptr;
		}
		if (roomEvent)
		{
			CloseHandle(roomEvent);
			roomEvent = nullptr;
		}
		INFO_LOG(AUDIO, "Xbox DirectSound stopped");
	}
};

// Registered at static-init (ctor -> registerAudioBackend). Slug "directsound"
// sorts before "null", so "auto" backend selection picks it.
static XboxDirectSoundBackend xboxDirectSoundBackend;
