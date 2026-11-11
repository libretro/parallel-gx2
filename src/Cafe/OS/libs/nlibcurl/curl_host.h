#ifndef NLIBCURL_CURL_HOST_H
#define NLIBCURL_CURL_HOST_H

/* The host side of nlibcurl, the console's libcurl: libcurl's behaviour -
 * easy handles and their options, callbacks and pause, the multi and share
 * interfaces - over the core's HTTP transfers (http_xfer.h), for nlibcurl
 * to hand a game's calls to. Not libcurl, and not named as it: its
 * functions are curlh_*. Its types and its option, info and error numbers
 * are libcurl's, as the game gives them.
 *
 * Unlike libcurl:
 * - CURLOPT_SSL_CTX_FUNCTION receives the transfer's http_xfer_tls_t,
 *   to fill in with the http_xfer_tls_* functions.
 * - CURLOPT_SOCKOPTFUNCTION and CURLOPT_CLOSESOCKETFUNCTION are never
 *   called: the sockets are net_http's.
 * - A request body from CURLOPT_READFUNCTION is read whole before the
 *   request goes out.
 * - curlh_multi_fdset reports no descriptors; curlh_multi_timeout says
 *   when to call curlh_multi_perform again.
 * - Redirects are followed whatever CURLOPT_FOLLOWLOCATION says; a proxy
 *   is not used; options it has no use for are accepted and do nothing.
 *
 * C89. */

#include <stddef.h>
#include <stdint.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#include <winsock2.h>
typedef SOCKET curl_socket_t;
#else
typedef int curl_socket_t;
#endif

typedef void CURL;
typedef void CURLSH;
typedef void CURLM;
typedef int64_t curl_off_t;

struct curl_slist
{
   char *data;
   struct curl_slist *next;
};

#define CURLOPTTYPE_LONG          0
#define CURLOPTTYPE_OBJECTPOINT   10000
#define CURLOPTTYPE_FUNCTIONPOINT 20000
#define CURLOPTTYPE_OFF_T         30000
#define CURLOPTTYPE_BLOB          40000

typedef enum
{
   CURLOPT_WRITEDATA            = 10001,
   CURLOPT_URL                  = 10002,
   CURLOPT_PROXY                = 10004,
   CURLOPT_READDATA             = 10009,
   CURLOPT_TIMEOUT              = 13,
   CURLOPT_INFILESIZE           = 14,
   CURLOPT_POSTFIELDS           = 10015,
   CURLOPT_USERAGENT            = 10018,
   CURLOPT_LOW_SPEED_LIMIT      = 19,
   CURLOPT_LOW_SPEED_TIME       = 20,
   CURLOPT_HTTPHEADER           = 10023,
   CURLOPT_HTTPPOST             = 10024,
   CURLOPT_HEADERDATA           = 10029,
   CURLOPT_SSLVERSION           = 32,
   CURLOPT_CUSTOMREQUEST        = 10036,
   CURLOPT_VERBOSE              = 41,
   CURLOPT_NOPROGRESS           = 43,
   CURLOPT_NOBODY               = 44,
   CURLOPT_POST                 = 47,
   CURLOPT_FOLLOWLOCATION       = 52,
   CURLOPT_PROGRESSDATA         = 10057,
   CURLOPT_POSTFIELDSIZE        = 60,
   CURLOPT_SSL_VERIFYPEER       = 64,
   CURLOPT_MAXREDIRS            = 68,
   CURLOPT_CONNECTTIMEOUT       = 78,
   CURLOPT_HTTPGET              = 80,
   CURLOPT_SSL_VERIFYHOST       = 81,
   CURLOPT_HTTP_VERSION         = 84,
   CURLOPT_BUFFERSIZE           = 98,
   CURLOPT_NOSIGNAL             = 99,
   CURLOPT_SHARE                = 10100,
   CURLOPT_SSL_CTX_DATA         = 10109,
   CURLOPT_SOCKOPTDATA          = 10149,
   CURLOPT_TIMEOUT_MS           = 155,
   CURLOPT_CONNECTTIMEOUT_MS    = 156,
   CURLOPT_CLOSESOCKETDATA      = 10209,
   CURLOPT_WRITEFUNCTION        = 20011,
   CURLOPT_READFUNCTION         = 20012,
   CURLOPT_PROGRESSFUNCTION     = 20056,
   CURLOPT_HEADERFUNCTION       = 20079,
   CURLOPT_SSL_CTX_FUNCTION     = 20108,
   CURLOPT_SOCKOPTFUNCTION      = 20148,
   CURLOPT_CLOSESOCKETFUNCTION  = 20208,
   CURLOPT_INFILESIZE_LARGE     = 30115,
   CURLOPT_POSTFIELDSIZE_LARGE  = 30120,
   CURLOPT_MAX_SEND_SPEED_LARGE = 30145,
   CURLOPT_MAX_RECV_SPEED_LARGE = 30146
} CURLoption;

#define CURLOPT_FILE CURLOPT_WRITEDATA

typedef enum
{
   CURLE_OK                       = 0,
   CURLE_UNSUPPORTED_PROTOCOL     = 1,
   CURLE_FAILED_INIT              = 2,
   CURLE_URL_MALFORMAT            = 3,
   CURLE_COULDNT_RESOLVE_HOST     = 6,
   CURLE_COULDNT_CONNECT          = 7,
   CURLE_WRITE_ERROR              = 23,
   CURLE_READ_ERROR               = 26,
   CURLE_OUT_OF_MEMORY            = 27,
   CURLE_OPERATION_TIMEDOUT       = 28,
   CURLE_SSL_CONNECT_ERROR        = 35,
   CURLE_ABORTED_BY_CALLBACK      = 42,
   CURLE_BAD_FUNCTION_ARGUMENT    = 43,
   CURLE_TOO_MANY_REDIRECTS       = 47,
   CURLE_UNKNOWN_OPTION           = 48,
   CURLE_GOT_NOTHING              = 52,
   CURLE_SEND_ERROR               = 55,
   CURLE_RECV_ERROR               = 56,
   CURLE_SSL_CERTPROBLEM          = 58,
   CURLE_PEER_FAILED_VERIFICATION = 60
} CURLcode;

#define CURLE_SSL_CACERT CURLE_PEER_FAILED_VERIFICATION

#define CURLINFO_STRING   0x100000
#define CURLINFO_LONG     0x200000
#define CURLINFO_DOUBLE   0x300000
#define CURLINFO_OFF_T    0x600000
#define CURLINFO_TYPEMASK 0xf00000

typedef enum
{
   CURLINFO_EFFECTIVE_URL           = CURLINFO_STRING + 1,
   CURLINFO_RESPONSE_CODE           = CURLINFO_LONG   + 2,
   CURLINFO_SIZE_UPLOAD             = CURLINFO_DOUBLE + 7,
   CURLINFO_SIZE_DOWNLOAD           = CURLINFO_DOUBLE + 8,
   CURLINFO_SPEED_DOWNLOAD          = CURLINFO_DOUBLE + 9,
   CURLINFO_SPEED_UPLOAD            = CURLINFO_DOUBLE + 10,
   CURLINFO_SSL_VERIFYRESULT        = CURLINFO_LONG   + 13,
   CURLINFO_CONTENT_LENGTH_DOWNLOAD = CURLINFO_DOUBLE + 15,
   CURLINFO_CONTENT_TYPE            = CURLINFO_STRING + 18,
   CURLINFO_REDIRECT_URL            = CURLINFO_STRING + 31
} CURLINFO;

typedef enum
{
   CURLM_OK                    = 0,
   CURLM_BAD_HANDLE            = 1,
   CURLM_BAD_EASY_HANDLE       = 2,
   CURLM_OUT_OF_MEMORY         = 3,
   CURLM_BAD_FUNCTION_ARGUMENT = 10
} CURLMcode;

typedef enum
{
   CURLMOPT_MAXCONNECTS = 6
} CURLMoption;

typedef enum
{
   CURLMSG_NONE,
   CURLMSG_DONE
} CURLMSG;

typedef struct CURLMsg
{
   CURLMSG msg;
   CURL *easy_handle;
   union
   {
      void *whatever;
      CURLcode result;
   } data;
} CURLMsg;

typedef enum
{
   CURLSHE_OK,
   CURLSHE_BAD_OPTION,
   CURLSHE_IN_USE,
   CURLSHE_INVALID,
   CURLSHE_NOMEM
} CURLSHcode;

typedef enum
{
   CURLSHOPT_NONE,
   CURLSHOPT_SHARE,
   CURLSHOPT_UNSHARE,
   CURLSHOPT_LOCKFUNC,
   CURLSHOPT_UNLOCKFUNC,
   CURLSHOPT_USERDATA
} CURLSHoption;

typedef enum
{
   CURL_LOCK_DATA_NONE,
   CURL_LOCK_DATA_SHARE,
   CURL_LOCK_DATA_COOKIE,
   CURL_LOCK_DATA_DNS,
   CURL_LOCK_DATA_SSL_SESSION,
   CURL_LOCK_DATA_CONNECT
} curl_lock_data;

typedef enum
{
   CURL_LOCK_ACCESS_NONE,
   CURL_LOCK_ACCESS_SHARED,
   CURL_LOCK_ACCESS_SINGLE
} curl_lock_access;

typedef enum
{
   CURLSOCKTYPE_IPCXN,
   CURLSOCKTYPE_ACCEPT
} curlsocktype;

#define CURL_GLOBAL_SSL      (1 << 0)
#define CURL_GLOBAL_WIN32    (1 << 1)
#define CURL_GLOBAL_ALL      (CURL_GLOBAL_SSL | CURL_GLOBAL_WIN32)
#define CURL_GLOBAL_DEFAULT  CURL_GLOBAL_ALL

#define CURL_HTTP_VERSION_1_1   2L
#define CURL_SSLVERSION_DEFAULT 0
#define CURL_SSLVERSION_SSLv3   3
#define CURL_SSLVERSION_TLSv1_2 6

#define CURL_WRITEFUNC_PAUSE 0x10000001
#define CURL_READFUNC_ABORT  0x10000000
#define CURL_READFUNC_PAUSE  0x10000001
#define CURLPAUSE_RECV       (1 << 0)
#define CURLPAUSE_SEND       (1 << 2)
#define CURLPAUSE_ALL        (CURLPAUSE_RECV | CURLPAUSE_SEND)
#define CURLPAUSE_CONT       0

typedef size_t (*curl_write_callback)(char *buffer, size_t size,
      size_t nitems, void *userdata);
typedef size_t (*curl_read_callback)(char *buffer, size_t size,
      size_t nitems, void *instream);
typedef int (*curl_progress_callback)(void *clientp,
      double dltotal, double dlnow, double ultotal, double ulnow);
typedef int (*curl_sockopt_callback)(void *clientp,
      curl_socket_t curlfd, curlsocktype purpose);
typedef int (*curl_closesocket_callback)(void *clientp,
      curl_socket_t item);
/* ssl_ctx is the transfer's http_xfer_tls_t, to fill in (http_xfer.h) */
typedef CURLcode (*curl_ssl_ctx_callback)(CURL *curl, void *ssl_ctx,
      void *userptr);
typedef void (*curl_lock_function)(CURL *handle, curl_lock_data data,
      curl_lock_access locktype, void *userptr);
typedef void (*curl_unlock_function)(CURL *handle, curl_lock_data data,
      void *userptr);
typedef void *(*curl_malloc_callback)(size_t size);
typedef void (*curl_free_callback)(void *ptr);
typedef void *(*curl_realloc_callback)(void *ptr, size_t size);
typedef char *(*curl_strdup_callback)(const char *str);
typedef void *(*curl_calloc_callback)(size_t nmemb, size_t size);

/* the transfer thread, started (also by curlh_easy_init) and stopped:
 * not while anything transfers */
CURLcode curlh_global_init(long flags);
void curlh_global_cleanup(void);

CURL *curlh_easy_init(void);
void curlh_easy_cleanup(CURL *curl);
void curlh_easy_reset(CURL *curl);
CURLcode curlh_easy_setopt(CURL *curl, CURLoption option, ...);
CURLcode curlh_easy_perform(CURL *curl);
CURLcode curlh_easy_getinfo(CURL *curl, CURLINFO info, ...);
CURLcode curlh_easy_pause(CURL *curl, int bitmask);
const char *curlh_easy_strerror(CURLcode code);

struct curl_slist *curlh_slist_append(struct curl_slist *list, const char *data);
void curlh_slist_free_all(struct curl_slist *list);

CURLM *curlh_multi_init(void);
CURLMcode curlh_multi_cleanup(CURLM *multi);
CURLMcode curlh_multi_add_handle(CURLM *multi, CURL *curl);
CURLMcode curlh_multi_remove_handle(CURLM *multi, CURL *curl);
CURLMcode curlh_multi_perform(CURLM *multi, int *running_handles);
CURLMcode curlh_multi_timeout(CURLM *multi, long *milliseconds);
CURLMcode curlh_multi_setopt(CURLM *multi, CURLMoption option, ...);
CURLMcode curlh_multi_fdset(CURLM *multi, void *read_fd_set,
      void *write_fd_set, void *exc_fd_set, int *max_fd);
CURLMsg *curlh_multi_info_read(CURLM *multi, int *msgs_in_queue);

CURLSH *curlh_share_init(void);
CURLSHcode curlh_share_setopt(CURLSH *share, CURLSHoption option, ...);
CURLSHcode curlh_share_cleanup(CURLSH *share);

RETRO_END_DECLS

#endif
