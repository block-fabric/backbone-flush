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

#include <webui/local.h>

#include <addresstype.h>
#include <bitassets/logo.h>
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
} // namespace

std::map<std::string, confirm::Method> ConfirmedRpcMethods()
{
    // FastBlocks: none (bonds are made through fastblocks.bond and fastblocks.unbond, which the host confirms, below).
    // BitAssets: what the pages do that pays or signs: never the page's to call by itself. The host shows each call on the desktop, its
    // destinations whole first, then its amounts, then the rest of its parameters, with the passphrase of a locked wallet
    // typed there, and runs it with the keys unlocked for it alone (confirm.h, audit 7 #1).
    return {
        // Reserved and registered in one question: the registration, exactly as shown, is sent by the app without asking
        // again once the reservation is deep enough (confirm::Reveal).
        {"reserveasset", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, WithNames(confirm::Generic("Reserve an asset", "Reserve", "The wallet pays for a reservation of an asset name."), 0, SIZE_MAX, false), /*fee_unknown=*/true, 1, false, RegisterAfterReserve()}},
        {"registerasset", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, WithNames(WithLogo(confirm::Generic("Register an asset", "Register", "The wallet registers an asset it reserved, with the supply shown."), 5, false), 0, 6, false), /*fee_unknown=*/true}},
        {"setassetlogo", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, WithLogo(confirm::Generic("Set the logo of an asset", "Set the logo", "The wallet puts this logo on the chain for an asset it controls, in place of the one before: everyone sees it where the asset is shown."), 1, true), /*fee_unknown=*/false, 2}},
        {"setassetdisplayname", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, WithNames(confirm::Generic("Set the display name of an asset", "Set the display name", "The wallet puts this display name on the chain for an asset it controls, in place of the one before: everyone sees it after the asset's ticker. Display names are not unique."), SIZE_MAX, 1, true), /*fee_unknown=*/false, 2}},
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
    // mainchain block (-validatoranchorbid), and where the pot it wins goes. Information only: no RPC says it.
    bridge.RegisterHost("fastblocks.anchoring", [node](const UniValue&) -> UniValue {
        interfaces::Node& n{node()};
        UniValue out{UniValue::VOBJ};
        const pos::FastBlocksHooks* hooks{pos::InstalledHooks()};
        const auto anchoring{hooks ? hooks->ValidatorAnchoring() : std::nullopt};
        out.pushKV("on", anchoring.has_value());
        if (anchoring) {
            out.pushKV("bid", FormatMoney(anchoring->second));
            CTxDestination dest;
            out.pushKV("payee", ExtractDestination(anchoring->first, dest) ? EncodeDestination(dest) : HexStr(anchoring->first));
        }
        node::NodeContext* context{n.context()};
        const ArgsManager* args{context ? context->args : nullptr};
        out.pushKV("validator", hooks && hooks->GetProducer() != nullptr);
        if (args && args->IsArgSet("-validatoranchorbid")) out.pushKV("option", guard::OneLine(args->GetArg("-validatoranchorbid", ""), 32));
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
