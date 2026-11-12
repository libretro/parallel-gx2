#include "config/ActiveSettings.h"
#include "Cafe/Filesystem/fsc.h"
#include "Cafe/Filesystem/fscDeviceHostFS.h"

#include "Common/FileStream.h"
#include "Cafe/Filesystem/host_fs.h"

/* FSCVirtualFile implementation for HostFS, over the C (host_fs.c) */

FSCVirtualFile_Host::~FSCVirtualFile_Host()
{
	host_fs_close(m_node);
}

sint32 FSCVirtualFile_Host::fscGetType()
{
	return host_fs_is_dir(m_node) ? FSC_TYPE_DIRECTORY : FSC_TYPE_FILE;
}

uint32 FSCVirtualFile_Host::fscDeviceHostFSFile_getFileSize()
{
	const uint64 size = host_fs_size(m_node);
	if (size > 0xFFFFFFFFULL)
		cemu_assert_suspicious(); // files larger than 4GB are not supported by Wii U filesystem
	return (uint32)size;
}

uint64 FSCVirtualFile_Host::fscQueryValueU64(uint32 id)
{
	if (id == FSC_QUERY_SIZE)
		return fscDeviceHostFSFile_getFileSize();
	if (id == FSC_QUERY_WRITEABLE && !host_fs_is_dir(m_node))
		return host_fs_writable(m_node);
	cemu_assert_unimplemented();
	return 0;
}

uint32 FSCVirtualFile_Host::fscWriteData(void* buffer, uint32 size)
{
	if (!host_fs_is_dir(m_node) && size >= (2UL * 1024UL * 1024UL * 1024UL))
		cemu_assert_suspicious();
	return host_fs_write(m_node, buffer, size);
}

uint32 FSCVirtualFile_Host::fscReadData(void* buffer, uint32 size)
{
	if (!host_fs_is_dir(m_node) && size >= (2UL * 1024UL * 1024UL * 1024UL))
		cemu_assert_suspicious();
	return host_fs_read(m_node, buffer, size);
}

void FSCVirtualFile_Host::fscSetSeek(uint64 seek)
{
	cemu_assert_debug(host_fs_is_dir(m_node) || seek <= host_fs_size(m_node));
	host_fs_seek(m_node, seek);
}

uint64 FSCVirtualFile_Host::fscGetSeek()
{
	return host_fs_tell(m_node);
}

void FSCVirtualFile_Host::fscSetFileLength(uint64 endOffset)
{
	if (host_fs_is_dir(m_node))
		return;
	if (!host_fs_set_length(m_node, endOffset))
		cemuLog_log(LogType::Force, "fscSetFileLength: Failed to set size to 0x{:x}", endOffset);
}

bool FSCVirtualFile_Host::fscDirNext(FSCDirEntry* dirEntry)
{
	// the name and type as the directory read gave them: nothing allocated
	bool isDir = false, failed = false;
	uint64_t size = 0;
	const bool any = host_fs_dir_next(m_node, dirEntry->path, sizeof(dirEntry->path), &isDir, &size, &failed);
	if (failed)
		cemuLog_log(LogType::Force, "Failed to iterate directory");
	if (!any)
		return false;
	dirEntry->isDirectory = isDir;
	dirEntry->isFile = !isDir;
	dirEntry->fileSize = size;
	return true;
}

void FSCVirtualFile_Host::fscFlush()
{
	// Only writable files hold anything worth flushing; the read side has
	// nothing pending, and directories have no stream at all.
	host_fs_flush(m_node);
}

FSCVirtualFile* FSCVirtualFile_Host::OpenFile(const fs::path& path, FSC_ACCESS_FLAG accessFlags, sint32& fscStatus)
{
	if (!HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::OPEN_FILE) && !HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::OPEN_DIR))
		cemu_assert_debug(false); // not allowed. At least one of both flags must be set

	unsigned flags = 0;
	if (HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::OPEN_FILE))
		flags |= HOST_FS_OPEN_FILE;
	if (HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::OPEN_DIR))
		flags |= HOST_FS_OPEN_DIR;
	if (HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::WRITE_PERMISSION))
		flags |= HOST_FS_WRITE;
	if (HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::FILE_ALLOW_CREATE))
		flags |= HOST_FS_ALLOW_CREATE;
	else if (HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::FILE_ALWAYS_CREATE))
		flags |= HOST_FS_ALWAYS_CREATE;
	bool createFailed = false;
	host_fs_node* node = host_fs_open(_pathToUtf8(path).c_str(), flags, &createFailed);
	if (createFailed)
		cemuLog_log(LogType::Force, "FSC: File create failed for {}", _pathToUtf8(path));
	if (!node)
	{
		fscStatus = FSC_STATUS_FILE_NOT_FOUND;
		return nullptr;
	}
	fscStatus = FSC_STATUS_OK;
	return new FSCVirtualFile_Host(node);
}

/* Device implementation */

class fscDeviceHostFSC : public fscDeviceC
{
public:
	FSCVirtualFile* fscDeviceOpenByPath(std::string_view path, FSC_ACCESS_FLAG accessFlags, void* ctx, sint32* fscStatus) override
	{
		*fscStatus = FSC_STATUS_OK;
		FSCVirtualFile* vf = FSCVirtualFile_Host::OpenFile(_utf8ToPath(path), accessFlags, *fscStatus);
		cemu_assert_debug((bool)vf == (*fscStatus == FSC_STATUS_OK));
		return vf;
	}

	bool fscDeviceCreateDir(std::string_view path, void* ctx, sint32* fscStatus) override
	{
		fs::path dirPath = _utf8ToPath(path);
		if (FileStream::Exists(dirPath))
		{
			if (!FileStream::IsDirectory(dirPath))
				cemuLog_log(LogType::Force, "CreateDir: {} already exists but is not a directory", path);
			*fscStatus = FSC_STATUS_ALREADY_EXISTS;
			return false;
		}
		bool r = FileStream::CreateDirectories(dirPath);
		if (!r)
			cemuLog_log(LogType::Force, "CreateDir: Failed to create {}", path);
		*fscStatus = FSC_STATUS_OK;
		return true;
	}

	bool fscDeviceRemoveFileOrDir(std::string_view path, void* ctx, sint32* fscStatus) override
	{
		*fscStatus = FSC_STATUS_OK;
		fs::path _path = _utf8ToPath(path);
		if (!FileStream::Exists(_path))
		{
			*fscStatus = FSC_STATUS_FILE_NOT_FOUND;
			return false;
		}
		if (!FileStream::Remove(_path))
		{
			cemu_assert_unimplemented(); // return correct error (e.g. if directory is non-empty)
			*fscStatus = FSC_STATUS_FILE_NOT_FOUND;
		}
		return true;
	}

	bool fscDeviceRename(std::string_view srcPath, std::string_view dstPath, void* ctx, sint32* fscStatus) override
	{
		*fscStatus = FSC_STATUS_OK;
		fs::path _srcPath = _utf8ToPath(srcPath);
		fs::path _dstPath = _utf8ToPath(dstPath);
		if (!FileStream::Exists(_srcPath))
		{
			*fscStatus = FSC_STATUS_FILE_NOT_FOUND;
			return false;
		}
		FileStream::Rename(_srcPath, _dstPath);
		return true;
	}

	// singleton
public:
	static fscDeviceHostFSC& instance()
	{
		static fscDeviceHostFSC _instance;
		return _instance;
	}
};

bool FSCDeviceHostFS_Mount(std::string_view mountPath, std::string_view hostTargetPath, sint32 priority)
{
	return fsc_mount(mountPath, hostTargetPath, &fscDeviceHostFSC::instance(), nullptr, priority) == FSC_STATUS_OK;
}