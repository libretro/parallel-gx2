#include "Cafe/OS/libs/h264_avc/H264DecodeEngine.h"
#include "Cafe/OS/libs/h264_avc/parser/H264Parser.h"

#include <algorithm>
#include <atomic>
#include <cstring>

#include <libretro.h>
#include <formats/rh264.h>

namespace H264
{
	static std::atomic<int> s_openFrontendDecoders{0};

	int DecodeEngine::OpenFrontendDecoders()
	{
		return s_openFrontendDecoders.load(std::memory_order_acquire);
	}

	DecodeEngine::DecodeEngine(const retro_video_decode_interface* frontend, bool isBotW)
		: m_frontend(frontend && frontend->interface_version >= 4 && frontend->open && frontend->decode && frontend->drain && frontend->close && frontend->set_tag && frontend->get_tag ? frontend : nullptr),
		  m_isBotW(isBotW)
	{
	}

	DecodeEngine::~DecodeEngine()
	{
		Close();
	}

	void DecodeEngine::Open()
	{
		if (m_frontendDecoder || m_video)
			return;
		if (m_frontend && (m_frontendDecoder = m_frontend->open()) != nullptr)
		{
			s_openFrontendDecoders.fetch_add(1, std::memory_order_acq_rel);
			return;
		}
		m_video = rh264_video_open();
	}

	void DecodeEngine::Close()
	{
		if (m_frontendDecoder)
		{
			m_frontend->close(m_frontendDecoder);
			m_frontendDecoder = nullptr;
			s_openFrontendDecoders.fetch_sub(1, std::memory_order_acq_rel);
		}
		if (m_video)
		{
			rh264_video_close(m_video);
			m_video = nullptr;
		}
		m_frontendFrameValid = false;
	}

	void DecodeEngine::Begin(bool buffered)
	{
		m_buffered = buffered;
		Open();
	}

	// The SPS's geometry, for the coded size and crop rectangle the game is
	// told, and whether the unit holds a picture's slice at all
	bool DecodeEngine::ScanAccessUnit(const uint8_t* data, size_t length)
	{
		bool hasSlice = false;
		size_t i = 0;
		while (i + 3 <= length)
		{
			if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1)
			{
				i++;
				continue;
			}
			const size_t start = i + 3;
			size_t end = start;
			while (end + 3 <= length && !(data[end] == 0 && data[end + 1] == 0 && data[end + 2] <= 1))
				end++;
			if (end + 3 > length)
				end = length;
			i = end;
			if (start >= end)
				continue;
			const uint8_t type = data[start] & 0x1F;
			if (type == 1 || type == 5)
				hasSlice = true;
			else if (type == 7 && end - start > 1)
			{
				h264State_seq_parameter_set_t sps{};
				if (!h264Parser_ParseSPS(const_cast<uint8_t*>(data + start + 1), (uint32_t)(end - start - 1), sps))
					continue;
				Geometry g;
				const int32_t fieldFactor = sps.frame_mbs_only_flag ? 1 : 2;
				g.codedWidth = (int32_t)(sps.pic_width_in_mbs_minus1 + 1) * 16;
				g.codedHeight = (int32_t)(sps.pic_height_in_map_units_minus1 + 1) * 16 * fieldFactor;
				if (sps.frame_cropping_flag)
				{
					// 4:2:0: a crop unit is two samples across and two rows
					// (four for field coding) down
					g.cropEnable = true;
					g.cropLeft = (int32_t)sps.frame_crop_left_offset;
					g.cropRight = (int32_t)sps.frame_crop_right_offset;
					g.cropTop = (int32_t)sps.frame_crop_top_offset;
					g.cropBottom = (int32_t)sps.frame_crop_bottom_offset;
					g.cropLeftPx = g.cropLeft * 2;
					g.cropTopPx = g.cropTop * 2 * fieldFactor;
				}
				m_geometry = g;
			}
		}
		return hasSlice;
	}

	int DecodeEngine::RawDecode(const uint8_t* data, size_t length)
	{
		if (m_frontendDecoder)
		{
			retro_video_decode_frame frame{};
			m_frontendFrameValid = false;
			const int r = m_frontend->decode(m_frontendDecoder, data, length, &frame);
			if (r == 1)
			{
				for (int i = 0; i < 3; i++)
				{
					m_fePlane[i] = frame.planes[i];
					m_feStride[i] = frame.strides[i];
				}
				m_feWidth = frame.width;
				m_feHeight = frame.height;
				m_frontendFrameValid = frame.planes[0] && frame.planes[1] && frame.planes[2];
				return m_frontendFrameValid ? 1 : -1;
			}
			return r == 2 ? 2 : r < 0 ? -1 : 0;
		}
		if (!m_video)
			return -1;
		const int r = rh264_video_decode(m_video, data, length);
		return r == 1 ? 1 : r == 2 ? 2 : r < 0 ? -1 : 0;
	}

	int DecodeEngine::RawDrain()
	{
		if (m_frontendDecoder)
		{
			retro_video_decode_frame frame{};
			m_frontendFrameValid = false;
			const int r = m_frontend->drain(m_frontendDecoder, &frame);
			if (r == 1)
			{
				for (int i = 0; i < 3; i++)
				{
					m_fePlane[i] = frame.planes[i];
					m_feStride[i] = frame.strides[i];
				}
				m_feWidth = frame.width;
				m_feHeight = frame.height;
				m_frontendFrameValid = frame.planes[0] && frame.planes[1] && frame.planes[2];
				return m_frontendFrameValid ? 1 : -1;
			}
			return r == 2 ? 2 : r < 0 ? -1 : 0;
		}
		if (!m_video)
			return 0;
		const int r = rh264_video_drain(m_video);
		return r == 0 ? 1 : r == 2 ? 2 : r == -2 ? -1 : 0;
	}

	bool DecodeEngine::CurrentPicture(Planes& planes, uint64_t& tag)
	{
		if (m_frontendDecoder)
		{
			if (!m_frontendFrameValid)
				return false;
			for (int i = 0; i < 3; i++)
			{
				planes.plane[i] = m_fePlane[i];
				planes.stride[i] = m_feStride[i];
			}
			planes.width = m_feWidth;
			planes.height = m_feHeight;
			tag = m_frontend->get_tag(m_frontendDecoder);
			return true;
		}
		for (int i = 0; i < 3; i++)
		{
			int stride = 0, w = 0, h = 0;
			planes.plane[i] = rh264_video_plane(m_video, i, &stride, &w, &h);
			if (!planes.plane[i] || stride <= 0 || w <= 0 || h <= 0)
				return false;
			planes.stride[i] = (unsigned)stride;
			if (i == 0)
			{
				planes.width = (unsigned)w;
				planes.height = (unsigned)h;
			}
		}
		if (rh264_video_bit_depth(m_video) != 8)
			return false;
		tag = rh264_video_tag(m_video);
		return true;
	}

	bool DecodeEngine::TakePending(uint64_t tag, Geometry& geometry)
	{
		for (size_t i = 0; i < m_numPending; i++)
		{
			if (m_pending[i].tag != tag)
				continue;
			geometry = m_pending[i].geometry;
			std::copy(m_pending + i + 1, m_pending + m_numPending, m_pending + i);
			m_numPending--;
			return true;
		}
		return false;
	}

	void DecodeEngine::EmitEmpty(EngineSink& sink, size_t pendingIndex)
	{
		EngineOutput out;
		out.tag = m_pending[pendingIndex].tag;
		std::copy(m_pending + pendingIndex + 1, m_pending + m_numPending, m_pending + pendingIndex);
		m_numPending--;
		sink.Output(out);
	}

	void DecodeEngine::EmitEmptyFor(EngineSink& sink, uint64_t tag)
	{
		for (size_t i = 0; i < m_numPending; i++)
		{
			if (m_pending[i].tag == tag)
			{
				EmitEmpty(sink, i);
				return;
			}
		}
	}

	// The current picture into its input's buffer: the cropped picture the
	// decoder gives back at its place in the coded one, U and V interleaved
	void DecodeEngine::Emit(EngineSink& sink)
	{
		Planes planes;
		uint64_t tag = 0;
		Geometry g;
		if (!CurrentPicture(planes, tag) || !TakePending(tag, g))
			return;
		const int32_t width = (int32_t)planes.width, height = (int32_t)planes.height;
		if (g.codedWidth < g.cropLeftPx + width || g.codedHeight < g.cropTopPx + height)
		{
			// no SPS seen, or one this picture does not fit: what came out is
			// the picture
			g = Geometry();
			g.codedWidth = width;
			g.codedHeight = height;
		}
		int32_t rows = g.codedHeight;
		if (m_isBotW && g.codedWidth == 1920 && g.codedHeight == 1088)
			rows = 1080;
		const uint32_t stride = ((uint32_t)g.codedWidth + 0xFF) & ~0xFFu;

		EngineOutput out;
		out.tag = tag;
		out.hasFrame = true;
		out.frameWidth = g.codedWidth;
		out.frameHeight = rows;
		out.bytesPerRow = stride;
		out.cropEnable = g.cropEnable;
		out.cropLeft = g.cropLeft;
		out.cropRight = g.cropRight;
		out.cropTop = g.cropTop;
		out.cropBottom = g.cropBottom;

		if (uint8_t* dst = sink.Destination(tag))
		{
			const int32_t lumaRows = std::min(height, rows - g.cropTopPx);
			for (int32_t y = 0; y < lumaRows; y++)
				memcpy(dst + (size_t)(g.cropTopPx + y) * stride + g.cropLeftPx, planes.plane[0] + (size_t)y * planes.stride[0], (size_t)width);
			uint8_t* uv = dst + (size_t)stride * rows;
			const int32_t chromaWidth = (width + 1) / 2;
			const int32_t chromaRows = std::min((height + 1) / 2, rows / 2 - g.cropTopPx / 2);
			for (int32_t y = 0; y < chromaRows; y++)
			{
				const uint8_t* u = planes.plane[1] + (size_t)y * planes.stride[1];
				const uint8_t* v = planes.plane[2] + (size_t)y * planes.stride[2];
				uint8_t* row = uv + (size_t)(g.cropTopPx / 2 + y) * stride + g.cropLeftPx;
				for (int32_t x = 0; x < chromaWidth; x++)
				{
					row[2 * x] = u[x];
					row[2 * x + 1] = v[x];
				}
			}
		}
		sink.Output(out);
	}

	DecodeEngine::Result DecodeEngine::Decode(const uint8_t* data, size_t length, uint64_t tag, EngineSink& sink)
	{
		Open();
		if (m_stage == STAGE_NONE)
		{
			m_hasSlice = ScanAccessUnit(data, length);
			if (m_numPending == kMaxPending)
				EmitEmpty(sink, 0); // a decoder holding more than any stream reorders lost it
			m_pending[m_numPending++] = {tag, m_geometry};
			if (m_frontendDecoder)
				m_frontend->set_tag(m_frontendDecoder, tag);
			else if (m_video)
				rh264_video_set_tag(m_video, tag);
		}
		// a retry after BUSY picks up at the step that was busy
		const int r = m_stage == STAGE_DRAIN ? 0 : RawDecode(data, length);
		if (r == 2)
		{
			m_stage = STAGE_DECODE;
			return BUSY;
		}
		if (m_stage == STAGE_DRAIN)
			;
		else if (r == 1)
			Emit(sink);
		else if (r < 0)
		{
			EmitEmptyFor(sink, tag);
			if (m_frontendDecoder)
			{
				// the frontend's decoder is gone for good (a lost device):
				// rh264 from here, its pictures right again at the next IDR
				m_frontend->close(m_frontendDecoder);
				m_frontendDecoder = nullptr;
				s_openFrontendDecoders.fetch_sub(1, std::memory_order_acq_rel);
				m_frontend = nullptr;
				m_frontendFrameValid = false;
				m_video = rh264_video_open();
			}
		}
		else if (!m_hasSlice)
			EmitEmptyFor(sink, tag); // parameter sets alone
		if (!m_buffered)
		{
			// a picture for every input: whatever is held comes out now
			for (;;)
			{
				bool pending = false;
				for (size_t i = 0; i < m_numPending; i++)
					pending = pending || m_pending[i].tag == tag;
				if (!pending)
					break;
				const int d = RawDrain();
				if (d == 2)
				{
					m_stage = STAGE_DRAIN;
					return BUSY;
				}
				if (d != 1)
				{
					EmitEmptyFor(sink, tag);
					break;
				}
				Emit(sink);
			}
		}
		m_stage = STAGE_NONE;
		return DONE;
	}

	DecodeEngine::Result DecodeEngine::Flush(EngineSink& sink)
	{
		for (;;)
		{
			const int d = RawDrain();
			if (d == 2)
				return BUSY;
			if (d != 1)
				break;
			Emit(sink);
		}
		while (m_numPending)
			EmitEmpty(sink, 0);
		return DONE;
	}
}
