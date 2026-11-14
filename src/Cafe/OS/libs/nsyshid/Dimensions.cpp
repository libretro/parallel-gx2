#include "Dimensions.h"

#include "nsyshid.h"
#include "Backend.h"

#include <array>

namespace nsyshid
{
	static constexpr std::array<uint8, 16> COMMAND_KEY = {0x55, 0xFE, 0xF6, 0xB0, 0x62, 0xBF, 0x0B, 0x41,
														  0xC9, 0xB3, 0x7C, 0xB4, 0x97, 0x3E, 0x29, 0x7B};

	DimensionsUSB g_dimensionstoypad;

	DimensionsToypadDevice::DimensionsToypadDevice()
		: Device(0x0E6F, 0x0241, 1, 2, 0)
	{
		m_IsOpened = false;
	}

	bool DimensionsToypadDevice::Open()
	{
		if (!IsOpened())
		{
			m_IsOpened = true;
		}
		return true;
	}

	void DimensionsToypadDevice::Close()
	{
		if (IsOpened())
		{
			m_IsOpened = false;
		}
	}

	bool DimensionsToypadDevice::IsOpened()
	{
		return m_IsOpened;
	}

	Device::ReadResult DimensionsToypadDevice::Read(ReadMessage* message)
	{
		std::array<uint8, 32> status;
		if (!g_dimensionstoypad.GetStatus(status))
			return Device::ReadResult::Error;
		memcpy(message->data, status.data(), std::min<uint32>(message->length, 32));
		message->bytesRead = message->length;
		return Device::ReadResult::Success;
	}

	Device::WriteResult DimensionsToypadDevice::Write(WriteMessage* message)
	{
		if (message->length != 32)
			return Device::WriteResult::Error;

		g_dimensionstoypad.Submit(message->data, 32);
		message->bytesWritten = message->length;
		return Device::WriteResult::Success;
	}

	bool DimensionsToypadDevice::GetDescriptor(uint8 descType,
											   uint8 descIndex,
											   uint16 lang,
											   uint8* output,
											   uint32 outputMaxLength)
	{
		uint8 configurationDescriptor[0x29];

		uint8* currentWritePtr;

		// configuration descriptor
		currentWritePtr = configurationDescriptor + 0;
		*(uint8*)(currentWritePtr + 0) = 9;			// bLength
		*(uint8*)(currentWritePtr + 1) = 2;			// bDescriptorType
		*(uint16be*)(currentWritePtr + 2) = 0x0029; // wTotalLength
		*(uint8*)(currentWritePtr + 4) = 1;			// bNumInterfaces
		*(uint8*)(currentWritePtr + 5) = 1;			// bConfigurationValue
		*(uint8*)(currentWritePtr + 6) = 0;			// iConfiguration
		*(uint8*)(currentWritePtr + 7) = 0x80;		// bmAttributes
		*(uint8*)(currentWritePtr + 8) = 0xFA;		// MaxPower
		currentWritePtr = currentWritePtr + 9;
		// configuration descriptor
		*(uint8*)(currentWritePtr + 0) = 9;	   // bLength
		*(uint8*)(currentWritePtr + 1) = 0x04; // bDescriptorType
		*(uint8*)(currentWritePtr + 2) = 0;	   // bInterfaceNumber
		*(uint8*)(currentWritePtr + 3) = 0;	   // bAlternateSetting
		*(uint8*)(currentWritePtr + 4) = 2;	   // bNumEndpoints
		*(uint8*)(currentWritePtr + 5) = 3;	   // bInterfaceClass
		*(uint8*)(currentWritePtr + 6) = 0;	   // bInterfaceSubClass
		*(uint8*)(currentWritePtr + 7) = 0;	   // bInterfaceProtocol
		*(uint8*)(currentWritePtr + 8) = 0;	   // iInterface
		currentWritePtr = currentWritePtr + 9;
		// configuration descriptor
		*(uint8*)(currentWritePtr + 0) = 9;			// bLength
		*(uint8*)(currentWritePtr + 1) = 0x21;		// bDescriptorType
		*(uint16be*)(currentWritePtr + 2) = 0x0111; // bcdHID
		*(uint8*)(currentWritePtr + 4) = 0x00;		// bCountryCode
		*(uint8*)(currentWritePtr + 5) = 0x01;		// bNumDescriptors
		*(uint8*)(currentWritePtr + 6) = 0x22;		// bDescriptorType
		*(uint16be*)(currentWritePtr + 7) = 0x001D; // wDescriptorLength
		currentWritePtr = currentWritePtr + 9;
		// endpoint descriptor 1
		*(uint8*)(currentWritePtr + 0) = 7;		  // bLength
		*(uint8*)(currentWritePtr + 1) = 0x05;	  // bDescriptorType
		*(uint8*)(currentWritePtr + 2) = 0x81;	  // bEndpointAddress
		*(uint8*)(currentWritePtr + 3) = 0x03;	  // bmAttributes
		*(uint16be*)(currentWritePtr + 4) = 0x40; // wMaxPacketSize
		*(uint8*)(currentWritePtr + 6) = 0x01;	  // bInterval
		currentWritePtr = currentWritePtr + 7;
		// endpoint descriptor 2
		*(uint8*)(currentWritePtr + 0) = 7;		  // bLength
		*(uint8*)(currentWritePtr + 1) = 0x05;	  // bDescriptorType
		*(uint8*)(currentWritePtr + 1) = 0x02;	  // bEndpointAddress
		*(uint8*)(currentWritePtr + 2) = 0x03;	  // bmAttributes
		*(uint16be*)(currentWritePtr + 3) = 0x40; // wMaxPacketSize
		*(uint8*)(currentWritePtr + 5) = 0x01;	  // bInterval
		currentWritePtr = currentWritePtr + 7;

		cemu_assert_debug((currentWritePtr - configurationDescriptor) == 0x29);

		memcpy(output, configurationDescriptor,
			   std::min<uint32>(outputMaxLength, sizeof(configurationDescriptor)));
		return true;
	}

	bool DimensionsToypadDevice::SetIdle(uint8 ifIndex,
										 uint8 reportId,
										 uint8 duration)
	{
		return true;
	}

	bool DimensionsToypadDevice::SetProtocol(uint8 ifIndex, uint8 protocol)
	{
		cemuLog_log(LogType::Force, "Toypad Protocol");
		return true;
	}

	bool DimensionsToypadDevice::SetReport(ReportMessage* message)
	{
		cemuLog_log(LogType::Force, "Toypad Report");
		return true;
	}

	void DimensionsUSB::Start()
	{
		// a title loaded again without its unload in between
		Stop();
		for (auto& f : m_figures)
			f = {};
		m_randomA = m_randomB = m_randomC = m_randomD = 0;
		m_started = hid_portal_init(&m_portal, &DimensionsUSB::RunCommand, this);
	}

	void DimensionsUSB::Close()
	{
		if (m_started)
			hid_portal_close(&m_portal);
	}

	void DimensionsUSB::Stop()
	{
		if (m_started)
			hid_portal_free(&m_portal);
		m_started = false;
	}

	void DimensionsUSB::Submit(const uint8* buf, uint32 length)
	{
		if (m_started && !hid_portal_submit(&m_portal, buf, length))
			cemuLog_logDebug(LogType::Force, "Dimensions toypad: command dropped, {} queued", HID_PORTAL_SLOTS);
	}

	void DimensionsUSB::RunCommand(void* ctx, const unsigned char* cmd, unsigned len)
	{
		std::array<uint8, 32> buf = {};
		memcpy(buf.data(), cmd, std::min<unsigned>(len, 32));
		static_cast<DimensionsUSB*>(ctx)->SendCommand(buf);
	}

	// the toypad answers an interrupt read only once it has something to say
	bool DimensionsUSB::GetStatus(std::array<uint8, 32>& response)
	{
		uint8 report[HID_PORTAL_REPORT];
		if (!m_started || !hid_portal_read(&m_portal, report))
			return false;
		memcpy(response.data(), report, response.size());
		return true;
	}

	void DimensionsUSB::SendCommand(std::span<const uint8, 32> buf)
	{
		const uint8 command = buf[2];
		const uint8 sequence = buf[3];

		std::array<uint8, 32> q_result{};

		switch (command)
		{
		case 0xB0: // Wake
		{
			// Consistent device response to the wake command
			q_result = {0x55, 0x0e, 0x01, 0x28, 0x63, 0x29,
						0x20, 0x4c, 0x45, 0x47, 0x4f, 0x20,
						0x32, 0x30, 0x31, 0x34, 0x46};
			break;
		}
		case 0xB1: // Seed
		{
			// Initialise a random number generator using the seed provided
			GenerateRandomNumber(std::span<const uint8, 8>{buf.begin() + 4, 8}, sequence, q_result);
			break;
		}
		case 0xB3: // Challenge
		{
			// Get the next number in the sequence based on the RNG from 0xB1 command
			GetChallengeResponse(std::span<const uint8, 8>{buf.begin() + 4, 8}, sequence, q_result);
			break;
		}
		case 0xC0: // Color
		case 0xC1: // Get Pad Color
		case 0xC2: // Fade
		case 0xC3: // Flash
		case 0xC4: // Fade Random
		case 0xC6: // Fade All
		case 0xC7: // Flash All
		case 0xC8: // Color All
		{
			// Send a blank response to acknowledge color has been sent to toypad
			q_result = {0x55, 0x01, sequence};
			q_result[3] = GenerateChecksum(q_result, 3);
			break;
		}
		case 0xD2: // Read
		{
			// Read 4 pages from the figure at index (buf[4]), starting with page buf[5]
			QueryBlock(buf[4], buf[5], q_result, sequence);
			break;
		}
		case 0xD3: // Write
		{
			// Write 4 bytes to page buf[5] to the figure at index buf[4]
			WriteBlock(buf[4], buf[5], std::span<const uint8, 4>{buf.begin() + 6, 4}, q_result, sequence);
			break;
		}
		case 0xD4: // Model
		{
			// Get the model id of the figure at index buf[4]
			GetModel(std::span<const uint8, 8>{buf.begin() + 4, 8}, sequence, q_result);
			break;
		}
		case 0xD0: // Tag List
		case 0xE1: // PWD
		case 0xE5: // Active
		case 0xFF: // LEDS Query
		{
			// Further investigation required
			cemuLog_log(LogType::Force, "Unimplemented LD Function: {:x}", command);
			break;
		}
		default:
		{
			cemuLog_log(LogType::Force, "Unknown LD Function: {:x}", command);
			break;
		}
		}

		uint8 report[HID_PORTAL_REPORT] = {};
		memcpy(report, q_result.data(), q_result.size());
		hid_portal_report(&m_portal, report);
	}

	void DimensionsUSB::GenerateRandomNumber(std::span<const uint8, 8> buf, uint8 sequence,
											 std::array<uint8, 32>& replyBuf)
	{
		// Decrypt payload into an 8 byte array
		std::array<uint8, 8> value = Decrypt(buf, std::nullopt);
		// Seed is the first 4 bytes (little endian) of the decrypted payload
		uint32 seed = (uint32&)value[0];
		// Confirmation is the second 4 bytes (big endian) of the decrypted payload
		uint32 conf = (uint32be&)value[4];
		// Initialize rng using the seed from decrypted payload
		InitializeRNG(seed);
		// Encrypt 8 bytes, first 4 bytes is the decrypted confirmation from payload, 2nd 4 bytes are blank
		std::array<uint8, 8> valueToEncrypt = {value[4], value[5], value[6], value[7], 0, 0, 0, 0};
		std::array<uint8, 8> encrypted = Encrypt(valueToEncrypt, std::nullopt);
		replyBuf[0] = 0x55;
		replyBuf[1] = 0x09;
		replyBuf[2] = sequence;
		// Copy encrypted value to response data
		memcpy(&replyBuf[3], encrypted.data(), encrypted.size());
		replyBuf[11] = GenerateChecksum(replyBuf, 11);
	}

	void DimensionsUSB::GetChallengeResponse(std::span<const uint8, 8> buf, uint8 sequence,
											 std::array<uint8, 32>& replyBuf)
	{
		// Decrypt payload into an 8 byte array
		std::array<uint8, 8> value = Decrypt(buf, std::nullopt);
		// Confirmation is the first 4 bytes of the decrypted payload
		uint32 conf = (uint32be&)value[0];
		// Generate next random number based on RNG
		uint32 nextRandom = GetNext();
		// Encrypt an 8 byte array, first 4 bytes are the next random number (little endian)
		// followed by the confirmation from the decrypted payload
		std::array<uint8, 8> valueToEncrypt = {uint8(nextRandom & 0xFF), uint8((nextRandom >> 8) & 0xFF),
											   uint8((nextRandom >> 16) & 0xFF), uint8((nextRandom >> 24) & 0xFF),
											   value[0], value[1], value[2], value[3]};
		std::array<uint8, 8> encrypted = Encrypt(valueToEncrypt, std::nullopt);
		replyBuf[0] = 0x55;
		replyBuf[1] = 0x09;
		replyBuf[2] = sequence;
		// Copy encrypted value to response data
		memcpy(&replyBuf[3], encrypted.data(), encrypted.size());
		replyBuf[11] = GenerateChecksum(replyBuf, 11);
	}

	void DimensionsUSB::InitializeRNG(uint32 seed)
	{
		m_randomA = 0xF1EA5EED;
		m_randomB = seed;
		m_randomC = seed;
		m_randomD = seed;

		for (int i = 0; i < 42; i++)
		{
			GetNext();
		}
	}

	uint32 DimensionsUSB::GetNext()
	{
		uint32 e = m_randomA - std::rotl(m_randomB, 21);
		m_randomA = m_randomB ^ std::rotl(m_randomC, 19);
		m_randomB = m_randomC + std::rotl(m_randomD, 6);
		m_randomC = m_randomD + e;
		m_randomD = e + m_randomA;
		return m_randomD;
	}

	std::array<uint8, 8> DimensionsUSB::Decrypt(std::span<const uint8, 8> buf, std::optional<std::array<uint8, 16>> key)
	{
		// Value to decrypt is separated in to two little endian 32 bit unsigned integers
		uint32 dataOne = (uint32&)buf[0];
		uint32 dataTwo = (uint32&)buf[4];

		// Use the key as 4 32 bit little endian unsigned integers
		uint32 keyOne;
		uint32 keyTwo;
		uint32 keyThree;
		uint32 keyFour;

		if (key)
		{
			keyOne = (uint32&)key.value()[0];
			keyTwo = (uint32&)key.value()[4];
			keyThree = (uint32&)key.value()[8];
			keyFour = (uint32&)key.value()[12];
		}
		else
		{
			keyOne = (uint32&)COMMAND_KEY[0];
			keyTwo = (uint32&)COMMAND_KEY[4];
			keyThree = (uint32&)COMMAND_KEY[8];
			keyFour = (uint32&)COMMAND_KEY[12];
		}

		uint32 sum = 0xC6EF3720;
		uint32 delta = 0x9E3779B9;

		for (int i = 0; i < 32; i++)
		{
			dataTwo -= (((dataOne << 4) + keyThree) ^ (dataOne + sum) ^ ((dataOne >> 5) + keyFour));
			dataOne -= (((dataTwo << 4) + keyOne) ^ (dataTwo + sum) ^ ((dataTwo >> 5) + keyTwo));
			sum -= delta;
		}

		cemu_assert(sum == 0);

		std::array<uint8, 8> decrypted = {uint8(dataOne & 0xFF), uint8((dataOne >> 8) & 0xFF),
										  uint8((dataOne >> 16) & 0xFF), uint8((dataOne >> 24) & 0xFF),
										  uint8(dataTwo & 0xFF), uint8((dataTwo >> 8) & 0xFF),
										  uint8((dataTwo >> 16) & 0xFF), uint8((dataTwo >> 24) & 0xFF)};
		return decrypted;
	}
	std::array<uint8, 8> DimensionsUSB::Encrypt(std::span<const uint8, 8> buf, std::optional<std::array<uint8, 16>> key)
	{
		// Value to encrypt is separated in to two little endian 32 bit unsigned integers
		uint32 dataOne = (uint32&)buf[0];
		uint32 dataTwo = (uint32&)buf[4];

		// Use the key as 4 32 bit little endian unsigned integers
		uint32 keyOne;
		uint32 keyTwo;
		uint32 keyThree;
		uint32 keyFour;

		if (key)
		{
			keyOne = (uint32&)key.value()[0];
			keyTwo = (uint32&)key.value()[4];
			keyThree = (uint32&)key.value()[8];
			keyFour = (uint32&)key.value()[12];
		}
		else
		{
			keyOne = (uint32&)COMMAND_KEY[0];
			keyTwo = (uint32&)COMMAND_KEY[4];
			keyThree = (uint32&)COMMAND_KEY[8];
			keyFour = (uint32&)COMMAND_KEY[12];
		}

		uint32 sum = 0;
		uint32 delta = 0x9E3779B9;

		for (int i = 0; i < 32; i++)
		{
			sum += delta;
			dataOne += (((dataTwo << 4) + keyOne) ^ (dataTwo + sum) ^ ((dataTwo >> 5) + keyTwo));
			dataTwo += (((dataOne << 4) + keyThree) ^ (dataOne + sum) ^ ((dataOne >> 5) + keyFour));
		}

		cemu_assert(sum == 0xC6EF3720);

		std::array<uint8, 8> encrypted = {uint8(dataOne & 0xFF), uint8((dataOne >> 8) & 0xFF),
										  uint8((dataOne >> 16) & 0xFF), uint8((dataOne >> 24) & 0xFF),
										  uint8(dataTwo & 0xFF), uint8((dataTwo >> 8) & 0xFF),
										  uint8((dataTwo >> 16) & 0xFF), uint8((dataTwo >> 24) & 0xFF)};
		return encrypted;
	}

	DimensionsUSB::DimensionsMini&
	DimensionsUSB::GetFigureByIndex(uint8 index)
	{
		return m_figures[index];
	}

	void DimensionsUSB::QueryBlock(uint8 index, uint8 page,
								   std::array<uint8, 32>& replyBuf,
								   uint8 sequence)
	{
		replyBuf[0] = 0x55;
		replyBuf[1] = 0x12;
		replyBuf[2] = sequence;
		replyBuf[3] = 0x00;

		// Index from game begins at 1 rather than 0, so minus 1 here
		if (const uint8 figureIndex = index - 1; figureIndex < 7)
		{
			const DimensionsMini& figure = GetFigureByIndex(figureIndex);

			// Query 4 pages of 4 bytes from the figure, copy this to the response
			if (figure.index != 255 && (4 * page) < ((0x2D * 4) - 16))
			{
				std::memcpy(&replyBuf[4], figure.data.data() + (4 * page), 16);
			}
		}
		replyBuf[20] = GenerateChecksum(replyBuf, 20);
	}

	void DimensionsUSB::WriteBlock(uint8 index, uint8 page, std::span<const uint8, 4> toWriteBuf,
								   std::array<uint8, 32>& replyBuf, uint8 sequence)
	{
		replyBuf[0] = 0x55;
		replyBuf[1] = 0x02;
		replyBuf[2] = sequence;
		replyBuf[3] = 0x00;

		// Index from game begins at 1 rather than 0, so minus 1 here
		if (const uint8 figureIndex = index - 1; figureIndex < 7)
		{
			DimensionsMini& figure = GetFigureByIndex(figureIndex);

			// Copy 4 bytes to the page on the figure requested by the game
			if (figure.index != 255 && page < 0x2D)
			{
				// Id is written to page 36
				if (page == 36)
				{
					figure.id = (uint32&)toWriteBuf[0];
				}
				std::memcpy(figure.data.data() + (page * 4), toWriteBuf.data(), 4);
			}
		}
		replyBuf[4] = GenerateChecksum(replyBuf, 4);
	}

	void DimensionsUSB::GetModel(std::span<const uint8, 8> buf, uint8 sequence,
								 std::array<uint8, 32>& replyBuf)
	{
		// Decrypt payload to 8 byte array, byte 1 is the index, 4-7 are the confirmation
		std::array<uint8, 8> value = Decrypt(buf, std::nullopt);
		uint8 index = value[0];
		uint32 conf = (uint32be&)value[4];
		// Response is the figure's id (little endian) followed by the confirmation from payload
		// Index from game begins at 1 rather than 0, so minus 1 here
		std::array<uint8, 8> valueToEncrypt = {};
		if (const uint8 figureIndex = index - 1; figureIndex < 7)
		{
			const DimensionsMini& figure = GetFigureByIndex(figureIndex);
			valueToEncrypt = {uint8(figure.id & 0xFF), uint8((figure.id >> 8) & 0xFF),
							  uint8((figure.id >> 16) & 0xFF), uint8((figure.id >> 24) & 0xFF),
							  value[4], value[5], value[6], value[7]};
		}
		std::array<uint8, 8> encrypted = Encrypt(valueToEncrypt, std::nullopt);
		replyBuf[0] = 0x55;
		replyBuf[1] = 0x0a;
		replyBuf[2] = sequence;
		replyBuf[3] = 0x00;
		memcpy(&replyBuf[4], encrypted.data(), encrypted.size());
		replyBuf[12] = GenerateChecksum(replyBuf, 12);
	}

	uint8 DimensionsUSB::GenerateChecksum(const std::array<uint8, 32>& data,
										  int num_of_bytes) const
	{
		int checksum = 0;
		for (int i = 0; i < num_of_bytes; i++)
		{
			checksum += data[i];
		}
		return (checksum & 0xFF);
	}

} // namespace nsyshid