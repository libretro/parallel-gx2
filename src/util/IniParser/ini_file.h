#ifndef UTIL_INIPARSER_INI_FILE_H
#define UTIL_INIPARSER_INI_FILE_H

/* An ini file (rules.txt, game profiles, a WUHB's meta.ini) parsed once
 * into one allocation: its [sections] in order, each with its name = value
 * options. C89.
 *
 * Lines end at \r or \n. # and ; start a comment outside "quotes". A
 * section's name is what lies between [ and the first ], or all of the line
 * when there is no ]. An option's name and value are trimmed of blanks; a
 * value in quotes loses them (one opening with a quote and not closing with
 * one is dropped). Dropped too: names that are empty or hold a blank, a
 * control character, [, ] or an octet above 128, and options before the
 * first section. A UTF-8 byte order mark at the start is let be. */

#include <stddef.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

typedef struct ini_span
{
   const char *s;   /* into the file's copy; not terminated */
   size_t len;
} ini_span_t;

typedef struct ini_option
{
   ini_span_t name;
   ini_span_t value;
} ini_option_t;

typedef struct ini_section
{
   ini_span_t name;
   size_t line;     /* counting only lines that are not blank, from 1 */
   size_t first;    /* its options: options[first .. first + count) */
   size_t count;
} ini_section_t;

typedef struct ini_file
{
   const ini_section_t *sections;
   size_t section_count;
   const ini_option_t *options;
   size_t option_count;
} ini_file_t;

/* len octets of data, copied; NULL only for no memory */
ini_file_t *ini_file_parse(const void *data, size_t len);
void ini_file_free(ini_file_t *ini);

/* The first option of the section named name (name_len octets, matched
 * ignoring ASCII case); NULL for none, or no such section */
const ini_option_t *ini_file_find(const ini_file_t *ini, size_t section,
      const char *name, size_t name_len);

/* The first section named name, exactly; section_count for none */
size_t ini_file_section(const ini_file_t *ini, const char *name);

RETRO_END_DECLS

#endif
