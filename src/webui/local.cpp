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
#include <uint256.h>
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
        // What the wallet does, transactions it signs: confirmed on the desktop (ConfirmedRpcMethods, below).
    };
}

std::set<std::string> KeyedRpcMethods()
{
    // They sign the nonce of a reservation to tell it (src/wallet/rpc/bitassets.cpp ReadyWallet): the passphrase of a locked
    // wallet asked by the host.
    return {"listmyassets", "listassetactivity"};
}

std::map<std::string, confirm::Method> ConfirmedRpcMethods()
{
    // What the pages do that pays or signs: never the page's to call by itself. The host shows each call on the desktop, its
    // destinations whole first, then its amounts, then the rest of its parameters, with the passphrase of a locked wallet
    // typed there, and runs it with the keys unlocked for it alone (confirm.h, audit 7 #1).
    return {
        {"reserveasset", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Reserve an asset", "Reserve", "The wallet pays for a reservation of an asset name."), /*fee_unknown=*/true}},
        {"registerasset", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Register an asset", "Register", "The wallet registers an asset it reserved, with the supply shown."), /*fee_unknown=*/true}},
        {"releaseassetreservation", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Release a reservation", "Release", "The wallet gives up a reservation of an asset name."), /*fee_unknown=*/true}},
        {"sendasset", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Send an asset", "Send", "The wallet sends units of an asset to the address shown.", {"address"}, {"amount"}), /*fee_unknown=*/true}},
        {"mintasset", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Mint an asset", "Mint", "The wallet mints more units of an asset it controls.", {"address"}, {"amount"}), /*fee_unknown=*/true}},
        {"burnasset", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Burn an asset", "Burn", "The wallet destroys units of an asset: they cannot come back.", {}, {"amount"}), /*fee_unknown=*/true}},
        {"updateasset", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Update an asset", "Update", "The wallet signs a change of an asset it controls."), /*fee_unknown=*/true}},
        {"transferassetcontrol", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Transfer the control of an asset", "Transfer", "The wallet gives the control of an asset to the address shown: it cannot be taken back.", {"address"}, {}), /*fee_unknown=*/true}},
        {"fixassetsupply", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Fix the supply of an asset", "Fix the supply", "The wallet fixes the supply of an asset for good: no more can ever be minted."), /*fee_unknown=*/true}},
        {"releaseasset", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Release an asset", "Release", "The wallet gives up an asset it controls."), /*fee_unknown=*/true}},
        {"swapasset", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Swap", "Swap", "The wallet trades an asset in a pool, for at least what is shown (min_out): refused, not signed for less, should the pool have moved.", {}, {"amount_in", "min_out"}), /*fee_unknown=*/true}},
        {"addliquidity", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Add liquidity", "Add", "The wallet puts both assets into a pool, for shares of it.", {}, {"amount_a", "amount_b"}), /*fee_unknown=*/true}},
        {"removeliquidity", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Remove liquidity", "Remove", "The wallet takes its shares out of a pool, for both assets."), /*fee_unknown=*/true}},
        {"createauction", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Create an auction", "Create", "The wallet puts units of an asset up for auction.", {}, {"amount"}), /*fee_unknown=*/true}},
        {"bidauction", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Bid in an auction", "Bid", "The wallet pays a bid in an auction.", {}, {"amount"}), /*fee_unknown=*/true}},
        {"collectauction", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Collect an auction", "Collect", "The wallet collects what an auction owes it."), /*fee_unknown=*/true}},
    };
}

void RegisterHost(Bridge& bridge, NodeFn node, guard::ConfirmFn confirm, confirm::KeysFn keys)
{
    (void)keys;
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

std::vector<std::string> CoinbaseRoles(interfaces::Chain& chain, const uint256& block)
{
    // For example (FastBlocks): {"staked", "bmm"} for a coinbase whose first output is the producer's share
    // and whose second is the fee pot paid to a BMM bidder.
    (void)chain;
    (void)block;
    return {};
}

} // namespace webui::local
