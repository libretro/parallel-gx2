/* nlibcurl's host side (src/Cafe/OS/libs/nlibcurl/curl_host.c) against
 * local servers (run_tests.sh starts them):
 *   curl_host_test <case> <http port> <https port> <dir>
 * <dir> holds body.bin and its SHA-256 in body.sha256, and for the TLS
 * cases ca.der, cli.der and cli.key.der. Exit 0 on success. */
#include "Cafe/OS/libs/nlibcurl/curl_host.h"
#include "Common/http_xfer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <lrc_hash.h>

static std::string g_dir;
static int g_http, g_https;

static std::vector<uint8_t> slurp(const std::string &path)
{
   std::vector<uint8_t> v;
   FILE *f = fopen(path.c_str(), "rb");
   if (!f)
      return v;
   char buf[65536];
   size_t n;
   while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
      v.insert(v.end(), buf, buf + n);
   fclose(f);
   return v;
}

static std::string sha256hex(const std::vector<uint8_t> &v)
{
   char hex[65];
   sha256_hash(hex, v.data(), v.size());
   return hex;
}

static std::string wantSha()
{
   std::vector<uint8_t> s = slurp(g_dir + "/body.sha256");
   return std::string(s.begin(), s.begin() + 64);
}

static std::string url(bool tls, const char *path)
{
   return std::string(tls ? "https://localhost:" : "http://127.0.0.1:")
      + std::to_string(tls ? g_https : g_http) + path;
}

struct Sink
{
   std::vector<uint8_t> body;
   std::string headers;
   int pauses = 0;      /* pause this many times before taking data */
   int paused = 0;
   bool waiting = false; /* paused, and not yet resumed */
   bool early = false;   /* handed data while paused */
};

static size_t onWrite(char *p, size_t s, size_t n, void *u)
{
   Sink *k = (Sink*)u;
   if (k->waiting)
      k->early = true;
   if (k->paused < k->pauses)
   {
      k->paused++;
      k->waiting = true;
      return CURL_WRITEFUNC_PAUSE;
   }
   k->body.insert(k->body.end(), p, p + s * n);
   return s * n;
}

static size_t onHeader(char *p, size_t s, size_t n, void *u)
{
   ((Sink*)u)->headers.append(p, s * n);
   return s * n;
}

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

static int getBody(bool tls, curl_ssl_ctx_callback ctxFn = nullptr)
{
   Sink k;
   CURL *c = curlh_easy_init();
   curlh_easy_setopt(c, CURLOPT_URL, url(tls, "/body.bin").c_str());
   curlh_easy_setopt(c, CURLOPT_WRITEFUNCTION, onWrite);
   curlh_easy_setopt(c, CURLOPT_WRITEDATA, &k);
   curlh_easy_setopt(c, CURLOPT_HEADERFUNCTION, onHeader);
   curlh_easy_setopt(c, CURLOPT_HEADERDATA, &k);
   if (ctxFn)
   {
      curlh_easy_setopt(c, CURLOPT_SSL_CTX_FUNCTION, ctxFn);
      curlh_easy_setopt(c, CURLOPT_SSL_CTX_DATA, nullptr);
   }
   const CURLcode r = curlh_easy_perform(c);
   long status = 0;
   double size = 0;
   curlh_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
   curlh_easy_getinfo(c, CURLINFO_SIZE_DOWNLOAD, &size);
   curlh_easy_cleanup(c);
   if (r != CURLE_OK)
      fprintf(stderr, "perform: %s\n", curlh_easy_strerror(r));
   CHECK(r == CURLE_OK);
   CHECK(status == 200);
   CHECK(k.headers.compare(0, 13, "HTTP/1.1 200 ") == 0 || k.headers.compare(0, 13, "HTTP/1.1 200\r") == 0);
   CHECK(k.headers.size() >= 4 && k.headers.compare(k.headers.size() - 4, 4, "\r\n\r\n") == 0);
   CHECK((size_t)size == k.body.size());
   CHECK(sha256hex(k.body) == wantSha());
   return 0;
}

static int postEcho(bool useRead)
{
   /* the server answers a POST with the SHA-256 of what it got */
   std::vector<uint8_t> body = slurp(g_dir + "/body.bin");
   body.resize(300001);
   struct Reader { const std::vector<uint8_t> *v; size_t off; } rd{&body, 0};
   Sink k;
   CURL *c = curlh_easy_init();
   struct curl_slist *h = curlh_slist_append(nullptr, "Content-Type: application/octet-stream");
   h = curlh_slist_append(h, "Content-Length: 1");  /* replaced, as nlibcurl relies on */
   h = curlh_slist_append(h, "X-Test: yes");
   curlh_easy_setopt(c, CURLOPT_URL, url(false, "/echo").c_str());
   curlh_easy_setopt(c, CURLOPT_HTTPHEADER, h);
   curlh_easy_setopt(c, CURLOPT_POST, 1L);
   if (useRead)
   {
      curlh_easy_setopt(c, CURLOPT_READFUNCTION, (curl_read_callback)[](char *p, size_t s, size_t n, void *u) -> size_t {
         Reader *r = (Reader*)u;
         size_t take = std::min(s * n, r->v->size() - r->off);
         memcpy(p, r->v->data() + r->off, take);
         r->off += take;
         return take;
      });
      curlh_easy_setopt(c, CURLOPT_READDATA, &rd);
      curlh_easy_setopt(c, CURLOPT_INFILESIZE_LARGE, (curl_off_t)body.size());
   }
   else
   {
      curlh_easy_setopt(c, CURLOPT_POSTFIELDS, body.data());
      curlh_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size());
   }
   curlh_easy_setopt(c, CURLOPT_WRITEFUNCTION, onWrite);
   curlh_easy_setopt(c, CURLOPT_WRITEDATA, &k);
   const CURLcode r = curlh_easy_perform(c);
   curlh_easy_cleanup(c);
   curlh_slist_free_all(h);
   CHECK(r == CURLE_OK);
   CHECK(std::string(k.body.begin(), k.body.end()) == sha256hex(body) + " yes application/octet-stream");
   return 0;
}

static int notFound()
{
   Sink k;
   CURL *c = curlh_easy_init();
   curlh_easy_setopt(c, CURLOPT_URL, url(false, "/missing").c_str());
   curlh_easy_setopt(c, CURLOPT_WRITEFUNCTION, onWrite);
   curlh_easy_setopt(c, CURLOPT_WRITEDATA, &k);
   const CURLcode r = curlh_easy_perform(c);
   long status = 0;
   curlh_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
   curlh_easy_cleanup(c);
   CHECK(r == CURLE_OK);
   CHECK(status == 404);
   return 0;
}

static int timeout()
{
   CURL *c = curlh_easy_init();
   curlh_easy_setopt(c, CURLOPT_URL, url(false, "/stall").c_str());
   curlh_easy_setopt(c, CURLOPT_TIMEOUT_MS, 700L);
   const CURLcode r = curlh_easy_perform(c);
   curlh_easy_cleanup(c);
   CHECK(r == CURLE_OPERATION_TIMEDOUT);
   return 0;
}

static int multi(int pauses)
{
   const int N = 4;
   Sink k[N];
   CURL *c[N];
   CURLM *m = curlh_multi_init();
   for (int i = 0; i < N; i++)
   {
      k[i].pauses = pauses;
      c[i] = curlh_easy_init();
      curlh_easy_setopt(c[i], CURLOPT_URL, url(false, "/body.bin").c_str());
      curlh_easy_setopt(c[i], CURLOPT_WRITEFUNCTION, onWrite);
      curlh_easy_setopt(c[i], CURLOPT_WRITEDATA, &k[i]);
      CHECK(curlh_multi_add_handle(m, c[i]) == CURLM_OK);
   }
   int running = N, done = 0;
   while (done < N)
   {
      curlh_multi_perform(m, &running);
      CURLMsg *msg;
      int left;
      while ((msg = curlh_multi_info_read(m, &left)))
      {
         CHECK(msg->msg == CURLMSG_DONE);
         CHECK(msg->data.result == CURLE_OK);
         done++;
      }
      /* a paused transfer resumes when asked, as a game would */
      for (int i = 0; i < N; i++)
         if (k[i].waiting)
         {
            k[i].waiting = false;
            curlh_easy_pause(c[i], CURLPAUSE_CONT);
         }
      long t = 0;
      curlh_multi_timeout(m, &t);
      if (t > 0)
         std::this_thread::sleep_for(std::chrono::milliseconds(t));
   }
   CHECK(running == 0);
   CHECK(done == N);
   /* each reported once */
   curlh_multi_perform(m, &running);
   int left = -1;
   CHECK(curlh_multi_info_read(m, &left) == nullptr && left == 0);
   for (int i = 0; i < N; i++)
   {
      CHECK(sha256hex(k[i].body) == wantSha());
      CHECK(k[i].paused == pauses);
      CHECK(!k[i].early);
      curlh_multi_remove_handle(m, c[i]);
      curlh_easy_cleanup(c[i]);
   }
   curlh_multi_cleanup(m);
   return 0;
}

static CURLcode tlsCtx(CURL *, void *tls, void *)
{
   std::vector<uint8_t> ca = slurp(g_dir + "/ca.der");
   std::vector<uint8_t> cert = slurp(g_dir + "/cli.der");
   std::vector<uint8_t> key = slurp(g_dir + "/cli.key.der");
   static const unsigned suite = 0x0035; /* TLS_RSA_WITH_AES_256_CBC_SHA */
   if (!http_xfer_tls_add_trust((http_xfer_tls_t*)tls, ca.data(), ca.size())
         || !http_xfer_tls_set_client((http_xfer_tls_t*)tls, cert.data(), cert.size(), key.data(), key.size()))
      return CURLE_SSL_CERTPROBLEM;
   http_xfer_tls_set_suites((http_xfer_tls_t*)tls, &suite, 1);
   return CURLE_OK;
}

static CURLcode tlsWrongCa(CURL *, void *tls, void *)
{
   std::vector<uint8_t> other = slurp(g_dir + "/cli.der"); /* signed nothing the server shows */
   http_xfer_tls_add_trust((http_xfer_tls_t*)tls, other.data(), other.size());
   return CURLE_OK;
}

static CURLcode tlsUnchecked(CURL *, void *tls, void *)
{
   /* no trust anchor for the server's certificate: only unchecked does it
    * connect */
   std::vector<uint8_t> cert = slurp(g_dir + "/cli.der");
   std::vector<uint8_t> key = slurp(g_dir + "/cli.key.der");
   static const unsigned suite = 0x0035;
   if (!http_xfer_tls_set_client((http_xfer_tls_t*)tls, cert.data(), cert.size(), key.data(), key.size()))
      return CURLE_SSL_CERTPROBLEM;
   http_xfer_tls_set_suites((http_xfer_tls_t*)tls, &suite, 1);
   http_xfer_tls_set_verify((http_xfer_tls_t*)tls, false, false);
   return CURLE_OK;
}

static int tlsUncheckedStays()
{
   /* nlibcurl's NSSL context turns the checks off; the game's
    * CURLOPT_SSL_VERIFYHOST 0 must not turn the peer's back on */
   Sink k;
   CURL *c = curlh_easy_init();
   curlh_easy_setopt(c, CURLOPT_URL, url(true, "/body.bin").c_str());
   curlh_easy_setopt(c, CURLOPT_SSL_CTX_FUNCTION, tlsUnchecked);
   curlh_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
   curlh_easy_setopt(c, CURLOPT_WRITEFUNCTION, onWrite);
   curlh_easy_setopt(c, CURLOPT_WRITEDATA, &k);
   const CURLcode r = curlh_easy_perform(c);
   curlh_easy_cleanup(c);
   CHECK(r == CURLE_OK);
   CHECK(sha256hex(k.body) == wantSha());
   return 0;
}

static int tlsRefused()
{
   CURL *c = curlh_easy_init();
   curlh_easy_setopt(c, CURLOPT_URL, url(true, "/body.bin").c_str());
   curlh_easy_setopt(c, CURLOPT_SSL_CTX_FUNCTION, tlsWrongCa);
   const CURLcode r = curlh_easy_perform(c);
   curlh_easy_cleanup(c);
   CHECK(r == CURLE_SSL_CONNECT_ERROR);
   return 0;
}

static int threads()
{
   /* several threads at once, each with its own handle, as the IOSU
    * services and the guest's nlibcurl workers do */
   int fails = 0;
   std::vector<std::thread> t;
   for (int i = 0; i < 6; i++)
      t.emplace_back([&fails, i] { if ((i & 1) ? postEcho(true) : getBody(false)) fails++; });
   for (auto &x : t)
      x.join();
   CHECK(fails == 0);
   return 0;
}

static int restart()
{
   /* the transfer thread stops at curlh_global_cleanup() and comes back */
   CHECK(getBody(false) == 0);
   curlh_global_cleanup();
   CHECK(getBody(false) == 0);
   curlh_global_cleanup();
   curlh_global_cleanup();
   return 0;
}

int main(int argc, char **argv)
{
   if (argc != 5)
      return 2;
   g_http = atoi(argv[2]);
   g_https = atoi(argv[3]);
   g_dir = argv[4];
   const std::string t = argv[1];
   int r = 2;
   curlh_global_init(CURL_GLOBAL_DEFAULT); /* as retro_init() does */
   if (t == "get") r = getBody(false);
   else if (t == "post") r = postEcho(false);
   else if (t == "post-read") r = postEcho(true);
   else if (t == "404") r = notFound();
   else if (t == "timeout") r = timeout();
   else if (t == "multi") r = multi(0);
   else if (t == "pause") r = multi(2);
   else if (t == "tls") r = getBody(true, tlsCtx);
   else if (t == "tls-refused") r = tlsRefused();
   else if (t == "tls-unchecked") r = tlsUncheckedStays();
   else if (t == "threads") r = threads();
   else if (t == "restart") r = restart();
   curlh_global_cleanup();
   return r;
}
