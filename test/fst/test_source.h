#pragma once
// A source of the test's own for an FST volume (fst_source.h): a C++ object
// with the two reads, handed to the C source as its context. For the tests
// of the FST reader (fst) and of the sources themselves (disc_image).
#include "Cafe/Filesystem/FST/fst_source.h"

struct TestSource
{
	virtual ~TestSource() = default;
	virtual uint64 readData(uint16 cluster, uint64 clusterOffset, uint64 offset, void* data, uint64 size) = 0;
	virtual const uint8* Lend(uint16 cluster, uint64 clusterOffset, uint64 offset, uint64 size) { return nullptr; }
};

namespace test_source
{
	inline uint64_t Read(void* ctx, uint16_t cluster, uint64_t clusterOffset, uint64_t offset, void* dst, uint64_t size)
	{
		return ((TestSource*)ctx)->readData(cluster, clusterOffset, offset, dst, size);
	}
	inline const uint8_t* Lend(void* ctx, uint16_t cluster, uint64_t clusterOffset, uint64_t offset, uint64_t size)
	{
		return ((TestSource*)ctx)->Lend(cluster, clusterOffset, offset, size);
	}
	inline void Delete(void* ctx)
	{
		delete (TestSource*)ctx;
	}
}

// owned: closing the source deletes the object
inline fst_source_t* AsSource(TestSource* s, bool owned)
{
	const fst_source_ops_t ops = {test_source::Read, test_source::Lend, owned ? test_source::Delete : nullptr};
	return fst_source_new(&ops, s);
}
