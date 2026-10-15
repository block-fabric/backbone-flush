// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The host side of the pages of BitAssets (src/webui/app/src/chain/local.ts): the RPC methods of the assets,
// the pools and the auctions they call (src/rpc/bitassets.cpp, src/wallet/rpc/bitassets.cpp), and the journal
// of what the chain does not keep (the assets being created, the prices of the pools as the app saw them),
// in a file of the data directory.

#include <webui/local.h>

#include <common/args.h>
#include <interfaces/node.h>
#include <node/context.h>
#include <tinyformat.h>
#include <util/fs.h>
#include <util/fs_helpers.h>

#include <cstdio>
#include <fstream>
#include <sstream>

namespace webui::local {

namespace {
const UniValue& Param(const UniValue& params, const std::string& key)
{
    return params.isObject() ? params.find_value(key) : NullUniValue;
}

/** Where the journal is kept: in the data directory of the network, apart from the settings of the app. */
fs::path StorePath(interfaces::Node& node, const std::string& name)
{
    node::NodeContext* context{node.context()};
    if (!context || !context->args) throw HostError("The node is still starting");
    return context->args->GetDataDirNet() / fs::u8path(name);
}
} // namespace

UniValue ReadStore(const fs::path& path)
{
    std::ostringstream text;
    {
        // Closed before anything else: on Windows an open file cannot be renamed.
        std::ifstream file{path.std_path(), std::ios::binary};
        if (!file) return UniValue{};
        text << file.rdbuf();
    }
    UniValue value;
    if (!value.read(text.str()) || !value.isObject()) {
        // Kept aside rather than lost: the names being created, with their supply and data.
        std::error_code ec;
        fs::rename(path, fs::PathFromString(fs::PathToString(path) + ".unreadable"), ec);
        return UniValue{};
    }
    return value;
}

void WriteStore(const fs::path& path, const UniValue& value)
{
    if (!value.isObject()) throw HostError(BRIDGE_INVALID_REQUEST, "A store is an object");
    const std::string text{value.write()};
    if (text.size() > MAX_STORE_BYTES) throw HostError(strprintf("The record is too large (%u bytes; at most %u)", text.size(), MAX_STORE_BYTES));
    // Written whole to another file, flushed to the disk, then put in place: a crash leaves the old one or the new one.
    const fs::path temp{fs::PathFromString(fs::PathToString(path) + ".new")};
    FILE* file{fsbridge::fopen(temp, "wb")};
    if (!file) throw HostError(strprintf("%s cannot be written", fs::PathToString(temp)));
    const bool written{std::fwrite(text.data(), 1, text.size(), file) == text.size() && FileCommit(file)};
    if (std::fclose(file) != 0 || !written || !RenameOver(temp, path)) {
        std::error_code ec;
        fs::remove(temp, ec);
        throw HostError(strprintf("%s could not be written", fs::PathToString(path)));
    }
}

std::map<std::string, Bridge::Thread> RpcMethods()
{
    return {
        // What the node says of the assets, the pools and the auctions: read only.
        {"getbitassetsinfo", Bridge::Thread::WORKER},
        {"listassets", Bridge::Thread::WORKER},
        {"getasset", Bridge::Thread::WORKER},
        {"getassethistory", Bridge::Thread::WORKER},
        {"listpools", Bridge::Thread::WORKER},
        {"getpool", Bridge::Thread::WORKER},
        {"quoteswap", Bridge::Thread::WORKER},
        {"listauctions", Bridge::Thread::WORKER},
        {"getauction", Bridge::Thread::WORKER},
        {"quotebid", Bridge::Thread::WORKER},
        // What the wallet holds and did: they need the keys too (a reservation is told by a signature of the wallet).
        {"listmyassets", Bridge::Thread::WORKER},
        {"listassetactivity", Bridge::Thread::WORKER},
        // What the wallet does: transactions it signs (the keys, below).
        {"reserveasset", Bridge::Thread::WORKER},
        {"registerasset", Bridge::Thread::WORKER},
        {"releaseassetreservation", Bridge::Thread::WORKER},
        {"sendasset", Bridge::Thread::WORKER},
        {"mintasset", Bridge::Thread::WORKER},
        {"burnasset", Bridge::Thread::WORKER},
        {"updateasset", Bridge::Thread::WORKER},
        {"transferassetcontrol", Bridge::Thread::WORKER},
        {"fixassetsupply", Bridge::Thread::WORKER},
        {"releaseasset", Bridge::Thread::WORKER},
        {"swapasset", Bridge::Thread::WORKER},
        {"addliquidity", Bridge::Thread::WORKER},
        {"removeliquidity", Bridge::Thread::WORKER},
        {"createauction", Bridge::Thread::WORKER},
        {"bidauction", Bridge::Thread::WORKER},
        {"collectauction", Bridge::Thread::WORKER},
    };
}

std::set<std::string> KeyedRpcMethods()
{
    // Each signs a transaction, but listmyassets and listassetactivity, which sign the nonce of a reservation to tell it
    // (the wallet asks for its keys to be unlocked for them all, src/wallet/rpc/bitassets.cpp ReadyWallet).
    return {"listmyassets", "listassetactivity", "reserveasset", "registerasset", "releaseassetreservation", "sendasset", "mintasset", "burnasset",
            "updateasset", "transferassetcontrol", "fixassetsupply", "releaseasset", "swapasset", "addliquidity", "removeliquidity",
            "createauction", "bidauction", "collectauction"};
}

void RegisterHost(Bridge& bridge, NodeFn node, guard::ConfirmFn confirm)
{
    // The journal of the app: the assets being created (what to register once each reservation is deep enough), and
    // the prices of the pools as the app saw them; in a file of the data directory, with a sanity cap.
    bridge.RegisterHost("bitassets.store.load", [node](const UniValue&) {
        return ReadStore(StorePath(node(), JOURNAL_FILE));
    }, Bridge::Thread::LONG);
    bridge.RegisterHost("bitassets.store.save", [node](const UniValue& params) {
        WriteStore(StorePath(node(), JOURNAL_FILE), Param(params, "value"));
        return UniValue{true};
    }, Bridge::Thread::LONG);
    (void)confirm;
}

} // namespace webui::local
