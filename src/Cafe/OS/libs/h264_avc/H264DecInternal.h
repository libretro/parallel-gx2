#pragma once

#include "util/helpers/IndexFreeList.h"
#include "util/helpers/MpmcQueue.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/OS/libs/coreinit/coreinit_SysHeap.h"

#include "Cafe/OS/libs/h264_avc/parser/H264Parser.h"

namespace H264
{
	class H264DecoderBackend
	{
	  protected:
		struct DataToDecode
		{
			uint8* m_data;
			uint32 m_length;
			std::vector<uint8> m_buffer;
		};

		static constexpr uint32 CMD_FLUSH = 0xFFFFFFFF;
		static constexpr uint32 CMD_EXIT = 0xFFFFFFFE;

	  public:
		struct DecodeResult
		{
			bool isDecoded{false};
			bool hasFrame{false}; // set to true if a full frame was successfully decoded
			double timestamp{};
			void* imageOutput{nullptr};
			sint32 frameWidth{0};
			sint32 frameHeight{0};
			uint32 bytesPerRow{0};
			bool cropEnable{false};
			sint32 cropTop{0};
			sint32 cropBottom{0};
			sint32 cropLeft{0};
			sint32 cropRight{0};
		};

		struct DecodedSlice
		{
			DecodeResult result;
			DataToDecode dataToDecode;
		};

		H264DecoderBackend()
		{
			m_displayQueueEvt = (coreinit::OSEvent*)coreinit::OSAllocFromSystem(sizeof(coreinit::OSEvent), 4);
			coreinit::OSInitEvent(m_displayQueueEvt, coreinit::OSEvent::EVENT_STATE::STATE_NOT_SIGNALED, coreinit::OSEvent::EVENT_MODE::MODE_AUTO);
			m_flushEvt = (coreinit::OSEvent*)coreinit::OSAllocFromSystem(sizeof(coreinit::OSEvent), 4);
			coreinit::OSInitEvent(m_flushEvt, coreinit::OSEvent::EVENT_STATE::STATE_NOT_SIGNALED, coreinit::OSEvent::EVENT_MODE::MODE_AUTO);
			m_freeSlots.Fill();
		};

		virtual ~H264DecoderBackend()
		{
			coreinit::OSFreeToSystem(m_displayQueueEvt);
			coreinit::OSFreeToSystem(m_flushEvt);
		};

		virtual void Init(bool isBufferedMode) = 0;
		virtual void Destroy() = 0;

		// The queues take no lock: a slot is the game's from here until the
		// decoder thread takes its index off the decode queue, the decoder's
		// until it puts it on the display queue, and the game's again until
		// GetFrameOutputIfReady frees it
		void QueueForDecode(uint8* data, uint32 length, double timestamp, void* imagePtr)
		{
			DecodedSlice& ds = GetFreeDecodedSliceEntry();

			ds.dataToDecode.m_buffer.assign(data, data + length);
			ds.dataToDecode.m_data = ds.dataToDecode.m_buffer.data();
			ds.dataToDecode.m_length = length;

			ds.result.isDecoded = false;
			ds.result.imageOutput = imagePtr;
			ds.result.timestamp = timestamp;

			m_decodeQueue.push((uint32)std::distance(m_decodedSliceArray.data(), &ds));
		}

		void QueueFlush()
		{
			m_decodeQueue.push(CMD_FLUSH);
		}

		bool GetFrameOutputIfReady(DecodeResult& result)
		{
			uint32 sliceIndex;
			if (!m_displayQueue.peek2(sliceIndex))
				return false;
			DecodedSlice& ds = m_decodedSliceArray[sliceIndex];
			cemu_assert_debug(ds.result.isDecoded);
			std::swap(result, ds.result);
			m_freeSlots.Push(sliceIndex);
			return true;
		}

		coreinit::OSEvent& GetFrameOutputEvent()
		{
			return *m_displayQueueEvt;
		}

		coreinit::OSEvent& GetFlushEvent()
		{
			return *m_flushEvt;
		}

	  protected:
		DecodedSlice& GetFreeDecodedSliceEntry()
		{
			size_t index;
			if (m_freeSlots.Pop(index))
				return m_decodedSliceArray[index];
			cemu_assert_suspicious(); // more than 32 inputs in flight
			return m_decodedSliceArray[0];
		}

		MpmcQueue<uint32> m_decodeQueue; // indices into m_decodedSliceArray, in order of decode input, and commands
		MpmcQueue<uint32> m_displayQueue; // indices into m_decodedSliceArray, in order of frame display output
		coreinit::OSEvent* m_displayQueueEvt; // signalled when a new frame is ready for display
		coreinit::OSEvent* m_flushEvt; // signalled after flush operation finished and all queued slices are decoded

		std::array<DecodedSlice, 32> m_decodedSliceArray;
		IndexFreeList<32> m_freeSlots;
	};
}