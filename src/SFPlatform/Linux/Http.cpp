#include "include/Http.h"

#include "include/Encoding.h"
#include "include/Log.h"

#include <curl/curl.h>
#include <dlfcn.h>

#include <chrono>
#include <cstring>
#include <string>

namespace SFPlatform::Http {
namespace {

// curl is bound at runtime rather than linked.
//
// Steam runs inside the steam-runtime, which pins its own libcurl.so.4
// (exposing only CURL_OPENSSL_3) ahead of the host's (CURL_OPENSSL_4). Same
// SONAME, older version — so a link-time dependency makes the dynamic loader
// demand a version tag the runtime cannot supply and abort the entire Steam
// client before it starts. dlsym ignores symbol versions, so resolving the few
// easy-API entry points we use at load time binds cleanly to whichever libcurl
// the process actually has. The easy API used here is stable across both.
struct CurlApi {
    CURL*       (*easy_init)()                                = nullptr;
    CURLcode    (*easy_setopt)(CURL*, CURLoption, ...)        = nullptr;
    CURLcode    (*easy_perform)(CURL*)                        = nullptr;
    CURLcode    (*easy_getinfo)(CURL*, CURLINFO, ...)         = nullptr;
    const char* (*easy_strerror)(CURLcode)                    = nullptr;
    void        (*easy_cleanup)(CURL*)                        = nullptr;
    curl_slist* (*slist_append)(curl_slist*, const char*)     = nullptr;
    void        (*slist_free_all)(curl_slist*)                = nullptr;
    bool ok = false;
};

const CurlApi& Curl() {
    static const CurlApi api = [] {
        CurlApi a;
        void* h = dlopen("libcurl.so.4", RTLD_LAZY | RTLD_LOCAL);
        if (!h) h = dlopen("libcurl.so.3", RTLD_LAZY | RTLD_LOCAL);
        if (!h) h = dlopen("libcurl.so",   RTLD_LAZY | RTLD_LOCAL);
        if (!h) {
            SFP_LOG_WARN("Http: no libcurl could be loaded ({})", dlerror());
            return a;
        }

        auto sym = [h](const char* name) {
            void* p = dlsym(h, name);
            if (!p) SFP_LOG_WARN("Http: libcurl is missing '{}'", name);
            return p;
        };

        a.easy_init      = reinterpret_cast<decltype(a.easy_init)>(sym("curl_easy_init"));
        a.easy_setopt    = reinterpret_cast<decltype(a.easy_setopt)>(sym("curl_easy_setopt"));
        a.easy_perform   = reinterpret_cast<decltype(a.easy_perform)>(sym("curl_easy_perform"));
        a.easy_getinfo   = reinterpret_cast<decltype(a.easy_getinfo)>(sym("curl_easy_getinfo"));
        a.easy_strerror  = reinterpret_cast<decltype(a.easy_strerror)>(sym("curl_easy_strerror"));
        a.easy_cleanup   = reinterpret_cast<decltype(a.easy_cleanup)>(sym("curl_easy_cleanup"));
        a.slist_append   = reinterpret_cast<decltype(a.slist_append)>(sym("curl_slist_append"));
        a.slist_free_all = reinterpret_cast<decltype(a.slist_free_all)>(sym("curl_slist_free_all"));

        a.ok = a.easy_init && a.easy_setopt && a.easy_perform && a.easy_getinfo &&
               a.easy_strerror && a.easy_cleanup && a.slist_append && a.slist_free_all;
        return a;
    }();
    return api;
}

struct WriteContext {
    std::string* body;
    uint32_t maxBytes;
    bool truncated = false;
};

size_t WriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<WriteContext*>(userdata);
    size_t totalBytes = size * nmemb;

    // Must return exactly totalBytes to keep curl happy: any short count is
    // treated as a write error and fails the whole transfer with
    // CURLE_WRITE_ERROR. Returning `allowed` here meant that hitting the cap --
    // the normal outcome for an oversized response -- looked like a network
    // failure rather than a successful truncated read. Keep consuming, just
    // stop appending once the cap is reached.
    if (ctx->body->size() < ctx->maxBytes) {
        const size_t room = ctx->maxBytes - ctx->body->size();
        ctx->body->append(ptr, totalBytes < room ? totalBytes : room);
        if (totalBytes > room) ctx->truncated = true;
    } else if (totalBytes) {
        ctx->truncated = true;
    }
    return totalBytes;
}

} // namespace

Result Execute(const wchar_t* method,
               const char* url,
               const void* reqBody,
               uint32_t reqBodyLen,
               const wchar_t* headers,
               uint32_t /*timeoutResolve*/,
               uint32_t timeoutConnect,
               uint32_t /*timeoutSend*/,
               uint32_t timeoutRecv,
               uint32_t maxBodyBytes) {
    Result r;
    if (!url || !url[0]) {
        SFP_LOG_WARN("Http::Execute: null/empty URL");
        return r;
    }

    const CurlApi& api = Curl();
    if (!api.ok) {
        SFP_LOG_WARN("Http::Execute: libcurl unavailable");
        return r;
    }

    CURL* curl = api.easy_init();
    if (!curl) {
        SFP_LOG_WARN("Http::Execute: curl_easy_init failed");
        return r;
    }

    auto t0 = std::chrono::steady_clock::now();
    api.easy_setopt(curl, CURLOPT_URL, url);
    api.easy_setopt(curl, CURLOPT_USERAGENT, "SteamFlipper/1.0");
    api.easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    api.easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(timeoutConnect));
    // Deliberately NOT CURLOPT_TIMEOUT_MS: that is a deadline on the entire
    // transfer, whereas the caller's timeoutRecv is WinHTTP's per-operation
    // receive timeout. Summing them killed slow-but-healthy downloads once they
    // exceeded the total. LOW_SPEED_TIME expresses the intended meaning --
    // abort only when the connection actually stalls.
    api.easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    api.easy_setopt(curl, CURLOPT_LOW_SPEED_TIME,
                    static_cast<long>((timeoutRecv + 999) / 1000));

    std::string methodStr;
    if (method) {
        methodStr = Encoding::WideToUtf8(method);
        if (!methodStr.empty()) {
            api.easy_setopt(curl, CURLOPT_CUSTOMREQUEST, methodStr.c_str());
        }
    }

    if (reqBody && reqBodyLen > 0) {
        api.easy_setopt(curl, CURLOPT_POSTFIELDS, reqBody);
        api.easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(reqBodyLen));
    }

    struct curl_slist* headerList = nullptr;
    if (headers && headers[0]) {
        std::string hStr = Encoding::WideToUtf8(headers);
        size_t start = 0;
        while (start < hStr.size()) {
            size_t end = hStr.find("\r\n", start);
            if (end == std::string::npos) end = hStr.find('\n', start);
            std::string line = (end == std::string::npos) ? hStr.substr(start) : hStr.substr(start, end - start);
            if (!line.empty()) {
                headerList = api.slist_append(headerList, line.c_str());
            }
            if (end == std::string::npos) break;
            start = (hStr[end] == '\r' && end + 1 < hStr.size() && hStr[end + 1] == '\n') ? end + 2 : end + 1;
        }
        if (headerList) {
            api.easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
        }
    }

    WriteContext ctx{&r.body, maxBodyBytes};
    api.easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    api.easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

    CURLcode res = api.easy_perform(curl);
    if (res == CURLE_OK) {
        long httpCode = 0;
        api.easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        r.status = static_cast<uint32_t>(httpCode);
        r.ok = true;
    } else {
        SFP_LOG_WARN("Http::Execute: curl_easy_perform failed for '{}' ({})", url, api.easy_strerror(res));
    }

    if (headerList) {
        api.slist_free_all(headerList);
    }
    api.easy_cleanup(curl);

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    SFP_LOG_DEBUG("{} - elapsed: {}ms status={} body_bytes={}",
                   url, elapsed, r.status, r.body.size());
    return r;
}

} // namespace SFPlatform::Http
