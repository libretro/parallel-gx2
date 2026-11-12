#ifndef UTIL_HELPERS_MEM_STREAM_H
#define UTIL_HELPERS_MEM_STREAM_H

/* Bytes read from memory with bounds checked, and bytes written to memory
 * that grows: the shader, pipeline and register caches' records, a graphic
 * pack's patch files. C89.
 *
 * A read past the end reads nothing, gives 0, moves to the end and sets
 * error, which stays set. A write that cannot grow the buffer is dropped
 * and sets failed, which stays set. Numbers are big-endian. */

#include <stddef.h>
#include <stdint.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

typedef struct mem_reader
{
   const uint8_t *data;
   size_t size;
   size_t pos;
   int error;
} mem_reader_t;

void mem_reader_init(mem_reader_t *r, const void *data, size_t size);
uint8_t mem_read_u8(mem_reader_t *r);
uint16_t mem_read_u16(mem_reader_t *r);
uint32_t mem_read_u32(mem_reader_t *r);
uint64_t mem_read_u64(mem_reader_t *r);
/* n octets into out; 0 (and nothing read) when fewer are left */
int mem_read(mem_reader_t *r, void *out, size_t n);
/* n octets lent where they lie, moved past; NULL when fewer are left (and
 * for none of a reader over no data) */
const uint8_t *mem_read_view(mem_reader_t *r, size_t n);
/* The next line, up to '\n' or the end, without the '\n' and without '\r's
 * at either end; its start in *s and length in *len. 0 (and error set)
 * when the reader is already at the end */
int mem_read_line(mem_reader_t *r, const char **s, size_t *len);

typedef struct mem_writer
{
   uint8_t *buf;
   size_t len;
   size_t cap;
   int failed;
} mem_writer_t;

/* room for reserve octets made now, when it can be */
void mem_writer_init(mem_writer_t *w, size_t reserve);
void mem_writer_free(mem_writer_t *w);
/* emptied, its room kept */
void mem_writer_reset(mem_writer_t *w);
void mem_write(mem_writer_t *w, const void *data, size_t n);
void mem_write_u8(mem_writer_t *w, uint8_t v);
void mem_write_u16(mem_writer_t *w, uint16_t v);
void mem_write_u32(mem_writer_t *w, uint32_t v);
void mem_write_u64(mem_writer_t *w, uint64_t v);

RETRO_END_DECLS

#endif
