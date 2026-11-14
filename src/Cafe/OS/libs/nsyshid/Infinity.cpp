#include "Infinity.h"

#include "nsyshid.h"
#include "Backend.h"

namespace nsyshid
{
	InfinityUSB g_infinitybase;

	InfinityBaseDevice::InfinityBaseDevice()
		: Device(0x0E6F, 0x0129, 1, 2, 0)
	{
		m_IsOpened = false;
	}

	bool InfinityBaseDevice::Open()
	{
		if (!IsOpened())
		{
			m_IsOpened = true;
		}
		return true;
	}

	void InfinityBaseDevice::Close()
	{
		if (IsOpened())
		{
			m_IsOpened = false;
		}
	}

	bool InfinityBaseDevice::IsOpened()
	{
		return m_IsOpened;
	}

	Device::ReadResult InfinityBaseDevice::Read(ReadMessage* message)
	{
		std::array<uint8, 32> status;
		if (!g_infinitybase.GetStatus(status))
			return Device::ReadResult::Error;
		memcpy(message->data, status.data(), std::min<uint32>(message->length, 32));
		message->bytesRead = message->length;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		return Device::ReadResult::Success;
	}

	Device::WriteResult InfinityBaseDevice::Write(WriteMessage* message)
	{
		g_infinitybase.Submit(message->data, message->length);
		message->bytesWritten = message->length;
		return Device::WriteResult::Success;
	}

	bool InfinityBaseDevice::GetDescriptor(uint8 descType,
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

	bool InfinityBaseDevice::SetIdle(uint8 ifIndex,
										 uint8 reportId,
										 uint8 duration)
	{
		return true;
	}

	bool InfinityBaseDevice::SetProtocol(uint8 ifIndex, uint8 protocol)
	{
		return true;
	}

	bool InfinityBaseDevice::SetReport(ReportMessage* message)
	{
		return true;
	}

	void InfinityUSB::Start()
	{
		// a title loaded again without its unload in between
		Stop();
		for (auto& f : m_figures)
			f = {};
		m_randomA = m_randomB = m_randomC = m_randomD = 0;
		m_started = hid_portal_init(&m_portal, &InfinityUSB::RunCommand, this);
	}

	void InfinityUSB::Close()
	{
		if (m_started)
			hid_portal_close(&m_portal);
	}

	void InfinityUSB::Stop()
	{
		if (m_started)
			hid_portal_free(&m_portal);
		m_started = false;
	}

	void InfinityUSB::Submit(const uint8* buf, uint32 length)
	{
		if (m_started && !hid_portal_submit(&m_portal, buf, length))
			cemuLog_logDebug(LogType::Force, "Infinity base: command dropped, {} queued", HID_PORTAL_SLOTS);
	}

	void InfinityUSB::RunCommand(void* ctx, const unsigned char* cmd, unsigned len)
	{
		uint8 buf[HID_PORTAL_REPORT] = {};
		memcpy(buf, cmd, len);
		static_cast<InfinityUSB*>(ctx)->SendCommand(buf, len);
	}

	// the base answers an interrupt read only once it has something to say
	bool InfinityUSB::GetStatus(std::array<uint8, 32>& response)
	{
		uint8 report[HID_PORTAL_REPORT];
		if (!m_started || !hid_portal_read(&m_portal, report))
			return false;
		memcpy(response.data(), report, response.size());
		return true;
	}

	void InfinityUSB::SendCommand(uint8* buf, uint32 length)
	{
		const uint8 command = buf[2];
		const uint8 sequence = buf[3];

		std::array<uint8, 32> q_result{};

		switch (command)
		{
		case 0x80:
		{
			q_result = {0xaa, 0x15, 0x00, 0x00, 0x0f, 0x01, 0x00, 0x03,
						0x02, 0x09, 0x09, 0x43, 0x20, 0x32, 0x62, 0x36,
						0x36, 0x4b, 0x34, 0x99, 0x67, 0x31, 0x93, 0x8c};
			break;
		}
		case 0x81:
		{
			// Initiate Challenge
			DescrambleAndSeed(buf, sequence, q_result);
			break;
		}
		case 0x83:
		{
			// Challenge Response
			GetNextAndScramble(sequence, q_result);
			break;
		}
		case 0x90:
		case 0x92:
		case 0x93:
		case 0x95:
		case 0x96:
		{
			// Color commands
			GetBlankResponse(sequence, q_result);
			break;
		}
		case 0xA1:
		{
			// Get Present Figures
			GetPresentFigures(sequence, q_result);
			break;
		}
		case 0xA2:
		{
			// Read Block from Figure
			QueryBlock(buf[4], buf[5], q_result, sequence);
			break;
		}
		case 0xA3:
		{
			// Write block to figure
			WriteBlock(buf[4], buf[5], &buf[7], q_result, sequence);
			break;
		}
		case 0xB4:
		{
			// Get figure ID
			GetFigureIdentifier(buf[4], sequence, q_result);
			break;
		}
		case 0xB5:
		{
			// Get status?
			GetBlankResponse(sequence, q_result);
			break;
		}
		default:
			cemuLog_logDebug(LogType::Force, "Infinity base: unknown command {:02x}", command);
			break;
		}

		uint8 report[HID_PORTAL_REPORT] = {};
		memcpy(report, q_result.data(), q_result.size());
		hid_portal_report(&m_portal, report);
	}

	uint8 InfinityUSB::GenerateChecksum(const std::array<uint8, 32>& data,
										int numOfBytes) const
	{
		int checksum = 0;
		for (int i = 0; i < numOfBytes; i++)
		{
			checksum += data[i];
		}
		return (checksum & 0xFF);
	}

	void InfinityUSB::GetBlankResponse(uint8 sequence,
									   std::array<uint8, 32>& replyBuf)
	{
		replyBuf[0] = 0xaa;
		replyBuf[1] = 0x01;
		replyBuf[2] = sequence;
		replyBuf[3] = GenerateChecksum(replyBuf, 3);
	}

	void InfinityUSB::DescrambleAndSeed(uint8* buf, uint8 sequence,
										std::array<uint8, 32>& replyBuf)
	{
		uint64 value = uint64(buf[4]) << 56 | uint64(buf[5]) << 48 |
					   uint64(buf[6]) << 40 | uint64(buf[7]) << 32 |
					   uint64(buf[8]) << 24 | uint64(buf[9]) << 16 |
					   uint64(buf[10]) << 8 | uint64(buf[11]);
		uint32 seed = Descramble(value);
		GenerateSeed(seed);
		GetBlankResponse(sequence, replyBuf);
	}

	void InfinityUSB::GetNextAndScramble(uint8 sequence,
										 std::array<uint8, 32>& replyBuf)
	{
		const uint32 nextRandom = GetNext();
		const uint64 scrambledNextRandom = Scramble(nextRandom, 0);
		replyBuf = {0xAA, 0x09, sequence};
		replyBuf[3] = uint8((scrambledNextRandom >> 56) & 0xFF);
		replyBuf[4] = uint8((scrambledNextRandom >> 48) & 0xFF);
		replyBuf[5] = uint8((scrambledNextRandom >> 40) & 0xFF);
		replyBuf[6] = uint8((scrambledNextRandom >> 32) & 0xFF);
		replyBuf[7] = uint8((scrambledNextRandom >> 24) & 0xFF);
		replyBuf[8] = uint8((scrambledNextRandom >> 16) & 0xFF);
		replyBuf[9] = uint8((scrambledNextRandom >> 8) & 0xFF);
		replyBuf[10] = uint8(scrambledNextRandom & 0xFF);
		replyBuf[11] = GenerateChecksum(replyBuf, 11);
	}

	uint32 InfinityUSB::Descramble(uint64 numToDescramble)
	{
		uint64 mask = 0x8E55AA1B3999E8AA;
		uint32 ret = 0;

		for (int i = 0; i < 64; i++)
		{
			if (mask & 0x8000000000000000)
			{
				ret = (ret << 1) | (numToDescramble & 0x01);
			}

			numToDescramble >>= 1;
			mask <<= 1;
		}

		return ret;
	}

	uint64 InfinityUSB::Scramble(uint32 numToScramble, uint32 garbage)
	{
		uint64 mask = 0x8E55AA1B3999E8AA;
		uint64 ret = 0;

		for (int i = 0; i < 64; i++)
		{
			ret <<= 1;

			if ((mask & 1) != 0)
			{
				ret |= (numToScramble & 1);
				numToScramble >>= 1;
			}
			else
			{
				ret |= (garbage & 1);
				garbage >>= 1;
			}

			mask >>= 1;
		}

		return ret;
	}

	void InfinityUSB::GenerateSeed(uint32 seed)
	{
		m_randomA = 0xF1EA5EED;
		m_randomB = seed;
		m_randomC = seed;
		m_randomD = seed;

		for (int i = 0; i < 23; i++)
		{
			GetNext();
		}
	}

	uint32 InfinityUSB::GetNext()
	{
		uint32 a = m_randomA;
		uint32 b = m_randomB;
		uint32 c = m_randomC;
		uint32 ret = std::rotl(m_randomB, 27);

		const uint32 temp = (a + ((ret ^ 0xFFFFFFFF) + 1));
		b ^= std::rotl(c, 17);
		a = m_randomD;
		c += a;
		ret = b + temp;
		a += temp;

		m_randomC = a;
		m_randomA = b;
		m_randomB = c;
		m_randomD = ret;

		return ret;
	}

	void InfinityUSB::GetPresentFigures(uint8 sequence,
										std::array<uint8, 32>& replyBuf)
	{
		int x = 3;
		for (uint8 i = 0; i < m_figures.size(); i++)
		{
			uint8 slot = i == 0 ? 0x10 : (i < 4) ? 0x20
												 : 0x30;
			if (m_figures[i].present)
			{
				replyBuf[x] = slot + m_figures[i].orderAdded;
				replyBuf[x + 1] = 0x09;
				x += 2;
			}
		}
		replyBuf[0] = 0xaa;
		replyBuf[1] = x - 2;
		replyBuf[2] = sequence;
		replyBuf[x] = GenerateChecksum(replyBuf, x);
	}

	InfinityUSB::InfinityFigure&
	InfinityUSB::GetFigureByOrder(uint8 orderAdded)
	{
		for (uint8 i = 0; i < m_figures.size(); i++)
		{
			if (m_figures[i].orderAdded == orderAdded)
			{
				return m_figures[i];
			}
		}
		return m_figures[0];
	}

	void InfinityUSB::QueryBlock(uint8 fig_num, uint8 block,
								 std::array<uint8, 32>& replyBuf,
								 uint8 sequence)
	{
		InfinityFigure& figure = GetFigureByOrder(fig_num);

		replyBuf[0] = 0xaa;
		replyBuf[1] = 0x12;
		replyBuf[2] = sequence;
		replyBuf[3] = 0x00;
		const uint8 file_block = (block == 0) ? 1 : (block * 4);
		if (figure.present && file_block < 20)
		{
			memcpy(&replyBuf[4], figure.data.data() + (16 * file_block), 16);
		}
		replyBuf[20] = GenerateChecksum(replyBuf, 20);
	}

	void InfinityUSB::WriteBlock(uint8 fig_num, uint8 block,
								 const uint8* to_write_buf,
								 std::array<uint8, 32>& replyBuf,
								 uint8 sequence)
	{
		InfinityFigure& figure = GetFigureByOrder(fig_num);

		replyBuf[0] = 0xaa;
		replyBuf[1] = 0x02;
		replyBuf[2] = sequence;
		replyBuf[3] = 0x00;
		const uint8 file_block = (block == 0) ? 1 : (block * 4);
		if (figure.present && file_block < 20)
		{
			memcpy(figure.data.data() + (file_block * 16), to_write_buf, 16);
		}
		replyBuf[4] = GenerateChecksum(replyBuf, 4);
	}

	void InfinityUSB::GetFigureIdentifier(uint8 fig_num, uint8 sequence,
										  std::array<uint8, 32>& replyBuf)
	{
		InfinityFigure& figure = GetFigureByOrder(fig_num);

		replyBuf[0] = 0xaa;
		replyBuf[1] = 0x09;
		replyBuf[2] = sequence;
		replyBuf[3] = 0x00;

		if (figure.present)
		{
			memcpy(&replyBuf[4], figure.data.data(), 7);
		}
		replyBuf[11] = GenerateChecksum(replyBuf, 11);
	}

} // namespace nsyshid