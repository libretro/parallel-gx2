#include "Cafe/Filesystem/fsc.h"
#include "Cafe/Filesystem/zarchive.h"

class FSCDeviceWuaFileCtx : public FSCVirtualFile
{
	friend class fscDeviceWUAC;

protected:
	// a directory is listed a name at a time as it is iterated (zarchive_dir),
	// nothing collected and nothing allocated; a file is read through the
	// archive's readers, with no stream of its own
	FSCDeviceWuaFileCtx(const zarchive_t* archive, const zar_file_t& node, std::string_view path, uint32 fscType)
	{
		this->m_archive = archive;
		this->m_fscType = fscType;
		this->m_node = node;
		this->m_seek = 0;
		if (fscType == FSC_TYPE_DIRECTORY)
			m_listing = zarchive_dir_open(archive, &m_dir, path.data(), path.size());
	};

public:
	~FSCDeviceWuaFileCtx() override
	{
		if (m_fscType == FSC_TYPE_DIRECTORY)
			zarchive_dir_close(&m_dir);
	}

	sint32 fscGetType() override
	{
		return m_fscType;
	}

	uint32 fscDeviceWuaFile_getFileSize()
	{
		return (uint32)m_node.size;
	}

	uint64 fscQueryValueU64(uint32 id) override
	{
		if (m_fscType == FSC_TYPE_FILE)
		{
			if (id == FSC_QUERY_SIZE)
				return fscDeviceWuaFile_getFileSize();
			else if (id == FSC_QUERY_WRITEABLE)
				return 0; // WUD images are read-only
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
		cemu_assert(size < (2ULL * 1024 * 1024 * 1024)); // single read operation larger than 2GiB not supported
		uint32 bytesLeft = fscDeviceWuaFile_getFileSize() - m_seek;
		uint32 bytesToRead = (std::min)(bytesLeft, (uint32)size);
		uint32 bytesSuccessfullyRead = (uint32)zarchive_read(m_archive, &m_node, m_seek, buffer, bytesToRead);
		m_seek += bytesSuccessfullyRead;
		return bytesSuccessfullyRead;
	}

	void fscSetSeek(uint64 seek) override
	{
		if (m_fscType != FSC_TYPE_FILE)
			return;
		cemu_assert_debug(seek <= 0xFFFFFFFFULL);
		this->m_seek = (uint32)seek;
	}

	uint64 fscGetSeek() override
	{
		if (m_fscType != FSC_TYPE_FILE)
			return 0;
		return m_seek;
	}

	bool fscDirNext(FSCDirEntry* dirEntry) override
	{
		if (m_fscType != FSC_TYPE_DIRECTORY || !m_listing)
			return false;
		const char* name;
		size_t len;
		zar_file_t node;
		if (!zarchive_dir_next(&m_dir, &name, &len, &node))
			return false;
		if (node.dir)
		{
			dirEntry->isDirectory = true;
			dirEntry->isFile = false;
			dirEntry->fileSize = 0;
		}
		else
		{
			dirEntry->isDirectory = false;
			dirEntry->isFile = true;
			dirEntry->fileSize = (uint32)node.size;
		}
		// cut to fit, and ended
		const size_t n = std::min(sizeof(dirEntry->path) - 1, len);
		memcpy(dirEntry->path, name, n);
		memset(dirEntry->path + n, 0, sizeof(dirEntry->path) - n);
		return true;
	}

private:
	const zarchive_t* m_archive{nullptr};
	sint32 m_fscType;
	zar_file_t m_node;
	uint32 m_seek{0};
	// directory
	zarchive_dir_t m_dir{};
	bool m_listing{false};
};

class fscDeviceWUAC : public fscDeviceC
{
	FSCVirtualFile* fscDeviceOpenByPath(std::string_view path, FSC_ACCESS_FLAG accessFlags, void* ctx, sint32* fscStatus) override
	{
		const zarchive_t* archive = (const zarchive_t*)ctx;
		cemu_assert_debug(!HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::WRITE_PERMISSION)); // writing to WUA is not supported

		zar_file_t node;
		if (!zarchive_lookup(archive, path.data(), path.size(), &node) || !HAS_FLAG(accessFlags, node.dir ? FSC_ACCESS_FLAG::OPEN_DIR : FSC_ACCESS_FLAG::OPEN_FILE))
		{
			*fscStatus = FSC_STATUS_FILE_NOT_FOUND;
			return nullptr;
		}
		*fscStatus = FSC_STATUS_OK;
		return new FSCDeviceWuaFileCtx(archive, node, path, node.dir ? FSC_TYPE_DIRECTORY : FSC_TYPE_FILE);
	}

	// singleton
public:
	static fscDeviceWUAC& instance()
	{
		static fscDeviceWUAC _instance;
		return _instance;
	}
};

bool FSCDeviceWUA_Mount(std::string_view mountPath, std::string_view destinationBaseDir, zarchive_t* archive, sint32 priority)
{
	return fsc_mount(mountPath, destinationBaseDir, &fscDeviceWUAC::instance(), archive, priority) == FSC_STATUS_OK;
}