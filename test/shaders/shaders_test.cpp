// The Vulkan renderer's shader compiler: rslc, handed GLSL the way
// RendererShaderVk hands it (RslcGlsl.h), must compile
//  - the core's own shaders, read out of the sources that build them: the
//    output shaders (vertex stage and each filter after the fragment
//    preamble) and the surface copy shaders;
//  - the shapes the decompiler writes for Vulkan: its header macros, the
//    redeclared gl_PerVertex, the 32 bit round-to-even mode, every sampler
//    kind it declares, shadow sampling, a geometry stage's blocks, and the
//    rect geometry stage the pipeline compiler generates;
//  - every graphic pack shader the core ships for Vulkan (a pack whose rules
//    say rendererFilter = opengl is never loaded there), its preset variables
//    set to 1, save the few the packs themselves get wrong (glslang refuses
//    them too).
// Each module is written to the directory given, for spirv-val.
//
// The shader/PSO gate (test/pso) builds this file with SHADERS_CORPUS_ONLY
// and takes every module through s_onModule: the corpus is the same one.
#include "Cafe/HW/Latte/Renderer/Vulkan/RslcGlsl.h"

#include <gfx/shaders/rslc.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int s_failures = 0;
static int s_compiled = 0;
static std::string s_out;

// The widths of the module's RoundingModeRTE execution modes, as a mask of
// 16, 32 and 64 (bits 0, 1, 2)
static unsigned s_lastRte;

// Each module compiled: its name, its words and how long rslc took
static void (*s_onModule)(const std::string& name, const uint32_t* words, size_t count, double micros);

static unsigned RteWidths(const uint32_t* w, size_t n)
{
	unsigned mask = 0;
	for (size_t i = 5; i < n && (w[i] >> 16);)
	{
		const uint32_t len = w[i] >> 16;
		if ((w[i] & 0xffff) == 16 /* OpExecutionMode */ && len == 4 && w[i + 2] == 4462)
			mask |= w[i + 3] == 16 ? 1u : w[i + 3] == 32 ? 2u : 4u;
		i += len;
	}
	return mask;
}

static bool Compile(const std::string& name, const std::string& glsl, rslc_stage stage, bool report = true, bool rte32 = true)
{
	const std::string src = RslcGlsl(glsl, rte32);
	char err[512];
	err[0] = '\0';
	rslc_opts opts{};
	opts.dst = RSLC_DST_SPIRV;
	opts.err = err;
	opts.err_cap = sizeof(err);
	rslc_result result{};
	const auto start = std::chrono::steady_clock::now();
	const bool compiled = rslc_compile(src.data(), src.size(), stage, &opts, &result);
	const double micros = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
	if (!compiled)
	{
		if (report)
		{
			std::fprintf(stderr, "FAIL %s: %s\n", name.c_str(), err);
			s_failures++;
		}
		return false;
	}
	s_compiled++;
	s_lastRte = RteWidths(result.spirv, result.spirv_len);
	if (s_onModule)
		s_onModule(name, result.spirv, result.spirv_len, micros);
	if (!s_out.empty())
	{
		std::string file = name;
		for (char& c : file)
			if (c == '/' || c == '\\' || c == ' ')
				c = '_';
		std::ofstream(s_out + "/" + file + ".spv", std::ios::binary)
			.write((const char*)result.spirv, (std::streamsize)(result.spirv_len * 4));
	}
	rslc_result_free(&result, &opts);
	return true;
}

static std::string ReadFile(const fs::path& path)
{
	std::ifstream f(path, std::ios::binary);
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

// --- the core's own shaders, out of their sources ---------------------------

// The raw string R"(...)" that follows @after in @src
static std::string RawString(const std::string& src, const std::string& after)
{
	size_t at = src.find(after);
	if (at == std::string::npos || (at = src.find("R\"(", at)) == std::string::npos)
		return {};
	size_t end = src.find(")\"", at + 3);
	return end == std::string::npos ? std::string() : src.substr(at + 3, end - at - 3);
}

// The run of "..." literals after @after up to the ';', unescaped; lines
// commented out between them are left out
static std::string StringLiterals(const std::string& src, const std::string& after)
{
	size_t at = src.find(after);
	if (at == std::string::npos)
		return {};
	at += after.size();
	std::string out;
	while (at < src.size() && src[at] != ';')
	{
		if (src.compare(at, 2, "//") == 0)
		{
			at = src.find('\n', at);
			continue;
		}
		if (src[at] != '"')
		{
			at++;
			continue;
		}
		for (at++; at < src.size() && src[at] != '"'; at++)
		{
			if (src[at] != '\\')
			{
				out.push_back(src[at]);
				continue;
			}
			char e = src[++at];
			out.push_back(e == 'n' ? '\n' : e == 'r' ? '\r' : e == 't' ? '\t' : e);
		}
		at++;
	}
	return out;
}

static void TestCoreShaders(const fs::path& srcDir)
{
	const std::string out = ReadFile(srcDir / "Cafe/HW/Latte/Renderer/RendererOuputShader.cpp");
	const std::string vkVertex = out.substr(out.find("RendererOutputShader::GetVulkanVertexSource"));
	// the vertex stage: its head, one of the two vertex tables, its tail
	std::vector<std::string> parts;
	for (size_t at = 0; (at = vkVertex.find("R\"(", at)) != std::string::npos && parts.size() < 4;)
	{
		size_t end = vkVertex.find(")\"", at);
		parts.push_back(vkVertex.substr(at + 3, end - at - 3));
		at = end;
	}
	if (parts.size() != 4)
	{
		std::fprintf(stderr, "FAIL: the output vertex stage's source not found\n");
		s_failures++;
		return;
	}
	Compile("output_vertex", parts[0] + parts[1] + parts[3], RSLC_STAGE_VERTEX);
	Compile("output_vertex_upside_down", parts[0] + parts[2] + parts[3], RSLC_STAGE_VERTEX);

	const std::string preamble = RawString(out, "RendererOutputShader::PrependFragmentPreamble");
	for (const char* filter : { "s_copy_shader_source =", "s_bicubic_shader_source =", "s_hermite_shader_source =" })
	{
		const std::string body = RawString(out, filter);
		if (preamble.empty() || body.empty())
		{
			std::fprintf(stderr, "FAIL: the output shader %s not found\n", filter);
			s_failures++;
			continue;
		}
		Compile(std::string("output_") + std::string(filter).substr(2, 6), preamble + body, RSLC_STAGE_FRAGMENT);
	}

	const std::string copy = ReadFile(srcDir / "Cafe/HW/Latte/Renderer/Vulkan/VulkanSurfaceCopy.cpp");
	const std::string vs = StringLiterals(copy, "const char* vsShaderSrc =");
	const std::string c2d = StringLiterals(copy.substr(copy.find("_vkGenSurfaceCopyShader_ps_colorToDepth")), "const char* psShaderSrc =");
	const std::string d2c = StringLiterals(copy.substr(copy.find("_vkGenSurfaceCopyShader_ps_depthToColor")), "const char* psShaderSrc =");
	if (vs.find("#version") == std::string::npos || c2d.find("gl_FragDepth") == std::string::npos
		|| d2c.find("colorOut0") == std::string::npos)
	{
		std::fprintf(stderr, "FAIL: the surface copy shaders' sources not found\n");
		s_failures++;
		return;
	}
	Compile("surface_copy_vertex", vs, RSLC_STAGE_VERTEX);
	Compile("surface_copy_color_to_depth", c2d, RSLC_STAGE_FRAGMENT);
	Compile("surface_copy_depth_to_color", d2c, RSLC_STAGE_FRAGMENT);
}

// --- the shapes the decompiler and the pipeline compiler write -----------

static const char kHeader[] =
	"#version 430\r\n"
	"#extension GL_ARB_texture_gather : enable\r\n"
	"#extension GL_ARB_separate_shader_objects : enable\r\n"
	"#ifdef VULKAN\r\n"
	"#define ATTR_LAYOUT(__location) layout(location = __location)\r\n"
	"#define UNIFORM_BUFFER_LAYOUT(__glLocation, __vkSet, __vkLocation) layout(set = __vkSet, binding = __vkLocation, std140)\r\n"
	"#define TEXTURE_LAYOUT(__glLocation, __vkSet, __vkLocation) layout(set = __vkSet, binding = __vkLocation)\r\n"
	"#define gl_VertexID gl_VertexIndex\r\n"
	"#define gl_InstanceID gl_InstanceIndex\r\n"
	"#define SET_POSITION(_v) gl_Position = _v; gl_Position.z = (gl_Position.z + gl_Position.w) / 2.0\r\n"
	"#define GET_FRAGCOORD() vec4(gl_FragCoord.xy*uf_fragCoordScale.xy,gl_FragCoord.z, 1.0/gl_FragCoord.w)\r\n"
	"#extension GL_EXT_spirv_intrinsics: enable\r\n"
	"spirv_execution_mode(capabilities = [4467], extensions = [\"SPV_KHR_float_controls\"], 4462, 32);\r\n"
	"#else\r\n"
	"#define ATTR_LAYOUT(__location) layout(location = __location)\r\n"
	"#endif\r\n";

static const char kVertex[] =
	"UNIFORM_BUFFER_LAYOUT(0, 0, 0) uniform uniformBlockVS0 { vec4 uf_blockVS0[32]; };\r\n"
	"ATTR_LAYOUT(0) in uvec4 attrDataSem0;\r\n"
	"out gl_PerVertex\r\n"
	"{\r\n"
	"	vec4 gl_Position;\r\n"
	"	float gl_PointSize;\r\n"
	"};\r\n"
	"layout(location = 0) out vec4 passParameterSem0;\r\n"
	"layout(set = 0, binding = 1) buffer StreamoutBuffer\r\n"
	"{\r\n"
	"int sb_buffer[];\r\n"
	"};\r\n"
	"void main()\r\n"
	"{\r\n"
	"ivec4 R1i = ivec4(attrDataSem0);\r\n"
	"vec4 R0f = intBitsToFloat(R1i) * uf_blockVS0[int(attrDataSem0.w) & 31];\r\n"
	"sb_buffer[gl_VertexID] = floatBitsToInt(R0f.x);\r\n"
	"passParameterSem0 = R0f;\r\n"
	"gl_PointSize = 1.0;\r\n"
	"SET_POSITION(vec4(R0f.xyz, 1.0));\r\n"
	"}\r\n";

static const char kFragment[] =
	"layout(set = 1, binding = 1) uniform ufBlock { ivec4 uf_remappedPS[4]; vec4 uf_fragCoordScale; };\r\n"
	"TEXTURE_LAYOUT(0, 1, 0) uniform sampler2D textureUnitPS0;\r\n"
	"TEXTURE_LAYOUT(1, 1, 2) uniform sampler1D textureUnitPS1;\r\n"
	"TEXTURE_LAYOUT(2, 1, 3) uniform sampler2DArray textureUnitPS2;\r\n"
	"TEXTURE_LAYOUT(3, 1, 4) uniform samplerCubeArray textureUnitPS3;\r\n"
	"TEXTURE_LAYOUT(4, 1, 5) uniform sampler3D textureUnitPS4;\r\n"
	"TEXTURE_LAYOUT(5, 1, 6) uniform usampler2D textureUnitPS5;\r\n"
	"TEXTURE_LAYOUT(6, 1, 7) uniform usampler1D textureUnitPS6;\r\n"
	"TEXTURE_LAYOUT(7, 1, 8) uniform sampler2DShadow textureUnitPS7;\r\n"
	"TEXTURE_LAYOUT(8, 1, 9) uniform sampler2DArrayShadow textureUnitPS8;\r\n"
	"TEXTURE_LAYOUT(9, 1, 10) uniform samplerCubeArrayShadow textureUnitPS9;\r\n"
	"TEXTURE_LAYOUT(10, 1, 11) uniform sampler1DShadow textureUnitPS10;\r\n"
	"layout(location = 0) in vec4 passParameterSem0;\r\n"
	"layout(location = 1) flat in vec4 passParameterSem1;\r\n"
	"layout(location = 2) noperspective in vec4 passParameterSem2;\r\n"
	"layout(location = 0) out vec4 passPixelColor0;\r\n"
	"layout(location = 1) out vec4 passPixelColor1;\r\n"
	"vec3 redcCUBEReverse(vec2 st, int faceId) { return vec3(st, float(faceId)); }\r\n"
	"float mul_nonIEEE(float a, float b){ if( a == 0.0 || b == 0.0 ) return 0.0; return a*b; }\r\n"
	"void main()\r\n"
	"{\r\n"
	"vec4 R0f = passParameterSem0;\r\n"
	"vec4 R1f = passParameterSem1 + passParameterSem2;\r\n"
	"float cubeMapArrayIndex3 = 0.0;\r\n"
	"R1f.xyzw = (texture(textureUnitPS0, R0f.xy).xyzw) + textureLod(textureUnitPS0, R0f.xy, R0f.w)\r\n"
	"	+ textureGrad(textureUnitPS0, R0f.xy, R1f.xy, R1f.zw) + textureGather(textureUnitPS0, vec2(0.0001) + R0f.xy).yzxw\r\n"
	"	+ texelFetch(textureUnitPS0, ivec2(R0f.xy), 0) + textureOffset(textureUnitPS0, R0f.xy, ivec2(1, -1));\r\n"
	"R1f += texture(textureUnitPS1, R0f.x) + textureLod(textureUnitPS1, R0f.x, 0.0) + texelFetch(textureUnitPS1, int(R0f.x), 0)\r\n"
	"	+ textureGrad(textureUnitPS1, R0f.x, R1f.x, R1f.y);\r\n"
	"R1f += texture(textureUnitPS2, R0f.xyz) + textureLodOffset(textureUnitPS2, R0f.xyz, 0.0, ivec2(1, 0));\r\n"
	"R1f += texture(textureUnitPS3, vec4(redcCUBEReverse(R0f.xy, int(R0f.z)), cubeMapArrayIndex3))\r\n"
	"	+ textureLod(textureUnitPS3, vec4(redcCUBEReverse(R0f.xy, 2), cubeMapArrayIndex3), R0f.w);\r\n"
	"R1f += texture(textureUnitPS4, R0f.xyz) + textureLod(textureUnitPS4, R0f.xyz, 1.0);\r\n"
	"R1f += uintBitsToFloat(texelFetch(textureUnitPS5, ivec2(0), 0)) + uintBitsToFloat(texelFetch(textureUnitPS6, 0, 0));\r\n"
	"R1f.x += texture(textureUnitPS7, vec3(R0f.xy, R0f.w)) + textureLod(textureUnitPS7, vec3(R0f.xy, R0f.z), 0.0);\r\n"
	"R1f.y += texture(textureUnitPS8, vec4(R0f.xyz, R0f.w)) + texture(textureUnitPS9, vec4(redcCUBEReverse(R0f.xy, 1), 0.0), R0f.w);\r\n"
	"R1f.z += texture(textureUnitPS10, vec3(R0f.x, 0.0, R0f.w));\r\n"
	"{\r\n"
	"int levels = textureQueryLevels(textureUnitPS2);\r\n"
	"ivec4 info = ivec4(textureSize(textureUnitPS3, clamp(0, 0, levels - 1)).xy, 0, levels);\r\n"
	"float lod = textureQueryLod(textureUnitPS4, R0f.xyz).y;\r\n"
	"R1f.w += float(info.x + textureSize(textureUnitPS1, 0)) + lod + float(textureSize(textureUnitPS8, 0).z);\r\n"
	"}\r\n"
	"passPixelColor0 = vec4(mul_nonIEEE(R1f.x, GET_FRAGCOORD().x), R1f.yzw);\r\n"
	"passPixelColor1 = vec4(gl_FrontFacing ? 1.0 : 0.0);\r\n"
	"}\r\n";

static const char kGeometry[] =
	"#define V2G_LAYOUT layout(location = 0)\r\n"
	"layout(points) in;\r\n"
	"layout(triangle_strip, max_vertices = 4) out;\r\n"
	"out gl_PerVertex\r\n"
	"{\r\n"
	"	vec4 gl_Position;\r\n"
	"};\r\n"
	"V2G_LAYOUT in Vertex\r\n"
	"{\r\n"
	" ivec4 passV2GParameter0;\r\n"
	" ivec4 passV2GParameter1;\r\n"
	"}v2g[];\r\n"
	"layout(location = 0) out vec4 passG2PParameter0;\r\n"
	"void main()\r\n"
	"{\r\n"
	"for (int i = 0; i < 4; i++)\r\n"
	"{\r\n"
	"passG2PParameter0 = intBitsToFloat(v2g[0].passV2GParameter1) + vec4(float(gl_PrimitiveIDIn));\r\n"
	"SET_POSITION(intBitsToFloat(v2g[0].passV2GParameter0) + vec4(float(i)));\r\n"
	"EmitVertex();\r\n"
	"}\r\n"
	"EndPrimitive();\r\n"
	"}\r\n";

// As rectsEmulationGS_generate writes it
static const char kRectGeometry[] =
	"#version 450\r\n"
	"layout(triangles) in;\r\n"
	"layout(triangle_strip) out;\r\n"
	"layout(max_vertices = 4) out;\r\n"
	"layout(location = 0) in vec4 passParameterSem0In[];\r\n"
	"layout(location = 1) flat in vec4 passParameterSem1In[];\r\n"
	"layout(location = 0) out vec4 passParameterSem0Out;\r\n"
	"layout(location = 1) flat out vec4 passParameterSem1Out;\r\n"
	"vec4 gen4thVertexA(vec4 a, vec4 b, vec4 c)\r\n"
	"{\r\n"
	"return b - (c - a);\r\n"
	"}\r\n"
	"void main()\r\n"
	"{\r\n"
	"float dist0_1 = length(gl_in[1].gl_Position.xy - gl_in[0].gl_Position.xy);\r\n"
	"float dist0_2 = length(gl_in[2].gl_Position.xy - gl_in[0].gl_Position.xy);\r\n"
	"if(dist0_1 > dist0_2)\r\n"
	"{\r\n"
	"passParameterSem0Out = passParameterSem0In[2];\r\n"
	"passParameterSem1Out = passParameterSem1In[2];\r\n"
	"gl_Position = gl_in[2].gl_Position;\r\n"
	"EmitVertex();\r\n"
	"passParameterSem0Out = gen4thVertexA(passParameterSem0In[0], passParameterSem0In[1], passParameterSem0In[2]);\r\n"
	"gl_Position = gen4thVertexA(gl_in[0].gl_Position, gl_in[1].gl_Position, gl_in[2].gl_Position);\r\n"
	"EmitVertex();\r\n"
	"}\r\n"
	"EndPrimitive();\r\n"
	"}\r\n";

static void TestDecompilerShapes()
{
	Compile("decompiler_vertex", std::string(kHeader) + kVertex, RSLC_STAGE_VERTEX);
	if (s_lastRte != 2)
	{
		std::fprintf(stderr, "FAIL: the 32 bit round-to-even mode, got mask %u\n", s_lastRte);
		s_failures++;
	}
	// a device without it: no mode
	Compile("decompiler_vertex_no_rte", std::string(kHeader) + kVertex, RSLC_STAGE_VERTEX, true, false);
	if (s_lastRte != 0)
	{
		std::fprintf(stderr, "FAIL: no round-to-even mode without the device's, got mask %u\n", s_lastRte);
		s_failures++;
	}
	// what older decompilers wrote, and the graphic packs carry: 32 bit only
	{
		std::string old(kHeader);
		const std::string line = "spirv_execution_mode(capabilities = [4467], extensions = [\"SPV_KHR_float_controls\"], 4462, 32);\r\n";
		old.replace(old.find(line), line.size(),
			"spirv_execution_mode(4462, 16);\r\nspirv_execution_mode(4462, 32);\r\n  spirv_execution_mode( 4462 , 64 );\r\n");
		Compile("pack_rte_modes", old + kFragment, RSLC_STAGE_FRAGMENT);
		if (s_lastRte != 2)
		{
			std::fprintf(stderr, "FAIL: a pack's round-to-even modes, got mask %u\n", s_lastRte);
			s_failures++;
		}
	}
	Compile("decompiler_fragment", std::string(kHeader) + kFragment, RSLC_STAGE_FRAGMENT);
	Compile("decompiler_geometry", std::string(kHeader) + kGeometry, RSLC_STAGE_GEOMETRY);
	Compile("rect_geometry", kRectGeometry, RSLC_STAGE_GEOMETRY);
	// a message's line is the source's
	const std::string bad = std::string(kHeader) + "void main()\r\n{\r\nundeclared = 1;\r\n}\r\n";
	char err[256];
	const std::string src = RslcGlsl(bad, true);
	rslc_opts opts{};
	opts.dst = RSLC_DST_SPIRV;
	opts.err = err;
	opts.err_cap = sizeof(err);
	rslc_result result{};
	const bool ok = rslc_compile(src.data(), src.size(), RSLC_STAGE_FRAGMENT, &opts, &result);
	if (ok || !std::strstr(err, "line 19"))
	{
		std::fprintf(stderr, "FAIL: an error's line, got \"%s\"\n", ok ? "no error" : err);
		s_failures++;
	}
}

// --- the graphic packs -----------------------------------------------------

// Shaders the packs get wrong, which glslang refuses as well
static const char* const kBrokenPacks[] = {
	"src/Bayonetta2/Graphics/78a2659662685d55_0000000000000079_ps.txt",            // unbalanced parentheses
	"src/BreathOfTheWild/Mods/GlowingGuardianColor/4e0bab929e11836f_00003cd249549249_ps.txt", // a missing ';'
	"src/SkylandersTrapTeam/Mods/HUD/35fb394719fd96f4_00000000000003c9_ps.txt",     // R0i undeclared
	"src/SkylandersTrapTeam/Mods/HUD/648d2ae359c1959f_0000000000000079_ps.txt",     // R0i undeclared
	"src/XenobladeChroniclesX/Enhancements/AntiAliasing/59df1c7e1806366c_00000000000003c9_ps.txt", // no main()
};

static void TestGraphicPacks(const fs::path& packs)
{
	// what GraphicPack2::LoadShaders takes for a shader: <hash>_<hash>_<type>
	const std::regex name(R"(^[0-9a-fA-F]+_[0-9a-fA-F]+_(vs|ps|gs).*)");
	const std::regex var(R"(\$[a-zA-Z_0-9]+)");
	const std::regex openglOnly(R"(rendererFilter\s*=\s*opengl)", std::regex::icase);
	int packsCompiled = 0;
	for (const char* group : { "Enhancements", "Resolutions", "Mods", "Workarounds", "src" })
	{
		if (!fs::is_directory(packs / group))
			continue;
		for (const auto& it : fs::recursive_directory_iterator(packs / group))
		{
			const std::string file = it.path().filename().string();
			std::smatch m;
			if (!it.is_regular_file() || !std::regex_match(file, m, name) || file.find("_msl") != std::string::npos)
				continue;
			const std::string rel = fs::relative(it.path(), packs).generic_string();
			if (std::regex_search(ReadFile(it.path().parent_path() / "rules.txt"), openglOnly))
				continue;
			bool broken = false;
			for (const char* b : kBrokenPacks)
				broken = broken || rel == b;
			// preset variables, as ApplyShaderPresets fills them in
			const std::string glsl = std::regex_replace(ReadFile(it.path()), var, "1");
			const rslc_stage stage = m[1] == "vs" ? RSLC_STAGE_VERTEX : m[1] == "gs" ? RSLC_STAGE_GEOMETRY : RSLC_STAGE_FRAGMENT;
			if (Compile(rel, glsl, stage, !broken))
				packsCompiled++;
		}
	}
	std::printf("graphic packs: %d shaders compiled\n", packsCompiled);
	if (packsCompiled < 400)
	{
		std::fprintf(stderr, "FAIL: too few graphic pack shaders (is the submodule checked out?)\n");
		s_failures++;
	}
}

#ifndef SHADERS_CORPUS_ONLY
int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::fprintf(stderr, "usage: shaders_test <cemu_graphic_packs> [spirv out dir]\n");
		return 2;
	}
	if (argc > 2)
		s_out = argv[2];
	TestCoreShaders(fs::path(argv[0]).parent_path() / "../../src");
	TestDecompilerShapes();
	TestGraphicPacks(argv[1]);
	if (s_failures)
	{
		std::printf("shaders_test: %d failure(s)\n", s_failures);
		return 1;
	}
	std::printf("shaders_test: %d modules, all passed\n", s_compiled);
	return 0;
}
#endif
