/* HTTP transfers on a transfer thread of their own (http_xfer.h) */

/* net_compat.h names getaddrinfo's struct, which strict C hides */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200112L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <retro_atomic.h>
#include <rthreads/rthreads.h>
#include <rthreads/retro_eventcount.h>
#include <features/features_cpu.h>
#include <net/net_compat.h>
#include <net/net_http.h>
#include <net/net_socket_ssl.h>
#include <crypto/pk.h>
#include <crypto/x509.h>

#include "http_xfer.h"

/* net_socket_ssl_retro.c reports refused certificates through these */
void ssl_socket_log_verify_fail(int mode_required, const char *domain, const char *info)
{
   (void)mode_required;
   (void)domain;
   (void)info;
}

void ssl_socket_log_verify_disabled(const char *domain)
{
   (void)domain;
}

/* the transfer thread stops reading a transfer whose caller has this
 * much unread */
#define MAX_QUEUED (1 << 20)

/* --- TLS settings ---------------------------------------------------------- */

struct http_xfer_tls
{
   uint8_t **trust;
   size_t *trust_len;
   unsigned trust_count;
   uint8_t *cert;
   size_t cert_len;
   uint8_t *key_der; /* the parsed key points into it */
   struct rsa_private_key key;
   bool has_key;
   unsigned *suites;
   unsigned suite_count;
   bool verify_peer;
   bool verify_host;
   /* what net_http is given, pointing into the above */
   const uint8_t *chain;
   struct ssl_socket_retro_config cfg;
};

http_xfer_tls_t *http_xfer_tls_new(void)
{
   http_xfer_tls_t *tls = (http_xfer_tls_t*)calloc(1, sizeof(*tls));
   if (tls)
   {
      tls->verify_peer = true;
      tls->verify_host = true;
   }
   return tls;
}

void http_xfer_tls_free(http_xfer_tls_t *tls)
{
   unsigned i;
   if (!tls)
      return;
   for (i = 0; i < tls->trust_count; i++)
      free(tls->trust[i]);
   free(tls->trust);
   free(tls->trust_len);
   free(tls->cert);
   free(tls->key_der);
   free(tls->suites);
   free(tls);
}

static uint8_t *copy_of(const void *data, size_t len)
{
   uint8_t *c = (uint8_t*)malloc(len ? len : 1);
   if (c && len)
      memcpy(c, data, len);
   return c;
}

bool http_xfer_tls_add_trust(http_xfer_tls_t *tls, const void *der, size_t len)
{
   uint8_t **trust;
   size_t *lens;
   uint8_t *c;
   if (!tls || !der || !len)
      return false;
   trust = (uint8_t**)realloc(tls->trust, (tls->trust_count + 1) * sizeof(*trust));
   if (!trust)
      return false;
   tls->trust = trust;
   lens = (size_t*)realloc(tls->trust_len, (tls->trust_count + 1) * sizeof(*lens));
   if (!lens)
      return false;
   tls->trust_len = lens;
   c = copy_of(der, len);
   if (!c)
      return false;
   tls->trust[tls->trust_count] = c;
   tls->trust_len[tls->trust_count] = len;
   tls->trust_count++;
   return true;
}

bool http_xfer_tls_set_client(http_xfer_tls_t *tls, const void *cert_der,
      size_t cert_len, const void *key_der, size_t key_len)
{
   uint8_t *cert, *key;
   if (!tls || !cert_der || !cert_len || !key_der || !key_len)
      return false;
   cert = copy_of(cert_der, cert_len);
   key = copy_of(key_der, key_len);
   if (!cert || !key)
   {
      free(cert);
      free(key);
      return false;
   }
   free(tls->cert);
   free(tls->key_der);
   tls->cert = cert;
   tls->cert_len = cert_len;
   tls->key_der = key;
   tls->has_key = x509_rsa_private_key_parse(&tls->key, key, key_len) == 0;
   return tls->has_key;
}

bool http_xfer_tls_set_suites(http_xfer_tls_t *tls, const unsigned *suites, size_t count)
{
   unsigned *s;
   if (!tls)
      return false;
   s = count ? (unsigned*)malloc(count * sizeof(*s)) : NULL;
   if (count && !s)
      return false;
   if (count)
      memcpy(s, suites, count * sizeof(*s));
   free(tls->suites);
   tls->suites = s;
   tls->suite_count = (unsigned)count;
   return true;
}

void http_xfer_tls_set_verify(http_xfer_tls_t *tls, bool peer, bool host)
{
   if (!tls)
      return;
   tls->verify_peer = peer;
   tls->verify_host = host;
}

void http_xfer_tls_get_verify(const http_xfer_tls_t *tls, bool *peer, bool *host)
{
   *peer = tls->verify_peer;
   *host = tls->verify_host;
}

static const struct ssl_socket_retro_config *tls_config(http_xfer_tls_t *tls)
{
   memset(&tls->cfg, 0, sizeof(tls->cfg));
   if (tls->trust_count)
   {
      tls->cfg.trust = (const uint8_t *const*)tls->trust;
      tls->cfg.trust_lens = tls->trust_len;
      tls->cfg.trust_count = tls->trust_count;
   }
   if (tls->has_key)
   {
      tls->chain = tls->cert;
      tls->cfg.chain = &tls->chain;
      tls->cfg.chain_lens = &tls->cert_len;
      tls->cfg.chain_count = 1;
      tls->cfg.key = &tls->key;
   }
   if (tls->suite_count)
   {
      tls->cfg.suites = tls->suites;
      tls->cfg.suite_count = tls->suite_count;
   }
   if (!tls->verify_peer)
      tls->cfg.flags |= SSL_RETRO_NO_VERIFY;
   if (!tls->verify_host)
      tls->cfg.flags |= SSL_RETRO_NO_HOSTNAME;
   return &tls->cfg;
}

/* --- transfers ------------------------------------------------------------- */

/* an event in its queue */
typedef struct node
{
   retro_atomic_ptr_t next;
   http_xfer_event_t ev; /* its data runs on past the node */
} node_t;

/* One request: started by its caller, run by the transfer thread, its
 * results coming back through the queue. Two owners - the caller and the
 * transfer thread - and the last to let go frees it. */
struct http_xfer
{
   retro_atomic_int_t refs;
   http_xfer_t *next_submitted; /* the submission stack's */
   http_xfer_t *next_active;    /* the transfer thread's list */
   /* the request, its strings and body in the allocation after it */
   const char *url;
   const char *method;
   const char *headers;
   const char *content_type;
   const char *user_agent;
   const uint8_t *body;
   size_t body_len;
   bool has_body;
   http_xfer_tls_t *tls;
   retro_time_t deadline;     /* 0 for none */
   /* caller to transfer thread */
   retro_atomic_int_t abandoned;
   retro_atomic_int_t paused;
   /* Transfer thread to caller: a single-producer queue, its head (a stub
    * node, the last popped) the caller's and its tail the transfer
    * thread's */
   node_t *head;
   node_t *tail;
   retro_atomic_int_t posted;
   retro_atomic_int_t queued;  /* body octets not yet popped */
   retro_eventcount_t ec;      /* the caller sleeps here */
   /* the transfer thread's */
   struct http_t *http;
   bool headers_sent;
   bool finished;
};

static retro_atomic_ptr_t submitted;
static retro_eventcount_t wake;       /* the transfer thread sleeps here */
static retro_atomic_int_t stopping;
static bool running;
static sthread_t *thread;

static void wake_transfer_thread(void)
{
   retro_eventcount_notify(&wake);
}

static node_t *node_new(int kind, size_t len)
{
   node_t *n = (node_t*)malloc(sizeof(*n) + len);
   if (!n)
      return NULL;
   retro_atomic_ptr_init(&n->next, NULL);
   n->ev.kind = kind;
   n->ev.result = HTTP_XFER_OK;
   n->ev.status = 0;
   n->ev.len = len;
   n->ev.data[len] = '\0';
   return n;
}

/* Each transfer's outcome node, made with it so that finishing it never
 * fails for want of memory: in the same allocation, after it. */
#define DONE_NODE(x) ((node_t*)((char*)(x) + sizeof(http_xfer_t)))

static void node_free(http_xfer_t *x, node_t *n)
{
   if (n != DONE_NODE(x))
      free(n);
}

static void xfer_free(http_xfer_t *x)
{
   node_t *n = x->head;
   while (n)
   {
      node_t *next = (node_t*)retro_atomic_load_relaxed_ptr(&n->next);
      node_free(x, n);
      n = next;
   }
   retro_eventcount_free(&x->ec);
   http_xfer_tls_free(x->tls);
   free(x);
}

static void xfer_release(http_xfer_t *x)
{
   if (retro_atomic_fetch_sub_int(&x->refs, 1) == 1)
      xfer_free(x);
}

/* the transfer thread's side */
static void post(http_xfer_t *x, node_t *n)
{
   if (n->ev.kind == HTTP_XFER_BODY)
      retro_atomic_fetch_add_int(&x->queued, (int)n->ev.len);
   retro_atomic_store_release_ptr(&x->tail->next, n);
   x->tail = n;
   retro_atomic_fetch_add_int(&x->posted, 1);
   retro_eventcount_notify(&x->ec);
}

const http_xfer_event_t *http_xfer_peek(http_xfer_t *x)
{
   node_t *n = (node_t*)retro_atomic_load_acquire_ptr(&x->head->next);
   return n ? &n->ev : NULL;
}

void http_xfer_pop(http_xfer_t *x)
{
   node_t *n = (node_t*)retro_atomic_load_acquire_ptr(&x->head->next);
   if (!n)
      return;
   if (n->ev.kind == HTTP_XFER_BODY)
   {
      int was = retro_atomic_fetch_sub_int(&x->queued, (int)n->ev.len);
      /* the transfer thread stopped reading for this one */
      if (was >= MAX_QUEUED && was - (int)n->ev.len < MAX_QUEUED)
         wake_transfer_thread();
   }
   node_free(x, x->head);
   x->head = n; /* the stub now */
}

static int failure(struct http_t *h)
{
   int code = 0;
   const char *stage = net_http_failure(h, &code);
   if (!stage)
      return HTTP_XFER_RECV;
   if (strstr(stage, "dns"))
      return HTTP_XFER_RESOLVE;
   if (strstr(stage, "ssl_config"))
      return HTTP_XFER_TLS_SETUP;
   if (strstr(stage, "ssl"))
      return HTTP_XFER_TLS;
   if (strstr(stage, "connect") || strstr(stage, "socket_create"))
      return HTTP_XFER_CONNECT;
   if (strstr(stage, "send") || strstr(stage, "source"))
      return HTTP_XFER_SEND;
   if (strstr(stage, "redirect"))
      return HTTP_XFER_REDIRECTS;
   return HTTP_XFER_RECV;
}

/* the status line and the header lines, once */
static void post_headers(http_xfer_t *x)
{
   int status;
   char *block;
   char line0[32];
   const char *l;
   size_t len, at;
   node_t *n;
   if (x->headers_sent)
      return;
   x->headers_sent = true;
   status = net_http_status(x->http);
   block = net_http_headers_take(x->http, true);
   if (status <= 0 && !block)
      return;
   sprintf(line0, "HTTP/1.1 %d\r\n", status);
   len = strlen(line0) + 2;
   for (l = net_http_header_next(block, NULL); l; l = net_http_header_next(block, l))
      len += strlen(l) + 2;
   n = node_new(HTTP_XFER_HEADERS, len);
   if (n)
   {
      n->ev.status = status;
      at = strlen(line0);
      memcpy(n->ev.data, line0, at);
      for (l = net_http_header_next(block, NULL); l; l = net_http_header_next(block, l))
      {
         size_t ll = strlen(l);
         memcpy(n->ev.data + at, l, ll);
         memcpy(n->ev.data + at + ll, "\r\n", 2);
         at += ll + 2;
      }
      memcpy(n->ev.data + at, "\r\n", 2);
      post(x, n);
   }
   free(block);
}

static bool sink(void *userdata, const void *data, size_t len)
{
   http_xfer_t *x = (http_xfer_t*)userdata;
   node_t *n;
   if (retro_atomic_load_relaxed_int(&x->abandoned))
      return false;
   post_headers(x);
   n = node_new(HTTP_XFER_BODY, len);
   if (!n)
      return false;
   memcpy(n->ev.data, data, len);
   post(x, n);
   return true;
}

/* the outcome, the last of what comes back, in the node kept for it */
static void finish(http_xfer_t *x, int result, node_t *done)
{
   int status = 0;
   if (x->http)
   {
      post_headers(x);
      status = net_http_status(x->http);
      net_http_delete(x->http);
      x->http = NULL;
   }
   done->ev.result = result;
   done->ev.status = status > 0 ? status : 0;
   post(x, done);
   x->finished = true;
}

static bool start(http_xfer_t *x)
{
   struct http_connection_t *conn = net_http_connection_new(x->url, x->method, NULL);
   if (!conn)
      return false;
   if (x->user_agent)
      net_http_connection_set_user_agent(conn, x->user_agent);
   if (x->headers)
      net_http_connection_set_headers(conn, x->headers);
   if (x->has_body)
      net_http_connection_set_content(conn, x->content_type, x->body_len, x->body);
   if (x->tls)
      net_http_connection_set_tls(conn, tls_config(x->tls));
   net_http_connection_set_sink(conn, sink, x);
   if (!net_http_connection_iterate(conn) || !net_http_connection_done(conn)
         || !(x->http = net_http_new(conn)))
   {
      net_http_connection_free(conn);
      return false;
   }
   net_http_connection_free(conn);
   return true;
}

static bool reading(http_xfer_t *x)
{
   return !retro_atomic_load_relaxed_int(&x->paused)
      && retro_atomic_load_relaxed_int(&x->queued) < MAX_QUEUED;
}

static void transfer_thread(void *arg)
{
   http_xfer_t *active = NULL;
   unsigned active_count = 0;
   (void)arg;
   net_http_init();
   for (;;)
   {
      int key = retro_eventcount_prepare_wait(&wake);
      http_xfer_t *list, *fresh = NULL;
      http_xfer_t **link;
      http_xfer_t *first;
      bool moved = false;
      retro_time_t now;
      if (retro_atomic_load_acquire_int(&stopping))
      {
         retro_eventcount_cancel_wait(&wake);
         break;
      }

      /* the new ones, oldest first: the stack reversed */
      list = (http_xfer_t*)retro_atomic_exchange_ptr(&submitted, NULL);
      while (list)
      {
         http_xfer_t *next = list->next_submitted;
         list->next_submitted = fresh;
         fresh = list;
         list = next;
      }
      while (fresh)
      {
         http_xfer_t *x = fresh;
         fresh = x->next_submitted;
         if (retro_atomic_load_relaxed_int(&x->abandoned))
            finish(x, HTTP_XFER_ABORTED, DONE_NODE(x));
         else if (!start(x))
            finish(x, HTTP_XFER_BAD_URL, DONE_NODE(x));
         if (x->finished)
            xfer_release(x);
         else
         {
            x->next_active = active;
            active = x;
            active_count++;
         }
      }

      now = cpu_features_get_time_usec();
      for (link = &active; *link;)
      {
         http_xfer_t *x = *link;
         if (retro_atomic_load_relaxed_int(&x->abandoned))
            finish(x, HTTP_XFER_ABORTED, DONE_NODE(x));
         else if (x->deadline && now >= x->deadline)
            finish(x, HTTP_XFER_TIMEOUT, DONE_NODE(x));
         else if (reading(x))
         {
            size_t pos = 0, total = 0;
            int before = retro_atomic_load_relaxed_int(&x->posted);
            if (net_http_update(x->http, &pos, &total))
            {
               int status = net_http_status(x->http);
               finish(x, status > 0 ? HTTP_XFER_OK : failure(x->http), DONE_NODE(x));
            }
            if (retro_atomic_load_relaxed_int(&x->posted) != before)
               moved = true;
         }
         /* else its caller is not reading: neither is this */
         if (x->finished)
         {
            moved = true;
            *link = x->next_active;
            active_count--;
            xfer_release(x);
         }
         else
            link = &x->next_active;
      }

      if (moved)
      {
         retro_eventcount_cancel_wait(&wake);
         continue;
      }
      /* Wait on the sockets. net_http_wait() takes one transfer; with
       * more than one, the wait is short so that each gets its turn, and
       * new work waits at most that long. With none to read, asleep
       * until handed something. */
      for (first = active; first && !reading(first); first = first->next_active);
      if (!first)
         retro_eventcount_commit_wait(&wake, key);
      else
      {
         retro_eventcount_cancel_wait(&wake);
         net_http_wait(first->http, active_count == 1 ? 50 : 5);
      }
   }
   while (active)
   {
      http_xfer_t *x = active;
      active = x->next_active;
      finish(x, HTTP_XFER_ABORTED, DONE_NODE(x));
      xfer_release(x);
   }
   net_http_deinit();
}

bool http_xfer_init(void)
{
   if (running)
      return true;
   if (!retro_eventcount_init(&wake))
      return false;
   network_init();
   retro_atomic_int_init(&stopping, 0);
   retro_atomic_ptr_init(&submitted, NULL);
   thread = sthread_create(transfer_thread, NULL);
   if (!thread)
   {
      retro_eventcount_free(&wake);
      return false;
   }
   running = true;
   return true;
}

void http_xfer_deinit(void)
{
   if (!running)
      return;
   retro_atomic_store_release_int(&stopping, 1);
   wake_transfer_thread();
   sthread_join(thread);
   thread = NULL;
   retro_eventcount_free(&wake);
   running = false;
}

/* a copy of s at *at, which moves on past it; NULL stays NULL */
static const char *put_str(char **at, const char *s)
{
   char *c;
   size_t n;
   if (!s)
      return NULL;
   n = strlen(s) + 1;
   c = *at;
   memcpy(c, s, n);
   *at += n;
   return c;
}

static size_t str_size(const char *s)
{
   return s ? strlen(s) + 1 : 0;
}

http_xfer_t *http_xfer_start(const http_xfer_request_t *req)
{
   http_xfer_t *x, *head;
   node_t *stub;
   size_t extra;
   char *at;
   if (!req || !req->url || (!running && !http_xfer_init()))
   {
      if (req)
         http_xfer_tls_free(req->tls);
      return NULL;
   }
   /* the transfer, its outcome node, then the request's octets */
   extra = sizeof(node_t) + str_size(req->url) + str_size(req->method ? req->method : "GET")
      + str_size(req->headers) + str_size(req->content_type) + str_size(req->user_agent)
      + (req->has_body ? req->body_len : 0);
   x = (http_xfer_t*)calloc(1, sizeof(*x) + extra);
   stub = node_new(0, 0);
   if (!x || !stub || !retro_eventcount_init(&x->ec))
   {
      free(x);
      free(stub);
      http_xfer_tls_free(req->tls);
      return NULL;
   }
   retro_atomic_ptr_init(&DONE_NODE(x)->next, NULL);
   DONE_NODE(x)->ev.kind = HTTP_XFER_DONE;
   DONE_NODE(x)->ev.len = 0;
   DONE_NODE(x)->ev.data[0] = '\0';
   at = (char*)DONE_NODE(x) + sizeof(node_t);
   x->url = put_str(&at, req->url);
   x->method = put_str(&at, req->method ? req->method : "GET");
   x->headers = put_str(&at, req->headers);
   x->content_type = put_str(&at, req->content_type);
   x->user_agent = put_str(&at, req->user_agent);
   if (req->has_body)
   {
      x->has_body = true;
      x->body_len = req->body_len;
      if (req->body_len)
         memcpy(at, req->body, req->body_len);
      x->body = (const uint8_t*)at;
   }
   x->tls = req->tls;
   if (req->timeout_ms)
      x->deadline = cpu_features_get_time_usec() + (retro_time_t)req->timeout_ms * 1000;
   retro_atomic_int_init(&x->refs, 2);
   retro_atomic_int_init(&x->abandoned, 0);
   retro_atomic_int_init(&x->paused, 0);
   retro_atomic_int_init(&x->posted, 0);
   retro_atomic_int_init(&x->queued, 0);
   x->head = x->tail = stub;

   /* onto the stack the transfer thread takes from */
   for (;;)
   {
      head = (http_xfer_t*)retro_atomic_load_relaxed_ptr(&submitted);
      x->next_submitted = head;
      if (retro_atomic_cas_ptr(&submitted, head, x))
         break;
   }
   wake_transfer_thread();
   return x;
}

void http_xfer_close(http_xfer_t *x)
{
   if (!x)
      return;
   retro_atomic_store_release_int(&x->abandoned, 1);
   wake_transfer_thread();
   xfer_release(x);
}

size_t http_xfer_queued(http_xfer_t *x)
{
   return (size_t)retro_atomic_load_acquire_int(&x->queued);
}

void http_xfer_pause(http_xfer_t *x, bool paused)
{
   retro_atomic_store_release_int(&x->paused, paused ? 1 : 0);
   if (!paused)
      wake_transfer_thread();
}

void http_xfer_wait(http_xfer_t *x, bool (*ready)(void *ctx), void *ctx)
{
   int key = retro_eventcount_prepare_wait(&x->ec);
   if (ready && ready(ctx))
      retro_eventcount_cancel_wait(&x->ec);
   else
      retro_eventcount_commit_wait(&x->ec, key);
}

void http_xfer_wake(http_xfer_t *x)
{
   retro_eventcount_notify(&x->ec);
}

static bool has_event(void *ctx)
{
   return http_xfer_peek((http_xfer_t*)ctx) != NULL;
}

int http_xfer_perform(const http_xfer_request_t *req,
      bool (*on_body)(void *ctx, const void *data, size_t len), void *ctx,
      int *status)
{
   http_xfer_t *x = http_xfer_start(req);
   int result = HTTP_XFER_MEMORY;
   if (status)
      *status = 0;
   if (!x)
      return result;
   for (;;)
   {
      const http_xfer_event_t *ev = http_xfer_peek(x);
      if (!ev)
      {
         http_xfer_wait(x, has_event, x);
         continue;
      }
      if (ev->kind == HTTP_XFER_HEADERS && status)
         *status = ev->status;
      else if (ev->kind == HTTP_XFER_BODY && on_body && !on_body(ctx, ev->data, ev->len))
      {
         result = HTTP_XFER_ABORTED;
         break;
      }
      else if (ev->kind == HTTP_XFER_DONE)
      {
         result = ev->result;
         if (status && ev->status > 0)
            *status = ev->status;
         break;
      }
      http_xfer_pop(x);
   }
   http_xfer_close(x);
   return result;
}

/* --- multipart/form-data ----------------------------------------------------- */

static bool contains(const void *data, size_t len, const char *s, size_t n)
{
   const char *p = (const char*)data;
   size_t i;
   if (n > len)
      return false;
   for (i = 0; i + n <= len; i++)
      if (p[i] == s[0] && memcmp(p + i, s, n) == 0)
         return true;
   return false;
}

void *http_xfer_multipart(const http_xfer_part_t *parts, size_t count,
      size_t *len, char **content_type)
{
   static const char base[] = "------------------------httpxfer";
   static const char disposition[] = "Content-Disposition: form-data; name=\"";
   char *boundary, *out, *at;
   size_t blen = sizeof(base) - 1, total, i;
   bool clash = true;
   *len = 0;
   *content_type = NULL;
   /* the boundary, grown by an x until no part contains it */
   boundary = (char*)malloc(blen + 1);
   if (!boundary)
      return NULL;
   memcpy(boundary, base, blen + 1);
   while (clash)
   {
      clash = false;
      for (i = 0; i < count && !clash; i++)
         clash = parts[i].data && contains(parts[i].data, parts[i].len, boundary, blen);
      if (clash)
      {
         char *grown = (char*)realloc(boundary, blen + 2);
         if (!grown)
         {
            free(boundary);
            return NULL;
         }
         boundary = grown;
         boundary[blen++] = 'x';
         boundary[blen] = '\0';
      }
   }
   total = 2 + blen + 4; /* the closing "--boundary--\r\n" */
   for (i = 0; i < count; i++)
   {
      total += 2 + blen + 2 + sizeof(disposition) - 1 + strlen(parts[i].name) + 3;
      if (parts[i].content_type)
         total += 14 + strlen(parts[i].content_type) + 2;
      total += 2 + parts[i].len + 2;
   }
   out = (char*)malloc(total + 1);
   *content_type = (char*)malloc(30 + blen + 1);
   if (!out || !*content_type)
   {
      free(out);
      free(*content_type);
      *content_type = NULL;
      free(boundary);
      return NULL;
   }
   at = out;
#define PUT(s, n) do { memcpy(at, (s), (n)); at += (n); } while (0)
   for (i = 0; i < count; i++)
   {
      PUT("--", 2);
      PUT(boundary, blen);
      PUT("\r\n", 2);
      PUT(disposition, sizeof(disposition) - 1);
      PUT(parts[i].name, strlen(parts[i].name));
      PUT("\"\r\n", 3);
      if (parts[i].content_type)
      {
         PUT("Content-Type: ", 14);
         PUT(parts[i].content_type, strlen(parts[i].content_type));
         PUT("\r\n", 2);
      }
      PUT("\r\n", 2);
      if (parts[i].len)
         PUT(parts[i].data, parts[i].len);
      PUT("\r\n", 2);
   }
   PUT("--", 2);
   PUT(boundary, blen);
   PUT("--\r\n", 4);
#undef PUT
   *len = (size_t)(at - out);
   strcpy(*content_type, "multipart/form-data; boundary=");
   strcat(*content_type, boundary);
   free(boundary);
   return out;
}
