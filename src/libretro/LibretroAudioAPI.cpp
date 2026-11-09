#include "LibretroAudioAPI.h"
#include "audio/AudioMix.h"

#include <algorithm>
#include <atomic>
#include <chrono>

#include "Cemu/Logging/CemuLogging.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"

// Defined in snd_core/ax_out.cpp: how often the AX gate is reached and how
// often it opens.
namespace snd_core
{
	extern std::atomic<uint64_t> g_ax_update_calls;
	extern std::atomic<uint64_t> g_ax_update_passed;
	uint32 getNumProcessedFrames();
}

LibretroAudioAPI::AudioCallback LibretroAudioAPI::s_audio_callback = nullptr;
bool LibretroAudioAPI::s_log_stats = false;
uint64_t LibretroAudioAPI::s_stat_offered = 0;
uint64_t LibretroAudioAPI::s_stat_written = 0;
uint64_t LibretroAudioAPI::s_stat_read = 0;
uint64_t LibretroAudioAPI::s_stat_sent = 0;
uint64_t LibretroAudioAPI::s_stat_flushes = 0;
uint64_t LibretroAudioAPI::s_stat_empty_flushes = 0;
LibretroAudioRingBuffer LibretroAudioAPI::s_ring_buffer;
std::atomic_bool LibretroAudioAPI::s_thread_mode{false};
LibretroAudioAPI::MultiAudioCallback LibretroAudioAPI::s_multi_callback = nullptr;
unsigned LibretroAudioAPI::s_out_channels = 2;
bool LibretroAudioAPI::s_last_pull_refused = false;

// What RetroArch's audio thread sleeps on between pulls that found nothing
static LibretroAudioFeedWake& GetFeedWake()
{
	static LibretroAudioFeedWake s_wake;
	return s_wake;
}

LibretroAudioAPI::LibretroAudioAPI(uint32 samplerate, uint32 channels, uint32 samples_per_block, uint32 bits_per_sample)
	: IAudioAPI(samplerate, channels, samples_per_block, bits_per_sample)
{
	s_ring_buffer.Reset();
}

LibretroAudioAPI::~LibretroAudioAPI()
{
	m_playing = false;
	s_ring_buffer.Reset();
}

bool LibretroAudioAPI::NeedAdditionalBlocks() const
{
	// Match upstream behavior: ask for more blocks while the ring has less
	// than (audio_delay * samples_per_block) buffered.
	// In the ring's samples, which are the output's channels
	const size_t bufferedSamples = s_ring_buffer.GetReadAvailableSamples();
	const size_t targetBufferSamples = GetAudioDelay() * m_samplesPerBlock * s_out_channels;
	return bufferedSamples < targetBufferSamples;
}

bool LibretroAudioAPI::FeedBlock(sint16* data)
{
	if (!data || !m_playing)
		return false;

	// AX's channels (the TV mode: 1, 2 or 6) to the output's (2 or 6). 5.1
	// is FL FR C LFE SL SR, as standalone hands it to cubeb. Converted
	// straight into the ring, in as many pieces as it lies in there.
	const size_t frames = m_samplesPerBlock;
	const unsigned in = m_channels, out = s_out_channels;
	const size_t sampleCount = frames * out;
	size_t written = 0;
	if (in == out)
		written = s_ring_buffer.Write(data, sampleCount);
	else
	{
		size_t f = 0;
		while (f < frames)
		{
			int16_t* dst;
			size_t n = s_ring_buffer.Reserve(&dst, (frames - f) * out);
			n -= n % out;
			if (n == 0)
				break;
			const size_t pieceFrames = n / out;
			for (size_t i = 0; i < pieceFrames; i++, f++)
			{
				const int16_t* s = data + f * in;
				int16_t* d = dst + i * out;
				if (in == 1)
				{
					for (unsigned c = 0; c < out; c++)
						d[c] = c < 2 ? s[0] : 0;
				}
				else if (in == 6 && out == 2)
				{
					AudioMix::Downmix51(s, d);
				}
				else
				{
					for (unsigned ch = 0; ch < out; ch++)
						d[ch] = ch < in ? s[ch] : 0;
				}
			}
			s_ring_buffer.Commit(n);
			written += n;
		}
	}
	AccountWrite(sampleCount, written);
	if (written > 0 && s_thread_mode.load(std::memory_order_relaxed))
		GetFeedWake().Notify(); // WaitForFeed
	return written > 0;
}

bool LibretroAudioAPI::Play()
{
	m_playing = true;
	return true;
}

bool LibretroAudioAPI::Stop()
{
	m_playing = false;
	return true;
}

void LibretroAudioAPI::SetAudioCallback(AudioCallback cb)
{
	s_audio_callback = cb;
}

void LibretroAudioAPI::SetOutput(unsigned channels, MultiAudioCallback multi)
{
	s_out_channels = channels == 6 && multi ? 6 : 2;
	s_multi_callback = s_out_channels == 6 ? multi : nullptr;
	s_ring_buffer.Reset();
}

void LibretroAudioAPI::FlushAudio(size_t frames)
{
	if (!s_audio_callback)
		return;

	// One frame's worth per retro_run, as other cores hand over. Handing over
	// whatever AX had made by then gave RetroArch a different amount every
	// run - AX makes a run's audio over the whole frame, so how much of it was
	// there at the end of retro_run varied - and its buffer swung between
	// blocking and running dry without ever settling, where rpcs3's or
	// xenia's settle within a few thousand frames (NNshi). What is not handed
	// over stays in the ring for the next run. Only a backlog of more than two
	// frames goes out on top, so the ring cannot grow without bound.
	const size_t channels = s_out_channels;
	const size_t availableSamples = s_ring_buffer.GetReadAvailableSamples();
	const size_t frameSamples = frames * channels;
	if (availableSamples == 0)
	{
		if (s_log_stats)
		{
			s_stat_flushes++;
			s_stat_empty_flushes++;
			ReportStats();
		}
		return;
	}

	size_t wanted = frameSamples;
	if (availableSamples > 3 * frameSamples)
		wanted = availableSamples - 2 * frameSamples;
	wanted -= wanted % channels;
	const size_t samplesToRead = std::min(availableSamples, wanted);
	size_t samplesSent = 0;
	const size_t samplesRead = SendFromRing(samplesToRead, samplesSent);
	if (samplesRead == 0)
		return;
	AccountFlush(samplesRead, samplesSent);
}

// Hands the frontend up to sampleCount buffered samples straight out of the
// ring - no copy on the way - and consumes all of them, as many as it took or
// not: what it refuses is dropped, as it always was. Returns how many were
// taken out of the ring; sent is how many the frontend accepted.
size_t LibretroAudioAPI::SendFromRing(size_t sampleCount, size_t& sent)
{
	// 5.1: FL FR C LFE SL SR, as RetroArch's speaker bits run
	constexpr unsigned k51 = 0x001 | 0x002 | 0x004 | 0x008 | 0x200 | 0x400;
	const size_t channels = s_out_channels;
	size_t taken = 0;
	bool refused = false;
	sent = 0;
	while (taken < sampleCount)
	{
		const int16_t* span;
		const size_t n = s_ring_buffer.Peek(&span, sampleCount - taken);
		if (n == 0)
			break;
		const size_t frames = n / channels;
		size_t framesSent = 0;
		// retro_audio_sample_batch_t may consume fewer frames than offered;
		// loop so a partial accept doesn't lose audio
		while (!refused && framesSent < frames)
		{
			const int16_t* cursor = span + framesSent * channels;
			const size_t accepted = channels == 6
				? s_multi_callback(cursor, frames - framesSent, 6, k51)
				: s_audio_callback(cursor, frames - framesSent);
			if (accepted == 0)
				refused = true;
			framesSent += accepted;
		}
		sent += framesSent * channels;
		s_ring_buffer.Consume(n);
		taken += n;
	}
	return taken;
}

size_t LibretroAudioAPI::PullAudio(size_t maxFrames)
{
	if (!s_audio_callback)
		return 0;
	const size_t channels = s_out_channels;

	// AX makes the audio by the host's clock and RetroArch plays it by the
	// sound card's, which drift apart, so the ring would slowly fill and the
	// sound lag further behind the picture. Past 4096 frames (85 ms) the
	// oldest is dropped.
	constexpr size_t kMaxBufferedFrames = 4096;
	const size_t available = s_ring_buffer.GetReadAvailableSamples();
	if (available > kMaxBufferedFrames * channels)
	{
		size_t excess = available - kMaxBufferedFrames * channels;
		s_ring_buffer.Discard(excess - excess % channels);
	}

	const size_t wanted = std::min<size_t>(maxFrames, 1024) * channels;
	size_t samplesSent = 0;
	const size_t samplesRead = SendFromRing(wanted, samplesSent);
	if (samplesRead)
		AccountFlush(samplesRead, samplesSent);
	s_last_pull_refused = samplesRead != 0 && samplesSent == 0;
	const size_t framesSent = samplesSent / channels;
	return framesSent;
}

void LibretroAudioAPI::DropBuffered()
{
	s_ring_buffer.Discard(s_ring_buffer.GetReadAvailableSamples());
}

void LibretroAudioAPI::WaitForFeed(bool refused, const std::atomic_bool& active, std::chrono::microseconds bound)
{
	// A refused pull ends at the first wake: that is the stop, or more audio
	// the frontend is as likely to refuse - it asks again either way
	GetFeedWake().WaitUntil([&] {
		return !active.load(std::memory_order_relaxed) ||
			(!refused && s_ring_buffer.GetReadAvailableSamples() != 0);
	}, std::chrono::steady_clock::now() + bound, refused);
}

void LibretroAudioAPI::WakeFeedWaiter()
{
	GetFeedWake().Notify();
}

void LibretroAudioAPI::SetStatsLogging(bool enabled)
{
	s_log_stats = enabled;
}

void LibretroAudioAPI::AccountWrite(size_t offered, size_t written)
{
	if (!s_log_stats)
		return;
	s_stat_offered += offered;
	s_stat_written += written;
}

void LibretroAudioAPI::AccountFlush(size_t read, size_t sent)
{
	if (!s_log_stats)
		return;
	s_stat_read += read;
	s_stat_sent += sent;
	s_stat_flushes++;
	ReportStats();
}

void LibretroAudioAPI::ReportStats()
{
	using clock = std::chrono::steady_clock;
	static clock::time_point s_last = clock::now();

	const clock::time_point now = clock::now();
	if (now - s_last < std::chrono::seconds(1))
		return;
	s_last = now;

	// 48 kHz stereo is 96000 samples a second, so "AX produced" against that
	// says whether the title is being given enough time to make audio at all;
	// dropped says the ring overflowed; the frontend line says whether what was
	// drained was actually taken.
	// Where the samples would have to come from: the scheduler reaching its
	// system-event check, that check reaching the AX gate, and the gate opening.
	static uint64_t s_last_fibers[3] = {};
	static uint64_t s_last_idles[3] = {};
	static uint64_t s_last_processed = 0;
	static uint64_t s_last_events = 0;
	static uint64_t s_last_ax_calls = 0;
	static uint64_t s_last_ax_passed = 0;
	const uint64_t events = coreinit::OSSchedulerGetSystemEventCount();
	const uint64_t ax_calls = snd_core::g_ax_update_calls.load(std::memory_order_relaxed);
	const uint64_t ax_passed = snd_core::g_ax_update_passed.load(std::memory_order_relaxed);

	// And what the guest is doing with its time: how many frames its own AX
	// thread finished, and how often each emulated core came round its loop.
	// A gate that opens every time it is reached but produces one block per
	// video frame means the wait is on the guest, not on us.
	const uint64_t processed = snd_core::getNumProcessedFrames();
	uint64_t fibers[3], idles[3];
	for (int i = 0; i < 3; i++)
	{
		fibers[i] = coreinit::OSSchedulerGetPpcFiberLoopCount(i);
		idles[i] = coreinit::OSSchedulerGetIdleLoopCount(i);
	}

	cemuLog_log(LogType::Force,
		"audio: AX produced {} samples, ring dropped {}, drained {}, frontend took {}, "
		"{} flushes ({} with nothing to send), ring holds {}; "
		"scheduler events {}, AX update called {}, gate opened {}, "
		"guest AX frames {}, core loops {}/{}/{}, idle loops {}/{}/{}",
		s_stat_offered, s_stat_offered - s_stat_written, s_stat_read, s_stat_sent,
		s_stat_flushes, s_stat_empty_flushes, s_ring_buffer.GetReadAvailableSamples(),
		events - s_last_events, ax_calls - s_last_ax_calls, ax_passed - s_last_ax_passed,
		processed - s_last_processed,
		fibers[0] - s_last_fibers[0], fibers[1] - s_last_fibers[1], fibers[2] - s_last_fibers[2],
		idles[0] - s_last_idles[0], idles[1] - s_last_idles[1], idles[2] - s_last_idles[2]);

	s_last_processed = processed;
	for (int i = 0; i < 3; i++)
	{
		s_last_fibers[i] = fibers[i];
		s_last_idles[i] = idles[i];
	}

	s_last_events = events;
	s_last_ax_calls = ax_calls;
	s_last_ax_passed = ax_passed;

	s_stat_offered = 0;
	s_stat_written = 0;
	s_stat_read = 0;
	s_stat_sent = 0;
	s_stat_flushes = 0;
	s_stat_empty_flushes = 0;
}

void LibretroAudioAPI::Reset()
{
	s_ring_buffer.Reset();
}
