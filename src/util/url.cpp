// Copyright (c) 2015-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <util/url.h>

#include <charconv>
#include <cstddef>
#include <string>
#include <system_error>

std::string urlDecode(const std::string &urlEncoded) {
    std::string res;
    res.reserve(urlEncoded.size());

    for (size_t i = 0; i < urlEncoded.size(); ++i) {
        char c = urlEncoded[i];
        // Special handling for percent which should be followed by two hex digits
        // representing an octet value, see RFC 3986, Section 2.1 Percent-Encoding
        if (c == '%' && i + 2 < urlEncoded.size()) {
            unsigned int decoded_value{0};
            auto [p, ec] = std::from_chars(urlEncoded.data() + i + 1, urlEncoded.data() + i + 3, decoded_value, 16);

            // Only if there is no error and the pointer is set to the end of
            // the string, we can be sure both characters were valid hex
            if (ec == std::errc{} && p == urlEncoded.data() + i + 3) {
                res += static_cast<char>(decoded_value);
                // Next two characters are part of the percent encoding
                i += 2;
                continue;
            }
            // In case of invalid percent encoding, add the '%' and continue
        }
        res += c;
    }

    return res;
}

std::string urlEncode(const std::string &str) {
    std::string res;
    res.reserve(str.size() * 3); // worst case: every char needs encoding

    for (char ch : str) {
        auto c = static_cast<unsigned char>(ch);
        // Unreserved characters per RFC 3986, Section 2.3
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            res += ch;
        } else {
            // Percent-encode all other characters
            res += '%';
            constexpr char hex_chars[] = "0123456789ABCDEF";
            res += hex_chars[(c >> 4) & 0xF];
            res += hex_chars[c & 0xF];
        }
    }
    return res;
}
