// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The handler of the app's links on a freedesktop.org desktop (Linux): a desktop entry of this user and the default
// handler of x-scheme-handler/<scheme> in mimeapps.list, as the XDG specifications say, written by the app itself on
// its start (the release is a tarball to unpack anywhere: no package installs a system entry). Files only: no program
// is run (no xdg-mime, no shell).

#include <webui/urischeme.h>

#include <logging.h>
#include <tinyformat.h>
#include <util/fs.h>
#include <webui/chainconfig.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#ifndef WIN32
#include <unistd.h>
#endif

namespace webui {
namespace {
/** A path in the Exec key of a desktop entry: quoted, with ", `, $ and \ escaped, and % doubled. */
std::string ExecQuoted(const std::string& path)
{
    std::string out{"\""};
    for (const char c : path) {
        if (c == '"' || c == '`' || c == '$' || c == '\\') {
            // Escaped for the quoting rules of Exec, then the backslash itself escaped for a string value.
            out += "\\\\";
            out += c;
        } else if (c == '%') {
            out += "%%";
        } else {
            out += c;
        }
    }
    return out + "\"";
}

std::string ReadFile(const fs::path& path)
{
    std::ifstream file{path.std_path(), std::ios::binary};
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

/** Write a file whole, or nothing: a temporary file beside it, renamed over it. */
bool WriteFile(const fs::path& path, const std::string& text)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const fs::path temp{fs::PathFromString(fs::PathToString(path) + ".new")};
    {
        std::ofstream file{temp.std_path(), std::ios::binary | std::ios::trunc};
        file << text;
        if (!file.good()) return false;
    }
    std::filesystem::rename(temp, path, ec);
    return !ec;
}

/** The value of a key of a section of an ini file (desktop entries, mimeapps.list); empty if none. */
std::string IniValue(const std::string& text, const std::string& section, const std::string& key)
{
    std::istringstream lines{text};
    std::string line;
    bool in{false};
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line.front() == '[') {
            in = line == "[" + section + "]";
            continue;
        }
        if (in && line.starts_with(key + "=")) return line.substr(key.size() + 1);
    }
    return {};
}

/** The text of an ini file with a key of a section set (the section added if it is not there), the rest kept as it was. */
std::string IniSet(const std::string& text, const std::string& section, const std::string& key, const std::string& value)
{
    std::istringstream lines{text};
    std::string line, out;
    bool in{false}, found_section{false}, done{false};
    while (std::getline(lines, line)) {
        std::string bare{line};
        if (!bare.empty() && bare.back() == '\r') bare.pop_back();
        if (!bare.empty() && bare.front() == '[') {
            if (in && !done) {
                out += key + "=" + value + "\n";
                done = true;
            }
            in = bare == "[" + section + "]";
            found_section |= in;
        } else if (in && bare.starts_with(key + "=")) {
            if (!done) out += key + "=" + value + "\n";
            done = true;
            continue;
        }
        out += line + "\n";
    }
    if (!done) {
        if (!found_section) out += (out.empty() || out.ends_with("\n\n") ? "" : out.ends_with("\n") ? "\n" : "\n\n") + std::string{"["} + section + "]\n";
        out += key + "=" + value + "\n";
    }
    return out;
}

/** The program of an Exec key as ExecQuoted writes it ("<path>" %u), unescaped; empty for anything else. */
std::string ExecProgram(const std::string& exec)
{
    if (exec.empty() || exec.front() != '"') return {};
    std::string out;
    for (size_t i{1}; i < exec.size(); ++i) {
        const char c{exec[i]};
        if (c == '"') return out;
        if (c == '\\' && i + 2 < exec.size() && exec[i + 1] == '\\') {
            out += exec[i + 2];
            i += 2;
        } else if (c == '%' && i + 1 < exec.size() && exec[i + 1] == '%') {
            out += '%';
            ++i;
        } else {
            out += c;
        }
    }
    return {};
}

bool Exists(const fs::path& path)
{
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

fs::path EnvPath(const char* name, const fs::path& fallback)
{
    const char* value{std::getenv(name)};
    // A relative path is invalid (the specification): the default then.
    return value && *value == '/' ? fs::PathFromString(value) : fallback;
}
} // namespace

bool RegisterUriSchemeIn(const std::string& scheme, const std::string& name, const std::string& app, const fs::path& exe, const fs::path& data_home,
                         const fs::path& config_home, const std::vector<fs::path>& data_dirs)
{
    const std::string id{app + ".desktop"};
    const std::string mime{"x-scheme-handler/" + scheme};
    const fs::path mimeapps{config_home / "mimeapps.list"};
    const std::string list{ReadFile(mimeapps)};
    // The handler set now: the first entry of the list.
    std::string current{IniValue(list, "Default Applications", mime)};
    current = current.substr(0, current.find(';'));
    if (!current.empty() && current != id) {
        // Another entry the user chose: kept while it exists.
        bool exists{Exists(data_home / "applications" / fs::PathFromString(current))};
        for (const fs::path& dir : data_dirs) exists |= Exists(dir / "applications" / fs::PathFromString(current));
        if (exists) return false;
    }
    const std::string entry{strprintf("[Desktop Entry]\n"
                                      "Type=Application\n"
                                      "Name=%s\n"
                                      "GenericName=%s node and wallet\n"
                                      "Comment=The node of %s, with its wallet; opens its links (%s:)\n"
                                      "Exec=%s %%u\n"
                                      "Icon=%s\n"
                                      "Terminal=false\n"
                                      "Categories=Office;Finance;Network;\n"
                                      "MimeType=%s;\n"
                                      "StartupWMClass=%s\n",
                                      name, name, name, scheme, ExecQuoted(fs::PathToString(exe)), std::string{chain::STEM}, mime, app)};
    const fs::path desktop{data_home / "applications" / fs::PathFromString(id)};
    // Our entry, written by another installation of the app (another folder) that still exists: kept, as on Windows.
    const std::string written{ReadFile(desktop)};
    if (current == id && !written.empty() && written != entry) {
        const std::string other{ExecProgram(IniValue(written, "Desktop Entry", "Exec"))};
        if (!other.empty() && other != fs::PathToString(exe) && Exists(fs::PathFromString(other))) return false;
    }
    if (written != entry && !WriteFile(desktop, entry)) {
        LogWarning("webui: cannot write %s", fs::PathToString(desktop));
        return false;
    }
    if (current != id && !WriteFile(mimeapps, IniSet(list, "Default Applications", mime, id))) {
        LogWarning("webui: cannot write %s", fs::PathToString(mimeapps));
        return false;
    }
    LogInfo("webui: %s: links open with this program (%s)", scheme, fs::PathToString(desktop));
    return true;
}

#ifndef WIN32
bool RegisterUriScheme(const std::string& scheme, const std::string& description)
{
    const char* home{std::getenv("HOME")};
    if (!home || *home != '/') return false;
    std::error_code ec;
    const fs::path exe{std::filesystem::read_symlink("/proc/self/exe", ec)};
    if (ec || exe.empty()) return false;
    const fs::path data_home{EnvPath("XDG_DATA_HOME", fs::PathFromString(home) / ".local" / "share")};
    const fs::path config_home{EnvPath("XDG_CONFIG_HOME", fs::PathFromString(home) / ".config")};
    std::vector<fs::path> data_dirs;
    const char* dirs{std::getenv("XDG_DATA_DIRS")};
    std::string list{dirs && *dirs ? dirs : "/usr/local/share:/usr/share"};
    size_t start{0};
    while (start <= list.size()) {
        const size_t colon{list.find(':', start)};
        const std::string dir{list.substr(start, colon == std::string::npos ? std::string::npos : colon - start)};
        if (!dir.empty() && dir.front() == '/') data_dirs.push_back(fs::PathFromString(dir));
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return RegisterUriSchemeIn(scheme, description, std::string{chain::APP}, exe, data_home, config_home, data_dirs);
}
#endif

} // namespace webui
