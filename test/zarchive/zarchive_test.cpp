// The core's Wii U archive reader (ZArchiveVFS), over archives libretro-common's
// zar writer makes:
//  - lookups and directory listings come from the index read on opening, with
//    no file I/O after it, any case, files and directories, the root included;
//  - every file reads back whole, in chunks and at random places, through
//    blocks kept as they are and compressed ones;
//  - threads share one archive, each with its own ZArchiveFile, and read and
//    look up at once (run under ThreadSanitizer by CI);
//  - an archive that is not one, or is cut short, does not open, and says
//    what it found instead (the reason the user is shown).
// VFSFileStream is the stdio one below, counting what the reader opens and
// reads, in place of the frontend's VFS.
#include "Cafe/Filesystem/ZArchiveVFS.h"

#include <atomic>
#include <cstdio>
#include <random>
#include <thread>

static int s_failures = 0;
#define CHECK(cond) \
	do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); s_failures++; } } while (0)

// --- the stream ----------------------------------------------------------

static std::atomic<uint32> s_opens{0};
static std::atomic<uint32> s_reads{0};

VFSFileStream* VFSFileStream::openFile2(const fs::path& path, bool allowWrite)
{
	FILE* f = std::fopen(path.string().c_str(), "rb");
	if (!f)
		return nullptr;
	s_opens++;
	VFSFileStream* s = new VFSFileStream();
	s->m_vfsHandle = (struct retro_vfs_file_handle*)f;
	s->m_isValid = true;
	return s;
}

VFSFileStream::~VFSFileStream()
{
	if (m_vfsHandle)
		std::fclose((FILE*)m_vfsHandle);
}

uint64 VFSFileStream::GetSize()
{
	FILE* f = (FILE*)m_vfsHandle;
	std::fseek(f, 0, SEEK_END);
	return (uint64)std::ftell(f);
}

void VFSFileStream::SetPosition(uint64 pos)
{
	std::fseek((FILE*)m_vfsHandle, (long)pos, SEEK_SET);
}

uint32 VFSFileStream::readData(void* data, uint32 length)
{
	s_reads++;
	return (uint32)std::fread(data, 1, length, (FILE*)m_vfsHandle);
}

// --- an archive ----------------------------------------------------------

struct TestFile
{
	std::string path;
	std::vector<uint8> data;
};

static int WriteToFile(void* ud, const void* data, size_t len)
{
	return std::fwrite(data, 1, len, (FILE*)ud) == len;
}

static uint8 Byte(size_t file, size_t i, bool random, std::mt19937& rng)
{
	return random ? (uint8)rng() : (uint8)(file * 37 + i / 97);
}

static std::string CommonPath(const char* title, int i)
{
	char name[96];
	std::snprintf(name, sizeof(name), "%s/content/Common/File_%04d.szs", title, i);
	return name;
}

static fs::path MakeArchive(std::vector<TestFile>& files)
{
	std::mt19937 rng(1234);
	const char* title = "0005000010101a00_v0";
	// a title's shape: code, meta, content with a crowded directory
	files.push_back({std::string(title) + "/code/app.xml", std::vector<uint8>(700)});
	files.push_back({std::string(title) + "/code/game.rpx", std::vector<uint8>(3 * 65536 + 4321)});
	files.push_back({std::string(title) + "/meta/meta.xml", std::vector<uint8>(1200)});
	files.push_back({std::string(title) + "/content/empty.bin", {}});
	files.push_back({std::string(title) + "/content/Movie/intro.mp4", std::vector<uint8>(200000)});
	for (int i = 0; i < 1500; i++)
		files.push_back({CommonPath(title, i), std::vector<uint8>(1 + (i * 131) % 3000)});
	for (size_t f = 0; f < files.size(); f++)
	{
		const bool random = f == 4; // a movie compresses no better than it is
		for (size_t i = 0; i < files[f].data.size(); i++)
			files[f].data[i] = Byte(f, i, random, rng);
	}

	fs::path path = fs::temp_directory_path() / "parallel_gx2_zarchive_test.wua";
	FILE* out = std::fopen(path.string().c_str(), "wb");
	CHECK(out != nullptr);
	zar_writer_t* w = zar_writer_new(WriteToFile, out, 3);
	CHECK(w != nullptr);
	CHECK(zar_writer_dir(w, "0005000e10101a00_v32/content") == ZAR_OK);
	for (const TestFile& f : files)
	{
		CHECK(zar_writer_file(w, f.path.c_str()) == ZAR_OK);
		CHECK(zar_writer_append(w, f.data.data(), f.data.size()) == ZAR_OK);
	}
	CHECK(zar_writer_finish(w) == ZAR_OK);
	zar_writer_free(w);
	std::fclose(out);
	return path;
}

static std::string Upper(std::string s)
{
	for (char& c : s)
		if (c >= 'a' && c <= 'z')
			c -= 32;
	return s;
}

// --- tests -------------------------------------------------------------------

static void TestIndex(const ZArchive& zar, const std::vector<TestFile>& files)
{
	const uint32 opens = s_opens, reads = s_reads;

	std::optional<ZArchive::Node> root = zar.LookUp("");
	CHECK(root && root->dir && root->size == 2);
	std::vector<ZArchive::DirEntry> entries;
	CHECK(zar.ListDir("", entries) && entries.size() == 2);
	CHECK(entries.size() == 2 && entries[0].name == "0005000010101a00_v0" && entries[0].node.dir);
	CHECK(entries.size() == 2 && entries[1].name == "0005000e10101a00_v32" && entries[1].node.dir);
	CHECK(!zar.ListDir("0005000010101a00_v0/code/app.xml", entries));
	CHECK(!zar.ListDir("nope", entries));

	bool all = true;
	for (const TestFile& f : files)
	{
		std::optional<ZArchive::Node> n = zar.LookUp(Upper(f.path));
		all = all && n && !n->dir && n->size == f.data.size();
	}
	CHECK(all);
	CHECK(!zar.LookUp("0005000010101a00_v0/content/Common/File_9999.szs"));
	CHECK(!zar.LookUp("0005000010101a00_v0/code/app.xml/x"));

	std::optional<ZArchive::Node> common = zar.LookUp("/0005000010101a00_v0\\content/common/");
	CHECK(common && common->dir && common->size == 1500);
	std::string prev;
	all = zar.ListDir("0005000010101A00_V0/content/Common", entries) && entries.size() == 1500;
	for (const ZArchive::DirEntry& e : entries)
	{
		// each with the size a lookup gives, in name order
		std::optional<ZArchive::Node> n = zar.LookUp("0005000010101a00_v0/content/Common/" + e.name);
		all = all && !e.node.dir && (prev.empty() || prev < e.name) && n && n->size == e.node.size
			&& n->offset == e.node.offset;
		prev = e.name;
	}
	CHECK(all);

	// all of it from the index in memory
	CHECK(s_opens == opens && s_reads == reads);
}

static bool ReadsBack(const ZArchive& zar, const TestFile& f, std::mt19937& rng)
{
	std::optional<ZArchive::Node> n = zar.LookUp(f.path);
	if (!n)
		return false;
	ZArchiveFile file(zar, *n);
	std::vector<uint8> got(f.data.size() + 16);
	if (file.Read(0, got.data(), got.size()) != f.data.size() || !std::equal(f.data.begin(), f.data.end(), got.begin()))
		return false;
	// in chunks, as a game reads
	for (uint64 at = 0; at < f.data.size(); at += 4096)
	{
		const uint64 want = std::min<uint64>(4096, f.data.size() - at);
		if (file.Read(at, got.data(), 4096) != want || !std::equal(got.begin(), got.begin() + want, f.data.begin() + at))
			return false;
	}
	for (int i = 0; i < 20 && !f.data.empty(); i++)
	{
		const uint64 at = rng() % f.data.size();
		const uint64 len = 1 + rng() % 70000;
		const uint64 want = std::min<uint64>(len, f.data.size() - at);
		if (file.Read(at, got.data(), std::min<uint64>(len, got.size())) != std::min<uint64>(want, got.size()))
			return false;
		if (!std::equal(got.begin(), got.begin() + std::min<uint64>(want, got.size()), f.data.begin() + at))
			return false;
	}
	return file.Read(f.data.size(), got.data(), 1) == 0;
}

static void TestReads(const ZArchive& zar, const std::vector<TestFile>& files)
{
	std::mt19937 rng(99);
	bool all = true;
	for (const TestFile& f : files)
		all = all && ReadsBack(zar, f, rng);
	CHECK(all);
}

static void TestThreads(const ZArchive& zar, const std::vector<TestFile>& files)
{
	std::atomic<int> bad{0};
	std::vector<std::thread> threads;
	for (int t = 0; t < 4; t++)
	{
		threads.emplace_back([&, t]() {
			std::mt19937 rng(t + 7);
			for (int i = 0; i < 150; i++)
			{
				const TestFile& f = files[rng() % files.size()];
				if (!ReadsBack(zar, f, rng))
					bad++;
			}
		});
	}
	for (std::thread& t : threads)
		t.join();
	CHECK(bad == 0);
}

static void TestBroken(const fs::path& good)
{
	std::vector<uint8> bytes;
	{
		FILE* f = std::fopen(good.string().c_str(), "rb");
		std::fseek(f, 0, SEEK_END);
		bytes.resize(std::ftell(f));
		std::fseek(f, 0, SEEK_SET);
		CHECK(std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size());
		std::fclose(f);
	}
	const fs::path bad = fs::temp_directory_path() / "parallel_gx2_zarchive_test_bad.wua";
	auto write = [&](size_t len) {
		FILE* f = std::fopen(bad.string().c_str(), "wb");
		std::fwrite(bytes.data(), 1, len, f);
		std::fclose(f);
	};
	// and each says why, which is what the user is told
	std::string why;
	auto says = [&](const char* what) { return why.find(what) != std::string::npos; };
	write(bytes.size() - 1);
	CHECK(ZArchive::Open(bad, &why) == nullptr);
	// cut short by one: the footer is read one octet early
	CHECK(says("magic") && says(fmt::format("the file's: {}", bytes.size() - 1).c_str()));
	write(100);
	CHECK(ZArchive::Open(bad, &why) == nullptr);
	CHECK(says("100 bytes long"));
	CHECK(ZArchive::Open(fs::temp_directory_path() / "parallel_gx2_no_such.wua", &why) == nullptr);
	CHECK(says("could not be opened"));
	// one whose footer is right but whose size is not the file's (as a
	// stream reporting the wrong size would make it): both sizes are given
	{
		std::vector<uint8> saved = bytes;
		const uint64 claimed = bytes.size() + 4096;
		for (int i = 0; i < 8; i++)
			bytes[bytes.size() - 16 + i] = (uint8)(claimed >> (56 - 8 * i));
		write(bytes.size());
		CHECK(ZArchive::Open(bad, &why) == nullptr);
		CHECK(says("magic 169f52d6") && says(fmt::format("size {} (the file's: {})", claimed, bytes.size()).c_str()));
		bytes = saved;
	}
	// an archive that opens leaves the reason alone
	why = "untouched";
	std::unique_ptr<ZArchive> ok(ZArchive::Open(good, &why));
	CHECK(ok != nullptr && why == "untouched");
	fs::remove(bad);
}

int main()
{
	std::vector<TestFile> files;
	const fs::path path = MakeArchive(files);
	std::unique_ptr<ZArchive> zar(ZArchive::Open(path));
	CHECK(zar != nullptr);
	if (zar)
	{
		TestIndex(*zar, files);
		TestReads(*zar, files);
		TestThreads(*zar, files);
	}
	zar.reset();
	TestBroken(path);
	fs::remove(path);
	if (s_failures)
	{
		std::printf("zarchive_test: %d failure(s)\n", s_failures);
		return 1;
	}
	std::printf("zarchive_test: all passed\n");
	return 0;
}
