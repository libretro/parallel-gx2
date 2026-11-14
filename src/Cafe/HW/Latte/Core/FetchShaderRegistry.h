#pragma once

#include <cstddef>
#include <cstdint>

#include "util/helpers/u64_map.h"

// Objects by a 64-bit key in a fixed table (u64_map.c), for one thread: no
// lock and nothing allocated. The fetch shaders by the hash of their program
// and the GPU's fetch shader addresses use it, both the GPU thread's alone.
// It takes up to three quarters of Slots; past that an object is not kept.
template<typename T, size_t Slots>
class FetchShaderRegistry
{
	static_assert(Slots && (Slots & (Slots - 1)) == 0, "a power of two");

public:
	FetchShaderRegistry() { Clear(); }

	T* Find(uint64_t key) const
	{
		return static_cast<T*>(u64_map_get(&m_map, key));
	}

	// obj under key, unless one is there already: that one is returned, and
	// obj is not kept. nullptr when obj is kept; obj itself when the table is
	// full and it could not be.
	T* Register(uint64_t key, T* obj)
	{
		if (T* had = Find(key))
			return had;
		return u64_map_put(&m_map, key, obj) ? nullptr : obj;
	}

	// obj out, if it is what key has
	void Unregister(uint64_t key, const T* obj)
	{
		if (Find(key) == obj)
			u64_map_take(&m_map, key);
	}

	size_t Count() const { return m_map.count; }

	void Clear()
	{
		for (auto& s : m_slots)
			s = {};
		u64_map_init(&m_map, m_slots, Slots);
	}

private:
	u64_map_t m_map{};
	u64_map_slot_t m_slots[Slots]{};
};
