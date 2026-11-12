/* The core's ini reader (src/util/IniParser/ini_file.c), as rules.txt,
 * game profiles and a WUHB's meta.ini are read with it: sections and their
 * lines, options trimmed and unquoted, comments outside quotes, lines it
 * drops, names found ignoring case, a byte order mark. C89, as the C89
 * lane builds it. */

#include <stdio.h>
#include <string.h>

#include "util/IniParser/ini_file.h"

static int failures = 0;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static char doc[4096];

static size_t join(const char *const *lines)
{
   size_t n = 0;
   for (; *lines; lines++)
   {
      size_t l = strlen(*lines);
      memcpy(doc + n, *lines, l);
      n += l;
   }
   return n;
}

static int is(ini_span_t v, const char *s)
{
   return v.len == strlen(s) && memcmp(v.s, s, v.len) == 0;
}

static const char *value_of(const ini_file_t *f, size_t sec, const char *name)
{
   static char buf[256];
   const ini_option_t *o = ini_file_find(f, sec, name, strlen(name));
   if (!o)
      return NULL;
   memcpy(buf, o->value.s, o->value.len);
   buf[o->value.len] = '\0';
   return buf;
}

static const char *const rules[] = {
   "\xEF\xBB\xBF# a graphic pack\n",
   "dropped = before any section\n",
   "[Definition]\n",
   "titleIds = 00050000101C9400,00050000101C9500\n",
   "name = \"Resolution\"\n",
   "path = \"BotW/Graphics/Resolution\" ; a comment\n",
   "\n\n   \n",
   "version = 7 # another\n",
   "Version = 8\n",
   "description = \"a;b#c\"\n",
   "  [Preset] \r\n",
   "name=\"720p\"\r\n",
   "$width:int = 1280\r",
   "empty =\n",
   "quoted = \"\"\n",
   "half = \"open\n",
   "na me = 1\n",
   "= 2\n",
   "a]b = 3\n",
   "a[b = 3\n",
   "Zip = z\n",
   "ctl\x01 = 4\n",
   "hi\x80 = 5\n",
   "hi\x81 = 6\n",
   "just words\n",
   "\t[Third#not a comment]\n",
   "[No end\n",
   "k=v\n",
   "[]\n",
   NULL};

static void test_rules(void)
{
   size_t len = join(rules);
   ini_file_t *f = ini_file_parse(doc, len);
   const ini_section_t *s;
   size_t i;
   CHECK(f != NULL);
   if (!f)
      return;
   /* the parse is a copy */
   memset(doc, 'x', len);
   CHECK(f->section_count == 5);
   s = f->sections;
   CHECK(is(s[0].name, "Definition") && s[0].line == 3);
   CHECK(is(s[1].name, "Preset") && s[1].line == 11);
   /* the comment goes first, and with it the ] */
   CHECK(is(s[2].name, "[Third"));
   CHECK(is(s[3].name, "[No end"));
   CHECK(is(s[4].name, ""));
   CHECK(ini_file_section(f, "Preset") == 1);
   CHECK(ini_file_section(f, "preset") == f->section_count);
   CHECK(ini_file_section(f, "[Third") == 2);
   CHECK(ini_file_section(f, "[No end") == 3);

   CHECK(s[0].first == 0 && s[0].count == 6);
   CHECK(is(f->options[0].name, "titleIds") && is(f->options[0].value, "00050000101C9400,00050000101C9500"));
   CHECK(strcmp(value_of(f, 0, "NAME"), "Resolution") == 0);
   CHECK(strcmp(value_of(f, 0, "path"), "BotW/Graphics/Resolution") == 0);
   /* the first of a name */
   CHECK(strcmp(value_of(f, 0, "version"), "7") == 0);
   CHECK(strcmp(value_of(f, 0, "description"), "a;b#c") == 0);
   CHECK(value_of(f, 0, "nam") == NULL && value_of(f, 0, "names") == NULL);
   /* only its own section's */
   CHECK(value_of(f, 0, "$width:int") == NULL);
   CHECK(value_of(f, 0, "dropped") == NULL);

   CHECK(s[1].first == 6);
   CHECK(strcmp(value_of(f, 1, "name"), "720p") == 0);
   CHECK(strcmp(value_of(f, 1, "$WIDTH:INT"), "1280") == 0);
   CHECK(strcmp(value_of(f, 1, "empty"), "") == 0);
   /* "" and an unclosed quote are dropped, as names that are not names */
   CHECK(value_of(f, 1, "quoted") == NULL);
   CHECK(value_of(f, 1, "half") == NULL);
   CHECK(value_of(f, 1, "na me") == NULL && value_of(f, 1, "") == NULL);
   CHECK(value_of(f, 1, "a]b") == NULL && value_of(f, 1, "ctl\x01") == NULL);
   CHECK(strcmp(value_of(f, 1, "hi\x80"), "5") == 0);
   CHECK(value_of(f, 1, "hi\x81") == NULL);
   CHECK(strcmp(value_of(f, 1, "zIP"), "z") == 0);
   CHECK(value_of(f, 1, "a[b") == NULL);
   CHECK(s[1].count == 5);
   CHECK(s[2].count == 0 && s[4].count == 0);
   CHECK(strcmp(value_of(f, 3, "K"), "v") == 0);
   CHECK(f->option_count == 12);
   for (i = 0; i + 1 < f->section_count; i++)
      CHECK(s[i].first + s[i].count == s[i + 1].first);
   CHECK(value_of(f, f->section_count, "k") == NULL);
   CHECK(ini_file_find(NULL, 0, "k", 1) == NULL);
   ini_file_free(f);
}

static void test_edges(void)
{
   ini_file_t *f;
   f = ini_file_parse("", 0);
   CHECK(f && f->section_count == 0 && f->option_count == 0);
   CHECK(ini_file_find(f, 0, "k", 1) == NULL);
   CHECK(f && ini_file_section(f, "menu") == 0);
   ini_file_free(f);
   f = ini_file_parse(NULL, 5);
   CHECK(f && f->section_count == 0);
   ini_file_free(f);
   ini_file_free(NULL);
   /* the length, not a terminator, ends it; a BOM only at the start */
   strcpy(doc, "[a]\nk=v1XYZ");
   f = ini_file_parse(doc, strlen(doc) - 3);
   CHECK(f && strcmp(value_of(f, 0, "k"), "v1") == 0);
   ini_file_free(f);
   strcpy(doc, "\n\xEF\xBB\xBF[a]\nk=v\n");
   f = ini_file_parse(doc, strlen(doc));
   CHECK(f && f->section_count == 0 && f->option_count == 0);
   ini_file_free(f);
   strcpy(doc, "\xEF\xBB\xBF[a]b]\nk=v\n");
   f = ini_file_parse(doc, strlen(doc));
   CHECK(f && f->section_count == 1 && is(f->sections[0].name, "a") && f->sections[0].count == 1);
   ini_file_free(f);
   strcpy(doc, "\xEF\xBB\xBF");
   f = ini_file_parse(doc, 3);
   CHECK(f && f->section_count == 0);
   ini_file_free(f);
   /* a quote opened in the name still hides a comment */
   strcpy(doc, "[a]\n\"k=v;w\"\nx=\"1\"#2\n");
   f = ini_file_parse(doc, strlen(doc));
   CHECK(f && strcmp(value_of(f, 0, "\"k"), "v;w\"") == 0);
   CHECK(f && strcmp(value_of(f, 0, "x"), "1") == 0);
   ini_file_free(f);
   /* \r alone ends a line too, and blank runs are not lines */
   strcpy(doc, "\r\r[a]\r\r\rk=1\r[b]\n");
   f = ini_file_parse(doc, strlen(doc));
   CHECK(f && f->section_count == 2 && f->sections[0].line == 1 && f->sections[1].line == 3);
   ini_file_free(f);
}

int main(void)
{
   test_rules();
   test_edges();
   if (failures)
   {
      fprintf(stderr, "ini_file_test: %d failed\n", failures);
      return 1;
   }
   printf("ini_file_test: all passed\n");
   return 0;
}
