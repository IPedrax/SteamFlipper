#pragma once

/*
 * The part of libcurl's interface this module actually uses.
 *
 * Http.cpp has never linked against libcurl -- it dlopens it, because the
 * steam-runtime pins an older libcurl.so.4 (CURL_OPENSSL_3) ahead of the
 * host's (CURL_OPENSSL_4), and a link-time dependency makes the loader demand
 * a symbol version the runtime cannot supply, killing the Steam client at
 * startup. So <curl/curl.h> was contributing nothing but constants and two
 * type names, while making the development headers a hard build requirement.
 *
 * That requirement is why this file exists. On SteamOS the rootfs is read-only
 * and ships libcurl without its headers, so `find_package(CURL REQUIRED)`
 * failed on a Steam Deck that had a perfectly usable libcurl to dlopen at
 * runtime. Clearing it meant disabling the read-only filesystem and installing
 * development packages that the next SteamOS update removes again -- a lot to
 * ask for a handful of integers.
 *
 * The values are curl's public ABI and are guaranteed not to change: an option
 * number is fixed forever once assigned, which is the entire point of the
 * numbering scheme below. They were taken from the system header rather than
 * typed from memory, and tools/curl_abi_test.cpp re-checks them against the real
 * header on any machine that has it.
 */

extern "C" {

// Opaque to us: only ever passed back to the functions we resolved.
typedef void CURL;
typedef int  CURLcode;
typedef int  CURLINFO;
// Only ever the parameter type of easy_setopt, which is variadic past it, so
// an int matches how curl itself passes these.
typedef int  CURLoption;

struct curl_slist {
    char*              data;
    struct curl_slist* next;
};

// Option numbers are their type's base plus an index, and both halves are
// permanent. Spelling the bases out is what makes the values below checkable
// against curl's own header by eye.
enum {
    CURLOPTTYPE_LONG          = 0,
    CURLOPTTYPE_OBJECTPOINT   = 10000,
    CURLOPTTYPE_FUNCTIONPOINT = 20000
};

enum {
    CURLOPT_WRITEDATA         = CURLOPTTYPE_OBJECTPOINT + 1,     // 10001
    CURLOPT_URL               = CURLOPTTYPE_OBJECTPOINT + 2,     // 10002
    CURLOPT_POSTFIELDS        = CURLOPTTYPE_OBJECTPOINT + 15,    // 10015
    CURLOPT_USERAGENT         = CURLOPTTYPE_OBJECTPOINT + 18,    // 10018
    CURLOPT_HTTPHEADER        = CURLOPTTYPE_OBJECTPOINT + 23,    // 10023
    CURLOPT_CUSTOMREQUEST     = CURLOPTTYPE_OBJECTPOINT + 36,    // 10036

    CURLOPT_WRITEFUNCTION     = CURLOPTTYPE_FUNCTIONPOINT + 11,  // 20011

    CURLOPT_LOW_SPEED_LIMIT   = CURLOPTTYPE_LONG + 19,           // 19
    CURLOPT_LOW_SPEED_TIME    = CURLOPTTYPE_LONG + 20,           // 20
    CURLOPT_FOLLOWLOCATION    = CURLOPTTYPE_LONG + 52,           // 52
    CURLOPT_POSTFIELDSIZE     = CURLOPTTYPE_LONG + 60,           // 60
    CURLOPT_NOSIGNAL          = CURLOPTTYPE_LONG + 99,           // 99
    CURLOPT_TIMEOUT_MS        = CURLOPTTYPE_LONG + 155,          // 155
    CURLOPT_CONNECTTIMEOUT_MS = CURLOPTTYPE_LONG + 156           // 156
};

enum {
    CURLE_OK          = 0,
    CURLE_WRITE_ERROR = 23
};

// CURLINFO_LONG is 0x200000; RESPONSE_CODE is the second long-typed info.
enum { CURLINFO_RESPONSE_CODE = 0x200000 + 2 };                  // 2097154

} // extern "C"
