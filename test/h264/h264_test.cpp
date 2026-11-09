// h264.rpl's decoder engine (src/Cafe/OS/libs/h264_avc/H264DecodeEngine.cpp)
// against what ffmpeg decodes the same streams to (streams/*.txt, made by
// streams/make_streams.sh), with rh264 and with a frontend's decoder (the
// libretro video decode interface, version 4, here over rh264 too):
//  - with B pictures, buffered: pictures come out in display order, each
//    into the buffer of the input that coded it (the tag), every one the
//    picture ffmpeg makes, cropped;
//  - the layout the game is given: NV12 at the coded size, rows 256-byte
//    aligned, the chroma below the luma, the crop rectangle beside it, and
//    nothing written outside the picture;
//  - not buffered: a picture out for every input as it goes in, its own,
//    with B pictures too;
//  - Breath of the Wild's 1920x1088 movies handed over 1080 rows tall;
//  - an input with no picture gets its output at once, without one;
//  - a frontend whose hardware is busy: each call retried, nothing decoded
//    twice or lost;
//  - every input has had its output after a flush, and no frontend decoder
//    is left open.
#include "Cafe/OS/libs/h264_avc/H264DecodeEngine.h"

#include <libretro.h>
#include <formats/rh264.h>
#include <lrc_hash.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

static int s_failures = 0;
#define CHECK(cond) \
	do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); s_failures++; } } while (0)

// --- a frontend's decoder: RetroArch's runloop over rh264, busy now and then --

static int s_busyEvery = 0; // 0: never busy
static int s_calls = 0;
static bool s_wasBusy = false;

static bool Busy()
{
	if (!s_busyEvery || s_wasBusy)
	{
		s_wasBusy = false;
		return false;
	}
	if (++s_calls % s_busyEvery == 0)
	{
		s_wasBusy = true; // the retry goes through
		return true;
	}
	return false;
}

static int FeFrame(rh264_video* v, retro_video_decode_frame* frame)
{
	memset(frame, 0, sizeof(*frame));
	for (int i = 0; i < 3; i++)
	{
		int stride, w, h;
		frame->planes[i] = rh264_video_plane(v, i, &stride, &w, &h);
		if (!frame->planes[i])
			return -1;
		frame->strides[i] = (unsigned)stride;
		if (!i)
		{
			frame->width = (unsigned)w;
			frame->height = (unsigned)h;
		}
	}
	return 1;
}

static void* FeOpen() { return rh264_video_open(); }
static int FeDecode(void* d, const uint8_t* data, size_t size, retro_video_decode_frame* frame)
{
	if (Busy())
		return 2;
	const int r = rh264_video_decode((rh264_video*)d, data, size);
	return r == 1 ? FeFrame((rh264_video*)d, frame) : r;
}
static int FeDrain(void* d, retro_video_decode_frame* frame)
{
	if (Busy())
		return 2;
	return rh264_video_drain((rh264_video*)d) == 0 ? FeFrame((rh264_video*)d, frame) : 0;
}
static void FeClose(void* d) { rh264_video_close((rh264_video*)d); }
static void FeSetTag(void* d, uint64_t tag) { rh264_video_set_tag((rh264_video*)d, tag); }
static uint64_t FeGetTag(void* d) { return rh264_video_tag((rh264_video*)d); }

static retro_video_decode_interface MakeFrontend()
{
	retro_video_decode_interface f{};
	f.interface_version = 4;
	f.open = FeOpen;
	f.decode = FeDecode;
	f.drain = FeDrain;
	f.close = FeClose;
	f.set_tag = FeSetTag;
	f.get_tag = FeGetTag;
	return f;
}

// --- a stream and what ffmpeg made of it -------------------------------------

struct Stream
{
	std::vector<std::vector<uint8_t>> units; // decode order
	std::vector<size_t> displayUnit;         // display order: the unit that coded it
	std::vector<std::string> displayMd5;
};

static Stream Load(const std::string& name)
{
	Stream s;
	const std::string dir = "streams/";
	std::vector<uint8_t> data;
	if (FILE* f = std::fopen((dir + name + ".h264").c_str(), "rb"))
	{
		std::fseek(f, 0, SEEK_END);
		data.resize(std::ftell(f));
		std::fseek(f, 0, SEEK_SET);
		CHECK(std::fread(data.data(), 1, data.size(), f) == data.size());
		std::fclose(f);
	}
	std::map<size_t, size_t> unitAt;
	FILE* t = std::fopen((dir + name + ".txt").c_str(), "r");
	CHECK(t != nullptr);
	char line[256];
	while (t && std::fgets(line, sizeof(line), t))
	{
		size_t a, b;
		char md5[64];
		if (std::sscanf(line, "packet %zu %zu", &a, &b) == 2 && b + a <= data.size())
		{
			unitAt[b] = s.units.size();
			s.units.emplace_back(data.begin() + b, data.begin() + b + a);
		}
		else if (std::sscanf(line, "frame %zu %63s", &a, md5) == 2)
		{
			s.displayUnit.push_back(unitAt.count(a) ? unitAt[a] : (size_t)-1);
			s.displayMd5.push_back(md5);
		}
	}
	if (t)
		std::fclose(t);
	CHECK(!s.units.empty() && s.displayUnit.size() == s.units.size());
	return s;
}

// --- the game's side ---------------------------------------------------------

static constexpr uint8_t kCanary = 0xCD;

struct Sink : H264::EngineSink
{
	size_t bufferSize = 0;
	std::vector<std::vector<uint8_t>> buffers; // by tag
	std::vector<H264::EngineOutput> outputs;   // in the order they came

	explicit Sink(size_t tags, size_t size) : bufferSize(size), buffers(tags, std::vector<uint8_t>(size, kCanary)) {}
	uint8_t* Destination(uint64_t tag) override { return tag < buffers.size() ? buffers[tag].data() : nullptr; }
	void Output(const H264::EngineOutput& out) override { outputs.push_back(out); }
};

static std::string Hex(const uint8_t* d)
{
	char s[33];
	for (int i = 0; i < 16; i++)
		std::snprintf(s + 2 * i, 3, "%02x", d[i]);
	return s;
}

// The cropped picture as ffmpeg's framemd5 hashes it: I420, Y then U then V
static std::string PictureMd5(const H264::EngineOutput& o, const uint8_t* buf, int w, int h)
{
	const int x0 = o.cropLeft * 2, y0 = o.cropTop * 2;
	std::vector<uint8_t> i420;
	for (int y = 0; y < h; y++)
		i420.insert(i420.end(), buf + (size_t)(y0 + y) * o.bytesPerRow + x0, buf + (size_t)(y0 + y) * o.bytesPerRow + x0 + w);
	const uint8_t* uv = buf + (size_t)o.bytesPerRow * o.frameHeight;
	for (int plane = 0; plane < 2; plane++)
		for (int y = 0; y < h / 2; y++)
			for (int x = 0; x < w / 2; x++)
				i420.push_back(uv[(size_t)(y0 / 2 + y) * o.bytesPerRow + x0 + 2 * x + plane]);
	MD5_CTX ctx;
	uint8_t digest[16];
	MD5_Init(&ctx);
	MD5_Update(&ctx, i420.data(), (unsigned long)i420.size());
	MD5_Final(digest, &ctx);
	return Hex(digest);
}

static void Decode(H264::DecodeEngine& e, const std::vector<uint8_t>& unit, uint64_t tag, Sink& sink)
{
	int tries = 0;
	while (e.Decode(unit.data(), unit.size(), tag, sink) == H264::DecodeEngine::BUSY)
		tries++;
	CHECK(tries <= 1);
}

static void Flush(H264::DecodeEngine& e, Sink& sink)
{
	while (e.Flush(sink) == H264::DecodeEngine::BUSY)
		;
}

static const char* s_lane = "";

// A stream in display order, every picture ffmpeg's, at its place in the
// coded picture
static void TestBuffered(const retro_video_decode_interface* fe)
{
	const Stream s = Load("bframes");
	const int w = 200, h = 120, codedW = 208, codedH = 128, stride = 256;
	Sink sink(s.units.size() + 1, (size_t)stride * codedH * 3 / 2);
	{
		H264::DecodeEngine e(fe, false);
		e.Begin(true);
		CHECK(e.UsesFrontend() == (fe != nullptr));
		for (size_t i = 0; i < s.units.size(); i++)
		{
			Decode(e, s.units[i], i, sink);
			if (i == 5)
			{
				// an access unit delimiter alone: out at once, with no picture,
				// ahead of the pictures still held
				const size_t before = sink.outputs.size();
				const std::vector<uint8_t> aud = {0, 0, 0, 1, 0x09, 0xF0};
				Decode(e, aud, s.units.size(), sink);
				CHECK(sink.outputs.size() == before + 1 && sink.outputs.back().tag == s.units.size() && !sink.outputs.back().hasFrame);
				if (sink.outputs.size() == before + 1)
					sink.outputs.pop_back();
			}
		}
		// pictures held back for reordering, not all of them
		CHECK(sink.outputs.size() < s.units.size());
		Flush(e, sink);
	}
	CHECK(H264::DecodeEngine::OpenFrontendDecoders() == 0);
	CHECK(sink.outputs.size() == s.units.size());
	bool order = sink.outputs.size() == s.units.size(), pictures = order, layout = order, outside = order;
	for (size_t k = 0; k < sink.outputs.size() && k < s.displayUnit.size(); k++)
	{
		const H264::EngineOutput& o = sink.outputs[k];
		order = order && o.hasFrame && o.tag == s.displayUnit[k];
		layout = layout && o.frameWidth == codedW && o.frameHeight == codedH && o.bytesPerRow == (uint32_t)stride && o.cropEnable &&
			o.cropLeft == 0 && o.cropTop == 0 && o.cropRight == (codedW - w) / 2 && o.cropBottom == (codedH - h) / 2;
		if (o.tag >= sink.buffers.size())
			continue;
		const uint8_t* buf = sink.buffers[o.tag].data();
		pictures = pictures && PictureMd5(o, buf, w, h) == s.displayMd5[k];
		// the coded picture's cropped band, the rows' padding and the
		// buffer past the chroma are the game's, untouched
		for (int y = 0; y < codedH; y++)
			for (int x = (y < h ? w : 0); x < stride; x++)
				outside = outside && buf[(size_t)y * stride + x] == kCanary;
		for (size_t i = (size_t)stride * codedH + (size_t)stride * (codedH / 2); i < sink.bufferSize; i++)
			outside = outside && buf[i] == kCanary;
	}
	if (!order || !pictures || !layout || !outside)
		std::fprintf(stderr, "  (%s) order %d pictures %d layout %d outside %d\n", s_lane, order, pictures, layout, outside);
	CHECK(order);
	CHECK(pictures);
	CHECK(layout);
	CHECK(outside);
}

// One picture out for every input as it goes in; a unit with no picture
// gets its output at once
static void TestNotBuffered(const retro_video_decode_interface* fe)
{
	const Stream s = Load("baseline");
	const int w = 96, h = 72;
	Sink sink(s.units.size() + 1, (size_t)256 * 80 * 3 / 2);
	H264::DecodeEngine e(fe, false);
	e.Begin(false);
	bool each = true, pictures = true;
	for (size_t i = 0; i < s.units.size(); i++)
	{
		Decode(e, s.units[i], i, sink);
		each = each && sink.outputs.size() == i + 1 && sink.outputs.back().tag == i && sink.outputs.back().hasFrame;
		if (each)
			pictures = pictures && PictureMd5(sink.outputs.back(), sink.buffers[i].data(), w, h) == s.displayMd5[i];
		if (i == 3)
		{
			// an access unit delimiter alone
			const std::vector<uint8_t> aud = {0, 0, 0, 1, 0x09, 0xF0};
			Decode(e, aud, s.units.size(), sink);
			each = each && sink.outputs.size() == i + 2 && sink.outputs.back().tag == s.units.size() && !sink.outputs.back().hasFrame;
			sink.outputs.pop_back();
		}
	}
	CHECK(each);
	CHECK(pictures);
	const size_t before = sink.outputs.size();
	Flush(e, sink);
	CHECK(sink.outputs.size() == before);
}

// With B pictures and not buffered: still a picture for every input as it
// goes in, its own - decode order, as the console's decoder gives them
static void TestNotBufferedReordered(const retro_video_decode_interface* fe)
{
	const Stream s = Load("bframes");
	const int w = 200, h = 120;
	std::vector<std::string> md5Of(s.units.size());
	for (size_t k = 0; k < s.displayUnit.size(); k++)
		if (s.displayUnit[k] < md5Of.size())
			md5Of[s.displayUnit[k]] = s.displayMd5[k];
	Sink sink(s.units.size(), (size_t)256 * 128 * 3 / 2);
	H264::DecodeEngine e(fe, false);
	e.Begin(false);
	bool each = true, pictures = true;
	for (size_t i = 0; i < s.units.size(); i++)
	{
		Decode(e, s.units[i], i, sink);
		each = each && sink.outputs.size() == i + 1 && sink.outputs.back().tag == i && sink.outputs.back().hasFrame;
		if (each)
			pictures = pictures && PictureMd5(sink.outputs.back(), sink.buffers[i].data(), w, h) == md5Of[i];
	}
	CHECK(each);
	CHECK(pictures);
}

// 1920x1088 coded: 1080 rows for Breath of the Wild, the chroma right below
// them; the coded height for anyone else
static void TestBotW(const retro_video_decode_interface* fe)
{
	const Stream s = Load("fullhd");
	for (int botw = 0; botw < 2; botw++)
	{
		Sink sink(s.units.size(), (size_t)2048 * 1088 * 3 / 2);
		H264::DecodeEngine e(fe, botw != 0);
		e.Begin(true);
		for (size_t i = 0; i < s.units.size(); i++)
			Decode(e, s.units[i], i, sink);
		Flush(e, sink);
		CHECK(sink.outputs.size() == s.units.size());
		bool all = sink.outputs.size() == s.units.size();
		for (size_t k = 0; k < sink.outputs.size(); k++)
		{
			const H264::EngineOutput& o = sink.outputs[k];
			all = all && o.hasFrame && o.frameWidth == 1920 && o.frameHeight == (botw ? 1080 : 1088) && o.bytesPerRow == 2048 &&
				PictureMd5(o, sink.buffers[o.tag].data(), 1920, 1080) == s.displayMd5[k];
		}
		CHECK(all);
	}
}

static void Lanes(const char* lane, const retro_video_decode_interface* fe)
{
	s_lane = lane;
	TestBuffered(fe);
	TestNotBuffered(fe);
	TestNotBufferedReordered(fe);
	TestBotW(fe);
}

int main()
{
	Lanes("rh264", nullptr);
	const retro_video_decode_interface fe = MakeFrontend();
	Lanes("frontend", &fe);
	s_busyEvery = 3;
	Lanes("frontend, busy", &fe);
	CHECK(s_calls > 0);
	// an interface older than version 4 has no tags: rh264 instead
	retro_video_decode_interface v3 = fe;
	v3.interface_version = 3;
	H264::DecodeEngine old(&v3, false);
	old.Begin(true);
	CHECK(!old.UsesFrontend());
	if (s_failures)
	{
		std::printf("h264_test: %d failure(s)\n", s_failures);
		return 1;
	}
	std::printf("h264_test: all passed\n");
	return 0;
}
