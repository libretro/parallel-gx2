#ifndef UTIL_XML_XML_TREE_H
#define UTIL_XML_XML_TREE_H

/* An XML document as a tree of elements to read, change and write back,
 * over libretro-common's rxml. C89.
 *
 * A document is an element with the name "" whose one child, once loaded,
 * is the root element. An element's text is NULL for none: text that is
 * only whitespace is none when loaded. A name of len octets need not be
 * terminated. */

#include <stddef.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

typedef struct xml_attr
{
   char *name;
   char *value;
   struct xml_attr *next;
} xml_attr_t;

typedef struct xml_elem
{
   char *name;
   char *text;
   xml_attr_t *attrs;   /* in order */
   struct xml_elem *parent;
   struct xml_elem *first, *last;   /* children */
   struct xml_elem *prev, *next;    /* siblings */
} xml_elem_t;

/* a new element with no parent; NULL for no memory */
xml_elem_t *xml_elem_new(const char *name, size_t len);
/* it and all under it, taken out of its parent first */
void xml_elem_free(xml_elem_t *e);
/* its children, attributes and text freed; its name kept */
void xml_elem_clear(xml_elem_t *e);

/* a new last (or first) child; NULL for no parent or no memory */
xml_elem_t *xml_elem_append(xml_elem_t *parent, const char *name, size_t len);
xml_elem_t *xml_elem_prepend(xml_elem_t *parent, const char *name, size_t len);
/* a new last attribute, its value ""; NULL for none or no memory */
xml_attr_t *xml_elem_add_attr(xml_elem_t *e, const char *name, size_t len);

/* the first child, attribute, or element anywhere under e (depth first)
 * of that name; NULL for none or no e */
xml_elem_t *xml_elem_child(const xml_elem_t *e, const char *name, size_t len);
xml_attr_t *xml_elem_attr(const xml_elem_t *e, const char *name, size_t len);
xml_elem_t *xml_elem_find(const xml_elem_t *e, const char *name, size_t len);
/* e, or the first sibling after it, of that name; any name for len 0 */
xml_elem_t *xml_elem_next_named(xml_elem_t *e, const char *name, size_t len);

/* *slot made a copy of len octets of s, the old one freed; false (and
 * *slot as it was) for no memory */
bool xml_set_str(char **slot, const char *s, size_t len);

/* doc cleared, then given the root of the size octets at data; false (and
 * *err_offset where parsing stopped, when it did) when they are not one
 * well-formed document, or for no memory */
bool xml_tree_load(xml_elem_t *doc, const void *data, size_t size,
      size_t *err_offset);
/* doc's root written out, with an XML declaration when asked, each element
 * on a line of its own indented by indent (NULL: all on one line); a
 * malloc'd string of *len octets, or NULL for no root or no memory */
char *xml_tree_save(const xml_elem_t *doc, const char *indent,
      bool declaration, size_t *len);

/* A number as pugixml reads one: leading whitespace, a sign, a 0x prefix
 * for hex; def for no text; 0 for no digits; out of range saturating */
long long xml_to_signed(const char *s, long long def, long long min,
      long long max);
/* as above; a minus wraps, as pugixml's does for unsigned */
unsigned long long xml_to_unsigned(const char *s, unsigned long long def,
      unsigned long long max);

RETRO_END_DECLS

#endif
