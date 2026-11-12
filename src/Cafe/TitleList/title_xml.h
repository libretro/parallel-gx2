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

RETRO_END_DECLS

#endif
