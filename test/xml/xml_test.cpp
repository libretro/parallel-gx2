// The core's XML layer (src/util/xml/Xml.h over libretro-common's rxml,
// which replaced pugixml and tinyxml2), for what its users rely on:
//  - reading: text, attributes, children by name, siblings, a descendant
//    anywhere below, and every lookup on a node that is not there giving
//    nothing or "" rather than failing; numbers read as pugixml read them;
//  - a document that is not well-formed is refused, with where it broke;
//  - a document written and read back is the same tree, escapes included,
//    and children put first (saveinfo.xml's accounts) stay in order;
//  - the settings files (XMLConfigParser): what Save writes Load reads back,
//    attributes set after children included, and a file as tinyxml2 wrote
//    it reads as it did.
#include "util/xml/Xml.h"
#include "config/XMLConfig.h"

#include <cstdio>
#include <cstring>
#include <string>

static int s_failures = 0;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); s_failures++; } } while (0)

static void TestRead()
{
	static const char doc[] =
		"\xef\xbb\xbf<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
		"<!-- a comment -->\n"
		"<menu type=\"complex\" access=\"777\">\n"
		"  <version type=\"unsignedInt\" length=\"4\">33</version>\n"
		"  <title_id type=\"hexBinary\" length=\"8\">0005000010145D00</title_id>\n"
		"  <longname_en type=\"string\" length=\"512\">The Legend of Zelda\n"
		"Breath of the Wild &amp; more</longname_en>\n"
		"  <empty>   </empty>\n"
		"  <hex>0x1F</hex><neg> -12</neg><yes>true</yes><no>0</no>\n"
		"  <list><item>a</item><other/><item>b</item><item>c</item></list>\n"
		"  <deep><er><est>here</est></er></deep>\n"
		"</menu>\n";
	xml::Document d;
	CHECK(d.load(doc, sizeof(doc) - 1));
	xml::Node menu = d.child("menu");
	CHECK(menu && d.document_element() == menu);
	CHECK(!strcmp(menu.name(), "menu"));
	CHECK(menu.attribute("access").as_uint() == 777);
	CHECK(menu.child("version").text().as_uint() == 33);
	CHECK(!strcmp(menu.child("version").attribute("type").value(), "unsignedInt"));
	CHECK(std::stoull(menu.child_value("title_id"), nullptr, 16) == 0x0005000010145D00ull);
	CHECK(!strcmp(menu.child_value("longname_en"), "The Legend of Zelda\nBreath of the Wild & more"));
	// whitespace alone is no text; a parent's indentation is none either
	CHECK(!strcmp(menu.child_value("empty"), "") && !menu.child("empty").text());
	CHECK(!strcmp(menu.child_value(), ""));
	CHECK(menu.child("empty").text().as_uint(7) == 7);
	CHECK(menu.child("hex").text().as_uint() == 0x1F);
	CHECK(menu.child("neg").text().as_int() == -12);
	CHECK(menu.child("yes").text().as_bool() && !menu.child("no").text().as_bool(true));
	std::string items;
	for (xml::Node item : menu.child("list").children("item"))
		items += item.child_value();
	CHECK(items == "abc");
	int all = 0;
	for (xml::Node n : menu.child("list"))
		all += n ? 1 : 0;
	CHECK(all == 4);
	xml::Node first = menu.child("list").child("item");
	CHECK(!strcmp(first.next_sibling("item").child_value(), "b"));
	CHECK(!strcmp(first.next_sibling().name(), "other"));
	CHECK(!strcmp(first.next_sibling("item").next_sibling("item").child_value(), "c"));
	CHECK(!first.next_sibling("item").next_sibling("item").next_sibling("item"));
	CHECK(first.parent() == menu.child("list"));
	CHECK(!strcmp(d.find_descendant("est").child_value(), "here"));
	CHECK(!d.find_descendant("nowhere"));
	// nothing is safe to use
	xml::Node none = menu.child("missing");
	CHECK(!none && none.empty());
	CHECK(!strcmp(none.child("x").child("y").child_value(), ""));
	CHECK(!strcmp(none.child_value("x"), "") && !strcmp(none.name(), ""));
	CHECK(!none.attribute("a") && !strcmp(none.attribute("a").value(), ""));
	CHECK(none.attribute("a").as_uint(5) == 5 && none.text().as_int(-1) == -1);
	CHECK(!strcmp(none.attribute("a").as_string("def"), "def"));
	CHECK(!none.next_sibling() && !none.first_child() && !none.find_descendant("x"));
	CHECK(none.children().begin() == none.children().end());
	CHECK(!none.append_child("x"));
}

static void TestRefuse()
{
	xml::Document d;
	const std::string good = "<a><b>text</b></a>";
	CHECK(d.load(good));
	CHECK(!d.load(good.substr(0, good.size() / 2)));
	CHECK(d.error_offset() > 0);
	// a failed load leaves nothing of the previous document
	CHECK(!d.child("a"));
	CHECK(!d.load(""));
	CHECK(!d.load("<a></b>"));
	CHECK(!d.load("<a/><b/>"));
	CHECK(!d.load("not xml"));
	CHECK(d.save().empty());
}

static void TestWrite()
{
	xml::Document d;
	xml::Node root = d.append_child("info");
	xml::Node a = root.append_child("account");
	a.append_attribute("persistentId") = "80000001";
	a.append_child("timestamp").text().set("1234");
	// put first, as saveinfo.xml's newest account is
	xml::Node b = root.prepend_child("account");
	b.append_attribute("persistentId").set_value("80000002");
	b.append_child("timestamp").text() = "5678";
	root.append_child("odd").text().set("<&>\"' \t\r\n\x01 end");
	root.append_child("attr").append_attribute("v") = "a\"b\tc\nd&e<f";
	root.append_child("utf8").text().set("\xc3\xa9\xe3\x82\xbc\xe3\x83\xab\xe3\x83\x80");
	root.append_child("nothing");
	CHECK(root.first_child() == b && b.next_sibling() == a && a.next_sibling("account").empty());
	CHECK(root.child("account") == b);

	for (const char* indent : {"\t", "    ", (const char*)nullptr})
	{
		const std::string out = d.save(indent);
		CHECK(out.rfind("<?xml version=\"1.0\" encoding=\"UTF-8\"?>", 0) == 0);
		xml::Document r;
		CHECK(r.load(out));
		xml::Node info = r.child("info");
		std::string ids;
		for (xml::Node acc : info.children("account"))
			ids += std::string(acc.attribute("persistentId").value()) + "=" + acc.child_value("timestamp") + ";";
		CHECK(ids == "80000002=5678;80000001=1234;");
		CHECK(!strcmp(info.child_value("odd"), "<&>\"' \t\r\n\x01 end"));
		CHECK(!strcmp(info.child("attr").attribute("v").value(), "a\"b\tc\nd&e<f"));
		CHECK(!strcmp(info.child_value("utf8"), "\xc3\xa9\xe3\x82\xbc\xe3\x83\xab\xe3\x83\x80"));
		CHECK(info.child("nothing") && !strcmp(info.child_value("nothing"), ""));
	}
	CHECK(d.save("\t", false).rfind("<info>", 0) == 0);
}

enum class TestEnum : sint32 { A = 3, B = 7 };

struct TestSettings
{
	bool flag = false;
	float scale = 0.0f;
	sint32 signedValue = 0;
	uint32 unsignedValue = 0;
	uint64 big = 0;
	TestEnum e = TestEnum::A;
	std::string name;
	std::array<uint8, 4> key{};
	std::vector<std::pair<std::string, sint32>> entries;
	sint32 version = 0;

	XMLConfigParser Save(XMLConfigParser& parser)
	{
		auto content = parser.set("content");
		content.set("flag", flag);
		content.set("scale", scale);
		content.set("signed", signedValue);
		content.set("unsigned", unsignedValue);
		content.set("big", big);
		content.set("enum", e);
		content.set("name", name.c_str());
		content.set("key", key);
		auto list = content.set("list");
		for (auto& [n, v] : entries)
		{
			auto entry = list.set("entry");
			entry.set_attribute("name", n);
			entry.set_attribute("value", v);
		}
		// an attribute set after its element's children
		content.set_attribute("version", version);
		return content;
	}

	XMLConfigParser Load(XMLConfigParser& parser)
	{
		auto content = parser.get("content");
		flag = content.get("flag", false);
		scale = content.get("scale", 0.0f);
		signedValue = content.get("signed", (sint32)0);
		unsignedValue = content.get("unsigned", (uint32)0);
		big = content.get("big", (uint64)0);
		e = content.get("enum", TestEnum::A);
		name = content.get("name", std::string("none"));
		content.get("key", key);
		entries.clear();
		auto list = content.get("list");
		for (auto entry = list.get("entry"); entry.valid(); entry = list.get("entry", entry))
			entries.emplace_back(entry.get_attribute("name", std::string()), entry.get_attribute("value", (sint32)0));
		version = content.get_attribute("version", (sint32)0);
		return content;
	}
};

static void TestConfig()
{
	TestSettings out;
	out.flag = true;
	out.scale = 1.5f;
	out.signedValue = -42;
	out.unsignedValue = 4000000000u;
	out.big = 1234567890123ull;
	out.e = TestEnum::B;
	out.name = "Mii & <me>";
	out.key = {0xde, 0xad, 0x0b, 0xef};
	out.entries = {{"one", 1}, {"two", -2}, {"three", 3}};
	out.version = 9;
	xml::Document d;
	XMLConfigParser saver(&d);
	out.Save(saver);
	const std::string text = d.save("    ");

	xml::Document r;
	CHECK(r.load(text));
	XMLConfigParser loader(&r);
	TestSettings in;
	in.Load(loader);
	CHECK(in.flag && in.scale == 1.5f && in.signedValue == -42 && in.unsignedValue == 4000000000u);
	CHECK(in.big == 1234567890123ull && in.e == TestEnum::B && in.name == "Mii & <me>");
	CHECK(in.key == out.key);
	CHECK(in.entries == out.entries);
	CHECK(in.version == 9);

	// a settings file as tinyxml2 wrote it, and values it read: 1 for true,
	// an absent value its default
	static const char tinyxml2File[] =
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		"<content version=\"3\">\n"
		"    <flag>1</flag>\n"
		"    <scale>0.25</scale>\n"
		"    <signed>17</signed>\n"
		"    <name>From tinyxml2</name>\n"
		"    <list>\n"
		"        <entry name=\"x\" value=\"5\"/>\n"
		"    </list>\n"
		"</content>\n";
	xml::Document t;
	CHECK(t.load(tinyxml2File, sizeof(tinyxml2File) - 1));
	XMLConfigParser tl(&t);
	TestSettings old;
	old.Load(tl);
	CHECK(old.flag && old.scale == 0.25f && old.signedValue == 17 && old.unsignedValue == 0);
	CHECK(old.name == "From tinyxml2" && old.e == TestEnum::A && old.version == 3);
	CHECK(old.entries.size() == 1 && old.entries[0].first == "x" && old.entries[0].second == 5);

	// an empty document: every value its default
	xml::Document empty;
	XMLConfigParser el(&empty);
	TestSettings none;
	none.Load(el);
	CHECK(!none.flag && none.name == "none" && none.entries.empty());
}

int main()
{
	TestRead();
	TestRefuse();
	TestWrite();
	TestConfig();
	if (s_failures)
	{
		fprintf(stderr, "%d failed\n", s_failures);
		return 1;
	}
	printf("xml: all passed\n");
	return 0;
}
