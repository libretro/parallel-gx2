#include <array>
#include <optional>
#include <span>

#include "nsyshid.h"
#include "Backend.h"

#include "hid_portal.h"

namespace nsyshid
{
	class DimensionsToypadDevice final : public Device
	{
	  public:
		DimensionsToypadDevice();
		~DimensionsToypadDevice() = default;

		bool Open() override;

		void Close() override;

		bool IsOpened() override;

		ReadResult Read(ReadMessage* message) override;

		WriteResult Write(WriteMessage* message) override;

		bool GetDescriptor(uint8 descType,
						   uint8 descIndex,
						   uint16 lang,
						   uint8* output,
						   uint32 outputMaxLength) override;

		bool SetIdle(uint8 ifIndex,
					 uint8 reportId,
					 uint8 duration) override;

		bool SetProtocol(uint8 ifIndex, uint8 protocol) override;

		bool SetReport(ReportMessage* message) override;

	  private:
		bool m_IsOpened;
	};

	// The emulated toypad. Commands run one at a time on whichever transfer
	// worker hid_portal hands them to, so the state below is the runner's
	// alone; each answer is queued for an interrupt read, which waits for one.
	// No figure is placed on it in this core.
	class DimensionsUSB
	{
	  public:
		struct DimensionsMini final
		{
			std::array<uint8, 0x2D * 0x04> data{};
			uint8 index = 255;
			uint8 pad = 255;
			uint32 id = 0;
		};

		// for each title, before its transfers start
		void Start();
		// wakes the reads still waiting, before the transfer workers stop
		void Close();
		// once they have
		void Stop();

		void Submit(const uint8* buf, uint32 length);
		// false once closed
		bool GetStatus(std::array<uint8, 32>& response);

	  private:
		static void RunCommand(void* ctx, const unsigned char* cmd, unsigned len);
		void SendCommand(std::span<const uint8, 32> buf);
		void GenerateRandomNumber(std::span<const uint8, 8> buf, uint8 sequence,
								  std::array<uint8, 32>& replyBuf);
		void InitializeRNG(uint32 seed);
		void GetChallengeResponse(std::span<const uint8, 8> buf, uint8 sequence,
								  std::array<uint8, 32>& replyBuf);
		void QueryBlock(uint8 index, uint8 page, std::array<uint8, 32>& replyBuf,
						uint8 sequence);
		void WriteBlock(uint8 index, uint8 page, std::span<const uint8, 4> toWriteBuf, std::array<uint8, 32>& replyBuf,
						uint8 sequence);
		void GetModel(std::span<const uint8, 8> buf, uint8 sequence,
					  std::array<uint8, 32>& replyBuf);
		uint8 GenerateChecksum(const std::array<uint8, 32>& data,
							   int numOfBytes) const;
		std::array<uint8, 8> Decrypt(std::span<const uint8, 8> buf, std::optional<std::array<uint8, 16>> key);
		std::array<uint8, 8> Encrypt(std::span<const uint8, 8> buf, std::optional<std::array<uint8, 16>> key);
		uint32 GetNext();
		DimensionsMini& GetFigureByIndex(uint8 index);

		hid_portal_t m_portal;
		bool m_started = false;
		std::array<DimensionsMini, 7> m_figures{};
		uint32 m_randomA = 0;
		uint32 m_randomB = 0;
		uint32 m_randomC = 0;
		uint32 m_randomD = 0;
	};
	extern DimensionsUSB g_dimensionstoypad;

} // namespace nsyshid