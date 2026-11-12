/* The core's title files parsed (src/Cafe/TitleList/title_xml.c): a
 * meta.xml right, cut short, rooted wrong, with no title id, and with its
 * numbers written each way the old reader took them; a WUHB's meta.ini
 * with comments, quotes, sections around [menu] and keys missing, its
 * title id the CRC-32 the old reader made; an app.xml's hex and decimal
 * fields as stoull read them; a cos.xml's argstr and permissions as
 * ConvertString read them; the title list's cache file read and written
 * as the C++ before did; and the CRC-32 the RPL loader takes. C89, as the
 * C89 lane builds it. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <encodings/crc32.h>

#include "Cafe/TitleList/title_xml.h"

static int failures = 0;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); failures++; } } while (0)

#define EN 1
#define JA 0
#define ZHT 11

static char doc[8192];

/* the lines, joined, into doc; its length */
static size_t join(const char *const *lines)
{
   size_t n = 0;
   doc[0] = '\0';
   for (; *lines; lines++)
   {
      size_t l = strlen(*lines);
      memcpy(doc + n, *lines, l + 1);
      n += l;
   }
   return n;
}

static bool same(const char *a, const char *b)
{
   return a && b && strcmp(a, b) == 0;
}

static const char *const meta_lines[] = {
   "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n",
   "<menu type=\"complex\" access=\"777\">\n",
   "  <version type=\"unsignedInt\" length=\"4\">33</version>\n",
   "  <product_code type=\"string\" length=\"32\">WUP-N-ALZP</product_code>\n",
   "  <company_code type=\"string\" length=\"8\">0001</company_code>\n",
   "  <content_platform type=\"string\" length=\"32\">WUP</content_platform>\n",
   "  <title_id type=\"hexBinary\" length=\"8\">00050000101C9400</title_id>\n",
   "  <title_version type=\"unsignedInt\" length=\"4\">208</title_version>\n",
   "  <region type=\"hexBinary\" length=\"4\">00000002</region>\n",
   "  <olv_accesskey type=\"unsignedInt\" length=\"4\">2194993150</olv_accesskey>\n",
   "  <longname_en type=\"string\" length=\"512\">The Legend of Zelda\r\nBreath of the Wild</longname_en>\n",
   "  <shortname_en type=\"string\" length=\"256\">Zelda &amp; Link</shortname_en>\n",
   "  <shortname_ja type=\"string\" length=\"256\">   </shortname_ja>\n",
   "  <shortname_zht type=\"string\" length=\"256\">Z</shortname_zht>\n",
   "  <shortnameXzht type=\"string\" length=\"256\">Q</shortnameXzht>\n",
   "  <publisher_en type=\"string\" length=\"256\">Nintendo</publisher_en>\n",
   "  <publisher_xx type=\"string\" length=\"256\">X</publisher_xx>\n",
   "  <shortname_ type=\"string\" length=\"256\">Y</shortname_>\n",
   "</menu>\n",
   NULL};

static void test_meta(void)
{
   title_meta_t m, copy;
   char why[256];
   size_t len = join(meta_lines);
   int i, set;
   strcpy(why, "untouched");
   CHECK(title_meta_parse(doc, len, &m, why, sizeof(why)));
   CHECK(strcmp(why, "untouched") == 0);
   CHECK(m.title_id == ((uint64_t)0x00050000UL << 32 | 0x101C9400UL));
   CHECK(m.version == 208);
   CHECK(m.region == 2);
   CHECK(m.olv_accesskey == 2194993150UL);
   CHECK(same(m.product_code, "WUP-N-ALZP"));
   CHECK(same(m.company_code, "0001"));
   CHECK(same(m.content_platform, "WUP"));
   /* each line end a space (XML makes \r\n one) */
   CHECK(same(m.long_name[EN], "The Legend of Zelda Breath of the Wild"));
   CHECK(same(m.short_name[EN], "Zelda & Link"));
   CHECK(same(m.short_name[ZHT], "Z"));
   CHECK(same(m.publisher[EN], "Nintendo"));
   /* whitespace alone is no text */
   CHECK(same(m.short_name[JA], ""));
   CHECK(m.long_name[JA] == NULL);
   /* no language, or one not known, sets nothing */
   for (set = 0, i = 0; i < TITLE_LANGUAGES; i++)
      set += m.publisher[i] != NULL;
   CHECK(set == 1);
   for (set = 0, i = 0; i < TITLE_LANGUAGES; i++)
      set += m.short_name[i] != NULL;
   CHECK(set == 3);

   /* a copy owns its own strings */
   CHECK(title_meta_copy(&copy, &m));
   CHECK(copy.title_id == m.title_id && copy.olv_accesskey == m.olv_accesskey);
   CHECK(copy.long_name[EN] != m.long_name[EN]);
   CHECK(copy.product_code != m.product_code);
   title_meta_free(&m);
   CHECK(m.long_name[EN] == NULL && m.title_id == 0);
   CHECK(same(copy.long_name[EN], "The Legend of Zelda Breath of the Wild"));
   CHECK(same(copy.short_name[ZHT], "Z") && same(copy.product_code, "WUP-N-ALZP"));
   CHECK(copy.long_name[JA] == NULL);
   title_meta_free(&copy);
   title_meta_free(&copy);

   /* cut short: where it stops parsing */
   CHECK(!title_meta_parse(doc, len / 2, &m, why, sizeof(why)));
   CHECK(strncmp(why, "it is ", 6) == 0);
   CHECK(strstr(why, " bytes that do not parse as XML: not well-formed at offset ") != NULL);
   CHECK(m.title_id == 0 && m.product_code == NULL);
   {
      char want[64];
      sprintf(want, "it is %lu bytes", (unsigned long)(len / 2));
      CHECK(strncmp(why, want, strlen(want)) == 0);
   }
   /* no room for why, or a little */
   CHECK(!title_meta_parse(doc, len / 2, &m, NULL, 0));
   CHECK(!title_meta_parse("", 0, &m, why, 8));
   CHECK(strcmp(why, "it is e") == 0);
   CHECK(!title_meta_parse("", 0, &m, why, sizeof(why)));
   CHECK(strcmp(why, "it is empty") == 0);
   CHECK(!title_meta_parse("<item><title_id>1</title_id></item>", 35, &m, why, sizeof(why)));
   CHECK(strcmp(why, "its root element is <item>, not <menu>") == 0);
   CHECK(!title_meta_parse("<menu><region>1</region></menu>", 31, &m, why, sizeof(why)));
   CHECK(strcmp(why, "it has no title_id") == 0);
   CHECK(m.region == 0);
   CHECK(!title_meta_parse("<menu><title_id>zz</title_id></menu>", 36, &m, why, sizeof(why)));
   CHECK(strcmp(why, "it has no title_id") == 0);
   CHECK(!title_meta_parse("<menu><title_id>1</title_id></menu><x/>", 39, &m, why, sizeof(why)));
   CHECK(strstr(why, "do not parse as XML") != NULL);
}

/* a meta.xml with a title id and one more element, of text */
static title_meta_t meta_with(const char *element, const char *text)
{
   title_meta_t m;
   sprintf(doc, "<menu><title_id>1</title_id><%s>%s</%s></menu>", element, text, element);
   if (!title_meta_parse(doc, strlen(doc), &m, NULL, 0))
      memset(&m, 0xAB, sizeof(m));
   return m;
}

static void test_meta_numbers(void)
{
   title_meta_t m;
   /* title_version, region and olv_accesskey: a decimal or 0x hex number,
    * after blanks and a sign */
   m = meta_with("title_version", "0x10"); CHECK(m.version == 16);
   m = meta_with("title_version", "0X1f"); CHECK(m.version == 31);
   m = meta_with("title_version", " +7"); CHECK(m.version == 7);
   m = meta_with("title_version", "12abc"); CHECK(m.version == 12);
   m = meta_with("title_version", "abc"); CHECK(m.version == 0);
   m = meta_with("title_version", "-1"); CHECK(m.version == 0xFFFFFFFFUL);
   m = meta_with("title_version", "-0x2"); CHECK(m.version == 0xFFFFFFFEUL);
   m = meta_with("title_version", "4294967295"); CHECK(m.version == 0xFFFFFFFFUL);
   m = meta_with("title_version", "4294967296"); CHECK(m.version == 0xFFFFFFFFUL);
   m = meta_with("title_version", "-99999999999999999999999"); CHECK(m.version == 0xFFFFFFFFUL);
   m = meta_with("title_version", "0x123456789"); CHECK(m.version == 0xFFFFFFFFUL);
   m = meta_with("title_version", "00000000000000000000000000042"); CHECK(m.version == 42);
   m = meta_with("region", "\v5"); CHECK(m.region == 0);
   /* olv_accesskey with no text is all ones; with one, as the others */
   m = meta_with("olv_accesskey", ""); CHECK(m.olv_accesskey == 0xFFFFFFFFUL);
   m = meta_with("olv_accesskey", " \n "); CHECK(m.olv_accesskey == 0xFFFFFFFFUL);
   m = meta_with("olv_accesskey", "x"); CHECK(m.olv_accesskey == 0);
   m = meta_with("olv_accesskey_extra", "3"); CHECK(m.olv_accesskey == 3);
   m = meta_with("region", ""); CHECK(m.region == 0);
   /* the title id: hex as stoull read it */
   sprintf(doc, "<menu><title_id> 0x00050000101c9400</title_id></menu>");
   CHECK(title_meta_parse(doc, strlen(doc), &m, NULL, 0));
   CHECK(m.title_id == ((uint64_t)0x00050000UL << 32 | 0x101C9400UL));
   title_meta_free(&m);
   sprintf(doc, "<menu><title_id>10000000000000000</title_id></menu>");
   CHECK(!title_meta_parse(doc, strlen(doc), &m, NULL, 0));
}

static void test_ini(void)
{
   static const char *const lines[] = {
      "\xEF\xBB\xBF# a WUHB\n",
      "[other]\n",
      "longname=Nope\n",
      "[menu]\n",
      "  Author = \"Some ; One\"  ; comment\n",
      "LONGNAME=Long Name # c\n",
      "shortname = \"Short\n",
      "shortname=Second\n",
      "longname=dup\n",
      "[after]\n",
      "author=after\n",
      NULL};
   title_meta_t m;
   size_t len = join(lines);
   CHECK(title_meta_parse_ini(doc, len, &m));
   CHECK(same(m.publisher[EN], "Some ; One"));
   CHECK(same(m.long_name[EN], "Long Name"));
   CHECK(same(m.short_name[EN], "Second"));
   /* 0005000F and the CRC-32 of author, longname and shortname */
   CHECK(m.title_id == ((uint64_t)0x0005000FUL << 32 | 0xF7B7BC03UL));
   CHECK(m.long_name[JA] == NULL && m.product_code == NULL);
   title_meta_free(&m);

   /* keys missing count as empty */
   strcpy(doc, "[menu]\r\nlongname=Long Name\r\n");
   CHECK(title_meta_parse_ini(doc, strlen(doc), &m));
   CHECK(same(m.long_name[EN], "Long Name"));
   CHECK(same(m.publisher[EN], "") && same(m.short_name[EN], ""));
   CHECK(m.title_id == ((uint64_t)0x0005000FUL << 32 | 0x07887FD0UL));
   title_meta_free(&m);
   strcpy(doc, "[menu]");
   CHECK(title_meta_parse_ini(doc, strlen(doc), &m));
   CHECK(m.title_id == (uint64_t)0x0005000FUL << 32);
   title_meta_free(&m);
   /* the length, not a terminator, ends it */
   strcpy(doc, "[menu]\nauthor=Nintendo\nlongname=Mario Kart 8\nshortname=Mario Kart 8XYZ");
   CHECK(title_meta_parse_ini(doc, strlen(doc) - 3, &m));
   CHECK(same(m.short_name[EN], "Mario Kart 8"));
   CHECK(m.title_id == ((uint64_t)0x0005000FUL << 32 | 0x316CA22CUL));
   title_meta_free(&m);

   /* only [menu]'s keys, and a BOM let be */
   strcpy(doc, "\xEF\xBB\xBF[menu]\nauthor=A\n[x]\nshortname=S\n");
   CHECK(title_meta_parse_ini(doc, strlen(doc), &m));
   CHECK(same(m.publisher[EN], "A") && same(m.short_name[EN], ""));
   title_meta_free(&m);

   /* no [menu] */
   strcpy(doc, "[menuX]\nauthor=x\n");
   CHECK(!title_meta_parse_ini(doc, strlen(doc), &m));
   strcpy(doc, "[other]\nauthor=x\n");
   CHECK(!title_meta_parse_ini(doc, strlen(doc), &m));
   CHECK(m.title_id == 0 && m.publisher[EN] == NULL);
   strcpy(doc, "[menu\nauthor=x\n");
   CHECK(!title_meta_parse_ini(doc, strlen(doc), &m));
   strcpy(doc, "[Menu]\nauthor=x\n");
   CHECK(!title_meta_parse_ini(doc, strlen(doc), &m));
   strcpy(doc, "; [menu]\nauthor=x\n");
   CHECK(!title_meta_parse_ini(doc, strlen(doc), &m));
   CHECK(!title_meta_parse_ini("", 0, &m));
   CHECK(!title_meta_parse_ini(NULL, 0, &m));
}

static const char *const app_lines[] = {
   "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n",
   "<app type=\"complex\" access=\"777\">\n",
   "  <version type=\"unsignedInt\" length=\"4\">15</version>\n",
   "  <os_version type=\"hexBinary\" length=\"8\">000500101000400A</os_version>\n",
   "  <title_id type=\"hexBinary\" length=\"8\">00050000101C9400</title_id>\n",
   "  <title_version type=\"hexBinary\" length=\"2\">00D0</title_version>\n",
   "  <sdk_version type=\"unsignedInt\" length=\"4\">21204</sdk_version>\n",
   "  <app_type type=\"hexBinary\" length=\"4\">80000000</app_type>\n",
   "  <group_id type=\"hexBinary\" length=\"4\">000016A4</group_id>\n",
   "</app>\n",
   NULL};

static title_app_t app_with(const char *element, const char *text)
{
   title_app_t a;
   sprintf(doc, "<app><%s>%s</%s></app>", element, text, element);
   if (!title_app_parse(doc, strlen(doc), &a))
      memset(&a, 0xAB, sizeof(a));
   return a;
}

static void test_app(void)
{
   title_app_t a;
   size_t len = join(app_lines);
   CHECK(title_app_parse(doc, len, &a));
   CHECK(a.title_id == ((uint64_t)0x00050000UL << 32 | 0x101C9400UL));
   CHECK(a.title_version == 0xD0);
   CHECK(a.sdk_version == 21204);
   CHECK(a.app_type == 0x80000000UL);
   CHECK(a.group_id == 0x16A4);
   CHECK(!title_app_parse(doc, len - 10, &a));
   CHECK(a.title_id == 0);
   CHECK(!title_app_parse("<menu/>", 7, &a));
   CHECK(!title_app_parse("", 0, &a));

   /* as stoull: blanks (\v too), a sign, a 0x before a digit */
   a = app_with("title_version", "0x1F"); CHECK(a.title_version == 0x1F);
   a = app_with("title_version", "0xZ"); CHECK(a.title_version == 0);
   a = app_with("title_version", "\v\f+12"); CHECK(a.title_version == 0x12);
   a = app_with("title_id", "-1"); CHECK(a.title_id == ~(uint64_t)0);
   a = app_with("title_id", "-0x2"); CHECK(a.title_id == ~(uint64_t)1);
   a = app_with("title_id", "FFFFFFFFFFFFFFFF"); CHECK(a.title_id == ~(uint64_t)0);
   a = app_with("title_id", "10000000000000000"); CHECK(a.title_id == 0);
   a = app_with("title_id", "abcdefg"); CHECK(a.title_id == 0xABCDEFUL);
   a = app_with("app_type", "123456789"); CHECK(a.app_type == 0x23456789UL);
   a = app_with("group_id", ""); CHECK(a.group_id == 0);
   a = app_with("sdk_version", "1A"); CHECK(a.sdk_version == 1);
   a = app_with("sdk_version", "0x10"); CHECK(a.sdk_version == 0);
   a = app_with("sdk_version", "\v12"); CHECK(a.sdk_version == 12);
   a = app_with("sdk_version", "18446744073709551615"); CHECK(a.sdk_version == 0xFFFFFFFFUL);
   a = app_with("sdk_version", "18446744073709551616"); CHECK(a.sdk_version == 0);
}

static void test_cos(void)
{
   static const char *const lines[] = {
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n",
      "<app type=\"complex\" access=\"777\">\n",
      "  <argstr type=\"string\" length=\"4096\">U-King.rpx</argstr>\n",
      "  <permissions type=\"complex\">\n",
      "    <p0><group>1</group><mask>FFFFFFFFFFFFFFFF</mask></p0>\n",
      "    <p1><group> 9</group><mask>0x3</mask></p1>\n",
      "    <p2><group>4294967297</group><mask>-0x1</mask></p2>\n",
      "    <p3><group>+5</group><mask>-1</mask></p3>\n",
      "    <p4><group>12</group><mask>10000000000000000</mask></p4>\n",
      "    <p5><group>4294967295</group><mask>f0zz</mask></p5>\n",
      "    <p7><group>13</group><mask>1</mask></p7>\n",
      "  </permissions>\n",
      "</app>\n",
      NULL};
   title_cos_t c, copy;
   size_t len = join(lines);
   int i;
   CHECK(title_cos_parse(doc, len, &c));
   CHECK(same(c.argstr, "U-King.rpx"));
   CHECK(c.permissions[0].group == 1 && c.permissions[0].mask == ~(uint64_t)0);
   CHECK(c.permissions[1].group == 9 && c.permissions[1].mask == 3);
   /* too big for its type, and a -0x negating */
   CHECK(c.permissions[2].group == 0 && c.permissions[2].mask == ~(uint64_t)0);
   /* no sign but -0x */
   CHECK(c.permissions[3].group == 0 && c.permissions[3].mask == 0);
   CHECK(c.permissions[4].group == 12 && c.permissions[4].mask == 0);
   CHECK(c.permissions[5].group == 0xFFFFFFFFUL && c.permissions[5].mask == 0xF0);
   /* p6 missing: it and those after it left as they were */
   for (i = 6; i < TITLE_COS_PERMISSIONS; i++)
      CHECK(c.permissions[i].group == 0 && c.permissions[i].mask == ~(uint64_t)0);

   CHECK(title_cos_copy(&copy, &c));
   CHECK(copy.argstr != c.argstr);
   title_cos_free(&c);
   CHECK(c.argstr == NULL && c.permissions[1].mask == ~(uint64_t)0 && c.permissions[1].group == 0);
   CHECK(same(copy.argstr, "U-King.rpx") && copy.permissions[1].group == 9 && copy.permissions[5].mask == 0xF0);
   title_cos_free(&copy);

   /* no argstr, no permissions */
   strcpy(doc, "<app><x/></app>");
   CHECK(title_cos_parse(doc, strlen(doc), &c));
   CHECK(c.argstr == NULL);
   for (i = 0; i < TITLE_COS_PERMISSIONS; i++)
      CHECK(c.permissions[i].group == 0 && c.permissions[i].mask == ~(uint64_t)0);
   title_cos_free(&c);
   /* an empty argstr is one */
   strcpy(doc, "<app><argstr></argstr></app>");
   CHECK(title_cos_parse(doc, strlen(doc), &c));
   CHECK(same(c.argstr, ""));
   title_cos_free(&c);
   CHECK(!title_cos_parse("<menu/>", 7, &c));
   CHECK(c.argstr == NULL && c.permissions[0].mask == ~(uint64_t)0);
   CHECK(!title_cos_parse(doc, 10, &c));

   /* a WUHB's */
   CHECK(title_cos_default(&c, "root.rpx"));
   CHECK(same(c.argstr, "root.rpx"));
   for (i = 0; i < TITLE_COS_PERMISSIONS; i++)
      CHECK(c.permissions[i].group == 0 && c.permissions[i].mask == ~(uint64_t)0);
   title_cos_free(&c);
}

/* The CRC-32 the RPL loader checks sections with and chains over a
 * module's patch description (rpl.cpp): the value the core's own CRC-32
 * self-test wanted, and the same chained in pieces of each length its
 * table-at-a-time path takes */
static void test_crc(void)
{
   uint8_t data[89];
   uint32_t crc;
   int i;
   for (i = 0; i < 89; i++)
      data[i] = (uint8_t)i;
   CHECK(encoding_crc32(0, data, sizeof(data)) == 0x3FC61683UL);
   crc = encoding_crc32(0, data, 5);
   crc = encoding_crc32(crc, data + 5, 17);
   crc = encoding_crc32(crc, data + 22, 0);
   crc = encoding_crc32(crc, data + 22, 67);
   CHECK(crc == 0x3FC61683UL);
}

static void test_title_list(void)
{
   static const char *const lines[] = {
      "<?xml version=\"1.0\"?>\n<title_list>\n",
      " <title titleId=\"0005000010101C00\" version=\"70000\" sdk_version=\"21204\" group_id=\"0x1A\" app_type=\"\">\n",
      "  <region>-0x1</region><name>Zelda &amp; Link</name><format>0x3</format>\n",
      "  <path>/games/a.wua</path><sub_path>0005000010101c00_v0</sub_path>\n",
      " </title>\n",
      " <title titleId=\"0005000010101C0\"/>\n",
      " <title titleId=\"000500001010zC00\"/>\n",
      " <title/>\n",
      " <other titleId=\"00050000101d7500extra\" version=\"\" sdk_version=\"x\" group_id=\"zz\">\n",
      "  <region> 12</region><name>   </name><format>-5</format>\n",
      " </other>\n",
      "</title_list>\n",
      NULL};
   title_list_entry_t *e = NULL, *back = NULL;
   title_list_entry_t made[2];
   size_t n = 0, nb = 0, len = 0;
   size_t doclen = join(lines);
   char *saved;
   CHECK(title_list_parse(doc, doclen, &e, &n));
   CHECK(n == 2);
   if (n == 2)
   {
      CHECK(e[0].title_id == ((uint64_t)0x00050000UL << 32 | 0x10101C00UL));
      CHECK(e[0].version == 70000 && e[0].sdk_version == 21204);
      CHECK(e[0].group_id == 0x1A && e[0].app_type == 0);
      CHECK(e[0].region == 0xFFFFFFFFUL && e[0].format == 3);
      CHECK(same(e[0].name, "Zelda & Link") && same(e[0].path, "/games/a.wua"));
      CHECK(same(e[0].sub_path, "0005000010101c00_v0"));
      /* any element; sixteen hex digits first; what is missing, 0 or "" */
      CHECK(e[1].title_id == ((uint64_t)0x00050000UL << 32 | 0x101D7500UL));
      CHECK(e[1].version == 0 && e[1].sdk_version == 0 && e[1].group_id == 0);
      CHECK(e[1].region == 12 && e[1].format == 0);
      CHECK(same(e[1].name, "") && same(e[1].path, "") && same(e[1].sub_path, ""));
   }
   title_list_free(e, n);
   /* another root: no titles; not XML: no list */
   strcpy(doc, "<menu><title titleId=\"0005000010101C00\"/></menu>");
   CHECK(title_list_parse(doc, strlen(doc), &e, &n) && n == 0 && e == NULL);
   doclen = join(lines);
   CHECK(!title_list_parse(doc, doclen / 2, &e, &n) && n == 0);
   CHECK(!title_list_parse("", 0, &e, &n));

   /* written as the C++ wrote it, and read back */
   memset(made, 0, sizeof(made));
   made[0].title_id = ((uint64_t)0x00050000UL << 32) | 0x10101C00UL;
   made[0].version = 0x10020; /* a uint16 in the file */
   made[0].sdk_version = 4000000000UL;
   made[0].format = 3;
   made[0].region = 2;
   made[0].group_id = 0x1C9;
   made[0].app_type = 0x80000000UL;
   made[0].name = (char*)"A & <B>";
   made[0].path = (char*)"/x/a.wua";
   made[0].sub_path = (char*)"0005000010101c00_v32";
   made[1].title_id = 0xABC;
   made[1].name = (char*)"";
   made[1].path = (char*)"/y";
   made[1].sub_path = (char*)"";
   saved = title_list_save(made, 2, &len);
   CHECK(saved && len == strlen(saved));
   if (saved)
   {
      CHECK(strncmp(saved, "<?xml", 5) == 0);
      CHECK(strstr(saved, "<title titleId=\"0005000010101c00\" version=\"32\" sdk_version=\"4000000000\" group_id=\"000001c9\" app_type=\"80000000\">") != NULL);
      CHECK(strstr(saved, "<name>A &amp; &lt;B&gt;</name>") != NULL);
      CHECK(strstr(saved, "<region>2</region>") && strstr(saved, "<format>3</format>"));
      CHECK(strstr(saved, "<title titleId=\"0000000000000abc\" version=\"0\" sdk_version=\"0\" group_id=\"00000000\" app_type=\"00000000\">") != NULL);
      CHECK(strstr(saved, "<name />") != NULL);
      /* a sub_path only where there is one */
      CHECK(strstr(saved, "<sub_path>0005000010101c00_v32</sub_path>") != NULL);
      CHECK(strstr(strstr(saved, "abc"), "sub_path") == NULL);
      /* one element a line, indented a space a level */
      CHECK(strstr(saved, "\n <title ") != NULL && strstr(saved, "\n  <path>/y</path>") != NULL);
      CHECK(title_list_parse(saved, len, &back, &nb) && nb == 2);
      if (nb == 2)
      {
         CHECK(back[0].title_id == made[0].title_id && back[0].version == 32);
         CHECK(back[0].sdk_version == made[0].sdk_version && back[0].group_id == 0x1C9 && back[0].app_type == 0x80000000UL);
         CHECK(same(back[0].name, "A & <B>") && same(back[0].sub_path, made[0].sub_path));
         CHECK(back[1].title_id == 0xABC && same(back[1].path, "/y") && same(back[1].sub_path, ""));
      }
      title_list_free(back, nb);
      free(saved);
   }
   saved = title_list_save(NULL, 0, &len);
   CHECK(saved && strstr(saved, "<title_list />") != NULL);
   free(saved);
}

int main(void)
{
   test_title_list();
   test_crc();
   test_meta();
   test_meta_numbers();
   test_ini();
   test_app();
   test_cos();
   if (failures)
   {
      fprintf(stderr, "title_xml_test: %d failed\n", failures);
      return 1;
   }
   printf("title_xml_test: all passed\n");
   return 0;
}
