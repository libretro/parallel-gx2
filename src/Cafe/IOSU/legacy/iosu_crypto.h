#pragma once

void iosuCrypto_init();

bool iosuCrypto_getDeviceId(uint32* deviceId);
void iosuCrypto_getDeviceSerialString(char* serialString);

// certificate API

struct CertECC_t
{
	enum class SIGTYPE : uint32
	{
		ECC_SHA256 = 0x00010005
	};

	/* +0x000 */ betype<SIGTYPE>	sigType; // 01 00 02 00
	/* +0x004 */ uint8				signature[0x3C]; // from OTP 0xA3*4
	/* +0x040 */ uint8				ukn040[0x40]; // seems to be just padding
	/* +0x080 */ char				certificateSubject[0x40]; // "Root - CA%08x - MS%08x" 
	/* +0x0C0 */ char				ukn0C0[0x4]; // ??? 00 00 00 02 ?
	/* +0x0C4 */ char				ngName[0x40]; // "NG%08X"
	/* +0x104 */ uint32				ngKeyId; // big endian? (from OTP 0xA2*4)
	/* +0x108 */ uint8				publicKey[0x3C];
	/* +0x144 */ uint8				padding[0x180 - 0x144];

	
};

static_assert(sizeof(CertECC_t) == 0x180);


// certificate store, into a transfer's TLS settings (Common/http_xfer.h)
struct http_xfer_tls;
bool iosuCrypto_addClientCertificate(struct http_xfer_tls* tlsSettings, sint32 certificateId);
bool iosuCrypto_addCACertificate(struct http_xfer_tls* tlsSettings, sint32 certificateId);
bool iosuCrypto_addCustomCACertificate(struct http_xfer_tls* tlsSettings, uint8* certData, sint32 certLength);

uint8* iosuCrypto_getCertificateDataById(sint32 certificateId, sint32* certificateSize);
uint8* iosuCrypto_getCertificatePrivateKeyById(sint32 certificateId, sint32* certificateSize);

// helper for online functionality
enum
{
	IOS_CRYPTO_ONLINE_REQ_OK,
	IOS_CRYPTO_ONLINE_REQ_OTP_MISSING,
	IOS_CRYPTO_ONLINE_REQ_OTP_CORRUPTED,
	IOS_CRYPTO_ONLINE_REQ_SEEPROM_MISSING,
	IOS_CRYPTO_ONLINE_REQ_SEEPROM_CORRUPTED,
	IOS_CRYPTO_ONLINE_REQ_MISSING_FILE
};

sint32 iosuCrypt_checkRequirementsForOnlineMode(std::string& additionalErrorInfo);
void iosuCrypto_readOtpData(void* output, sint32 wordIndex, sint32 size);

std::vector<const wchar_t*> iosuCrypt_getCertificateKeys();
std::vector<const wchar_t*> iosuCrypt_getCertificateNames();