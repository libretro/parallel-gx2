/* An ini file parsed into one allocation (ini_file.h) */

#include <stdlib.h>
#include <string.h>

#include "ini_file.h"

typedef struct ini_alloc
{
   ini_file_t pub;
   /* then: sections, options, the file's octets */
} ini_alloc_t;

static int is_blank(char c)
{
   return c == ' ' || c == '\t';
}

static void trim(ini_span_t *v)
{
   while (v->len && is_blank(v->s[0]))
   {
      v->s++;
      v->len--;
   }
   while (v->len && is_blank(v->s[v->len - 1]))
      v->len--;
}

static int name_ok(ini_span_t name)
{
   size_t i;
   if (!name.len)
      return 0;
   for (i = 0; i < name.len; i++)
   {
      unsigned char c = (unsigned char)name.s[i];
      if (c == '[' || c == ']' || c < 32 || c > 128 || c == ' ')
         return 0;
   }
   return 1;
}

/* The lines of [p, end), blank runs skipped; each line's span in *line,
 * false at the end */
static int next_line(const char **p, const char *end, ini_span_t *line)
{
   const char *s = *p;
   while (s < end && (*s == '\r' || *s == '\n'))
      s++;
   if (s >= end)
   {
      *p = s;
      return 0;
   }
   line->s = s;
   while (s < end && *s != '\r' && *s != '\n')
      s++;
   line->len = (size_t)(s - line->s);
   *p = s;
   return 1;
}

ini_file_t *ini_file_parse(const void *data, size_t len)
{
   const char *in = (const char*)data;
   const char *p, *end;
   size_t lines = 0, at_line = 0;
   ini_span_t line;
   ini_alloc_t *a;
   ini_section_t *sections;
   ini_option_t *options;
   char *text;

   if (!in)
      len = 0;
   if (len >= 3 && (unsigned char)in[0] == 0xEF && (unsigned char)in[1] == 0xBB
         && (unsigned char)in[2] == 0xBF)
   {
      in += 3;
      len -= 3;
   }
   /* no more sections or options than lines */
   for (p = in, end = in + len; next_line(&p, end, &line); )
      lines++;
   a = (ini_alloc_t*)malloc(sizeof(*a) + lines * (sizeof(ini_section_t)
         + sizeof(ini_option_t)) + len);
   if (!a)
      return NULL;
   sections = (ini_section_t*)(a + 1);
   options = (ini_option_t*)(sections + lines);
   text = (char*)(options + lines);
   if (len)
      memcpy(text, in, len);
   a->pub.sections = sections;
   a->pub.options = options;
   a->pub.section_count = 0;
   a->pub.option_count = 0;

   for (p = text, end = text + len; next_line(&p, end, &line); )
   {
      ini_span_t name, value;
      int quote = 0;
      size_t i;
      const char *eq;
      at_line++;
      while (line.len && is_blank(line.s[0]))
      {
         line.s++;
         line.len--;
      }
      for (i = 0; i < line.len; i++)
      {
         if (line.s[i] == '"')
            quote = !quote;
         if ((line.s[i] == '#' || line.s[i] == ';') && !quote)
         {
            line.len = i;
            break;
         }
      }
      if (!line.len)
         continue;
      if (line.s[0] == '[')
      {
         ini_section_t *sec = &sections[a->pub.section_count++];
         sec->name = line;
         for (i = 1; i < line.len; i++)
            if (line.s[i] == ']')
            {
               sec->name.s = line.s + 1;
               sec->name.len = i - 1;
               break;
            }
         sec->line = at_line;
         sec->first = a->pub.option_count;
         sec->count = 0;
         continue;
      }
      eq = (const char*)memchr(line.s, '=', line.len);
      if (!eq)
         continue;
      name.s = line.s;
      name.len = (size_t)(eq - line.s);
      value.s = eq + 1;
      value.len = line.len - name.len - 1;
      trim(&name);
      trim(&value);
      if (!name_ok(name))
         continue;
      if (value.len && value.s[0] == '"')
      {
         value.s++;
         value.len--;
         if (value.len >= 2 && value.s[value.len - 1] == '"')
            value.len--;
         else
            continue;
      }
      if (!a->pub.section_count)
         continue;
      options[a->pub.option_count].name = name;
      options[a->pub.option_count].value = value;
      a->pub.option_count++;
      sections[a->pub.section_count - 1].count++;
   }
   return &a->pub;
}

void ini_file_free(ini_file_t *ini)
{
   free(ini);
}

static int lower(char c)
{
   return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

const ini_option_t *ini_file_find(const ini_file_t *ini, size_t section,
      const char *name, size_t name_len)
{
   const ini_section_t *sec;
   size_t i, j;
   if (!ini || section >= ini->section_count)
      return NULL;
   sec = &ini->sections[section];
   for (i = sec->first; i < sec->first + sec->count; i++)
   {
      const ini_option_t *o = &ini->options[i];
      if (o->name.len != name_len)
         continue;
      for (j = 0; j < name_len; j++)
         if (lower(o->name.s[j]) != lower(name[j]))
            break;
      if (j == name_len)
         return o;
   }
   return NULL;
}

size_t ini_file_section(const ini_file_t *ini, const char *name)
{
   size_t i, n = strlen(name);
   for (i = 0; i < ini->section_count; i++)
      if (ini->sections[i].name.len == n && memcmp(ini->sections[i].name.s, name, n) == 0)
         return i;
   return ini->section_count;
}
