// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WEBUI_OPENER_H
#define BITCOIN_WEBUI_OPENER_H

#include <functional>
#include <string>
#include <vector>

namespace webui {

/**
 * How the app opens a web address with the system's browser on Linux (Window::OpenExternal of window_gtk.cpp), each way
 * injectable for the tests:
 *  - `gio`: GIO's handler of the address's scheme (g_app_info_launch_default_for_uri);
 *  - `spawn`: a program started with these arguments, no shell (g_spawn_async, the path searched; true once it runs).
 * Under WSL, GIO usually knows no handler of https (the Windows browser is not a Linux application).
 */
struct OpenSteps {
    std::function<bool(const std::string& uri)> gio;
    std::function<bool(const std::vector<std::string>& argv)> spawn;
    //! Whether the app runs under WSL (IsWsl).
    bool wsl{false};
};

/** Under WSL: WSL_DISTRO_NAME set (`distro`, null if not), or the kernel's release (/proc/sys/kernel/osrelease) says "microsoft". */
bool IsWsl(const char* distro, const std::string& osrelease);

/**
 * Open `uri`, in this order, until one works: GIO; `wslview` (wslu, WSL's opener); under WSL only, Windows' own
 * handler (`/mnt/c/Windows/System32/rundll32.exe url.dll,FileProtocolHandler <uri>`: the address one argument, its &
 * and # read by no shell); `xdg-open`. @return what opened it ("gio", "wslview", "rundll32", "xdg-open"), empty if
 * nothing could.
 */
std::string OpenWithFallbacks(const std::string& uri, const OpenSteps& steps);

} // namespace webui

#endif // BITCOIN_WEBUI_OPENER_H
