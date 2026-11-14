#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/HW/Espresso/PPCCallback.h"
#include "nsyshid.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Backend.h"
#include "Whitelist.h"
#include "hid_core.h"
#include "Skylander.h"
#include "Infinity.h"
#include "Dimensions.h"

namespace nsyshid
{
	// touched only as the module loads and unloads
	std::shared_ptr<Backend> s_backends[2];

	typedef struct _HIDClient_t
	{
		uint32be callbackFunc; // attach/detach callback
	} HIDClient_t;

	// what a device slot holds (hid_core.h)
	struct DeviceHolder
	{
		std::shared_ptr<Device> device;
	};

	static void ReleaseDeviceHolder(void* obj)
	{
		delete static_cast<DeviceHolder*>(obj);
	}

	static Device* SlotDevice(int slot)
	{
		return static_cast<DeviceHolder*>(hid_core_obj(slot))->device.get();
	}

	// a held device slot, put back when it goes
	struct HeldDevice
	{
		int slot;
		explicit HeldDevice(int s) : slot(s) {}
		~HeldDevice() { hid_core_put(slot); }
		HeldDevice(const HeldDevice&) = delete;
		HeldDevice& operator=(const HeldDevice&) = delete;
		Device* operator->() const { return SlotDevice(slot); }
	};

	const int HID_MAX_NUM_DEVICES = HID_CORE_DEVICES;

	SysAllocator<HID_t, HID_MAX_NUM_DEVICES> HIDPool;

	static int GetDeviceSlotByHandle(uint32 handle, bool openIfClosed)
	{
		const int slot = hid_core_get_handle(handle);
		if (slot < 0)
			return -1;
		Device* device = SlotDevice(slot);
		if (openIfClosed && !device->IsOpened() && !device->Open())
		{
			hid_core_put(slot);
			return -1;
		}
		return slot;
	}

	const int HID_CALLBACK_DETACH = 0;
	const int HID_CALLBACK_ATTACH = 1;

	static HIDClient_t* ClientPtr(uint32 client)
	{
		return (HIDClient_t*)memory_getPointerFromVirtualOffset(client);
	}

	static MPTR SlotHID(int slot)
	{
		return memory_getVirtualOffsetFromPointer(HIDPool.GetPtr() + slot);
	}

	static void DoClientCallback(uint32 client, int slot, int what, bool async)
	{
		HIDClient_t* hidClient = ClientPtr(client);
		if (async)
			coreinitAsyncCallback_add(hidClient->callbackFunc, 3, client, SlotHID(slot), what);
		else
			PPCCoreCallback(hidClient->callbackFunc, client, SlotHID(slot), what);
	}

	void AttachBackend(const std::shared_ptr<Backend>& backend)
	{
		for (auto& b : s_backends)
		{
			if (!b)
			{
				b = backend;
				break;
			}
		}
		backend->OnAttach();
	}

	void DetachBackend(const std::shared_ptr<Backend>& backend)
	{
		for (auto& b : s_backends)
		{
			if (b == backend)
				b.reset();
		}
		backend->OnDetach();
	}

	void DetachAllBackends()
	{
		for (auto& b : s_backends)
		{
			std::shared_ptr<Backend> backend = std::move(b);
			b.reset();
			if (backend)
				backend->OnDetach();
		}
	}

	void AttachDefaultBackends()
	{
		backend::AttachDefaultBackends();
	}

	bool AttachDevice(const std::shared_ptr<Device>& device, const Backend* owner)
	{
		DeviceHolder* holder = new DeviceHolder{device};
		uint32 handle = 0;
		const int slot = hid_core_attach(device.get(), holder, owner,
			HID_CORE_ID(device->m_vendorId, device->m_productId), &handle);
		if (slot < 0)
		{
			delete holder;
			cemuLog_logDebug(LogType::Force,
							 "nsyshid.AttachDevice(): failed to attach device: {:04x}:{:04x}: {}",
							 device->m_vendorId,
							 device->m_productId,
							 slot == -1 ? "already attached" : "no free device slots left");
			return false;
		}
		HID_t* hidDevice = HIDPool.GetPtr() + slot;
		hidDevice->handle = handle;
		device->AssignHID(hidDevice);

		int cursor = 0;
		while (uint32 client = hid_core_client_next(&cursor))
			DoClientCallback(client, slot, HID_CALLBACK_ATTACH, true);
		hid_core_put(slot);

		cemuLog_logDebug(LogType::Force, "nsyshid.AttachDevice(): device attached: {:04x}:{:04x}",
						 device->m_vendorId,
						 device->m_productId);
		return true;
	}

	void DetachDevice(const std::shared_ptr<Device>& device)
	{
		// held across the callbacks, so its HID_t is not handed out again
		const int slot = hid_core_get_key(device.get());
		if (slot < 0 || !hid_core_detach(device.get()))
		{
			if (slot >= 0)
				hid_core_put(slot);
			cemuLog_logDebug(LogType::Force, "nsyshid.DetachDevice(): device not found: {:04x}:{:04x}",
							 device->m_vendorId,
							 device->m_productId);
			return;
		}
		int cursor = 0;
		while (uint32 client = hid_core_client_next(&cursor))
			DoClientCallback(client, slot, HID_CALLBACK_DETACH, true);
		hid_core_put(slot);

		device->Close();

		cemuLog_logDebug(LogType::Force, "nsyshid.DetachDevice(): device removed: {:04x}:{:04x}",
						 device->m_vendorId,
						 device->m_productId);
	}

	std::shared_ptr<Device> FindDeviceById(uint16 vendorId, uint16 productId)
	{
		const int slot = hid_core_get_id(HID_CORE_ID(vendorId, productId));
		if (slot < 0)
			return nullptr;
		std::shared_ptr<Device> device = static_cast<DeviceHolder*>(hid_core_obj(slot))->device;
		hid_core_put(slot);
		return device;
	}

	void export_HIDAddClient(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamTypePtr(hidClient, HIDClient_t, 0);
		ppcDefineParamMPTR(callbackFuncMPTR, 1);
		cemuLog_logDebug(LogType::Force, "nsyshid.HIDAddClient(0x{:08x},0x{:08x})", hCPU->gpr[3], hCPU->gpr[4]);
		hidClient->callbackFunc = callbackFuncMPTR;

		const uint32 client = memory_getVirtualOffsetFromPointer(hidClient);
		if (!hid_core_client_add(client))
			cemuLog_log(LogType::Force, "nsyshid.HIDAddClient(): no room for another client");

		// do attach callbacks
		int cursor = 0;
		for (int slot; (slot = hid_core_next(&cursor, nullptr)) >= 0;)
		{
			DoClientCallback(client, slot, HID_CALLBACK_ATTACH, false);
			hid_core_put(slot);
		}

		osLib_returnFromFunction(hCPU, 0);
	}

	void export_HIDDelClient(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamTypePtr(hidClient, HIDClient_t, 0);
		cemuLog_logDebug(LogType::Force, "nsyshid.HIDDelClient(0x{:08x})", hCPU->gpr[3]);

		const uint32 client = memory_getVirtualOffsetFromPointer(hidClient);
		hid_core_client_del(client);

		// do detach callbacks
		int cursor = 0;
		for (int slot; (slot = hid_core_next(&cursor, nullptr)) >= 0;)
		{
			DoClientCallback(client, slot, HID_CALLBACK_DETACH, false);
			hid_core_put(slot);
		}

		osLib_returnFromFunction(hCPU, 0);
	}

	void DoHIDTransferCallback(MPTR callbackFuncMPTR, MPTR callbackParamMPTR, uint32 hidHandle, uint32 errorCode,
							   MPTR buffer, sint32 length)
	{
		coreinitAsyncCallback_add(callbackFuncMPTR, 5, hidHandle, errorCode, buffer, length, callbackParamMPTR);
	}

	// the transfers the workers carry out (hid_core.h)
	enum
	{
		REQ_GET_DESCRIPTOR,
		REQ_SET_IDLE,
		REQ_SET_PROTOCOL,
		REQ_SET_REPORT,
		REQ_READ,
		REQ_WRITE,
	};

	sint32 _hidReadInternalSync(Device* device, uint8* data, sint32 maxLength);
	sint32 _hidWriteInternalSync(Device* device, uint8* data, sint32 maxLength);

	// a request's arguments: arg[0] the callback (0: synchronous, with
	// ptr[1] the event to signal and ptr[2] where the result goes), arg[1]
	// its parameter, ptr[0] the guest buffer
	static void RunRequest(const hid_core_req_t* req)
	{
		Device* device = SlotDevice(req->slot);
		uint8* data = (uint8*)req->ptr[0];
		const MPTR cb = req->arg[0];
		const MPTR param = req->arg[1];
		const uint32 a2 = req->arg[2], a3 = req->arg[3], a4 = req->arg[4], a5 = req->arg[5];
		sint32 result = 0;
		sint32 errorCode = 0;
		MPTR buffer = 0;
		sint32 length = 0;
		switch (req->kind)
		{
		case REQ_GET_DESCRIPTOR:
			errorCode = device->GetDescriptor((uint8)a2, (uint8)a3, (uint16)a4, data, a5) ? 0 : -1;
			break;
		case REQ_SET_IDLE:
			errorCode = device->SetIdle((uint8)a2, (uint8)a3, (uint8)a4) ? 0 : -1;
			break;
		case REQ_SET_PROTOCOL:
			errorCode = device->SetProtocol((uint8)a2, (uint8)a3) ? 0 : -1;
			break;
		case REQ_SET_REPORT:
		{
			ReportMessage message((uint8)a2, (uint8)a3, data, a4);
			const bool ok = device->SetReport(&message);
			result = ok ? (sint32)a4 : 0;
			errorCode = ok ? 0 : -1;
			buffer = memory_getVirtualOffsetFromPointer(data);
			length = (sint32)a4;
			break;
		}
		case REQ_READ:
		case REQ_WRITE:
			result = req->kind == REQ_READ ? _hidReadInternalSync(device, data, (sint32)a2)
										   : _hidWriteInternalSync(device, data, (sint32)a2);
			errorCode = result < 0 ? result : 0; // don't return number of bytes in error code
			buffer = memory_getVirtualOffsetFromPointer(data);
			length = result > 0 ? result : 0;
			break;
		}
		if (cb != MPTR_NULL)
		{
			DoHIDTransferCallback(cb, param, req->handle, errorCode, buffer, length);
		}
		else
		{
			*(sint32*)req->ptr[2] = result;
			coreinit::OSSignalEvent((coreinit::OSEvent*)req->ptr[1]);
		}
	}

	// hand a request to the workers, or run it here when they cannot take
	// it; the slot goes with it. For a synchronous one, the result.
	static sint32 IssueRequest(int slot, int kind, MPTR cb, MPTR param, uint8* data,
							   uint32 a2, uint32 a3 = 0, uint32 a4 = 0, uint32 a5 = 0)
	{
		hid_core_req_t req = {};
		req.slot = slot;
		req.kind = kind;
		req.handle = hid_core_handle(slot);
		req.arg[0] = cb;
		req.arg[1] = param;
		req.arg[2] = a2;
		req.arg[3] = a3;
		req.arg[4] = a4;
		req.arg[5] = a5;
		req.ptr[0] = data;
		sint32 result = 0;
		StackAllocator<coreinit::OSEvent> event;
		if (cb == MPTR_NULL)
		{
			coreinit::OSInitEvent(&event, coreinit::OSEvent::EVENT_STATE::STATE_NOT_SIGNALED, coreinit::OSEvent::EVENT_MODE::MODE_AUTO);
			req.ptr[1] = event.GetPointer();
			req.ptr[2] = &result;
		}
		if (hid_core_submit(&req))
		{
			if (cb == MPTR_NULL)
				coreinit::OSWaitEvent(&event);
			return result;
		}
		RunRequest(&req);
		if (cb == MPTR_NULL)
			coreinit::OSWaitEvent(&event);
		hid_core_put(slot);
		return result;
	}

	void export_HIDGetDescriptor(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamU32(hidHandle, 0);	   // r3
		ppcDefineParamU8(descType, 1);		   // r4
		ppcDefineParamU8(descIndex, 2);		   // r5
		ppcDefineParamU16(lang, 3);			   // r6
		ppcDefineParamUStr(output, 4);		   // r7
		ppcDefineParamU32(outputMaxLength, 5); // r8
		ppcDefineParamMPTR(cbFuncMPTR, 6);	   // r9
		ppcDefineParamMPTR(cbParamMPTR, 7);	   // r10
		cemuLog_logDebug(LogType::Force, "nsyshid.HIDGetDescriptor(0x{:08x}, 0x{:02x}, 0x{:02x}, 0x{:04x}, 0x{:x}, 0x{:08x}, 0x{:08x}, 0x{:08x})",
					hCPU->gpr[3], hCPU->gpr[4], hCPU->gpr[5], hCPU->gpr[6], hCPU->gpr[7], hCPU->gpr[8], hCPU->gpr[9], hCPU->gpr[10]);

		const int slot = GetDeviceSlotByHandle(hidHandle, true);
		if (slot < 0)
		{
			cemuLog_log(LogType::Force, "nsyshid.HIDGetDescriptor(): Unable to find device with hid handle {}", hidHandle);
			osLib_returnFromFunction(hCPU, -1);
			return;
		}

		// issue request (synchronous or asynchronous)
		sint32 returnCode = 0;
		if (cbFuncMPTR == MPTR_NULL)
		{
			HeldDevice device(slot);
			returnCode = device->GetDescriptor(descType, descIndex, lang, output, outputMaxLength) ? (sint32)outputMaxLength : -1;
		}
		else
		{
			IssueRequest(slot, REQ_GET_DESCRIPTOR, cbFuncMPTR, cbParamMPTR, output, descType, descIndex, lang, outputMaxLength);
		}
		osLib_returnFromFunction(hCPU, returnCode);
	}

	void export_HIDSetIdle(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamU32(hidHandle, 0);		  // r3
		ppcDefineParamU8(ifIndex, 1);			  // r4
		ppcDefineParamU8(reportId, 2);			  // r5
		ppcDefineParamU8(duration, 3);			  // r6
		ppcDefineParamMPTR(callbackFuncMPTR, 4);  // r7
		ppcDefineParamMPTR(callbackParamMPTR, 5); // r8
		cemuLog_logDebug(LogType::Force, "nsyshid.HIDSetIdle(0x{:08x}, 0x{:02x}, 0x{:02x}, 0x{:02x}, 0x{:08x}, 0x{:08x})", hCPU->gpr[3],
					hCPU->gpr[4], hCPU->gpr[5], hCPU->gpr[6], hCPU->gpr[7], hCPU->gpr[8]);

		const int slot = GetDeviceSlotByHandle(hidHandle, true);
		if (slot < 0)
		{
			cemuLog_log(LogType::Force, "nsyshid.HIDSetIdle(): Unable to find device with hid handle {}", hidHandle);
			osLib_returnFromFunction(hCPU, -1);
			return;
		}

		// issue request (synchronous or asynchronous)
		sint32 returnCode = 0;
		if (callbackFuncMPTR == MPTR_NULL)
		{
			HeldDevice device(slot);
			returnCode = device->SetIdle(ifIndex, reportId, duration) ? 0 : -1;
		}
		else
		{
			IssueRequest(slot, REQ_SET_IDLE, callbackFuncMPTR, callbackParamMPTR, nullptr, ifIndex, reportId, duration);
		}
		osLib_returnFromFunction(hCPU, returnCode);
	}

	void export_HIDSetProtocol(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamU32(hidHandle, 0);		  // r3
		ppcDefineParamU8(ifIndex, 1);			  // r4
		ppcDefineParamU8(protocol, 2);			  // r5
		ppcDefineParamMPTR(callbackFuncMPTR, 3);  // r6
		ppcDefineParamMPTR(callbackParamMPTR, 4); // r7
		cemuLog_logDebug(LogType::Force, "nsyshid.HIDSetProtocol(0x{:08x}, 0x{:02x}, 0x{:02x}, 0x{:08x}, 0x{:08x})", hCPU->gpr[3],
					hCPU->gpr[4], hCPU->gpr[5], hCPU->gpr[6], hCPU->gpr[7]);

		const int slot = GetDeviceSlotByHandle(hidHandle, true);
		if (slot < 0)
		{
			cemuLog_log(LogType::Force, "nsyshid.HIDSetProtocol(): Unable to find device with hid handle {}", hidHandle);
			osLib_returnFromFunction(hCPU, -1);
			return;
		}
		// issue request (synchronous or asynchronous)
		sint32 returnCode = 0;
		if (callbackFuncMPTR == MPTR_NULL)
		{
			HeldDevice device(slot);
			returnCode = device->SetProtocol(ifIndex, protocol) ? 0 : -1;
		}
		else
		{
			IssueRequest(slot, REQ_SET_PROTOCOL, callbackFuncMPTR, callbackParamMPTR, nullptr, ifIndex, protocol);
		}
		osLib_returnFromFunction(hCPU, returnCode);
	}

	void export_HIDSetReport(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamU32(hidHandle, 0);		  // r3
		ppcDefineParamU8(reportType, 1);		  // r4
		ppcDefineParamU8(reportId, 2);			  // r5
		ppcDefineParamUStr(data, 3);			  // r6
		ppcDefineParamU32(dataLength, 4);		  // r7
		ppcDefineParamMPTR(callbackFuncMPTR, 5);  // r8
		ppcDefineParamMPTR(callbackParamMPTR, 6); // r9
		cemuLog_logDebug(LogType::Force, "nsyshid.HIDSetReport(0x{:08x}, 0x{:02x}, 0x{:02x}, 0x{:08x}, 0x{:08x}, 0x{:08x}, 0x{:08x})", hCPU->gpr[3],
					hCPU->gpr[4], hCPU->gpr[5], hCPU->gpr[6], hCPU->gpr[7], hCPU->gpr[8], hCPU->gpr[9]);

#ifdef CEMU_DEBUG_ASSERT
		if (reportType != 2 || reportId != 0)
			assert_dbg();
#endif

		const int slot = GetDeviceSlotByHandle(hidHandle, true);
		if (slot < 0)
		{
			cemuLog_log(LogType::Force, "nsyshid.HIDSetReport(): Unable to find device with hid handle {}", hidHandle);
			osLib_returnFromFunction(hCPU, -1);
			return;
		}

		// synchronous: the length set, or 0; asynchronous: 0
		const sint32 returnCode = IssueRequest(slot, REQ_SET_REPORT, callbackFuncMPTR, callbackParamMPTR, data,
											   reportType, reportId, dataLength);
		osLib_returnFromFunction(hCPU, returnCode);
	}

	sint32 _hidReadInternalSync(Device* device, uint8* data, sint32 maxLength)
	{
		cemuLog_logDebug(LogType::Force, "HidRead Begin (Length 0x{:08x})", maxLength);
		if (!device->IsOpened())
		{
			cemuLog_logDebug(LogType::Force, "nsyshid.hidReadInternalSync(): cannot read from a non-opened device");
			return -1;
		}
		memset(data, 0, maxLength);
		ReadMessage message(data, maxLength, 0);
		Device::ReadResult readResult = device->Read(&message);
		switch (readResult)
		{
		case Device::ReadResult::Success:
		{
			cemuLog_logDebug(LogType::Force, "nsyshid.hidReadInternalSync(): read {} of {} bytes",
							 message.bytesRead,
							 maxLength);
			return message.bytesRead;
		}
		break;
		case Device::ReadResult::Error:
		{
			cemuLog_logDebug(LogType::Force, "nsyshid.hidReadInternalSync(): read error");
			return -1;
		}
		break;
		case Device::ReadResult::ErrorTimeout:
		{
			cemuLog_logDebug(LogType::Force, "nsyshid.hidReadInternalSync(): read error: timeout");
			return -108;
		}
		break;
		}
		cemuLog_logDebug(LogType::Force, "nsyshid.hidReadInternalSync(): read error: unknown");
		return -1;
	}

	void export_HIDRead(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamU32(hidHandle, 0);		  // r3
		ppcDefineParamUStr(data, 1);			  // r4
		ppcDefineParamU32(maxLength, 2);		  // r5
		ppcDefineParamMPTR(callbackFuncMPTR, 3);  // r6
		ppcDefineParamMPTR(callbackParamMPTR, 4); // r7
		cemuLog_logDebug(LogType::Force, "nsyshid.HIDRead(0x{:x},0x{:08x},0x{:08x},0x{:08x},0x{:08x})", hCPU->gpr[3],
					hCPU->gpr[4], hCPU->gpr[5], hCPU->gpr[6], hCPU->gpr[7]);

		const int slot = GetDeviceSlotByHandle(hidHandle, true);
		if (slot < 0)
		{
			cemuLog_log(LogType::Force, "nsyshid.HIDRead(): Unable to find device with hid handle {}", hidHandle);
			osLib_returnFromFunction(hCPU, -1);
			return;
		}
		// synchronous: what was read, or the error; asynchronous: 0
		const sint32 returnCode = IssueRequest(slot, REQ_READ, callbackFuncMPTR, callbackParamMPTR, data, maxLength);
		osLib_returnFromFunction(hCPU, returnCode);
	}

	sint32 _hidWriteInternalSync(Device* device, uint8* data, sint32 maxLength)
	{
		cemuLog_logDebug(LogType::Force, "HidWrite Begin (Length 0x{:08x})", maxLength);
		if (!device->IsOpened())
		{
			cemuLog_logDebug(LogType::Force, "nsyshid.hidWriteInternalSync(): cannot write to a non-opened device");
			return -1;
		}
		WriteMessage message(data, maxLength, 0);
		Device::WriteResult writeResult = device->Write(&message);
		switch (writeResult)
		{
		case Device::WriteResult::Success:
		{
			cemuLog_logDebug(LogType::Force, "nsyshid.hidWriteInternalSync(): wrote {} of {} bytes", message.bytesWritten,
							 maxLength);
			return message.bytesWritten;
		}
		break;
		case Device::WriteResult::Error:
		{
			cemuLog_logDebug(LogType::Force, "nsyshid.hidWriteInternalSync(): write error");
			return -1;
		}
		break;
		case Device::WriteResult::ErrorTimeout:
		{
			cemuLog_logDebug(LogType::Force, "nsyshid.hidWriteInternalSync(): write error: timeout");
			return -108;
		}
		break;
		}
		cemuLog_logDebug(LogType::Force, "nsyshid.hidWriteInternalSync(): write error: unknown");
		return -1;
	}

	void export_HIDWrite(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamU32(hidHandle, 0);		  // r3
		ppcDefineParamUStr(data, 1);			  // r4
		ppcDefineParamU32(maxLength, 2);		  // r5
		ppcDefineParamMPTR(callbackFuncMPTR, 3);  // r6
		ppcDefineParamMPTR(callbackParamMPTR, 4); // r7
		cemuLog_logDebug(LogType::Force, "nsyshid.HIDWrite(0x{:x},0x{:08x},0x{:08x},0x{:08x},0x{:08x})", hCPU->gpr[3],
					hCPU->gpr[4], hCPU->gpr[5], hCPU->gpr[6], hCPU->gpr[7]);

		const int slot = GetDeviceSlotByHandle(hidHandle, true);
		if (slot < 0)
		{
			cemuLog_log(LogType::Force, "nsyshid.HIDWrite(): Unable to find device with hid handle {}", hidHandle);
			osLib_returnFromFunction(hCPU, -1);
			return;
		}
		// synchronous: what was written, or the error; asynchronous: 0
		const sint32 returnCode = IssueRequest(slot, REQ_WRITE, callbackFuncMPTR, callbackParamMPTR, data, maxLength);
		osLib_returnFromFunction(hCPU, returnCode);
	}

	void export_HIDDecodeError(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamU32(errorCode, 0);
		ppcDefineParamTypePtr(ukn0, uint32be, 1);
		ppcDefineParamTypePtr(ukn1, uint32be, 2);
		cemuLog_logDebug(LogType::Force, "nsyshid.HIDDecodeError(0x{:08x},0x{:08x},0x{:08x})", hCPU->gpr[3],
					hCPU->gpr[4], hCPU->gpr[5]);

		// todo
		*ukn0 = 0x3FF;
		*ukn1 = (uint32)-0x7FFF;

		osLib_returnFromFunction(hCPU, 0);
	}

	void Backend::DetachAllDevices()
	{
		int cursor = 0;
		for (int slot; (slot = hid_core_next(&cursor, this)) >= 0;)
		{
			std::shared_ptr<Device> device = static_cast<DeviceHolder*>(hid_core_obj(slot))->device;
			hid_core_put(slot);
			nsyshid::DetachDevice(device);
		}
	}

	bool Backend::AttachDevice(const std::shared_ptr<Device>& device)
	{
		if (!IsBackendAttached() || !nsyshid::AttachDevice(device, this))
			return false;
		// detached meanwhile: its devices went with it, this one too
		if (!IsBackendAttached())
		{
			nsyshid::DetachDevice(device);
			return false;
		}
		return true;
	}

	void Backend::DetachDevice(const std::shared_ptr<Device>& device)
	{
		nsyshid::DetachDevice(device);
	}

	std::shared_ptr<Device> Backend::FindDevice(std::function<bool(const std::shared_ptr<Device>&)> isWantedDevice)
	{
		int cursor = 0;
		for (int slot; (slot = hid_core_next(&cursor, this)) >= 0;)
		{
			std::shared_ptr<Device> device = static_cast<DeviceHolder*>(hid_core_obj(slot))->device;
			hid_core_put(slot);
			if (isWantedDevice(device))
				return device;
		}
		return nullptr;
	}

	std::shared_ptr<Device> Backend::FindDeviceById(uint16 vendorId, uint16 productId)
	{
		return nsyshid::FindDeviceById(vendorId, productId);
	}

	bool Backend::IsDeviceWhitelisted(uint16 vendorId, uint16 productId)
	{
		return Whitelist::GetInstance().IsDeviceWhitelisted(vendorId, productId);
	}

	Backend::Backend()
	{
		retro_atomic_int_init(&m_isAttached, 0);
	}

	void Backend::OnAttach()
	{
		retro_atomic_store_release_int(&m_isAttached, 1);
		AttachVisibleDevices();
	}

	void Backend::OnDetach()
	{
		retro_atomic_store_release_int(&m_isAttached, 0);
		DetachAllDevices();
	}

	bool Backend::IsBackendAttached()
	{
		return retro_atomic_load_acquire_int(&m_isAttached) != 0;
	}

	Device::Device(uint16 vendorId,
				   uint16 productId,
				   uint8 interfaceIndex,
				   uint8 interfaceSubClass,
				   uint8 protocol)
		: m_hid(nullptr),
		  m_vendorId(vendorId),
		  m_productId(productId),
		  m_interfaceIndex(interfaceIndex),
		  m_interfaceSubClass(interfaceSubClass),
		  m_protocol(protocol),
		  m_maxPacketSizeRX(0x20),
		  m_maxPacketSizeTX(0x20)
	{
	}

	void Device::AssignHID(HID_t* hid)
	{
		if (hid != nullptr)
		{
			hid->vendorId = this->m_vendorId;
			hid->productId = this->m_productId;
			hid->ifIndex = this->m_interfaceIndex;
			hid->subClass = this->m_interfaceSubClass;
			hid->protocol = this->m_protocol;
			hid->ukn04 = 0x11223344;
			hid->paddingGuessed0F = 0;
			hid->maxPacketSizeRX = this->m_maxPacketSizeRX;
			hid->maxPacketSizeTX = this->m_maxPacketSizeTX;
		}
		this->m_hid = hid;
	}

	class : public COSModule
	{
		public:
		std::string_view GetName() override
		{
			return "nsyshid";
		}

		void RPLMapped() override
		{
			osLib_addFunction("nsyshid", "HIDAddClient", export_HIDAddClient);
			osLib_addFunction("nsyshid", "HIDDelClient", export_HIDDelClient);
			osLib_addFunction("nsyshid", "HIDGetDescriptor", export_HIDGetDescriptor);
			osLib_addFunction("nsyshid", "HIDSetIdle", export_HIDSetIdle);
			osLib_addFunction("nsyshid", "HIDSetProtocol", export_HIDSetProtocol);
			osLib_addFunction("nsyshid", "HIDSetReport", export_HIDSetReport);

			osLib_addFunction("nsyshid", "HIDRead", export_HIDRead);
			osLib_addFunction("nsyshid", "HIDWrite", export_HIDWrite);

			osLib_addFunction("nsyshid", "HIDDecodeError", export_HIDDecodeError);

		};
		void rpl_entry(uint32 moduleHandle, coreinit::RplEntryReason reason) override
		{
			if (reason == coreinit::RplEntryReason::Loaded)
			{
				// initialise whitelist
				Whitelist::GetInstance();
				hid_core_set_release(ReleaseDeviceHolder);
				hid_core_clients_reset();
				// the emulated portals' queues before any transfer can reach them
				g_skyportal.Start();
				g_infinitybase.Start();
				g_dimensionstoypad.Start();
				hid_core_start(RunRequest);
				AttachDefaultBackends();
			}
			else if (reason == coreinit::RplEntryReason::Unloaded)
			{
				// the next title starts with no devices and no transfers; a
				// read waiting on an emulated portal returns before its worker
				// is joined
				g_skyportal.Close();
				g_infinitybase.Close();
				g_dimensionstoypad.Close();
				hid_core_stop();
				g_skyportal.Stop();
				g_infinitybase.Stop();
				g_dimensionstoypad.Stop();
				DetachAllBackends();
				hid_core_clients_reset();
			}
		}
	}s_COSnsyshidModule;

	COSModule* GetModule()
	{
		return &s_COSnsyshidModule;
	}

} // namespace nsyshid
