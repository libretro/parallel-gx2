#include "Cafe/Filesystem/fsc.h"
#include "Cafe/Filesystem/FST/fstUtil.h"

#include <atomic>

// The mount table: a tree of mount points for each priority. A table is
// never changed once it is published: a mount or an unmount builds a new one
// from the one in place and swaps it in (compare-exchange, again on a race
// with another), so a lookup never waits and never sees half a change. A
// reader announces the table it reads in a hazard slot of its thread's, and
// a replaced table is freed once no slot holds it.
struct FSCMountPathNode
{
	std::string path;
	std::vector<FSCMountPathNode*> subnodes;
	FSCMountPathNode* parent;
	// associated device target and path
	fscDeviceC* device{ nullptr };
	void* ctx{ nullptr };
	std::string deviceTargetPath; // the destination base path for the device, utf8
	// priority
	sint32 priority{};

	FSCMountPathNode(FSCMountPathNode* parent) : parent(parent)
	{
	}

	// a deep copy, under parent
	FSCMountPathNode(const FSCMountPathNode& other, FSCMountPathNode* parent)
		: path(other.path), parent(parent), device(other.device), ctx(other.ctx),
		  deviceTargetPath(other.deviceTargetPath), priority(other.priority)
	{
		subnodes.reserve(other.subnodes.size());
		for (const FSCMountPathNode* sub : other.subnodes)
			subnodes.push_back(new FSCMountPathNode(*sub, this));
	}

    void AssignDevice(fscDeviceC* device, void* ctx, std::string_view deviceBasePath)
    {
        this->device = device;
        this->ctx = ctx;
        this->deviceTargetPath = deviceBasePath;
    }

    void UnassignDevice()
    {
        this->device = nullptr;
        this->ctx = nullptr;
        this->deviceTargetPath.clear();
    }

    bool IsRootNode() const
    {
        return !parent;
    }

	~FSCMountPathNode()
	{
		for (auto& itr : subnodes)
			delete itr;
		subnodes.clear();
	}
};

struct FSCMountTable
{
	FSCMountPathNode* root[FSC_PRIORITY_COUNT];
	FSCMountTable* retiredNext{};

	FSCMountTable()
	{
		for (auto& r : root)
			r = new FSCMountPathNode(nullptr);
	}

	FSCMountTable(const FSCMountTable& other)
	{
		for (sint32 i = 0; i < FSC_PRIORITY_COUNT; i++)
			root[i] = new FSCMountPathNode(*other.root[i], nullptr);
	}

	FSCMountTable& operator=(const FSCMountTable&) = delete;

	~FSCMountTable()
	{
		for (auto& r : root)
			delete r;
	}
};

// compare two file or directory names using FSA rules
bool FSA_CompareNodeName(std::string_view a, std::string_view b)
{
	if (a.size() != b.size())
		return false;
	for (size_t i = 0; i < a.size(); i++)
	{
		uint8 ac = (uint8)a[i];
		uint8 bc = (uint8)b[i];
		// lower case compare
		if (ac >= (uint8)'A' && ac <= (uint8)'Z')
			ac -= ((uint8)'A' - (uint8)'a');
		if (bc >= (uint8)'A' && bc <= (uint8)'Z')
			bc -= ((uint8)'A' - (uint8)'a');
		if (ac != bc)
			return false;
	}
	return true;
}

static std::atomic<FSCMountTable*> s_fscTable{nullptr};
static std::atomic<FSCMountTable*> s_fscRetired{nullptr};
// tables made and not yet freed, for the tests
static std::atomic<sint32> s_fscLiveTables{0};

// a hazard slot for each thread that reads the table, claimed at its first
// read and given back when the thread ends
static constexpr sint32 FSC_HAZARD_SLOTS = 256;
static std::atomic<const FSCMountTable*> s_fscHazard[FSC_HAZARD_SLOTS];
static std::atomic<bool> s_fscHazardTaken[FSC_HAZARD_SLOTS];

struct FSCHazardSlotOwner
{
	sint32 index{-1};
	~FSCHazardSlotOwner()
	{
		if (index < 0)
			return;
		s_fscHazard[index].store(nullptr, std::memory_order_release);
		s_fscHazardTaken[index].store(false, std::memory_order_release);
	}
};
static thread_local FSCHazardSlotOwner t_fscHazard;

static std::atomic<const FSCMountTable*>& fsc_hazardSlot()
{
	if (t_fscHazard.index < 0)
	{
		for (sint32 i = 0; i < FSC_HAZARD_SLOTS; i++)
		{
			if (!s_fscHazardTaken[i].load(std::memory_order_relaxed) && !s_fscHazardTaken[i].exchange(true, std::memory_order_acquire))
			{
				t_fscHazard.index = i;
				break;
			}
		}
		cemu_assert(t_fscHazard.index >= 0); // more threads reading the file system than slots
	}
	return s_fscHazard[t_fscHazard.index];
}

// The table, held for as long as this lives. Held already on this thread
// (a lookup inside an open), it is the same table.
class FSCTableRef
{
public:
	FSCTableRef() : m_slot(fsc_hazardSlot())
	{
		m_prev = m_slot.load(std::memory_order_relaxed);
		if (m_prev)
		{
			m_table = m_prev;
			return;
		}
		const FSCMountTable* t;
		do
		{
			t = s_fscTable.load(std::memory_order_seq_cst);
			m_slot.store(t, std::memory_order_seq_cst);
		} while (t != s_fscTable.load(std::memory_order_seq_cst));
		m_table = t;
	}
	~FSCTableRef()
	{
		m_slot.store(m_prev, std::memory_order_release);
	}
	FSCTableRef(const FSCTableRef&) = delete;
	FSCTableRef& operator=(const FSCTableRef&) = delete;

	const FSCMountTable* get() const { return m_table; }
	// whether this thread held a table already
	bool nested() const { return m_prev != nullptr; }

private:
	std::atomic<const FSCMountTable*>& m_slot;
	const FSCMountTable* m_prev;
	const FSCMountTable* m_table;
};

static FSCMountTable* fsc_newTable(const FSCMountTable* from)
{
	s_fscLiveTables.fetch_add(1, std::memory_order_relaxed);
	return from ? new FSCMountTable(*from) : new FSCMountTable();
}

static void fsc_freeTable(FSCMountTable* t)
{
	delete t;
	s_fscLiveTables.fetch_sub(1, std::memory_order_relaxed);
}

static bool fsc_isHazard(const FSCMountTable* t)
{
	for (auto& h : s_fscHazard)
		if (h.load(std::memory_order_seq_cst) == t)
			return true;
	return false;
}

// tables replaced and no longer read are freed; those still read wait for
// the next replacement
static void fsc_reclaim()
{
	FSCMountTable* list = s_fscRetired.exchange(nullptr, std::memory_order_acq_rel);
	while (list)
	{
		FSCMountTable* next = list->retiredNext;
		if (fsc_isHazard(list))
		{
			list->retiredNext = s_fscRetired.load(std::memory_order_relaxed);
			while (!s_fscRetired.compare_exchange_weak(list->retiredNext, list, std::memory_order_release, std::memory_order_relaxed))
				;
		}
		else
			fsc_freeTable(list);
		list = next;
	}
}

// A new table from the one in place, changed by change(table) - false to
// leave things as they are - and swapped in
template<typename F>
static bool fsc_changeTable(F&& change)
{
	for (;;)
	{
		FSCTableRef ref;
		// a change made while this thread reads the table would start from
		// the table it reads, however stale, and never get in
		cemu_assert(!ref.nested());
		FSCMountTable* old = const_cast<FSCMountTable*>(ref.get());
		FSCMountTable* table = fsc_newTable(old);
		if (!change(*table))
		{
			fsc_freeTable(table);
			return false;
		}
		if (s_fscTable.compare_exchange_strong(old, table, std::memory_order_seq_cst))
		{
			if (old)
			{
				old->retiredNext = s_fscRetired.load(std::memory_order_relaxed);
				while (!s_fscRetired.compare_exchange_weak(old->retiredNext, old, std::memory_order_release, std::memory_order_relaxed))
					;
			}
			break;
		}
		// another change came first: from its table, again
		fsc_freeTable(table);
	}
	fsc_reclaim();
	return true;
}

sint32 fsc_liveMountTables()
{
	return s_fscLiveTables.load(std::memory_order_relaxed);
}

static FSCMountPathNode* fsc_lookupPathVirtualNode(const FSCMountTable* table, const char* path, sint32 priority);

void fsc_reset()
{
	fsc_changeTable([](FSCMountTable& table) {
		for (auto& r : table.root)
		{
			delete r;
			r = new FSCMountPathNode(nullptr);
		}
		return true;
	});
}

/*
 * Creates a node chain for the given mount path. Returns the bottom node.
 * If the path already exists for the given priority, NULL is returned (we can't mount two devices to the same path with the same priority)
 * But we can map devices to subdirectories. Something like this is possible:
 * /vol/content		 -> Map to WUD (includes all subdirectories except /data, which is handled by the entry below. This exclusion rule applies only if the priority of both mount entries is the same)
 * /vol/content/data -> Map to HostFS
 * If overlapping paths with different priority are created, then the higher priority one will be checked first
 */
static FSCMountPathNode* fsc_createMountPath(FSCMountTable& table, const FSCPath& mountPath, sint32 priority)
{
	cemu_assert(priority >= 0 && priority < FSC_PRIORITY_COUNT);
	FSCMountPathNode* nodeParent = table.root[priority];
	for (size_t i=0; i< mountPath.GetNodeCount(); i++)
	{
		// search for subdirectory
		FSCMountPathNode* nodeSub = nullptr; // set if we found a subnode with a matching name, else this is used to store the new nodes
		for (auto& nodeItr : nodeParent->subnodes)
		{
			if (mountPath.MatchNodeName(i, nodeItr->path))
			{
				// subnode found
				nodeSub = nodeItr;
				break;
			}
		}
		if (nodeSub)
		{
			// traverse subnode
			nodeParent = nodeSub;
			continue;
		}
		// no matching subnode, add new entry
		nodeSub = new FSCMountPathNode(nodeParent);
		nodeSub->path = mountPath.GetNodeName(i);
		nodeSub->priority = priority;
		nodeParent->subnodes.emplace_back(nodeSub);
		if (i == (mountPath.GetNodeCount() - 1))
		{
			// last node
			return nodeSub;
		}
		// traverse subnode
		nodeParent = nodeSub;
	}
	// path is empty or already mounted
	if (mountPath.GetNodeCount() == 0)
		return nodeParent;
	return nullptr;
}

// Map a virtual FSC directory to a device. targetPath points to the destination base directory within the device
sint32 fsc_mount(std::string_view mountPath, std::string_view targetPath, fscDeviceC* fscDevice, void* ctx, sint32 priority)
{
	cemu_assert(fscDevice);
	std::string mountPathTmp(mountPath);
	// make sure the target path ends with a slash
	std::string targetPathWithSlash(targetPath);
	if (!targetPathWithSlash.empty() && (targetPathWithSlash.back() != '/' && targetPathWithSlash.back() != '\\'))
		targetPathWithSlash.push_back('/');

	FSCPath parsedMountPath(mountPathTmp);
	// register path
	const bool mounted = fsc_changeTable([&](FSCMountTable& table) {
		FSCMountPathNode* node = fsc_createMountPath(table, parsedMountPath, priority);
		if (!node)
			return false;
		node->AssignDevice(fscDevice, ctx, targetPathWithSlash);
		return true;
	});
	if (!mounted)
	{
		// path empty, invalid or already used
		cemuLog_log(LogType::Force, "fsc_mount failed (virtual path: {})", mountPath);
		return FSC_STATUS_INVALID_PATH;
	}
	return FSC_STATUS_OK;
}

bool fsc_unmount(std::string_view mountPath, sint32 priority)
{
	std::string _tmp(mountPath);
	return fsc_changeTable([&](FSCMountTable& table) {
		FSCMountPathNode* mountPathNode = fsc_lookupPathVirtualNode(&table, _tmp.c_str(), priority);
		if (!mountPathNode)
			return false;
		cemu_assert(mountPathNode->priority == priority);
		cemu_assert(mountPathNode->device);
		// unassign device
		mountPathNode->UnassignDevice();
		// prune empty branch
		while (mountPathNode && !mountPathNode->IsRootNode() && mountPathNode->subnodes.empty() && !mountPathNode->device)
		{
			FSCMountPathNode* parent = mountPathNode->parent;
			std::erase(parent->subnodes, mountPathNode);
			delete mountPathNode;
			mountPathNode = parent;
		}
		return true;
	});
}

void fsc_unmountAll()
{
	fsc_reset();
}

// lookup virtual path and find mounted device and relative device directory
static bool fsc_lookupPath(const char* path, std::string& devicePathOut, fscDeviceC** fscDeviceOut, void** ctxOut, sint32 priority = FSC_PRIORITY_BASE)
{
	FSCTableRef ref;
	if (!ref.get())
		return false;
	FSCPath parsedPath(path);
	FSCMountPathNode* nodeParent = ref.get()->root[priority];
	size_t i;
	for (i = 0; i < parsedPath.GetNodeCount(); i++)
	{
		// search for subdirectory
		FSCMountPathNode* nodeSub = nullptr;
		for(auto& nodeItr : nodeParent->subnodes)
		{			
			if (parsedPath.MatchNodeName(i, nodeItr->path))
			{
				nodeSub = nodeItr;
				break;
			}
		}
		if (nodeSub)
		{
			nodeParent = nodeSub;
			continue;
		}
		// no matching subnode
		break;
	}
	// if the found node is not a device mount point, then travel back towards the root until we find one
	while (nodeParent)
	{
		if (nodeParent->device)
		{
			devicePathOut = nodeParent->deviceTargetPath;
			for (size_t f = i; f < parsedPath.GetNodeCount(); f++)
			{
				auto nodeName = parsedPath.GetNodeName(f);
				devicePathOut.append(nodeName);
				if (f < (parsedPath.GetNodeCount() - 1))
					devicePathOut.push_back('/');
			}
			*fscDeviceOut = nodeParent->device;
			*ctxOut = nodeParent->ctx;
			return true;
		}
		nodeParent = nodeParent->parent;
		i--;
	}
	return false;
}

// lookup path and find virtual device node, in table
static FSCMountPathNode* fsc_lookupPathVirtualNode(const FSCMountTable* table, const char* path, sint32 priority)
{
	if (!table)
		return nullptr;
	FSCPath parsedPath(path);
	FSCMountPathNode* nodeCurrentDir = table->root[priority];
	for (size_t i = 0; i < parsedPath.GetNodeCount(); i++)
	{
		// search for subdirectory
		FSCMountPathNode* nodeSub = nullptr;
		for (auto& nodeItr : nodeCurrentDir->subnodes)
		{
			if (parsedPath.MatchNodeName(i, nodeItr->path))
			{
				nodeSub = nodeItr;
				break;
			}
		}
		if (nodeSub)
		{
			// traverse subdirectory
			nodeCurrentDir = nodeSub;
			continue;
		}
		return nullptr;
	}
	return nodeCurrentDir;
}

// this wraps multiple iterated directories from different devices into one unified virtual representation
class FSCVirtualFileDirectoryIterator : public FSCVirtualFile
{
public:
	sint32 fscGetType() override
	{
		return FSC_TYPE_DIRECTORY;
	}

	FSCVirtualFileDirectoryIterator(std::string_view path, std::span<FSCVirtualFile*> mappedFolders)
		: m_path(path), m_folders(mappedFolders.begin(), mappedFolders.end())
	{
		dirIterator = nullptr;
	}

	~FSCVirtualFileDirectoryIterator()
	{
		// dirIterator is deleted in base constructor
		for (auto& itr : m_folders)
			delete itr;
	}

	bool fscDirNext(FSCDirEntry* dirEntry) override
	{
		if (!dirIterator)
		{
			// lazily populate list only if directory is actually iterated
			PopulateIterationList();
			cemu_assert_debug(dirIterator);
		}
		if (dirIterator->index >= dirIterator->dirEntries.size())
			return false;
		*dirEntry = dirIterator->dirEntries[dirIterator->index];
		dirIterator->index++;
		return true;
	}

	bool fscRewindDir() override
	{
		if (!dirIterator)
			return true;

		dirIterator->index  = 0;
		return true;
	}

	void addUniqueDirEntry(const FSCDirEntry& dirEntry)
	{
		// skip if already in list
		for (auto& itr : dirIterator->dirEntries)
		{
			if (FSA_CompareNodeName(dirEntry.path, itr.path))
				return;
		}
		dirIterator->dirEntries.emplace_back(dirEntry);
	}

private:
	void PopulateIterationList()
	{
		cemu_assert_debug(!dirIterator);
		dirIterator = new FSCVirtualFile::FSCDirIteratorState();
		FSCDirEntry dirEntry;
		FSCTableRef ref;
		for (auto& itr : m_folders)
		{
			while (itr->fscDirNext(&dirEntry))
				addUniqueDirEntry(dirEntry);
		}
		for (sint32 prio = FSC_PRIORITY_COUNT - 1; prio >= 0; prio--)
		{
			FSCMountPathNode* nodeVirtualPath = fsc_lookupPathVirtualNode(ref.get(), m_path.c_str(), prio);
			if (nodeVirtualPath)
			{
				for (auto& itr : nodeVirtualPath->subnodes)
				{
					dirEntry = {};
					dirEntry.isDirectory = true;
					strncpy(dirEntry.path, itr->path.c_str(), sizeof(dirEntry.path) - 1);
					dirEntry.path[sizeof(dirEntry.path) - 1] = '\0';
					dirEntry.fileSize = 0;
					addUniqueDirEntry(dirEntry);
				}
			}
		}
	}

private:
	std::string m_path;
	std::vector<FSCVirtualFile*> m_folders; // list of all folders mapped to the same directory (at different priorities)
};

// Open file or directory from virtual file system
FSCVirtualFile* fsc_open(const char* path, FSC_ACCESS_FLAG accessFlags, sint32* fscStatus, sint32 maxPriority)
{
	cemu_assert_debug(HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::OPEN_FILE) || HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::OPEN_DIR)); // must open either file or directory
	FSCVirtualFile* dirList[FSC_PRIORITY_COUNT];
	uint8 dirListCount = 0;
	std::string devicePath;
	fscDeviceC* fscDevice = NULL;
	*fscStatus = FSC_STATUS_UNDEFINED;
	void* ctx;
	// one table for every priority looked at
	FSCTableRef ref;
	for (sint32 prio = maxPriority; prio >= 0; prio--)
	{
		if (fsc_lookupPath(path, devicePath, &fscDevice, &ctx, prio))
		{
			FSCVirtualFile* fscVirtualFile = fscDevice->fscDeviceOpenByPath(devicePath, accessFlags, ctx, fscStatus);
			if (fscVirtualFile)
			{
				if (fscVirtualFile->fscGetType() == FSC_TYPE_DIRECTORY)
				{
					cemu_assert_debug(HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::OPEN_DIR));
					// collect all folders 
					dirList[dirListCount] = fscVirtualFile;
					dirListCount++;
				}
				else
				{
					// return first found file
					cemu_assert_debug(HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::OPEN_FILE));
					fscVirtualFile->m_isAppend = HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::IS_APPEND);
					return fscVirtualFile;
				}				
			}
		}
	}
	// for directories we create a virtual representation of the enumerated files of all priorities as well as the FSC folder structure itself
	if (HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::OPEN_DIR))
	{
		// create a virtual directory VirtualFile that represents all the mounted folders as well as the virtual FSC folder structure
		bool folderExists = dirListCount > 0;
		for (sint32 prio = FSC_PRIORITY_COUNT - 1; prio >= 0; prio--)
		{
			if (folderExists)
				break;
			folderExists |= (fsc_lookupPathVirtualNode(ref.get(), path, prio) != 0);
		}
		if (folderExists)
		{
			FSCVirtualFileDirectoryIterator* dirIteratorFile = new FSCVirtualFileDirectoryIterator(path, { dirList, dirListCount});
			*fscStatus = FSC_STATUS_OK;
			return dirIteratorFile;
		}
	}
	else
	{
		cemu_assert_debug(dirListCount == 0);
	}
	*fscStatus = FSC_STATUS_FILE_NOT_FOUND;
	return nullptr;
}

/*
 * Open file using virtual path
 */
FSCVirtualFile* fsc_openDirIterator(const char* path, sint32* fscStatus)
{
	return fsc_open(path, FSC_ACCESS_FLAG::OPEN_DIR, fscStatus);
}

/*
* Iterate next node in directory
* Returns false if there is no node left
*/
bool fsc_nextDir(FSCVirtualFile* fscFile, FSCDirEntry* dirEntry)
{
	if (fscFile->fscGetType() != FSC_TYPE_DIRECTORY)
	{
		cemu_assert_suspicious();
		return false;
	}
	bool r = fscFile->fscDirNext(dirEntry);
	return r;
}

/*
 * Create directory
 */
bool fsc_createDir(const char* path, sint32* fscStatus)
{
	fscDeviceC* fscDevice = NULL;
	*fscStatus = FSC_STATUS_UNDEFINED;
	void* ctx;
	std::string devicePath;
	if( fsc_lookupPath(path, devicePath, &fscDevice, &ctx) )
	{
		sint32 status = fscDevice->fscDeviceCreateDir(devicePath, ctx, fscStatus);
		return status;
	}
	return false;
}

/*
 * Rename file or directory
 */
bool fsc_rename(const char* srcPath, const char* dstPath, sint32* fscStatus)
{
	std::string srcDevicePath;
	std::string dstDevicePath;
	void* srcCtx;
	void* dstCtx;
	fscDeviceC* fscSrcDevice = NULL;
	fscDeviceC* fscDstDevice = NULL;
	*fscStatus = FSC_STATUS_UNDEFINED;
	if( fsc_lookupPath(srcPath, srcDevicePath, &fscSrcDevice, &srcCtx) && fsc_lookupPath(dstPath, dstDevicePath, &fscDstDevice, &dstCtx) )
	{
		if( fscSrcDevice == fscDstDevice )
			return fscSrcDevice->fscDeviceRename(srcDevicePath, dstDevicePath, srcCtx, fscStatus);
	}
	return false;
}

/*
 * Delete file or subdirectory
 */
bool fsc_remove(const char* path, sint32* fscStatus)
{
	std::string devicePath;
	fscDeviceC* fscDevice = NULL;
	*fscStatus = FSC_STATUS_UNDEFINED;
	void* ctx;
	if( fsc_lookupPath(path, devicePath, &fscDevice, &ctx) )
	{
		return fscDevice->fscDeviceRemoveFileOrDir(devicePath, ctx, fscStatus);
	}
	return false;
}

/*
 * Close file handle
 */
void fsc_close(FSCVirtualFile* fscFile)
{
	delete fscFile;
}

void fsc_flush(FSCVirtualFile* fscFile)
{
	if (!fscFile)
		return;
	fscFile->fscFlush();
}

/*
 * Return size of file
 */
uint32 fsc_getFileSize(FSCVirtualFile* fscFile)
{
	return (uint32)fscFile->fscQueryValueU64(FSC_QUERY_SIZE);
}

/*
 * Return file position
 */
uint32 fsc_getFileSeek(FSCVirtualFile* fscFile)
{
	return (uint32)fscFile->fscGetSeek();
}

/*
 * Set file seek
 * For writable files the seek pointer can be set past the end of the file
 */
void fsc_setFileSeek(FSCVirtualFile* fscFile, uint32 newSeek)
{
	uint32 fileSize = fsc_getFileSize(fscFile);
	if (fsc_isWritable(fscFile) == false)
		newSeek = std::min(newSeek, fileSize);
	fscFile->fscSetSeek((uint64)newSeek);
}

// set file length
void fsc_setFileLength(FSCVirtualFile* fscFile, uint32 newEndOffset)
{
	uint32 fileSize = fsc_getFileSize(fscFile);
	if (!fsc_isWritable(fscFile))
	{
		cemuLog_log(LogType::Force, "TruncateFile called on read-only file");
	}
	else
	{
		fscFile->fscSetFileLength((uint64)newEndOffset);
	}
}

/*
 * Returns true if the file object is a directory
 */
bool fsc_isDirectory(FSCVirtualFile* fscFile)
{
	return fscFile->fscGetType() == FSC_TYPE_DIRECTORY;
}

/*
 * Returns true if the file object is a file
 */
bool fsc_isFile(FSCVirtualFile* fscFile)
{
	return fscFile->fscGetType() == FSC_TYPE_FILE;
}

/*
 * Returns true if the file is writable
 */
bool fsc_isWritable(FSCVirtualFile* fscFile)
{	
	return fscFile->fscQueryValueU64(FSC_QUERY_WRITEABLE) != 0;
}

/*
 * Read data from file
 * Returns number of bytes successfully read
 */
uint32 fsc_readFile(FSCVirtualFile* fscFile, void* buffer, uint32 size)
{
	uint32 fscStatus = fscFile->fscReadData(buffer, size);
	return fscStatus;
}

/*
 * Write data to file
 * Returns number of bytes successfully written
 */
uint32 fsc_writeFile(FSCVirtualFile* fscFile, void* buffer, uint32 size)
{
	if (fsc_isWritable(fscFile) == false)
	{
		return 0;
	}
	if (fscFile->m_isAppend)
		fsc_setFileSeek(fscFile, fsc_getFileSize(fscFile));

	uint32 fscStatus = fscFile->fscWriteData(buffer, size);
	return fscStatus;
}

sint64 fsc_readFileInto(const char* path, void* dst, uint32 capacity, sint32 maxPriority)
{
	sint32 fscStatus = FSC_STATUS_UNDEFINED;
	FSCVirtualFile* file = fsc_open(path, FSC_ACCESS_FLAG::OPEN_FILE | FSC_ACCESS_FLAG::READ_PERMISSION, &fscStatus, maxPriority);
	if (!file)
		return -1;
	// every device stops a read at the file's end
	const uint32 got = capacity ? fsc_readFile(file, dst, capacity) : 0;
	fsc_close(file);
	return got;
}

sint32 fsc_getFilePriority(const char* path, sint32 maxPriority)
{
	std::string devicePath;
	fscDeviceC* fscDevice = nullptr;
	void* ctx;
	FSCTableRef ref;
	for (sint32 prio = maxPriority; prio >= 0; prio--)
	{
		if (!fsc_lookupPath(path, devicePath, &fscDevice, &ctx, prio))
			continue;
		sint32 fscStatus = FSC_STATUS_UNDEFINED;
		FSCVirtualFile* f = fscDevice->fscDeviceOpenByPath(devicePath, FSC_ACCESS_FLAG::OPEN_FILE | FSC_ACCESS_FLAG::READ_PERMISSION, ctx, &fscStatus);
		if (!f)
			continue;
		const bool isFile = f->fscGetType() == FSC_TYPE_FILE;
		delete f;
		if (isFile)
		{
			return prio;
		}
	}
	return -1;
}

// helper function to load a file into memory
uint8* fsc_extractFile(const char* path, uint32* fileSize, sint32 maxPriority)
{
	fscDeviceC* fscDevice = nullptr;
	sint32 fscStatus = FSC_STATUS_UNDEFINED;
	FSCVirtualFile* fscFile = fsc_open(path, FSC_ACCESS_FLAG::OPEN_FILE | FSC_ACCESS_FLAG::READ_PERMISSION, &fscStatus, maxPriority);
	if( !fscFile )
	{
		*fileSize = 0;
		return nullptr;
	}
	uint32 fscFileSize = fsc_getFileSize(fscFile);
	*fileSize = fscFileSize;
	uint8* fileMem = (uint8*)malloc(fscFileSize);
	if( fsc_readFile(fscFile, fileMem, fscFileSize) != fscFileSize )
	{
		free(fileMem);
		fsc_close(fscFile);
		*fileSize = 0;
		return nullptr;
	}
	fsc_close(fscFile);
	return fileMem;
}

std::optional<std::vector<uint8>> fsc_extractFile(const char* path, sint32 maxPriority)
{
	fscDeviceC* fscDevice = nullptr;
	sint32 fscStatus = FSC_STATUS_UNDEFINED;
	FSCVirtualFile* fscFile = fsc_open(path, FSC_ACCESS_FLAG::OPEN_FILE | FSC_ACCESS_FLAG::READ_PERMISSION, &fscStatus, maxPriority);
	if (!fscFile)
	{
		return std::nullopt;
	}
	std::vector<uint8> fileData;
	uint32 fscFileSize = fsc_getFileSize(fscFile);
	fileData.resize(fscFileSize);

	uint32 readOffset = 0;
	while (readOffset < fscFileSize)
	{
		uint32 stepReadSize = std::min(fscFileSize - readOffset, (uint32)1024 * 1024 * 32);
		uint32 numBytesRead = fsc_readFile(fscFile, fileData.data() + readOffset, stepReadSize);
		if (numBytesRead != stepReadSize)
		{
			fsc_close(fscFile);
			return std::nullopt;
		}
		readOffset += stepReadSize;
	}
	fsc_close(fscFile);
	return fileData;
}

// helper function to check if a file exists
bool fsc_doesFileExist(const char* path, sint32 maxPriority)
{
	fscDeviceC* fscDevice = nullptr;
	sint32 fscStatus = FSC_STATUS_UNDEFINED;
	FSCVirtualFile* fscFile = fsc_open(path, FSC_ACCESS_FLAG::OPEN_FILE, &fscStatus, maxPriority);
	if (!fscFile)
	{
		return false;
	}
	fsc_close(fscFile);
	return true;
}

// helper function to check if a directory exists
bool fsc_doesDirectoryExist(const char* path, sint32 maxPriority)
{
	fscDeviceC* fscDevice = nullptr;
	sint32 fscStatus = FSC_STATUS_UNDEFINED;
	FSCVirtualFile* fscFile = fsc_open(path, FSC_ACCESS_FLAG::OPEN_DIR, &fscStatus, maxPriority);
	if (!fscFile)
	{
		return false;
	}
	fsc_close(fscFile);
	return true;
}

// initialize Cemu's virtual filesystem
void fsc_init()
{
	fsc_reset();
}
