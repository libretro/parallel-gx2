#pragma once

#include <cstdlib>
#include <utility>

#include "util/helpers/rcu_cell.h"

// A value read all the time and replaced now and then, without a lock on the
// way in: the C++ over rcu_cell.h, which says how. A reader counts itself in
// and reads an object nothing will change or free while it is counted in; a
// writer builds the new value, swaps it in, and frees the old one once every
// reader that may still hold it has left. Readers never wait; writers take
// turns with each other and wait only for readers already inside. For
// configuration a game reads every frame and a menu changes - the emulated
// controllers and their mappings.
template<typename T>
class RcuCell
{
public:
	RcuCell() : RcuCell(T()) {}
	explicit RcuCell(T initial)
	{
		if (!rcu_cell_init(&m_cell, new T(std::move(initial))))
			std::abort();
	}
	~RcuCell() { delete (T*)rcu_cell_deinit(&m_cell); }
	RcuCell(const RcuCell&) = delete;
	RcuCell& operator=(const RcuCell&) = delete;

	class Reader
	{
	public:
		explicit Reader(const RcuCell& cell) { rcu_read_begin(const_cast<rcu_cell_t*>(&cell.m_cell), &m_reader); }
		~Reader() { rcu_read_end(&m_reader); }
		Reader(const Reader&) = delete;
		Reader& operator=(const Reader&) = delete;

		const T& operator*() const { return *(const T*)m_reader.ptr; }
		const T* operator->() const { return (const T*)m_reader.ptr; }

	private:
		rcu_reader_t m_reader;
	};

	Reader Read() const { return Reader(*this); }

	// make() builds the new value, in this writer's turn - so it may read
	// state only writers change, as the previous writer left it
	template<typename TMake>
	void Write(TMake make)
	{
		rcu_write_begin(&m_cell);
		T* fresh = new T(make());
		delete (T*)rcu_write_publish(&m_cell, fresh);
	}

private:
	rcu_cell_t m_cell;
};
