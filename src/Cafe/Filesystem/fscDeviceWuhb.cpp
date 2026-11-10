#include "Cafe/Filesystem/WUHB/wuhb.h"
#include "Common/FileStream.h"
#include "Cafe/Filesystem/fsc.h"

class FSCDeviceWuhbFileCtx : public FSCVirtualFile
{
  public:
	FSCDeviceWuhbFileCtx(wuhb_t* reader, uint32 entryOffset, uint32 fscType)
		: m_wuhbReader(reader), m_fscType(fscType), m_entryOffset(entryOffset)
	{
		cemu_assert(entryOffset != WUHB_ENTRY_EMPTY);
		wuhb_dir_t entry;
		if (fscType == FSC_TYPE_DIRECTORY && wuhb_dir_entry(reader, entryOffset, &entry, nullptr, 0))
		{
			m_dirIterOffset = entry.dir_list_head;
			m_fileIterOffset = entry.file_list_head;
		}
	}
	sint32 fscGetType() override
	{
		return m_fscType;
	}
	uint64 fscQueryValueU64(uint32 id) override
	{
		if (m_fscType == FSC_TYPE_FILE)
		{
			if (id == FSC_QUERY_SIZE)
				return wuhb_file_size(m_wuhbReader, m_entryOffset);
			else if (id == FSC_QUERY_WRITEABLE)
				return 0; // WUHB images are read-only
			else
				cemu_assert_error();
		}
		else
		{
			cemu_assert_unimplemented();
		}
		return 0;
	}
	uint32 fscWriteData(void* buffer, uint32 size) override
	{
		cemu_assert_error();
		return 0;
	}
	uint32 fscReadData(void* buffer, uint32 size) override
	{
		if (m_fscType != FSC_TYPE_FILE)
			return 0;
		const uint32 read = (uint32)wuhb_read_file(m_wuhbReader, m_entryOffset, m_seek, size, buffer);
		m_seek += read;
		return read;
	}
	void fscSetSeek(uint64 seek) override
	{
		m_seek = seek;
	}
	uint64 fscGetSeek() override
	{
		if (m_fscType != FSC_TYPE_FILE)
			return 0;
		return m_seek;
	}
	void fscSetFileLength(uint64 endOffset) override
	{
		cemu_assert_error();
	}
	// each name read straight into the entry, nothing allocated
	bool fscDirNext(FSCDirEntry* dirEntry) override
	{
		while (m_dirIterOffset != WUHB_ENTRY_EMPTY)
		{
			wuhb_dir_t entry;
			if (!wuhb_dir_entry(m_wuhbReader, m_dirIterOffset, &entry, dirEntry->path, sizeof(dirEntry->path)))
			{
				m_dirIterOffset = WUHB_ENTRY_EMPTY;
				break;
			}
			m_dirIterOffset = entry.list_next;
			if (entry.name_size > 0)
			{
				dirEntry->isDirectory = true;
				dirEntry->isFile = false;
				dirEntry->fileSize = 0;
				return true;
			}
		}
		while (m_fileIterOffset != WUHB_ENTRY_EMPTY)
		{
			wuhb_file_t entry;
			if (!wuhb_file_entry(m_wuhbReader, m_fileIterOffset, &entry, dirEntry->path, sizeof(dirEntry->path)))
			{
				m_fileIterOffset = WUHB_ENTRY_EMPTY;
				break;
			}
			m_fileIterOffset = entry.list_next;
			if (entry.name_size > 0)
			{
				dirEntry->isDirectory = false;
				dirEntry->isFile = true;
				dirEntry->fileSize = (uint32)entry.size;
				return true;
			}
		}
		return false;
	}

  private:
	wuhb_t* m_wuhbReader{};
	uint32 m_fscType;
	uint32 m_entryOffset = WUHB_ENTRY_EMPTY;
	uint32 m_dirIterOffset = WUHB_ENTRY_EMPTY;
	uint32 m_fileIterOffset = WUHB_ENTRY_EMPTY;
	uint64 m_seek = 0;
};

class fscDeviceWUHB : public fscDeviceC
{
	FSCVirtualFile* fscDeviceOpenByPath(std::string_view path, FSC_ACCESS_FLAG accessFlags, void* ctx, sint32* fscStatus) override
	{
		wuhb_t* reader = (wuhb_t*)ctx;
		cemu_assert_debug(!HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::WRITE_PERMISSION)); // writing to WUHB is not supported

		bool isFile;
		uint32 table_offset = WUHB_ENTRY_EMPTY;

		// the path looked up where it lies: nothing allocated
		if (table_offset == WUHB_ENTRY_EMPTY && HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::OPEN_DIR))
		{
			table_offset = wuhb_lookup(reader, path.data(), path.size(), false);
			isFile = false;
		}
		if (table_offset == WUHB_ENTRY_EMPTY && HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::OPEN_FILE))
		{
			table_offset = wuhb_lookup(reader, path.data(), path.size(), true);
			isFile = true;
		}

		if (table_offset == WUHB_ENTRY_EMPTY)
		{
			*fscStatus = FSC_STATUS_FILE_NOT_FOUND;
			return nullptr;
		}

		*fscStatus = FSC_STATUS_OK;
		return new FSCDeviceWuhbFileCtx(reader, table_offset, isFile ? FSC_TYPE_FILE : FSC_TYPE_DIRECTORY);
	}

	// singleton
  public:
	static fscDeviceWUHB& instance()
	{
		static fscDeviceWUHB _instance;
		return _instance;
	}
};

bool FSCDeviceWUHB_Mount(std::string_view mountPath, std::string_view destinationBaseDir, wuhb_t* wuhbReader, sint32 priority)
{
	return fsc_mount(mountPath, destinationBaseDir, &fscDeviceWUHB::instance(), wuhbReader, priority) == FSC_STATUS_OK;
}
