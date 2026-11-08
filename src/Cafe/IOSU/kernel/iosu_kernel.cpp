#include "iosu_kernel.h"
#include "util/helpers/helpers.h"
#include "util/helpers/EventWake.h"
#include "util/helpers/IndexFreeList.h"
#include "util/helpers/MpscQueue.h"
#include "util/helpers/RcuCell.h"
#include "Cafe/OS/libs/coreinit/coreinit_IPC.h"
#include "util/highresolutiontimer/HighResolutionTimer.h"

#include <atomic>
#include <memory>

// No lock anywhere in here (a global mutex, condition variables per message
// queue, a timer mutex and condition variable, a spinlock around the command
// pool and a mutex per core for replies before): every IPC a title makes -
// file system, accounts, the network services - passes through, from all
// three cores and the service threads at once.

namespace iosu
{
	namespace kernel
	{
		void _IPCDestroyAllHandlesForMsgQueue(IOSMsgQueueId msgQueueId);
		void IOS_DestroyResourceManagerForQueueId(IOSMsgQueueId msgQueueId);

		/* message queue */

		// A bounded queue any number of threads send to and receive from
		// (Vyukov's): each cell's sequence number says whether it holds the
		// message for position p (2p + 1) or is free for it (2p) - doubled so
		// a queue of one is told apart too - and senders and receivers each
		// claim a position with one compare-exchange. The messages
		// themselves are in the array the queue was created with. A blocked
		// sender or receiver sleeps on an eventcount the other side notifies.
		struct IOSMessageQueue
		{
			std::atomic<uint32> handle{0}; // 0 while free or being destroyed
			std::atomic<uint32> claimed{0};
			// sends and receives inside, which a destroy waits out
			std::atomic<uint32> users{0};
			betype<IOSMessage>* msgArray{};
			uint32 msgArraySize{0};
			std::unique_ptr<std::atomic<uint64>[]> seq;
			alignas(64) std::atomic<uint64> sendPos{0};
			alignas(64) std::atomic<uint64> recvPos{0};
			EventWake sendWake; // a message was received: room to send
			EventWake recvWake; // a message was sent
			EventWake drainWake;

			bool TrySend(IOSMessage message)
			{
				uint64 pos = sendPos.load(std::memory_order_relaxed);
				for (;;)
				{
					std::atomic<uint64>& cell = seq[pos % msgArraySize];
					const uint64 s = cell.load(std::memory_order_acquire);
					if (s == 2 * pos)
					{
						if (sendPos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
						{
							msgArray[pos % msgArraySize] = message;
							cell.store(2 * pos + 1, std::memory_order_release);
							return true;
						}
					}
					else if ((sint64)(s - 2 * pos) < 0)
						return false; // the cell still holds last lap's message: full
					else
						pos = sendPos.load(std::memory_order_relaxed);
				}
			}

			bool TryReceive(IOSMessage* out)
			{
				uint64 pos = recvPos.load(std::memory_order_relaxed);
				for (;;)
				{
					std::atomic<uint64>& cell = seq[pos % msgArraySize];
					const uint64 s = cell.load(std::memory_order_acquire);
					if (s == 2 * pos + 1)
					{
						if (recvPos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
						{
							*out = msgArray[pos % msgArraySize];
							cell.store(2 * (pos + msgArraySize), std::memory_order_release);
							return true;
						}
					}
					else if ((sint64)(s - (2 * pos + 1)) < 0)
						return false; // empty
					else
						pos = recvPos.load(std::memory_order_relaxed);
				}
			}

			bool Full() const
			{
				const uint64 pos = sendPos.load(std::memory_order_acquire);
				return seq[pos % msgArraySize].load(std::memory_order_acquire) != 2 * pos;
			}

			bool Empty() const
			{
				const uint64 pos = recvPos.load(std::memory_order_acquire);
				return seq[pos % msgArraySize].load(std::memory_order_acquire) != 2 * pos + 1;
			}
		};

		std::array<IOSMessageQueue, 750> sMsgQueuePool;
		std::atomic<uint32> sQueueHandleCounter{0};

		// The queue, counted in as a user so a destroy waits for this
		// caller; nullptr (and not counted) if the handle is not live
		IOSMessageQueue* _IOS_EnterMessageQueue(IOSMsgQueueId queueHandle)
		{
			uint32 index = (queueHandle & 0xFFF);
			if (index >= sMsgQueuePool.size() || queueHandle == 0)
				return nullptr;
			IOSMessageQueue& q = sMsgQueuePool[index];
			q.users.fetch_add(1, std::memory_order_seq_cst);
			if (q.handle.load(std::memory_order_seq_cst) != queueHandle)
			{
				if (q.users.fetch_sub(1, std::memory_order_seq_cst) == 1)
					q.drainWake.Notify();
				return nullptr;
			}
			return &q;
		}

		void _IOS_LeaveMessageQueue(IOSMessageQueue* q)
		{
			if (q->users.fetch_sub(1, std::memory_order_seq_cst) == 1)
				q->drainWake.Notify();
		}

		IOSMsgQueueId IOS_CreateMessageQueue(IOSMessage* messageArray, uint32 messageCount)
		{
			cemu_assert(messageCount != 0);
			for (size_t index = 0; index < sMsgQueuePool.size(); index++)
			{
				IOSMessageQueue& msgQueue = sMsgQueuePool[index];
				uint32 expected = 0;
				if (!msgQueue.claimed.compare_exchange_strong(expected, 1, std::memory_order_acquire))
					continue;
				uint32 queueHandle;
				do
					queueHandle = (uint32)index | ((sQueueHandleCounter.fetch_add(1, std::memory_order_relaxed) << 12) & 0x7FFFFFFF);
				while (queueHandle == 0);
				msgQueue.msgArray = (betype<IOSMessage>*)messageArray;
				msgQueue.msgArraySize = messageCount;
				msgQueue.seq.reset(new std::atomic<uint64>[messageCount]);
				for (uint32 i = 0; i < messageCount; i++)
					msgQueue.seq[i].store(2 * (uint64)i, std::memory_order_relaxed);
				msgQueue.sendPos.store(0, std::memory_order_relaxed);
				msgQueue.recvPos.store(0, std::memory_order_relaxed);
				msgQueue.handle.store(queueHandle, std::memory_order_seq_cst);
				return queueHandle;
			}
			cemu_assert_suspicious();
			return IOS_ERROR_MAXIMUM_REACHED;
		}

		IOS_ERROR IOS_DestroyMessageQueue(IOSMsgQueueId msgQueueId)
		{
			uint32 index = (msgQueueId & 0xFFF);
			if (index >= sMsgQueuePool.size() || msgQueueId == 0)
				return IOS_ERROR_INVALID;
			IOSMessageQueue& q = sMsgQueuePool[index];
			uint32 expected = msgQueueId;
			if (!q.handle.compare_exchange_strong(expected, 0, std::memory_order_seq_cst))
				return IOS_ERROR_INVALID;
			// senders and receivers blocked on it see the handle gone and leave
			q.sendWake.Notify();
			q.recvWake.Notify();
			q.drainWake.WaitUntil([&q] { return q.users.load(std::memory_order_seq_cst) == 0; });
			IOS_DestroyResourceManagerForQueueId(msgQueueId);
			q.msgArray = nullptr;
			q.claimed.store(0, std::memory_order_release);
			return IOS_ERROR_OK;
		}

		IOS_ERROR IOS_SendMessage(IOSMsgQueueId msgQueueId, IOSMessage message, uint32 flags)
		{
			cemu_assert_debug(flags == 0 || flags == 1);
			bool dontBlock = (flags & 1) != 0;
			IOSMessageQueue* msgQueue = _IOS_EnterMessageQueue(msgQueueId);
			if (!msgQueue)
				return IOS_ERROR_INVALID;
			IOS_ERROR r = IOS_ERROR_OK;
			while (!msgQueue->TrySend(message))
			{
				if (dontBlock)
				{
					r = IOS_ERROR_WOULD_BLOCK;
					break;
				}
				msgQueue->sendWake.WaitUntil([&] {
					return !msgQueue->Full() || msgQueue->handle.load(std::memory_order_seq_cst) != msgQueueId;
				});
				if (msgQueue->handle.load(std::memory_order_seq_cst) != msgQueueId)
				{
					r = IOS_ERROR_INVALID;
					break;
				}
			}
			if (r == IOS_ERROR_OK)
				msgQueue->recvWake.Notify();
			_IOS_LeaveMessageQueue(msgQueue);
			return r;
		}

		IOS_ERROR IOS_ReceiveMessage(IOSMsgQueueId msgQueueId, IOSMessage* messageOut, uint32 flags)
		{
			cemu_assert_debug(flags == 0 || flags == 1);
			bool dontBlock = (flags & 1) != 0;
			IOSMessageQueue* msgQueue = _IOS_EnterMessageQueue(msgQueueId);
			if (!msgQueue)
				return IOS_ERROR_INVALID;
			IOS_ERROR r = IOS_ERROR_OK;
			while (!msgQueue->TryReceive(messageOut))
			{
				if (dontBlock)
				{
					r = IOS_ERROR_NONE_AVAILABLE;
					break;
				}
				msgQueue->recvWake.WaitUntil([&] {
					return !msgQueue->Empty() || msgQueue->handle.load(std::memory_order_seq_cst) != msgQueueId;
				});
				if (msgQueue->handle.load(std::memory_order_seq_cst) != msgQueueId)
				{
					r = IOS_ERROR_INVALID;
					break;
				}
			}
			if (r == IOS_ERROR_OK)
				msgQueue->sendWake.Notify();
			_IOS_LeaveMessageQueue(msgQueue);
			return r;
		}

		/* timer */

		// The timers belong to their thread: create, stop and destroy hand it
		// an operation through a queue, and it sleeps on that queue until the
		// next timer is due or an operation comes. A timer's ID is a slot
		// taken from a fixed pool at create and given back by the thread once
		// it has carried out the destroy.
		static constexpr size_t IOS_TIMER_COUNT = 512;

		enum class IOSTimerOpType
		{
			Create,
			Stop,
			Destroy,
			Quit,
		};

		struct IOSTimerOp
		{
			IOSTimerOpType type;
			IOSTimerId timerId;
			IOSMsgQueueId queueId;
			uint32 message;
			HRTick nextFire;
			HRTick repeat;
		};

		// 0 free, 1 live, 2 being destroyed
		std::atomic<uint32> sTimerSlots[IOS_TIMER_COUNT];
		MpscQueue<IOSTimerOp> sTimerOps;

		void IOSTimerThread()
		{
			SetThreadName("IOS-Timer");
			struct Timer
			{
				IOSMsgQueueId queueId;
				uint32 message;
				HRTick nextFire;
				HRTick repeat;
			};
			std::vector<Timer> timers(IOS_TIMER_COUNT);
			auto byFireTime = [&timers](const IOSTimerId& idA, const IOSTimerId& idB) {
				// order by nextFire, then by timerId to avoid duplicate keys
				if (timers[idA].nextFire != timers[idB].nextFire)
					return timers[idA].nextFire < timers[idB].nextFire;
				return idA < idB;
			};
			std::set<IOSTimerId, decltype(byFireTime)> pending(byFireTime);
			auto setNextFire = [&](IOSTimerId id, HRTick nextFire) {
				pending.erase(id);
				timers[id].nextFire = nextFire;
				if (nextFire != 0)
					pending.insert(id);
			};
			while (true)
			{
				IOSTimerOp op;
				bool got;
				if (pending.empty())
				{
					sTimerOps.pop(op);
					got = true;
				}
				else
				{
					Timer& next = timers[*pending.begin()];
					HRTick now = HighResolutionTimer::now().getTick();
					if (now >= next.nextFire)
					{
						IOSTimerId id = *pending.begin();
						setNextFire(id, next.repeat == 0 ? 0 : next.nextFire + next.repeat);
						IOS_SendMessage(timers[id].queueId, timers[id].message, 1);
						continue;
					}
					auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(HighResolutionTimer::ticksToMicroseconds(next.nextFire - now));
					got = sTimerOps.pop_until(op, deadline);
				}
				if (!got)
					continue;
				switch (op.type)
				{
				case IOSTimerOpType::Create:
					timers[op.timerId] = { op.queueId, op.message, 0, op.repeat };
					setNextFire(op.timerId, op.nextFire);
					break;
				case IOSTimerOpType::Stop:
					setNextFire(op.timerId, 0);
					break;
				case IOSTimerOpType::Destroy:
					setNextFire(op.timerId, 0);
					sTimerSlots[op.timerId].store(0, std::memory_order_release);
					break;
				case IOSTimerOpType::Quit:
					// the timers go with the thread; a launch starts afresh
					for (auto& slot : sTimerSlots)
						slot.store(0, std::memory_order_release);
					return;
				}
			}
		}

		IOS_ERROR IOS_CreateTimer(uint32 startMicroseconds, uint32 repeatMicroseconds, uint32 queueId, uint32 message)
		{
			for (size_t i = 0; i < IOS_TIMER_COUNT; i++)
			{
				uint32 expected = 0;
				if (!sTimerSlots[i].compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
					continue;
				IOSTimerOp op{};
				op.type = IOSTimerOpType::Create;
				op.timerId = (IOSTimerId)i;
				op.queueId = queueId;
				op.message = message;
				op.nextFire = HighResolutionTimer::now().getTick() + HighResolutionTimer::microsecondsToTicks(startMicroseconds);
				op.repeat = HighResolutionTimer::microsecondsToTicks(repeatMicroseconds);
				sTimerOps.push(op);
				return (IOS_ERROR)i;
			}
			cemu_assert_suspicious();
			return IOS_ERROR_MAXIMUM_REACHED;
		}

		IOS_ERROR IOS_StopTimer(IOSTimerId timerId)
		{
			if (timerId >= IOS_TIMER_COUNT || sTimerSlots[timerId].load(std::memory_order_acquire) != 1)
				return IOS_ERROR_INVALID;
			IOSTimerOp op{};
			op.type = IOSTimerOpType::Stop;
			op.timerId = timerId;
			sTimerOps.push(op);
			return IOS_ERROR_OK;
		}

		IOS_ERROR IOS_DestroyTimer(IOSTimerId timerId)
		{
			uint32 expected = 1;
			if (timerId >= IOS_TIMER_COUNT || !sTimerSlots[timerId].compare_exchange_strong(expected, 2, std::memory_order_acq_rel))
				return IOS_ERROR_INVALID;
			IOSTimerOp op{};
			op.type = IOSTimerOpType::Destroy;
			op.timerId = timerId;
			sTimerOps.push(op);
			return IOS_ERROR_OK;
		}

		/* devices */

		// Read on every IOS_Open, written when a service starts or stops:
		// published whole, read with no lock
		struct IOSResourceManager
		{
			std::string path;
			IOSMsgQueueId msgQueueId;
		};

		RcuCell<std::vector<IOSResourceManager>> sDeviceResources;

		IOS_ERROR IOS_RegisterResourceManager(const char* devicePath, IOSMsgQueueId msgQueueId)
		{
			// verify if queue is valid
			IOSMessageQueue* msgQueue = _IOS_EnterMessageQueue(msgQueueId);
			if (!msgQueue)
				return IOS_ERROR_INVALID;
			_IOS_LeaveMessageQueue(msgQueue);
			IOS_ERROR r = IOS_ERROR_OK;
			std::string_view path{ devicePath };
			sDeviceResources.Write([&] {
				std::vector<IOSResourceManager> next;
				{
					auto current = sDeviceResources.Read();
					next = *current;
				}
				for (auto& it : next)
				{
					if (it.path == path)
					{
						cemu_assert_suspicious();
						r = IOS_ERROR_INVALID; // correct error code?
						return next;
					}
				}
				if (next.size() >= 512)
				{
					r = IOS_ERROR_MAXIMUM_REACHED;
					return next;
				}
				next.push_back({ std::string(path), msgQueueId });
				return next;
			});
			return r;
		}

		void IOS_DestroyResourceManagerForQueueId(IOSMsgQueueId msgQueueId)
		{
			// destroy all IPC handles associated with this queue
			_IPCDestroyAllHandlesForMsgQueue(msgQueueId);
			// destroy device resource manager
			sDeviceResources.Write([&] {
				std::vector<IOSResourceManager> next;
				auto current = sDeviceResources.Read();
				for (auto& it : *current)
					if (it.msgQueueId != msgQueueId)
						next.push_back(it);
				return next;
			});
		}

		// The pair for IOS_RegisterResourceManager, which refuses a path it
		// already knows. Nothing needed one while the process only ever started
		// its services once, but a libretro core is stopped and started inside
		// one process: the frontend deinitialises it when content is closed and
		// initialises it again for the next content. Without this, a service
		// that stops cannot come back - its second registration is refused and
		// the path is left pointing at a message queue nobody is reading, which
		// is a title that boots and then waits forever on its first request.
		IOS_ERROR IOS_UnregisterResourceManager(const char* devicePath)
		{
			bool found = false;
			std::string_view path{ devicePath };
			sDeviceResources.Write([&] {
				std::vector<IOSResourceManager> next;
				auto current = sDeviceResources.Read();
				for (auto& it : *current)
				{
					if (!found && it.path == path)
						found = true;
					else
						next.push_back(it);
				}
				return next;
			});
			return found ? IOS_ERROR_OK : IOS_ERROR_INVALID;
		}

		IOS_ERROR IOS_DeviceAssociateId(const char* devicePath, uint32 id)
		{
			// not yet implemented
			return IOS_ERROR_OK;
		}

		/* IPC */
		
		struct IOSDispatchableCommand
		{
			// stores a copy of incoming IPC requests with some extra information required for replies
			IPCCommandBody body; // our dispatchable copy
			IPCIoctlVector vecCopy[8]; // our copy of the Ioctlv vector array
			IPCCommandBody* originalBody; // the original command that was sent to us
			uint32 ppcCoreIndex;
			IOSDevHandle replyHandle; // handle for outgoing replies
			bool isAllocated{false};
		};

		SysAllocator<IOSDispatchableCommand, 96> sIPCDispatchableCommandPool;
		IndexFreeList<96> sIPCFreeDispatchableCommands;

		void _IPCInitDispatchablePool()
		{
			sIPCFreeDispatchableCommands.Fill();
		}

		IOSDispatchableCommand* _IPCAllocateDispatchableCommand()
		{
			size_t index;
			if (!sIPCFreeDispatchableCommands.Pop(index))
			{
				cemuLog_log(LogType::Force, "IOS: Exhausted pool of dispatchable commands");
				return nullptr;
			}
			IOSDispatchableCommand* cmd = sIPCDispatchableCommandPool.GetPtr() + index;
			cemu_assert_debug(!cmd->isAllocated);
			cmd->isAllocated = true;
			return cmd;
		}

		void _IPCReleaseDispatchableCommand(IOSDispatchableCommand* cmd)
		{
			cemu_assert_debug(cmd->isAllocated);
			cmd->isAllocated = false;
			sIPCFreeDispatchableCommands.Push(cmd - sIPCDispatchableCommandPool.GetPtr());
		}

		static constexpr size_t MAX_NUM_ACTIVE_DEV_HANDLES = 96; // per process

		// A handle is claimed with one exchange and published by its check
		// value, written last; it is gone once that is cleared. Its fields
		// keep their values until the slot is claimed again, so a dispatch
		// that checked the handle just before it closed reads them whole.
		struct IPCActiveDeviceHandle
		{
			std::atomic<uint32> claimed{0};
			std::atomic<uint32> handleCheckValue{0};
			std::atomic<IOSMsgQueueId> msgQueueId{0};
			// dispatch target handle (retrieved via IOS_OPEN command to dispatch target)
			std::atomic<bool> hasDispatchTargetHandle{false};
			std::atomic<IOSDevHandle> dispatchTargetHandle{0};
		};

		IPCActiveDeviceHandle sActiveDeviceHandles[MAX_NUM_ACTIVE_DEV_HANDLES];
		std::atomic<uint32> sHandleCreationCounter{1};

		IPCActiveDeviceHandle* _IPCGetActiveHandle(IOSDevHandle devHandle)
		{
			uint32 index = devHandle & 0xFFF;
			cemu_assert(index < MAX_NUM_ACTIVE_DEV_HANDLES);
			IPCActiveDeviceHandle& h = sActiveDeviceHandles[index];
			uint32 check = h.handleCheckValue.load(std::memory_order_acquire);
			if (check == 0)
			{
				cemuLog_log(LogType::Force, "_IPCDispatchToResourceManager(): Resource manager destroyed before all IPC commands were processed");
				return nullptr;
			}
			if (check != devHandle)
			{
				cemuLog_log(LogType::Force, "_IPCDispatchToResourceManager(): Mismatching handle");
				return nullptr;
			}
			return &h;
		}

		void _IPCFreeActiveHandle(IPCActiveDeviceHandle& h, uint32 check)
		{
			if (h.handleCheckValue.compare_exchange_strong(check, 0, std::memory_order_acq_rel))
			{
				h.hasDispatchTargetHandle.store(false, std::memory_order_relaxed);
				h.claimed.store(0, std::memory_order_release);
			}
		}

		IOS_ERROR _IPCCreateResourceHandle(const char* devicePath, IOSDevHandle& handleOut)
		{
			// find resource manager for device
			IOSMsgQueueId msgQueueId = 0;
			bool found = false;
			{
				auto resources = sDeviceResources.Read();
				std::string_view devicePathSV{ devicePath };
				for (auto& it : *resources)
				{
					if (it.path == devicePathSV)
					{
						msgQueueId = it.msgQueueId;
						found = true;
						break;
					}
				}
			}
			if (!found)
			{
				cemuLog_log(LogType::Force, "IOSU-Kernel: IOS_Open() could not open {}", devicePath);
				return IOS_ERROR_INVALID;
			}
			// create new handle
			for (size_t i = 0; i < MAX_NUM_ACTIVE_DEV_HANDLES; i++)
			{
				IPCActiveDeviceHandle& h = sActiveDeviceHandles[i];
				uint32 expected = 0;
				if (!h.claimed.compare_exchange_strong(expected, 1, std::memory_order_acquire))
					continue;
				// calc handle
				uint32 devHandle;
				do
					devHandle = (uint32)i | ((sHandleCreationCounter.fetch_add(1, std::memory_order_relaxed) << 12) & 0x7FFFFFFF);
				while (devHandle == 0);
				h.msgQueueId.store(msgQueueId, std::memory_order_relaxed);
				h.hasDispatchTargetHandle.store(false, std::memory_order_relaxed);
				h.handleCheckValue.store(devHandle, std::memory_order_release);
				handleOut = devHandle;
				return IOS_ERROR_OK;
			}
			cemu_assert_debug(false);
			return IOS_ERROR_MAXIMUM_REACHED;
		}

		IOS_ERROR _IPCDestroyResourceHandle(IOSDevHandle devHandle)
		{
			IPCActiveDeviceHandle* h = _IPCGetActiveHandle(devHandle);
			if (!h)
				return IOS_ERROR_INVALID;
			_IPCFreeActiveHandle(*h, devHandle);
			return IOS_ERROR_OK;
		}

		void _IPCDestroyAllHandlesForMsgQueue(IOSMsgQueueId msgQueueId)
		{
			for (auto& it : sActiveDeviceHandles)
			{
				uint32 check = it.handleCheckValue.load(std::memory_order_acquire);
				if (check != 0 && it.msgQueueId.load(std::memory_order_relaxed) == msgQueueId)
					_IPCFreeActiveHandle(it, check);
			}
		}

		IOS_ERROR _IPCAssignDispatchTargetHandle(IOSDevHandle devHandle, IOSDevHandle internalHandle)
		{
			IPCActiveDeviceHandle* h = _IPCGetActiveHandle(devHandle);
			if (!h)
				return IOS_ERROR_INVALID;
			cemu_assert_debug(!h->hasDispatchTargetHandle.load(std::memory_order_relaxed));
			h->dispatchTargetHandle.store(internalHandle, std::memory_order_relaxed);
			h->hasDispatchTargetHandle.store(true, std::memory_order_release);
			return IOS_ERROR_OK;
		}

		IOS_ERROR _IPCDispatchToResourceManager(IOSDevHandle devHandle, IOSDispatchableCommand* dispatchCmd)
		{
			IPCActiveDeviceHandle* h = _IPCGetActiveHandle(devHandle);
			if (!h)
				return IOS_ERROR_INVALID;
			IOSMsgQueueId msgQueueId = h->msgQueueId.load(std::memory_order_relaxed);
			if (dispatchCmd->body.cmdId == IPCCommandId::IOS_OPEN)
			{
				cemu_assert(!h->hasDispatchTargetHandle.load(std::memory_order_acquire));
				dispatchCmd->body.devHandle = 0;
			}
			else
			{
				cemu_assert(h->hasDispatchTargetHandle.load(std::memory_order_acquire));
				dispatchCmd->body.devHandle = h->dispatchTargetHandle.load(std::memory_order_relaxed);
			}
			MEMPTR<IOSDispatchableCommand> msgVal{ dispatchCmd };
			IOS_ERROR r = IOS_SendMessage(msgQueueId, msgVal.GetMPTR(), 1);
			if(r != IOS_ERROR_OK)
				cemuLog_log(LogType::Force, "_IPCDispatchToResourceManager(): SendMessage returned {}", (sint32)r);
			return r;
		}

		void _IPCReplyAndRelease(IOSDispatchableCommand* dispatchCmd, uint32 result)
		{
			cemu_assert(dispatchCmd->ppcCoreIndex < 3);
			cemu_assert(dispatchCmd >= sIPCDispatchableCommandPool.GetPtr() && dispatchCmd < sIPCDispatchableCommandPool.GetPtr() + sIPCDispatchableCommandPool.GetCount());	
			dispatchCmd->originalBody->result = result;
			// submit to COS
			IPCCommandBody* responseArray[1];
			responseArray[0] = dispatchCmd->originalBody;
			coreinit::IPCDriver_NotifyResponses(dispatchCmd->ppcCoreIndex, responseArray, 1);
			_IPCReleaseDispatchableCommand(dispatchCmd);
		}

		IOS_ERROR _IPCHandlerIn_IOS_Open(IOSDispatchableCommand* dispatchCmd)
		{
			IPCCommandBody& cmd = dispatchCmd->body;
			const char* name = MEMPTR<const char>(cmd.args[0]).GetPtr();
			uint32 nameLenPlusOne = cmd.args[1];
			cemu_assert(nameLenPlusOne > 0);
			uint32 flags = cmd.args[2];
			cemu_assert_debug(flags == 0);

			std::string devicePath{ name, nameLenPlusOne - 1 };

			IOSDevHandle handle;
			IOS_ERROR r = _IPCCreateResourceHandle(devicePath.c_str(), handle);
			if (r != IOS_ERROR_OK)
				return r;
			dispatchCmd->replyHandle = handle;
			dispatchCmd->body.devHandle = 0;
			r = _IPCDispatchToResourceManager(handle, dispatchCmd);
			return r;
		}

		IOS_ERROR _IPCHandlerIn_IOS_Close(IOSDispatchableCommand* dispatchCmd)
		{
			IPCCommandBody& cmd = dispatchCmd->body;
			IOS_ERROR r = _IPCDispatchToResourceManager(dispatchCmd->body.devHandle, dispatchCmd);
			return r;
		}

		IOS_ERROR _IPCHandlerIn_IOS_Ioctl(IOSDispatchableCommand* dispatchCmd)
		{
			IPCCommandBody& cmd = dispatchCmd->body;
			IOS_ERROR r = _IPCDispatchToResourceManager(dispatchCmd->body.devHandle, dispatchCmd);
			return r;
		}

		IOS_ERROR _IPCHandlerIn_IOS_Ioctlv(IOSDispatchableCommand* dispatchCmd)
		{
			IPCCommandBody& cmd = dispatchCmd->body;
			uint32 requestId = dispatchCmd->body.args[0];
			uint32 numIn = dispatchCmd->body.args[1];
			uint32 numOut = dispatchCmd->body.args[2];
			IPCIoctlVector* vec = MEMPTR<IPCIoctlVector>(cmd.args[3]).GetPtr();
			// copy the vector array
			uint32 numVec = numIn + numOut;
			if (numVec <= 8)
			{
				std::copy(vec, vec + numVec, dispatchCmd->vecCopy);
				dispatchCmd->body.args[3] = MEMPTR<IPCIoctlVector>(vec).GetMPTR();
			}
			else
			{
				// reuse the original vector pointer
				cemuLog_log(LogType::Force, "Info: Ioctlv command with more than 8 vectors");
			}
			return _IPCDispatchToResourceManager(dispatchCmd->body.devHandle, dispatchCmd);
		}

		// normally COS kernel handles this, but currently we skip the IPC getting proxied through it
		IOS_ERROR _IPCHandlerIn_TranslateVectorAddresses(IOSDispatchableCommand* dispatchCmd)
		{
			uint32 numIn = dispatchCmd->body.args[1];
			uint32 numOut = dispatchCmd->body.args[2];
			IPCIoctlVector* vec = MEMPTR<IPCIoctlVector>(dispatchCmd->body.args[3]).GetPtr();
			for (uint32 i = 0; i < numIn + numOut; i++)
			{
				if (vec[i].baseVirt == nullptr && vec[i].size != 0)
				{
					cemuLog_logDebug(LogType::Force, "IPC Ioctlv failed because baseVirt is null but size is not 0");
					return IOS_ERROR_INVALID;
				}
				// todo - check for valid pointer range
				vec[i].basePhys = vec[i].baseVirt;
			}
			return IOS_ERROR_OK;
		}

		// called by COS directly
		void IPCSubmitFromCOS(uint32 ppcCoreIndex, IPCCommandBody* cmd)
		{
			// create a copy of the cmd
			IOSDispatchableCommand* dispatchCmd = _IPCAllocateDispatchableCommand();
			dispatchCmd->body = *cmd;
			dispatchCmd->originalBody = cmd;
			dispatchCmd->ppcCoreIndex = ppcCoreIndex;
			dispatchCmd->replyHandle = cmd->devHandle;
			// forward command to device
			IOS_ERROR r = IOS_ERROR_INVALID;
			switch ((IPCCommandId)cmd->cmdId)
			{
			case IPCCommandId::IOS_OPEN:
				dispatchCmd->replyHandle = 0;
				r = _IPCHandlerIn_IOS_Open(dispatchCmd);
				break;
			case IPCCommandId::IOS_CLOSE:
				r = _IPCHandlerIn_IOS_Close(dispatchCmd);
				break;
			case IPCCommandId::IOS_IOCTL:
				r = _IPCHandlerIn_IOS_Ioctl(dispatchCmd);
				break;
			case IPCCommandId::IOS_IOCTLV:
				r = _IPCHandlerIn_TranslateVectorAddresses(dispatchCmd);
				if(r < 0)
					cemuLog_log(LogType::Force, "Ioctlv error");
				else
					r = _IPCHandlerIn_IOS_Ioctlv(dispatchCmd);
				break;
			default:
				cemuLog_log(LogType::Force, "Invalid IPC command {}", (uint32)(IPCCommandId)cmd->cmdId);
				break;
			}
			if (r < 0)
			{
				cemuLog_log(LogType::Force, "Error occurred while trying to dispatch IPC");			
				_IPCReplyAndRelease(dispatchCmd, r);
				// in non-error case the device handler will send the result asynchronously via IOS_ResourceReply
			}
		}

		IOS_ERROR IOS_ResourceReply(IPCCommandBody* cmd, IOS_ERROR result)
		{
			IOSDispatchableCommand* dispatchCmd = (IOSDispatchableCommand*)cmd;
			cemu_assert(dispatchCmd >= sIPCDispatchableCommandPool.GetPtr() && dispatchCmd < sIPCDispatchableCommandPool.GetPtr() + sIPCDispatchableCommandPool.GetCount());
			cemu_assert_debug(dispatchCmd->isAllocated);
			dispatchCmd->originalBody->result = result;
			if (dispatchCmd->originalBody->cmdId == IPCCommandId::IOS_OPEN)
			{
				IOSDevHandle devHandle = dispatchCmd->replyHandle;
				if (IOS_ResultIsError(result))
				{
					cemuLog_log(LogType::Force, "IOS_ResourceReply(): Target device triggered an error on IOS_OPEN");
					// dispatch target returned error, destroy our device handle again
					IOS_ERROR r = _IPCDestroyResourceHandle(devHandle);
					cemu_assert(r == IOS_ERROR_OK);
				}
				else
				{
					cemu_assert(_IPCAssignDispatchTargetHandle(devHandle, (IOSDevHandle)result) == IOS_ERROR_OK);
					result = (IOS_ERROR)(uint32)devHandle;
				}
			}
			else if (dispatchCmd->originalBody->cmdId == IPCCommandId::IOS_CLOSE)
			{
				if (IOS_ResultIsError(result))
				{
					cemuLog_log(LogType::Force, "IOS_ResourceReply(): Target device triggered an error on IOS_CLOSE");
				}
				// destroy the handle, then reply: a core that has its answer
				// finds the handle closed, rather than still open for a moment
				IOSDevHandle devHandle = dispatchCmd->replyHandle;
				IOS_ERROR r = _IPCDestroyResourceHandle(devHandle);
				cemu_assert_debug(r == IOS_ERROR::IOS_ERROR_OK);
				_IPCReplyAndRelease(dispatchCmd, result);
				return IOS_ERROR_OK;
			}
			_IPCReplyAndRelease(dispatchCmd, result);
			return IOS_ERROR_OK;
		}

		class : public ::IOSUModule
		{
			void SystemLaunch() override
			{
				_IPCInitDispatchablePool();
				// start timer thread
				sTimerOps.clear();
				m_timerThread = std::thread(IOSTimerThread);
			}

			void SystemExit() override
			{
				// stop timer thread
				IOSTimerOp quit{};
				quit.type = IOSTimerOpType::Quit;
				sTimerOps.push(quit);
				m_timerThread.join();
				// reset resources
				// todo
			}

			std::thread m_timerThread;
		}sIOSUModuleKernel;

		IOSUModule* GetModule()
		{
			return static_cast<IOSUModule*>(&sIOSUModuleKernel);
		}


	}
}