/* The core's XML tree (src/util/xml/xml_tree.c) under Xml.h: children
 * appended, prepended and freed with every link kept right, attributes in
 * order, a tree too deep to free by recursion, a load that keeps no blank
 * text and says where a bad one stops, writing back, and numbers read as
 * pugixml reads them. C89, as the C89 lane builds it. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "util/xml/xml_tree.h"

static int failures = 0;

/* C89 has no LL_MAX */
#define LL_MAX ((long long)(~0ULL >> 1))
#define LL_MIN (-LL_MAX - 1)
#define ULL_MAX (~0ULL)

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); failures++; } } while (0)

/* every child's links agree, and the names in order are want ("a,b,c") */
static int children_are(const xml_elem_t *p, const char *want)
{
   char got[256];
   const xml_elem_t *c, *prev = NULL;
   got[0] = '\0';
   for (c = p->first; c; prev = c, c = c->next)
   {
      if (c->parent != p || c->prev != prev)
         return 0;
      if (got[0])
         strcat(got, ",");
      strcat(got, c->name);
   }
   if (p->last != prev)
      return 0;
   return strcmp(got, want) == 0;
}

static void test_links(void)
{
   xml_elem_t *doc = xml_elem_new("", 0), *root, *b, *c;
   xml_attr_t *a;
   CHECK(doc && strcmp(doc->name, "") == 0);
   if (!doc)
      return;
   CHECK(xml_elem_append(NULL, "x", 1) == NULL && xml_elem_add_attr(NULL, "x", 1) == NULL);
   root = xml_elem_append(doc, "rootXYZ", 4);
   CHECK(root && strcmp(root->name, "root") == 0 && children_are(doc, "root"));
   b = xml_elem_append(root, "b", 1);
   c = xml_elem_append(root, "c", 1);
   xml_elem_prepend(root, "a", 1);
   xml_elem_append(root, "b", 1);
   CHECK(children_are(root, "a,b,c,b"));
   CHECK(xml_elem_child(root, "b", 1) == b && xml_elem_child(root, "bb", 2) == NULL);
   CHECK(xml_elem_next_named(root->first, "b", 1) == b);
   CHECK(xml_elem_next_named(b->next, "b", 1) == root->last);
   CHECK(xml_elem_next_named(c, "", 0) == c && xml_elem_next_named(NULL, "b", 1) == NULL);
   xml_elem_append(c, "deep", 4);
   CHECK(xml_elem_find(root, "deep", 4) == c->first && xml_elem_find(root, "nope", 4) == NULL);
   CHECK(xml_elem_find(doc, "c", 1) == c);

   /* freed from the middle, the ends, and to none */
   xml_elem_free(b);
   CHECK(children_are(root, "a,c,b"));
   xml_elem_free(root->first);
   CHECK(children_are(root, "c,b"));
   xml_elem_free(root->last);
   CHECK(children_are(root, "c"));
   xml_elem_free(c);
   CHECK(children_are(root, ""));
   xml_elem_prepend(root, "z", 1);
   CHECK(children_are(root, "z"));

   a = xml_elem_add_attr(root, "one", 3);
   CHECK(a && strcmp(a->value, "") == 0);
   CHECK(xml_set_str(&a->value, "1xx", 1) && strcmp(a->value, "1") == 0);
   xml_elem_add_attr(root, "two", 3);
   xml_elem_add_attr(root, "one", 3);
   CHECK(xml_elem_attr(root, "one", 3) == root->attrs);
   CHECK(strcmp(root->attrs->next->name, "two") == 0 && strcmp(root->attrs->next->next->name, "one") == 0);
   CHECK(xml_elem_attr(root, "on", 2) == NULL && xml_elem_attr(NULL, "one", 3) == NULL);
   CHECK(xml_set_str(&root->text, "t", 1) && strcmp(root->text, "t") == 0);

   xml_elem_clear(root);
   CHECK(root->first == NULL && root->attrs == NULL && root->text == NULL && strcmp(root->name, "root") == 0);
   xml_elem_free(doc);
   xml_elem_free(NULL);
   xml_elem_clear(NULL);
}

static void test_deep(void)
{
   /* deeper than a recursive free's stack would take */
   xml_elem_t *doc = xml_elem_new("", 0), *e = doc;
   long i;
   for (i = 0; i < 1000000L && e; i++)
   {
      xml_elem_t *sib;
      e = xml_elem_append(e, "d", 1);
      if (e && (i & 1))
      {
         sib = xml_elem_append(e->parent, "s", 1);
         CHECK(sib != NULL);
         if (sib)
            xml_elem_append(sib, "t", 1);
      }
   }
   CHECK(e != NULL);
   xml_elem_free(doc);
}

static void test_load_save(void)
{
   static const char doc_text[] =
      "<?xml version=\"1.0\"?>\n<menu a=\"1\" b=\"x &amp; y\">\n"
      "  <empty>   </empty>\n  <v>33</v>\n  <list><i>1</i><i>2</i></list>\n</menu>\n";
   xml_elem_t *doc = xml_elem_new("", 0), *menu;
   size_t at = 12345, len = 0;
   char *out;
   CHECK(xml_tree_load(doc, doc_text, strlen(doc_text), &at));
   CHECK(at == 12345);
   menu = doc->first;
   CHECK(menu && strcmp(menu->name, "menu") == 0 && menu == doc->last);
   CHECK(menu && menu->text == NULL);
   CHECK(menu && strcmp(xml_elem_attr(menu, "b", 1)->value, "x & y") == 0);
   CHECK(menu && xml_elem_child(menu, "empty", 5)->text == NULL);
   CHECK(menu && strcmp(xml_elem_child(menu, "v", 1)->text, "33") == 0);
   CHECK(menu && children_are(xml_elem_child(menu, "list", 4), "i,i"));

   out = xml_tree_save(doc, NULL, false, &len);
   CHECK(out && len == strlen(out));
   CHECK(out && strcmp(out, "<menu a=\"1\" b=\"x &amp; y\"><empty /><v>33</v><list><i>1</i><i>2</i></list></menu>") == 0);
   free(out);
   out = xml_tree_save(doc, "  ", true, NULL);
   CHECK(out && strncmp(out, "<?xml", 5) == 0 && strstr(out, "\n  <v>33</v>") != NULL);
   free(out);

   /* a bad one leaves nothing, and says where it stopped */
   CHECK(!xml_tree_load(doc, doc_text, 40, &at));
   CHECK(doc->first == NULL && at > 0 && at <= 40);
   CHECK(xml_tree_save(doc, NULL, false, &len) == NULL);
   CHECK(!xml_tree_load(doc, "", 0, NULL));
   CHECK(!xml_tree_load(doc, "<a/><b/>", 8, NULL));
   CHECK(xml_tree_load(doc, "<a/>", 4, NULL) && doc->first && !doc->first->first);
   CHECK(xml_tree_load(doc, "<b/>", 4, NULL) && children_are(doc, "b"));
   xml_elem_free(doc);
}

static void test_numbers(void)
{
   CHECK(xml_to_signed(NULL, 7, INT_MIN, INT_MAX) == 7);
   CHECK(xml_to_signed("", 7, INT_MIN, INT_MAX) == 0);
   CHECK(xml_to_signed(" \t-12", 7, INT_MIN, INT_MAX) == -12);
   CHECK(xml_to_signed("+0x1f", 7, INT_MIN, INT_MAX) == 31);
   CHECK(xml_to_signed("0X1F", 7, INT_MIN, INT_MAX) == 31);
   CHECK(xml_to_signed("12abc", 7, INT_MIN, INT_MAX) == 12);
   CHECK(xml_to_signed("\r\n12", 7, INT_MIN, INT_MAX) == 12);
   CHECK(xml_to_signed("\v12", 7, INT_MIN, INT_MAX) == 0);
   CHECK(xml_to_signed("2147483647", 7, INT_MIN, INT_MAX) == INT_MAX);
   CHECK(xml_to_signed("2147483648", 7, INT_MIN, INT_MAX) == INT_MAX);
   CHECK(xml_to_signed("-2147483648", 7, INT_MIN, INT_MAX) == INT_MIN);
   CHECK(xml_to_signed("-2147483649", 7, INT_MIN, INT_MAX) == INT_MIN);
   CHECK(xml_to_signed("-99999999999999999999999", 7, INT_MIN, INT_MAX) == INT_MIN);
   CHECK(xml_to_signed("99999999999999999999999", 7, INT_MIN, INT_MAX) == INT_MAX);
   CHECK(xml_to_signed("-9223372036854775808", 7, LL_MIN, LL_MAX) == LL_MIN);
   CHECK(xml_to_signed("9223372036854775807", 7, LL_MIN, LL_MAX) == LL_MAX);
   CHECK(xml_to_unsigned(NULL, 5, UINT_MAX) == 5);
   CHECK(xml_to_unsigned("x", 5, UINT_MAX) == 0);
   CHECK(xml_to_unsigned("777", 5, UINT_MAX) == 777);
   CHECK(xml_to_unsigned("4294967296", 5, UINT_MAX) == UINT_MAX);
   CHECK(xml_to_unsigned("-1", 5, UINT_MAX) == UINT_MAX);
   CHECK(xml_to_unsigned("-2", 5, UINT_MAX) == UINT_MAX - 1);
   CHECK(xml_to_unsigned("-4294967296", 5, UINT_MAX) == UINT_MAX);
   CHECK(xml_to_unsigned("0xFFFFFFFFFFFFFFFF", 5, ULL_MAX) == ULL_MAX);
   CHECK(xml_to_unsigned("0x10000000000000000", 5, ULL_MAX) == ULL_MAX);
   CHECK(xml_to_unsigned("18446744073709551615", 5, ULL_MAX) == ULL_MAX);
   CHECK(xml_to_unsigned("18446744073709551616", 5, ULL_MAX) == ULL_MAX);
   CHECK(xml_to_unsigned("-1", 5, ULL_MAX) == ULL_MAX);
}

int main(void)
{
   test_links();
   test_deep();
   test_load_save();
   test_numbers();
   if (failures)
   {
      fprintf(stderr, "xml_tree_test: %d failed\n", failures);
      return 1;
   }
   printf("xml_tree_test: all passed\n");
   return 0;
}
