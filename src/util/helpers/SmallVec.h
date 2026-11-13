#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <type_traits>
#include <utility>

// A vector of trivially copyable items whose first N live in the object:
// the texture cache's per-texture and per-view lists, which mostly hold a
// handful, allocate nothing while a game plays until one grows past N. Past
// that it doubles on the heap, as std::vector does, and keeps that room.
// What the texture cache asks of std::vector, and no more.
template<typename T, size_t N>
class SmallVec
{
	static_assert(std::is_trivially_copyable<T>::value, "items are moved as octets");
	static_assert(N > 0, "room for one at least");

public:
	SmallVec() = default;
	SmallVec(const SmallVec&) = delete;
	SmallVec& operator=(const SmallVec&) = delete;
	~SmallVec()
	{
		if (m_items != m_inline)
			delete[] m_items;
	}

	T* begin() { return m_items; }
	T* end() { return m_items + m_count; }
	const T* begin() const { return m_items; }
	const T* end() const { return m_items + m_count; }
	T* data() { return m_items; }
	size_t size() const { return m_count; }
	bool empty() const { return m_count == 0; }
	T& operator[](size_t i) { return m_items[i]; }
	const T& operator[](size_t i) const { return m_items[i]; }
	T& front() { return m_items[0]; }
	T& back() { return m_items[m_count - 1]; }

	void push_back(const T& v)
	{
		if (m_count == m_cap)
			Grow(m_cap * 2);
		m_items[m_count++] = v;
	}

	template<typename... Args>
	T& emplace_back(Args&&... args)
	{
		if (m_count == m_cap)
			Grow(m_cap * 2);
		m_items[m_count] = T{std::forward<Args>(args)...};
		return m_items[m_count++];
	}

	void pop_back() { m_count--; }
	void clear() { m_count = 0; }

	// new items value-made, or v
	void resize(size_t n) { resize(n, T{}); }

	void resize(size_t n, const T& v)
	{
		if (n > m_cap)
			Grow(n);
		for (size_t i = m_count; i < n; i++)
			m_items[i] = v;
		m_count = n;
	}

	T* erase(T* it) { return erase(it, it + 1); }

	T* erase(T* first, T* last)
	{
		const size_t at = (size_t)(first - m_items), n = (size_t)(last - first);
		if (n)
		{
			std::memmove((void*)first, (const void*)last, (m_count - at - n) * sizeof(T));
			m_count -= n;
		}
		return m_items + at;
	}

private:
	void Grow(size_t cap)
	{
		T* items = new T[cap];
		std::memcpy((void*)items, (const void*)m_items, m_count * sizeof(T));
		if (m_items != m_inline)
			delete[] m_items;
		m_items = items;
		m_cap = cap;
	}

	T m_inline[N];
	T* m_items{m_inline};
	size_t m_count{0};
	size_t m_cap{N};
};

template<typename T, size_t N>
void vectorAppendUnique(SmallVec<T, N>& vec, const T& val)
{
	if (std::find(vec.begin(), vec.end(), val) != vec.end())
		return;
	vec.push_back(val);
}

template<typename T, size_t N>
void vectorRemoveByValue(SmallVec<T, N>& vec, const T& val)
{
	vec.erase(std::remove(vec.begin(), vec.end(), val), vec.end());
}

template<typename T, size_t N>
void vectorRemoveByIndex(SmallVec<T, N>& vec, const size_t index)
{
	vec.erase(vec.begin() + index);
}
