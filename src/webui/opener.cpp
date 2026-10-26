// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <webui/opener.h>

#include <algorithm>
#include <cctype>

namespace webui {

bool IsWsl(const char* distro, const std::string& osrelease)
{
    if (distro && *distro) return true;
    std::string lower{osrelease};
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find("microsoft") != std::string::npos;
}

std::string LoggableUri(const std::string& uri)
{
    const size_t end{uri.find_first_of("?#")};
    const std::string head{uri.substr(0, end)};
    const size_t sep{head.find("://")};
    if (sep == std::string::npos) {
        // No host: the scheme and its opaque part before any query (flush:connect), no path beyond a first segment.
        const size_t colon{head.find(':')};
        if (colon == std::string::npos) return "(an address)";
        const size_t slash{head.find('/', colon + 1)};
        return head.substr(0, slash);
    }
    std::string authority{head.substr(sep + 3, head.find('/', sep + 3) == std::string::npos ? std::string::npos : head.find('/', sep + 3) - (sep + 3))};
    if (const size_t at{authority.rfind('@')}; at != std::string::npos) authority = authority.substr(at + 1);
    return head.substr(0, sep + 3) + authority;
}

std::string OpenWithFallbacks(const std::string& uri, const OpenSteps& steps)
{
    if (steps.gio && steps.gio(uri)) return "gio";
    if (!steps.spawn) return {};
    if (steps.spawn({"wslview", uri})) return "wslview";
    if (steps.wsl && steps.spawn({"/mnt/c/Windows/System32/rundll32.exe", "url.dll,FileProtocolHandler", uri})) return "rundll32";
    if (steps.spawn({"xdg-open", uri})) return "xdg-open";
    return {};
}

} // namespace webui
