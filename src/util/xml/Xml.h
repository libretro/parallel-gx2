#pragma once

// XML documents read and written with libretro-common's rxml, the tree kept
// in C (xml_tree.h). A Document owns the tree; Node, Attribute and Text are
// handles into it, and as in pugixml, whose interface this follows, a
// handle that refers to nothing is safe to use: a child of nothing is
// nothing and its text is "". Text that is only whitespace counts as no
// text, as pugixml's default parse drops it.

#include <cstddef>
#include <cstdlib>
#include <limits>
#include <type_traits>
#include <string>
#include <string_view>

#include "util/xml/xml_tree.h"

namespace xml
{
	using Element = xml_elem_t;

	namespace detail
	{
		template<typename T>
		T Integer(const char* s, T def)
		{
			if constexpr (std::is_signed_v<T>)
				return (T)xml_to_signed(s, def, std::numeric_limits<T>::min(), std::numeric_limits<T>::max());
			else
				return (T)xml_to_unsigned(s, def, std::numeric_limits<T>::max());
		}

		inline bool Bool(const char* s, bool def)
		{
			if (!s || !*s)
				return def;
			const char c = *s;
			return c == '1' || c == 't' || c == 'T' || c == 'y' || c == 'Y';
		}
	}

	class Attribute
	{
	public:
		Attribute() = default;
		explicit Attribute(xml_attr_t* a) : m_a(a) {}
		explicit operator bool() const { return m_a != nullptr; }
		bool empty() const { return !m_a; }
		const char* name() const { return m_a ? m_a->name : ""; }
		const char* value() const { return m_a ? m_a->value : ""; }
		const char* as_string(const char* def = "") const { return m_a ? m_a->value : def; }
		int as_int(int def = 0) const { return m_a ? detail::Integer<int>(m_a->value, def) : def; }
		unsigned as_uint(unsigned def = 0) const { return m_a ? detail::Integer<unsigned>(m_a->value, def) : def; }
		long long as_llong(long long def = 0) const { return m_a ? detail::Integer<long long>(m_a->value, def) : def; }
		unsigned long long as_ullong(unsigned long long def = 0) const { return m_a ? detail::Integer<unsigned long long>(m_a->value, def) : def; }
		bool as_bool(bool def = false) const { return m_a ? detail::Bool(m_a->value, def) : def; }
		bool set_value(std::string_view v) { return m_a && xml_set_str(&m_a->value, v.data(), v.size()); }
		Attribute& operator=(std::string_view v) { set_value(v); return *this; }
	private:
		xml_attr_t* m_a = nullptr;
	};

	class Text
	{
	public:
		Text() = default;
		explicit Text(xml_elem_t* e) : m_e(e) {}
		explicit operator bool() const { return m_e && m_e->text && *m_e->text; }
		bool empty() const { return !*this; }
		const char* get() const { return *this ? m_e->text : ""; }
		const char* as_string(const char* def = "") const { return *this ? m_e->text : def; }
		int as_int(int def = 0) const { return *this ? detail::Integer<int>(m_e->text, def) : def; }
		unsigned as_uint(unsigned def = 0) const { return *this ? detail::Integer<unsigned>(m_e->text, def) : def; }
		long long as_llong(long long def = 0) const { return *this ? detail::Integer<long long>(m_e->text, def) : def; }
		unsigned long long as_ullong(unsigned long long def = 0) const { return *this ? detail::Integer<unsigned long long>(m_e->text, def) : def; }
		bool as_bool(bool def = false) const { return *this ? detail::Bool(m_e->text, def) : def; }
		float as_float(float def = 0.0f) const { return *this ? strtof(m_e->text, nullptr) : def; }
		double as_double(double def = 0.0) const { return *this ? strtod(m_e->text, nullptr) : def; }
		bool set(std::string_view v) { return m_e && xml_set_str(&m_e->text, v.data(), v.size()); }
		Text& operator=(std::string_view v) { set(v); return *this; }
	private:
		xml_elem_t* m_e = nullptr;
	};

	class Node
	{
	public:
		class Range;

		Node() = default;
		explicit Node(xml_elem_t* e) : m_e(e) {}
		explicit operator bool() const { return m_e != nullptr; }
		bool empty() const { return !m_e; }
		bool operator==(const Node& o) const { return m_e == o.m_e; }
		bool operator!=(const Node& o) const { return m_e != o.m_e; }

		const char* name() const { return m_e ? m_e->name : ""; }
		Node parent() const { return Node(m_e ? m_e->parent : nullptr); }
		Node child(std::string_view name) const { return Node(xml_elem_child(m_e, name.data(), name.size())); }
		Node first_child() const { return Node(m_e ? m_e->first : nullptr); }
		Node next_sibling() const { return Node(m_e ? m_e->next : nullptr); }
		Node next_sibling(std::string_view name) const { return Node(xml_elem_next_named(m_e ? m_e->next : nullptr, name.data(), name.size())); }
		// the first element of that name anywhere under this one, depth
		// first (what pugixml's select_node("//name") finds)
		Node find_descendant(std::string_view name) const { return Node(xml_elem_find(m_e, name.data(), name.size())); }
		Range children() const;
		Range children(std::string_view name) const;
		// a node iterates over its children
		auto begin() const;
		auto end() const;

		const char* child_value() const { return Text(m_e).get(); }
		const char* child_value(std::string_view name) const { return child(name).child_value(); }
		Text text() const { return Text(m_e); }
		Attribute attribute(std::string_view name) const { return Attribute(xml_elem_attr(m_e, name.data(), name.size())); }

		Node append_child(std::string_view name) { return Node(xml_elem_append(m_e, name.data(), name.size())); }
		Node prepend_child(std::string_view name) { return Node(xml_elem_prepend(m_e, name.data(), name.size())); }
		Attribute append_attribute(std::string_view name) { return Attribute(xml_elem_add_attr(m_e, name.data(), name.size())); }

		xml_elem_t* element() const { return m_e; }

	protected:
		xml_elem_t* m_e = nullptr;
	};

	class Node::Range
	{
	public:
		class iterator
		{
		public:
			iterator(xml_elem_t* e, std::string_view name) : m_e(e), m_name(name) {}
			Node operator*() const { return Node(m_e); }
			iterator& operator++() { m_e = xml_elem_next_named(m_e->next, m_name.data(), m_name.size()); return *this; }
			bool operator!=(const iterator& o) const { return m_e != o.m_e; }
			bool operator==(const iterator& o) const { return m_e == o.m_e; }
		private:
			xml_elem_t* m_e;
			std::string_view m_name;
		};
		Range(xml_elem_t* first, std::string_view name) : m_first(first), m_name(name) {}
		iterator begin() const { return iterator(m_first, m_name); }
		iterator end() const { return iterator(nullptr, m_name); }
	private:
		xml_elem_t* m_first;
		std::string_view m_name;
	};

	inline Node::Range Node::children() const { return Range(m_e ? m_e->first : nullptr, {}); }
	inline Node::Range Node::children(std::string_view name) const { return Range(xml_elem_next_named(m_e ? m_e->first : nullptr, name.data(), name.size()), name); }
	inline auto Node::begin() const { return children().begin(); }
	inline auto Node::end() const { return children().end(); }

	class Document : public Node
	{
	public:
		Document() { m_e = xml_elem_new("", 0); }
		Document(const Document&) = delete;
		Document& operator=(const Document&) = delete;
		~Document() { xml_elem_free(m_e); }

		// Replaces what the document holds; false (and an empty document)
		// when the bytes are not one well-formed XML document
		bool load(const void* data, size_t size)
		{
			m_errorOffset = 0;
			return xml_tree_load(m_e, data, size, &m_errorOffset);
		}
		bool load(std::string_view s) { return load(s.data(), s.size()); }
		// where parsing stopped, after a load that failed
		size_t error_offset() const { return m_errorOffset; }
		void reset() { xml_elem_clear(m_e); m_errorOffset = 0; }
		// the root element
		Node document_element() const { return first_child(); }
		// UTF-8, with an XML declaration; one element a line indented by
		// indent, or all on one line for nullptr. Empty if the document has
		// no root element.
		std::string save(const char* indent = "\t", bool declaration = true) const
		{
			size_t len = 0;
			char* out = xml_tree_save(m_e, indent, declaration, &len);
			if (!out)
				return {};
			std::string s(out, len);
			free(out);
			return s;
		}

	private:
		size_t m_errorOffset = 0;
	};
}
