#pragma once

#include <array>

#include "nsyshid.h"
#include "Backend.h"

#include "hid_portal.h"

namespace nsyshid
{
	class SkylanderPortalDevice final : public Device {
	  public:
		SkylanderPortalDevice();
		~SkylanderPortalDevice() = default;

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

	constexpr uint16 SKY_BLOCK_COUNT = 0x40;
	constexpr uint16 SKY_BLOCK_SIZE = 0x10;
	constexpr uint16 SKY_FIGURE_SIZE = SKY_BLOCK_COUNT * SKY_BLOCK_SIZE;
	constexpr uint8 MAX_SKYLANDERS = 16;

	// The emulated portal. Commands run one at a time on whichever transfer
	// worker hid_portal hands them to, so the state below is the runner's
	// alone; an interrupt read takes a queued answer, or else the status the
	// last command published. No figure is placed on it in this core.
	class SkylanderUSB {
	  public:
		struct Skylander final
		{
			uint8 status = 0;
			std::array<uint8, SKY_FIGURE_SIZE> data{};

			enum : uint8
			{
				REMOVED = 0,
				READY = 1,
				REMOVING = 2,
				ADDED = 3
			};
		};

		struct SkylanderLEDColor final
		{
			uint8 red = 0;
			uint8 green = 0;
			uint8 blue = 0;
		};

		// for each title, before its transfers start
		void Start();
		// wakes the reads still waiting, before the transfer workers stop
		void Close();
		// once they have
		void Stop();

		void Submit(const uint8* buf, uint32 length);
		std::array<uint8, 64> GetStatus();

	  private:
		static void RunCommand(void* ctx, const unsigned char* cmd, unsigned len);
		void ControlTransfer(uint8* buf, uint32 length);
		void PublishStatus();
		void Activate();
		void SetLeds(uint8 side, uint8 r, uint8 g, uint8 b);
		void QueryBlock(uint8 skyNum, uint8 block, uint8* replyBuf);
		void WriteBlock(uint8 skyNum, uint8 block, const uint8* toWriteBuf,
						uint8* replyBuf);

		hid_portal_t m_portal;
		bool m_started = false;
		// read by any transfer
		retro_atomic_int_t m_statusWord;
		retro_atomic_int_t m_active;
		retro_atomic_int_t m_interruptCounter;
		// the command runner's
		std::array<Skylander, MAX_SKYLANDERS> m_skylanders;
		bool m_activated = true;
		SkylanderLEDColor m_colorRight = {};
		SkylanderLEDColor m_colorLeft = {};
		SkylanderLEDColor m_colorTrap = {};
	};
	extern SkylanderUSB g_skyportal;
} // namespace nsyshid