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
