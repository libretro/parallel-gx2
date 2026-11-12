#include "Cafe/Filesystem/fsc.h"
#include "Common/FileStream.h"

// a file or directory of a mounted host folder, over the C (host_fs.c)
class FSCVirtualFile_Host : public FSCVirtualFile
{
public:
	static FSCVirtualFile* OpenFile(const fs::path& path, FSC_ACCESS_FLAG accessFlags, sint32& fscStatus);
	~FSCVirtualFile_Host() override;

	sint32 fscGetType() override;

	uint32 fscDeviceHostFSFile_getFileSize();

	uint64 fscQueryValueU64(uint32 id) override;
	uint32 fscWriteData(void* buffer, uint32 size) override;
	uint32 fscReadData(void* buffer, uint32 size) override;
	void fscSetSeek(uint64 seek) override;
	uint64 fscGetSeek() override;
	void fscSetFileLength(uint64 endOffset) override;
	bool fscDirNext(FSCDirEntry* dirEntry) override;
	void fscFlush() override;

private:
	explicit FSCVirtualFile_Host(struct host_fs_node* node) : m_node(node) {};

	struct host_fs_node* m_node;
};
