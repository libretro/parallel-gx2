#include "Common/FileStream.h"

FileStreamPool::FileStreamPool(const fs::path& path, FileStream* first) : m_path(path)
{
	if (first)
	{
		m_slots[0].store(first, std::memory_order_relaxed);
		m_opened.store(1, std::memory_order_relaxed);
	}
}

FileStreamPool::~FileStreamPool()
{
	for (std::atomic<FileStream*>& slot : m_slots)
		delete slot.exchange(nullptr, std::memory_order_acquire);
}

FileStream* FileStreamPool::Acquire()
{
	for (std::atomic<FileStream*>& slot : m_slots)
	{
		if (slot.load(std::memory_order_relaxed) == nullptr)
			continue;
		if (FileStream* fs = slot.exchange(nullptr, std::memory_order_acquire))
			return fs;
	}
	FileStream* fs = FileStream::openFile2(m_path);
	if (fs)
		m_opened.fetch_add(1, std::memory_order_relaxed);
	return fs;
}

void FileStreamPool::Release(FileStream* stream)
{
	if (!stream)
		return;
	for (std::atomic<FileStream*>& slot : m_slots)
	{
		FileStream* empty = nullptr;
		if (slot.load(std::memory_order_relaxed) == nullptr
			&& slot.compare_exchange_strong(empty, stream, std::memory_order_release, std::memory_order_relaxed))
			return;
	}
	delete stream;
}
