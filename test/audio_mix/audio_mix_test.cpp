// The core's own audio arithmetic between AX and the frontend
// (src/audio/AudioMix.h):
//  - 5.1 to stereo: every sample the exact mix rounded to the nearest
//    value, not cut toward zero, and clamped;
//  - the GamePad onto the TV: each group of the TV's frames goes out with
//    the GamePad's group for the same frames, fed in AX's order (the TV's
//    frame, then the GamePad's), so a sound played on both comes out once,
//    twice as loud - not with a copy of itself a group late; a mono TV takes
//    the GamePad's average; a TV group whose GamePad group never comes goes
//    out as it is.
#include "audio/AudioMix.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

static int s_failures = 0;
#define CHECK(cond) \
	do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); s_failures++; } } while (0)

static void TestDownmix()
{
	std::mt19937 rng(7);
	double worst = 0, towardZero = 0;
	size_t counted = 0;
	bool clamped = true;
	for (int i = 0; i < 1000000; i++)
	{
		int16_t in[6], out[2];
		// quiet sound mostly, where the error matters; loud now and then
		const int range = (i % 8) ? 2000 : 32768;
		for (int16_t& s : in)
			s = (int16_t)std::max(-32768, std::min(32767, (int)(rng() % (2 * range)) - range));
		AudioMix::Downmix51(in, out);
		for (int c = 0; c < 2; c++)
		{
			const double exact = in[c] + (in[2] + in[4 + c]) * std::sqrt(0.5);
			if (exact >= 32767.0 || exact <= -32768.0)
			{
				clamped = clamped && out[c] == (exact > 0 ? 32767 : -32768);
				continue;
			}
			const double err = out[c] - exact;
			worst = std::max(worst, std::fabs(err));
			if (exact != 0)
			{
				towardZero += exact > 0 ? -err : err;
				counted++;
			}
		}
	}
	// rounded: half a step at most, and no lean toward silence
	CHECK(worst <= 0.5 + 1e-3);
	CHECK(std::fabs(towardZero / counted) < 0.01);
	CHECK(clamped);
	int16_t loud[6] = {32767, 32767, 32767, 32767, 32767, 32767}, out[2];
	AudioMix::Downmix51(loud, out);
	CHECK(out[0] == 32767 && out[1] == 32767);
	int16_t lfe[6] = {0, 0, 0, 30000, 0, 0};
	AudioMix::Downmix51(lfe, out);
	CHECK(out[0] == 0 && out[1] == 0);
}

// A sound, as AX's frames carry it: sample n of the stream
static int16_t Sound(size_t n)
{
	return (int16_t)(8000.0 * std::sin(n * 0.0731) + 3000.0 * std::sin(n * 0.29));
}

static void TestTvPad(unsigned channels)
{
	constexpr size_t kGroup = 4 * 144;
	AudioMix::TvPadMixer mixer;
	std::vector<int16_t> fed;
	auto feed = [&](int16_t* block) { fed.insert(fed.end(), block, block + kGroup * channels); };
	std::vector<int16_t> tv(kGroup * channels), pad(kGroup * 2);
	const size_t groups = 20;
	for (size_t g = 0; g < groups; g++)
	{
		for (size_t f = 0; f < kGroup; f++)
		{
			const int16_t s = Sound(g * kGroup + f);
			for (unsigned c = 0; c < channels; c++)
				tv[f * channels + c] = c < 2 ? s : 0;
			pad[f * 2] = pad[f * 2 + 1] = s;
		}
		mixer.TvGroup(tv.data(), kGroup, channels, feed);
		mixer.PadGroup(pad.data(), kGroup, feed);
	}
	CHECK(fed.size() == groups * kGroup * channels);
	bool once = fed.size() == groups * kGroup * channels;
	for (size_t n = 0; once && n < groups * kGroup; n++)
	{
		const int s = Sound(n);
		for (unsigned c = 0; c < channels; c++)
			once = once && fed[n * channels + c] == (c < 2 ? std::max(-32768, std::min(32767, 2 * s)) : 0);
	}
	CHECK(once);

	// a GamePad group that never comes: the TV's group goes as it is
	fed.clear();
	mixer.TvGroup(tv.data(), kGroup, channels, feed);
	CHECK(fed.empty());
	mixer.TvGroup(tv.data(), kGroup, channels, feed);
	CHECK(fed.size() == kGroup * channels && std::equal(fed.begin(), fed.end(), tv.begin()));
}

int main()
{
	TestDownmix();
	TestTvPad(1);
	TestTvPad(2);
	TestTvPad(6);
	if (s_failures)
	{
		std::printf("audio_mix_test: %d failure(s)\n", s_failures);
		return 1;
	}
	std::printf("audio_mix_test: all passed\n");
	return 0;
}
