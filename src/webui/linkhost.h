// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WEBUI_LINKHOST_H
#define BITCOIN_WEBUI_LINKHOST_H

#include <uint256.h>
#include <univalue.h>
#include <webui/dialog.h>
#include <webui/guard.h>
#include <webui/links.h>
#include <webui/window.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace interfaces {
class Node;
} // namespace interfaces

namespace webui {
class Bridge;
} // namespace webui

/**
 * What the app does with the action links of flush.markets (links.h): each read in the host, resolved against the
 * node by id, quoted again from the node, and shown in the host's own dialog (Window::Ask, in the look of the app,
 * which the page can neither draw nor answer) with where it comes from; nothing runs unless the user accepts there.
 *
 *  - buy, sell, swap: one call of the confirmed RPC methods (confirm.h: buyfromlaunch, selltolaunch, swaproute), its
 *    floor the link's (refused at once when the fresh quote cannot meet it; the node refuses again when it signs);
 *  - launch: reserveasset then launchtoken, in one question (confirm::Reveal), the launch sent by the app once the
 *    reservation is deep enough; a logo named by its SHA-256 is asked for (a file chooser) and refused if it differs;
 *  - register: reserveasset then registerasset, the same way (its supply to this wallet, no market);
 *  - curve, pool, auction, order: one call of curveasset, createpool, createauction, placeorder, every asset by id with
 *    the ticker the link names checked against it, the amounts exactly in their asset's places;
 *  - route, deposit, withdraw, position, reposition, settle, bid, collect, claim: one call of swaproute,
 *    addpoolliquidity, removepoolliquidity, openposition, changeposition, settleorder, bidauction, collectauction,
 *    claimlaunch, each pool, order, auction or position by id with what the link says of it checked against it, the
 *    floors checked against the node's quote of now and given to the call so that it never accepts less;
 *  - pegin: a deposit from the wallet of the eCash node to this wallet's flush.markets address (the link's question,
 *    then the deposit's own); pegout: createwithdrawal to an eCash address, with what a withdrawal takes said first;
 *  - what an action of a link brings in goes to the wallet's flush.markets address (the one connect shares), so that
 *    the site, which knows that address only, sees it (setlinkdestination, for the call alone);
 *  - connect: the address to share and the site, in a question; on a yes, the browser opens the one allowed callback
 *    with it and the site's nonce (the system's own call to open a URI, never a shell).
 *
 * A link is used once (its nonce, or the link itself once it ran) in a session; it expires (links.h).
 */
namespace webui::links {

/** Where a link comes from, as its question says. */
enum class Source {
    //! A second start of the app, or its command line: what a browser hands over.
    BROWSER,
    //! Pasted or dropped into the window (the page hands it over as it is).
    PASTED,
    //! A launch file opened (File > Import launch, or dropped on the window).
    FILE,
};

/** What the host reads of the chain for a link (the node, or the tests' own). */
class Reader
{
public:
    virtual ~Reader() = default;
    struct Asset {
        uint256 id;
        //! Its public name (a ticker), empty if it has none.
        std::string ticker;
        std::string display_name;
        int decimals{0};
        uint32_t seq{0};
    };
    struct Launch {
        uint256 pool;
        bool graduated{false};
    };
    struct Pool {
        uint256 asset0;
        uint256 asset1;
        bool abandoned{false};
    };
    virtual std::optional<Asset> AssetById(const uint256& id) = 0;
    //! Whether an asset of this name is registered (a ticker taken).
    virtual bool NameTaken(const std::string& name) = 0;
    virtual std::optional<Launch> LaunchOf(const uint256& token) = 0;
    //! What a trade alone on a launch's curve gives now: tokens for ECX (buy), ECX for tokens; 0 if nothing.
    virtual uint64_t QuoteLaunch(const uint256& token, bool buy, uint64_t amount) = 0;
    //! What a launch's first buy of `ecx` gives alone, on a new curve of `supply` units (0: the network's) at `fee` (millionths; 0: the network's).
    virtual uint64_t QuoteFirstBuy(uint64_t ecx, uint64_t supply = 0, uint32_t fee = 0) = 0;
    //! Whether the next block takes launches with parameters (a supply, decimals, a fee; an existing asset on a curve).
    virtual bool LaunchParamsActive() = 0;
    //! The network's fee of a launch's curve (millionths).
    virtual uint32_t LaunchFee() = 0;
    virtual std::optional<Pool> PoolById(const uint256& id) = 0;
    //! What `amount` of `asset_in` gives alone in a pool now; 0 if nothing.
    virtual uint64_t QuotePool(const uint256& asset_in, const uint256& pool, uint64_t amount) = 0;
    //! Whether the next block takes launches.
    virtual bool LaunchesActive() = 0;
    //! Blocks until a reservation made by `txid` is deep enough to reveal (0: now); none while it is in no block.
    virtual std::optional<int> RevealWait(const uint256& txid) = 0;
    //! A read of the node (no wallet's): getorder, getauction, getposition, getlaunch, quotepoolliquidity,
    //! previewposition. Its error as a LinkError with the node's words.
    virtual UniValue Read(const std::string& method, const UniValue& params) = 0;
};

/** The reader of the node (its chain's state, as the wallet's RPCs read it). */
std::unique_ptr<Reader> NodeReader(interfaces::Node& node);

/** What a link will do, resolved: the call (and its second step) the host confirms and runs, and its preface. */
struct Plan {
    Request request;
    //! The rows and the text that say where it comes from and what it asks, above the describer's own (confirm::Preface).
    std::vector<dialog::Row> preface;
    std::string message;
    std::string method;
    UniValue params{UniValue::VARR};
    //! launch, register: launchtoken or registerasset, after reserveasset (method).
    std::string then_method;
    UniValue then_params{UniValue::VARR};
    //! What it asks, in a few words ("Buy TIDE with 5 ECX").
    std::string summary;
    //! pegin: the amount deposited (no RPC of this wallet: the host's peg.deposit).
    uint64_t amount_units{0};
};

/** The words for where a link comes from. */
std::string SourceText(Source source);

/**
 * Resolve a buy, a sell, a swap or a launch against the chain (Reader): every id to what it is, the ticker the link names
 * checked against it, the amounts in the units of their assets (what is paid exactly, the floor never lowered), the
 * quote of now checked against the floor. Refused (LinkError) with what is wrong. A launch's logo, if the link names one,
 * must be in request.logo already (its SHA-256 checked).
 */
Plan Resolve(const Request& request, Source source, Reader& reader, int64_t now);

/** The question of a connect: the site, the address shared, where the answer goes. */
Window::ConfirmDialog ConnectQuestion(const Request& request, Source source, const std::string& wallet, const std::string& address, int64_t now);

class LinkHost
{
public:
    struct Deps {
        std::function<interfaces::Node&()> node;
        //! The confirmed RPC methods (confirm::ConfirmedRpc::Call): the question, then the call.
        std::function<UniValue(const UniValue&)> confirmed;
        //! A question of the host (App::AskHost).
        guard::AskFn ask;
        //! Open a web address with the system's browser (Window::OpenExternal); false if it could not.
        std::function<bool(const std::string&)> open;
        //! A file chooser of the desktop (App::Choose).
        std::function<std::optional<std::string>(const Window::FileDialog&)> choose;
        //! Tell the user (a message of the app): a link refused, a launch done.
        std::function<void(const std::string& title, const std::string& message, bool error)> inform;
        //! The wallet the app shows (its preferences); empty if none chosen.
        std::function<std::string()> wallet;
        //! The network of the app ("ecashbeta").
        std::function<std::string()> network;
        std::function<int64_t()> now;
        std::function<std::unique_ptr<Reader>(interfaces::Node&)> reader{NodeReader};
        //! How often a launch looks whether its reservation is deep enough.
        std::chrono::milliseconds poll{2000};
        //! A deposit from the wallet of the mainchain node to a deposit address (SidechainApi's peg.deposit, which asks again).
        std::function<UniValue(const std::string& deposit_address, const std::string& amount)> deposit;
    };
    explicit LinkHost(Deps deps);
    ~LinkHost();
    LinkHost(const LinkHost&) = delete;
    LinkHost& operator=(const LinkHost&) = delete;

    /** At most so many links wait their turn; more are refused (a flood of links opens no flood of questions). */
    static constexpr size_t MAX_QUEUED{4};

    /** A link from the browser (BROWSER): handled on the thread of the links, one at a time; false if refused. */
    bool Enqueue(std::string uri, Source source);
    /** Handle a link now, on this thread: what ran. Throws LinkError (refused) or HostError (declined, failed). */
    UniValue Handle(const std::string& uri, Source source);
    /** Handle a launch file (its text), on this thread. */
    UniValue ImportLaunch(const std::string& json);
    /** The host methods: links.open {uri}, links.importLaunch {text?} (without text: a file chooser). */
    void Register(Bridge& bridge);
    /** Stop the thread of the links and the launches waiting (the app stops). */
    void Stop();

private:
    UniValue Run(Request request, Source source, const std::string& uri);
    UniValue Connect(const Request& request, Source source, const std::string& wallet);
    UniValue Deposit(const Plan& plan, const std::string& wallet);
    std::string Wallet(interfaces::Node& node);
    void Worker();
    void WaitAndLaunch(Plan plan, std::string wallet, std::string reserve_txid);

    Deps m_deps;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::pair<std::string, Source>> m_queue;
    //! The links used in this session (the SHA-256 of each that ran), and the nonces of the connects answered.
    std::set<std::string> m_used;
    std::set<std::string> m_nonces;
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
    std::vector<std::thread> m_launches;
};

} // namespace webui::links

#endif // BITCOIN_WEBUI_LINKHOST_H
