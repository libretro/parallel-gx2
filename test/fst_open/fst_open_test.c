/* The core's volume openers (src/Cafe/Filesystem/FST/fst_open.c): a disc
 * image and a title folder made here - right, and wrong in each of the
 * ways an opener turns one away - opened and read; tickets and TMDs
 * parsed. C89, as the C89 lane builds it. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lrc_hash.h>
#include <streams/file_stream.h>

#include "util/crypto/aes128.h"
#include "Cafe/Filesystem/FST/fst_open.h"

static int failures = 0;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); failures++; } } while (0)

#define SECTOR 0x8000

static const uint8_t DISC_KEY[16] = {
   0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
   0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xf0, 0x01};
static const uint8_t TITLE_KEY[16] = {
   0xc0, 0xff, 0xee, 0x00, 0x12, 0x34, 0x56, 0x78,
   0x9a, 0xbc, 0xde, 0xf0, 0x0f, 0x1e, 0x2d, 0x3c};
static const uint8_t COMMON_KEY[16] = {
   0xD7, 0xB0, 0x04, 0x02, 0x65, 0x9B, 0xA2, 0xAB,
   0xD2, 0xCB, 0x0D, 0xB2, 0x7F, 0xA2, 0xB6, 0x56};

#define GAME_SIZE  (SECTOR + 1000)
#define TIK_SIZE   0x350

static void be32(uint8_t *p, unsigned long v)
{
   p[0] = (uint8_t)(v >> 24);
   p[1] = (uint8_t)(v >> 16);
   p[2] = (uint8_t)(v >> 8);
   p[3] = (uint8_t)v;
}

static void be16(uint8_t *p, unsigned v)
{
   p[0] = (uint8_t)(v >> 8);
   p[1] = (uint8_t)v;
}

static uint8_t game_octet(size_t i)
{
   return (uint8_t)((i * 131u + (i >> 7)) & 0xFF);
}

/* An FST of two clusters (0: the FST, 1: raw, at cluster1_sector) and one
 * file at path ("dir/name" or "name") in cluster 1, of size octets; into
 * out, its size returned (a multiple of 16) */
static uint32_t make_fst(uint8_t *out, uint32_t cluster1_sector, const char *dir,
      const char *name, uint32_t size)
{
   uint8_t *e;
   uint32_t entries = dir ? 3 : 2, at;
   char *names;
   memset(out, 0, 0x200);
   be32(out, 0x46535400UL);
   be32(out + 4, 0x20);
   be32(out + 8, 2);
   be32(out + 0x40, cluster1_sector);
   e = out + 0x60;
   names = (char*)(e + 0x10 * entries);
   names[0] = '\0';
   at = 1;
   e[0] = 1;
   be32(e + 8, entries); /* root */
   e += 0x10;
   if (dir)
   {
      be32(e, 0x01000000UL | at);
      be32(e + 8, entries);
      strcpy(names + at, dir);
      at += (uint32_t)strlen(dir) + 1;
      e += 0x10;
   }
   be32(e, at);
   be32(e + 4, 0);
   be32(e + 8, size);
   be16(e + 14, 1);
   strcpy(names + at, name);
   at += (uint32_t)strlen(name) + 1;
   return (((uint32_t)((uint8_t*)names - out) + at) + 15) & ~15u;
}

/* a raw cluster: one CBC stream, its IV the cluster's index */
static void encrypt_cluster(uint8_t *out, const uint8_t *plain, size_t size,
      unsigned cluster, const uint8_t *key)
{
   uint8_t iv[16];
   memset(iv, 0, sizeof(iv));
   iv[0] = (uint8_t)(cluster >> 8);
   iv[1] = (uint8_t)cluster;
   AES128_CBC_encrypt(out, plain, (uint32_t)size, key, iv);
}

static void encrypt_fst(uint8_t *out, const uint8_t *plain, size_t size, const uint8_t *key)
{
   uint8_t iv[16];
   memset(iv, 0, sizeof(iv));
   AES128_CBC_encrypt(out, plain, (uint32_t)size, key, iv);
}

/* a ticket of title_id_high for TITLE_KEY */
static void make_ticket(uint8_t *tik, unsigned long title_id_high)
{
   uint8_t iv[16];
   memset(tik, 0, TIK_SIZE);
   be32(tik + 0x1DC, title_id_high);
   be32(tik + 0x1E0, 0x10101000UL);
   memcpy(iv, tik + 0x1DC, 8);
   memset(iv + 8, 0, 8);
   AES128_CBC_encrypt(tik + 0x1BF, TITLE_KEY, 16, COMMON_KEY, iv);
}

/* --- a disc image -------------------------------------------------------- */

#define SI_SECTOR 4
#define GM_SECTOR 10
#define DISC_SECTORS 14

typedef struct disc_faults
{
   bool no_magic;
   bool wrong_key;
   unsigned long partitions;   /* 0 for 2 */
   uint8_t addresses;          /* 0 for 1 */
   bool block_size;            /* the table's not a sector */
   bool no_si;
   bool two_si;                /* a second SI before the GM */
   bool second_gm;             /* a second GM after the first: not read */
   bool bad_gm_header;
   bool ticket_elsewhere;
   unsigned long ticket_title; /* 0 for 0x00050000 */
} disc_faults_t;

static bool write_disc(const char *path, const disc_faults_t *f)
{
   uint8_t *img = (uint8_t*)calloc(DISC_SECTORS, SECTOR);
   uint8_t *plain = (uint8_t*)calloc(1, SECTOR);
   uint8_t tik[TIK_SIZE];
   uint8_t fst[0x200];
   uint32_t fst_size, i;
   uint8_t *t;
   bool ok;
   if (!img || !plain)
      return false;
   if (!f->no_magic)
      be32(img + 2 * SECTOR, 0xCC549EB9UL);

   /* the partition table */
   t = plain;
   be32(t, 0xCCA6E67BUL);
   be32(t + 4, f->block_size ? 2 * SECTOR : SECTOR);
   be32(t + 0x1C, f->partitions ? f->partitions : (f->two_si || f->second_gm) ? 3 : 2);
   memcpy(t + 0x800, f->no_si ? "XX" : "SI", 2);
   t[0x800 + 0x1F] = f->addresses ? f->addresses : 1;
   be32(t + 0x800 + 0x20, SI_SECTOR);
   i = 0x880;
   if (f->two_si)
   {
      memcpy(t + i, "SI", 2);
      t[i + 0x1F] = 1;
      be32(t + i + 0x20, SI_SECTOR);
      i += 0x80;
   }
   memcpy(t + i, "GM", 2);
   t[i + 0x1F] = 1;
   be32(t + i + 0x20, GM_SECTOR);
   if (f->second_gm)
   {
      /* at sector 1, where there is no partition */
      memcpy(t + i + 0x80, "GM", 2);
      t[i + 0x80 + 0x1F] = 1;
      be32(t + i + 0x80 + 0x20, 1);
   }
   encrypt_fst(img + 3 * SECTOR, plain, SECTOR, f->wrong_key ? TITLE_KEY : DISC_KEY);

   /* SI: its header, its FST (sector 1 of it), the ticket (cluster 1, sector 2) */
   be32(img + SI_SECTOR * SECTOR, 0xCC93A4F5UL);
   be32(img + SI_SECTOR * SECTOR + 4, SECTOR);
   fst_size = make_fst(fst, 2, f->ticket_elsewhere ? "02" : "01", "title.tik", TIK_SIZE);
   be32(img + SI_SECTOR * SECTOR + 0x14, fst_size);
   be32(img + SI_SECTOR * SECTOR + 0x18, 1);
   encrypt_fst(img + (SI_SECTOR + 1) * SECTOR, fst, fst_size, DISC_KEY);
   make_ticket(tik, f->ticket_title ? f->ticket_title : 0x00050000UL);
   memset(plain, 0, SECTOR);
   memcpy(plain, tik, TIK_SIZE);
   encrypt_cluster(img + (SI_SECTOR + 2) * SECTOR, plain, SECTOR, 1, DISC_KEY);

   /* GM: its header, its FST, game.bin (cluster 1, sectors 2 and 3) */
   if (!f->bad_gm_header)
      be32(img + GM_SECTOR * SECTOR, 0xCC93A4F5UL);
   be32(img + GM_SECTOR * SECTOR + 4, SECTOR);
   fst_size = make_fst(fst, 2, NULL, "game.bin", GAME_SIZE);
   be32(img + GM_SECTOR * SECTOR + 0x14, fst_size);
   be32(img + GM_SECTOR * SECTOR + 0x18, 1);
   encrypt_fst(img + (GM_SECTOR + 1) * SECTOR, fst, fst_size, TITLE_KEY);
   {
      uint8_t *game = (uint8_t*)calloc(2, SECTOR);
      for (i = 0; i < GAME_SIZE; i++)
         game[i] = game_octet(i);
      encrypt_cluster(img + (GM_SECTOR + 2) * SECTOR, game, 2 * SECTOR, 1, TITLE_KEY);
      free(game);
   }
   ok = filestream_write_file(path, img, (int64_t)DISC_SECTORS * SECTOR);
   free(img);
   free(plain);
   return ok;
}

static bool reads_game(fst_volume_t *vol)
{
   const fst_table_t *t = fst_volume_table(vol);
   uint32_t e = fst_table_lookup(t, "game.bin", 8, true);
   uint8_t *buf = (uint8_t*)malloc(GAME_SIZE);
   bool ok = e != FST_TABLE_NONE && buf
      && fst_volume_read(vol, e, 0, GAME_SIZE, buf, NULL) == GAME_SIZE;
   uint32_t i;
   for (i = 0; ok && i < GAME_SIZE; i++)
      ok = buf[i] == game_octet(i);
   free(buf);
   return ok && !fst_volume_corrupted(vol);
}

static int open_disc(const disc_faults_t *f, uint32_t *detail)
{
   static const char path[] = "fst_open_test.wud";
   int why = -1;
   fst_volume_t *vol;
   if (!write_disc(path, f))
      return -2;
   vol = fst_disc_open(path, DISC_KEY, &why, detail);
   if (vol)
   {
      if (!reads_game(vol))
         why = -3;
      fst_volume_free(vol);
   }
   filestream_delete(path);
   return why;
}

static void test_disc(void)
{
   disc_faults_t f;
   uint32_t detail;
#define FAULT(field, value, want) do { memset(&f, 0, sizeof(f)); f.field = value; detail = 0; \
      CHECK(open_disc(&f, &detail) == (want)); } while (0)
   memset(&f, 0, sizeof(f));
   CHECK(open_disc(&f, NULL) == FST_DISC_OK);
   FAULT(no_magic, true, FST_DISC_NO_IMAGE);
   FAULT(wrong_key, true, FST_DISC_WRONG_KEY);
   FAULT(partitions, 31, FST_DISC_BAD_TABLE);
   CHECK(detail == 31);
   FAULT(addresses, 2, FST_DISC_BAD_TABLE);
   CHECK(detail == 2);
   FAULT(block_size, true, FST_DISC_BAD_TABLE);
   FAULT(second_gm, true, FST_DISC_OK);
   FAULT(no_si, true, FST_DISC_NO_PARTITION);
   FAULT(two_si, true, FST_DISC_NO_PARTITION);
   FAULT(bad_gm_header, true, FST_DISC_BAD_PARTITION);
   FAULT(ticket_elsewhere, true, FST_DISC_BAD_TICKET);
   FAULT(ticket_title, 0x00040000UL, FST_DISC_BAD_TICKET);
#undef FAULT
   CHECK(fst_disc_open("fst_open_test_missing.wud", DISC_KEY, NULL, NULL) == NULL);
}

/* --- a title folder -------------------------------------------------------- */

#define TMD_SIZE (0x204 + 64 * 36 + 2 * 0x30)

static void make_tmd(uint8_t *tmd, bool fst_cluster, const uint8_t *game_hash)
{
   uint8_t *e;
   memset(tmd, 0, TMD_SIZE);
   be32(tmd + 0x18C, 0x00050000UL);
   be32(tmd + 0x190, 0x10101000UL);
   be16(tmd + 0x1DC, 32);
   be16(tmd + 0x1DE, 2);
   e = tmd + 0x204 + 64 * 36;
   be32(e, 0x10);                      /* 00000010.app: the FST */
   be16(e + 4, fst_cluster ? 0 : 5);
   be32(e + 12, 0x200);
   e += 0x30;
   be32(e + 0, 0x11);                  /* 00000011.app: game.bin */
   be16(e + 4, 1);
   be16(e + 6, FST_TMD_SHA1);
   be32(e + 12, 2 * SECTOR);
   memcpy(e + 16, game_hash, 20);
}

static int open_title(bool fst_cluster, bool wrong_hash, bool missing, bool *corrupted)
{
   static const char *const paths[2] = {"fst_open_test_0.app", "fst_open_test_1.app"};
   uint8_t fst[0x200], enc[0x200];
   uint8_t tmd_data[TMD_SIZE];
   uint8_t *game = (uint8_t*)calloc(2, SECTOR);
   uint8_t *enc_game = (uint8_t*)malloc(2 * SECTOR);
   uint8_t hash[20];
   fst_tmd_t tmd;
   fst_volume_t *vol;
   uint32_t i;
   int why = -1;
   *corrupted = false;
   memset(fst, 0, sizeof(fst));
   make_fst(fst, 0, NULL, "game.bin", GAME_SIZE);
   encrypt_fst(enc, fst, sizeof(fst), TITLE_KEY);
   for (i = 0; i < GAME_SIZE; i++)
      game[i] = game_octet(i);
   SHA1Digest(game, 2 * SECTOR, hash);
   if (wrong_hash)
      hash[7] ^= 1;
   encrypt_cluster(enc_game, game, 2 * SECTOR, 1, TITLE_KEY);
   filestream_write_file(paths[0], enc, sizeof(enc));
   if (!missing)
      filestream_write_file(paths[1], enc_game, 2 * SECTOR);
   make_tmd(tmd_data, fst_cluster, hash);
   CHECK(fst_tmd_parse(tmd_data, TMD_SIZE, &tmd));
   vol = fst_title_open(&tmd, TITLE_KEY, paths, &why);
   fst_tmd_free(&tmd);
   if (vol)
   {
      const fst_table_t *t = fst_volume_table(vol);
      uint32_t e = fst_table_lookup(t, "game.bin", 8, true);
      uint8_t *buf = (uint8_t*)malloc(GAME_SIZE);
      /* all of it, in order: what hashes all of the content */
      uint32_t got = fst_volume_read(vol, e, 0, SECTOR, buf, NULL);
      got += fst_volume_read(vol, e, SECTOR, GAME_SIZE - SECTOR, buf + SECTOR, NULL);
      for (i = 0; i < got; i++)
         if (buf[i] != game_octet(i))
            why = -3;
      *corrupted = fst_volume_corrupted(vol);
      free(buf);
      fst_volume_free(vol);
   }
   filestream_delete(paths[0]);
   filestream_delete(paths[1]);
   free(game);
   free(enc_game);
   return why;
}

static void test_title(void)
{
   bool corrupted;
   CHECK(open_title(true, false, false, &corrupted) == FST_TITLE_OK && !corrupted);
   CHECK(open_title(true, true, false, &corrupted) == FST_TITLE_OK && corrupted);
   CHECK(open_title(true, false, true, &corrupted) == FST_TITLE_NO_FILES);
   CHECK(open_title(false, false, false, &corrupted) == FST_TITLE_NO_FST);
}

static void test_parsers(void)
{
   uint8_t tik[TIK_SIZE], key[16], tmd_data[TMD_SIZE], hash[20];
   fst_tmd_t tmd;
   make_ticket(tik, 0x00050000UL);
   CHECK(fst_ticket_title_key(tik, TIK_SIZE, key) && memcmp(key, TITLE_KEY, 16) == 0);
   CHECK(fst_ticket_title_key(tik, 0x220, key));
   CHECK(!fst_ticket_title_key(tik, 0x21F, key));
   CHECK(!fst_ticket_title_key(NULL, TIK_SIZE, key));
   make_ticket(tik, 0x00070000UL);
   CHECK(!fst_ticket_title_key(tik, TIK_SIZE, key));

   memset(hash, 0xAB, sizeof(hash));
   make_tmd(tmd_data, true, hash);
   CHECK(fst_tmd_parse(tmd_data, TMD_SIZE, &tmd));
   CHECK(tmd.title_id == 0x0005000010101000ULL && tmd.title_version == 32 && tmd.count == 2);
   CHECK(tmd.contents[1].id == 0x11 && tmd.contents[1].index == 1
         && tmd.contents[1].type == FST_TMD_SHA1 && tmd.contents[1].size == 2 * SECTOR
         && memcmp(tmd.contents[1].hash, hash, 20) == 0);
   fst_tmd_free(&tmd);
   CHECK(tmd.contents == NULL);
   /* short of its header, and of the contents it says it has */
   CHECK(!fst_tmd_parse(tmd_data, 0x204 + 64 * 36 - 1, &tmd));
   CHECK(!fst_tmd_parse(tmd_data, TMD_SIZE - 1, &tmd));
   be16(tmd_data + 0x1DE, 0xFFFF);
   CHECK(!fst_tmd_parse(tmd_data, TMD_SIZE, &tmd));
}

int main(void)
{
   test_parsers();
   test_disc();
   test_title();
   if (failures)
   {
      printf("fst_open_test: %d failure(s)\n", failures);
      return 1;
   }
   printf("fst_open_test: all passed\n");
   return 0;
}
