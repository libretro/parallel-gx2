/* The core's memory streams (src/util/helpers/mem_stream.c) under
 * Serializer.h, as the shader, pipeline and register caches and graphic
 * pack patch files use them: big-endian numbers, reads past the end, lent
 * views, lines with '\r's, a writer growing from nothing and kept through
 * a reset, and a write too big to be had. C89, as the C89 lane builds it. */

#include <stdio.h>
#include <string.h>

#include "util/helpers/mem_stream.h"

static int failures = 0;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static int line_is(mem_reader_t *r, const char *want)
{
   const char *s;
   size_t len;
   if (!mem_read_line(r, &s, &len))
      return 0;
   return len == strlen(want) && memcmp(s, want, len) == 0;
}

static void test_reader(void)
{
   static const uint8_t data[] = {
      0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
      0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11};
   mem_reader_t r;
   uint8_t buf[4];
   const uint8_t *v;
   mem_reader_init(&r, data, sizeof(data));
   CHECK(mem_read_u8(&r) == 0x01);
   CHECK(mem_read_u16(&r) == 0x0203);
   CHECK(mem_read_u32(&r) == 0x04050607UL);
   CHECK(mem_read_u64(&r) == (((uint64_t)0x08090A0BUL << 32) | 0x0C0D0E0FUL));
   CHECK(!r.error && r.pos == 15);
   v = mem_read_view(&r, 0);
   CHECK(v == data + 15 && !r.error);
   /* two left: a u32 is past the end */
   CHECK(mem_read_u32(&r) == 0);
   CHECK(r.error && r.pos == r.size);
   /* and it stays an error, past the end */
   CHECK(mem_read_u8(&r) == 0 && r.error);

   mem_reader_init(&r, data, sizeof(data));
   CHECK(mem_read(&r, buf, 4) && memcmp(buf, data, 4) == 0);
   v = mem_read_view(&r, 13);
   CHECK(v == data + 4 && r.pos == r.size && !r.error);
   CHECK(mem_read(&r, buf, 0) && !r.error);
   memset(buf, 0x55, 4);
   CHECK(!mem_read(&r, buf, 1) && r.error && buf[0] == 0x55);

   mem_reader_init(&r, data, 3);
   CHECK(mem_read_view(&r, (size_t)-1) == NULL && r.error && r.pos == 3);
   mem_reader_init(&r, data, 3);
   CHECK(mem_read_u8(&r) == 1 && mem_read_view(&r, 3) == NULL && r.pos == 3);
   mem_reader_init(&r, NULL, 10);
   CHECK(r.size == 0 && mem_read(&r, buf, 0) && !r.error);
   CHECK(mem_read_u8(&r) == 0 && r.error);
}

static void test_lines(void)
{
   static const char text[] = "first\r\n\r\n\rboth\r\r\nlast";
   mem_reader_t r;
   const char *s;
   size_t len;
   mem_reader_init(&r, text, strlen(text));
   CHECK(line_is(&r, "first"));
   CHECK(line_is(&r, ""));
   CHECK(line_is(&r, "both"));
   CHECK(line_is(&r, "last"));
   CHECK(!r.error && r.pos == r.size);
   CHECK(!mem_read_line(&r, &s, &len) && r.error && s == NULL && len == 0);

   /* a '\n' last: no empty line after it */
   mem_reader_init(&r, "a\nb\n", 4);
   CHECK(line_is(&r, "a") && line_is(&r, "b"));
   CHECK(!mem_read_line(&r, &s, &len));
   /* '\r's within a line stay */
   mem_reader_init(&r, "a\rb\n", 4);
   CHECK(line_is(&r, "a\rb"));
   mem_reader_init(&r, "\r\r", 2);
   CHECK(line_is(&r, "") && r.pos == 2);
}

static void test_writer(void)
{
   mem_writer_t w;
   uint8_t big[1000];
   size_t i, cap;
   mem_writer_init(&w, 0);
   CHECK(w.buf == NULL && w.len == 0 && w.cap == 0);
   mem_write_u8(&w, 0x01);
   mem_write_u16(&w, 0x0203);
   mem_write_u32(&w, 0x04050607UL);
   mem_write_u64(&w, ((uint64_t)0x08090A0BUL << 32) | 0x0C0D0E0FUL);
   CHECK(w.len == 15 && !w.failed);
   for (i = 0; i < 15; i++)
      CHECK(w.buf[i] == i + 1);
   for (i = 0; i < sizeof(big); i++)
      big[i] = (uint8_t)i;
   for (i = 0; i < 50; i++)
      mem_write(&w, big, sizeof(big));
   CHECK(w.len == 15 + 50 * sizeof(big) && w.cap >= w.len);
   CHECK(memcmp(w.buf + 15 + 49 * sizeof(big), big, sizeof(big)) == 0);
   mem_write(&w, NULL, 0);
   CHECK(w.len == 15 + 50 * sizeof(big));
   cap = w.cap;
   mem_writer_reset(&w);
   CHECK(w.len == 0 && w.cap == cap && w.buf != NULL);
   mem_write_u16(&w, 0xBEEF);
   CHECK(w.len == 2 && w.buf[0] == 0xBE && w.buf[1] == 0xEF && w.cap == cap);
   /* more than can be had: dropped, and later writes too, till a reset */
   mem_write(&w, big, (size_t)-1);
   CHECK(w.failed && w.len == 2);
   mem_write_u8(&w, 7);
   CHECK(w.len == 2);
   mem_writer_reset(&w);
   mem_write_u8(&w, 7);
   CHECK(!w.failed && w.len == 1 && w.buf[0] == 7);
   mem_writer_free(&w);
   CHECK(w.buf == NULL && w.cap == 0);
   mem_writer_free(&w);

   /* room doubles, so a stream of small writes reallocates rarely */
   mem_writer_init(&w, 0);
   mem_write(&w, big, 129);
   CHECK(w.cap == 256);
   mem_write(&w, big, 128);
   CHECK(w.cap == 512 && w.len == 257);
   mem_writer_free(&w);

   mem_writer_init(&w, 4096);
   CHECK(w.cap >= 4096 && w.len == 0 && !w.failed);
   mem_writer_free(&w);
}

int main(void)
{
   test_reader();
   test_lines();
   test_writer();
   if (failures)
   {
      fprintf(stderr, "mem_stream_test: %d failed\n", failures);
      return 1;
   }
   printf("mem_stream_test: all passed\n");
   return 0;
}
