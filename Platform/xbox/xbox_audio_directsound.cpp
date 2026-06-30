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
    position-notification events. Neither exists on Xbox -- there is no
    windowing, DirectSoundCreate takes no "8", and IDirectSoundNotify is folded
    into the buffer. So we use the proven Xbox streaming pattern instead
    (matches Super-Mario-World-X / Zelda-A-Link-To-The-Past-X xbox_audio.cpp):
    one looping secondary buffer + a feeder thread that polls the play cursor
    and refills the space the hardware has consumed.

  Threading model (mirrors the desktop backend's intent):
    - push()  : AICA/emu side. Writes frames into a lock-free RingBuffer.
                Blocks on s_pushWait when the ring is full (wait == true).
    - feeder  : polls IDirectSoundBuffer::GetCurrentPosition, copies whatever
                the ring holds into the freed region (silence on underrun),
                then signals s_pushWait so push() can proceed.
==========================================================================*/

#include "audio/audiostream.h"   // AudioBackend, RingBuffer, SAMPLE_COUNT
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
// with exactly SAMPLE_COUNT frames, so one "block" is the natural feed unit.
static const DWORD AUDIO_SAMPLE_RATE = 44100;
static const DWORD AUDIO_CHANNELS    = 2;
static const DWORD AUDIO_BITS        = 16;
static const DWORD FRAME_BYTES       = AUDIO_CHANNELS * (AUDIO_BITS / 8); // 4
static const DWORD BLOCK_BYTES       = SAMPLE_COUNT * FRAME_BYTES;        // 2048

// The hardware ring DirectSound loops over. 4 blocks ~= 46 ms of latency,
// large enough to ride out emulator frame-time jitter on xemu/hardware,
// small enough to stay responsive.
static const DWORD DS_BLOCKS    = 4;
static const DWORD DS_BUF_BYTES = DS_BLOCKS * BLOCK_BYTES;                // 8192

class XboxDirectSoundBackend : public AudioBackend
{
	LPDIRECTSOUND       dsound      = nullptr;
	LPDIRECTSOUNDBUFFER streamBuf   = nullptr;
	DWORD               dsWritePos  = 0;   // our running write cursor into streamBuf

	HANDLE              feederThread = nullptr;
	volatile bool       feederRunning = false;
	HANDLE              pushWait     = nullptr;  // signalled when ring space frees

	RingBuffer          ringBuffer;            // push() -> feeder hand-off

	// Feeder thread: drain the ring into the region DirectSound has played past.
	static DWORD WINAPI feederTrampoline(LPVOID self)
	{
		((XboxDirectSoundBackend *)self)->feederMain();
		return 0;
	}

	void feederMain()
	{
		BYTE block[BLOCK_BYTES];
		while (feederRunning)
		{
			DWORD playCursor = 0, writeCursorUnused = 0;
			if (FAILED(streamBuf->GetCurrentPosition(&playCursor, &writeCursorUnused)))
			{
				Sleep(4);
				continue;
			}

			// Bytes the hardware has consumed since our last write position.
			DWORD avail = (playCursor >= dsWritePos)
				? (playCursor - dsWritePos)
				: (DS_BUF_BYTES - dsWritePos + playCursor);

			bool signalled = false;
			while (avail >= BLOCK_BYTES)
			{
				// Pull one block from the ring; on underrun emit silence so the
				// looping buffer never replays stale audio (avoids buzzing).
				if (!ringBuffer.read(block, BLOCK_BYTES))
					memset(block, 0, BLOCK_BYTES);
				else
					signalled = true;

				void *p1 = nullptr, *p2 = nullptr;
				DWORD s1 = 0, s2 = 0;
				if (SUCCEEDED(streamBuf->Lock(dsWritePos, BLOCK_BYTES,
						&p1, &s1, &p2, &s2, 0)))
				{
					if (p1 && s1) memcpy(p1, block, s1);
					if (p2 && s2) memcpy(p2, block + s1, s2);
					streamBuf->Unlock(p1, s1, p2, s2);
					dsWritePos = (dsWritePos + BLOCK_BYTES) % DS_BUF_BYTES;
					avail -= BLOCK_BYTES;
				}
				else
				{
					break;
				}
			}

			// Let a blocked push() know the ring drained a bit.
			if (signalled && pushWait)
				SetEvent(pushWait);

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

		// Ring buffer: follow the desktop backend's sizing (AudioBufferSize
		// frames * FRAME_BYTES), but never smaller than the hardware buffer so
		// the feeder always has room to work with.
		u32 ringBytes = (u32)config::AudioBufferSize * FRAME_BYTES;
		if (ringBytes < DS_BUF_BYTES * 2)
			ringBytes = DS_BUF_BYTES * 2;
		ringBuffer.setCapacity(ringBytes);

		dsWritePos = 0;
		pushWait = CreateEvent(NULL, FALSE, FALSE, NULL);   // auto-reset

		// Spin up the feeder before play so the cursor never outruns us.
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

		INFO_LOG(AUDIO, "Xbox DirectSound playback started (%lu Hz, %lu-byte ring)",
			(unsigned long)AUDIO_SAMPLE_RATE, (unsigned long)ringBytes);
		return true;
	}

	u32 push(const void *frame, u32 frames, bool wait) override
	{
		const u32 bytes = frames * FRAME_BYTES;
		// Block until the feeder makes room (LimitFPS path). Time out so a
		// stalled feeder can never deadlock the emulator.
		while (!ringBuffer.write((const u8 *)frame, bytes) && wait)
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
		INFO_LOG(AUDIO, "Xbox DirectSound playback stopped");
	}
};

// Registered with AudioBackend at static-init time (ctor calls
// registerAudioBackend). Slug "directsound" sorts before "null", so the
// "auto" backend selection picks it over the null driver automatically.
static XboxDirectSoundBackend xboxDirectSoundBackend;
