// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The download of a link's logo where the app has no HTTP client of its own (macOS, window_generic.cpp): none, so the
// app asks for the file as before 8.1.0.

#include <webui/fetch.h>

namespace webui {

std::optional<std::vector<unsigned char>> FetchUrl(const std::string&, size_t, std::string& error)
{
    error = "not supported here";
    return std::nullopt;
}

} // namespace webui
