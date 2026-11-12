// The C++ walk over the C ini reader (IniParser.h): sections in order,
// FindOption, GetAllOptions, and a parser read past its last section.
#include <cstdio>
#include <string>
#include <vector>

#include "util/IniParser/IniParser.h"

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); failures++; } } while (0)

int main()
{
	const std::string text = "[Definition]\nname = \"Pack\"\n$a = 1\n\n[Preset]\nNAME=x\n$b:int=2\n$c = 3\n[Empty]\n";
	IniParser ini(std::span<const char>(text.data(), text.size()), "rules.txt");
	// before the first NextSection there is no section
	CHECK(ini.GetCurrentSectionName().empty() && ini.GetCurrentSectionLineNumber() == 0);
	CHECK(!ini.FindOption("name"));
	CHECK(!(ini.GetAllOptions().begin() != ini.GetAllOptions().end()));

	CHECK(ini.NextSection() && ini.GetCurrentSectionName() == "Definition" && ini.GetCurrentSectionLineNumber() == 1);
	CHECK(ini.FindOption("Name") && *ini.FindOption("Name") == "Pack");
	CHECK(!ini.FindOption("$b:int"));

	CHECK(ini.NextSection() && ini.GetCurrentSectionName() == "Preset" && ini.GetCurrentSectionLineNumber() == 4);
	std::vector<std::pair<std::string, std::string>> all;
	for (const auto& itr : ini.GetAllOptions())
		all.emplace_back(itr.first, itr.second);
	CHECK(all.size() == 3);
	CHECK(all.size() == 3 && all[0].first == "NAME" && all[1].first == "$b:int" && all[1].second == "2" && all[2].second == "3");

	CHECK(ini.NextSection() && ini.GetCurrentSectionName() == "Empty");
	CHECK(!(ini.GetAllOptions().begin() != ini.GetAllOptions().end()));
	CHECK(!ini.NextSection() && !ini.NextSection());
	CHECK(ini.GetCurrentSectionName().empty() && !ini.FindOption("name"));

	// unsigned octets, as a file's data comes
	const std::vector<unsigned char> bytes = {'[', 'm', 'e', 'n', 'u', ']', '\n', 'a', '=', 'b'};
	IniParser menu{std::span<const unsigned char>(bytes)};
	CHECK(menu.NextSection() && menu.FindOption("A") == std::string_view("b"));

	// none at all
	IniParser none(std::span<const char>{});
	CHECK(!none.NextSection() && none.GetCurrentSectionName().empty());

	if (failures)
	{
		std::fprintf(stderr, "ini_parser_test: %d failed\n", failures);
		return 1;
	}
	std::printf("ini_parser_test: all passed\n");
	return 0;
}
