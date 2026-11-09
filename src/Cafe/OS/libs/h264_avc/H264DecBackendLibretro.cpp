#include "H264DecInternal.h"
#include "H264DecodeEngine.h"
#include "util/helpers/EventWake.h"

#include <libretro.h>

// h264.rpl's decoder over the frontend's H.264 decoder - RetroArch's, on the
// GPU where it can - and over rh264 where the frontend has none. A thread per
// session decodes what the game queues, as the console's decoder runs beside
// the game, so H264DECExecute only waits when the game asks for a picture
// that is not decoded yet.
namespace H264
{
	bool H264_IsBotW();

	static retro_video_decode_interface s_frontendInterface{};
	static std::atomic<bool> s_haveFrontendInterface{false};

	// A decoder the hardware is behind on tries again once a frame, as the
	// interface's callers on the frontend's own thread do: the frontend
	// gives nothing to wait on. retro_run is the frame.
	static EventWake s_retryWake;
	static std::atomic<uint64> s_retryTick{0};
	static std::atomic<int> s_retryWaiters{0};

	void SetFrontendDecoder(const retro_video_decode_interface* iface)
	{
		if (!iface)
		{
			s_haveFrontendInterface.store(false, std::memory_order_release);
			return;
		}
		s_frontendInterface = *iface;
		s_haveFrontendInterface.store(true, std::memory_order_release);
	}

	void FrameTick()
	{
		if (s_retryWaiters.load(std::memory_order_acquire) == 0)
			return;
		s_retryTick.fetch_add(1, std::memory_order_acq_rel);
		s_retryWake.Notify();
	}

	int OpenFrontendDecoders()
	{
		return DecodeEngine::OpenFrontendDecoders();
	}

	class H264LibretroDecoder : public H264DecoderBackend, private EngineSink
	{
	public:
		H264LibretroDecoder()
			: m_engine(s_haveFrontendInterface.load(std::memory_order_acquire) ? &s_frontendInterface : nullptr, H264_IsBotW())
		{
			m_thread = std::thread(&H264LibretroDecoder::DecoderThread, this);
		}

		~H264LibretroDecoder() override
		{
			m_exit.store(true, std::memory_order_release);
			s_retryWake.Notify();
			m_decodeQueue.push(CMD_EXIT);
			if (m_thread.joinable())
				m_thread.join();
		}

		void Init(bool isBufferedMode) override
		{
			m_buffered.store(isBufferedMode, std::memory_order_relaxed);
			m_begun.store(true, std::memory_order_release);
		}

		void Destroy() override {}

	private:
		uint8_t* Destination(uint64_t tag) override
		{
			return tag < m_decodedSliceArray.size() ? (uint8_t*)m_decodedSliceArray[tag].result.imageOutput : nullptr;
		}

		void Output(const EngineOutput& out) override
		{
			if (out.tag >= m_decodedSliceArray.size())
				return;
			DecodeResult& r = m_decodedSliceArray[out.tag].result;
			r.isDecoded = true;
			r.hasFrame = out.hasFrame;
			r.frameWidth = out.frameWidth;
			r.frameHeight = out.frameHeight;
			r.bytesPerRow = out.bytesPerRow;
			r.cropEnable = out.cropEnable;
			r.cropTop = out.cropTop;
			r.cropBottom = out.cropBottom;
			r.cropLeft = out.cropLeft;
			r.cropRight = out.cropRight;
			m_displayQueue.push((uint32)out.tag);
			coreinit::OSSignalEvent(m_displayQueueEvt);
		}

		// Until the next frame, or until the session closes
		bool WaitForRetry()
		{
			const uint64 tick = s_retryTick.load(std::memory_order_acquire);
			s_retryWaiters.fetch_add(1, std::memory_order_acq_rel);
			s_retryWake.WaitUntil([&] { return s_retryTick.load(std::memory_order_acquire) != tick || m_exit.load(std::memory_order_acquire); });
			s_retryWaiters.fetch_sub(1, std::memory_order_acq_rel);
			return !m_exit.load(std::memory_order_acquire);
		}

		void DecoderThread()
		{
			bool begun = false;
			for (;;)
			{
				uint32 index;
				m_decodeQueue.pop(index);
				if (index == CMD_EXIT)
					return;
				if (!begun)
				{
					// H264DECBegin comes before the first input
					const bool initialized = m_begun.load(std::memory_order_acquire);
					m_engine.Begin(!initialized || m_buffered.load(std::memory_order_relaxed));
					begun = true;
					cemuLog_log(LogType::Force, "H264: decoding with {}", m_engine.UsesFrontend() ? "the frontend's decoder" : "rh264");
				}
				if (index == CMD_FLUSH)
				{
					while (m_engine.Flush(*this) == DecodeEngine::BUSY)
						if (!WaitForRetry())
							return;
					coreinit::OSSignalEvent(m_flushEvt);
					continue;
				}
				DataToDecode& in = m_decodedSliceArray[index].dataToDecode;
				while (m_engine.Decode(in.m_data, in.m_length, index, *this) == DecodeEngine::BUSY)
					if (!WaitForRetry())
						return;
			}
		}

		DecodeEngine m_engine;
		std::atomic<bool> m_buffered{true}; // set by Init before the first input is queued
		std::atomic<bool> m_begun{false};
		std::atomic<bool> m_exit{false};
		std::thread m_thread;
	};

	H264DecoderBackend* CreateAVCDecoder()
	{
		return new H264LibretroDecoder();
	}
}
