#pragma once

#include <cstdint>
#include <cstddef>

struct retro_video_decode_interface;
struct rh264_video;

namespace H264
{
	// What the decoder hands the game for one input: the picture its tag
	// names, written into that input's own buffer, in the layout the Wii U's
	// decoder gives it - NV12 at the coded size, rows 256-byte aligned, the
	// chroma right below the luma - with the SPS's crop rectangle beside it
	// for the game to apply. hasFrame is false for an input that made no
	// picture (one that failed to decode, or held no picture at all): its
	// buffer is left as it was, and the game still gets its callback.
	struct EngineOutput
	{
		uint64_t tag{};
		bool hasFrame{false};
		int32_t frameWidth{0};
		int32_t frameHeight{0};
		uint32_t bytesPerRow{0};
		bool cropEnable{false};
		int32_t cropTop{0};
		int32_t cropBottom{0};
		int32_t cropLeft{0};
		int32_t cropRight{0};
	};

	class EngineSink
	{
	public:
		virtual ~EngineSink() = default;
		// The buffer the picture of the input tagged @tag goes to, big enough
		// for the coded size's NV12 at a 256-byte aligned stride
		virtual uint8_t* Destination(uint64_t tag) = 0;
		// Called once per input, in the order pictures are output
		virtual void Output(const EngineOutput& out) = 0;
	};

	// One H.264 stream's decoder: the frontend's (RetroArch's, hardware where
	// it has it) when it offers version 4 of the video decode interface, whose
	// tags carry each input's identity through B-picture reordering, and
	// rh264's otherwise. Not thread safe: one thread drives it.
	class DecodeEngine
	{
	public:
		enum Result
		{
			DONE = 0, // the input is taken; zero or more outputs were made
			BUSY = 1, // the hardware is behind: call again with the same input
		};

		// @frontend may be null. @isBotW: Breath of the Wild's 1920x1088
		// movies are handed over 1080 rows tall, which the game wants
		DecodeEngine(const retro_video_decode_interface* frontend, bool isBotW);
		~DecodeEngine();
		DecodeEngine(const DecodeEngine&) = delete;
		DecodeEngine& operator=(const DecodeEngine&) = delete;

		// Buffered: pictures in display order, as many held back as the
		// stream reorders. Not buffered: one picture out for every input, in
		// decode order. Starts the stream over.
		void Begin(bool buffered);
		Result Decode(const uint8_t* data, size_t length, uint64_t tag, EngineSink& sink);
		// Every picture still held, then a hasFrame=false output for each input
		// that made none. BUSY: call again.
		Result Flush(EngineSink& sink);

		bool UsesFrontend() const { return m_frontendDecoder != nullptr; }
		// Frontend decoders open in the process, for the check that none
		// outlives the graphics context
		static int OpenFrontendDecoders();

	private:
		struct Geometry
		{
			int32_t codedWidth{0}, codedHeight{0};
			int32_t cropLeftPx{0}, cropTopPx{0};
			bool cropEnable{false};
			int32_t cropLeft{0}, cropRight{0}, cropTop{0}, cropBottom{0};
		};
		struct Pending
		{
			uint64_t tag;
			Geometry geometry;
		};
		struct Planes
		{
			const uint8_t* plane[3];
			unsigned stride[3];
			unsigned width, height;
		};

		void Open();
		void Close();
		int RawDecode(const uint8_t* data, size_t length); // 1 picture, 0 none, -1 error, 2 busy
		int RawDrain(); // 1 picture, 0 none, -1 error, 2 busy
		bool CurrentPicture(Planes& planes, uint64_t& tag);
		void Emit(EngineSink& sink);
		void EmitEmpty(EngineSink& sink, size_t pendingIndex);
		void EmitEmptyFor(EngineSink& sink, uint64_t tag);
		bool TakePending(uint64_t tag, Geometry& geometry);
		bool ScanAccessUnit(const uint8_t* data, size_t length);

		const retro_video_decode_interface* m_frontend;
		void* m_frontendDecoder{nullptr};
		rh264_video* m_video{nullptr};
		bool m_isBotW;
		bool m_buffered{true};
		bool m_frontendFrameValid{false};
		// the frontend's current picture, from its last decode or drain
		const uint8_t* m_fePlane[3]{};
		unsigned m_feStride[3]{};
		unsigned m_feWidth{0}, m_feHeight{0};
		enum { STAGE_NONE, STAGE_DECODE, STAGE_DRAIN } m_stage{STAGE_NONE}; // what a BUSY left to do
		bool m_hasSlice{true}; // the input in hand makes a picture
		Geometry m_geometry; // the stream's, from its latest SPS
		static constexpr size_t kMaxPending = 64;
		Pending m_pending[kMaxPending];
		size_t m_numPending{0};
	};
}
