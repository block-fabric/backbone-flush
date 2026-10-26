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
    // The scheme: letters, digits, + - . after a letter (RFC 3986), then ':'.
    size_t colon{0};
    while (colon < uri.size() && (std::isalnum(static_cast<unsigned char>(uri[colon])) || ((uri[colon] == '+' || uri[colon] == '-' || uri[colon] == '.') && colon > 0))) ++colon;
    if (colon == 0 || colon >= uri.size() || uri[colon] != ':' || !std::isalpha(static_cast<unsigned char>(uri[0]))) return "(an address)";
    const std::string scheme{uri.substr(0, colon)};
    if (uri.compare(colon, 3, "://") == 0) {
        // Hierarchical: the host alone, ended by the first of / ? # \ ; (no path, query, fragment), without its user
        // part or its port.
        const size_t start{colon + 3};
        const size_t end{uri.find_first_of("/?#\\;", start)};
        std::string authority{uri.substr(start, end == std::string::npos ? std::string::npos : end - start)};
        if (const size_t at{authority.rfind('@')}; at != std::string::npos) authority = authority.substr(at + 1);
        if (!authority.empty() && authority.front() == '[') {
            authority = authority.substr(0, authority.find(']') == std::string::npos ? std::string::npos : authority.find(']') + 1);
        } else if (const size_t port{authority.find(':')}; port != std::string::npos) {
            authority = authority.substr(0, port);
        }
        return scheme + "://" + authority;
    }
    // Opaque (flush:connect?...): the scheme and a leading verb of [A-Za-z0-9_-] alone.
    size_t verb{colon + 1};
    while (verb < uri.size() && (std::isalnum(static_cast<unsigned char>(uri[verb])) || uri[verb] == '_' || uri[verb] == '-')) ++verb;
    return scheme + ":" + uri.substr(colon + 1, verb - colon - 1);
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
