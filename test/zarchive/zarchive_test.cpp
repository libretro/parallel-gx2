// The core's Wii U archive reader (ZArchiveVFS), over archives libretro-common's
// zar writer makes:
//  - lookups and directory listings come from the index read on opening, with
//    no file I/O after it, any case, files and directories, the root included;
//  - every file reads back whole, in chunks and at random places, through
//    blocks kept as they are and compressed ones;
//  - threads share one archive and read and look up at once (run under
//    ThreadSanitizer by CI);
//  - the archive's readers are lent and given back: on one thread every
//    lookup, listing and read of every file shares one, and four threads
//    at once make no more than four;
//  - an archive shared by path: asked for twice, the same one, kept until
//    the last holder gives it back; asked for and given back by four threads
//    at once, never more than one for the path, and none once all are back;
//  - an archive that maps is read from its mapping alone: no lookup,
//    listing or read of any file opens a stream; one that does not map (as
//    a frontend's VFS file does not) reads the same through the stream it
//    was opened with on one thread, and no more than one a thread on four;
//  - a listing taken a name at a time and given up halfway gives its reader
//    back; a path too long for a listing or a lookup is none;
//  - an archive that is not one, or is cut short, does not open, and says
//    what it found instead (the reason the user is shown);
//  - a title's meta.xml as Breath of the Wild's (a BOM, twelve languages,
//    Japanese) parses, and one that will not says why: not XML (where),
//    no <menu>, no title_id, empty;
//  - a directory out of order with a name in it twice, as Breath of the
//    Wild's .wua holds its meta/ (meta.xml first and last, the rest unsorted
//    between): every name is found, the first of the two the one a lookup
//    gives, as Cemu's reader takes it.
// The reader is C (zarchive.c, over shared_file.c), built as the C89 lane
// builds it; the shared table and the listing helpers are the C++ over it.
#include "Cafe/Filesystem/ZArchiveVFS.h"
#include "Cafe/TitleList/ParsedMetaXml.h"


#include <atomic>
#include <functional>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>

static int s_failures = 0;
#define CHECK(cond) \
	do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); s_failures++; } } while (0)

// --- the archive, as the C++ above it uses one ---------------------------

// as the core's, but for a case-insensitive search the test does not need
std::string FileStream::HostPath(const fs::path& path)
{
	return path.string();
}

static ZArchivePtr Open(const fs::path& path, bool map = true, std::string* whyNot = nullptr)
{
	char why[512];
	ZArchivePtr zar(zarchive_open(path.string().c_str(), map ? 0 : ZARCHIVE_OPEN_NO_MAP, why, sizeof(why)));
	if (!zar && whyNot)
		*whyNot = why;
	return zar;
}

static std::optional<zar_file_t> LookUp(const zarchive_t* zar, std::string_view path)
{
	zar_file_t n;
	if (!zarchive_lookup(zar, path.data(), path.size(), &n))
		return std::nullopt;
	return n;
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

static void TestIndex(const zarchive_t* zar, const std::vector<TestFile>& files)
{
	const unsigned streams = zarchive_streams_opened(zar);

	std::optional<zar_file_t> root = LookUp(zar, "");
	CHECK(root && root->dir && root->size == 2);
	std::vector<ZArchiveEntry> entries;
	CHECK(ZArchive_ListDir(zar, "", entries) && entries.size() == 2);
	CHECK(entries.size() == 2 && entries[0].name == "0005000010101a00_v0" && entries[0].node.dir);
	CHECK(entries.size() == 2 && entries[1].name == "0005000e10101a00_v32" && entries[1].node.dir);
	CHECK(!ZArchive_ListDir(zar, "0005000010101a00_v0/code/app.xml", entries));
	CHECK(!ZArchive_ListDir(zar, "nope", entries));

	bool all = true;
	for (const TestFile& f : files)
	{
		std::optional<zar_file_t> n = LookUp(zar, Upper(f.path));
		all = all && n && !n->dir && n->size == f.data.size();
	}
	CHECK(all);
	CHECK(!LookUp(zar, "0005000010101a00_v0/content/Common/File_9999.szs"));
	CHECK(!LookUp(zar, "0005000010101a00_v0/code/app.xml/x"));

	std::optional<zar_file_t> common = LookUp(zar, "/0005000010101a00_v0\\content/common/");
	CHECK(common && common->dir && common->size == 1500);
	std::string prev;
	all = ZArchive_ListDir(zar, "0005000010101A00_V0/content/Common", entries) && entries.size() == 1500;
	for (const ZArchiveEntry& e : entries)
	{
		// each with the size a lookup gives, in name order
		std::optional<zar_file_t> n = LookUp(zar, "0005000010101a00_v0/content/Common/" + e.name);
		all = all && !e.node.dir && (prev.empty() || prev < e.name) && n && n->size == e.node.size
			&& n->offset == e.node.offset;
		prev = e.name;
	}
	CHECK(all);

	// all of it from the index in memory: no stream more
	CHECK(zarchive_streams_opened(zar) == streams);

	// a listing a name at a time, given up halfway: its reader goes back
	{
		zarchive_dir_t dir;
		const char* name;
		size_t len;
		zar_file_t node;
		CHECK(zarchive_dir_open(zar, &dir, "0005000010101a00_v0/content/Common", 34));
		for (int i = 0; i < 10; i++)
			CHECK(zarchive_dir_next(&dir, &name, &len, &node) && len == 13 && name[len] == 0);
		zarchive_dir_close(&dir);
		const unsigned made = zarchive_readers_made(zar);
		CHECK(LookUp(zar, "0005000010101a00_v0/code/app.xml"));
		CHECK(zarchive_readers_made(zar) == made);
		// a path too long for a listing is none
		const std::string longPath(ZARCHIVE_DIR_PATH_MAX, 'a');
		CHECK(!zarchive_dir_open(zar, &dir, longPath.data(), longPath.size()) && !zarchive_dir_next(&dir, &name, &len, &node));
		zarchive_dir_close(&dir);
		CHECK(!LookUp(zar, longPath));
	}
}

static bool ReadsBack(const zarchive_t* zar, const TestFile& f, std::mt19937& rng)
{
	std::optional<zar_file_t> n = LookUp(zar, f.path);
	if (!n)
		return false;
	struct
	{
		const zarchive_t* zar;
		zar_file_t node;
		uint64 Read(uint64 offset, void* dst, uint64 size) { return zarchive_read(zar, &node, offset, dst, size); }
	} file{zar, *n};
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

static void TestReads(const zarchive_t* zar, const std::vector<TestFile>& files)
{
	std::mt19937 rng(99);
	bool all = true;
	for (const TestFile& f : files)
		all = all && ReadsBack(zar, f, rng);
	CHECK(all);
}

static void TestThreads(const zarchive_t* zar, const std::vector<TestFile>& files)
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
	CHECK(Open(bad, true, &why) == nullptr);
	// cut short by one: the footer is read one octet early
	CHECK(says("magic") && says(fmt::format("the file's: {}", bytes.size() - 1).c_str()));
	write(100);
	CHECK(Open(bad, true, &why) == nullptr);
	CHECK(says("100 bytes long"));
	CHECK(Open(fs::temp_directory_path() / "parallel_gx2_no_such.wua", true, &why) == nullptr);
	CHECK(says("could not be opened"));
	// one whose footer is right but whose size is not the file's (as a
	// stream reporting the wrong size would make it): both sizes are given
	{
		std::vector<uint8> saved = bytes;
		const uint64 claimed = bytes.size() + 4096;
		for (int i = 0; i < 8; i++)
			bytes[bytes.size() - 16 + i] = (uint8)(claimed >> (56 - 8 * i));
		write(bytes.size());
		CHECK(Open(bad, true, &why) == nullptr);
		CHECK(says("magic 169f52d6") && says(fmt::format("size {} (the file's: {})", claimed, bytes.size()).c_str()));
		bytes = saved;
	}
	// an archive that opens leaves the reason alone
	why = "untouched";
	ZArchivePtr ok = Open(good, true, &why);
	CHECK(ok != nullptr && why == "untouched");
	fs::remove(bad);
}

static std::string BotwMetaXml()
{
	std::string xml = "\xef\xbb\xbf<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<menu type=\"complex\" access=\"777\">\n"
		"  <version type=\"unsignedInt\" length=\"4\">33</version>\n"
		"  <product_code type=\"string\" length=\"32\">WUP-P-ALZE</product_code>\n"
		"  <title_id type=\"hexBinary\" length=\"8\">00050000101C9400</title_id>\n"
		"  <title_version type=\"unsignedInt\" length=\"4\">0</title_version>\n"
		"  <region type=\"hexBinary\" length=\"4\">00000002</region>\n";
	const char* langs[] = {"ja", "en", "fr", "de", "it", "es", "zhs", "ko", "nl", "pt", "ru", "zht"};
	for (const char* l : langs)
		xml += fmt::format("  <longname_{0} type=\"string\" length=\"512\">\xe3\x82\xbc\xe3\x83\xab\xe3\x83\x80\xe3\x81\xae\xe4\xbc\x9d\xe8\xaa\xac\n\xe3\x83\x96\xe3\x83\xac\xe3\x82\xb9 {0}</longname_{0}>\n"
			"  <shortname_{0} type=\"string\" length=\"512\">The Legend of Zelda {0}</shortname_{0}>\n"
			"  <publisher_{0} type=\"string\" length=\"256\">Nintendo</publisher_{0}>\n", l);
	for (int i = 0; i < 300; i++) // BotW's is long: padded out with other elements
		xml += fmt::format("  <pc_e{0} type=\"unsignedInt\" length=\"4\">{0}</pc_e{0}>\n", i);
	xml += "</menu>\n";
	return xml;
}

static void TestMetaXml()
{
	const std::string xml = BotwMetaXml();
	auto parse = [](std::string text, std::string* why) {
		std::vector<uint8> buf(text.begin(), text.end());
		return std::unique_ptr<ParsedMetaXml>(ParsedMetaXml::Parse(buf.data(), buf.size(), why));
	};
	std::string why = "untouched";
	auto ok = parse(xml, &why);
	CHECK(ok && ok->GetTitleId() == 0x00050000101C9400ull && why == "untouched");
	CHECK(ok && ok->GetShortName(CafeConsoleLanguage::EN) == "The Legend of Zelda en");
	CHECK(ok && ok->GetLongName(CafeConsoleLanguage::JA).find("\xe3\x82\xbc") == 0);
	CHECK(!parse(xml.substr(0, xml.size() / 2), &why) && why.find("do not parse as XML") != std::string::npos && why.find("at offset") != std::string::npos);
	std::string notMenu = xml;
	notMenu.replace(notMenu.find("<menu"), 5, "<item");
	notMenu.replace(notMenu.rfind("</menu>"), 7, "</item>");
	CHECK(!parse(notMenu, &why) && why == "its root element is <item>, not <menu>");
	std::string noId = xml;
	noId.erase(noId.find("  <title_id"), noId.find("</title_id>") + 12 - noId.find("  <title_id"));
	CHECK(!parse(noId, &why) && why == "it has no title_id");
	CHECK(!parse("", &why) && why == "it is empty");
}

static uint32 Be32(const uint8* p)
{
	return ((uint32)p[0] << 24) | ((uint32)p[1] << 16) | ((uint32)p[2] << 8) | p[3];
}

static uint64 Be64(const uint8* p)
{
	return ((uint64)Be32(p) << 32) | Be32(p + 4);
}

static void TestUnordered()
{
	const char* meta = "00050000101c9400_v0/meta/";
	const std::string xml = BotwMetaXml();
	const std::vector<std::pair<std::string, std::string>> files = {
		{"meta.xml", xml}, {"bootTvTex.tga", std::string(3000, 'T')}, {"iconTex.tga", std::string(900, 'I')},
		{"Manual.bfma", std::string(5000, 'M')}, {"PEGI_en.jpg", "J"}, {"USK_de.jpg", "U"}};
	const fs::path path = fs::temp_directory_path() / "parallel_gx2_zarchive_test_unordered.wua";
	FILE* out = std::fopen(path.string().c_str(), "wb");
	CHECK(out != nullptr);
	zar_writer_t* w = zar_writer_new(WriteToFile, out, 3);
	CHECK(zar_writer_file(w, "00050000101c9400_v0/code/app.xml") == ZAR_OK);
	for (const auto& f : files)
	{
		CHECK(zar_writer_file(w, (meta + f.first).c_str()) == ZAR_OK);
		CHECK(zar_writer_append(w, f.second.data(), f.second.size()) == ZAR_OK);
	}
	CHECK(zar_writer_finish(w) == ZAR_OK);
	zar_writer_free(w);
	std::fclose(out);

	// the index as the writer left it: meta/'s entries sorted
	std::vector<uint8> bytes;
	{
		FILE* f = std::fopen(path.string().c_str(), "rb");
		std::fseek(f, 0, SEEK_END);
		bytes.resize(std::ftell(f));
		std::fseek(f, 0, SEEK_SET);
		CHECK(std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size());
		std::fclose(f);
	}
	const uint8* footer = bytes.data() + bytes.size() - 144;
	const uint8* names = bytes.data() + Be64(footer + 32);
	uint8* tree = bytes.data() + Be64(footer + 48);
	auto nameOf = [&](const uint8* node) {
		const uint8* n = names + (Be32(node) & 0x7FFFFFFF);
		return std::string((const char*)n + 1, n[0] & 0x7F);
	};
	auto child = [&](uint32 dir, const char* name) {
		for (uint32 i = 0; i < Be32(tree + dir * 16 + 8); i++)
			if (nameOf(tree + (Be32(tree + dir * 16 + 4) + i) * 16) == name)
				return Be32(tree + dir * 16 + 4) + i;
		return 0u;
	};
	const uint32 metaDir = child(child(0, "00050000101c9400_v0"), "meta");
	const uint32 first = Be32(tree + metaDir * 16 + 4), count = Be32(tree + metaDir * 16 + 8);
	CHECK(metaDir && count == files.size());
	// reversed, and the last - the first sorted, bootTvTex.tga - named
	// meta.xml, a second one after the real one
	for (uint32 i = 0; i < count / 2; i++)
		for (int k = 0; k < 16; k++)
			std::swap(tree[(first + i) * 16 + k], tree[(first + count - 1 - i) * 16 + k]);
	uint8* last = tree + (first + count - 1) * 16;
	CHECK(nameOf(last) == "bootTvTex.tga");
	uint32 metaXml = 0;
	for (uint32 i = 0; i < count; i++)
		if (nameOf(tree + (first + i) * 16) == "meta.xml")
			metaXml = first + i;
	CHECK(metaXml && metaXml < first + count - 1);
	std::memcpy(last, tree + metaXml * 16, 4);
	{
		FILE* f = std::fopen(path.string().c_str(), "wb");
		CHECK(std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size());
		std::fclose(f);
	}

	ZArchivePtr zar = Open(path);
	CHECK(zar != nullptr);
	if (zar)
	{
		std::optional<zar_file_t> n = LookUp(zar.get(), std::string(meta) + "META.XML");
		CHECK(n && n->size == xml.size());
		if (n)
		{
			std::vector<uint8> buf(n->size);
			CHECK(zarchive_read(zar.get(), &*n, 0, buf.data(), buf.size()) == xml.size());
			std::unique_ptr<ParsedMetaXml> parsed(ParsedMetaXml::Parse(buf.data(), buf.size()));
			CHECK(parsed && parsed->GetTitleId() == 0x00050000101C9400ull);
		}
		// bootTvTex.tga is no more; every other name is found
		CHECK(!LookUp(zar.get(), std::string(meta) + "bootTvTex.tga"));
		bool all = true;
		for (const auto& f : files)
			if (f.first != "bootTvTex.tga")
			{
				n = LookUp(zar.get(), meta + f.first);
				all = all && n && n->size == f.second.size();
			}
		CHECK(all);
		std::vector<ZArchiveEntry> entries;
		CHECK(ZArchive_ListDir(zar.get(), std::string(meta), entries) && entries.size() == files.size());
		CHECK(entries.size() == files.size() && entries.front().name == "USK_de.jpg" && entries.back().name == "meta.xml");
	}
	zar.reset();
	fs::remove(path);
}

// zarchive.c's test hooks (ZARCHIVE_TEST_HOOKS): run once each when set
static std::function<void(const char*)> s_onOpened;
static std::function<void(const zarchive_t*)> s_onLetGo;
extern "C" void zarchive_test_opened(const char* path)
{
	auto fn = std::move(s_onOpened);
	s_onOpened = nullptr;
	if (fn)
		fn(path);
}
extern "C" void zarchive_test_let_go(const zarchive_t* ar)
{
	auto fn = std::move(s_onLetGo);
	s_onLetGo = nullptr;
	if (fn)
		fn(ar);
}

static void TestShared(const fs::path& path, const std::vector<TestFile>& files)
{
	CHECK(ZArchive_SharedCount() == 0);
	zarchive_t* a = ZArchive_AcquireShared(path);
	zarchive_t* b = ZArchive_AcquireShared(path); // not opened again
	CHECK(a && a == b && ZArchive_SharedCount() == 1);
	ZArchive_ReleaseShared(a);
	CHECK(ZArchive_SharedCount() == 1 && LookUp(b, files[0].path));
	ZArchive_ReleaseShared(b);
	CHECK(ZArchive_SharedCount() == 0);
	CHECK(ZArchive_AcquireShared(fs::temp_directory_path() / "parallel_gx2_no_such.wua") == nullptr);
	CHECK(ZArchive_SharedCount() == 0);

	std::atomic<int> bad{0};
	std::vector<std::thread> threads;
	zarchive_t* held = ZArchive_AcquireShared(path); // one kept throughout
	for (int t = 0; t < 4; t++)
	{
		threads.emplace_back([&, t]() {
			std::mt19937 rng(t + 11);
			for (int i = 0; i < 50; i++)
			{
				zarchive_t* z = ZArchive_AcquireShared(path);
				if (!z || z != held || ZArchive_SharedCount() != 1)
					bad++;
				if (z && !ReadsBack(z, files[rng() % files.size()], rng))
					bad++;
				ZArchive_ReleaseShared(z);
			}
		});
	}
	for (std::thread& t : threads)
		t.join();
	ZArchive_ReleaseShared(held);
	CHECK(bad == 0 && ZArchive_SharedCount() == 0);
	// and with none held between: opened and deleted as they come and go
	threads.clear();
	for (int t = 0; t < 4; t++)
	{
		threads.emplace_back([&, t]() {
			std::mt19937 rng(t + 21);
			for (int i = 0; i < 20; i++)
			{
				zarchive_t* z = ZArchive_AcquireShared(path);
				if (!z || ZArchive_SharedCount() != 1)
					bad++;
				if (z && !ReadsBack(z, files[rng() % files.size()], rng))
					bad++;
				ZArchive_ReleaseShared(z);
			}
		});
	}
	for (std::thread& t : threads)
		t.join();
	CHECK(bad == 0 && ZArchive_SharedCount() == 0);

	// Two open it at once: the second to put it in the table finds the
	// first's there, shares that one, and closes its own
	{
		zarchive_t* other = nullptr;
		s_onOpened = [&](const char*) { other = ZArchive_AcquireShared(path); };
		zarchive_t* mine = ZArchive_AcquireShared(path);
		CHECK(other && mine == other && ZArchive_SharedCount() == 1);
		ZArchive_ReleaseShared(mine);
		CHECK(ZArchive_SharedCount() == 1 && LookUp(other, files[0].path));
		ZArchive_ReleaseShared(other);
		CHECK(ZArchive_SharedCount() == 0);
	}
	// Asked for while its last holder lets it go, and before it leaves the
	// table: not taken back from the one closing it, but opened anew
	{
		zarchive_t* first = ZArchive_AcquireShared(path);
		zarchive_t* again = nullptr;
		size_t during = 99;
		s_onLetGo = [&](const zarchive_t*) {
			again = ZArchive_AcquireShared(path);
			during = ZArchive_SharedCount();
		};
		ZArchive_ReleaseShared(first);
		CHECK(again && again != first && during == 1);
		CHECK(ZArchive_SharedCount() == 1 && LookUp(again, files[1].path));
		ZArchive_ReleaseShared(again);
		CHECK(ZArchive_SharedCount() == 0);
	}
}

int main()
{
	TestMetaXml();
	TestUnordered();
	std::vector<TestFile> files;
	const fs::path path = MakeArchive(files);
	for (bool map : {true, false})
	{
		ZArchivePtr zar = Open(path, map);
		CHECK(zar != nullptr);
		if (!zar)
			continue;
		CHECK(zarchive_is_mapped(zar.get()) == map);
		CHECK(zarchive_size(zar.get()) > 0 && std::string(zarchive_path(zar.get())) == path.string());
		TestIndex(zar.get(), files);
		TestReads(zar.get(), files);
		CHECK(zarchive_readers_made(zar.get()) == 1);
		// one thread: the stream it was opened with serves every read
		CHECK(zarchive_streams_opened(zar.get()) == (map ? 0u : 1u));
		TestThreads(zar.get(), files);
		CHECK(zarchive_readers_made(zar.get()) <= 4);
		// mapped, every octet came from the pages; else no more streams
		// than threads, shared by every file
		CHECK(map ? zarchive_streams_opened(zar.get()) == 0 : zarchive_streams_opened(zar.get()) <= 4);
	}
	TestShared(path, files);
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
