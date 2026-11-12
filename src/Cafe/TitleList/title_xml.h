#ifndef CAFE_TITLELIST_TITLE_XML_H
#define CAFE_TITLELIST_TITLE_XML_H

/* What a title says of itself: meta/meta.xml (or a WUHB's meta/meta.ini),
 * code/app.xml and code/cos.xml, parsed with libretro-common's rxml into
 * plain structs. C89. */

#include <stddef.h>
#include <stdint.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* the console's languages, in CafeConsoleLanguage's order: ja en fr de it
 * es zhs ko nl pt ru zht */
#define TITLE_LANGUAGES 12

typedef struct title_meta
{
   uint64_t title_id;
   uint32_t version;
   uint32_t region;
   uint32_t olv_accesskey;
   /* each NULL when the file has none */
   char *product_code;
   char *company_code;
   char *content_platform;
   char *long_name[TITLE_LANGUAGES];  /* line ends made spaces */
   char *short_name[TITLE_LANGUAGES];
   char *publisher[TITLE_LANGUAGES];
} title_meta_t;

/* A meta.xml of len octets. False if it is not one, and then in why (when
 * not NULL, why_size octets) which part of one it lacks. */
bool title_meta_parse(const void *xml, size_t len, title_meta_t *out,
      char *why, size_t why_size);
/* A WUHB's meta.ini: its [menu] author, longname and shortname as the
 * English publisher and names, and a title id made from them. */
bool title_meta_parse_ini(const void *ini, size_t len, title_meta_t *out);
/* a copy of all of it; false (and to empty) for no memory */
bool title_meta_copy(title_meta_t *to, const title_meta_t *from);
void title_meta_free(title_meta_t *meta);

typedef struct title_app
{
   uint64_t title_id;
   uint16_t title_version;
   uint32_t app_type;
   uint32_t group_id;
   uint32_t sdk_version;
} title_app_t;

bool title_app_parse(const void *xml, size_t len, title_app_t *out);

#define TITLE_COS_PERMISSIONS 19

typedef struct title_cos
{
   char *argstr;      /* NULL when the file has none */
   struct
   {
      uint32_t group;  /* 0 for none */
      uint64_t mask;   /* all set by default */
   } permissions[TITLE_COS_PERMISSIONS];
} title_cos_t;

bool title_cos_parse(const void *xml, size_t len, title_cos_t *out);
/* argstr, copied; for a title with no cos.xml (a WUHB's "root.rpx") */
bool title_cos_default(title_cos_t *out, const char *argstr);
bool title_cos_copy(title_cos_t *to, const title_cos_t *from);
/* its argstr freed, and it left as a cos.xml with nothing in it reads:
 * no argstr, every capability (and so it may be freed twice) */
void title_cos_free(title_cos_t *cos);

/* A title as the title list's cache file (title_list_cache.xml) keeps it */
typedef struct title_list_entry
{
   uint64_t title_id;
   uint32_t version;
   uint32_t sdk_version;
   uint32_t format;      /* TitleInfo::TitleDataFormat */
   uint32_t region;
   uint32_t group_id;
   uint32_t app_type;
   char *name;           /* each "" for none */
   char *path;
   char *sub_path;
} title_list_entry_t;

/* The titles of a cache file of len octets, every element of its
 * <title_list> whose titleId is one (none, for another root); *entries
 * malloc'd (NULL for none). False for a file that is not XML, or no
 * memory. */
bool title_list_parse(const void *xml, size_t len, title_list_entry_t **entries,
      size_t *count);
void title_list_free(title_list_entry_t *entries, size_t count);
/* The cache file of count titles: malloc'd, *len octets; NULL for no
 * memory */
char *title_list_save(const title_list_entry_t *entries, size_t count,
      size_t *len);

RETRO_END_DECLS

#endif
