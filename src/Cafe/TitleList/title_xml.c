/* A title's meta.xml, meta.ini, app.xml and cos.xml (title_xml.h) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <formats/rxml.h>
#include <encodings/crc32.h>

#include "title_xml.h"

static const char *const languages[TITLE_LANGUAGES] = {
   "ja", "en", "fr", "de", "it", "es", "zhs", "ko", "nl", "pt", "ru", "zht"};
#define LANGUAGE_EN 1

static bool is_space(char c)
{
   return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static char *dup_n(const char *s, size_t n)
{
   char *c = (char*)malloc(n + 1);
   if (c)
   {
      memcpy(c, s, n);
      c[n] = '\0';
   }
   return c;
}

static char *dup_str(const char *s)
{
   return dup_n(s, strlen(s));
}

/* an element's text; "" for none, or one only of whitespace */
static const char *text_of(const struct rxml_node *n)
{
   const char *s;
   if (!n || !n->data)
      return "";
   for (s = n->data; *s; s++)
      if (!is_space(*s))
         return n->data;
   return "";
}

static const struct rxml_node *child_named(const struct rxml_node *n, const char *name)
{
   const struct rxml_node *c;
   for (c = n ? n->children : NULL; c; c = c->next)
      if (c->name && strcmp(c->name, name) == 0)
         return c;
   return NULL;
}

/* C's isspace, which stoull and ConvertString skip */
static bool is_c_space(char c)
{
   return is_space(c) || c == '\v' || c == '\f';
}

static int digit_of(char c, unsigned base)
{
   unsigned d;
   if (c >= '0' && c <= '9')
      d = (unsigned)(c - '0');
   else if ((c | 32) >= 'a' && (c | 32) <= 'f')
      d = (unsigned)((c | 32) - 'a' + 10);
   else
      return -1;
   return d < base ? (int)d : -1;
}

/* The digits of base at s, saturating at max (*over set then); false for
 * none */
static bool digits(const char *s, unsigned base, uint64_t max,
      uint64_t *out, bool *over)
{
   uint64_t v = 0;
   bool any = false;
   int d;
   *over = false;
   for (; (d = digit_of(*s, base)) >= 0; s++)
   {
      if (v > (max - (unsigned)d) / base)
      {
         *over = true;
         v = max;
      }
      else
         v = v * base + (unsigned)d;
      any = true;
   }
   *out = v;
   return any;
}

/* As std::stoull(text, nullptr, base): blanks, a sign, for base 16 a 0x,
 * then digits, a minus wrapping; 0 where stoull would throw (no digits,
 * more than 64 bits) or where it reads only the 0 of a 0x */
static uint64_t as_stoull(const char *text, unsigned base)
{
   const char *s = text;
   bool neg = false, over;
   uint64_t v;
   while (is_c_space(*s))
      s++;
   if (*s == '+' || *s == '-')
      neg = *s++ == '-';
   if (base == 16 && s[0] == '0' && (s[1] | 32) == 'x')
      s += 2;
   if (!digits(s, base, ~(uint64_t)0, &v, &over) || over)
      return 0;
   return neg ? (uint64_t)0 - v : v;
}

/* As the core's ConvertString(text, base) into a type whose top is max:
 * blanks, for base 16 a 0x (a -0x negating), then digits with no sign; 0
 * for none or too many */
static uint64_t as_convert(const char *text, unsigned base, uint64_t max)
{
   const char *s = text;
   bool neg = false, over;
   uint64_t v;
   while (is_c_space(*s))
      s++;
   if (base == 16)
   {
      neg = s[0] == '-' && s[1] == '0' && (s[2] | 32) == 'x';
      if (neg)
         s += 3;
      else if (s[0] == '0' && (s[1] | 32) == 'x')
         s += 2;
   }
   if (!digits(s, base, max, &v, &over) || over)
      return 0;
   return neg ? ((uint64_t)0 - v) & max : v;
}

/* As the core's XML reader's as_uint (pugixml's, but for a minus): def
 * for no text; blanks, a sign, a decimal or 0x hex number; 0 for no
 * digits, the top for too many, and a minus wrapping */
static uint32_t as_uint(const char *text, uint32_t def)
{
   const char *s = text;
   bool neg, over;
   unsigned base = 10;
   uint64_t v;
   if (!*text)
      return def;
   while (is_space(*s))
      s++;
   neg = *s == '-';
   if (*s == '+' || *s == '-')
      s++;
   if (s[0] == '0' && (s[1] | 32) == 'x')
   {
      s += 2;
      base = 16;
   }
   if (!digits(s, base, 0xFFFFFFFFUL, &v, &over))
      return 0;
   if (over)
      return 0xFFFFFFFFUL;
   return neg ? (uint32_t)(0 - (uint32_t)v) : (uint32_t)v;
}

/* the index of the language a name ends in, after prefix; -1 for none */
static int language_of(const char *name, const char *prefix)
{
   size_t n = strlen(prefix);
   int i;
   if (strncmp(name, prefix, n) != 0)
      return -1;
   for (i = 0; i < TITLE_LANGUAGES; i++)
      if (strcmp(name + n, languages[i]) == 0)
         return i;
   return -1;
}

static void set_str(char **slot, const char *s)
{
   free(*slot);
   *slot = dup_str(s);
}

static void why_not(char *why, size_t size, const char *fmt, const char *a, unsigned long b)
{
   char msg[256];
   if (!why || !size)
      return;
   if (a)
      sprintf(msg, fmt, a);
   else
      sprintf(msg, fmt, b);
   strncpy(why, msg, size - 1);
   why[size - 1] = '\0';
}

/* the document of len octets; its root, NULL with *err set if it does
 * not parse */
static rxml_document_t *load(const void *data, size_t len, size_t *err_offset)
{
   rxml_parse_error_t err;
   char *buf = (char*)malloc(len + 1);
   rxml_document_t *doc;
   if (!buf)
      return NULL;
   if (len)
      memcpy(buf, data, len);
   buf[len] = '\0';
   memset(&err, 0, sizeof(err));
   doc = rxml_load_document_owned_opts(buf, len, RXML_OPT_STRICT_EOF, &err);
   if (!doc && err_offset)
      *err_offset = err.offset;
   if (doc && !rxml_root_node(doc))
   {
      rxml_free_document(doc);
      return NULL;
   }
   return doc;
}

static bool copy_str(char **to, const char *from)
{
   *to = from ? dup_str(from) : NULL;
   return !from || *to;
}

bool title_meta_copy(title_meta_t *to, const title_meta_t *from)
{
   bool ok;
   int i;
   memset(to, 0, sizeof(*to));
   to->title_id = from->title_id;
   to->version = from->version;
   to->region = from->region;
   to->olv_accesskey = from->olv_accesskey;
   ok = copy_str(&to->product_code, from->product_code)
      & copy_str(&to->company_code, from->company_code)
      & copy_str(&to->content_platform, from->content_platform);
   for (i = 0; i < TITLE_LANGUAGES; i++)
      ok = ok & copy_str(&to->long_name[i], from->long_name[i])
         & copy_str(&to->short_name[i], from->short_name[i])
         & copy_str(&to->publisher[i], from->publisher[i]);
   if (!ok)
      title_meta_free(to);
   return ok;
}

void title_meta_free(title_meta_t *meta)
{
   int i;
   if (!meta)
      return;
   free(meta->product_code);
   free(meta->company_code);
   free(meta->content_platform);
   for (i = 0; i < TITLE_LANGUAGES; i++)
   {
      free(meta->long_name[i]);
      free(meta->short_name[i]);
      free(meta->publisher[i]);
   }
   memset(meta, 0, sizeof(*meta));
}

bool title_meta_parse(const void *xml, size_t len, title_meta_t *out,
      char *why, size_t why_size)
{
   size_t err_offset = 0;
   rxml_document_t *doc;
   const struct rxml_node *root, *c;
   memset(out, 0, sizeof(*out));
   if (!len)
   {
      why_not(why, why_size, "it is empty", NULL, 0);
      return false;
   }
   doc = load(xml, len, &err_offset);
   if (!doc)
   {
      char msg[128];
      sprintf(msg, "it is %lu bytes that do not parse as XML: not well-formed at offset %%lu",
            (unsigned long)len);
      why_not(why, why_size, msg, NULL, (unsigned long)err_offset);
      return false;
   }
   root = rxml_root_node(doc);
   if (!root->name || strcmp(root->name, "menu") != 0)
   {
      why_not(why, why_size, "its root element is <%.200s>, not <menu>", root->name ? root->name : "", 0);
      rxml_free_document(doc);
      return false;
   }
   for (c = root->children; c; c = c->next)
   {
      const char *name = c->name ? c->name : "";
      const char *text = text_of(c);
      int lang;
      if (!strcmp(name, "title_version"))
         out->version = as_uint(text, 0);
      else if (!strcmp(name, "product_code"))
         set_str(&out->product_code, text);
      else if (!strcmp(name, "company_code"))
         set_str(&out->company_code, text);
      else if (!strcmp(name, "content_platform"))
         set_str(&out->content_platform, text);
      else if (!strcmp(name, "title_id"))
         out->title_id = as_stoull(text, 16);
      else if (!strcmp(name, "region"))
         out->region = as_uint(text, 0);
      else if ((lang = language_of(name, "longname_")) >= 0)
      {
         char *s;
         set_str(&out->long_name[lang], text);
         for (s = out->long_name[lang]; s && *s; s++)
            if (*s == '\r' || *s == '\n')
               *s = ' ';
      }
      else if ((lang = language_of(name, "shortname_")) >= 0)
         set_str(&out->short_name[lang], text);
      else if ((lang = language_of(name, "publisher_")) >= 0)
         set_str(&out->publisher[lang], text);
      else if (!strncmp(name, "olv_accesskey", 13))
         out->olv_accesskey = as_uint(text, 0xFFFFFFFFUL);
   }
   rxml_free_document(doc);
   if (!out->title_id)
   {
      title_meta_free(out);
      why_not(why, why_size, "it has no title_id", NULL, 0);
      return false;
   }
   return true;
}

/* --- meta.ini ------------------------------------------------------------- */

typedef struct span
{
   const char *s;
   size_t n;
} span_t;

static void trim(span_t *v)
{
   while (v->n && (v->s[0] == ' ' || v->s[0] == '\t'))
   {
      v->s++;
      v->n--;
   }
   while (v->n && (v->s[v->n - 1] == ' ' || v->s[v->n - 1] == '\t'))
      v->n--;
}

static bool name_is(span_t v, const char *name)
{
   size_t i;
   if (v.n != strlen(name))
      return false;
   for (i = 0; i < v.n; i++)
   {
      char a = v.s[i], b = name[i];
      if (a >= 'A' && a <= 'Z')
         a = (char)(a - 'A' + 'a');
      if (a != b)
         return false;
   }
   return true;
}

/* As the core's ini reader: lines, # and ; comments outside quotes,
 * [sections] (the first named menu taken), name = value with the value
 * unquoted, names matched ignoring case, the first of a name taken. */
bool title_meta_parse_ini(const void *ini, size_t len, title_meta_t *out)
{
   static const char *const keys[3] = {"author", "longname", "shortname"};
   const char *p = (const char*)ini;
   const char *end = p + len;
   span_t found[3];
   bool have[3] = {false, false, false};
   int section = 0; /* 0 before [menu], 1 in it, 2 after it */
   size_t crc_len, at;
   char *crc_in;
   int k;
   memset(out, 0, sizeof(*out));
   if (!ini)
      return false;
   if (len >= 3 && (uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF)
      p += 3;
   while (p < end && section < 2)
   {
      span_t line, name, value;
      bool quote = false;
      size_t i;
      const char *eq;
      while (p < end && (*p == '\r' || *p == '\n'))
         p++;
      line.s = p;
      while (p < end && *p != '\r' && *p != '\n')
         p++;
      line.n = (size_t)(p - line.s);
      while (line.n && (line.s[0] == ' ' || line.s[0] == '\t'))
      {
         line.s++;
         line.n--;
      }
      for (i = 0; i < line.n; i++)
      {
         if (line.s[i] == '"')
            quote = !quote;
         if ((line.s[i] == '#' || line.s[i] == ';') && !quote)
         {
            line.n = i;
            break;
         }
      }
      if (!line.n)
         continue;
      if (line.s[0] == '[')
      {
         /* the name in the brackets; with no ], all of the line */
         span_t sec = line;
         for (i = 1; i < line.n; i++)
            if (line.s[i] == ']')
            {
               sec.s = line.s + 1;
               sec.n = i - 1;
               break;
            }
         if (section == 1)
            section = 2;
         else if (sec.n == 4 && memcmp(sec.s, "menu", 4) == 0)
            section = 1;
         continue;
      }
      if (section != 1)
         continue;
      eq = (const char*)memchr(line.s, '=', line.n);
      if (!eq)
         continue;
      name.s = line.s;
      name.n = (size_t)(eq - line.s);
      value.s = eq + 1;
      value.n = line.n - name.n - 1;
      trim(&name);
      trim(&value);
      if (!name.n)
         continue;
      if (value.n && value.s[0] == '"')
      {
         value.s++;
         value.n--;
         if (value.n >= 2 && value.s[value.n - 1] == '"')
            value.n--;
         else
            continue;
      }
      for (k = 0; k < 3; k++)
         if (!have[k] && name_is(name, keys[k]))
         {
            found[k] = value;
            have[k] = true;
         }
   }
   if (section == 0)
      return false;
   for (k = 0; k < 3; k++)
      if (!have[k])
      {
         found[k].s = "";
         found[k].n = 0;
      }
   out->publisher[LANGUAGE_EN] = dup_n(found[0].s, found[0].n);
   out->long_name[LANGUAGE_EN] = dup_n(found[1].s, found[1].n);
   out->short_name[LANGUAGE_EN] = dup_n(found[2].s, found[2].n);
   /* the title id: 0005000F, then the CRC-32 of the three */
   crc_len = found[0].n + found[1].n + found[2].n;
   crc_in = (char*)malloc(crc_len + 1);
   if (!crc_in || !out->publisher[LANGUAGE_EN] || !out->long_name[LANGUAGE_EN]
         || !out->short_name[LANGUAGE_EN])
   {
      free(crc_in);
      title_meta_free(out);
      return false;
   }
   at = 0;
   for (k = 0; k < 3; k++)
   {
      memcpy(crc_in + at, found[k].s, found[k].n);
      at += found[k].n;
   }
   out->title_id = ((uint64_t)0x0005000FUL << 32)
      | encoding_crc32(0, (const uint8_t*)crc_in, crc_len);
   free(crc_in);
   return true;
}

/* --- app.xml and cos.xml ---------------------------------------------------- */

bool title_app_parse(const void *xml, size_t len, title_app_t *out)
{
   rxml_document_t *doc = load(xml, len, NULL);
   const struct rxml_node *root, *c;
   memset(out, 0, sizeof(*out));
   if (!doc)
      return false;
   root = rxml_root_node(doc);
   if (!root->name || strcmp(root->name, "app") != 0)
   {
      rxml_free_document(doc);
      return false;
   }
   for (c = root->children; c; c = c->next)
   {
      const char *name = c->name ? c->name : "";
      const char *text = text_of(c);
      if (!strcmp(name, "title_version"))
         out->title_version = (uint16_t)as_stoull(text, 16);
      else if (!strcmp(name, "title_id"))
         out->title_id = as_stoull(text, 16);
      else if (!strcmp(name, "app_type"))
         out->app_type = (uint32_t)as_stoull(text, 16);
      else if (!strcmp(name, "group_id"))
         out->group_id = (uint32_t)as_stoull(text, 16);
      else if (!strcmp(name, "sdk_version"))
         out->sdk_version = (uint32_t)as_stoull(text, 10);
   }
   rxml_free_document(doc);
   return true;
}

static void cos_defaults(title_cos_t *out)
{
   int i;
   memset(out, 0, sizeof(*out));
   for (i = 0; i < TITLE_COS_PERMISSIONS; i++)
      out->permissions[i].mask = ~(uint64_t)0;
}

bool title_cos_default(title_cos_t *out, const char *argstr)
{
   cos_defaults(out);
   out->argstr = dup_str(argstr);
   return out->argstr != NULL;
}

bool title_cos_copy(title_cos_t *to, const title_cos_t *from)
{
   *to = *from;
   to->argstr = NULL;
   if (!copy_str(&to->argstr, from->argstr))
   {
      cos_defaults(to);
      return false;
   }
   return true;
}

void title_cos_free(title_cos_t *cos)
{
   if (!cos)
      return;
   free(cos->argstr);
   cos_defaults(cos);
}

bool title_cos_parse(const void *xml, size_t len, title_cos_t *out)
{
   rxml_document_t *doc = load(xml, len, NULL);
   const struct rxml_node *root, *argstr, *perms;
   int i;
   cos_defaults(out);
   if (!doc)
      return false;
   root = rxml_root_node(doc);
   if (!root->name || strcmp(root->name, "app") != 0)
   {
      rxml_free_document(doc);
      return false;
   }
   argstr = child_named(root, "argstr");
   if (argstr)
      out->argstr = dup_str(text_of(argstr));
   perms = child_named(root, "permissions");
   for (i = 0; i < TITLE_COS_PERMISSIONS; i++)
   {
      char pname[16];
      const struct rxml_node *p;
      sprintf(pname, "p%d", i);
      p = child_named(perms, pname);
      if (!p)
         break;
      out->permissions[i].group = (uint32_t)as_convert(text_of(child_named(p, "group")), 10, 0xFFFFFFFFUL);
      out->permissions[i].mask = as_convert(text_of(child_named(p, "mask")), 16, ~(uint64_t)0);
   }
   rxml_free_document(doc);
   return true;
}
