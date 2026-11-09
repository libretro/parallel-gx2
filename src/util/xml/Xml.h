#pragma once

// XML documents read and written with libretro-common's rxml. A Document
// owns a tree of elements; Node, Attribute and Text are handles into it,
// and as in pugixml, whose interface this follows, a handle that refers to
// nothing is safe to use: a child of nothing is nothing and its text is "".
// Text that is only whitespace counts as no text, as pugixml's default
// parse drops it.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xml
{
	struct Element
	{
		std::string name;
		std::string text;
		std::vector<std::pair<std::string, std::string>> attributes;
		std::vector<std::unique_ptr<Element>> children;
		Element* parent = nullptr;
		size_t index = 0; // in parent->children
	};

	class Attribute
	{
	public:
		Attribute() = default;
		Attribute(Element* e, size_t i) : m_e(e), m_i(i) {}
		explicit operator bool() const { return m_e != nullptr; }
		bool empty() const { return !m_e; }
		const char* name() const;
		const char* value() const;
		const char* as_string(const char* def = "") const { return m_e ? value() : def; }
		int as_int(int def = 0) const;
		unsigned as_uint(unsigned def = 0) const;
		long long as_llong(long long def = 0) const;
		unsigned long long as_ullong(unsigned long long def = 0) const;
		bool as_bool(bool def = false) const;
		bool set_value(std::string_view v);
		Attribute& operator=(std::string_view v) { set_value(v); return *this; }
	private:
		Element* m_e = nullptr;
		size_t m_i = 0;
	};

	class Text
	{
	public:
		Text() = default;
		explicit Text(Element* e) : m_e(e) {}
		explicit operator bool() const { return m_e && !m_e->text.empty(); }
		bool empty() const { return !*this; }
		const char* get() const { return m_e ? m_e->text.c_str() : ""; }
		const char* as_string(const char* def = "") const { return *this ? m_e->text.c_str() : def; }
		int as_int(int def = 0) const;
		unsigned as_uint(unsigned def = 0) const;
		long long as_llong(long long def = 0) const;
		unsigned long long as_ullong(unsigned long long def = 0) const;
		bool as_bool(bool def = false) const;
		float as_float(float def = 0.0f) const;
		double as_double(double def = 0.0) const;
		bool set(std::string_view v);
		Text& operator=(std::string_view v) { set(v); return *this; }
	private:
		Element* m_e = nullptr;
	};

	class Node
	{
	public:
		class Range;

		Node() = default;
		explicit Node(Element* e) : m_e(e) {}
		explicit operator bool() const { return m_e != nullptr; }
		bool empty() const { return !m_e; }
		bool operator==(const Node& o) const { return m_e == o.m_e; }
		bool operator!=(const Node& o) const { return m_e != o.m_e; }

		const char* name() const { return m_e ? m_e->name.c_str() : ""; }
		Node parent() const;
		Node child(std::string_view name) const;
		Node first_child() const;
		Node next_sibling() const;
		Node next_sibling(std::string_view name) const;
		// the first element of that name anywhere under this one, depth
		// first (what pugixml's select_node("//name") finds)
		Node find_descendant(std::string_view name) const;
		Range children() const;
		Range children(std::string_view name) const;
		// a node iterates over its children
		auto begin() const;
		auto end() const;

		const char* child_value() const { return m_e ? m_e->text.c_str() : ""; }
		const char* child_value(std::string_view name) const { return child(name).child_value(); }
		Text text() const { return Text(m_e); }
		Attribute attribute(std::string_view name) const;

		Node append_child(std::string_view name);
		Node prepend_child(std::string_view name);
		Attribute append_attribute(std::string_view name);

		Element* element() const { return m_e; }

	protected:
		Element* m_e = nullptr;
	};

	class Node::Range
	{
	public:
		class iterator
		{
		public:
			iterator(Element* e, std::string_view name) : m_e(e), m_name(name) {}
			Node operator*() const { return Node(m_e); }
			iterator& operator++();
			bool operator!=(const iterator& o) const { return m_e != o.m_e; }
			bool operator==(const iterator& o) const { return m_e == o.m_e; }
		private:
			Element* m_e;
			std::string_view m_name;
		};
		Range(Element* first, std::string_view name) : m_first(first), m_name(name) {}
		iterator begin() const { return iterator(m_first, m_name); }
		iterator end() const { return iterator(nullptr, m_name); }
	private:
		Element* m_first;
		std::string_view m_name;
	};

	inline auto Node::begin() const { return children().begin(); }
	inline auto Node::end() const { return children().end(); }

	class Document : public Node
	{
	public:
		Document();
		Document(const Document&) = delete;
		Document& operator=(const Document&) = delete;

		// Replaces what the document holds; false (and an empty document)
		// when the bytes are not one well-formed XML document
		bool load(const void* data, size_t size);
		bool load(std::string_view s) { return load(s.data(), s.size()); }
		// where parsing stopped, after a load that failed
		size_t error_offset() const { return m_errorOffset; }
		void reset();
		// the root element
		Node document_element() const { return first_child(); }
		// UTF-8, with an XML declaration; one element a line indented by
		// indent, or all on one line for nullptr. Empty if the document has
		// no root element.
		std::string save(const char* indent = "\t", bool declaration = true) const;

	private:
		std::unique_ptr<Element> m_doc;
		size_t m_errorOffset = 0;
	};
}
