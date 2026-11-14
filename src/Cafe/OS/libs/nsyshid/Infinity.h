#pragma once

#include <array>

#include "nsyshid.h"
#include "Backend.h"

#include "hid_portal.h"

namespace nsyshid
{
	class InfinityBaseDevice final : public Device {
	  public:
		InfinityBaseDevice();
		~InfinityBaseDevice() = default;

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

	constexpr uint16 INF_BLOCK_COUNT = 0x14;
	constexpr uint16 INF_BLOCK_SIZE = 0x10;
	constexpr uint16 INF_FIGURE_SIZE = INF_BLOCK_COUNT * INF_BLOCK_SIZE;
	constexpr uint8 MAX_FIGURES = 9;
	// The emulated base. Commands run one at a time on whichever transfer
	// worker hid_portal hands them to, so the state below is the runner's
	// alone; each answer is queued for an interrupt read, which waits for one.
	// No figure is placed on it in this core.
	class InfinityUSB {
	  public:
		struct InfinityFigure final
		{
			std::array<uint8, INF_FIGURE_SIZE> data{};
			bool present = false;
			uint8 orderAdded = 255;
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
		void SendCommand(uint8* buf, uint32 length);
		void GetBlankResponse(uint8 sequence, std::array<uint8, 32>& replyBuf);
		void DescrambleAndSeed(uint8* buf, uint8 sequence,
							   std::array<uint8, 32>& replyBuf);
		void GetNextAndScramble(uint8 sequence, std::array<uint8, 32>& replyBuf);
		void GetPresentFigures(uint8 sequence, std::array<uint8, 32>& replyBuf);
		void QueryBlock(uint8 figNum, uint8 block, std::array<uint8, 32>& replyBuf,
						uint8 sequence);
		void WriteBlock(uint8 figNum, uint8 block, const uint8* toWriteBuf,
						std::array<uint8, 32>& replyBuf, uint8 sequence);
		void GetFigureIdentifier(uint8 figNum, uint8 sequence,
								 std::array<uint8, 32>& replyBuf);
		uint8 GenerateChecksum(const std::array<uint8, 32>& data,
							   int numOfBytes) const;
		uint32 Descramble(uint64 numToDescramble);
		uint64 Scramble(uint32 numToScramble, uint32 garbage);
		void GenerateSeed(uint32 seed);
		uint32 GetNext();
		InfinityFigure& GetFigureByOrder(uint8 orderAdded);

		hid_portal_t m_portal;
		bool m_started = false;
		std::array<InfinityFigure, MAX_FIGURES> m_figures;
		uint32 m_randomA = 0;
		uint32 m_randomB = 0;
		uint32 m_randomC = 0;
		uint32 m_randomD = 0;
	};
	extern InfinityUSB g_infinitybase;

} // namespace nsyshid