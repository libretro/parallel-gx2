/* Base64 (src/util/crypto/base64_codec.c), as Miiverse's and the console
 * services' payloads are encoded and decoded: RFC 4648's vectors both ways,
 * every length of a run of bytes there and back, and decoding as Cemu
 * always has - nothing for a length that is not a multiple of 4 or is 3 or
 * less, '=' as 0 wherever it is, and a character that is not base64 as the
 * bits it always gave. C89, as the C89 lane builds it. */

#include <stdio.h>
#include <string.h>

#include "util/crypto/base64_codec.h"

static int failures = 0;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static int encodes_to(const char *in, const char *want)
{
   char out[64];
   size_t n;
   memset(out, '?', sizeof(out));
   n = b64_encode(out, in, strlen(in));
   return n == strlen(want) && memcmp(out, want, n) == 0 && out[n] == '?'
      && n == B64_ENCODED_SIZE(strlen(in));
}

static int decodes_to(const char *in, const char *want, size_t want_len)
{
   unsigned char out[64];
   size_t n;
   memset(out, 0xEE, sizeof(out));
   if (b64_decoded_size(in, strlen(in)) != want_len)
      return 0;
   n = b64_decode(out, in, strlen(in));
   return n == want_len && memcmp(out, want, n) == 0 && out[n] == 0xEE;
}

static void test_vectors(void)
{
   static const char *plain[] = { "", "f", "fo", "foo", "foob", "fooba", "foobar" };
   static const char *coded[] = { "", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy" };
   size_t i;
   for (i = 0; i < 7; i++)
   {
      CHECK(encodes_to(plain[i], coded[i]));
      /* "" and lengths of 3 or less decode to nothing, as they always did */
      CHECK(decodes_to(coded[i], plain[i], strlen(coded[i]) <= 3 ? 0 : strlen(plain[i])));
   }
   CHECK(encodes_to("\xFB\xFF\xBF", "+/+/"));
   CHECK(decodes_to("+/+/", "\xFB\xFF\xBF", 3));
}

static void test_round_trip(void)
{
   unsigned char in[300], back[300];
   char coded[400];
   size_t len, i, n;
   for (len = 0; len < sizeof(in); len++)
   {
      for (i = 0; i < len; i++)
         in[i] = (unsigned char)(len * 21 + i * 133);
      n = b64_encode(coded, in, len);
      CHECK(n == B64_ENCODED_SIZE(len));
      if (len == 0)
         continue;
      CHECK(b64_decoded_size(coded, n) == len);
      CHECK(b64_decode(back, coded, n) == len && memcmp(in, back, len) == 0);
   }
}

static void test_as_always(void)
{
   unsigned char out[16];
   /* not a multiple of 4, or too short: nothing */
   CHECK(b64_decoded_size("Zm9vY", 5) == 0 && b64_decode(out, "Zm9vY", 5) == 0);
   CHECK(b64_decoded_size("Zm=", 3) == 0);
   CHECK(b64_decoded_size("Zm9", 3) == 0);
   /* '=' in the middle counts as 0 and does not end it */
   CHECK(decodes_to("Z===Zm9v", "\x64\x00\x00" "foo", 6));
   /* a character that is not base64 is 64, shifted in as it always was:
    * in the first place it falls off the top, in the second it sets bit 2
    * of the first octet */
   CHECK(decodes_to("-m9v", "\x02\x6F\x6F", 3));
   CHECK(decodes_to("Z-9v", "\x68\x0F\x6F", 3));
   CHECK(decodes_to("Zm-v", "\x66\x70\x2F", 3));
   CHECK(decodes_to("Zm9-", "\x66\x6F\x80", 3));
   /* an octet above 0x7F is not base64 either */
   CHECK(decodes_to("Z\xC3" "9v", "\x68\x0F\x6F", 3));
}

int main(void)
{
   test_vectors();
   test_round_trip();
   test_as_always();
   if (failures)
   {
      fprintf(stderr, "base64_test: %d failure(s)\n", failures);
      return 1;
   }
   printf("base64_test: all passed\n");
   return 0;
}
