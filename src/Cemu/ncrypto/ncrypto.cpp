#include "Common/precompiled.h"
#include "Cemu/ncrypto/ncrypto.h"
#include "util/helpers/helpers.h"
#include "config/CemuConfig.h"

#include "util/crypto/base64_codec.h"

void iosuCrypto_getDeviceCertificate(void* certOut, sint32 len);
bool iosuCrypto_getDeviceId(uint32* deviceId);
void iosuCrypto_getDeviceSerialString(char* serialString);

void iosuCrypto_readSeepromData(void* output, sint32 wordIndex, sint32 size);

extern bool hasSeepromMem; // remove later (migrate otp/seeprom loading & parsing to this class)

namespace NCrypto
{
	// through the C (base64_codec.c), which decodes as these always did
	std::string base64Encode(const void* inputMem, size_t inputLen)
	{
		std::string out(B64_ENCODED_SIZE(inputLen), '\0');
		out.resize(b64_encode(out.data(), inputMem, inputLen));
		return out;
	}

	std::vector<uint8> base64Decode(std::string_view inputStr)
	{
		std::vector<uint8> out(b64_decoded_size(inputStr.data(), inputStr.size()));
		out.resize(b64_decode(out.data(), inputStr.data(), inputStr.size()));
		return out;
	}

	/* Certificate */

	std::string CertECC::encodeToBase64()
	{
		return base64Encode(this, sizeof(CertECC));
	}

	CertECC CertECC::GetDeviceCertificate()
	{
		CertECC deviceCert{};
		iosuCrypto_getDeviceCertificate(&deviceCert, sizeof(CertECC));
		return deviceCert;
	}

	uint32 GetDeviceId()
	{
		uint32 deviceId;
		if (!iosuCrypto_getDeviceId(&deviceId))
			return 0x11223344;
		return deviceId;
	}

	std::string GetSerial()
	{
		char serialBuffer[128]{};
		iosuCrypto_getDeviceSerialString(serialBuffer);
		return serialBuffer;
	}

	bool SEEPROM_IsPresent()
	{
		return hasSeepromMem;
	}

	CafeConsoleRegion SEEPROM_GetRegion()
	{
		uint8 seepromRegionU32[4] = {};
		iosuCrypto_readSeepromData(seepromRegionU32, 0xA4, 4);
		if (seepromRegionU32[3] == 0)
		{
			cemuLog_log(LogType::Force, "SEEPROM region is invalid (0)");
		}
		return (CafeConsoleRegion)seepromRegionU32[3];
	}

	std::string GetRegionAsString(CafeConsoleRegion regionCode)
	{
		if (regionCode == CafeConsoleRegion::EUR)
			return "EUR";
		else if (regionCode == CafeConsoleRegion::USA)
			return "USA";
		else if (regionCode == CafeConsoleRegion::JPN)
			return "JPN";
		else if (regionCode == CafeConsoleRegion::CHN)
			return "CHN";
		else if (regionCode == CafeConsoleRegion::KOR)
			return "KOR";
		else if (regionCode == CafeConsoleRegion::TWN)
			return "TWN";
		cemuLog_log(LogType::Force, "Unknown region code 0x{:x}", (uint32)regionCode);
		return "UKN";
	}

	const std::unordered_map<sint32, const char*> g_countryTable = {
		{1,"JP"},
		{8,"AI"},
		{9,"AG"},
		{10,"AR"},
		{11,"AW"},
		{12,"BS"},
		{13,"BB"},
		{14,"BZ"},
		{15,"BO"},
		{16,"BR"},
		{17,"VG"},
		{18,"CA"},
		{19,"KY"},
		{20,"CL"},
		{21,"CO"},
		{22,"CR"},
		{23,"DM"},
		{24,"DO"},
		{25,"EC"},
		{26,"SV"},
		{27,"GF"},
		{28,"GD"},
		{29,"GP"},
		{30,"GT"},
		{31,"GY"},
		{32,"HT"},
		{33,"HN"},
		{34,"JM"},
		{35,"MQ"},
		{36,"MX"},
		{37,"MS"},
		{38,"AN"},
		{39,"NI"},
		{40,"PA"},
		{41,"PY"},
		{42,"PE"},
		{43,"KN"},
		{44,"LC"},
		{45,"VC"},
		{46,"SR"},
		{47,"TT"},
		{48,"TC"},
		{49,"US"},
		{50,"UY"},
		{51,"VI"},
		{52,"VE"},
		{64,"AL"},
		{65,"AU"},
		{66,"AT"},
		{67,"BE"},
		{68,"BA"},
		{69,"BW"},
		{70,"BG"},
		{71,"HR"},
		{72,"CY"},
		{73,"CZ"},
		{74,"DK"},
		{75,"EE"},
		{76,"FI"},
		{77,"FR"},
		{78,"DE"},
		{79,"GR"},
		{80,"HU"},
		{81,"IS"},
		{82,"IE"},
		{83,"IT"},
		{84,"LV"},
		{85,"LS"},
		{86,"LI"},
		{87,"LT"},
		{88,"LU"},
		{89,"MK"},
		{90,"MT"},
		{91,"ME"},
		{92,"MZ"},
		{93,"NA"},
		{94,"NL"},
		{95,"NZ"},
		{96,"NO"},
		{97,"PL"},
		{98,"PT"},
		{99,"RO"},
		{100,"RU"},
		{101,"RS"},
		{102,"SK"},
		{103,"SI"},
		{104,"ZA"},
		{105,"ES"},
		{106,"SZ"},
		{107,"SE"},
		{108,"CH"},
		{109,"TR"},
		{110,"GB"},
		{111,"ZM"},
		{112,"ZW"},
		{113,"AZ"},
		{114,"MR"},
		{115,"ML"},
		{116,"NE"},
		{117,"TD"},
		{118,"SD"},
		{119,"ER"},
		{120,"DJ"},
		{121,"SO"},
		{122,"AD"},
		{123,"GI"},
		{124,"GG"},
		{125,"IM"},
		{126,"JE"},
		{127,"MC"},
		{128,"TW"},
		{136,"KR"},
		{144,"HK"},
		{145,"MO"},
		{152,"ID"},
		{153,"SG"},
		{154,"TH"},
		{155,"PH"},
		{156,"MY"},
		{160,"CN"},
		{168,"AE"},
		{170,"EG"},
		{171,"OM"},
		{172,"QA"},
		{173,"KW"},
		{174,"SA"},
		{175,"SY"},
		{176,"BH"},
		{177,"JO"},
		{184,"SM"},
		{185,"VA"},
		{186,"BM"},
		{187,"IN"},
		{192,"NG"},
		{193,"AO"},
		{194,"GH"}
	};

	const char* GetCountryAsString(sint32 index)
	{
		const auto it = g_countryTable.find(index);
		if (it == g_countryTable.cend())
			return "NN";
		return it->second;
	}

	size_t GetCountryCount()
	{
		return g_countryTable.size();
	}

};
