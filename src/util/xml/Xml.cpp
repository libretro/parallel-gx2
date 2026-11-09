#include "util/xml/Xml.h"

#include <cstdlib>
#include <cstring>
#include <climits>
#include <limits>
#include <type_traits>

#include <formats/rxml.h>

namespace xml
{
	namespace
	{
		bool IsSpace(char c)
		{
			return c == ' ' || c == '\t' || c == '\n' || c == '\r';
		}

		// As pugixml: leading whitespace, a sign, and a 0x prefix for hex.
		// Out of range saturates.
		template<typename T>
		T ParseInteger(const char* s, T def)
		{
			if (!s)
				return def;
			while (IsSpace(*s))
				s++;
			bool negative = false;
			if (*s == '-' || *s == '+')
				negative = *s++ == '-';
			int base = 10;
			if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
			{
				base = 16;
				s += 2;
			}
			unsigned long long v = 0;
			bool overflow = false, any = false;
			for (;; s++)
			{
				unsigned d;
				if (*s >= '0' && *s <= '9')
					d = (unsigned)(*s - '0');
				else if (base == 16 && (*s | 32) >= 'a' && (*s | 32) <= 'f')
					d = (unsigned)((*s | 32) - 'a' + 10);
				else
					break;
				any = true;
				if (v > (ULLONG_MAX - d) / (unsigned)base)
					overflow = true;
				else
					v = v * (unsigned)base + d;
			}
			if (!any)
				return 0;
			if constexpr (std::is_signed_v<T>)
			{
				using U = std::make_unsigned_t<T>;
				const unsigned long long max = (unsigned long long)std::numeric_limits<T>::max();
				if (negative)
				{
					if (overflow || v > max + 1)
						return std::numeric_limits<T>::min();
					return (T)(U)(0 - (U)v);
				}
				if (overflow || v > max)
					return std::numeric_limits<T>::max();
				return (T)v;
			}
			else
			{
				if (overflow || v > (unsigned long long)std::numeric_limits<T>::max())
					return std::numeric_limits<T>::max();
				return negative ? (T)(0 - (T)v) : (T)v;
			}
		}

		bool ParseBool(const char* s, bool def)
		{
			if (!s || !*s)
				return def;
			const char c = *s;
			return c == '1' || c == 't' || c == 'T' || c == 'y' || c == 'Y';
		}

		bool IsBlank(const char* s)
		{
			for (; *s; s++)
				if (!IsSpace(*s))
					return false;
			return true;
		}

		void Copy(Element* to, const rxml_node* from)
		{
			to->name = from->name;
			if (from->data && !IsBlank(from->data))
				to->text = from->data;
			for (const rxml_attrib_node* a = from->attrib; a; a = a->next)
				to->attributes.emplace_back(a->attrib, a->value ? a->value : "");
			for (const rxml_node* c = from->children; c; c = c->next)
			{
				auto e = std::make_unique<Element>();
				e->parent = to;
				e->index = to->children.size();
				Copy(e.get(), c);
				to->children.push_back(std::move(e));
			}
		}

		void Write(rxml_writer_t* w, const Element* e)
		{
			rxml_write_open(w, e->name.c_str());
			for (auto& a : e->attributes)
				rxml_write_attrib(w, a.first.c_str(), a.second.c_str());
			if (!e->text.empty())
				rxml_write_text(w, e->text.c_str());
			for (auto& c : e->children)
				Write(w, c.get());
			rxml_write_close(w);
		}

		Element* NextNamed(Element* e, std::string_view name)
		{
			while (e && !name.empty() && e->name != name)
			{
				Element* p = e->parent;
				e = p && e->index + 1 < p->children.size() ? p->children[e->index + 1].get() : nullptr;
			}
			return e;
		}
	}

	const char* Attribute::name() const
	{
		return m_e ? m_e->attributes[m_i].first.c_str() : "";
	}

	const char* Attribute::value() const
	{
		return m_e ? m_e->attributes[m_i].second.c_str() : "";
	}

	int Attribute::as_int(int def) const { return m_e ? ParseInteger<int>(value(), def) : def; }
	unsigned Attribute::as_uint(unsigned def) const { return m_e ? ParseInteger<unsigned>(value(), def) : def; }
	long long Attribute::as_llong(long long def) const { return m_e ? ParseInteger<long long>(value(), def) : def; }
	unsigned long long Attribute::as_ullong(unsigned long long def) const { return m_e ? ParseInteger<unsigned long long>(value(), def) : def; }
	bool Attribute::as_bool(bool def) const { return m_e ? ParseBool(value(), def) : def; }

	bool Attribute::set_value(std::string_view v)
	{
		if (!m_e)
			return false;
		m_e->attributes[m_i].second.assign(v.data(), v.size());
		return true;
	}

	int Text::as_int(int def) const { return *this ? ParseInteger<int>(get(), def) : def; }
	unsigned Text::as_uint(unsigned def) const { return *this ? ParseInteger<unsigned>(get(), def) : def; }
	long long Text::as_llong(long long def) const { return *this ? ParseInteger<long long>(get(), def) : def; }
	unsigned long long Text::as_ullong(unsigned long long def) const { return *this ? ParseInteger<unsigned long long>(get(), def) : def; }
	bool Text::as_bool(bool def) const { return *this ? ParseBool(get(), def) : def; }
	float Text::as_float(float def) const { return *this ? strtof(get(), nullptr) : def; }
	double Text::as_double(double def) const { return *this ? strtod(get(), nullptr) : def; }

	bool Text::set(std::string_view v)
	{
		if (!m_e)
			return false;
		m_e->text.assign(v.data(), v.size());
		return true;
	}

	Node Node::parent() const
	{
		return Node(m_e ? m_e->parent : nullptr);
	}

	Node Node::child(std::string_view name) const
	{
		if (!m_e)
			return {};
		for (auto& c : m_e->children)
			if (c->name == name)
				return Node(c.get());
		return {};
	}

	Node Node::first_child() const
	{
		return Node(m_e && !m_e->children.empty() ? m_e->children.front().get() : nullptr);
	}

	Node Node::next_sibling() const
	{
		if (!m_e || !m_e->parent || m_e->index + 1 >= m_e->parent->children.size())
			return {};
		return Node(m_e->parent->children[m_e->index + 1].get());
	}

	Node Node::next_sibling(std::string_view name) const
	{
		return Node(NextNamed(next_sibling().m_e, name));
	}

	Node Node::find_descendant(std::string_view name) const
	{
		if (!m_e)
			return {};
		for (auto& c : m_e->children)
		{
			if (c->name == name)
				return Node(c.get());
			if (Node n = Node(c.get()).find_descendant(name))
				return n;
		}
		return {};
	}

	Node::Range Node::children() const
	{
		return Range(first_child().m_e, {});
	}

	Node::Range Node::children(std::string_view name) const
	{
		return Range(NextNamed(first_child().m_e, name), name);
	}

	Node::Range::iterator& Node::Range::iterator::operator++()
	{
		m_e = NextNamed(Node(m_e).next_sibling().element(), m_name);
		return *this;
	}

	Attribute Node::attribute(std::string_view name) const
	{
		if (!m_e)
			return {};
		for (size_t i = 0; i < m_e->attributes.size(); i++)
			if (m_e->attributes[i].first == name)
				return Attribute(m_e, i);
		return {};
	}

	Node Node::append_child(std::string_view name)
	{
		if (!m_e)
			return {};
		auto e = std::make_unique<Element>();
		e->name.assign(name.data(), name.size());
		e->parent = m_e;
		e->index = m_e->children.size();
		m_e->children.push_back(std::move(e));
		return Node(m_e->children.back().get());
	}

	Node Node::prepend_child(std::string_view name)
	{
		if (!m_e)
			return {};
		auto e = std::make_unique<Element>();
		e->name.assign(name.data(), name.size());
		e->parent = m_e;
		m_e->children.insert(m_e->children.begin(), std::move(e));
		for (size_t i = 0; i < m_e->children.size(); i++)
			m_e->children[i]->index = i;
		return Node(m_e->children.front().get());
	}

	Attribute Node::append_attribute(std::string_view name)
	{
		if (!m_e)
			return {};
		m_e->attributes.emplace_back(std::string(name), std::string());
		return Attribute(m_e, m_e->attributes.size() - 1);
	}

	Document::Document() : m_doc(std::make_unique<Element>())
	{
		m_e = m_doc.get();
	}

	void Document::reset()
	{
		m_doc->children.clear();
		m_doc->attributes.clear();
		m_doc->text.clear();
		m_errorOffset = 0;
	}

	bool Document::load(const void* data, size_t size)
	{
		reset();
		// rxml takes a NUL-terminated buffer it owns
		char* buf = (char*)malloc(size + 1);
		if (!buf)
			return false;
		if (size)
			memcpy(buf, data, size);
		buf[size] = '\0';
		rxml_parse_error_t err{};
		rxml_document_t* doc = rxml_load_document_owned_opts(buf, size, RXML_OPT_STRICT_EOF, &err);
		if (!doc)
		{
			m_errorOffset = err.offset;
			return false;
		}
		const rxml_node* root = rxml_root_node(doc);
		if (root)
		{
			auto e = std::make_unique<Element>();
			e->parent = m_doc.get();
			Copy(e.get(), root);
			m_doc->children.push_back(std::move(e));
		}
		rxml_free_document(doc);
		return root != nullptr;
	}

	std::string Document::save(const char* indent, bool declaration) const
	{
		Node root = document_element();
		if (!root)
			return {};
		rxml_writer_t* w = rxml_writer_new(indent);
		if (!w)
			return {};
		if (declaration)
			rxml_write_declaration(w);
		Write(w, root.element());
		size_t len = 0;
		const char* out = rxml_writer_data(w, &len);
		std::string s = out ? std::string(out, len) : std::string();
		rxml_writer_free(w);
		return s;
	}
}
