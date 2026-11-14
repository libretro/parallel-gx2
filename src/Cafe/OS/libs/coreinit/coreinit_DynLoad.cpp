#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/HW/Espresso/PPCCallback.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/OS/RPL/rpl_structs.h"
#include "Cafe/OS/libs/coreinit/coreinit_DynLoad.h"
#include "Cafe/OS/libs/coreinit/coreinit_MEM.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/OS/libs/coreinit/coreinit_Scheduler.h"

namespace coreinit
{
	MPTR _osDynLoadFuncAlloc = MPTR_NULL;
	MPTR _osDynLoadFuncFree = MPTR_NULL;
	MPTR _osDynLoadTLSFuncAlloc = MPTR_NULL;
	MPTR _osDynLoadTLSFuncFree = MPTR_NULL;

	// The loader is the title's to share between its threads, as on the
	// console: every OSDynLoad call that loads, unloads or looks at the
	// modules or the notify callbacks holds this guest mutex. It is
	// recursive, so a module's entry point or a notify callback that calls
	// OSDynLoad on the same thread goes on.
	SysAllocator<OSMutex> s_dynLoadMutex;

	class DynLoadLock
	{
	public:
		DynLoadLock()
		{
			// as OSLockMutex, but not a point where the thread may be cancelled
			__OSLockScheduler();
			OSLockMutexInternal(s_dynLoadMutex.GetPtr());
			__OSUnlockScheduler();
		}
		~DynLoadLock() { OSUnlockMutex(s_dynLoadMutex.GetPtr()); }
		DynLoadLock(const DynLoadLock&) = delete;
		DynLoadLock& operator=(const DynLoadLock&) = delete;
	};

	// fixed, so a callback adding or removing one while the list is walked
	// moves nothing under the walk, and nothing is allocated
	static constexpr sint32 DYNLOAD_MAX_NOTIFY_CALLBACKS = 32;
	static NotifyCallbackEntry s_notifyCallbacks[DYNLOAD_MAX_NOTIFY_CALLBACKS];
	static sint32 s_notifyCallbackCount = 0;

	uint32 OSDynLoad_SetAllocator(MPTR allocFunc, MPTR freeFunc)
	{
		_osDynLoadFuncAlloc = allocFunc;
		_osDynLoadFuncFree = freeFunc;
		return 0;
	}

	void OSDynLoad_SetTLSAllocator(MPTR allocFunc, MPTR freeFunc)
	{
		_osDynLoadTLSFuncAlloc = allocFunc;
		_osDynLoadTLSFuncFree = freeFunc;
	}

	uint32 OSDynLoad_GetAllocator(betype<MPTR>* funcAlloc, betype<MPTR>* funcFree)
	{
		*funcAlloc = _osDynLoadFuncAlloc;
		*funcFree = _osDynLoadFuncFree;
		return 0;
	}

	void OSDynLoad_GetTLSAllocator(betype<MPTR>* funcAlloc, betype<MPTR>* funcFree)
	{
		*funcAlloc = _osDynLoadTLSFuncAlloc;
		*funcFree = _osDynLoadTLSFuncFree;
	}

	void* OSDynLoad_AllocatorAlloc(sint32 size, sint32 alignment)
	{
		if (_osDynLoadFuncAlloc == MPTR_NULL)
			return MPTR_NULL;
		StackAllocator<MEMPTR<void>> _ptrStorage;
		int r = PPCCoreCallback(_osDynLoadFuncAlloc, size, alignment, _ptrStorage.GetMPTR());
		if (r != 0)
		{
			cemu_assert_debug(false);
			return MPTR_NULL;
		}
		return _ptrStorage->GetPtr();
	}

	void OSDynLoad_AllocatorFree(void* mem)
	{
		if (_osDynLoadFuncFree == MPTR_NULL)
			return;
		MEMPTR<void> _mem = mem;
		PPCCoreCallback(_osDynLoadFuncFree, _mem);
	}

	uint32 OSDynLoad_AcquireInternal(const char* libName, uint32be* moduleHandleOut, bool checkOnly)
	{
		DynLoadLock lock;
		// truncate path
		sint32 fileNameStartIndex = 0;
		sint32 tempLen = (sint32)strlen(libName);
		for (sint32 i = tempLen - 1; i >= 0; i--)
		{
			if (libName[i] == '/')
			{
				fileNameStartIndex = i + 1;
				break;
			}
		}
		// truncate file extension
		char tempLibName[512];
		strcpy(tempLibName, libName + fileNameStartIndex);
		tempLen = (sint32)strlen(tempLibName);
		for (sint32 i = tempLen - 1; i >= 0; i--)
		{
			if (tempLibName[i] == '.')
			{
				tempLibName[i] = '\0';
				break;
			}
		}
		// search for loaded modules with matching name
		uint32 rplHandle = RPLLoader_GetHandleByModuleName(libName);
		if (rplHandle == RPL_INVALID_HANDLE && !RPLLoader_HasDependency(libName))
		{
			if (checkOnly)
			{
				*moduleHandleOut = 0;
				return 0;
			}
			RPLLoader_AddDependency(libName);
			RPLLoader_UpdateDependencies();
			RPLLoader_Link();
			RPLLoader_CallEntrypoints();
			rplHandle = RPLLoader_GetHandleByModuleName(libName);
		}
		if (rplHandle == RPL_INVALID_HANDLE)
			*moduleHandleOut = 0;
		else
			*moduleHandleOut = rplHandle;
		if (rplHandle == RPL_INVALID_HANDLE)
		{
			cemuLog_logDebug(LogType::Force, "OSDynLoad_Acquire() failed to load module '{}'", libName);
			return 0xFFFCFFE9; // module not found
		}

		for (sint32 i = 0; i < s_notifyCallbackCount; i++)
		{
			const NotifyCallbackEntry cb = s_notifyCallbacks[i];
			MEMPTR<OSDynLoad_NotifyData> notifyData;
			notifyData = (OSDynLoad_NotifyData*)OSDynLoad_AllocatorAlloc(sizeof(OSDynLoad_NotifyData), 4);
			RPLModule* module = RPLLoader_GetModuleByName(tempLibName);

			if (!module) 
				break;

			notifyData->name = module->ppcName.GetMPTR();

			notifyData->textAddr = module->regionMappingBase_text.GetBEValue();
			notifyData->textOffset = module->regionMappingBase_text.GetMPTR() - module->regionOrigAddr_text;
			notifyData->textSize = module->regionSize_text;

			notifyData->dataAddr = module->regionMappingBase_data;
			notifyData->dataOffset = module->regionMappingBase_data - module->regionOrigAddr_data;
			notifyData->dataSize = module->regionSize_data;

			notifyData->readAddr = module->regionMappingBase_data;
			notifyData->readOffset = module->regionMappingBase_data - module->regionOrigAddr_data;
			notifyData->readSize = module->regionSize_data;

			PPCCoreCallback(cb.callback, moduleHandleOut, cb.userContext.GetMPTR(), 1, notifyData.GetMPTR());
			OSDynLoad_AllocatorFree(notifyData);

		}
		return 0;
	}

	uint32 OSDynLoad_Acquire(const char* libName, uint32be* moduleHandleOut)
	{
		return OSDynLoad_AcquireInternal(libName, moduleHandleOut, false);
	}

	void OSDynLoad_Release(uint32 moduleHandle)
	{
		if (moduleHandle == RPL_INVALID_HANDLE)
			return;
		DynLoadLock lock;

		for (sint32 i = 0; i < s_notifyCallbackCount; i++)
		{
			const NotifyCallbackEntry cb = s_notifyCallbacks[i];
			MEMPTR<OSDynLoad_NotifyData> notifyData;
			notifyData = (OSDynLoad_NotifyData*)OSDynLoad_AllocatorAlloc(sizeof(OSDynLoad_NotifyData), 4);
			RPLModule* module = RPLLoader_GetModuleByName(RPLLoader_GetModuleNameByHandle(moduleHandle));

			if (!module) 
				break;

			notifyData->name = module->ppcName.GetMPTR();

			notifyData->textAddr = module->regionMappingBase_text.GetBEValue();
			notifyData->textOffset = module->regionMappingBase_text.GetMPTR() - module->regionOrigAddr_text;
			notifyData->textSize = module->regionSize_text;

			notifyData->dataAddr = module->regionMappingBase_data;
			notifyData->dataOffset = module->regionMappingBase_data - module->regionOrigAddr_data;
			notifyData->dataSize = module->regionSize_data;

			notifyData->readAddr = module->regionMappingBase_data;
			notifyData->readOffset = module->regionMappingBase_data - module->regionOrigAddr_data;
			notifyData->readSize = module->regionSize_data;

			PPCCoreCallback(cb.callback, moduleHandle, cb.userContext.GetMPTR(), 2, notifyData.GetMPTR());
			OSDynLoad_AllocatorFree(notifyData);

		}

		RPLLoader_RemoveDependency(moduleHandle);
		RPLLoader_UpdateDependencies();
	}

	uint32 OSDynLoad_IsModuleLoaded(const char* libName, uint32be* moduleHandleOut)
	{
		return OSDynLoad_AcquireInternal(libName, moduleHandleOut, true);
	}

	uint32 OSDynLoad_FindExport(uint32 moduleHandle, uint32 isData, const char* exportName, betype<MPTR>* addrOut)
	{
		DynLoadLock lock;
		if (moduleHandle == 0xFFFFFFFF)
		{
			// main module
			// Assassins Creed 4 has this handle hardcoded
			moduleHandle = RPLLoader_GetMainModuleHandle();
		}

		MPTR exportResult = RPLLoader_FindModuleOrHLEExport(moduleHandle, isData, exportName);
		*addrOut = exportResult;

		if (exportResult == MPTR_NULL)
			return 0xFFFFFFFF;
		return 0;
	}

	uint32 OSDynLoad_GetModuleName(uint32 moduleHandle, char* nameBuf, sint32be* nameBufSize) 
	{
		DynLoadLock lock;
		if (moduleHandle == 0xFFFFFFFF)
		{
			// main module
			// Assassins Creed 4 has this handle hardcoded
			moduleHandle = RPLLoader_GetMainModuleHandle();
		}

		const std::string moduleName = RPLLoader_GetModuleNameByHandle(moduleHandle);
		std::strncpy(nameBuf, moduleName.c_str(), *nameBufSize);

		return 0;

	}

	sint32 OSDynLoad_GetNumberOfRPLs()
	{
		DynLoadLock lock;
		return (sint32)RPLLoader_GetModuleList().size();
	}

	uint32 OSDynLoad_GetRPLInfo(uint32 first, uint32 count, OSDynLoad_NotifyData* outInfos) 
	{
		if (count == 0)
			return 1;
		DynLoadLock lock;
		auto modules = RPLLoader_GetModuleList();
		for (uint32 i = first; i < first + count; i++)
		{
			auto& outEntry = outInfos[i - first];
			if (i >= modules.size())
			{
				memset(&outEntry, 0, sizeof(OSDynLoad_NotifyData));
				continue;
			}
			outEntry.name = modules[i]->ppcName.GetMPTR();

			outEntry.textAddr = modules[i]->regionMappingBase_text.GetBEValue();
			outEntry.textOffset = modules[i]->regionMappingBase_text.GetMPTR() - modules[i]->regionOrigAddr_text;
			outEntry.textSize = modules[i]->regionSize_text;

			outEntry.dataAddr = modules[i]->regionMappingBase_data;
			outEntry.dataOffset = modules[i]->regionMappingBase_data - modules[i]->regionOrigAddr_data;
			outEntry.dataSize = modules[i]->regionSize_data;

			outEntry.readAddr = modules[i]->regionMappingBase_data;
			outEntry.readOffset = modules[i]->regionMappingBase_data - modules[i]->regionOrigAddr_data;
			outEntry.readSize = modules[i]->regionSize_data;
		}

		return 1;
	}

	uint32 OSDynLoad_AddNotifyCallback(MEMPTR<OSDynLoadNotifyFunc> notifyFn, MEMPTR<void> userContext)
	{
		if (!notifyFn)
			return 0xBAD1000E; // notify function pointer is null

		DynLoadLock lock;
		if (s_notifyCallbackCount >= DYNLOAD_MAX_NOTIFY_CALLBACKS)
		{
			cemuLog_log(LogType::Force, "OSDynLoad_AddNotifyCallback: more than {} callbacks", DYNLOAD_MAX_NOTIFY_CALLBACKS);
			return 0xBAD10019; // out of memory
		}
		s_notifyCallbacks[s_notifyCallbackCount++] = NotifyCallbackEntry(notifyFn, userContext);
		return 0;
	}

	uint32 OSDynLoad_DelNotifyCallback(MEMPTR<OSDynLoadNotifyFunc> notifyFn, MEMPTR<void> userContext) 
	{
		DynLoadLock lock;
		sint32 kept = 0;
		for (sint32 i = 0; i < s_notifyCallbackCount; i++)
		{
			const NotifyCallbackEntry& entry = s_notifyCallbacks[i];
			if (entry.callback.GetMPTR() == notifyFn.GetMPTR() && entry.userContext == userContext)
				continue;
			s_notifyCallbacks[kept++] = entry;
		}
		s_notifyCallbackCount = kept;
		return 0;
	}

	void InitializeDynLoad()
	{
		// each title starts with none
		OSInitMutex(s_dynLoadMutex.GetPtr());
		s_notifyCallbackCount = 0;

		cafeExportRegister("coreinit", OSDynLoad_SetAllocator, LogType::Placeholder);
		cafeExportRegister("coreinit", OSDynLoad_SetTLSAllocator, LogType::Placeholder);
		cafeExportRegister("coreinit", OSDynLoad_GetAllocator, LogType::Placeholder);
		cafeExportRegister("coreinit", OSDynLoad_GetTLSAllocator, LogType::Placeholder);

		cafeExportRegister("coreinit", OSDynLoad_Acquire, LogType::Placeholder);
		cafeExportRegister("coreinit", OSDynLoad_Release, LogType::Placeholder);
		cafeExportRegister("coreinit", OSDynLoad_IsModuleLoaded, LogType::Placeholder);
		cafeExportRegister("coreinit", OSDynLoad_FindExport, LogType::Placeholder);
		cafeExportRegister("coreinit", OSDynLoad_GetModuleName, LogType::Placeholder);
		cafeExportRegister("coreinit", OSDynLoad_GetNumberOfRPLs, LogType::Placeholder);
		cafeExportRegister("coreinit", OSDynLoad_GetRPLInfo, LogType::Placeholder);

		cafeExportRegister("coreinit", OSDynLoad_AddNotifyCallback, LogType::Placeholder);
		cafeExportRegister("coreinit", OSDynLoad_DelNotifyCallback, LogType::Placeholder);
	}
}
