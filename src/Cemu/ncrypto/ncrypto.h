#pragma once

enum class CafeConsoleRegion;

namespace NCrypto
{
	/* Base 64 */
	std::string base64Encode(const void* inputMem, size_t inputLen);
	std::vector<uint8> base64Decode(std::string_view inputStr);
	
	/* key and iv helper struct */
	struct AesKey 
	{
		static constexpr size_t SIZE = 16;
		uint8 b[SIZE];
	};

	struct AesIv
	{
		static constexpr size_t SIZE = 16;
		uint8 iv[SIZE];
	};

	/* ECC Certificate */
	struct ECCPubKey
	{
		uint8 x[30];
		uint8 y[30];
	};

	struct ECCSig
	{
		uint8 r[30];
		uint8 s[30];
	};

	struct CertECC
	{
		enum class SIGTYPE : uint32
		{
			ECC_SHA1   = 0x00010002, // guessed
			ECC_SHA256 = 0x00010005
		};

		/* +0x000 */ betype<SIGTYPE>	signatureType; // 01 00 02 00
		/* +0x004 */ ECCSig				signature; //uint8				signature[0x3C]; // from OTP 0xA3*4
		/* +0x040 */ uint8				ukn040[0x40]; // seems to be just padding
		/* +0x080 */ char				issuer[0x40]; // "Root - CA%08x - MS%08x" 
		/* +0x0C0 */ char				ukn0C0[0x4]; // ??? 00 00 00 02 ?
		/* +0x0C4 */ char				ngName[0x40]; // "NG%08X"
		/* +0x104 */ uint32				date; // big endian? (from OTP 0xA2*4)
		/* +0x108 */ ECCPubKey			publicKey; //uint8				publicKey[0x3C];
		/* +0x144 */ uint8				padding[0x180 - 0x144];

		std::string encodeToBase64();

		static CertECC GetDeviceCertificate();
	};

	static_assert(sizeof(CertECC) == 0x180);


	uint32 GetDeviceId();
	std::string GetSerial();
	CafeConsoleRegion SEEPROM_GetRegion();
	std::string GetRegionAsString(CafeConsoleRegion regionCode);
	const char* GetCountryAsString(sint32 index); // returns NN if index is not valid or known
	size_t GetCountryCount();
	bool SEEPROM_IsPresent();
}