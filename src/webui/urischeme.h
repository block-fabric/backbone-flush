// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WEBUI_URISCHEME_H
#define BITCOIN_WEBUI_URISCHEME_H

#include <util/fs.h>

#include <string>
#include <vector>

namespace webui {

/**
 * Open the links of a scheme ("flush": payments and the action links of the site) with this program, for this user.
 * Windows: HKCU\Software\Classes\<scheme> (what the installer writes for all users). Linux: a desktop entry and the
 * default handler in mimeapps.list (RegisterUriSchemeIn), at the places of the XDG base directories. Left alone when it
 * already opens with a program that still exists (another installation the user chose). Whether the scheme opens with
 * this program now.
 */
bool RegisterUriScheme(const std::string& scheme, const std::string& description);

#ifndef WIN32
/**
 * Linux (freedesktop.org): the same, for this user: a desktop entry of the app in `data_home`/applications (its Exec this
 * program, `exe`, quoted as the specification says) with MimeType=x-scheme-handler/<scheme>, and that entry the default
 * handler of the scheme in `config_home`/mimeapps.list ([Default Applications]). Left alone when the scheme opens with
 * another entry that exists (in data_home or one of `data_dirs`: another installation the user chose). Never a program run,
 * never a shell. Whether the scheme opens with this program now.
 */
bool RegisterUriSchemeIn(const std::string& scheme, const std::string& name, const std::string& app, const fs::path& exe, const fs::path& data_home,
                         const fs::path& config_home, const std::vector<fs::path>& data_dirs);
#endif

} // namespace webui

#endif // BITCOIN_WEBUI_URISCHEME_H
