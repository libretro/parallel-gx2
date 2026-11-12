#include "iosu_pdm.h"
#include "Cafe/CafeSystem.h"
#include "config/ActiveSettings.h"
#include "Cafe/IOSU/PDM/pdm_store.h"

// The play statistics and diary are kept in C (pdm_store.c): this is the
// IOSU module around them, and its log
namespace iosu
{
	namespace pdm
	{
		static_assert(sizeof(PlayDiaryEntry) == PDM_PLAY_DIARY_ENTRY_SIZE);
		static_assert(NUM_PLAY_DIARY_ENTRIES_MAX == PDM_PLAY_DIARY_ENTRIES);
		static_assert(NUM_PLAY_STATS_ENTRIES == PDM_PLAY_STATS_ENTRIES);

		// how often the minutes played are written while a title runs
		static constexpr sint64 kTrackingWakeUs = 15 * 1000000;

		static void Load()
		{
			// todo - support for per-account tracking
			const unsigned r = pdm_load(_pathToUtf8(ActiveSettings::GetMlcPath("usr/save/system/pdm/80000001")).c_str());
			if (r & PDM_STATS_UNAVAILABLE)
				cemuLog_log(LogType::Force, "Unable to open or create PlayStats.dat");
			if (r & PDM_STATS_MALFORMED)
				cemuLog_log(LogType::Force, "PlayStats.dat malformed. Time tracking wont be used");
			if (r & PDM_DIARY_BAD_HEADER)
				cemuLog_log(LogType::Force, "Failed to read valid PlayDiary header");
			if (r & PDM_DIARY_BAD_INDEX)
				cemuLog_log(LogType::Force, "Bad value in play diary header, wrapped");
			if (r & PDM_DIARY_UNAVAILABLE)
				cemuLog_log(LogType::Force, "Failed to read or write PlayDiary.dat, playtime tracking will not be possible");
		}

		uint32 GetDiaryEntries(uint8 accountSlot, PlayDiaryEntry* diaryEntries, uint32 maxEntries)
		{
			return pdm_diary_get(diaryEntries, maxEntries);
		}

		class : public ::IOSUModule
		{
			void SystemLaunch() override
			{
				// todo - add support for per-account handling
				Load();
				pdm_close(); // close the files again, user may mess with MLC files or change MLC path while no game is running
			}
			void SystemExit() override
			{
				pdm_unload();
			}
			void TitleStart() override
			{
				// reload data and keep files open
				pdm_unload();
				Load();
				if (!pdm_tracking_start(CafeSystem::GetForegroundTitleId(), kTrackingWakeUs))
					cemuLog_log(LogType::Force, "PDM: play time is not counted for this title");
			}
			void TitleStop() override
			{
				if (pdm_tracking_stop() != 0)
					cemuLog_log(LogType::Force, "Failed to write to PlayStats.dat");
				pdm_close();
			}
		}sIOSUModuleNNPDM;

		IOSUModule* GetModule()
		{
			return static_cast<IOSUModule*>(&sIOSUModuleNNPDM);
		}
	};
};
