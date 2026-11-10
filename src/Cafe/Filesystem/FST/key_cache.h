#ifndef CAFE_FILESYSTEM_FST_KEY_CACHE_H
#define CAFE_FILESYSTEM_FST_KEY_CACHE_H

/* Disc keys: what keys.txt holds, a key kept in a file of its own (the
 * game.key a dumper writes beside game.wux), and whether a key decrypts a
 * disc's header. KeyCache.h is the C++ over it, which keeps the set
 * keys.txt gave and says what was tried. */

#include <stddef.h>
#include <stdint.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

#define KEY_CACHE_KEY_SIZE 16

/* The keys of a keys.txt */
typedef struct key_set
{
   uint8_t (*keys)[KEY_CACHE_KEY_SIZE];
   size_t count;
   size_t keys_txt; /* the example the file is made with not counted */
} key_set_t;

/* what a line of keys.txt that is no key is: called for each */
#define KEY_CACHE_LINE_NOT_HEX 1     /* not hex digits */
#define KEY_CACHE_LINE_NOT_A_KEY 2   /* hex, but not 32 digits */
typedef void (*key_cache_bad_line_t)(void *ud, unsigned line, int what);

/* keys.txt's text: one key a line, anything after a '#' or ';' a comment,
 * spaces, tabs, '-', '_' and '\r' let be. set is the caller's to
 * key_set_free; false only out of memory. */
bool key_set_parse(key_set_t *set, const char *text, size_t len,
      key_cache_bad_line_t bad, void *ud, unsigned *lines);
void key_set_free(key_set_t *set);

/* keys.txt at path into set: false when there is none to read */
bool key_set_load(key_set_t *set, const char *path,
      key_cache_bad_line_t bad, void *ud, unsigned *lines);

/* keys.txt as it is made where there is none: comments and the example */
bool key_cache_write_template(const char *path);

/* A disc key kept in a file: 16 octets, or 32 hex digits as text */
#define KEY_CACHE_FILE_ABSENT 0
#define KEY_CACHE_FILE_KEY 1
#define KEY_CACHE_FILE_NOT_A_KEY 2
int key_cache_read_key_file(const char *path, uint8_t *key_out);

/* Whether key decrypts a disc's header (the 48 octets at 0x18100: an IV,
 * then 32 octets that are zeros once decrypted) */
bool key_cache_fits(const uint8_t *key, const uint8_t *header48);

/* The keys in files beside image: its own <name>.key first, then the first
 * few dozen other .key files in its folder. Each one tried is told to
 * tried with what it is (KEY_CACHE_FILE_KEY or _NOT_A_KEY) and its name;
 * the keys go into keys, at most max of them. How many there are. */
typedef void (*key_cache_tried_t)(void *ud, const char *name, int what);
size_t key_cache_keys_beside(const char *image, uint8_t (*keys)[KEY_CACHE_KEY_SIZE], size_t max,
      key_cache_tried_t tried, void *ud);

RETRO_END_DECLS

#endif
