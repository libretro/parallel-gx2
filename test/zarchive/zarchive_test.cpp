// The core's Wii U archive reader (ZArchiveVFS), over archives libretro-common's
// zar writer makes:
//  - lookups and directory listings come from the index read on opening, with
//    no file I/O after it, any case, files and directories, the root included;
//  - every file reads back whole, in chunks and at random places, through
//    blocks kept as they are and compressed ones;
//  - threads share one archive, each with its own ZArchiveFile, and read and
//    look up at once (run under ThreadSanitizer by CI);
//  - an archive that is not one, or is cut short, does not open, and says
//    what it found instead (the reason the user is shown);
//  - a title's meta.xml as Breath of the Wild's (a BOM, twelve languages,
//    Japanese) parses, and one that will not says why: not XML (where),
//    no <menu>, no title_id, empty;
//  - a directory out of order with a name in it twice, as Breath of the
//    Wild's .wua holds its meta/ (meta.xml first and last, the rest unsorted
//    between): every name is found, the first of the two the one a lookup
//    gives, as Cemu's reader takes it.
// VFSFileStream is the stdio one below, counting what the reader opens and
// reads, in place of the frontend's VFS.
#include "Cafe/Filesystem/ZArchiveVFS.h"
#include "Cafe/TitleList/ParsedMetaXml.h"

#include <atomic>
#include <cstdio>
#include <cstring>
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

	std::unique_ptr<ZArchive> zar(ZArchive::Open(path));
	CHECK(zar != nullptr);
	if (zar)
	{
		std::optional<ZArchive::Node> n = zar->LookUp(std::string(meta) + "META.XML");
		CHECK(n && n->size == xml.size());
		if (n)
		{
			std::vector<uint8> buf(n->size);
			ZArchiveFile file(*zar, *n);
			CHECK(file.Read(0, buf.data(), buf.size()) == xml.size());
			std::unique_ptr<ParsedMetaXml> parsed(ParsedMetaXml::Parse(buf.data(), buf.size()));
			CHECK(parsed && parsed->GetTitleId() == 0x00050000101C9400ull);
		}
		// bootTvTex.tga is no more; every other name is found
		CHECK(!zar->LookUp(std::string(meta) + "bootTvTex.tga"));
		bool all = true;
		for (const auto& f : files)
			if (f.first != "bootTvTex.tga")
			{
				n = zar->LookUp(meta + f.first);
				all = all && n && n->size == f.second.size();
			}
		CHECK(all);
		std::vector<ZArchive::DirEntry> entries;
		CHECK(zar->ListDir(std::string(meta), entries) && entries.size() == files.size());
		CHECK(entries.size() == files.size() && entries.front().name == "USK_de.jpg" && entries.back().name == "meta.xml");
	}
	zar.reset();
	fs::remove(path);
}

int main()
{
	TestMetaXml();
	TestUnordered();
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
