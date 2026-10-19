// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WEBUI_LOCAL_H
#define BITCOIN_WEBUI_LOCAL_H

#include <univalue.h>
#include <util/fs.h>
#include <webui/bridge.h>
#include <webui/confirm.h>
#include <webui/guard.h>

#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <set>
#include <string>
#include <vector>

class uint256;
namespace interfaces {
class Chain;
class Node;
} // namespace interfaces

/**
 * What a chain adds to the host for its own pages (the pages of src/webui/app/src/chain/local.ts), in
 * local.cpp, the one host file a chain made from the template changes: the kit itself stays as it is, so
 * merges of the template stay clean. See doc/webui.md, "Pages of a chain of its own".
 */
namespace webui::local {

/** RPC methods the chain's pages call, added to the page's allowlist (rpcpolicy.cpp): what reads. */
std::map<std::string, Bridge::Thread> RpcMethods();

/**
 * Of these, the wallet methods that need the keys to read (or to make a key), not to spend: a locked wallet answers -13,
 * and the page asks the host (rpc.confirmed), which asks for the passphrase in its own dialog (confirm.h, KEYS).
 */
std::set<std::string> KeyedRpcMethods();

/**
 * The RPC methods of the chain's pages that pay, sign or change what the node does (a registration, a trade, a vote, a
 * bond): never in the page's allowlist. The page calls them through the host (rpc.confirmed; the kit's walletRpc does), which
 * describes each from its parameters (the describer, confirm::Generic does most: destinations first and whole, then amounts),
 * asks the user in the dialog window of the host with the passphrase of a locked wallet typed there, and only then runs it
 * with the keys unlocked for it alone (audit 7 #1). confirm::Method::Kind SPEND for a wallet's, NODE for the node's.
 */
std::map<std::string, confirm::Method> ConfirmedRpcMethods();

/** A logo of an asset (bitassets/logo.h) as the host shows it: checked by the rules' own check. */
struct Logo {
    std::string mime;
    size_t size{0};
    //! SHA-256 of the bytes, as sha256sum prints it.
    std::string hash;
    //! "data:<mime>;base64,...": for an <img> only.
    std::string url;
};
/** A logo from its bytes, if the rules take it; else why (`reason`). */
std::optional<Logo> ReadLogo(std::span<const unsigned char> bytes, std::string& reason);
/** A logo given in base64 (a parameter of setassetlogo, registerasset), if the rules take it. */
std::optional<Logo> ReadLogoParam(const UniValue& value, std::string& reason);
/** The rows of a confirmation that show a logo: the picture (dialog::Row::Kind::IMAGE), its file and its hash. */
std::vector<dialog::Row> LogoRows(const Logo& logo);

/** The logos the pages asked for (bitassets.logo), newest first: at most MAX_ENTRIES, MAX_BYTES of URLs. */
class LogoCache
{
public:
    static constexpr size_t MAX_ENTRIES{512};
    static constexpr size_t MAX_BYTES{16 << 20};
    /** The logo of an asset (its id, 64 hex digits) as JSON {asset, hash, mime, size, url}, null if it has none; `hash` if known. */
    UniValue Get(interfaces::Node& node, const std::string& asset, const std::string& hash);
    static UniValue ToJson(const std::string& asset, const Logo& logo);

private:
    struct Entry {
        std::string asset;
        Logo logo;
    };
    std::mutex m_mutex;
    std::list<Entry> m_entries;
    size_t m_bytes{0};
};

/** The node, while it runs (throws a HostError before); the host methods take it per call. */
using NodeFn = std::function<interfaces::Node&()>;

/**
 * Host methods of the chain's pages ("<chain>.something"), registered after the kit's. `confirm` asks the
 * user in the dialog window of the host (App::Confirm, dialog.h: in the look of the app, outside the page): what a
 * page must not do on its own word, such as showing a key, asks there first and answers BRIDGE_CANCELLED on a no
 * (audit port-2 #3). A Window::ConfirmDialog is all it takes (its level may say DANGER). `keys` runs a call with
 * the keys of a wallet, the passphrase of a locked one typed in that dialog, never given by the page (audit 7 #1):
 * keys(wallet, dialog, always, fee_unknown, run), as rpc.confirmed does (confirm.h KeysFn); empty without wallets.
 */
void RegisterHost(Bridge& bridge, NodeFn node, guard::ConfirmFn confirm, confirm::KeysFn keys);

/**
 * BitAssets: the journal of the app (bitassets.store.load, bitassets.store.save), a JSON object in a file of
 * the data directory of the network, written whole and flushed before it replaces the old one: the assets
 * being created (the name, supply and data to register once the reservation is deep enough, as chains-qt
 * kept them in QSettings) and the prices of the pools as the app saw them (the chain keeps no history of them).
 */
inline constexpr const char* JOURNAL_FILE{"bitassets-journal.json"};
inline constexpr size_t MAX_STORE_BYTES{32 << 20};
/** The object of a store; null when there is none (or it could not be read: then kept aside as <file>.unreadable). */
UniValue ReadStore(const fs::path& path);
/** Write a store (an object). Throws HostError. */
void WriteStore(const fs::path& path, const UniValue& value);

/**
 * What each output of the coinbase of a block of the wallet pays, by the chain's own words (a short id,
 * "" for an output it has nothing to say of): the history shows such an output by its role rather than
 * "Mined" (chain/roles.ts, COINBASE_ROLES). The template has none: empty.
 */
std::vector<std::string> CoinbaseRoles(interfaces::Chain& chain, const uint256& block);

} // namespace webui::local

#endif // BITCOIN_WEBUI_LOCAL_H
