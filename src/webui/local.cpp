// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The host side of the pages of Layers (src/webui/app/src/chain/local.ts): FastBlocks' and BitAssets'.
//
// FastBlocks: the RPC methods of its blocks of stake the pages read (src/rpc/pos.cpp, src/wallet/rpc/sidechain.cpp),
// and what they do that must not happen on the page's word alone: bonding and unbonding coins of the wallet, turning
// the making of blocks of stake off, signing a proof of possession with a validator key. Each of these asks the
// desktop first (Window::Confirm, outside the page), with the key and the amount in full, and only then calls the node.
//
// BitAssets: the RPC methods of the assets, the pools and the auctions they call (src/rpc/bitassets.cpp,
// src/wallet/rpc/bitassets.cpp), those that pay or sign confirmed on the desktop (ConfirmedRpcMethods), and the
// journal of what the chain does not keep (the assets being created, the prices of the pools as the app saw them),
// in a file of the data directory.

#include <policy/feerate.h>
#include <webui/local.h>

#include <addresstype.h>
#include <bitassets/logo.h>
#include <bitassets/rpcutil.h>
#include <bitassets/state.h>
#include <chainparams.h>
#include <common/args.h>
#include <consensus/amount.h>
#include <crypto/sha256.h>
#include <interfaces/chain.h>
#include <util/strencodings.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <node/context.h>
#include <pos/hooks.h>
#include <rpc/util.h>
#include <tinyformat.h>
#include <uint256.h>
#include <univalue.h>
#include <util/fs.h>
#include <util/fs_helpers.h>
#include <util/chaintype.h>
#include <util/moneystr.h>
#include <util/strencodings.h>
#include <webui/chainconfig.h>
#include <webui/walletapi.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <set>
#include <string>

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

std::optional<Logo> ReadLogo(std::span<const unsigned char> bytes, std::string& reason)
{
    const auto format{bitassets::CheckLogo(bytes, reason)};
    if (!format) return std::nullopt;
    Logo logo;
    logo.mime = bitassets::LogoMime(*format);
    logo.size = bytes.size();
    uint256 hash;
    CSHA256().Write(bytes.data(), bytes.size()).Finalize(hash.data());
    logo.hash = HexStr(hash);
    // The type is the one the rules found, never one the chain or the page says.
    logo.url = "data:" + logo.mime + ";base64," + EncodeBase64(bytes);
    return logo;
}

std::optional<Logo> ReadLogoParam(const UniValue& value, std::string& reason)
{
    if (!value.isStr()) {
        reason = "not a string";
        return std::nullopt;
    }
    if (value.get_str().size() > (bitassets::MAX_LOGO_BYTES + 2) / 3 * 4) {
        reason = "bad-ba-logo-oversize";
        return std::nullopt;
    }
    const auto bytes{DecodeBase64(value.get_str())};
    if (!bytes) {
        reason = "not base64";
        return std::nullopt;
    }
    return ReadLogo(*bytes, reason);
}

std::vector<dialog::Row> LogoRows(const Logo& logo)
{
    return {
        confirm::Image("Logo", logo.url),
        confirm::Text("Logo file", strprintf("%s, %ux%u pixels, %u bytes", logo.mime == "image/png" ? "PNG" : "JPEG", bitassets::LOGO_SIZE, bitassets::LOGO_SIZE, logo.size)),
        confirm::Text("Logo SHA-256", logo.hash),
    };
}

UniValue LogoCache::Get(interfaces::Node& node, const std::string& asset, const std::string& hash)
{
    if (asset.size() != 64 || !IsHex(asset)) throw HostError(BRIDGE_INVALID_REQUEST, "asset: the id of an asset (64 hex digits)");
    {
        std::lock_guard lock{m_mutex};
        for (auto it{m_entries.begin()}; it != m_entries.end(); ++it) {
            if (it->asset != asset || (!hash.empty() && it->logo.hash != hash)) continue;
            m_entries.splice(m_entries.begin(), m_entries, it);
            return ToJson(asset, it->logo);
        }
    }
    UniValue args{UniValue::VARR};
    args.push_back("0x" + asset);
    UniValue result;
    try {
        result = node.executeRpc("getassetlogo", args, "/");
    } catch (const UniValue&) {
        return UniValue{}; // no logo (or no such asset)
    } catch (const std::exception&) {
        return UniValue{};
    }
    if (!result.isObject() || !result.find_value("base64").isStr()) return UniValue{};
    std::string reason;
    const auto logo{ReadLogoParam(result.find_value("base64"), reason)};
    // Checked again here, as the rules check it: what the page gets is a picture of PNG or JPEG bytes, or nothing.
    if (!logo) return UniValue{};
    Put(asset, *logo);
    return ToJson(asset, *logo);
}

void LogoCache::Put(const std::string& asset, const Logo& logo)
{
    std::lock_guard lock{m_mutex};
    // The one before of this asset goes, its bytes counted out with it.
    for (auto it{m_entries.begin()}; it != m_entries.end();) {
        if (it->asset == asset) {
            m_bytes -= it->logo.url.size();
            it = m_entries.erase(it);
        } else {
            ++it;
        }
    }
    // Larger than the whole cache (no logo of the rules is: at most 44 KiB of URL): given, not kept.
    if (logo.url.size() > MAX_BYTES) return;
    m_entries.push_front({asset, logo});
    m_bytes += logo.url.size();
    while (!m_entries.empty() && (m_entries.size() > MAX_ENTRIES || m_bytes > MAX_BYTES)) {
        m_bytes -= m_entries.back().logo.url.size();
        m_entries.pop_back();
    }
}

size_t LogoCache::Bytes() const
{
    std::lock_guard lock{m_mutex};
    return m_bytes;
}

size_t LogoCache::Size() const
{
    std::lock_guard lock{m_mutex};
    return m_entries.size();
}

size_t LogoCache::CountedBytes() const
{
    std::lock_guard lock{m_mutex};
    size_t n{0};
    for (const Entry& e : m_entries) n += e.logo.url.size();
    return n;
}

UniValue LogoCache::ToJson(const std::string& asset, const Logo& logo)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("asset", asset);
    out.pushKV("hash", logo.hash);
    out.pushKV("mime", logo.mime);
    out.pushKV("size", static_cast<uint64_t>(logo.size));
    out.pushKV("url", logo.url);
    return out;
}

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
        // What the node says of the blocks of stake: read only.
        {"getposinfo", Bridge::Thread::WORKER},
        {"getstakinginfo", Bridge::Thread::WORKER},
        {"getvalidators", Bridge::Thread::WORKER},
        {"getvalidator", Bridge::Thread::WORKER},
        {"getleaderschedule", Bridge::Thread::WORKER},
        {"getanchorinfo", Bridge::Thread::WORKER},
        // What a mainchain commitment of this chain is (a ticket: the block it anchors), for Merged mining's table.
        {"getanchorticket", Bridge::Thread::WORKER},
        // The validator's anchor bid policy: read here; changed through setanchorbidpolicy, which spends the mainchain
        // wallet and is confirmed on the desktop (ConfirmedRpcMethods).
        {"getanchorbidpolicy", Bridge::Thread::WORKER},
        {"listevidence", Bridge::Thread::WORKER},
        // A proof that a leader signed two blocks for a slot, given by the user: the node checks it, keeps it and
        // relays it (it moves no coins; a wrong one is refused).
        {"submitevidence", Bridge::Thread::WORKER},
        // The bonds of the wallet (and, with all, of every validator): read only.
        {"listbonds", Bridge::Thread::WORKER},
        // Not createbond, createunbond, setstaking or signbondpop: fastblocks.bond, fastblocks.unbond,
        // fastblocks.staking and fastblocks.pop ask the desktop first (below).
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
        // Layers' DEX: its bounds, routes, prices over time, and the pool kinds' quotes, previews and depth: read only.
        {"getdexinfo", Bridge::Thread::WORKER},
        {"quoteroute", Bridge::Thread::WORKER},
        {"getpooltwap", Bridge::Thread::WORKER},
        {"getswapquote", Bridge::Thread::WORKER},
        {"simulateclearing", Bridge::Thread::WORKER},
        {"previewpool", Bridge::Thread::WORKER},
        {"getpoolcurve", Bridge::Thread::WORKER},
        {"quotepoolliquidity", Bridge::Thread::WORKER},
        // Launches: read only.
        {"getlaunch", Bridge::Thread::WORKER},
        {"listlaunches", Bridge::Thread::WORKER},
        {"quotelaunch", Bridge::Thread::WORKER},
        {"getlaunchstats", Bridge::Thread::WORKER},
        {"getlaunchlog", Bridge::Thread::WORKER},
        // Concentrated liquidity: positions, ticks, previews: read only.
        {"getposition", Bridge::Thread::WORKER},
        {"getpoolticks", Bridge::Thread::WORKER},
        {"previewposition", Bridge::Thread::WORKER},
        {"listpositions", Bridge::Thread::WORKER},
        // Order books: read only.
        {"getbook", Bridge::Thread::WORKER},
        {"getpoolcandles", Bridge::Thread::WORKER},
        {"getorder", Bridge::Thread::WORKER},
        {"listorders", Bridge::Thread::WORKER},
        {"getverifications", Bridge::Thread::WORKER},
        {"listoracles", Bridge::Thread::WORKER},
        // Searches of the node, a page at a time (cursor; the pages never read a whole list: user feedback 2026-10-19).
        // A node without them answers "method not found", and the pages ask the list methods above instead.
        {"searchassets", Bridge::Thread::WORKER},
        {"searchpools", Bridge::Thread::WORKER},
        {"searchauctions", Bridge::Thread::WORKER},
        // What the wallet holds and did: they need the keys too (a reservation is told by a signature of the wallet).
        {"listmyassets", Bridge::Thread::WORKER},
        // Browse assets' "Held or controlled": searchassets among the wallet's assets (the same arguments; no key).
        {"searchmyassets", Bridge::Thread::WORKER},
        {"listassetactivity", Bridge::Thread::WORKER},
        // The asset moves of the wallet with the other side: it reads only (no key).
        {"listassettransactions", Bridge::Thread::WORKER},
        // What the wallet does, transactions it signs: confirmed on the desktop (ConfirmedRpcMethods, below).
    };
}

std::set<std::string> KeyedRpcMethods()
{
    // They sign the nonce of a reservation to tell it (src/wallet/rpc/bitassets.cpp ReadyWallet): the passphrase of a locked
    // wallet asked by the host.
    return {"listmyassets", "listassetactivity"};
}

namespace {
std::string Text(const UniValue& params, const std::string& key, size_t max = 1024)
{
    const UniValue& value{Param(params, key)};
    if (!value.isStr() || value.get_str().size() > max) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("%s: a text", key));
    return value.get_str();
}

/** A validator key as the RPCs take it: an x-only key, 64 hex digits (lower case). */
std::string ValidatorKey(const UniValue& params)
{
    std::string key{Text(params, "pubkey", 64)};
    if (key.size() != 64 || !IsHex(key)) throw HostError(BRIDGE_INVALID_REQUEST, "A validator key is an x-only key: 64 hexadecimal digits.");
    for (char& c : key) c = ToLower(c);
    return key;
}

/** An amount of coins the user typed, as the RPCs take it ("1.5"): positive, at most 8 decimals. */
CAmount Coins(const std::string& text)
{
    const auto amount{ParseMoney(text)};
    if (!amount || *amount <= 0 || !MoneyRange(*amount)) throw HostError(BRIDGE_INVALID_REQUEST, "Enter the amount as a positive number, like 1.5.");
    return *amount;
}

std::string Shown(CAmount amount)
{
    return strprintf("%s %s", FormatMoney(amount), std::string{chain::TICKER});
}

/** The key, cut in groups of eight for a dialog: read whole, not taken for another by its ends. */
std::string Grouped(const std::string& key)
{
    std::string out;
    for (size_t i{0}; i < key.size(); i += 8) {
        if (i) out += i == 32 ? "\n" : " ";
        out += key.substr(i, 8);
    }
    return out;
}

/** A wallet that is loaded, by its name. */
std::unique_ptr<interfaces::Wallet> WalletNamed(interfaces::Node& node, const std::string& name)
{
    for (auto& wallet : node.walletLoader().getWallets()) {
        if (wallet->getWalletName() == name) return std::move(wallet);
    }
    throw HostError(-18, "This wallet is not loaded");
}

/** The wallet's bond to a validator key (listbonds), if it has one. */
std::optional<UniValue> WalletBond(interfaces::Node& node, const std::string& wallet, const std::string& key)
{
    const UniValue bonds{node.executeRpc("listbonds", UniValue{UniValue::VARR}, WalletUri(wallet))};
    for (const UniValue& bond : bonds.getValues()) {
        if (bond.isObject() && bond.find_value("validator").isStr() && bond.find_value("validator").get_str() == key) return bond;
    }
    return std::nullopt;
}

CAmount AmountOf(const UniValue& value)
{
    return value.isNum() ? AmountFromValue(value) : 0;
}

/** How many anchors an unbond waits (getposinfo staking.unbondinganchors). */
int UnbondingAnchors(interfaces::Node& node)
{
    const UniValue info{node.executeRpc("getposinfo", UniValue{UniValue::VARR}, "/")};
    const UniValue& staking{info.find_value("staking")};
    const UniValue& anchors{staking.isObject() ? staking.find_value("unbondinganchors") : NullUniValue};
    return anchors.isNum() ? anchors.getInt<int>() : 0;
}

//! About how long `anchors` BMM commitments take: one a mainchain block, ten minutes on the eCash networks, a minute on Chains'.
std::string AnchorsTime(int anchors)
{
    const int64_t seconds{int64_t{anchors} * (IsEcashChain(Params().GetChainType()) ? 600 : 60)};
    if (seconds >= 2 * 86400) return strprintf("about %d days", (seconds + 43200) / 86400);
    if (seconds >= 2 * 3600) return strprintf("about %d hours", (seconds + 1800) / 3600);
    return strprintf("about %d minutes", (seconds + 30) / 60);
}

void Ask(const guard::ConfirmFn& confirm, const Window::ConfirmDialog& dialog, const std::string& declined)
{
    if (!confirm || !confirm(dialog)) throw HostError(BRIDGE_CANCELLED, declined);
}

Window::ConfirmDialog BondConfirmation(const std::string& wallet, const std::string& key, CAmount amount, const std::optional<UniValue>& existing, int unbonding_anchors, bool other_node)
{
    Window::ConfirmDialog dialog;
    dialog.kind = "bond";
    // The facts, decoded by the host (audit 7 #1): the key whole, then the amount.
    dialog.rows = confirm::WalletRows(wallet);
    dialog.rows.push_back(confirm::Destination("Validator key", key));
    if (existing) dialog.rows.push_back(confirm::Amount("Bonded now", AmountOf(existing->find_value("bonded"))));
    dialog.rows.push_back(confirm::Amount(existing ? "Added" : "Amount", amount));
    dialog.title = existing ? "Add to a bond" : "Bond coins";
    const std::string what{existing ? strprintf("Add %s to this wallet's bond of %s to the validator key", Shown(amount), Shown(AmountOf(existing->find_value("bonded")))) :
                                      strprintf("Bond %s of this wallet to the validator key", Shown(amount))};
    dialog.message = strprintf("Wallet: %s\n\n%s\n\n%s\n\n"
                               "The coins leave this wallet now. The key takes part in the validator sets from the epoch after next, while its "
                               "bond is at least the minimum stake. Taking them back (unbonding) pays them %d BMM commitments later (%s).%s",
                               guard::OneLine(wallet, 64), what, Grouped(key), unbonding_anchors, AnchorsTime(unbonding_anchors),
                               other_node ? "\n\nThe key is not one of this node's: its own node signed the proof of possession given." : "");
    dialog.accept = existing ? "Add to the bond" : "Bond";
    return dialog;
}

Window::ConfirmDialog UnbondConfirmation(const std::string& wallet, const std::string& key, std::optional<CAmount> amount, const UniValue& bond, int unbonding_anchors)
{
    Window::ConfirmDialog dialog;
    dialog.title = "Unbond";
    dialog.kind = "unbond";
    const CAmount bonded{AmountOf(bond.find_value("bonded"))};
    dialog.rows = confirm::WalletRows(wallet);
    dialog.rows.push_back(confirm::Destination("Validator key", key));
    if (bond.find_value("owner").isStr()) dialog.rows.push_back(confirm::Destination("Paid back to", bond.find_value("owner").get_str()));
    dialog.rows.push_back(confirm::Amount("Bonded now", bonded));
    if (amount) dialog.rows.push_back(confirm::Amount("Unbonded", *amount));
    else dialog.rows.push_back(confirm::Text("Unbonded", "all of it"));
    const std::string what{amount ? strprintf("Unbond %s of the %s bonded to the validator key", Shown(*amount), Shown(bonded)) :
                                    strprintf("Unbond all of the bond to the validator key (%s now, and whatever is added before the unbond is mined)", Shown(bonded))};
    const UniValue& owner{bond.find_value("owner")};
    dialog.message = strprintf("Wallet: %s\n\n%s\n\n%s\n\n"
                               "The coins leave the validator's stake at once. They are paid back to the bond's owner, %s, %d BMM commitments "
                               "later (%s); until then a proof that the validator signed two blocks for one "
                               "slot can still take a share of them.",
                               guard::OneLine(wallet, 64), what, Grouped(key), owner.isStr() ? guard::OneLine(owner.get_str(), 90) : std::string{"this wallet"}, unbonding_anchors, AnchorsTime(unbonding_anchors));
    dialog.accept = "Unbond";
    // The stake leaves the validator at once and earns nothing until it is paid back (user decision: danger).
    dialog.level = dialog::Level::DANGER;
    return dialog;
}

Window::ConfirmDialog StopStakingConfirmation()
{
    Window::ConfirmDialog dialog;
    dialog.title = "Stop staking";
    dialog.message = "This node stops making blocks of stake at the slots its validator keys lead, until you start it again (or the app "
                     "restarts).\n\nA validator that misses its slots is jailed for a while: left out of the validator sets of the next "
                     "epochs. No coins are lost, but it earns nothing meanwhile.";
    dialog.accept = "Stop staking";
    // A validator that misses its slots is jailed (user decision: danger).
    dialog.level = dialog::Level::DANGER;
    return dialog;
}

Window::ConfirmDialog PopConfirmation(const std::string& owner, const std::string& key)
{
    Window::ConfirmDialog dialog;
    dialog.title = "Sign a proof of possession";
    dialog.message = strprintf("Sign, with this node's validator key\n\n%s\n\nthe proof a bond owned by the address\n\n%s\n\nneeds. "
                               "With it, the wallet that holds that address can bond its coins to this key, and alone take them back. "
                               "Sign it only for an address you trust, such as one of your own wallets on another node.",
                               key.empty() ? std::string{"(its first key)"} : Grouped(key), guard::OneLine(owner, 90));
    dialog.accept = "Sign";
    return dialog;
}

/** The second step of reserveasset: registerasset of the same name, [name, supply, decimals, data, public]. */
confirm::Reveal RegisterAfterReserve()
{
    confirm::Reveal reveal;
    reveal.method = "registerasset";
    reveal.title = "Create an asset";
    reveal.accept = "Reserve and register";
    reveal.first_label = "Reservation";
    reveal.second_label = "Registration";
    reveal.what = "Two transactions: the reservation hides the name until it is deep enough, so that nobody who sees it coming can "
                  "take it first; the registration then creates the asset with the supply and data below, and its control coin.";
    reveal.matches = confirm::SameFirstParam;
    // A registration spends the reservation and makes the control coin, the supply and the change; its data adds to it.
    reveal.second_vsize = [](const UniValue& params) {
        // Its logo, if any, goes whole into the marker: a byte for a virtual byte.
        const int64_t logo{params.size() > 5 && params[5].isStr() ? static_cast<int64_t>(params[5].get_str().size() * 3 / 4) : 0};
        // And its display name (at most 64 bytes, and the logo's length when it has none).
        const int64_t display{params.size() > 6 && params[6].isStr() ? static_cast<int64_t>(params[6].get_str().size()) + 2 : 0};
        return int64_t{350} + (params.size() > 3 ? static_cast<int64_t>(params[3].write().size()) : 0) + logo + display;
    };
    reveal.when = confirm::AfterDepth("getbitassetsinfo", "reveal_depth");
    return reveal;
}
} // namespace

namespace {
/**
 * A describer that shows a logo among the parameters (`index`, base64) as a picture with its size and hash, never as text,
 * the other parameters as `generic` does; a logo the rules refuse is refused before anything is asked.
 */
std::function<std::optional<Window::ConfirmDialog>(const confirm::Call&)> WithLogo(std::function<std::optional<Window::ConfirmDialog>(const confirm::Call&)> generic, size_t index, bool remove_allowed)
{
    return [=](const confirm::Call& call) -> std::optional<Window::ConfirmDialog> {
        const UniValue value{call.Arg(index)};
        UniValue others{call.params};
        if (others.isArray() && index < others.size()) {
            UniValue trimmed{UniValue::VARR};
            for (size_t i{0}; i < others.size(); ++i) trimmed.push_back(i == index ? UniValue{} : others[i]);
            others = trimmed;
        }
        confirm::Call shown{call.node, call.wallet, call.method, others, call.fee_for};
        auto dialog{generic(shown)};
        if (!dialog) return dialog;
        if (value.isNull() || (value.isStr() && value.get_str().empty())) {
            if (remove_allowed && value.isStr()) dialog->rows.push_back(confirm::Text("Logo", "removed: the asset is shown by its initials"));
            return dialog;
        }
        std::string reason;
        const auto logo{ReadLogoParam(value, reason)};
        if (!logo) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("The rules refuse this logo (%s): a PNG or JPEG of exactly %ux%u pixels, at most %u bytes, without metadata", reason, bitassets::LOGO_SIZE, bitassets::LOGO_SIZE, bitassets::MAX_LOGO_BYTES));
        for (auto& row : LogoRows(*logo)) dialog->rows.push_back(std::move(row));
        if (call.method == "setassetlogo" && call.fee_for) {
            // The logo goes whole into the transaction: about its size, and the inputs and outputs of an update.
            if (const auto fee{call.fee_for(static_cast<int64_t>(logo->size) + 260)}) dialog->rows.push_back(confirm::Amount("Network fee, about", *fee));
        }
        return dialog;
    };
}
} // namespace

namespace {
/** Whether text is plain ASCII (a display name beyond it is told: the ticker is what tells assets apart). */
bool PlainAscii(const std::string& text)
{
    return std::all_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) < 0x7f; });
}

/**
 * A describer that shows the names of an asset as rows of their own after the command: the name (`name_index`; "Ticker"
 * when it is one, from ticker_height every new name is) and the display name (`display_index`; "removed" when empty, if
 * `remove_allowed`), the other parameters as `generic` shows them. SIZE_MAX: no such parameter.
 */
std::function<std::optional<Window::ConfirmDialog>(const confirm::Call&)> WithNames(std::function<std::optional<Window::ConfirmDialog>(const confirm::Call&)> generic, size_t name_index, size_t display_index, bool remove_allowed)
{
    return [=](const confirm::Call& call) -> std::optional<Window::ConfirmDialog> {
        const UniValue name{name_index == SIZE_MAX ? UniValue{} : call.Arg(name_index)};
        const UniValue display{display_index == SIZE_MAX ? UniValue{} : call.Arg(display_index)};
        UniValue others{call.params};
        if (others.isArray()) {
            UniValue trimmed{UniValue::VARR};
            for (size_t i{0}; i < others.size(); ++i) trimmed.push_back(i == name_index || i == display_index ? UniValue{} : others[i]);
            others = trimmed;
        }
        confirm::Call shown{call.node, call.wallet, call.method, others, call.fee_for};
        auto dialog{generic(shown)};
        if (!dialog) return dialog;
        std::vector<dialog::Row> rows;
        if (name.isStr()) {
            const std::string& text{name.get_str()};
            rows.push_back(confirm::Text(bitassets::IsTicker(text) ? "Ticker" : "Name", text));
        }
        if (display.isStr()) {
            const std::string& text{display.get_str()};
            if (text.empty()) {
                if (remove_allowed) rows.push_back(confirm::Text("Display name", "removed: the asset is shown by its ticker alone"));
            } else {
                if (const auto why{bitassets::WhyNotDisplayName(text)}) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("The rules refuse this display name: %s", *why));
                rows.push_back(confirm::Text("Display name", text));
                if (!PlainAscii(text)) rows.push_back(confirm::Note("The display name holds characters beyond plain ASCII. Display names are not unique: the ticker and the number tell assets apart."));
            }
        }
        if (call.method == "setassetdisplayname" && call.fee_for && display.isStr()) {
            // An update: its inputs and outputs, and the text.
            if (const auto fee{call.fee_for(static_cast<int64_t>(display.get_str().size()) + 260)}) rows.push_back(confirm::Amount("Network fee, about", *fee));
        }
        // After the command (the first rows: the wallet, then "Command").
        auto at{std::find_if(dialog->rows.begin(), dialog->rows.end(), [](const dialog::Row& r) { return r.label == "Command"; })};
        dialog->rows.insert(at == dialog->rows.end() ? dialog->rows.end() : at + 1, rows.begin(), rows.end());
        return dialog;
    };
}

/**
 * An asset as a confirmation shows it, resolved as the node resolves it: only the chain's coin ("CHN", as the node reads it)
 * or "0x" and the 64 hex digits of an id are taken, never a name or a number, which a page could pick to read as one asset
 * while the node trades another (the parser differential of a confirmation, security review 2026-10-21). The row says the
 * ticker, the display name and the id, short. Throws for anything else, or an id not registered: nothing unresolved runs.
 */
std::string ResolvedAsset(const confirm::Call& call, const UniValue& value)
{
    if (!value.isStr()) throw HostError(BRIDGE_INVALID_REQUEST, "An asset is named by its id (\"0x\" and 64 hex digits) or the chain's coin");
    const std::string& text{value.get_str()};
    if (text == "CHN" || text == bitassets::AssetLabel(bitassets::CHN, nullptr)) return bitassets::AssetLabel(bitassets::CHN, nullptr) + " (the chain's coin)";
    if (text.size() != 66 || text.substr(0, 2) != "0x" || !IsHex(text.substr(2))) {
        throw HostError(BRIDGE_INVALID_REQUEST, strprintf("The page named an asset as \"%s\": a confirmation takes an asset's id (\"0x\" and 64 hex digits) only", text.substr(0, 80)));
    }
    UniValue args{UniValue::VARR};
    args.push_back(text);
    const UniValue asset{call.Read("getasset", args)};
    if (!asset.isObject() || !asset.find_value("asset").isStr() || asset.find_value("asset").get_str() != text.substr(2)) {
        throw HostError(BRIDGE_INVALID_REQUEST, strprintf("No asset %s is registered", text));
    }
    const UniValue& name{asset.find_value("name")};
    const UniValue& display{asset.find_value("display_name")};
    const std::string id{text.substr(2)};
    std::string shown{name.isStr() ? name.get_str() : "(no published name)"};
    if (display.isStr()) shown += " — " + display.get_str();
    return strprintf("%s (#%s, 0x%s…%s)", shown, asset.find_value("seq").isStr() ? asset.find_value("seq").get_str() : "?", id.substr(0, 8), id.substr(56));
}

/** A pool as a confirmation shows it: its two assets, resolved, its kind and its id, short. Throws if there is none. */
std::string ResolvedPool(const confirm::Call& call, const UniValue& value)
{
    if (!value.isStr() || value.get_str().size() != 64 || !IsHex(value.get_str())) throw HostError(BRIDGE_INVALID_REQUEST, "A pool is named by its id (64 hex digits)");
    UniValue args{UniValue::VARR};
    args.push_back(value.get_str());
    const UniValue pool{call.Read("getpool", args)};
    if (!pool.isObject() || !pool.find_value("pool").isStr() || pool.find_value("pool").get_str() != value.get_str()) throw HostError(BRIDGE_INVALID_REQUEST, "No such pool");
    const auto side{[&](const char* key) {
        const std::string id{pool.find_value(key).get_str()};
        return ResolvedAsset(call, UniValue{uint256::FromHex(id) && uint256::FromHex(id)->IsNull() ? std::string{CURRENCY_UNIT} : "0x" + id});
    }};
    std::string kind{pool.find_value("curve").isStr() ? pool.find_value("curve").get_str() : "constant_product"};
    if (pool.find_value("amp").isNum()) kind += strprintf(", A %s", pool.find_value("amp").getValStr());
    if (pool.find_value("weight_a").isNum()) kind += strprintf(", weights %s/%s", pool.find_value("weight_a").getValStr(), pool.find_value("weight_b").getValStr());
    if (pool.find_value("fee").isNum()) kind += strprintf(", fee %s %%", pool.find_value("fee").getValStr());
    return strprintf("%s / %s — %s (pool %s…%s)", side("asset_a_id"), side("asset_b_id"), kind, value.get_str().substr(0, 8), value.get_str().substr(56));
}

/** A position of concentrated liquidity as a confirmation shows it: its pool resolved, its range, what it holds. Throws if there is none. */
std::string ResolvedPosition(const confirm::Call& call, const UniValue& value)
{
    if (!value.isStr() || value.get_str().size() != 64 || !IsHex(value.get_str())) throw HostError(BRIDGE_INVALID_REQUEST, "A position is named by its id (64 hex digits)");
    UniValue args{UniValue::VARR};
    args.push_back(value.get_str());
    const UniValue position{call.Read("getposition", args)};
    if (!position.isObject() || !position.find_value("pool").isStr()) throw HostError(BRIDGE_INVALID_REQUEST, "No such position");
    return strprintf("%s, from %s to %s (%s %s and %s %s now; position %s…)", ResolvedPool(call, position.find_value("pool")), position.find_value("price_lower").getValStr(),
                     position.find_value("price_upper").getValStr(), position.find_value("amount0").getValStr(), position.find_value("asset0").getValStr(),
                     position.find_value("amount1").getValStr(), position.find_value("asset1").getValStr(), value.get_str().substr(0, 8));
}

/** Which parameters of a call name assets or pools: one each, an array of them, or an object keyed by them. */
struct AssetParams {
    std::vector<size_t> assets;
    std::vector<size_t> asset_arrays;
    std::vector<size_t> asset_keys;
    std::vector<size_t> pools;
    std::vector<size_t> pool_arrays;
    std::vector<size_t> positions;
};

/**
 * A describer that resolves every asset and pool a call names (ResolvedAsset, ResolvedPool) and shows each as a row of its
 * own after the command, in place of the text the page gave (the other parameters as `generic` shows them). The host runs
 * the very parameters it described (confirm::CanonicalParams), which name assets by id only: what is shown is what trades.
 */
std::function<std::optional<Window::ConfirmDialog>(const confirm::Call&)> WithAssets(std::function<std::optional<Window::ConfirmDialog>(const confirm::Call&)> generic, AssetParams which)
{
    return [=](const confirm::Call& call) -> std::optional<Window::ConfirmDialog> {
        const std::vector<std::string> names{confirm::ArgNames(call.node, call.method)};
        const auto label{[&](size_t index) {
            std::string name{index < names.size() ? names[index] : strprintf("parameter %u", index + 1)};
            std::replace(name.begin(), name.end(), '_', ' ');
            if (!name.empty()) name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
            return name;
        }};
        std::vector<dialog::Row> rows;
        std::set<size_t> resolved;
        const auto contains{[](const std::vector<size_t>& v, size_t i) { return std::find(v.begin(), v.end(), i) != v.end(); }};
        for (size_t i{0}; i < std::max<size_t>(names.size(), call.params.isArray() ? call.params.size() : 0); ++i) {
            const UniValue arg{call.Arg(i)};
            if (contains(which.assets, i) && !arg.isNull()) {
                rows.push_back(confirm::Text(label(i), ResolvedAsset(call, arg)));
            } else if (contains(which.asset_arrays, i) && !arg.isNull()) {
                if (!arg.isArray()) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("%s: a list of assets", label(i)));
                for (size_t k{0}; k < arg.size(); ++k) rows.push_back(confirm::Text(strprintf("%s %u", label(i), k + 1), ResolvedAsset(call, arg[k])));
            } else if (contains(which.asset_keys, i) && !arg.isNull()) {
                if (!arg.isObject()) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("%s: amounts by asset", label(i)));
                for (const std::string& key : arg.getKeys()) rows.push_back(confirm::Text(strprintf("%s of", label(i)), strprintf("%s %s", confirm::ValueText(arg[key]), ResolvedAsset(call, UniValue{key}))));
            } else if (contains(which.pools, i) && !arg.isNull()) {
                rows.push_back(confirm::Text(label(i), ResolvedPool(call, arg)));
            } else if (contains(which.positions, i) && !arg.isNull()) {
                rows.push_back(confirm::Text(label(i), ResolvedPosition(call, arg)));
            } else if (contains(which.pool_arrays, i) && !arg.isNull()) {
                if (!arg.isArray()) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("%s: a list of pools", label(i)));
                for (size_t k{0}; k < arg.size(); ++k) rows.push_back(confirm::Text(strprintf("Pool %u", k + 1), ResolvedPool(call, arg[k])));
            } else {
                continue;
            }
            resolved.insert(i);
        }
        UniValue others{call.params};
        if (others.isArray()) {
            UniValue trimmed{UniValue::VARR};
            for (size_t i{0}; i < others.size(); ++i) trimmed.push_back(resolved.contains(i) ? UniValue{} : others[i]);
            others = trimmed;
        }
        confirm::Call shown{call.node, call.wallet, call.method, others, call.fee_for};
        auto dialog{generic(shown)};
        if (!dialog) return dialog;
        auto at{std::find_if(dialog->rows.begin(), dialog->rows.end(), [](const dialog::Row& r) { return r.label == "Command"; })};
        dialog->rows.insert(at == dialog->rows.end() ? dialog->rows.end() : at + 1, rows.begin(), rows.end());
        return dialog;
    };
}
} // namespace

namespace {
/**
 * launchtoken [ticker, options]: the ticker (WithNames), then each option as a row of its own (never the options as one block
 * of JSON, a logo of 44 KB in it): the display name, the logo as a picture, the description and links, the allocation, the
 * first buy and the least it gets, the bounds of a buy. An option this app does not know is refused (what is shown is what
 * runs).
 */
std::optional<Window::ConfirmDialog> DescribeLaunch(const confirm::Call& call)
{
    const UniValue ticker{call.Arg(0)};
    const UniValue options{call.Arg(1)};
    if (!options.isNull() && !options.isObject()) throw HostError(BRIDGE_INVALID_REQUEST, "launchtoken: its options are an object");
    UniValue first{UniValue::VARR};
    first.push_back(ticker);
    confirm::Call shown{call.node, call.wallet, call.method, first, call.fee_for};
    auto dialog{WithNames(confirm::Generic("Launch an asset", "Launch", "The wallet launches an asset on a bonding curve, from its reservation of the ticker: its supply fixed (a billion units unless shown), 3/4 sold on the curve; buyers pay " + CURRENCY_UNIT + " into its curve and sellers get " + CURRENCY_UNIT + " back from it, until it graduates into a locked pool with the last 1/4. The first buy shown, if any, is paid now."), 0, SIZE_MAX, false)(shown)};
    if (!dialog || options.isNull()) return dialog;
    const auto coins{[&](const char* key) {
        try {
            return confirm::Amount("", AmountFromValue(options.find_value(key)));
        } catch (const UniValue&) {
            throw HostError(BRIDGE_INVALID_REQUEST, strprintf("launchtoken: %s is an amount", key));
        }
    }};
    for (const std::string& key : options.getKeys()) {
        const UniValue& value{options.find_value(key)};
        if (key == "display_name") {
            if (!value.isStr()) throw HostError(BRIDGE_INVALID_REQUEST, "launchtoken: display_name is text");
            if (value.get_str().empty()) continue;
            if (const auto why{bitassets::WhyNotDisplayName(value.get_str())}) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("The rules refuse this display name: %s", *why));
            dialog->rows.push_back(confirm::Text("Display name", value.get_str()));
            if (!PlainAscii(value.get_str())) dialog->rows.push_back(confirm::Note("The display name holds characters beyond plain ASCII. Display names are not unique: the ticker and the number tell assets apart."));
        } else if (key == "logo") {
            if (value.isStr() && value.get_str().empty()) continue;
            std::string reason;
            const auto logo{ReadLogoParam(value, reason)};
            if (!logo) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("The rules refuse this logo (%s): a PNG or JPEG of exactly %ux%u pixels, at most %u bytes, without metadata", reason, bitassets::LOGO_SIZE, bitassets::LOGO_SIZE, bitassets::MAX_LOGO_BYTES));
            for (auto& row : LogoRows(*logo)) dialog->rows.push_back(std::move(row));
        } else if (key == "info") {
            if (!value.isStr()) throw HostError(BRIDGE_INVALID_REQUEST, "launchtoken: info is text");
            if (!value.get_str().empty()) dialog->rows.push_back(confirm::Text("Description and links", value.get_str()));
        } else if (key == "supply") {
            dialog->rows.push_back(confirm::Text("Supply", confirm::ValueText(value) + " (fixed: 3/4 on the curve, 1/4 into the pool at graduation)"));
        } else if (key == "decimals") {
            if (!value.isNum()) throw HostError(BRIDGE_INVALID_REQUEST, "launchtoken: decimals is a number");
            dialog->rows.push_back(confirm::Text("Decimals", confirm::ValueText(value)));
        } else if (key == "fee") {
            dialog->rows.push_back(confirm::Text("Curve's fee", confirm::ValueText(value) + " % of each trade (its parts as the network's: creator and security)"));
        } else if (key == "allocation") {
            dialog->rows.push_back(confirm::Text("Creator's allocation", confirm::ValueText(value) + " % of the supply, minted to you at graduation (shown to everyone)"));
        } else if (key == "first_buy") {
            auto row{coins("first_buy")};
            row.label = "First buy, paid now";
            dialog->rows.push_back(std::move(row));
        } else if (key == "first_min_out") {
            dialog->rows.push_back(confirm::Text("First buy gets at least", confirm::ValueText(value) + " units (refused by the rules below it, its " + CURRENCY_UNIT + " back)"));
        } else if (key == "min_buy" || key == "max_buy") {
            auto row{coins(key.c_str())};
            row.label = key == "min_buy" ? "Least a buy pays" : "Most a buy pays";
            dialog->rows.push_back(std::move(row));
        } else if (key == "fee_destination") {
            if (!value.isStr()) throw HostError(BRIDGE_INVALID_REQUEST, "launchtoken: fee_destination is text");
            const std::string& d{value.get_str()};
            if (d == "creator") {
                dialog->rows.push_back(confirm::Text("Creator's fees", "To you (claimed with Claim), fixed for good"));
            } else if (d == "burn") {
                dialog->rows.push_back(confirm::Text("Creator's fees", "Bought back in its own market each block and burned, fixed for good"));
            } else if (d == "split") {
                dialog->rows.push_back(confirm::Text("Creator's fees", "Split between you and buyback and burn (below), fixed for good"));
            } else {
                throw HostError(BRIDGE_INVALID_REQUEST, "launchtoken: fee_destination is creator, burn or split");
            }
        } else if (key == "burn_percent") {
            if (!value.isNum()) throw HostError(BRIDGE_INVALID_REQUEST, "launchtoken: burn_percent is a number");
            dialog->rows.push_back(confirm::Text("Bought back and burned", confirm::ValueText(value) + " % of the creator's fees (the rest yours)"));
        } else if (key == "fee_split") {
            // Upgrade v7: every recipient of the tunable part of the fees, in full (the node checks the split itself).
            if (!value.isArray() || value.empty() || value.size() > bitassets::MAX_FEE_SHARES) throw HostError(BRIDGE_INVALID_REQUEST, "launchtoken: fee_split is a list of 1 to 5 {to, bps}");
            dialog->rows.push_back(confirm::Text("Fee split", "The 75 % of the curve's fees that are not the network's (and the creator's 60 % of the locked pool's), fixed for good:"));
            for (const UniValue& e : value.getValues()) {
                if (!e.isObject() || !e["to"].isStr() || !e["bps"].isNum()) throw HostError(BRIDGE_INVALID_REQUEST, "launchtoken: each fee_split entry is {to, bps}");
                const std::string& to{e["to"].get_str()};
                const int64_t bps{e["bps"].getInt<int64_t>()};
                if (bps < 1 || bps > bitassets::FEE_SPLIT_BPS) throw HostError(BRIDGE_INVALID_REQUEST, "launchtoken: a fee share is 1 to 10000 basis points");
                const std::string share{strprintf("%d.%02d %%", bps / 100, bps % 100)};
                if (to == "creator") {
                    dialog->rows.push_back(confirm::Text("To you (claimed)", share));
                } else if (to == "burn") {
                    dialog->rows.push_back(confirm::Text("Bought back and burned", share));
                } else {
                    dialog->rows.push_back(confirm::Text("Paid to " + to, share + " (paid by the chain, no claim)"));
                }
            }
        } else if (key == "keep_control") {
            if (!value.isBool()) throw HostError(BRIDGE_INVALID_REQUEST, "launchtoken: keep_control is true or false");
            if (value.get_bool()) {
                dialog::Row head;
                head.kind = dialog::Row::Kind::HEADING;
                head.label = "Unlocked Supply: you keep control";
                dialog->rows.push_back(std::move(head));
                dialog->rows.push_back(confirm::Text("Control coin", "Kept by you: you can create more units at any time. Its pages tag this launch \"Unlocked Supply\""));
            }
        } else {
            throw HostError(BRIDGE_INVALID_REQUEST, strprintf("launchtoken has no option %s", key.substr(0, 40)));
        }
    }
    return dialog;
}

/** The second step of reserveasset for a launch: launchtoken of the same ticker, [ticker, options]. */
confirm::Reveal LaunchAfterReserve()
{
    confirm::Reveal reveal;
    reveal.method = "launchtoken";
    reveal.title = "Launch an asset";
    reveal.accept = "Reserve and launch";
    reveal.first_label = "Reservation";
    reveal.second_label = "Launch";
    reveal.what = "Two transactions: the reservation hides the ticker until it is deep enough, so that nobody who sees it coming can "
                  "take it first; the launch then puts the asset on its curve with what is below (and its first buy, if any).";
    reveal.matches = confirm::SameFirstParam;
    reveal.second_vsize = [](const UniValue& params) {
        const UniValue& options{params.size() > 1 && params[1].isObject() ? params[1] : NullUniValue};
        const UniValue& logo{options.isObject() ? options.find_value("logo") : NullUniValue};
        return int64_t{450} + (options.isObject() ? static_cast<int64_t>(options.write().size()) : 0) - (logo.isStr() ? static_cast<int64_t>(logo.get_str().size() / 4) : 0);
    };
    reveal.when = confirm::AfterDepth("getbitassetsinfo", "reveal_depth");
    return reveal;
}
/** setanchorbidpolicy: what the mainchain wallet will spend on bids, from now on (the fields changed, then the limits). */
std::optional<Window::ConfirmDialog> AnchorBidPolicyDialog(const confirm::Call& call)
{
    const UniValue fields{call.Arg(0)};
    const UniValue reset{call.Arg(1)};
    if (!fields.isNull() && !fields.isObject()) throw HostError(BRIDGE_INVALID_REQUEST, "policy: an object of fields");
    const bool back{reset.isBool() && reset.get_bool()};
    // The policy the call makes, as the node would take it (checked there again).
    const UniValue now{call.Read("getanchorbidpolicy", UniValue{UniValue::VARR}, /*wallet=*/false)};
    if (!now.find_value("validator").isTrue()) throw HostError(BRIDGE_INVALID_REQUEST, "This node is not a validator: only validators bid for anchors.");
    pos::AnchorBidPolicy policy;
    std::string error;
    if (!pos::ApplyPolicyFields(policy, now.find_value(back ? "defaults" : "policy"), error) ||
        (fields.isObject() && !pos::ApplyPolicyFields(policy, fields, error)) || !(error = pos::CheckPolicy(policy)).empty()) {
        throw HostError(BRIDGE_INVALID_REQUEST, error.empty() ? std::string{"Not a policy"} : error);
    }
    Window::ConfirmDialog dialog;
    dialog.title = "Change the anchor bid policy";
    dialog.kind = "rpc:setanchorbidpolicy";
    dialog.accept = "Change";
    dialog.rows = {confirm::Text("Network", Params().GetChainTypeString())};
    if (back) dialog.rows.push_back(confirm::Text("Reset", "back to the options' policy"));
    const UniValue said{pos::PolicyToUniValue(policy)};
    const std::set<std::string> amounts{"floor", "max", "fixed", "periodcap"};
    if (fields.isObject()) {
        for (const std::string& key : fields.getKeys()) {
            const UniValue& value{said.find_value(key)};
            if (amounts.contains(key)) {
                dialog.rows.push_back(confirm::Amount(key, *ParseMoney(value.getValStr())));
            } else {
                dialog.rows.push_back(confirm::Text(key, guard::OneLine(value.isStr() ? value.get_str() : value.write(), 200)));
            }
        }
    }
    dialog.rows.push_back(confirm::Heading("Limits"));
    dialog.rows.push_back(confirm::Text("Strategy", pos::StrategyName(policy.strategy)));
    dialog.rows.push_back(confirm::Amount("Most per mainchain block", policy.max));
    if (policy.period_cap > 0) dialog.rows.push_back(confirm::Amount(strprintf("Most in %d hours", policy.period_hours), policy.period_cap));
    dialog.message = policy.Bids() ?
        strprintf("This node's validator bids from the wallet of the %s node for a BMM commitment to its ticket in every %s block, "
                  "as this policy says, from now on and after a restart (kept in the node's settings): %s.\n\nChange it only if you asked for it.",
                  std::string{chain::MAINCHAIN_NAME}, std::string{chain::MAINCHAIN_NAME}, pos::AnchorBidPolicySummary(policy)) :
        std::string{"This node's validator will bid for no anchors: the chain then relies on other bidders, and its blocks of stake halt "
                    "after max_unanchored_slots without one.\n\nChange it only if you asked for it."};
    return dialog;
}

} // namespace

std::map<std::string, confirm::Method> ConfirmedRpcMethods()
{
    // FastBlocks: the anchor bid policy, which spends the mainchain wallet (bonds are made through fastblocks.bond and
    // fastblocks.unbond, which the host confirms, below).
    // BitAssets: what the pages do that pays or signs: never the page's to call by itself. The host shows each call on the desktop, its
    // destinations whole first, then its amounts, then the rest of its parameters, with the passphrase of a locked wallet
    // typed there, and runs it with the keys unlocked for it alone (confirm.h, audit 7 #1).
    return {
        {"setanchorbidpolicy", {confirm::Method::Kind::NODE, Bridge::Thread::LONG, AnchorBidPolicyDialog, /*fee_unknown=*/false, 0}},
        // Reserved and registered in one question: the registration, exactly as shown, is sent by the app without asking
        // again once the reservation is deep enough (confirm::Reveal).
        {"reserveasset", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, WithNames(confirm::Generic("Reserve an asset", "Reserve", "The wallet pays for a reservation of an asset name."), 0, SIZE_MAX, false), /*fee_unknown=*/true, 1, false, RegisterAfterReserve(), {LaunchAfterReserve()}}},
        {"registerasset", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, WithNames(WithLogo(confirm::Generic("Register an asset", "Register", "The wallet registers an asset it reserved, with the supply shown."), 5, false), 0, 6, false), /*fee_unknown=*/true}},
        {"setassetlogo", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, WithLogo(confirm::Generic("Set the logo of an asset", "Set the logo", "The wallet puts this logo on the chain for an asset it controls, in place of the one before: everyone sees it where the asset is shown."), 1, true), /*fee_unknown=*/false, 2}},
        {"setassetdisplayname", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, WithNames(confirm::Generic("Set the display name of an asset", "Set the display name", "The wallet puts this display name on the chain for an asset it controls, in place of the one before: everyone sees it after the asset's ticker. Display names are not unique."), SIZE_MAX, 1, true), /*fee_unknown=*/false, 2}},
        {"releaseassetreservation", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Release a reservation", "Release", "The wallet gives up a reservation of an asset name."), /*fee_unknown=*/true}},
        {"sendasset", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, WithAssets(confirm::Generic("Send an asset", "Send", "The wallet sends units of an asset to the address shown.", {"address"}, {"amount"}), {.assets = {1}}), /*fee_unknown=*/true}},
        {"mintasset", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, WithAssets(confirm::Generic("Mint an asset", "Mint", "The wallet mints more units of an asset it controls.", {"address"}, {"amount"}), {.assets = {0}}), /*fee_unknown=*/true}},
        {"burnasset", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, WithAssets(confirm::Generic("Burn an asset", "Burn", "The wallet destroys units of an asset: they cannot come back.", {}, {"amount"}), {.assets = {0}}), /*fee_unknown=*/true}},
        {"updateasset", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, WithAssets(confirm::Generic("Update an asset", "Update", "The wallet signs a change of an asset it controls."), {.assets = {0}}), /*fee_unknown=*/true}},
        {"transferassetcontrol", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, WithAssets(confirm::Generic("Transfer the control of an asset", "Transfer", "The wallet gives the control of an asset to the address shown: it cannot be taken back.", {"address"}, {}), {.assets = {0}}), /*fee_unknown=*/true}},
        {"fixassetsupply", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, WithAssets(confirm::Generic("Fix the supply of an asset", "Fix the supply", "The wallet fixes the supply of an asset for good: no more can ever be minted."), {.assets = {0}}), /*fee_unknown=*/true}},
        {"releaseasset", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, WithAssets(confirm::Generic("Release an asset", "Release", "The wallet gives up an asset it controls."), {.assets = {0}}), /*fee_unknown=*/true}},
        {"swapasset", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, WithAssets(confirm::Generic("Swap", "Swap", "The wallet trades an asset in a pool, for at least what is shown (min_out): refused, not signed for less, should the pool have moved.", {}, {"amount_in", "min_out"}), {.assets = {0, 2}}), /*fee_unknown=*/true}},
        {"addliquidity", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, WithAssets(confirm::Generic("Add liquidity", "Add", "The wallet puts both assets into a pool, for shares of it.", {}, {"amount_a", "amount_b"}), {.assets = {0, 2}}), /*fee_unknown=*/true}},
        {"removeliquidity", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, WithAssets(confirm::Generic("Remove liquidity", "Remove", "The wallet takes its shares out of a pool, for both assets."), {.assets = {0, 1}}), /*fee_unknown=*/true}},
        // Layers' DEX: routes and the pool kinds. Every asset and pool resolved and shown (WithAssets).
        {"swaproute", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, WithAssets(confirm::Generic("Trade along a route", "Trade", "The wallet trades through the pools shown, in turn, for at least what is shown (min_out): its order clears at the end of its block with the block's other orders, at one price per pool; if it would give less, what went in comes back.", {}, {"amount_in", "min_out"}), {.asset_arrays = {0}, .pool_arrays = {4}}), /*fee_unknown=*/true}},
        {"swappools", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, WithAssets(confirm::Generic("Trade through pools", "Trade", "The wallet trades through the pools shown, in turn, for at least what is shown (min_out): its order clears at the end of its block, at one price per pool; if it would give less, what went in comes back.", {}, {"amount_in", "min_out"}), {.assets = {0}, .pool_arrays = {2}}), /*fee_unknown=*/true}},
        {"createpool", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, WithAssets(confirm::Generic("Create a pool", "Create the pool", "The wallet makes a pool of the kind shown and puts both amounts into it, for shares of it: they set its price.", {}, {"amount_a", "amount_b"}), {.assets = {0, 2}}), /*fee_unknown=*/true}},
        {"addpoolliquidity", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, WithAssets(confirm::Generic("Add liquidity", "Add", "The wallet puts the amounts shown into the pool, for shares of it; out of the pool's proportions, it pays the pool's fee on the part a swap would trade."), {.asset_keys = {1}, .pools = {0}}), /*fee_unknown=*/true}},
        {"removepoolliquidity", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, WithAssets(confirm::Generic("Remove liquidity", "Remove", "The wallet takes its shares out of the pool shown, for its assets (or the one shown alone, its fee paid on the part traded)."), {.assets = {2}, .pools = {0}}), /*fee_unknown=*/true}},
        // Launches: the asset resolved and shown (WithAssets); a launch names its ticker, which its reservation reveals.
        {"launchtoken", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, DescribeLaunch, /*fee_unknown=*/true}},
        {"buyfromlaunch", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, WithAssets(confirm::Generic("Buy from a launch", "Buy", (std::string{"The wallet pays "} + std::string{chain::TICKER} + std::string{" into the asset's curve, for at least what is shown (min_out): its order clears at the end of its block; past the curve's end it gets the rest of its "} + std::string{chain::TICKER} + std::string{" back."}), {}, {"amount", "min_out"}), {.assets = {0}}), /*fee_unknown=*/true}},
        {"selltolaunch", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, WithAssets(confirm::Generic("Sell to a launch", "Sell", (std::string{"The wallet sells the asset back to its curve for at least the "} + std::string{chain::TICKER} + std::string{" shown (min_out), paid by the coinbase."}), {}, {"amount", "min_out"}), {.assets = {0}}), /*fee_unknown=*/true}},
        {"curveasset", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, WithAssets(confirm::Generic("Put an asset on a bonding curve", "Put on a curve", "The wallet puts the amount shown of an asset it controls on a bonding curve, priced in " + CURRENCY_UNIT + ": 3/4 of it sold on the curve, 1/4 into the asset's pool with " + CURRENCY_UNIT + " at graduation, locked for good. It does not come back. The control coin goes with it: the supply is fixed for good. The curve buys back no more than it sold.", {}, {"amount"}), {.assets = {0}}), /*fee_unknown=*/true}},
        {"claimlaunch", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, WithAssets(confirm::Generic("Pay a launch's creator", "Pay", "The wallet sends what pays the launch's creator what it is owed (its fees, its allocation once graduated): nothing goes anywhere else."), {.assets = {0}}), /*fee_unknown=*/true}},
        // Concentrated liquidity: the pool shown resolved (its assets and kind); a position by its id.
        {"openposition", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, WithAssets(confirm::Generic("Open a position", "Open", "The wallet puts liquidity into the pool shown between the two prices shown, for at most the amounts shown (the rest back): a position, a token of its own, which earns the pool's fees while the price is in its range.", {}, {}), {.asset_keys = {3}, .pools = {0}}), /*fee_unknown=*/true}},
        {"changeposition", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, WithAssets(confirm::Generic("Change a position", "Change", "The wallet changes its position shown: adds liquidity, takes some out, collects its fees, or closes it (all out, all collected)."), {.positions = {0}}), /*fee_unknown=*/true}},
        // Order books: the assets resolved; an order by its id.
        {"placeorder", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, WithAssets(confirm::Generic("Place a limit order", "Place", "The wallet puts the amount shown up for sale in the book of the two assets shown, at the price shown at least, for the blocks shown; it trades in the blocks' batches, wholly or in part; its token collects and cancels it.", {}, {"amount", "price"}), {.assets = {0, 2}}), /*fee_unknown=*/true}},
        {"settleorder", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Settle a limit order", "Settle", "The wallet collects what its order got; cancelled, what is left of what it sells comes back too and the order is gone."), /*fee_unknown=*/true}},
        {"createauction", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, WithAssets(confirm::Generic("Create an auction", "Create", "The wallet puts units of an asset up for auction.", {}, {"amount"}), {.assets = {0, 2}}), /*fee_unknown=*/true}},
        {"bidauction", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Bid in an auction", "Bid", "The wallet pays a bid in an auction.", {}, {"amount"}), /*fee_unknown=*/true}},
        {"collectauction", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Collect an auction", "Collect", "The wallet collects what an auction owes it."), /*fee_unknown=*/true}},
    };
}

void RegisterHost(Bridge& bridge, NodeFn node, guard::ConfirmFn confirm, confirm::KeysFn keys)
{
    // The logo of an asset as a data: URL of the type the rules found (image/png or image/jpeg), for an <img> only; null when
    // it has none. Kept in memory (LogoCache), by asset and hash: the pages ask with the hash listassets gives.
    static LogoCache logos;
    bridge.RegisterHost("bitassets.logo", [node](const UniValue& params) {
        const UniValue& asset{Param(params, "asset")};
        const UniValue& hash{Param(params, "hash")};
        if (!asset.isStr()) throw HostError(BRIDGE_INVALID_REQUEST, "asset: the id of an asset");
        if (!hash.isNull() && (!hash.isStr() || hash.get_str().size() != 64 || !IsHex(hash.get_str()))) throw HostError(BRIDGE_INVALID_REQUEST, "hash: 64 hex digits");
        return logos.Get(node(), asset.get_str(), hash.isStr() ? hash.get_str() : std::string{});
    }, Bridge::Thread::WORKER);
    // A logo the user picked, as the rules and the confirmation will see it: {ok, reason?, mime, size, hash}. Read only.
    bridge.RegisterHost("bitassets.checkLogo", [](const UniValue& params) {
        std::string reason;
        const auto logo{ReadLogoParam(Param(params, "base64"), reason)};
        UniValue out{UniValue::VOBJ};
        out.pushKV("ok", logo.has_value());
        if (!logo) {
            out.pushKV("reason", reason);
            return out;
        }
        out.pushKV("mime", logo->mime);
        out.pushKV("size", static_cast<uint64_t>(logo->size));
        out.pushKV("hash", logo->hash);
        return out;
    }, Bridge::Thread::WORKER);
    // The journal of the app: the assets being created (what to register once each reservation is deep enough), and
    // the prices of the pools as the app saw them; in a file of the data directory, with a sanity cap.
    bridge.RegisterHost("bitassets.store.load", [node](const UniValue&) {
        return ReadStore(StorePath(node(), JOURNAL_FILE));
    }, Bridge::Thread::LONG);
    bridge.RegisterHost("bitassets.store.save", [node](const UniValue& params) {
        WriteStore(StorePath(node(), JOURNAL_FILE), Param(params, "value"));
        return UniValue{true};
    }, Bridge::Thread::LONG);

    // fastblocks.bond {wallet, pubkey, amount, pop?, owner?}: createbond, after the desktop's yes, the passphrase of a
    // locked wallet typed in the same dialog of the host (audit 7 #1). A new bond, or more coins to the wallet's bond
    // (only its owner may add); with pop and owner, to the key of another node, whose proof of possession that node
    // signed (fastblocks.pop there).
    bridge.RegisterHost("fastblocks.bond", [node, keys](const UniValue& params) -> UniValue {
        const std::string wallet_name{Text(params, "wallet")};
        const std::string key{ValidatorKey(params)};
        const std::string amount_text{Text(params, "amount", 32)};
        const CAmount amount{Coins(amount_text)};
        const UniValue& pop{Param(params, "pop")};
        const UniValue& owner{Param(params, "owner")};
        if (!pop.isNull() && (!pop.isStr() || pop.get_str().size() != 128 || !IsHex(pop.get_str()))) throw HostError(BRIDGE_INVALID_REQUEST, "A proof of possession is a signature: 128 hexadecimal digits.");
        if (!owner.isNull() && (!owner.isStr() || owner.get_str().size() > 90)) throw HostError(BRIDGE_INVALID_REQUEST, "owner: an address");
        interfaces::Node& n{node()};
        auto wallet{WalletNamed(n, wallet_name)};
        if (wallet->privateKeysDisabled()) throw HostError(-4, "This wallet has no private keys: it cannot bond.");
        if (!keys) throw HostError(-18, "Wallets are disabled in this node (-disablewallet).");
        const auto existing{WalletBond(n, wallet_name, key)};
        UniValue args{UniValue::VARR};
        args.push_back(key);
        args.push_back(FormatMoney(amount));
        if (!pop.isNull()) {
            args.push_back(pop.get_str());
            if (!owner.isNull()) args.push_back(owner.get_str());
        }
        return keys(wallet_name, BondConfirmation(wallet_name, key, amount, existing, UnbondingAnchors(n), !pop.isNull()), /*always=*/true, /*fee_unknown=*/true,
                    [&] { return n.executeRpc("createbond", args, WalletUri(wallet_name)); });
    }, Bridge::Thread::LONG);

    // fastblocks.unbond {wallet, pubkey, amount ("all" or coins)}: createunbond, after the desktop's yes (the passphrase
    // in the same dialog).
    bridge.RegisterHost("fastblocks.unbond", [node, keys](const UniValue& params) -> UniValue {
        const std::string wallet_name{Text(params, "wallet")};
        const std::string key{ValidatorKey(params)};
        const std::string amount_text{Text(params, "amount", 32)};
        const std::optional<CAmount> amount{amount_text == "all" ? std::nullopt : std::optional{Coins(amount_text)}};
        interfaces::Node& n{node()};
        auto wallet{WalletNamed(n, wallet_name)};
        if (wallet->privateKeysDisabled()) throw HostError(-4, "This wallet has no private keys: it cannot unbond.");
        if (!keys) throw HostError(-18, "Wallets are disabled in this node (-disablewallet).");
        const auto bond{WalletBond(n, wallet_name, key)};
        if (!bond || AmountOf(bond->find_value("bonded")) <= 0) throw HostError(-8, "This wallet has nothing bonded to this validator key.");
        UniValue args{UniValue::VARR};
        args.push_back(key);
        args.push_back(amount ? FormatMoney(*amount) : std::string{"all"});
        return keys(wallet_name, UnbondConfirmation(wallet_name, key, amount, *bond, UnbondingAnchors(n)), /*always=*/true, /*fee_unknown=*/true,
                    [&] { return n.executeRpc("createunbond", args, WalletUri(wallet_name)); });
    }, Bridge::Thread::LONG);

    // fastblocks.staking {enabled}: setstaking; turning it off asks the desktop first (the validator may be jailed).
    bridge.RegisterHost("fastblocks.staking", [node, confirm](const UniValue& params) -> UniValue {
        const UniValue& enabled{Param(params, "enabled")};
        if (!enabled.isBool()) throw HostError(BRIDGE_INVALID_REQUEST, "enabled: true or false");
        if (!enabled.get_bool()) Ask(confirm, StopStakingConfirmation(), "Staking goes on: stopping it was declined.");
        UniValue args{UniValue::VARR};
        args.push_back(enabled.get_bool());
        return node().executeRpc("setstaking", args, "/");
    });

    // fastblocks.pop {owner, pubkey?}: signbondpop, after the desktop's yes.
    bridge.RegisterHost("fastblocks.pop", [node, confirm](const UniValue& params) -> UniValue {
        const std::string owner{Text(params, "owner", 90)};
        const std::string key{Param(params, "pubkey").isNull() ? std::string{} : ValidatorKey(params)};
        Ask(confirm, PopConfirmation(owner, key), "No proof was signed: it was declined.");
        UniValue args{UniValue::VARR};
        args.push_back(owner);
        if (!key.empty()) args.push_back(key);
        return node().executeRpc("signbondpop", args, "/");
    });

    // fastblocks.anchoring {}: what this node bids, as a validator, for a BMM commitment to its ticket in every
    // mainchain block (its bid policy: getanchorbidpolicy, setanchorbidpolicy), and where the pot it wins goes. The
    // floor and the strategy in force, as the anchorer uses them (not the merged miner's offer: setbmm).
    bridge.RegisterHost("fastblocks.anchoring", [](const UniValue&) -> UniValue {
        UniValue out{UniValue::VOBJ};
        const pos::FastBlocksHooks* hooks{pos::InstalledHooks()};
        const auto anchoring{hooks ? hooks->ValidatorAnchoring() : std::nullopt};
        out.pushKV("on", anchoring.has_value() && anchoring->policy.Bids());
        if (anchoring) {
            out.pushKV("bid", FormatMoney(anchoring->policy.floor));
            out.pushKV("strategy", pos::StrategyName(anchoring->policy.strategy));
            out.pushKV("max", FormatMoney(anchoring->policy.max));
            CTxDestination dest;
            out.pushKV("payee", ExtractDestination(anchoring->payee, dest) ? EncodeDestination(dest) : HexStr(anchoring->payee));
        }
        out.pushKV("validator", anchoring.has_value());
        out.pushKV("defaultbid", FormatMoney(pos::DEFAULT_VALIDATOR_ANCHOR_BID));
        return out;
    });
}

std::vector<std::string> CoinbaseRoles(interfaces::Chain& chain, const uint256& block)
{
    // pos::CoinbaseRole by the words of the page (chain/local.ts COINBASE_ROLES): the producer's share of a block
    // of stake, the pot paid to a BMM bidder, a reporter's reward for a proof.
    std::vector<std::string> roles;
    for (const auto role : chain.getCoinbaseRoles(block)) {
        switch (role) {
        case interfaces::Chain::CoinbaseRole::STAKED: roles.emplace_back("staked"); break;
        case interfaces::Chain::CoinbaseRole::ANCHOR: roles.emplace_back("bmm"); break;
        case interfaces::Chain::CoinbaseRole::REPORTER: roles.emplace_back("slashing"); break;
        case interfaces::Chain::CoinbaseRole::VALIDATOR: roles.emplace_back("validator"); break;
        case interfaces::Chain::CoinbaseRole::OTHER: roles.emplace_back(); break;
        }
    }
    return roles;
}

} // namespace webui::local
