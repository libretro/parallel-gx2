#pragma once

#include <span>
#include <string_view>

#include "util/helpers/mem_stream.h"

// Big-endian records read from and written to memory, over the C streams
// (mem_stream.h): a read past the end gives 0 and sets hasError(); a write
// that cannot grow the buffer is dropped.
class MemStreamReader
{
public:
	MemStreamReader(const uint8* data, size_t size) { mem_reader_init(&m_r, data, size); }

	template<typename T> T readBE()
	{
		static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8);
		if constexpr (sizeof(T) == 1)
			return (T)mem_read_u8(&m_r);
		else if constexpr (sizeof(T) == 2)
			return (T)mem_read_u16(&m_r);
		else if constexpr (sizeof(T) == 4)
			return (T)mem_read_u32(&m_r);
		else
			return (T)mem_read_u64(&m_r);
	}

	// up to the next '\n' or the end, without it or '\r's at either end;
	// empty, with hasError() set, at the end
	std::string_view readLine()
	{
		const char* s;
		size_t len;
		if (!mem_read_line(&m_r, &s, &len))
			return {};
		return std::string_view(s, len);
	}

	bool readData(void* ptr, size_t size) { return mem_read(&m_r, ptr, size) != 0; }

	// lent where it lies; empty when fewer are left
	std::span<uint8> readDataNoCopy(size_t size)
	{
		const uint8_t* p = mem_read_view(&m_r, size);
		return p ? std::span<uint8>((uint8*)p, size) : std::span<uint8>();
	}

	// true if any read was out of bounds
	bool hasError() const { return m_r.error != 0; }
	bool isEndOfStream() const { return m_r.pos == m_r.size; }

private:
	mem_reader_t m_r;
};

class MemStreamWriter
{
public:
	explicit MemStreamWriter(size_t reservedSize) { mem_writer_init(&m_w, reservedSize ? reservedSize : 128); }
	~MemStreamWriter() { mem_writer_free(&m_w); }
	MemStreamWriter(const MemStreamWriter&) = delete;
	MemStreamWriter& operator=(const MemStreamWriter&) = delete;

	void writeData(const void* ptr, size_t size) { mem_write(&m_w, ptr, size); }

	template<typename T> void writeBE(const T& v)
	{
		static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8);
		if constexpr (sizeof(T) == 1)
			mem_write_u8(&m_w, (uint8_t)v);
		else if constexpr (sizeof(T) == 2)
			mem_write_u16(&m_w, (uint16_t)v);
		else if constexpr (sizeof(T) == 4)
			mem_write_u32(&m_w, (uint32_t)v);
		else
			mem_write_u64(&m_w, (uint64_t)v);
	}

	// what was written, lent until the next write or reset
	std::span<uint8> getResult() { return std::span<uint8>(m_w.buf, m_w.len); }
	// emptied, keeping its room
	void reset() { mem_writer_reset(&m_w); }
	// a write was dropped for want of memory
	bool failed() const { return m_w.failed != 0; }

private:
	mem_writer_t m_w;
};
