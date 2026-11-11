#ifndef COMMON_HTTP_XFER_H
#define COMMON_HTTP_XFER_H

/* HTTP and HTTPS transfers over libretro-common's net_http and its TLS
 * client, run on one transfer thread of their own: net_http's DNS cache
 * and connection pool belong to one thread, so every transfer runs there.
 *
 * A caller starts a transfer (http_xfer_start, which copies the request)
 * and takes what comes back - the response's header block, runs of its
 * body, the outcome - from the transfer's own queue, on its own thread.
 * Nothing takes a lock: a transfer is handed over through a lock-free
 * stack, its results come back through a single-producer queue, and a
 * side with nothing to do sleeps on an eventcount. The transfer thread
 * stops reading a transfer its caller has paused or has fallen 1 MiB
 * behind on.
 *
 * http_xfer_perform is the one-call form, for a request whose answer the
 * caller waits for. C89. */

#include <stddef.h>
#include <stdint.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* What a transfer's TLS connection is set up with, in place of the
 * defaults (the built-in trust bundle, no client certificate, the default
 * suites, the peer and its name checked). Certificates and keys are DER,
 * and copied. */
typedef struct http_xfer_tls http_xfer_tls_t;

http_xfer_tls_t *http_xfer_tls_new(void);
void http_xfer_tls_free(http_xfer_tls_t *tls);
/* a trust anchor; any given replace the built-in bundle */
bool http_xfer_tls_add_trust(http_xfer_tls_t *tls, const void *der, size_t len);
/* the client certificate and its RSA key (PKCS#1 or PKCS#8) */
bool http_xfer_tls_set_client(http_xfer_tls_t *tls, const void *cert_der,
      size_t cert_len, const void *key_der, size_t key_len);
/* the suites to offer, by IANA number, most preferred first */
bool http_xfer_tls_set_suites(http_xfer_tls_t *tls, const unsigned *suites, size_t count);
void http_xfer_tls_set_verify(http_xfer_tls_t *tls, bool peer, bool host);
void http_xfer_tls_get_verify(const http_xfer_tls_t *tls, bool *peer, bool *host);

typedef struct http_xfer_request
{
   const char *url;
   const char *method;       /* NULL for GET */
   /* "Name: value\r\n" lines, or NULL; net_http writes Host and
    * Content-Length itself */
   const char *headers;
   const char *content_type; /* the body's; NULL for net_http's default */
   const char *user_agent;   /* NULL for net_http's */
   const void *body;
   size_t body_len;
   bool has_body;            /* a body, even of no octets */
   /* NULL for the defaults; taken by the transfer, and freed with it,
    * also when it does not start */
   http_xfer_tls_t *tls;
   unsigned timeout_ms;      /* of all of it; 0 for none */
} http_xfer_request_t;

/* how a transfer ended */
#define HTTP_XFER_OK        0
#define HTTP_XFER_BAD_URL   1  /* or a request net_http would not make */
#define HTTP_XFER_RESOLVE   2
#define HTTP_XFER_CONNECT   3
#define HTTP_XFER_TLS       4  /* the handshake, or the peer refused */
#define HTTP_XFER_TLS_SETUP 5  /* the TLS settings could not be used */
#define HTTP_XFER_SEND      6
#define HTTP_XFER_RECV      7
#define HTTP_XFER_REDIRECTS 8
#define HTTP_XFER_TIMEOUT   9
#define HTTP_XFER_ABORTED   10 /* closed before it ended, or stopped */
#define HTTP_XFER_MEMORY    11

/* what comes back */
#define HTTP_XFER_HEADERS   1  /* the status line and header lines, each
                                * with its CRLF, then an empty one */
#define HTTP_XFER_BODY      2  /* a run of the body */
#define HTTP_XFER_DONE      3  /* the outcome: always the last */

typedef struct http_xfer_event
{
   int kind;                 /* HTTP_XFER_HEADERS, _BODY, _DONE */
   int result;               /* _DONE: HTTP_XFER_* */
   int status;               /* _HEADERS, _DONE: the HTTP status, or 0 */
   size_t len;
   char data[1];             /* len octets, then a 0 */
} http_xfer_event_t;

typedef struct http_xfer http_xfer_t;

/* The transfer thread, started and stopped; call http_xfer_deinit when
 * nothing transfers. Neither may run while the other does, nor while a
 * transfer starts. http_xfer_start starts the thread when no one has. */
bool http_xfer_init(void);
void http_xfer_deinit(void);

/* The request, copied and handed to the transfer thread; NULL for no
 * memory or no thread. */
http_xfer_t *http_xfer_start(const http_xfer_request_t *req);
/* Ends it if it has not ended (HTTP_XFER_ABORTED), and lets it go. */
void http_xfer_close(http_xfer_t *x);

/* The next of what has come back, or NULL; let go of with
 * http_xfer_pop. Of one thread at a time: the caller's. */
const http_xfer_event_t *http_xfer_peek(http_xfer_t *x);
void http_xfer_pop(http_xfer_t *x);

/* body octets come back and not yet popped */
size_t http_xfer_queued(http_xfer_t *x);

/* Paused: the transfer thread reads no more of it until it is not. */
void http_xfer_pause(http_xfer_t *x, bool paused);

/* Asleep until something comes back, or http_xfer_wake: unless ready
 * (with ctx) says, after the sleep is set up, that there is no need. */
void http_xfer_wait(http_xfer_t *x, bool (*ready)(void *ctx), void *ctx);
void http_xfer_wake(http_xfer_t *x);

/* One request, waited for: the body to on_body (false from it stops the
 * transfer, HTTP_XFER_ABORTED), the status into *status. HTTP_XFER_*. */
int http_xfer_perform(const http_xfer_request_t *req,
      bool (*on_body)(void *ctx, const void *data, size_t len), void *ctx,
      int *status);

/* A multipart/form-data body of count parts: malloc'd, its length into
 * *len, and the Content-Type naming its boundary - one no part contains -
 * into *content_type (malloc'd too). NULL for no memory. */
typedef struct http_xfer_part
{
   const char *name;
   const void *data;
   size_t len;
   const char *content_type; /* NULL for none */
} http_xfer_part_t;

void *http_xfer_multipart(const http_xfer_part_t *parts, size_t count,
      size_t *len, char **content_type);

RETRO_END_DECLS

#endif
