#include "Common/precompiled.h"
#include "Cafe/OS/common/OSCommon.h"
#include "camera.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/OS/libs/coreinit/coreinit_Alarm.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "Cafe/HW/Espresso/PPCCallback.h"
#include "Cafe/OS/libs/camera/cam_state.h"

namespace camera
{

	struct CAMInitInfo_t
	{
		/* +0x00 */ uint32be ukn00;
		/* +0x04 */ uint32be width;
		/* +0x08 */ uint32be height;
		
		/* +0x0C */ uint32be workMemorySize;
		/* +0x10 */ MEMPTR<void> workMemory;

		/* +0x14 */ uint32be handlerFuncPtr;

		/* +0x18 */ uint32be ukn18;
		/* +0x1C */ uint32be fps;

		/* +0x20 */ uint32be ukn20;
	};

	struct CAMTargetSurface 
	{
		/* +0x00 */ uint32be surfaceSize;
		/* +0x04 */ MEMPTR<void> surfacePtr;
		/* +0x08 */ uint32be ukn08;
		/* +0x0C */ uint32be ukn0C;
		/* +0x10 */ uint32be ukn10;
		/* +0x14 */ uint32be ukn14;
		/* +0x18 */ uint32be ukn18;
		/* +0x1C */ uint32be ukn1C;
	};

	struct CAMCallbackParam 
	{
		// type 0 - frame decoded | field1 - imagePtr, field2 - imageSize, field3 - ukn (0)
		// type 1 - ???


		/* +0x0 */ uint32be type; // 0 -> Frame decoded
		/* +0x4 */ uint32be field1;
		/* +0x8 */ uint32be field2;
		/* +0xC */ uint32be field3;
	};


	#define CAM_ERROR_SUCCESS			0
	#define CAM_ERROR_INVALID_HANDLE		-8

	// The handles and each one's queue of surfaces are C (cam_state.c): no
	// lock, nothing allocated, and nothing a title left behind handed to the
	// next one.
	SysAllocator<coreinit::OSAlarm_t, 1> g_alarm_camera;
	SysAllocator<CAMCallbackParam, 1> g_cameraHandlerParam;

	sint32 CAMGetMemReq(void* ukn)
	{
		return 1 * 1024; // always return 1KB
	}

	sint32 CAMCheckMemSegmentation(void* base, uint32 size)
	{
		return CAM_ERROR_SUCCESS; // always return success
	}

	// 60 times a second, on the alarm's thread: a surface back to each open
	// camera that has one queued
	void ppcCAMUpdate60(PPCInterpreter_t* hCPU)
	{
		// todo - handle 30 / 60 FPS
		int cursor = 0;
		uint32 handler, surface, size;
		while (cam_state_next(&cursor, &handler, &surface, &size))
		{
			g_cameraHandlerParam->type = 0;
			g_cameraHandlerParam->field1 = surface;
			g_cameraHandlerParam->field2 = size;
			g_cameraHandlerParam->field3 = 0;
			cemu_assert_debug(handler != MPTR_NULL);
			PPCCoreCallback(handler, g_cameraHandlerParam.GetPtr());
		}
		osLib_returnFromFunction(hCPU, 0);
	}

	sint32 CAMInit(uint32 cameraId, CAMInitInfo_t* camInitInfo, uint32be* error)
	{
		*error = 0; // Hunter's Trophy 2 will fail to boot if we don't set this
		bool first = false;
		const sint32 handle = cam_state_init(camInitInfo->width, camInitInfo->height, camInitInfo->handlerFuncPtr, &first);
		if (!handle)
		{
			cemuLog_log(LogType::Force, "CAMInit: all {} camera handles taken", CAM_STATE_HANDLES);
			return CAM_ERROR_INVALID_HANDLE;
		}
		// one alarm for every camera, from the first, until the title stops
		if (first)
		{
			coreinit::OSCreateAlarm(g_alarm_camera.GetPtr());
			coreinit::OSSetPeriodicAlarm(g_alarm_camera.GetPtr(), coreinit::OSGetTime(), (uint64)ESPRESSO_TIMER_CLOCK / 60ull, RPLLoader_MakePPCCallable(ppcCAMUpdate60));
		}
		return handle;
	}

	sint32 CAMExit(sint32 camHandle)
	{
		return cam_state_exit(camHandle) ? CAM_ERROR_SUCCESS : CAM_ERROR_INVALID_HANDLE;
	}

	sint32 CAMOpen(sint32 camHandle)
	{
		return cam_state_open(camHandle) ? CAM_ERROR_SUCCESS : CAM_ERROR_INVALID_HANDLE;
	}

	sint32 CAMClose(sint32 camHandle)
	{
		return cam_state_close(camHandle) ? CAM_ERROR_SUCCESS : CAM_ERROR_INVALID_HANDLE;
	}

	sint32 CAMSubmitTargetSurface(sint32 camHandle, CAMTargetSurface* targetSurface)
	{
		switch (cam_state_submit(camHandle, targetSurface->surfacePtr.GetMPTR(), targetSurface->surfaceSize))
		{
		case CAM_SUBMIT_BAD_HANDLE:
			return CAM_ERROR_INVALID_HANDLE;
		case CAM_SUBMIT_FULL:
			cemuLog_logDebugOnce(LogType::Force, "CAMSubmitTargetSurface: {} surfaces queued already, this one dropped", CAM_STATE_QUEUE);
			break;
		default:
			break;
		}
		return CAM_ERROR_SUCCESS;
	}

	class : public COSModule
	{
		public:
		std::string_view GetName() override
		{
			return "camera";
		}

		void RPLMapped() override
		{
			cafeExportRegister("camera", CAMGetMemReq, LogType::SysApi);
			cafeExportRegister("camera", CAMCheckMemSegmentation, LogType::SysApi);
			cafeExportRegister("camera", CAMInit, LogType::SysApi);
			cafeExportRegister("camera", CAMExit, LogType::SysApi);
			cafeExportRegister("camera", CAMOpen, LogType::SysApi);
			cafeExportRegister("camera", CAMClose, LogType::SysApi);
			cafeExportRegister("camera", CAMSubmitTargetSurface, LogType::SysApi);
		};

		void rpl_entry(uint32 moduleHandle, coreinit::RplEntryReason reason) override
		{
			if (reason == coreinit::RplEntryReason::Loaded)
			{
				// what a title before left is gone: no handle of its taken,
				// no surface of its handed to this one's handler
				cam_state_reset();
			}
			else if (reason == coreinit::RplEntryReason::Unloaded)
			{
				// todo
			}
		}
	}s_COScameraModule;

	COSModule* GetModule()
	{
		return &s_COScameraModule;
	}
}

