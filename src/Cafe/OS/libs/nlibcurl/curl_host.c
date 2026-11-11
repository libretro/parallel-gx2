/* libcurl's behaviour, for nlibcurl, over the core's HTTP transfers
 * (curl_host.h) */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <retro_atomic.h>
#include <features/features_cpu.h>

#include "Common/http_xfer.h"
#include "curl_host.h"

typedef struct easy
{
   char *url;
   char *custom_request;
   char *user_agent;
   char *headers;          /* the slist's lines, each with its CRLF */
   const void *post_fields;
   curl_off_t post_field_size;
   curl_off_t in_file_size;
   bool post;
   bool no_body;
   bool no_progress;
   bool verify_peer;
   bool verify_host;
   long timeout_ms;
   curl_write_callback write_fn;
   void *write_data;
   curl_write_callback header_fn;
   void *header_data;
   curl_read_callback read_fn;
   void *read_data;
   curl_progress_callback progress_fn;
   void *progress_data;
   curl_ssl_ctx_callback ssl_ctx_fn;
   void *ssl_ctx_data;
   /* the request in flight */
   http_xfer_t *xfer;
   bool in_multi;
   bool done;
   CURLcode result;
   retro_atomic_int_t pause_bits; /* curlh_easy_pause may come from another thread */
   /* what curlh_easy_getinfo reports */
   long status;
   double downloaded;
   double uploaded;
   double content_length;
   double seconds;
   retro_time_t started;
   char *content_type;
} easy_t;

typedef struct multi
{
   easy_t **handles;
   bool *started;
   bool *reported;         /* its CURLMSG_DONE is out */
   size_t count;
   size_t capacity;
   CURLMsg *messages;
   size_t message_count;
   size_t message_capacity;
   size_t message_read;
} multi_t;

static char *dup_str(const char *s)
{
   size_t n;
   char *c;
   if (!s)
      return NULL;
   n = strlen(s) + 1;
   c = (char*)malloc(n);
   if (c)
      memcpy(c, s, n);
   return c;
}

static void set_str(char **slot, const char *s)
{
   free(*slot);
   *slot = (s && *s) ? dup_str(s) : NULL;
}

static bool lower_eq(const char *a, const char *b, size_t n)
{
   size_t i;
   for (i = 0; i < n; i++)
   {
      char x = a[i], y = b[i];
      if (x >= 'A' && x <= 'Z')
         x = (char)(x + ('a' - 'A'));
      if (y >= 'A' && y <= 'Z')
         y = (char)(y + ('a' - 'A'));
      if (x != y)
         return false;
   }
   return true;
}

/* line (len octets) is header name's: "Name:" */
static bool header_is(const char *line, size_t len, const char *name)
{
   size_t n = strlen(name);
   return len > n && lower_eq(line, name, n) && line[n] == ':';
}

/* the value of line, a header, without leading blanks or the line end */
static void header_value(const char *line, size_t len, const char **value, size_t *value_len)
{
   const char *v = (const char*)memchr(line, ':', len);
   const char *end = line + len;
   v = v ? v + 1 : end;
   while (v < end && (*v == ' ' || *v == '\t'))
      v++;
   while (end > v && (end[-1] == '\r' || end[-1] == '\n'))
      end--;
   *value = v;
   *value_len = (size_t)(end - v);
}

static CURLcode curl_code(int result)
{
   switch (result)
   {
      case HTTP_XFER_OK:        return CURLE_OK;
      case HTTP_XFER_BAD_URL:   return CURLE_URL_MALFORMAT;
      case HTTP_XFER_RESOLVE:   return CURLE_COULDNT_RESOLVE_HOST;
      case HTTP_XFER_CONNECT:   return CURLE_COULDNT_CONNECT;
      case HTTP_XFER_TLS:       return CURLE_SSL_CONNECT_ERROR;
      case HTTP_XFER_TLS_SETUP: return CURLE_SSL_CERTPROBLEM;
      case HTTP_XFER_SEND:      return CURLE_SEND_ERROR;
      case HTTP_XFER_REDIRECTS: return CURLE_TOO_MANY_REDIRECTS;
      case HTTP_XFER_TIMEOUT:   return CURLE_OPERATION_TIMEDOUT;
      case HTTP_XFER_ABORTED:   return CURLE_ABORTED_BY_CALLBACK;
      case HTTP_XFER_MEMORY:    return CURLE_OUT_OF_MEMORY;
      default:                  return CURLE_RECV_ERROR;
   }
}

/* --- the transfer thread --------------------------------------------------- */

CURLcode curlh_global_init(long flags)
{
   (void)flags;
   return http_xfer_init() ? CURLE_OK : CURLE_FAILED_INIT;
}

void curlh_global_cleanup(void)
{
   http_xfer_deinit();
}

/* --- easy handles ---------------------------------------------------------- */

static void easy_defaults(easy_t *e)
{
   memset(e, 0, sizeof(*e));
   e->post_field_size = -1;
   e->in_file_size = -1;
   e->no_progress = true;
   e->verify_peer = true;
   e->verify_host = true;
   e->content_length = -1;
   retro_atomic_int_init(&e->pause_bits, 0);
}

/* the request in flight given up on */
static void abandon(easy_t *e)
{
   if (e->xfer)
   {
      http_xfer_close(e->xfer);
      e->xfer = NULL;
   }
}

static void easy_clear(easy_t *e)
{
   abandon(e);
   free(e->url);
   free(e->custom_request);
   free(e->user_agent);
   free(e->headers);
   free(e->content_type);
}

CURL *curlh_easy_init(void)
{
   easy_t *e;
   if (curlh_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
      return NULL;
   e = (easy_t*)malloc(sizeof(*e));
   if (e)
      easy_defaults(e);
   return e;
}

void curlh_easy_cleanup(CURL *curl)
{
   if (!curl)
      return;
   easy_clear((easy_t*)curl);
   free(curl);
}

void curlh_easy_reset(CURL *curl)
{
   easy_t *e = (easy_t*)curl;
   bool in_multi;
   if (!e)
      return;
   in_multi = e->in_multi;
   easy_clear(e);
   easy_defaults(e);
   e->in_multi = in_multi;
}

static void set_headers(easy_t *e, const struct curl_slist *list)
{
   const struct curl_slist *l;
   size_t n = 0;
   char *at;
   free(e->headers);
   e->headers = NULL;
   for (l = list; l; l = l->next)
      if (l->data)
         n += strlen(l->data) + 2;
   if (!n)
      return;
   e->headers = (char*)malloc(n + 1);
   if (!e->headers)
      return;
   at = e->headers;
   for (l = list; l; l = l->next)
   {
      size_t ln;
      if (!l->data)
         continue;
      ln = strlen(l->data);
      memcpy(at, l->data, ln);
      memcpy(at + ln, "\r\n", 2);
      at += ln + 2;
   }
   *at = '\0';
}

/* a function, of whichever type: an option's own is cast back to */
typedef void (*any_fn)(void);

CURLcode curlh_easy_setopt(CURL *curl, CURLoption option, ...)
{
   easy_t *e = (easy_t*)curl;
   va_list ap;
   long lv = 0;
   void *pv = NULL;
   any_fn fv = NULL;
   curl_off_t ov = 0;
   int opt = (int)option;
   if (!e)
      return CURLE_BAD_FUNCTION_ARGUMENT;
   va_start(ap, option);
   if (opt >= CURLOPTTYPE_OFF_T && opt < CURLOPTTYPE_BLOB)
      ov = va_arg(ap, curl_off_t);
   else if (opt >= CURLOPTTYPE_FUNCTIONPOINT && opt < CURLOPTTYPE_OFF_T)
      fv = va_arg(ap, any_fn);
   else if (opt >= CURLOPTTYPE_OBJECTPOINT)
      pv = va_arg(ap, void*);
   else
      lv = va_arg(ap, long);
   va_end(ap);

   switch (opt)
   {
      case CURLOPT_URL: set_str(&e->url, (const char*)pv); break;
      case CURLOPT_CUSTOMREQUEST: set_str(&e->custom_request, (const char*)pv); break;
      case CURLOPT_USERAGENT: set_str(&e->user_agent, (const char*)pv); break;
      case CURLOPT_HTTPHEADER: set_headers(e, (const struct curl_slist*)pv); break;
      case CURLOPT_POST:
         e->post = lv != 0;
         if (e->post)
            e->no_body = false;
         break;
      case CURLOPT_HTTPGET:
         if (lv)
         {
            e->post = false;
            e->no_body = false;
         }
         break;
      case CURLOPT_NOBODY: e->no_body = lv != 0; break;
      case CURLOPT_POSTFIELDS: e->post_fields = pv; e->post = true; break;
      case CURLOPT_POSTFIELDSIZE: e->post_field_size = lv; break;
      case CURLOPT_POSTFIELDSIZE_LARGE: e->post_field_size = ov; break;
      case CURLOPT_INFILESIZE: e->in_file_size = lv; break;
      case CURLOPT_INFILESIZE_LARGE: e->in_file_size = ov; break;
      case CURLOPT_TIMEOUT: e->timeout_ms = lv * 1000; break;
      case CURLOPT_TIMEOUT_MS: e->timeout_ms = lv; break;
      case CURLOPT_NOPROGRESS: e->no_progress = lv != 0; break;
      case CURLOPT_SSL_VERIFYPEER: e->verify_peer = lv != 0; break;
      case CURLOPT_SSL_VERIFYHOST: e->verify_host = lv != 0; break;
      case CURLOPT_WRITEFUNCTION: e->write_fn = (curl_write_callback)fv; break;
      case CURLOPT_WRITEDATA: e->write_data = pv; break;
      case CURLOPT_HEADERFUNCTION: e->header_fn = (curl_write_callback)fv; break;
      case CURLOPT_HEADERDATA: e->header_data = pv; break;
      case CURLOPT_READFUNCTION: e->read_fn = (curl_read_callback)fv; break;
      case CURLOPT_READDATA: e->read_data = pv; break;
      case CURLOPT_PROGRESSFUNCTION: e->progress_fn = (curl_progress_callback)fv; break;
      case CURLOPT_PROGRESSDATA: e->progress_data = pv; break;
      case CURLOPT_SSL_CTX_FUNCTION: e->ssl_ctx_fn = (curl_ssl_ctx_callback)fv; break;
      case CURLOPT_SSL_CTX_DATA: e->ssl_ctx_data = pv; break;
      default: break; /* accepted, without effect */
   }
   return CURLE_OK;
}

/* The request's headers as net_http takes them: net_http writes Host,
 * Content-Length, Accept-Encoding and User-Agent itself, and the
 * Content-Type goes in its own slot so that net_http adds no default.
 * "Name;" sends an empty header in libcurl; it is dropped. */
static char *request_headers(const easy_t *e, char **user_agent, char **content_type)
{
   const char *l = e->headers;
   char *out, *at;
   if (!l)
      return NULL;
   out = (char*)malloc(strlen(l) + 1);
   if (!out)
      return NULL;
   at = out;
   while (*l)
   {
      const char *end = strstr(l, "\r\n");
      size_t len = end ? (size_t)(end - l) : strlen(l);
      const char *v;
      size_t vlen;
      if (header_is(l, len, "Host") || header_is(l, len, "Content-Length")
            || header_is(l, len, "Accept-Encoding") || header_is(l, len, "Expect")
            || header_is(l, len, "Transfer-Encoding") || !memchr(l, ':', len))
      {
         /* dropped */
      }
      else if (header_is(l, len, "User-Agent") || header_is(l, len, "Content-Type"))
      {
         char **slot = header_is(l, len, "User-Agent") ? user_agent : content_type;
         header_value(l, len, &v, &vlen);
         free(*slot);
         *slot = (char*)malloc(vlen + 1);
         if (*slot)
         {
            memcpy(*slot, v, vlen);
            (*slot)[vlen] = '\0';
         }
      }
      else
      {
         memcpy(at, l, len);
         memcpy(at + len, "\r\n", 2);
         at += len + 2;
      }
      l += len + (end ? 2 : 0);
   }
   *at = '\0';
   return out;
}

/* a body from the read callback, read whole, 16 KiB at a time */
#define READ_STEP 16384
static CURLcode read_body(easy_t *e, uint8_t **body, size_t *len)
{
   char *buf = (char*)malloc(READ_STEP);
   size_t have = 0, cap = 0;
   uint8_t *out = NULL;
   if (!buf)
      return CURLE_OUT_OF_MEMORY;
   for (;;)
   {
      size_t want = READ_STEP, got;
      if (e->in_file_size >= 0)
      {
         if ((curl_off_t)have >= e->in_file_size)
            break;
         if ((curl_off_t)want > e->in_file_size - (curl_off_t)have)
            want = (size_t)(e->in_file_size - (curl_off_t)have);
      }
      got = e->read_fn(buf, 1, want, e->read_data);
      if (got == CURL_READFUNC_ABORT)
      {
         free(out);
         free(buf);
         return CURLE_ABORTED_BY_CALLBACK;
      }
      if (got == 0 || got > want)
         break;
      if (have + got > cap)
      {
         uint8_t *grown;
         cap = (have + got) * 2;
         grown = (uint8_t*)realloc(out, cap);
         if (!grown)
         {
            free(out);
            free(buf);
            return CURLE_OUT_OF_MEMORY;
         }
         out = grown;
      }
      memcpy(out + have, buf, got);
      have += got;
   }
   free(buf);
   *body = out;
   *len = have;
   return CURLE_OK;
}

/* the transfer built and handed over */
static CURLcode begin(CURL *handle, easy_t *e)
{
   http_xfer_request_t req;
   char *user_agent = NULL, *content_type = NULL, *headers;
   uint8_t *read = NULL;
   CURLcode r = CURLE_OK;

   abandon(e);
   e->done = false;
   e->result = CURLE_OK;
   e->status = 0;
   e->downloaded = 0;
   e->uploaded = 0;
   e->content_length = -1;
   free(e->content_type);
   e->content_type = NULL;
   retro_atomic_store_release_int(&e->pause_bits, 0);
   e->started = cpu_features_get_time_usec();
   if (!e->url)
      return CURLE_URL_MALFORMAT;

   memset(&req, 0, sizeof(req));
   req.url = e->url;
   if (e->custom_request)
      req.method = e->custom_request;
   else if (e->no_body)
      req.method = "HEAD";
   else if (e->post)
      req.method = "POST";
   else
      req.method = "GET";
   user_agent = dup_str(e->user_agent);
   headers = request_headers(e, &user_agent, &content_type);
   req.headers = headers;
   req.user_agent = user_agent;
   req.content_type = content_type;
   if (e->post)
   {
      req.has_body = true;
      if (e->post_fields)
      {
         req.body = e->post_fields;
         req.body_len = e->post_field_size >= 0 ? (size_t)e->post_field_size
            : strlen((const char*)e->post_fields);
      }
      else if (e->read_fn)
      {
         r = read_body(e, &read, &req.body_len);
         req.body = read;
      }
      e->uploaded = (double)req.body_len;
   }
   if (r == CURLE_OK && strlen(e->url) >= 6 && lower_eq(e->url, "https:", 6)
         && (e->ssl_ctx_fn || !e->verify_peer || !e->verify_host))
   {
      req.tls = http_xfer_tls_new();
      if (!req.tls)
         r = CURLE_OUT_OF_MEMORY;
      else if (e->ssl_ctx_fn)
         r = e->ssl_ctx_fn(handle, req.tls, e->ssl_ctx_data);
      if (r == CURLE_OK)
      {
         /* the options turn checks off, not back on */
         bool peer, host;
         http_xfer_tls_get_verify(req.tls, &peer, &host);
         http_xfer_tls_set_verify(req.tls, peer && e->verify_peer, host && e->verify_host);
      }
   }
   if (r == CURLE_OK)
   {
      if (e->timeout_ms > 0)
         req.timeout_ms = (unsigned)e->timeout_ms;
      e->xfer = http_xfer_start(&req); /* takes req.tls, copies the rest */
      req.tls = NULL;
      if (!e->xfer)
         r = CURLE_OUT_OF_MEMORY;
   }
   http_xfer_tls_free(req.tls);
   free(read);
   free(headers);
   free(user_agent);
   free(content_type);
   return r;
}

static void ended(easy_t *e, CURLcode result)
{
   e->result = result;
   e->done = true;
}

/* Hands what has come back to the callbacks: e->done once the transfer
 * is over, and then it is let go. */
static void deliver(easy_t *e)
{
   http_xfer_t *x = e->xfer;
   const http_xfer_event_t *ev;
   if (!x)
      return;
   for (ev = http_xfer_peek(x); ev; ev = http_xfer_peek(x))
   {
      if (ev->kind == HTTP_XFER_HEADERS)
      {
         size_t a = 0;
         e->status = ev->status;
         while (a < ev->len)
         {
            const char *line = ev->data + a;
            const char *nl = strstr(line, "\r\n");
            size_t len = nl ? (size_t)(nl - line) + 2 : ev->len - a;
            const char *v;
            size_t vlen;
            if (header_is(line, len, "Content-Type"))
            {
               header_value(line, len, &v, &vlen);
               free(e->content_type);
               e->content_type = (char*)malloc(vlen + 1);
               if (e->content_type)
               {
                  memcpy(e->content_type, v, vlen);
                  e->content_type[vlen] = '\0';
               }
            }
            else if (header_is(line, len, "Content-Length"))
            {
               header_value(line, len, &v, &vlen);
               e->content_length = atof(v);
            }
            /* libcurl's callback may write to the line */
            if (e->header_fn && e->header_fn((char*)line, 1, len, e->header_data) != len)
            {
               ended(e, CURLE_WRITE_ERROR);
               break;
            }
            a += len;
         }
         if (e->done)
            break;
         http_xfer_pop(x);
      }
      else if (ev->kind == HTTP_XFER_BODY)
      {
         size_t n = ev->len;
         if (retro_atomic_load_acquire_int(&e->pause_bits) & CURLPAUSE_RECV)
            return;
         if (e->write_fn)
         {
            size_t w = e->write_fn((char*)ev->data, 1, n, e->write_data);
            if (w == CURL_WRITEFUNC_PAUSE)
            {
               /* this run again on resume */
               retro_atomic_fetch_or_int(&e->pause_bits, CURLPAUSE_RECV);
               http_xfer_pause(x, true);
               return;
            }
            if (w != n)
            {
               ended(e, CURLE_WRITE_ERROR);
               break;
            }
         }
         e->downloaded += (double)n;
         http_xfer_pop(x);
         if (e->progress_fn && !e->no_progress
               && e->progress_fn(e->progress_data, e->content_length > 0 ? e->content_length : 0,
                  e->downloaded, e->uploaded, e->uploaded) != 0)
         {
            ended(e, CURLE_ABORTED_BY_CALLBACK);
            break;
         }
      }
      else
      {
         if (ev->status > 0)
            e->status = ev->status;
         e->seconds = (double)(cpu_features_get_time_usec() - e->started) / 1000000.0;
         ended(e, curl_code(ev->result));
         http_xfer_pop(x);
         break;
      }
   }
   if (e->done)
   {
      /* ended here by a callback, or by the transfer */
      http_xfer_close(x);
      e->xfer = NULL;
   }
}

/* nothing to hand over: an empty queue, or a paused one */
static bool can_deliver(void *ctx)
{
   easy_t *e = (easy_t*)ctx;
   return e->done || (http_xfer_peek(e->xfer)
      && !(retro_atomic_load_acquire_int(&e->pause_bits) & CURLPAUSE_RECV));
}

CURLcode curlh_easy_perform(CURL *curl)
{
   easy_t *e = (easy_t*)curl;
   CURLcode r;
   http_xfer_t *x;
   if (!e)
      return CURLE_BAD_FUNCTION_ARGUMENT;
   r = begin(curl, e);
   if (r != CURLE_OK)
      return r;
   x = e->xfer;
   for (;;)
   {
      deliver(e);
      if (e->done)
         break;
      http_xfer_wait(x, can_deliver, e);
   }
   return e->result;
}

CURLcode curlh_easy_pause(CURL *curl, int bitmask)
{
   easy_t *e = (easy_t*)curl;
   if (!e)
      return CURLE_BAD_FUNCTION_ARGUMENT;
   retro_atomic_store_release_int(&e->pause_bits, bitmask);
   if (e->xfer)
   {
      http_xfer_pause(e->xfer, (bitmask & CURLPAUSE_RECV) != 0);
      if (!(bitmask & CURLPAUSE_RECV))
      {
         if (e->in_multi)
            deliver(e);
         else
            http_xfer_wake(e->xfer); /* the thread in curlh_easy_perform */
      }
   }
   return CURLE_OK;
}

CURLcode curlh_easy_getinfo(CURL *curl, CURLINFO info, ...)
{
   easy_t *e = (easy_t*)curl;
   va_list ap;
   void *out;
   if (!e)
      return CURLE_BAD_FUNCTION_ARGUMENT;
   va_start(ap, info);
   out = va_arg(ap, void*);
   va_end(ap);
   if (!out)
      return CURLE_BAD_FUNCTION_ARGUMENT;
   switch (info)
   {
      case CURLINFO_RESPONSE_CODE: *(long*)out = e->status; break;
      case CURLINFO_SSL_VERIFYRESULT: *(long*)out = 0; break;
      case CURLINFO_SIZE_DOWNLOAD: *(double*)out = e->downloaded; break;
      case CURLINFO_SIZE_UPLOAD: *(double*)out = e->uploaded; break;
      case CURLINFO_SPEED_DOWNLOAD: *(double*)out = e->seconds > 0 ? e->downloaded / e->seconds : 0; break;
      case CURLINFO_SPEED_UPLOAD: *(double*)out = e->seconds > 0 ? e->uploaded / e->seconds : 0; break;
      case CURLINFO_CONTENT_LENGTH_DOWNLOAD: *(double*)out = e->content_length; break;
      case CURLINFO_EFFECTIVE_URL: *(const char**)out = e->url ? e->url : ""; break;
      case CURLINFO_CONTENT_TYPE: *(const char**)out = e->content_type; break;
      case CURLINFO_REDIRECT_URL: *(const char**)out = NULL; break;
      default:
         switch ((int)info & CURLINFO_TYPEMASK)
         {
            case CURLINFO_STRING: *(const char**)out = NULL; break;
            case CURLINFO_LONG: *(long*)out = 0; break;
            case CURLINFO_DOUBLE: *(double*)out = 0; break;
            case CURLINFO_OFF_T: *(curl_off_t*)out = 0; break;
            default: return CURLE_UNKNOWN_OPTION;
         }
   }
   return CURLE_OK;
}

const char *curlh_easy_strerror(CURLcode code)
{
   switch (code)
   {
      case CURLE_OK: return "No error";
      case CURLE_UNSUPPORTED_PROTOCOL: return "Unsupported protocol";
      case CURLE_FAILED_INIT: return "Failed initialization";
      case CURLE_URL_MALFORMAT: return "URL using bad/illegal format or missing URL";
      case CURLE_COULDNT_RESOLVE_HOST: return "Could not resolve hostname";
      case CURLE_COULDNT_CONNECT: return "Could not connect to server";
      case CURLE_WRITE_ERROR: return "Failed writing received data to disk/application";
      case CURLE_READ_ERROR: return "Failed to open/read local data from file/application";
      case CURLE_OUT_OF_MEMORY: return "Out of memory";
      case CURLE_OPERATION_TIMEDOUT: return "Timeout was reached";
      case CURLE_SSL_CONNECT_ERROR: return "SSL connect error";
      case CURLE_ABORTED_BY_CALLBACK: return "Operation was aborted by an application callback";
      case CURLE_TOO_MANY_REDIRECTS: return "Number of redirects hit maximum amount";
      case CURLE_GOT_NOTHING: return "Server returned nothing (no headers, no data)";
      case CURLE_SEND_ERROR: return "Failed sending data to the peer";
      case CURLE_RECV_ERROR: return "Failure when receiving data from the peer";
      case CURLE_SSL_CERTPROBLEM: return "Problem with the local SSL certificate";
      case CURLE_PEER_FAILED_VERIFICATION: return "SSL peer certificate or SSH remote key was not OK";
      default: return "Unknown error";
   }
}

/* --- lists ----------------------------------------------------------------- */

struct curl_slist *curlh_slist_append(struct curl_slist *list, const char *data)
{
   struct curl_slist *n = (struct curl_slist*)malloc(sizeof(*n));
   struct curl_slist *l;
   if (!n)
      return NULL;
   n->data = dup_str(data ? data : "");
   n->next = NULL;
   if (!n->data)
   {
      free(n);
      return NULL;
   }
   if (!list)
      return n;
   for (l = list; l->next; l = l->next);
   l->next = n;
   return list;
}

void curlh_slist_free_all(struct curl_slist *list)
{
   while (list)
   {
      struct curl_slist *n = list->next;
      free(list->data);
      free(list);
      list = n;
   }
}

/* --- multi handles --------------------------------------------------------- */

CURLM *curlh_multi_init(void)
{
   return calloc(1, sizeof(multi_t));
}

CURLMcode curlh_multi_cleanup(CURLM *multi)
{
   multi_t *m = (multi_t*)multi;
   size_t i;
   if (!m)
      return CURLM_BAD_HANDLE;
   for (i = 0; i < m->count; i++)
      m->handles[i]->in_multi = false;
   free(m->handles);
   free(m->started);
   free(m->reported);
   free(m->messages);
   free(m);
   return CURLM_OK;
}

CURLMcode curlh_multi_add_handle(CURLM *multi, CURL *curl)
{
   multi_t *m = (multi_t*)multi;
   easy_t *e = (easy_t*)curl;
   if (!m)
      return CURLM_BAD_HANDLE;
   if (!e || e->in_multi)
      return CURLM_BAD_EASY_HANDLE;
   if (m->count == m->capacity)
   {
      size_t cap = m->capacity ? m->capacity * 2 : 4;
      easy_t **h = (easy_t**)realloc(m->handles, cap * sizeof(*h));
      bool *s, *r;
      if (!h)
         return CURLM_OUT_OF_MEMORY;
      m->handles = h;
      s = (bool*)realloc(m->started, cap * sizeof(*s));
      if (!s)
         return CURLM_OUT_OF_MEMORY;
      m->started = s;
      r = (bool*)realloc(m->reported, cap * sizeof(*r));
      if (!r)
         return CURLM_OUT_OF_MEMORY;
      m->reported = r;
      m->capacity = cap;
   }
   e->in_multi = true;
   m->handles[m->count] = e;
   m->started[m->count] = false;
   m->reported[m->count] = false;
   m->count++;
   return CURLM_OK;
}

CURLMcode curlh_multi_remove_handle(CURLM *multi, CURL *curl)
{
   multi_t *m = (multi_t*)multi;
   size_t i;
   if (!m)
      return CURLM_BAD_HANDLE;
   for (i = 0; i < m->count; i++)
   {
      if (m->handles[i] == (easy_t*)curl)
      {
         size_t rest = m->count - i - 1;
         abandon(m->handles[i]);
         m->handles[i]->in_multi = false;
         memmove(&m->handles[i], &m->handles[i + 1], rest * sizeof(m->handles[0]));
         memmove(&m->started[i], &m->started[i + 1], rest * sizeof(m->started[0]));
         memmove(&m->reported[i], &m->reported[i + 1], rest * sizeof(m->reported[0]));
         m->count--;
         return CURLM_OK;
      }
   }
   return CURLM_BAD_EASY_HANDLE;
}

static bool add_message(multi_t *m, easy_t *e)
{
   CURLMsg *msg;
   if (m->message_count == m->message_capacity)
   {
      size_t cap = m->message_capacity ? m->message_capacity * 2 : 4;
      CURLMsg *grown = (CURLMsg*)realloc(m->messages, cap * sizeof(*grown));
      if (!grown)
         return false;
      m->messages = grown;
      m->message_capacity = cap;
   }
   msg = &m->messages[m->message_count++];
   memset(msg, 0, sizeof(*msg));
   msg->msg = CURLMSG_DONE;
   msg->easy_handle = e;
   msg->data.result = e->result;
   return true;
}

CURLMcode curlh_multi_perform(CURLM *multi, int *running_handles)
{
   multi_t *m = (multi_t*)multi;
   int running = 0;
   size_t i;
   if (!m)
      return CURLM_BAD_HANDLE;
   for (i = 0; i < m->count; i++)
   {
      easy_t *e = m->handles[i];
      if (!m->started[i])
      {
         CURLcode r;
         m->started[i] = true;
         r = begin(e, e);
         if (r != CURLE_OK)
            ended(e, r);
      }
      if (!e->done)
         deliver(e);
      /* done here, or earlier in a curlh_easy_pause that delivered */
      if (e->done && !m->reported[i] && add_message(m, e))
         m->reported[i] = true;
      if (!e->done)
         running++;
   }
   if (running_handles)
      *running_handles = running;
   return CURLM_OK;
}

CURLMcode curlh_multi_timeout(CURLM *multi, long *milliseconds)
{
   multi_t *m = (multi_t*)multi;
   long t = -1;
   size_t i;
   if (!m || !milliseconds)
      return CURLM_BAD_HANDLE;
   for (i = 0; i < m->count; i++)
   {
      easy_t *e = m->handles[i];
      if (!m->started[i] || (e->done && !m->reported[i])
            || (e->xfer && http_xfer_peek(e->xfer)
               && !(retro_atomic_load_acquire_int(&e->pause_bits) & CURLPAUSE_RECV)))
      {
         t = 0;
         break;
      }
      if (!e->done)
         t = 10;
   }
   *milliseconds = t;
   return CURLM_OK;
}

CURLMcode curlh_multi_setopt(CURLM *multi, CURLMoption option, ...)
{
   (void)option;
   return multi ? CURLM_OK : CURLM_BAD_HANDLE;
}

CURLMcode curlh_multi_fdset(CURLM *multi, void *read_fd_set, void *write_fd_set,
      void *exc_fd_set, int *max_fd)
{
   (void)read_fd_set;
   (void)write_fd_set;
   (void)exc_fd_set;
   if (!multi)
      return CURLM_BAD_HANDLE;
   if (max_fd)
      *max_fd = -1;
   return CURLM_OK;
}

CURLMsg *curlh_multi_info_read(CURLM *multi, int *msgs_in_queue)
{
   multi_t *m = (multi_t*)multi;
   CURLMsg *msg;
   if (!m || m->message_read >= m->message_count)
   {
      if (m)
      {
         m->message_count = 0;
         m->message_read = 0;
      }
      if (msgs_in_queue)
         *msgs_in_queue = 0;
      return NULL;
   }
   msg = &m->messages[m->message_read++];
   if (msgs_in_queue)
      *msgs_in_queue = (int)(m->message_count - m->message_read);
   return msg;
}

/* --- share handles --------------------------------------------------------- */

CURLSH *curlh_share_init(void)
{
   /* nothing is shared between handles that the transfer thread does not
    * share already */
   return malloc(1);
}

CURLSHcode curlh_share_setopt(CURLSH *share, CURLSHoption option, ...)
{
   (void)option;
   return share ? CURLSHE_OK : CURLSHE_INVALID;
}

CURLSHcode curlh_share_cleanup(CURLSH *share)
{
   free(share);
   return CURLSHE_OK;
}
