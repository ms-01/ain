// Copyright (c) 2015-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#ifndef DEFI_UTIL_URL_H
#define DEFI_UTIL_URL_H

#include <string>

/**
 * Decode a URL.
 *
 * Notably this implementation does not decode a '+' to a ' ' and keeps
 * invalid percent escapes as they are.
 */
std::string urlDecode(const std::string &urlEncoded);

/**
 * Encode a URL.
 *
 * All characters except the unreserved characters (see RFC 3986, section 2.3)
 * are percent-encoded.
 */
std::string urlEncode(const std::string &str);

#endif // DEFI_UTIL_URL_H
