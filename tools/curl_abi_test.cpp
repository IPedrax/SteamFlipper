/*
 * Check the vendored curl constants against the real header.
 *
 *   g++ -fsyntax-only -I src/SFPlatform/Linux tools/curl_abi_test.cpp
 *
 * SFPlatform/Linux/curl_min.h declares the handful of libcurl values Http.cpp
 * uses, so the build needs no curl development headers -- which is what lets a
 * Steam Deck build this without unlocking its read-only filesystem. That trade
 * is only safe while the numbers are right, so this compares them against
 * curl's own header on any machine that has one. There is nothing to run: a
 * mismatch is a compile error.
 *
 * It skips itself where curl headers are absent, which is precisely the
 * situation curl_min.h exists for.
 */
#if defined(__has_include)
#  if __has_include(<curl/curl.h>)
#    define SF_HAVE_CURL 1
#  endif
#endif

#if defined(SF_HAVE_CURL)

#include <curl/curl.h>
#include <cstddef>

// curl defines the option-type bases as macros, which would expand inside our
// own enum and turn its members into numeric constants. The values are
// captured first, then the macros dropped, so both sets can coexist.
enum {
    REAL_CURLOPTTYPE_LONG          = CURLOPTTYPE_LONG,
    REAL_CURLOPTTYPE_OBJECTPOINT   = CURLOPTTYPE_OBJECTPOINT,
    REAL_CURLOPTTYPE_FUNCTIONPOINT = CURLOPTTYPE_FUNCTIONPOINT
};
#undef CURLOPTTYPE_LONG
#undef CURLOPTTYPE_OBJECTPOINT
#undef CURLOPTTYPE_FUNCTIONPOINT

// Ours goes in a namespace so both sets of names are visible at once.
namespace sfmin {
#include "curl_min.h"
}

static_assert((long)sfmin::CURLOPTTYPE_LONG          == (long)REAL_CURLOPTTYPE_LONG,
              "the long option base moved");
static_assert((long)sfmin::CURLOPTTYPE_OBJECTPOINT   == (long)REAL_CURLOPTTYPE_OBJECTPOINT,
              "the object-pointer option base moved");
static_assert((long)sfmin::CURLOPTTYPE_FUNCTIONPOINT == (long)REAL_CURLOPTTYPE_FUNCTIONPOINT,
              "the function-pointer option base moved");

#define SAME(name) \
    static_assert(static_cast<long>(sfmin::name) == static_cast<long>(::name), \
                  #name " no longer matches libcurl")

SAME(CURLOPT_WRITEDATA);
SAME(CURLOPT_URL);
SAME(CURLOPT_POSTFIELDS);
SAME(CURLOPT_USERAGENT);
SAME(CURLOPT_HTTPHEADER);
SAME(CURLOPT_CUSTOMREQUEST);
SAME(CURLOPT_WRITEFUNCTION);
SAME(CURLOPT_LOW_SPEED_LIMIT);
SAME(CURLOPT_LOW_SPEED_TIME);
SAME(CURLOPT_FOLLOWLOCATION);
SAME(CURLOPT_POSTFIELDSIZE);
SAME(CURLOPT_NOSIGNAL);
SAME(CURLOPT_TIMEOUT_MS);
SAME(CURLOPT_CONNECTTIMEOUT_MS);
SAME(CURLE_OK);
SAME(CURLE_WRITE_ERROR);
SAME(CURLINFO_RESPONSE_CODE);

// The list node is passed straight to curl_slist_append, so its shape matters
// as much as the numbers.
static_assert(sizeof(sfmin::curl_slist) == sizeof(::curl_slist),
              "curl_slist has changed shape");
static_assert(offsetof(sfmin::curl_slist, data) == offsetof(::curl_slist, data),
              "curl_slist::data has moved");
static_assert(offsetof(sfmin::curl_slist, next) == offsetof(::curl_slist, next),
              "curl_slist::next has moved");

#endif
