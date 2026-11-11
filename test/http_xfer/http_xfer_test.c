/* The core's HTTP transfers (src/Common/http_xfer.c) against local servers
 * (run_tests.sh starts them):
 *   http_xfer_test <case> <http port> <https port> <dir>
 * <dir> holds body.bin and its SHA-256 in body.sha256, and for the TLS
 * cases ca.der, cli.der and cli.key.der. Exit 0 on success. C89, as the
 * C89 lane builds it. */

/* retro_sleep's nanosleep, which strict C hides */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200112L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lrc_hash.h>
#include <retro_timers.h>
#include <rthreads/rthreads.h>

#include "Common/http_xfer.h"

static const char *dir;
static int http_port, https_port;

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

typedef struct buffer
{
   unsigned char *data;
   size_t len;
   size_t cap;
} buffer_t;

static bool append(buffer_t *b, const void *data, size_t len)
{
   if (b->len + len > b->cap)
   {
      size_t cap = (b->len + len) * 2;
      unsigned char *grown = (unsigned char*)realloc(b->data, cap);
      if (!grown)
         return false;
      b->data = grown;
      b->cap = cap;
   }
   memcpy(b->data + b->len, data, len);
   b->len += len;
   return true;
}

static bool on_body(void *ctx, const void *data, size_t len)
{
   return append((buffer_t*)ctx, data, len);
}

static bool slurp(const char *name, buffer_t *b)
{
   char path[1024];
   char chunk[4096];
   size_t n;
   FILE *f;
   sprintf(path, "%.900s/%s", dir, name);
   f = fopen(path, "rb");
   memset(b, 0, sizeof(*b));
   if (!f)
      return false;
   while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
      append(b, chunk, n);
   fclose(f);
   return true;
}

static void sha256_of(const void *data, size_t len, char hex[65])
{
   sha256_hash(hex, (const uint8_t*)data, len);
}

static bool want_sha(const buffer_t *body)
{
   buffer_t want;
   char hex[65];
   bool same;
   if (!slurp("body.sha256", &want) || want.len < 64)
      return false;
   sha256_of(body->data, body->len, hex);
   same = memcmp(hex, want.data, 64) == 0;
   free(want.data);
   return same;
}

static void url(char *out, bool tls, const char *path)
{
   if (tls)
      sprintf(out, "https://localhost:%d%s", https_port, path);
   else
      sprintf(out, "http://127.0.0.1:%d%s", http_port, path);
}

static unsigned char big_octet(size_t i)
{
   return (unsigned char)((i * 7 + (i >> 9)) & 255);
}

static bool is_big(const buffer_t *b)
{
   size_t i;
   if (b->len != (size_t)3 << 20)
      return false;
   for (i = 0; i < b->len; i++)
      if (b->data[i] != big_octet(i))
         return false;
   return true;
}

static int get(http_xfer_tls_t *tls, int want_result)
{
   http_xfer_request_t req;
   buffer_t body;
   char u[128];
   int status = -1, r;
   memset(&req, 0, sizeof(req));
   memset(&body, 0, sizeof(body));
   url(u, tls != NULL, "/body.bin");
   req.url = u;
   req.tls = tls;
   r = http_xfer_perform(&req, on_body, &body, &status);
   if (r != want_result)
      fprintf(stderr, "result %d, status %d\n", r, status);
   CHECK(r == want_result);
   if (want_result == HTTP_XFER_OK)
   {
      CHECK(status == 200);
      CHECK(want_sha(&body));
   }
   free(body.data);
   return 0;
}

/* the server answers a POST with the SHA-256 of what it got, X-Test and
 * the Content-Type */
static int post(void)
{
   http_xfer_request_t req;
   buffer_t body, sent;
   char u[128], hex[65], want[256];
   int status = 0;
   CHECK(slurp("body.bin", &sent));
   sent.len = 300001;
   memset(&req, 0, sizeof(req));
   memset(&body, 0, sizeof(body));
   url(u, false, "/echo");
   req.url = u;
   req.method = "POST";
   req.headers = "X-Test: yes\r\n";
   req.content_type = "application/octet-stream";
   req.body = sent.data;
   req.body_len = sent.len;
   req.has_body = true;
   CHECK(http_xfer_perform(&req, on_body, &body, &status) == HTTP_XFER_OK && status == 200);
   sha256_of(sent.data, sent.len, hex);
   sprintf(want, "%s yes application/octet-stream", hex);
   CHECK(body.len == strlen(want) && memcmp(body.data, want, body.len) == 0);
   free(body.data);
   free(sent.data);
   return 0;
}

/* multipart/form-data, as nn_olv uploads: the boundary grown past the one
 * a part contains */
static int form(void)
{
   static const char bin[] = "\0\1\2--------------------------httpxfer";
   http_xfer_part_t parts[2];
   http_xfer_request_t req;
   buffer_t body;
   size_t len;
   char *type = NULL;
   void *data;
   char u[128], hex[65], want[512];
   const char *b = "------------------------httpxferx";
   int status = 0;
   parts[0].name = "description";
   parts[0].data = "a description";
   parts[0].len = 13;
   parts[0].content_type = NULL;
   parts[1].name = "app_data";
   parts[1].data = bin;
   parts[1].len = sizeof(bin) - 1;
   parts[1].content_type = "application/octet-stream";
   data = http_xfer_multipart(parts, 2, &len, &type);
   CHECK(data && type);
   sprintf(want, "multipart/form-data; boundary=%s", b);
   CHECK(strcmp(type, want) == 0);
   {
      /* what it should be, made here */
      buffer_t w;
      memset(&w, 0, sizeof(w));
      sprintf(want, "--%s\r\nContent-Disposition: form-data; name=\"description\"\r\n\r\na description\r\n"
            "--%s\r\nContent-Disposition: form-data; name=\"app_data\"\r\nContent-Type: application/octet-stream\r\n\r\n", b, b);
      append(&w, want, strlen(want));
      append(&w, bin, sizeof(bin) - 1);
      sprintf(want, "\r\n--%s--\r\n", b);
      append(&w, want, strlen(want));
      CHECK(w.len == len && memcmp(w.data, data, len) == 0);
      free(w.data);
   }
   memset(&req, 0, sizeof(req));
   memset(&body, 0, sizeof(body));
   url(u, false, "/echo");
   req.url = u;
   req.method = "POST";
   req.content_type = type;
   req.body = data;
   req.body_len = len;
   req.has_body = true;
   CHECK(http_xfer_perform(&req, on_body, &body, &status) == HTTP_XFER_OK);
   sha256_of(data, len, hex);
   sprintf(want, "%s None %s", hex, type);
   CHECK(body.len == strlen(want) && memcmp(body.data, want, body.len) == 0);
   free(body.data);
   free(data);
   free(type);
   return 0;
}

static int not_found(void)
{
   http_xfer_request_t req;
   char u[128];
   int status = 0;
   memset(&req, 0, sizeof(req));
   url(u, false, "/missing");
   req.url = u;
   CHECK(http_xfer_perform(&req, NULL, NULL, &status) == HTTP_XFER_OK);
   CHECK(status == 404);
   return 0;
}

static int timeout(void)
{
   http_xfer_request_t req;
   char u[128];
   memset(&req, 0, sizeof(req));
   url(u, false, "/stall");
   req.url = u;
   req.timeout_ms = 700;
   CHECK(http_xfer_perform(&req, NULL, NULL, NULL) == HTTP_XFER_TIMEOUT);
   return 0;
}

static bool refuse(void *ctx, const void *data, size_t len)
{
   (void)ctx;
   (void)data;
   (void)len;
   return false;
}

/* a body the caller refuses; one closed while it hangs: both ended, and
 * the next transfer runs */
static int abort_close(void)
{
   http_xfer_request_t req;
   http_xfer_t *x;
   char u[128];
   memset(&req, 0, sizeof(req));
   url(u, false, "/body.bin");
   req.url = u;
   CHECK(http_xfer_perform(&req, refuse, NULL, NULL) == HTTP_XFER_ABORTED);
   url(u, false, "/stall");
   x = http_xfer_start(&req);
   CHECK(x != NULL);
   retro_sleep(100);
   http_xfer_close(x);
   return get(NULL, HTTP_XFER_OK);
}

/* all of what has come back; done when the outcome is among it */
static bool drain(http_xfer_t *x, buffer_t *body, int *result, unsigned *events)
{
   const http_xfer_event_t *ev;
   while ((ev = http_xfer_peek(x)) != NULL)
   {
      (*events)++;
      if (ev->kind == HTTP_XFER_BODY)
         append(body, ev->data, ev->len);
      else if (ev->kind == HTTP_XFER_DONE)
      {
         *result = ev->result;
         http_xfer_pop(x);
         return true;
      }
      http_xfer_pop(x);
   }
   return false;
}

static bool has_event(void *ctx)
{
   return http_xfer_peek((http_xfer_t*)ctx) != NULL;
}

/* run to its end, a sleep at a time */
static bool finish(http_xfer_t *x, buffer_t *body, int *result)
{
   unsigned events = 0;
   while (!drain(x, body, result, &events))
      http_xfer_wait(x, has_event, x);
   return true;
}

/* A transfer its caller does not read stops at 1 MiB queued, and one
 * paused stops: neither ends until read, or let go on. */
static int backpressure(void)
{
   http_xfer_request_t req;
   http_xfer_t *x;
   buffer_t body;
   char u[128];
   int result = -1;
   unsigned events = 0;
   const http_xfer_event_t *ev;
   memset(&req, 0, sizeof(req));
   url(u, false, "/big.bin");
   req.url = u;

   /* not read: what has come back stops at 1 MiB, short of all of it -
    * and so short of its end */
   memset(&body, 0, sizeof(body));
   x = http_xfer_start(&req);
   CHECK(x != NULL);
   retro_sleep(500);
   CHECK(http_xfer_queued(x) >= ((size_t)1 << 20) && http_xfer_queued(x) < ((size_t)3 << 20));
   retro_sleep(200);
   CHECK(http_xfer_queued(x) < ((size_t)3 << 20));
   CHECK(finish(x, &body, &result) && result == HTTP_XFER_OK && is_big(&body));
   CHECK(http_xfer_queued(x) == 0);
   http_xfer_close(x);
   free(body.data);

   /* paused after its first run: nothing more until it is not */
   memset(&body, 0, sizeof(body));
   events = 0;
   x = http_xfer_start(&req);
   CHECK(x != NULL);
   for (;;)
   {
      ev = http_xfer_peek(x);
      if (!ev)
      {
         http_xfer_wait(x, has_event, x);
         continue;
      }
      if (ev->kind == HTTP_XFER_BODY)
         break;
      http_xfer_pop(x);
   }
   http_xfer_pause(x, true);
   retro_sleep(100);
   /* what was on its way when it paused */
   drain(x, &body, &result, &events);
   events = 0;
   retro_sleep(400);
   CHECK(!drain(x, &body, &result, &events) && events == 0);
   http_xfer_pause(x, false);
   CHECK(finish(x, &body, &result) && result == HTTP_XFER_OK && is_big(&body));
   http_xfer_close(x);
   free(body.data);
   return 0;
}

static int thread_fails;

static void thread_run(void *arg)
{
   if (((size_t)arg & 1) ? post() : get(NULL, HTTP_XFER_OK))
      thread_fails++;
}

/* several threads at once, as the IOSU services and nlibcurl's workers */
static int threads(void)
{
   sthread_t *t[6];
   size_t i;
   for (i = 0; i < 6; i++)
      t[i] = sthread_create(thread_run, (void*)i);
   for (i = 0; i < 6; i++)
   {
      CHECK(t[i] != NULL);
      sthread_join(t[i]);
   }
   CHECK(thread_fails == 0);
   return 0;
}

/* the transfer thread stops at http_xfer_deinit and comes back */
static int restart(void)
{
   CHECK(get(NULL, HTTP_XFER_OK) == 0);
   http_xfer_deinit();
   CHECK(get(NULL, HTTP_XFER_OK) == 0);
   http_xfer_deinit();
   http_xfer_deinit();
   return 0;
}

static http_xfer_tls_t *tls_for(bool right_ca)
{
   static const unsigned suite = 0x0035; /* TLS_RSA_WITH_AES_256_CBC_SHA */
   http_xfer_tls_t *tls = http_xfer_tls_new();
   buffer_t ca, cert, key;
   bool ok;
   slurp(right_ca ? "ca.der" : "cli.der", &ca); /* cli.der signed nothing the server shows */
   slurp("cli.der", &cert);
   slurp("cli.key.der", &key);
   ok = tls && http_xfer_tls_add_trust(tls, ca.data, ca.len)
      && http_xfer_tls_set_client(tls, cert.data, cert.len, key.data, key.len)
      && http_xfer_tls_set_suites(tls, &suite, 1);
   free(ca.data);
   free(cert.data);
   free(key.data);
   if (!ok)
   {
      http_xfer_tls_free(tls);
      return NULL;
   }
   return tls;
}

int main(int argc, char **argv)
{
   const char *t;
   int r = 2;
   if (argc != 5)
      return 2;
   t = argv[1];
   http_port = atoi(argv[2]);
   https_port = atoi(argv[3]);
   dir = argv[4];
   http_xfer_init(); /* as retro_init() does */
   if (!strcmp(t, "get"))
      r = get(NULL, HTTP_XFER_OK);
   else if (!strcmp(t, "post"))
      r = post();
   else if (!strcmp(t, "form"))
      r = form();
   else if (!strcmp(t, "404"))
      r = not_found();
   else if (!strcmp(t, "timeout"))
      r = timeout();
   else if (!strcmp(t, "abort"))
      r = abort_close();
   else if (!strcmp(t, "backpressure"))
      r = backpressure();
   else if (!strcmp(t, "threads"))
      r = threads();
   else if (!strcmp(t, "restart"))
      r = restart();
   else if (!strcmp(t, "tls"))
   {
      http_xfer_tls_t *tls = tls_for(true);
      r = tls ? get(tls, HTTP_XFER_OK) : 1;
   }
   else if (!strcmp(t, "tls-refused"))
   {
      http_xfer_tls_t *tls = tls_for(false);
      r = tls ? get(tls, HTTP_XFER_TLS) : 1;
   }
   http_xfer_deinit();
   return r;
}
