#ifndef CEMU_NCRYPTO_ETICKET_H
#define CEMU_NCRYPTO_ETICKET_H

/* A Wii U eTicket (.tik): its title, ticket id and version, whether it is
 * personalised to a console, and - in a version 1 ticket for DLC
 * (0005000c...) - which of its contents it grants. Read in place, every
 * offset the file gives checked against its size. C89. */

#include <stddef.h>
#include <stdint.h>

#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* a rights entry: a base index (at most ETICKET_RIGHTS_BASE_MAX) and 1024
 * bits from it */
#define ETICKET_RIGHTS_BASE_MAX 0x1000
#define ETICKET_RIGHTS_PER_ENTRY 1024
#define ETICKET_RIGHTS_MAX (ETICKET_RIGHTS_BASE_MAX + ETICKET_RIGHTS_PER_ENTRY)

/* eticket_t.warnings */
#define ETICKET_SKIPPED_RIGHTS 1 /* a rights entry's base index was too high */

/* why eticket_parse refused a ticket */
enum eticket_error
{
   ETICKET_OK = 0,
   ETICKET_TOO_SHORT,         /* smaller than its header */
   ETICKET_NOT_WIIU,          /* a title id not 0005... */
   ETICKET_TRUNCATED,         /* a section or entry past the end */
   ETICKET_BAD_RIGHTS_SIZE    /* rights entries not 0x84 octets */
};

typedef struct eticket
{
   uint64_t title_id;
   uint64_t ticket_id;
   uint32_t device_id;       /* 0 where it is not personalised */
   uint16_t title_version;
   uint8_t format_version;
   uint8_t warnings;
   /* contents granted: index i is bit i % 8 of rights[i / 8] */
   uint32_t rights_count;
   uint8_t rights[ETICKET_RIGHTS_MAX / 8];
} eticket_t;

enum eticket_error eticket_parse(const uint8_t *data, size_t size, eticket_t *out);

/* whether the ticket grants content index */
bool eticket_right(const eticket_t *t, uint32_t index);

RETRO_END_DECLS

#endif
