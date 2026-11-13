#pragma once

#include <cstddef>
#include <new>

#include "util/helpers/obj_pool.h"

// What a class derives from so that new and delete take its objects from a
// pool of its own (obj_pool.h) rather than the heap: once a game has made as
// many at once as it ever will, making and dropping them allocates nothing.
// A class derived from it again, of another size, goes to the heap as
// before. PerChunk objects are made at load, and as many more each time the
// pool runs out, up to OBJ_POOL_MAX_CHUNKS times.
template<typename T, size_t PerChunk = 256>
class PoolNew
{
	static_assert(PerChunk && (PerChunk & (PerChunk - 1)) == 0, "a power of two");

	struct Pool
	{
		Pool()
		{
			obj_pool_init(&pool, ring, (unsigned)sizeof(T), (unsigned)PerChunk);
			obj_pool_reserve(&pool, 1);
		}
		// left as it is while an object outlives it, for that one's delete
		~Pool() { obj_pool_free_all(&pool); }
		obj_pool_t pool;
		alignas(8) unsigned char ring[OBJ_POOL_RING_SIZE(PerChunk)]{};
	};
	inline static Pool s_pool;

public:
	static void* operator new(size_t size)
	{
		if (size != sizeof(T))
			return ::operator new(size);
		void* p = obj_pool_get(&s_pool.pool);
		if (!p)
			throw std::bad_alloc();
		return p;
	}

	static void operator delete(void* p, size_t size)
	{
		if (size != sizeof(T))
			::operator delete(p);
		else
			obj_pool_put(&s_pool.pool, p);
	}
};
