/* An XML document as a tree of elements (xml_tree.h) */

#include <stdlib.h>
#include <string.h>

#include <formats/rxml.h>

#include "xml_tree.h"

static char *dup_n(const char *s, size_t len)
{
   char *c = (char*)malloc(len + 1);
   if (c)
   {
      if (len)
         memcpy(c, s, len);
      c[len] = '\0';
   }
   return c;
}

static bool named(const char *have, const char *name, size_t len)
{
   return strlen(have) == len && memcmp(have, name, len) == 0;
}

static bool is_space(char c)
{
   return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool xml_set_str(char **slot, const char *s, size_t len)
{
   char *c = dup_n(s, len);
   if (!c)
      return false;
   free(*slot);
   *slot = c;
   return true;
}

xml_elem_t *xml_elem_new(const char *name, size_t len)
{
   xml_elem_t *e = (xml_elem_t*)calloc(1, sizeof(*e));
   if (!e)
      return NULL;
   if (!(e->name = dup_n(name, len)))
   {
      free(e);
      return NULL;
   }
   return e;
}

static void free_attrs(xml_attr_t *a)
{
   while (a)
   {
      xml_attr_t *next = a->next;
      free(a->name);
      free(a->value);
      free(a);
      a = next;
   }
}

/* everything under e, a level at a time with no recursion: each child's
 * children are moved up into e before the child goes */
static void free_children(xml_elem_t *e)
{
   e->last = NULL;
   while (e->first)
   {
      xml_elem_t *c = e->first;
      if (c->first)
      {
         /* c's children go before c's next sibling */
         c->last->next = c->next;
         if (c->next)
            c->next->prev = c->last;
         c->next = c->first;
         c->first->prev = c;
         c->first = c->last = NULL;
      }
      e->first = c->next;
      if (e->first)
         e->first->prev = NULL;
      free(c->name);
      free(c->text);
      free_attrs(c->attrs);
      free(c);
   }
}

void xml_elem_clear(xml_elem_t *e)
{
   if (!e)
      return;
   free_children(e);
   free_attrs(e->attrs);
   e->attrs = NULL;
   free(e->text);
   e->text = NULL;
}

void xml_elem_free(xml_elem_t *e)
{
   if (!e)
      return;
   if (e->parent)
   {
      if (e->prev)
         e->prev->next = e->next;
      else
         e->parent->first = e->next;
      if (e->next)
         e->next->prev = e->prev;
      else
         e->parent->last = e->prev;
   }
   xml_elem_clear(e);
   free(e->name);
   free(e);
}

xml_elem_t *xml_elem_append(xml_elem_t *parent, const char *name, size_t len)
{
   xml_elem_t *e;
   if (!parent || !(e = xml_elem_new(name, len)))
      return NULL;
   e->parent = parent;
   e->prev = parent->last;
   if (parent->last)
      parent->last->next = e;
   else
      parent->first = e;
   parent->last = e;
   return e;
}

xml_elem_t *xml_elem_prepend(xml_elem_t *parent, const char *name, size_t len)
{
   xml_elem_t *e;
   if (!parent || !(e = xml_elem_new(name, len)))
      return NULL;
   e->parent = parent;
   e->next = parent->first;
   if (parent->first)
      parent->first->prev = e;
   else
      parent->last = e;
   parent->first = e;
   return e;
}

xml_attr_t *xml_elem_add_attr(xml_elem_t *e, const char *name, size_t len)
{
   xml_attr_t *a, **tail;
   if (!e || !(a = (xml_attr_t*)calloc(1, sizeof(*a))))
      return NULL;
   a->name = dup_n(name, len);
   a->value = dup_n("", 0);
   if (!a->name || !a->value)
   {
      free_attrs(a);
      return NULL;
   }
   for (tail = &e->attrs; *tail; tail = &(*tail)->next)
      ;
   *tail = a;
   return a;
}

xml_elem_t *xml_elem_child(const xml_elem_t *e, const char *name, size_t len)
{
   xml_elem_t *c;
   for (c = e ? e->first : NULL; c; c = c->next)
      if (named(c->name, name, len))
         return c;
   return NULL;
}

xml_attr_t *xml_elem_attr(const xml_elem_t *e, const char *name, size_t len)
{
   xml_attr_t *a;
   for (a = e ? e->attrs : NULL; a; a = a->next)
      if (named(a->name, name, len))
         return a;
   return NULL;
}

xml_elem_t *xml_elem_find(const xml_elem_t *e, const char *name, size_t len)
{
   xml_elem_t *c;
   for (c = e ? e->first : NULL; c; c = c->next)
   {
      xml_elem_t *f;
      if (named(c->name, name, len))
         return c;
      if ((f = xml_elem_find(c, name, len)))
         return f;
   }
   return NULL;
}

xml_elem_t *xml_elem_next_named(xml_elem_t *e, const char *name, size_t len)
{
   while (e && len && !named(e->name, name, len))
      e = e->next;
   return e;
}

/* from's attributes, non-blank text and children, into to; false for no
 * memory */
static bool copy_node(xml_elem_t *to, const struct rxml_node *from)
{
   const struct rxml_attrib_node *a;
   const struct rxml_node *c;
   if (from->data)
   {
      const char *s;
      for (s = from->data; *s && is_space(*s); s++)
         ;
      if (*s && !xml_set_str(&to->text, from->data, strlen(from->data)))
         return false;
   }
   for (a = from->attrib; a; a = a->next)
   {
      xml_attr_t *t = xml_elem_add_attr(to, a->attrib, strlen(a->attrib));
      if (!t || (a->value && !xml_set_str(&t->value, a->value, strlen(a->value))))
         return false;
   }
   for (c = from->children; c; c = c->next)
   {
      xml_elem_t *e = xml_elem_append(to, c->name, strlen(c->name));
      if (!e || !copy_node(e, c))
         return false;
   }
   return true;
}

bool xml_tree_load(xml_elem_t *doc, const void *data, size_t size,
      size_t *err_offset)
{
   rxml_parse_error_t err;
   rxml_document_t *rdoc;
   const struct rxml_node *root;
   xml_elem_t *e;
   char *buf;
   bool ok;
   xml_elem_clear(doc);
   /* rxml takes a terminated buffer it owns */
   if (!(buf = dup_n((const char*)data, size)))
      return false;
   memset(&err, 0, sizeof(err));
   rdoc = rxml_load_document_owned_opts(buf, size, RXML_OPT_STRICT_EOF, &err);
   if (!rdoc)
   {
      if (err_offset)
         *err_offset = err.offset;
      return false;
   }
   root = rxml_root_node(rdoc);
   ok = root != NULL;
   if (ok)
   {
      e = xml_elem_append(doc, root->name, strlen(root->name));
      ok = e && copy_node(e, root);
      if (!ok)
         xml_elem_clear(doc);
   }
   rxml_free_document(rdoc);
   return ok;
}

static void write_elem(rxml_writer_t *w, const xml_elem_t *e)
{
   const xml_attr_t *a;
   const xml_elem_t *c;
   rxml_write_open(w, e->name);
   for (a = e->attrs; a; a = a->next)
      rxml_write_attrib(w, a->name, a->value);
   if (e->text && *e->text)
      rxml_write_text(w, e->text);
   for (c = e->first; c; c = c->next)
      write_elem(w, c);
   rxml_write_close(w);
}

char *xml_tree_save(const xml_elem_t *doc, const char *indent,
      bool declaration, size_t *len)
{
   rxml_writer_t *w;
   const char *data;
   char *out = NULL;
   size_t n = 0;
   if (!doc || !doc->first || !(w = rxml_writer_new(indent)))
      return NULL;
   if (declaration)
      rxml_write_declaration(w);
   write_elem(w, doc->first);
   data = rxml_writer_data(w, &n);
   if (data && !rxml_writer_failed(w))
      out = dup_n(data, n);
   rxml_writer_free(w);
   if (out && len)
      *len = n;
   return out;
}

/* the digits after blanks, a sign and a 0x; false for none */
static bool parse_digits(const char *s, unsigned long long *v, bool *negative,
      bool *overflow)
{
   unsigned base = 10;
   bool any = false;
   *v = 0;
   *negative = false;
   *overflow = false;
   while (is_space(*s))
      s++;
   if (*s == '-' || *s == '+')
      *negative = *s++ == '-';
   if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
   {
      base = 16;
      s += 2;
   }
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
      if (*v > (~0ULL - d) / base)
         *overflow = true;
      else
         *v = *v * base + d;
   }
   return any;
}

long long xml_to_signed(const char *s, long long def, long long min,
      long long max)
{
   unsigned long long v;
   bool negative, overflow;
   if (!s)
      return def;
   if (!parse_digits(s, &v, &negative, &overflow))
      return 0;
   if (negative)
   {
      if (overflow || v > (unsigned long long)max + 1)
         return min;
      return (long long)(0ULL - v);
   }
   if (overflow || v > (unsigned long long)max)
      return max;
   return (long long)v;
}

unsigned long long xml_to_unsigned(const char *s, unsigned long long def,
      unsigned long long max)
{
   unsigned long long v;
   bool negative, overflow;
   if (!s)
      return def;
   if (!parse_digits(s, &v, &negative, &overflow))
      return 0;
   if (overflow || v > max)
      return max;
   return negative ? (0ULL - v) & max : v;
}
