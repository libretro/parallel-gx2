#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

// The core's own sample arithmetic between AX's output and the frontend's,
// where a result is not a whole sample value: rounded to the nearest one,
// where a cast cuts toward zero - every sample pulled up to a whole step
// toward silence, half a step on average, a distortion around zero that
// grows as the sound gets quieter - and clamped to the 16-bit range.
namespace AudioMix
{
	inline int16_t RoundClamp(double v)
	{
		if (v >= 32767.0)
			return 32767;
		if (v <= -32768.0)
			return -32768;
		return (int16_t)std::lrint(v);
	}

	// 5.1 (FL FR C LFE SL SR) to stereo: centre and surrounds at -3 dB, the
	// LFE left out
	inline void Downmix51(const int16_t* in, int16_t* out)
	{
		const double k = 0.70710678118654752; // -3 dB
		const double c = in[2] * k;
		out[0] = RoundClamp(in[0] + c + in[4] * k);
		out[1] = RoundClamp(in[1] + c + in[5] * k);
	}

	// The GamePad's sound onto the TV's, as the frontend has one output: a
	// group of the TV's frames is held until the GamePad's group for the same
	// frames is complete, which AX makes right after it, and goes out with it
	// mixed in. Mixed with the group before it instead, the GamePad came 12 ms
	// late, and a game playing the same sound on both - many do - was heard
	// with a copy of itself 12 ms behind: a comb filter, every 83 Hz a notch,
	// the sound thin and hollow.
	class TvPadMixer
	{
	public:
		static constexpr size_t kMaxSamples = 4 * 144 * 6; // a group, 5.1

		// The TV's group, @channels interleaved. One still waiting for its
		// GamePad group, which never came, goes to @feed as it is first.
		template<typename TFeed>
		void TvGroup(const int16_t* tv, size_t frames, unsigned channels, TFeed&& feed)
		{
			if (m_waiting)
				feed(m_tv);
			if (frames * channels > kMaxSamples)
				return;
			memcpy(m_tv, tv, frames * channels * sizeof(int16_t));
			m_frames = frames;
			m_channels = channels;
			m_waiting = true;
		}

		// The GamePad's group for the same frames, stereo: onto the TV's front
		// pair (both on a mono TV), which then goes to @feed
		template<typename TFeed>
		void PadGroup(const int16_t* pad, size_t frames, TFeed&& feed)
		{
			if (!m_waiting)
				return; // no TV group to go with
			const size_t n = frames < m_frames ? frames : m_frames;
			for (size_t f = 0; f < n; f++)
			{
				int16_t* tv = m_tv + f * m_channels;
				const int l = pad[f * 2], r = pad[f * 2 + 1];
				if (m_channels == 1)
					MixInto(tv[0], (l + r) / 2);
				else
				{
					MixInto(tv[0], l);
					MixInto(tv[1], r);
				}
			}
			m_waiting = false;
			feed(m_tv);
		}

	private:
		static void MixInto(int16_t& out, int add)
		{
			const int v = (int)out + add;
			out = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
		}

		int16_t m_tv[kMaxSamples]{};
		size_t m_frames{0};
		unsigned m_channels{2};
		bool m_waiting{false};
	};
}
