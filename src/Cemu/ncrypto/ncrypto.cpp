#include "Common/precompiled.h"
#include "Cemu/ncrypto/ncrypto.h"
#include "util/helpers/helpers.h"
#include "config/CemuConfig.h"

#include <lrc_hash.h>
#include <crypto/pk.h>
#include <crypto/kdf.h>

#include "util/crypto/aes128.h"

void iosuCrypto_getDeviceCertificate(void* certOut, sint32 len);
void iosuCrypto_getDeviceCertPrivateKey(void* privKeyOut, sint32 len);
bool iosuCrypto_getDeviceId(uint32* deviceId);
void iosuCrypto_getDeviceSerialString(char* serialString);

void iosuCrypto_readOtpData(void* output, sint32 wordIndex, sint32 size);

void iosuCrypto_readSeepromData(void* output, sint32 wordIndex, sint32 size);

extern bool hasSeepromMem; // remove later (migrate otp/seeprom loading & parsing to this class)
extern bool hasOtpMem; // remove later

namespace NCrypto
{
	std::string base64Encode(const void* inputMem, size_t inputLen)
	{
		const uint8* input = (const uint8*)inputMem;

		static const char* base64_charset =
			"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
			"abcdefghijklmnopqrstuvwxyz"
			"0123456789+/";

		std::string strBase64;
		strBase64.resize((inputLen * 4) / 3 + 16);

		int i = 0;
		int j = 0;
		unsigned char charArray_3[3];
		unsigned char charArray_4[4];
		sint32 outputLength = 0;
		while (inputLen--)
		{
			charArray_3[i++] = *(input++);
			if (i == 3)
			{
				charArray_4[0] = (charArray_3[0] & 0xfc) >> 2;
				charArray_4[1] = ((charArray_3[0] & 0x03) << 4) + ((charArray_3[1] & 0xf0) >> 4);
				charArray_4[2] = ((charArray_3[1] & 0x0f) << 2) + ((charArray_3[2] & 0xc0) >> 6);
				charArray_4[3] = charArray_3[2] & 0x3f;
				for (i = 0; (i < 4); i++)
				{
					strBase64[outputLength] = base64_charset[charArray_4[i]];
					outputLength++;
				}
				i = 0;
			}
		}
		if (i)
		{
			for (j = i; j < 3; j++)
				charArray_3[j] = '\0';

			charArray_4[0] = (charArray_3[0] & 0xfc) >> 2;
			charArray_4[1] = ((charArray_3[0] & 0x03) << 4) + ((charArray_3[1] & 0xf0) >> 4);
			charArray_4[2] = ((charArray_3[1] & 0x0f) << 2) + ((charArray_3[2] & 0xc0) >> 6);
			charArray_4[3] = charArray_3[2] & 0x3f;

			for (j = 0; j < (i + 1); j++)
			{
				strBase64[outputLength] = base64_charset[charArray_4[j]];
				outputLength++;
			}
			while (i++ < 3)
			{
				strBase64[outputLength] = '=';
				outputLength++;
			}
		}

		cemu_assert(outputLength <= strBase64.size());
		strBase64.resize(outputLength);
		return strBase64;
	}

	std::vector<uint8> base64Decode(std::string_view inputStr)
	{
		static constexpr unsigned char kDecodingTable[] = {
		  64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
		  64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
		  64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 62, 64, 64, 64, 63,
		  52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 64, 64, 64, 64, 64, 64,
		  64,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
		  15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 64, 64, 64, 64, 64,
		  64, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
		  41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 64, 64, 64, 64, 64,
		  64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
		  64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
		  64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
		  64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
		  64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
		  64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
		  64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
		  64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64
		};

		size_t in_len = inputStr.size();
		if (in_len <= 3 || (in_len & 3) != 0)
			return std::vector<uint8>(); // invalid length

		std::vector<uint8> output;

		size_t out_len = in_len / 4 * 3;
		if (inputStr[in_len - 1] == '=') out_len--;
		if (inputStr[in_len - 2] == '=') out_len--;

		output.resize(out_len);

		for (size_t i = 0, j = 0; i < in_len;) 
		{
			uint32 a = inputStr[i] == '=' ? 0 & i++ : kDecodingTable[static_cast<uint8>(inputStr[i++])];
			uint32 b = inputStr[i] == '=' ? 0 & i++ : kDecodingTable[static_cast<uint8>(inputStr[i++])];
			uint32 c = inputStr[i] == '=' ? 0 & i++ : kDecodingTable[static_cast<uint8>(inputStr[i++])];
			uint32 d = inputStr[i] == '=' ? 0 & i++ : kDecodingTable[static_cast<uint8>(inputStr[i++])];

			uint32 triple = (a << 3 * 6) + (b << 2 * 6) + (c << 1 * 6) + (d << 0 * 6);

			if (j < out_len) output[j++] = (triple >> 2 * 8) & 0xFF;
			if (j < out_len) output[j++] = (triple >> 1 * 8) & 0xFF;
			if (j < out_len) output[j++] = (triple >> 0 * 8) & 0xFF;
		}

		return output;
	}

	void base64Tests()
	{
		std::vector<uint8> random;
		for (sint32 i = 0; i < 100; i++)
		{
			random.resize(0 + i);
			for (size_t x = 0; x < random.size(); x++)
				random[x] = (uint8)(i * 21 + x * 133);

			std::string b64 = base64Encode(random.data(), random.size());
			std::vector<uint8> dec = base64Decode(b64);

			cemu_assert(random == dec);
		}
	}

	/* Hashing */

	void GenerateHashSHA1(const void* data, size_t len, CHash160& hashOut)
	{
		SHA1Digest((const uint8_t*)data, len, hashOut.b);
	}

	void GenerateHashSHA256(const void* data, size_t len, CHash256& hashOut)
	{
		struct sha256_state st;
		sha256_stream_init(&st, 0);
		sha256_stream_update(&st, (const uint8_t*)data, len);
		sha256_stream_final(&st, hashOut.b);
	}

	/* Title meta data */

	struct TMDFileHeaderWiiU
	{
		/* +0x000 */ uint32be signatureType;

		/* +0x004 */ uint8be sig[0x100];
		/* +0x104 */ uint8be _padding104[0x140 - 0x104];
		/* +0x140 */ uint8be _ukn140[0x40];

		/* +0x180 */ uint8be tmdVersion;
		/* +0x181 */ uint8be _ukn181;
		/* +0x182 */ uint8be _ukn182;
		/* +0x183 */ uint8be isVWii;
		/* +0x184 */ uint32be iosTitleIdHigh;
		/* +0x188 */ uint32be iosTitleIdLow;
		/* +0x18C */ uint32be titleIdHigh;
		/* +0x190 */ uint32be titleIdLow;
		/* +0x194 */ uint32be titleType;
		/* +0x198 */ uint16be group;
		/* +0x19A */ uint16be _ukn19A;
		/* +0x19C */ uint16be region;
		/* +0x19E */ uint8be ratings[16];
		/* +0x1AE */ uint8be _ukn1AE[12];
		/* +0x1BA */ uint8be _ipcMask[12];
		/* +0x1C6 */ uint8be _ukn1C6[18];
		/* +0x1D8 */ uint32be accessRightsMask;
		/* +0x1DC */ uint16be titleVersion;
		/* +0x1DE */ uint16be numContent;
		/* +0x1E0 */ uint32be _ukn1E0;
		/* +0x1E4 */ uint8 uknHash[32]; // hash of array at 0x204

		/* +0x204 */
		struct 
		{
			// pointer to cert data and cert hash?
			uint16 ukn00; // index?
			uint16 ukn02;
			uint8 hash[32];
		}ContentInfo[64];
	};

	static_assert(sizeof(TMDFileHeaderWiiU) == 0x204 + 64*36);

	struct TMDFileContentEntryWiiU
	{
		/* +0x00 */ uint32be contentId;
		/* +0x04 */ uint16be index;
		/* +0x06 */ uint16be type;
		/* +0x08 */ uint32be sizeHigh;
		/* +0x0C */ uint32be sizeLow;
		/* +0x10 */ uint8 hashSHA256[32]; // only the first 20 bytes of the hash seem to be stored?
	};

	static_assert(sizeof(TMDFileContentEntryWiiU) == 0x30);

	bool TMDParser::parse(const uint8* data, size_t size)
	{
		if (size < sizeof(TMDFileHeaderWiiU))
		{
			cemuLog_log(LogType::Force, "TMD size {} below minimum size of {}", size, sizeof(TMDFileHeaderWiiU));
			return false;
		}
		TMDFileHeaderWiiU* header = (TMDFileHeaderWiiU*)data;
		m_titleId = ((uint64)header->titleIdHigh << 32) | ((uint64)header->titleIdLow);
		m_titleVersion = header->titleVersion;
		size_t expectedSize = sizeof(TMDFileHeaderWiiU) + sizeof(TMDFileContentEntryWiiU) * header->numContent;
		if (size < expectedSize)
		{
			cemuLog_log(LogType::Force, "TMD size {} below expected size of {}. Content count: {}", size, expectedSize, (uint16)header->numContent);
			return false;
		}

		// parse content
		TMDFileContentEntryWiiU* contentEntry = (TMDFileContentEntryWiiU*)(header + 1);
		for (uint32 i = 0; i < header->numContent; i++)
		{
			ContentEntry c{};
			c.contentId = contentEntry->contentId;
			c.index = contentEntry->index;
			c.size = MakeU64(contentEntry->sizeHigh, contentEntry->sizeLow);
			c.contentFlags = static_cast<TMDParser::TMDContentFlags>((uint16)contentEntry->type);
			std::memcpy(c.hash32, contentEntry->hashSHA256, sizeof(c.hash32));
			m_content.emplace_back(c);
			contentEntry++;
		}

		// todo - parse certificates
		return true;
	}

	/* ECC PrivateKey helper functions */

	ECCPrivKey ECCPrivKey::generate()
	{
		ECCPrivKey key{};
		uint8 pub[B233_POINT_SIZE];
		do
			crypto_random_bytes(key.keyData, sizeof(key.keyData));
		while (b233_keygen(key.keyData, pub) != 0);
		return key;
	}

	ECCPrivKey ECCPrivKey::getDeviceCertPrivateKey()
	{
		ECCPrivKey key{};
		iosuCrypto_getDeviceCertPrivateKey(key.keyData, sizeof(key.keyData));
		return key;
	}

	/* ECC PublicKey helper functions */

	void ECCPubKey::getPoint(uint8* point) const
	{
		point[0] = 0x04;
		memcpy(point + 1, this->x, sizeof(this->x));
		memcpy(point + 1 + sizeof(this->x), this->y, sizeof(this->y));
	}

	ECCPubKey ECCPubKey::generateFromPrivateKey(ECCPrivKey& privKey)
	{
		uint8 point[B233_POINT_SIZE];
		ECCPubKey genPubKey{};
		if (b233_keygen(privKey.keyData, point) == 0)
		{
			memcpy(genPubKey.x, point + 1, sizeof(genPubKey.x));
			memcpy(genPubKey.y, point + 1 + sizeof(genPubKey.x), sizeof(genPubKey.y));
		}
		return genPubKey;
	}

	/* Certificate */

	bool CertECC::decodeFromBase64(std::string_view input)
	{
		auto v = base64Decode(input);
		if (v.size() != sizeof(CertECC))
			return false;
		memcpy(this, v.data(), sizeof(CertECC));
		return true;
	}

	std::string CertECC::encodeToBase64()
	{
		return base64Encode(this, sizeof(CertECC));
	}

	bool CertECC::verifySignatureViaPubKey(ECCPubKey& signerPubKey)
	{
		CHash256 hash;
		GenerateHashSHA256(this->issuer, 0x100, hash);
		return verifyHashSignature(hash.b, sizeof(hash.b), signerPubKey, this->signature);
	}

	void CertECC::sign(ECCPrivKey& signerPrivKey)
	{
		CHash256 hash;
		GenerateHashSHA256(this->issuer, 0x100, hash);
		b233_ecdsa_sign(signerPrivKey.keyData, hash.b, sizeof(hash.b), this->signature.r, this->signature.s);
	}

	CertECC CertECC::GetDeviceCertificate()
	{
		CertECC deviceCert{};
		iosuCrypto_getDeviceCertificate(&deviceCert, sizeof(CertECC));
		return deviceCert;
	}

	// generate a new public key + certificate from privateKey. Certificate is signed with the device key
	CertECC CertECC::generateCertificate(uint32 signerTitleIdHigh, uint32 signerTitleIdLow, ECCPrivKey& privKeySigner, ECCPrivKey& privKeyIn, ECCPubKey& pubKeyOut)
	{
		CertECC deviceCert = GetDeviceCertificate();

		CertECC newCert = deviceCert;

		cemu_assert(newCert.signatureType == CertECC::SIGTYPE::ECC_SHA256);
		// update date
		newCert.date = 0;
		// update issuer
		strcat(newCert.issuer, "-");
		strcat(newCert.issuer, newCert.ngName);

		// update subject
		memset(newCert.ngName, 0, sizeof(newCert.ngName));
		sprintf(newCert.ngName, "AP%08x%08x", signerTitleIdHigh, signerTitleIdLow);

		// calculate public key from input private key
		newCert.publicKey = ECCPubKey::generateFromPrivateKey(privKeyIn);
		pubKeyOut = newCert.publicKey;

		// sign certificate
		newCert.sign(privKeySigner);

		return newCert;
	}

	ECCSig signHash(uint32 signerTitleIdHigh, uint32 signerTitleIdLow, uint8* hash, sint32 hashLen, CertECC& certChainOut)
	{
		// generate key pair (we only care about the private key)
		ECCPrivKey privKey = ECCPrivKey::generate();

		// get public key and certificate
		ECCPubKey pubKey;
		ECCPrivKey signerPrivKey = ECCPrivKey::getDeviceCertPrivateKey();
		certChainOut = CertECC::generateCertificate(signerTitleIdHigh, signerTitleIdLow, signerPrivKey, privKey, pubKey);

		// generate signature
		cemu_assert_debug(hashLen == 32);
		ECCSig eccSig;
		cemu_assert(b233_ecdsa_sign(privKey.keyData, hash, hashLen, eccSig.r, eccSig.s) == 0);
		cemu_assert(verifyHashSignature(hash, hashLen, pubKey, eccSig));
		return eccSig;
	}

	bool verifyHashSignature(uint8* hash, sint32 hashLen, ECCPubKey& certChainPubKey, ECCSig& sig)
	{
		uint8 point[B233_POINT_SIZE];
		certChainPubKey.getPoint(point);
		return b233_ecdsa_verify(point, hash, hashLen, sig.r, sig.s) == 0;
	}

	bool verifyCert(CertECC& cert, NCrypto::ECCPubKey& signerPubKey)
	{
		CertECC::SIGTYPE sigType = cert.signatureType;

		if (sigType == CertECC::SIGTYPE::ECC_SHA256)
		{
			// used for Wii U certs
			NCrypto::CHash256 hash;
			NCrypto::GenerateHashSHA256(cert.issuer, 0x100, hash);
			return NCrypto::verifyHashSignature(hash.b, 32, signerPubKey, cert.signature);
		}
		else if (sigType == CertECC::SIGTYPE::ECC_SHA1)
		{
			// from Wii era
			NCrypto::CHash160 hash;
			NCrypto::GenerateHashSHA1(cert.issuer, 0x100, hash);
			return NCrypto::verifyHashSignature(hash.b, 20, signerPubKey, cert.signature);
		}
		else
		{
			cemu_assert_unimplemented();
		}
		return false;
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

	bool OTP_IsPresent()
	{
		return hasOtpMem;
	}

	bool HasDataForConsoleCert()
	{
		return SEEPROM_IsPresent() && OTP_IsPresent();
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

	void unitTests()
	{
		base64Tests();
	}

};
