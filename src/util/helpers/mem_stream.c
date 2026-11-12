/* Bytes read from and written to memory (mem_stream.h) */

#include <stdlib.h>
#include <string.h>

#include "mem_stream.h"

void mem_reader_init(mem_reader_t *r, const void *data, size_t size)
{
   r->data = (const uint8_t*)data;
   r->size = data ? size : 0;
   r->pos = 0;
   r->error = 0;
}

const uint8_t *mem_read_view(mem_reader_t *r, size_t n)
{
   const uint8_t *p;
   if (n > r->size - r->pos)
   {
      r->pos = r->size;
      r->error = 1;
      return NULL;
   }
   p = r->data + r->pos;
   r->pos += n;
   return p;
}

int mem_read(mem_reader_t *r, void *out, size_t n)
{
   const uint8_t *p;
   if (!n)
      return 1;
   if (!(p = mem_read_view(r, n)))
      return 0;
   memcpy(out, p, n);
   return 1;
}

static uint64_t read_be(mem_reader_t *r, size_t n)
{
   const uint8_t *p = mem_read_view(r, n);
   uint64_t v = 0;
   size_t i;
   if (!p)
      return 0;
   for (i = 0; i < n; i++)
      v = (v << 8) | p[i];
   return v;
}

uint8_t mem_read_u8(mem_reader_t *r)
{
   return (uint8_t)read_be(r, 1);
}

uint16_t mem_read_u16(mem_reader_t *r)
{
   return (uint16_t)read_be(r, 2);
}

uint32_t mem_read_u32(mem_reader_t *r)
{
   return (uint32_t)read_be(r, 4);
}

uint64_t mem_read_u64(mem_reader_t *r)
{
   return read_be(r, 8);
}

int mem_read_line(mem_reader_t *r, const char **s, size_t *len)
{
   const char *begin, *end;
   const uint8_t *nl;
   if (r->pos >= r->size)
   {
      r->error = 1;
      *s = NULL;
      *len = 0;
      return 0;
   }
   begin = (const char*)r->data + r->pos;
   nl = (const uint8_t*)memchr(begin, '\n', r->size - r->pos);
   if (nl)
   {
      end = (const char*)nl;
      r->pos = (size_t)(nl - r->data) + 1;
   }
   else
   {
      end = (const char*)r->data + r->size;
      r->pos = r->size;
   }
   while (begin < end && *begin == '\r')
      begin++;
   while (end > begin && end[-1] == '\r')
      end--;
   *s = begin;
   *len = (size_t)(end - begin);
   return 1;
}

/* room for n more; 0 (failed set) when there is none to be had */
static int reserve(mem_writer_t *w, size_t n)
{
   size_t cap;
   uint8_t *buf;
   if (w->failed)
      return 0;
   if (n <= w->cap - w->len)
      return 1;
   if (n > (size_t)-1 - w->len)
   {
      w->failed = 1;
      return 0;
   }
   cap = w->cap ? w->cap : 128;
   while (cap < w->len + n)
      cap = cap > (size_t)-1 / 2 ? w->len + n : cap * 2;
   buf = (uint8_t*)realloc(w->buf, cap);
   if (!buf)
   {
      w->failed = 1;
      return 0;
   }
   w->buf = buf;
   w->cap = cap;
   return 1;
}

void mem_writer_init(mem_writer_t *w, size_t reserve_len)
{
   w->buf = NULL;
   w->len = 0;
   w->cap = 0;
   w->failed = 0;
   if (reserve_len)
   {
      reserve(w, reserve_len);
      w->failed = 0;
   }
}

void mem_writer_free(mem_writer_t *w)
{
   free(w->buf);
   w->buf = NULL;
   w->len = 0;
   w->cap = 0;
   w->failed = 0;
}

void mem_writer_reset(mem_writer_t *w)
{
   w->len = 0;
   w->failed = 0;
}

void mem_write(mem_writer_t *w, const void *data, size_t n)
{
   if (!n || !reserve(w, n))
      return;
   memcpy(w->buf + w->len, data, n);
   w->len += n;
}

static void write_be(mem_writer_t *w, uint64_t v, size_t n)
{
   uint8_t b[8];
   size_t i;
   for (i = 0; i < n; i++)
      b[i] = (uint8_t)(v >> (8 * (n - 1 - i)));
   mem_write(w, b, n);
}

void mem_write_u8(mem_writer_t *w, uint8_t v)
{
   write_be(w, v, 1);
}

void mem_write_u16(mem_writer_t *w, uint16_t v)
{
   write_be(w, v, 2);
}

void mem_write_u32(mem_writer_t *w, uint32_t v)
{
   write_be(w, v, 4);
}

void mem_write_u64(mem_writer_t *w, uint64_t v)
{
   write_be(w, v, 8);
}
