// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Inspired by BitAssets by LayerTwo Labs (plain-bitassets).

#ifndef BITCOIN_BITASSETS_STATE_H
#define BITCOIN_BITASSETS_STATE_H

#include <consensus/amount.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <serialize.h>
#include <sidechain/store.h>
#include <uint256.h>

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

/**
 * Assets (BitAssets, after the sidechain of that name by LayerTwo Labs).
 *
 *  - An **asset** is registered under a name, in two steps as BitNames are (a
 *    **reservation** commits to the name under a secret nonce, a later
 *    **registration** reveals it), and is known on the chain by the SHA-256
 *    hash of the name. The registration creates its initial supply and its
 *    **control** coin: whoever holds the control coin mints more and changes
 *    the asset's data. Burning the control coin fixes the supply for good.
 *  - Asset coins, control coins, reservations, liquidity (LP) shares and
 *    auction receipts are **tokens**, carried by outputs of no value that a
 *    **marker** output (an OP_RETURN) lists, each with what it carries. Every
 *    token is accounted for: what a transaction spends, plus what its
 *    operation creates, is exactly what it outputs, plus what its operation
 *    takes in. Nothing is burned by accident.
 *  - **Pools** (a constant-product market maker, 0.3% to the liquidity
 *    providers) trade any two assets, CHN included. **Dutch auctions** sell an
 *    amount of one asset for another at a price that falls block by block.
 *  - CHN a transaction puts into a pool or an auction is burned by the marker
 *    (its value); CHN a pool or an auction pays out is paid by the coinbase of
 *    the block, as deposits are. Asset coins a pool or an auction pays out go
 *    to the **result** outputs the marker lists, in the amount the state
 *    gives when the block is connected, never less than the transaction asks.
 */
namespace bitassets {

/** An asset: the hash of its name. The null hash is CHN, the coin of the chain. */
using AssetId = uint256;
inline const AssetId CHN{};

/** OP_RETURN, then a push of these bytes and what follows. */
inline constexpr unsigned char TAG[4]{0x42, 0x41, 0x53, 0x54}; // "BAST"
inline constexpr uint8_t MARKER_VERSION{0};

/** Most outputs one transaction lists. */
inline constexpr size_t MAX_MARKER_OUTPUTS{64};
/** Longest name of an asset that a registration publishes. */
inline constexpr size_t MAX_NAME_TEXT_SIZE{64};
/** Longest display name of an asset, in bytes of UTF-8 (IsDisplayName). */
inline constexpr size_t MAX_DISPLAY_NAME_SIZE{64};
/** Shortest and longest ticker: the name a registration publishes from SidechainParams::ticker_height (IsTicker). */
inline constexpr size_t MIN_TICKER_SIZE{2};
inline constexpr size_t MAX_TICKER_SIZE{12};
/** Longest description of an asset. */
inline constexpr size_t MAX_INFO_SIZE{512};
/** Most decimals an asset is shown with. */
inline constexpr uint8_t MAX_DECIMALS{12};
/** The most units of an asset there can be, and the most any amount can be. */
inline constexpr uint64_t MAX_AMOUNT{static_cast<uint64_t>(std::numeric_limits<int64_t>::max())};
/** Liquidity shares a new pool keeps for good, so that a pool can never be emptied (and its price made anything). */
inline constexpr uint64_t MIN_LIQUIDITY{1000};
/** Shares the deposit that opens a pool (or reopens an abandoned one) must give, MIN_LIQUIDITY aside. */
inline constexpr uint64_t MIN_OPEN_SHARES{100'000};
/** CHN the deposit that opens a pool with CHN must put in: 0.01 CHN. */
inline constexpr uint64_t MIN_OPEN_CHN{1'000'000};
/** The pools keep 3 of every 1000 units traded, for the liquidity providers. */
inline constexpr uint64_t SWAP_FEE_PER_MILLE{3};
/** Longest an auction may run, in blocks. */
inline constexpr int32_t MAX_AUCTION_DURATION{1'000'000};
/** Latest an auction may start, in blocks after the one that makes it: about a year. */
inline constexpr int32_t MAX_AUCTION_DELAY{52'560};
/**
 * Least CHN (ECX) a pool, a book, a curve or an auction pays out, MinChnPayout(height): 0.0005 ECX
 * (50 000 sat, MIN_CHN_PAYOUT_BEFORE_SMALL) before SidechainParams::small_payout_height, 0.00005 ECX (5 000 sat,
 * MIN_CHN_PAYOUT_SMALL) from it: about $0.50 at $10 000 an ECX, the least order of a book that still means something.
 * A payout takes a place in the coinbase's queue, which pays a bounded number per block
 * (MAX_TX_PAYOUTS_PER_BLOCK for the transactions' payouts, half of MAX_PAYOUTS_PER_BLOCK): small payouts cannot delay
 * anyone's by more than a block, whatever their size, as the queue is bounded, not their amount. What a smaller
 * least changes is the cost of filling that queue: 500 swaps of 5 000 sat (0.025 ECX turned over, each paying its
 * pool's fee and the relay fee of PAYOUT_POLICY_VSIZE for its payout, so about 0.0004 ECX of fees a block at the
 * minimum relay fee, and a pool's fee each way), against 0.25 ECX turned over before; the fees, not the amounts, are
 * what such a flood costs, and they stay. A payout of 5 000 sat is half the least withdrawal (min_withdrawal, 10 000
 * sat: a withdrawal of it gathers two), and an output worth spending at any fee rate below about 70 sat/vB (an input
 * of about 68 vbytes), seventy times eCash's usual 1 sat/byte; the UTXO set grows by at most 500 such outputs a
 * block, as before. Less from a swap or a bid is refused; less taken out of a pool, or coming back from a deposit,
 * stays in it, for its providers. An auction selling CHN sells this much at least, and a bid leaves no less of it
 * (or none); a bid in CHN pays this much at least: whatever an auction holds in CHN can be paid out. (Less from
 * collecting an auction is not paid: the CHN were burned when they went in.)
 */
inline constexpr uint64_t MIN_CHN_PAYOUT_BEFORE_SMALL{50'000};
inline constexpr uint64_t MIN_CHN_PAYOUT_SMALL{5'000};
/** The least CHN paid out in a block at `height` (SidechainParams::SmallPayoutsActive, the network's rules). */
uint64_t MinChnPayout(int height);
/** The least CHN paid out where small payouts are `active` or not. */
constexpr uint64_t MinChnPayoutOf(bool small_payouts_active) { return small_payouts_active ? MIN_CHN_PAYOUT_SMALL : MIN_CHN_PAYOUT_BEFORE_SMALL; }
/**
 * Most CHN payouts the transactions of a block may give rise to (swaps, bids, liquidity taken out,
 * auctions collected); a transaction that would go beyond is not valid in that block, and waits for
 * the next (the miner leaves it out, and fills the block around it). The half of a block that the
 * queue of these payouts is sure to get (sidechain::State::TakePayouts): transactions cannot make
 * that queue grow, so a flood of small swaps delays nobody's payout by more than a block, and refunds
 * and deposits, which have the other half, not at all. A transaction has one action and so at most
 * two payouts; 500 is about one in eight of the transactions a full block holds, many more than
 * pay out CHN in ordinary use. Kept at 500 with the smaller least payout (MinChnPayout): it bounds the count of
 * payouts and their outputs, which a smaller amount does not change.
 */
inline constexpr size_t MAX_TX_PAYOUTS_PER_BLOCK{500};
/**
 * Relay policy, not consensus: a transaction pays at least the minimum relay fee of this many virtual
 * bytes for each CHN payout and each refund of a withdrawal it would give rise to, or the mempool
 * refuses it ("ba-payouts-too-dense"). A payout takes one of the MAX_TX_PAYOUTS_PER_BLOCK, and a refund
 * one of the sidechain::MAX_REFUNDS_PER_BLOCK, of a block, which are scarcer than its weight: without
 * this, a transaction of 40 refunds takes 40 of the 100 for the minimum fee of its 4700 vbytes (470
 * sat at 0.1 sat/vB); it now pays 3000. The 600 of a block then cost the minimum fee of 450 000
 * vbytes, about half a block of transactions. What a wallet makes is never refused: it pays 1 sat/vB
 * at least (-mintxfee, ten times the minimum relay fee), a refund output alone takes 116 vbytes, and a
 * transaction has one action, which pays out once at most, in 150 vbytes or more.
 */
inline constexpr int64_t PAYOUT_POLICY_VSIZE{750};
/**
 * Upgrade v8: the most tokens of the index of buybacks a block looks at (State::ProtocolOrders): its work bounded
 * whatever the count of launches; the next block goes on from where it stopped, so each comes round in turn.
 */
inline constexpr size_t MAX_BUYBACK_SCAN{64};
/** Most tokens one burn names. */
inline constexpr size_t MAX_BURNS{16};

/** Serializes a std::optional as a flag and, if set, the value. */
struct OptionalFormatter {
    template <typename Stream, typename T>
    void Ser(Stream& s, const std::optional<T>& v)
    {
        s << v.has_value();
        if (v) s << *v;
    }
    template <typename Stream, typename T>
    void Unser(Stream& s, std::optional<T>& v)
    {
        bool has_value;
        s >> has_value;
        v.reset();
        if (has_value) s >> v.emplace();
    }
};
#define BA_OPT(x) Using<OptionalFormatter>(x)

struct SocketV4 {
    std::array<uint8_t, 4> ip{};
    uint16_t port{0};
    SERIALIZE_METHODS(SocketV4, obj) { READWRITE(obj.ip, obj.port); }
    friend bool operator==(const SocketV4&, const SocketV4&) = default;
};

struct SocketV6 {
    std::array<uint8_t, 16> ip{};
    uint16_t port{0};
    SERIALIZE_METHODS(SocketV6, obj) { READWRITE(obj.ip, obj.port); }
    friend bool operator==(const SocketV6&, const SocketV6&) = default;
};

/** A compressed secp256k1 public key, to encrypt messages to the issuer. */
using EncryptionKey = std::array<uint8_t, 33>;
/** An x-only secp256k1 public key (BIP340) the issuer signs with. */
using SigningKey = uint256;

/** The data of an asset, which its controller changes. Every field may be absent. */
struct AssetData {
    std::optional<uint256> commitment;
    std::optional<SocketV4> ipv4;
    std::optional<SocketV6> ipv6;
    std::optional<EncryptionKey> encryption_key;
    std::optional<SigningKey> signing_key;
    //! What the asset is: a description, a link.
    std::optional<std::string> info;

    SERIALIZE_METHODS(AssetData, obj) { READWRITE(BA_OPT(obj.commitment), BA_OPT(obj.ipv4), BA_OPT(obj.ipv6), BA_OPT(obj.encryption_key), BA_OPT(obj.signing_key), BA_OPT(obj.info)); }
    friend bool operator==(const AssetData&, const AssetData&) = default;
};

// windows.h (winnt.h) defines DELETE, an access right, as a macro; it would break the enumerator below.
#if defined(WIN32) && defined(DELETE)
#undef DELETE
#endif

/** How an update treats one field: delete it, leave it, or set it. */
enum class UpdateKind : uint8_t { DELETE = 0, RETAIN = 1, SET = 2 };

template <typename T>
struct Update {
    UpdateKind kind{UpdateKind::RETAIN};
    std::optional<T> value;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << static_cast<uint8_t>(kind);
        if (kind == UpdateKind::SET) s << *value;
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        uint8_t k;
        s >> k;
        if (k > static_cast<uint8_t>(UpdateKind::SET)) throw std::ios_base::failure("unknown update kind");
        kind = static_cast<UpdateKind>(k);
        value.reset();
        if (kind == UpdateKind::SET) s >> value.emplace();
    }
    friend bool operator==(const Update&, const Update&) = default;
};

struct AssetUpdates {
    Update<uint256> commitment;
    Update<SocketV4> ipv4;
    Update<SocketV6> ipv6;
    Update<EncryptionKey> encryption_key;
    Update<SigningKey> signing_key;
    Update<std::string> info;

    SERIALIZE_METHODS(AssetUpdates, obj) { READWRITE(obj.commitment, obj.ipv4, obj.ipv6, obj.encryption_key, obj.signing_key, obj.info); }
    friend bool operator==(const AssetUpdates&, const AssetUpdates&) = default;
    bool Empty() const;
};

/** What an output carries. */
struct Token {
    enum class Kind : uint8_t {
        ASSET = 0,       //!< `amount` units of the asset `id`
        CONTROL = 1,     //!< the control coin of the asset `id`
        RESERVATION = 2, //!< the reservation made by the transaction `id`
        LP = 3,          //!< `amount` shares of the pool `id`
        RECEIPT = 4,     //!< the right to what the auction `id` (the transaction that made it) brings in
        POSITION = 5,    //!< a position of concentrated liquidity, `id` (OpenPosition; the DEX's pool kinds only)
        ORDER = 6,       //!< a limit order resting in a book, `id` (PlaceOrder; the DEX's pool kinds only)
    };
    Kind kind{Kind::ASSET};
    uint256 id;
    //! For ASSET and LP; 1 for the others.
    uint64_t amount{1};

    SERIALIZE_METHODS(Token, obj)
    {
        uint8_t k{static_cast<uint8_t>(obj.kind)};
        READWRITE(k, obj.id, VARINT(obj.amount));
        SER_READ(obj, if (k > static_cast<uint8_t>(Kind::ORDER)) throw std::ios_base::failure("unknown token kind"));
        SER_READ(obj, obj.kind = static_cast<Kind>(k));
    }
    friend bool operator==(const Token&, const Token&) = default;
    /** Whether an amount goes with the kind. */
    static bool Divisible(Kind kind) { return kind == Kind::ASSET || kind == Kind::LP; }
};

/** A token kind's name (gettxout, decodeassettx): asset, control, reservation, lp, receipt, position, order; "unknown" past them. */
inline const char* TokenKindName(Token::Kind kind)
{
    static constexpr const char* NAMES[]{"asset", "control", "reservation", "lp", "receipt", "position", "order"};
    const auto k{static_cast<size_t>(kind)};
    return k < std::size(NAMES) ? NAMES[k] : "unknown";
}

/** An output the marker lists: what it carries, or that it takes a result of the operation. */
struct MarkerOutput {
    uint32_t n{0};
    //! Nothing: the output takes the next result of the operation.
    std::optional<Token> token;
    friend bool operator==(const MarkerOutput&, const MarkerOutput&) = default;
};

//
// What a transaction does, beyond moving tokens.
//

struct Reserve {
    static constexpr uint8_t KIND{1};
    //! ReservationCommitment(name, nonce, script of the reservation's output).
    uint256 commitment;
    SERIALIZE_METHODS(Reserve, obj) { READWRITE(obj.commitment); }
    friend bool operator==(const Reserve&, const Reserve&) = default;
};

/** Register an asset: spends its reservation, creates its control coin and its initial supply. */
struct Register {
    static constexpr uint8_t KIND{2};
    AssetId name;
    uint256 nonce;
    uint64_t supply{0};
    uint8_t decimals{0};
    AssetData data;
    //! The name, if the registration makes it public.
    std::optional<std::string> text;
    //! Its logo (bitassets/logo.h), if any: from SidechainParams::logo_height, a registration with one
    //! is a marker of the kind REGISTER_WITH_LOGO_KIND (these fields, then the logo).
    std::vector<unsigned char> logo;
    //! Its display name (IsDisplayName), if any: from SidechainParams::ticker_height, a registration with
    //! one is a marker of the kind REGISTER_WITH_DISPLAY_NAME_KIND (these fields, the logo, empty or not, then it).
    std::string display_name;
    SERIALIZE_METHODS(Register, obj) { READWRITE(obj.name, obj.nonce, VARINT(obj.supply), obj.decimals, obj.data, BA_OPT(obj.text)); }
    friend bool operator==(const Register&, const Register&) = default;
};
/** The kind of the marker of a Register that carries a logo. */
inline constexpr uint8_t REGISTER_WITH_LOGO_KIND{33};
/** The kind of the marker of a Register that carries a display name (and a logo, or an empty one). */
inline constexpr uint8_t REGISTER_WITH_DISPLAY_NAME_KIND{35};

/** More of an asset, by whoever spends its control coin. */
struct Mint {
    static constexpr uint8_t KIND{3};
    AssetId asset;
    uint64_t amount{0};
    SERIALIZE_METHODS(Mint, obj) { READWRITE(obj.asset, VARINT(obj.amount)); }
    friend bool operator==(const Mint&, const Mint&) = default;
};

/** Change the data of an asset, by whoever spends its control coin. */
struct UpdateAsset {
    static constexpr uint8_t KIND{4};
    AssetId asset;
    AssetUpdates updates;
    SERIALIZE_METHODS(UpdateAsset, obj) { READWRITE(obj.asset, obj.updates); }
    friend bool operator==(const UpdateAsset&, const UpdateAsset&) = default;
};

/** Destroy tokens the transaction spends: asset coins, control coins (the supply is then fixed), reservations. */
struct Burn {
    static constexpr uint8_t KIND{5};
    std::vector<Token> tokens;
    SERIALIZE_METHODS(Burn, obj) { READWRITE(obj.tokens); }
    friend bool operator==(const Burn&, const Burn&) = default;
};

/** Trade `amount_in` of one asset for at least `min_out` of another, in their pool. Result: what comes out. */
struct Swap {
    static constexpr uint8_t KIND{6};
    AssetId asset_in;
    uint64_t amount_in{0};
    AssetId asset_out;
    uint64_t min_out{0};
    //! Where the coinbase pays the CHN a result is in.
    CScript chn_to;
    SERIALIZE_METHODS(Swap, obj) { READWRITE(obj.asset_in, VARINT(obj.amount_in), obj.asset_out, VARINT(obj.min_out), obj.chn_to); }
    friend bool operator==(const Swap&, const Swap&) = default;
};

/**
 * Put two assets in their pool (making it, if there is none). Results: the shares, at least
 * `min_shares`, then what goes back.
 *
 * The amounts are the most that goes in (amm::Provide), and the marker lists two result outputs. Into a pool someone provides
 * liquidity to, the side that gives fewer shares goes in whole, and of the other only what those
 * shares are worth at the pool's price: the rest goes back, as asset coins to the second result
 * output, or, for CHN, paid by the coinbase to its script (CHN under MinChnPayout stays in the
 * pool). A new pool, or one nobody provides liquidity to any more, is opened by the amounts as a
 * new pool is made, what it holds merged in, and nothing goes back: the second output carries nothing.
 */
struct AddLiquidity {
    static constexpr uint8_t KIND{7};
    AssetId asset_a;
    AssetId asset_b;
    uint64_t amount_a{0};
    uint64_t amount_b{0};
    uint64_t min_shares{0};
    SERIALIZE_METHODS(AddLiquidity, obj) { READWRITE(obj.asset_a, obj.asset_b, VARINT(obj.amount_a), VARINT(obj.amount_b), VARINT(obj.min_shares)); }
    friend bool operator==(const AddLiquidity&, const AddLiquidity&) = default;
};

/** Give shares of a pool back. Results: its two assets, in the order named, at least the least amounts named. */
struct RemoveLiquidity {
    static constexpr uint8_t KIND{8};
    AssetId asset_a;
    AssetId asset_b;
    uint64_t shares{0};
    uint64_t min_a{0};
    uint64_t min_b{0};
    CScript chn_to;
    SERIALIZE_METHODS(RemoveLiquidity, obj) { READWRITE(obj.asset_a, obj.asset_b, VARINT(obj.shares), VARINT(obj.min_a), VARINT(obj.min_b), obj.chn_to); }
    friend bool operator==(const RemoveLiquidity&, const RemoveLiquidity&) = default;
};

/**
 * Sell `base_amount` of an asset for another, by Dutch auction: the price of all of it starts at
 * `start_price` at `start_height` and falls in a straight line to `end_price` over `duration`
 * blocks; a bid buys at the price of its block. The transaction outputs the receipt, which
 * collects what the auction brings in.
 */
struct CreateAuction {
    static constexpr uint8_t KIND{9};
    AssetId base;
    uint64_t base_amount{0};
    AssetId quote;
    uint64_t start_price{0};
    uint64_t end_price{0};
    int32_t start_height{0};
    int32_t duration{0};
    SERIALIZE_METHODS(CreateAuction, obj) { READWRITE(obj.base, VARINT(obj.base_amount), obj.quote, VARINT(obj.start_price), VARINT(obj.end_price), obj.start_height, obj.duration); }
    friend bool operator==(const CreateAuction&, const CreateAuction&) = default;
};

/**
 * Pay `quote_amount` into an auction. Result: what it buys, at least `min_base`. One that would buy
 * more than is left buys all of it, for what that costs, if it can be given the rest back: in CHN,
 * by the coinbase to `chn_to`; in an asset, to one more result output, after that of what it buys
 * (which carries nothing if nothing comes back). Otherwise it is refused.
 */
struct Bid {
    static constexpr uint8_t KIND{10};
    Txid auction;
    uint64_t quote_amount{0};
    uint64_t min_base{0};
    CScript chn_to;
    SERIALIZE_METHODS(Bid, obj) { READWRITE(obj.auction, VARINT(obj.quote_amount), VARINT(obj.min_base), obj.chn_to); }
    friend bool operator==(const Bid&, const Bid&) = default;
};

/**
 * Close an auction with its receipt, once it has ended, sold out, or before any bid (to cancel it).
 * Results: what is left of what it sold, then what it brought in; amounts of nothing are left out.
 */
struct Collect {
    static constexpr uint8_t KIND{11};
    Txid auction;
    CScript chn_to;
    SERIALIZE_METHODS(Collect, obj) { READWRITE(obj.auction, obj.chn_to); }
    friend bool operator==(const Collect&, const Collect&) = default;
};

/**
 * Retire a dead asset: one whose supply is fixed and whose every unit is in pools nobody provides
 * liquidity to (nobody holds any, nothing is up for auction). The asset and its pools go, and its
 * name is free again. What its pools held of other assets is burned; the CHN they held is paid to
 * mainchain miners, as the fee of a withdrawal in the next bundle. Anyone may do it.
 */
struct ReleaseAsset {
    static constexpr uint8_t KIND{12};
    AssetId asset;
    SERIALIZE_METHODS(ReleaseAsset, obj) { READWRITE(obj.asset); }
    friend bool operator==(const ReleaseAsset&, const ReleaseAsset&) = default;
};

//
// Layers' DEX (from SidechainParams::dex_height; bitassets/dex.h, doc/layers.md "The DEX"). Before that
// height a marker of these kinds is not valid (bad-ba-marker), as it never was.
//

/**
 * Trade `amount_in` of path[0] for at least `min_out` of path.back(), through the pools of each two
 * assets that follow each other on the path (2 to MAX_ROUTE_ASSETS assets, all different), in one
 * transaction: each hop pays its pool's fee. The trade is an order of the block's batch (Swap is one of
 * one hop from the DEX's height on): it clears at the end of the block with every other order of the
 * block. One result output, which gets what comes out (or, if the order does not fill, what went in
 * back); CHN out (or back) is paid by the coinbase to `chn_to`, which the order names when either end
 * is CHN.
 */
struct Route {
    static constexpr uint8_t KIND{13};
    std::vector<AssetId> path;
    uint64_t amount_in{0};
    uint64_t min_out{0};
    CScript chn_to;
    SERIALIZE_METHODS(Route, obj) { READWRITE(obj.path, VARINT(obj.amount_in), VARINT(obj.min_out), obj.chn_to); }
    friend bool operator==(const Route&, const Route&) = default;
};

/**
 * Register an oracle: its key, which is its identity (dex::OracleId), and a name that labels it (not
 * unique). `proof` is the key's signature (BIP340) over dex::RegistrationHash: nobody registers a key
 * that is not theirs. The marker burns ORACLE_REGISTRATION_FEE, which goes to mainchain miners.
 */
struct RegisterOracle {
    static constexpr uint8_t KIND{14};
    std::string name;
    //! An x-only secp256k1 public key (BIP340).
    uint256 pubkey;
    std::array<unsigned char, 64> proof{};
    SERIALIZE_METHODS(RegisterOracle, obj) { READWRITE(obj.name, obj.pubkey, obj.proof); }
    friend bool operator==(const RegisterOracle&, const RegisterOracle&) = default;
};

/** What an oracle says of an event: `value`, signed by its key (dex::AttestationHash), for a block at `height` or later. Once per event. */
struct Attest {
    static constexpr uint8_t KIND{15};
    uint256 oracle;
    uint256 event;
    int64_t value{0};
    int32_t height{0};
    std::array<unsigned char, 64> signature{};
    SERIALIZE_METHODS(Attest, obj) { READWRITE(obj.oracle, obj.event, obj.value, obj.height, obj.signature); }
    friend bool operator==(const Attest&, const Attest&) = default;
};

/** An outcome of an oracle contract: the values from `lo` to `hi` pay the collateral to `script`. */
struct OracleOutcome {
    int64_t lo{0};
    int64_t hi{0};
    CScript script;
    SERIALIZE_METHODS(OracleOutcome, obj) { READWRITE(obj.lo, obj.hi, obj.script); }
    friend bool operator==(const OracleOutcome&, const OracleOutcome&) = default;
};

/**
 * Lock `amount` of an asset (CHN included) until an oracle attests an event: then the first outcome
 * whose range holds the value gets all of it (none: `refund`); with no attestation in a block up to
 * `expiry`, `refund` gets it after that block. Settled by anyone (SettleOracleContract): the oracle
 * chooses among the scripts named here, and can pay nobody else.
 */
struct CreateOracleContract {
    static constexpr uint8_t KIND{16};
    uint256 oracle;
    uint256 event;
    AssetId asset;
    uint64_t amount{0};
    std::vector<OracleOutcome> outcomes;
    CScript refund;
    int32_t expiry{0};
    SERIALIZE_METHODS(CreateOracleContract, obj) { READWRITE(obj.oracle, obj.event, obj.asset, VARINT(obj.amount), obj.outcomes, obj.refund, obj.expiry); }
    friend bool operator==(const CreateOracleContract&, const CreateOracleContract&) = default;
};

/** Pay an oracle contract out. CHN by the coinbase; an asset to the one result output, whose script must be the winner's. */
struct SettleOracleContract {
    static constexpr uint8_t KIND{17};
    Txid contract;
    SERIALIZE_METHODS(SettleOracleContract, obj) { READWRITE(obj.contract); }
    friend bool operator==(const SettleOracleContract&, const SettleOracleContract&) = default;
};




/**
 * Set the logo of an asset (bitassets/logo.h), or remove it (an empty one), by whoever spends its
 * control coin; it replaces the one before. Valid from SidechainParams::logo_height (kinds 32 and 33
 * stay clear of those Layers adds, 13 on).
 */
struct SetLogo {
    static constexpr uint8_t KIND{32};
    AssetId asset;
    std::vector<unsigned char> logo;
    SERIALIZE_METHODS(SetLogo, obj) { READWRITE(obj.asset, obj.logo); }
    friend bool operator==(const SetLogo&, const SetLogo&) = default;
};

/**
 * Set the display name of an asset (IsDisplayName), or remove it (an empty one), by whoever spends its
 * control coin; it replaces the one before. Display names are not unique: the name of an asset (its
 * ticker) is. Valid from SidechainParams::ticker_height.
 */
struct SetDisplayName {
    static constexpr uint8_t KIND{34};
    AssetId asset;
    std::string display_name;
    SERIALIZE_METHODS(SetDisplayName, obj) { READWRITE(obj.asset, obj.display_name); }
    friend bool operator==(const SetDisplayName&, const SetDisplayName&) = default;
};

//
// The DEX's pool kinds (from SidechainParams::dex_curves_height, with the DEX's rules; bitassets/curves.h,
// doc/layers.md "Pool kinds"). Before that height a marker of these kinds is not valid (bad-ba-marker), as
// it never was. Kinds 18 to 31: clear of the DEX's 13 to 17 and of logos' and display names' 32 to 35.
//

/**
 * Make a pool of a kind (curve, fee tier, amplification or weights) and put its first liquidity in. The
 * pool's id is PoolIdOf(asset_a, asset_b, its parameters): a constant-product pool at 0.30 % is the pool
 * of the two assets that AddLiquidity makes (PoolId), any other kind a pool of its own, so that a pair has
 * a pool of each kind. One result output: the shares (MIN_LIQUIDITY more kept for good), at least
 * `min_shares`. Both amounts go in whole. A pool of the kind that exists and that someone provides
 * liquidity to is not made again (bad-ba-pool-exists); one nobody provides liquidity to any more is opened
 * again, as AddLiquidity opens one.
 */
struct CreatePool {
    static constexpr uint8_t KIND{18};
    AssetId asset_a;
    AssetId asset_b;
    //! Curve.
    uint8_t curve{0};
    //! Of FEE_DENOMINATOR (millionths): one of FEE_TIERS.
    uint32_t fee{0};
    //! STABLE: the amplification A, MIN_AMP to MAX_AMP; 0 for the others.
    uint32_t amp{0};
    //! WEIGHTED: the weight of asset_a in percent, one of WEIGHTS; 0 for the others.
    uint8_t weight_a{0};
    //! POOL_FLAG_*: none yet.
    uint8_t flags{0};
    uint64_t amount_a{0};
    uint64_t amount_b{0};
    uint64_t min_shares{0};
    SERIALIZE_METHODS(CreatePool, obj)
    {
        READWRITE(obj.asset_a, obj.asset_b, obj.curve, VARINT(obj.fee), VARINT(obj.amp), obj.weight_a, obj.flags, VARINT(obj.amount_a), VARINT(obj.amount_b),
                  VARINT(obj.min_shares));
    }
    friend bool operator==(const CreatePool&, const CreatePool&) = default;
};

/**
 * Put liquidity into a pool by its id: any amounts of its two assets (asset0, asset1 order), one of them
 * zero for a deposit of one side. The shares are what the deposit adds to the pool's invariant
 * (curves::DepositShares): a deposit at the pool's proportions gets what AddLiquidity gives; what is out of
 * proportion pays the pool's fee on the part a swap would trade. All of it goes in. One result output: the
 * shares, at least `min_shares`. Into a pool nobody provides liquidity to, both amounts, as CreatePool.
 */
struct PoolDeposit {
    static constexpr uint8_t KIND{19};
    uint256 pool;
    uint64_t amount0{0};
    uint64_t amount1{0};
    uint64_t min_shares{0};
    SERIALIZE_METHODS(PoolDeposit, obj) { READWRITE(obj.pool, VARINT(obj.amount0), VARINT(obj.amount1), VARINT(obj.min_shares)); }
    friend bool operator==(const PoolDeposit&, const PoolDeposit&) = default;
};

/**
 * Give shares of a pool back, by its id. `only` 0: both assets, what the shares are worth (as
 * RemoveLiquidity); 1 or 2: asset0 or asset1 alone, what the shares are worth of it plus what the other
 * side's part buys of it, alone, in the pool after (its fee paid). Results: the assets that come out, in
 * the pool's order, at least `min0` and `min1`; CHN by the coinbase to `chn_to`.
 */
struct PoolWithdraw {
    static constexpr uint8_t KIND{20};
    uint256 pool;
    uint64_t shares{0};
    uint8_t only{0};
    uint64_t min0{0};
    uint64_t min1{0};
    CScript chn_to;
    SERIALIZE_METHODS(PoolWithdraw, obj) { READWRITE(obj.pool, VARINT(obj.shares), obj.only, VARINT(obj.min0), VARINT(obj.min1), obj.chn_to); }
    friend bool operator==(const PoolWithdraw&, const PoolWithdraw&) = default;
};

/**
 * A route through named pools (Route names assets, and so the pool of each two, of the kind AddLiquidity
 * makes): `amount_in` of `asset_in` through each pool in turn, 1 to MAX_ROUTE_ASSETS - 1 of them, each
 * holding the asset the hop before gave, the assets all different; at least `min_out` of what the last
 * gives. An order of the block's batch, as Route: one result output; CHN out or back to `chn_to`.
 */
struct SwapPools {
    static constexpr uint8_t KIND{21};
    AssetId asset_in;
    std::vector<uint256> pools;
    uint64_t amount_in{0};
    uint64_t min_out{0};
    CScript chn_to;
    SERIALIZE_METHODS(SwapPools, obj) { READWRITE(obj.asset_in, obj.pools, VARINT(obj.amount_in), VARINT(obj.min_out), obj.chn_to); }
    friend bool operator==(const SwapPools&, const SwapPools&) = default;
};

/**
 * Upgrade v7 (SidechainParams::v7_height): one recipient of the tunable part of a launch's fees (the curve's fee but the
 * chain's security part, 75 %; and the creator's part of its locked position's fees, 60 %), in basis points of that part.
 * A split has 1 to MAX_FEE_SHARES of them, each of 1 bps at least, 10 000 in all: the creator at most once, a buyback and
 * burn at most once, and up to MAX_FEE_ADDRESSES other scripts, all different (CheckFeeSplit). The creator's part is
 * claimed (ClaimLaunch), as before; the burn's buys the token back in its venue each block and burns it, as v5's; an
 * address's is paid by the coinbase, without a claim, once MIN_FEE_SHARE_PAYOUT is owed it (State::FinishBlock).
 */
struct FeeShare {
    enum Kind : uint8_t { CREATOR = 0, BURN = 1, ADDRESS = 2 };
    uint8_t kind{CREATOR};
    //! ADDRESS: the script paid (spendable); empty for the others.
    CScript script;
    uint16_t bps{0};
    SERIALIZE_METHODS(FeeShare, obj) { READWRITE(obj.kind, obj.script, obj.bps); }
    friend bool operator==(const FeeShare&, const FeeShare&) = default;
};
inline constexpr size_t MAX_FEE_SHARES{5};
inline constexpr size_t MAX_FEE_ADDRESSES{3};
inline constexpr uint16_t FEE_SPLIT_BPS{10'000};
/** Longest script a split pays (a standard one: P2WSH and P2TR are 34 bytes). */
inline constexpr size_t MAX_FEE_SHARE_SCRIPT_SIZE{80};
/** The least an address of a split is paid at once (0.001 ECX): what it is owed waits until it is this much. */
inline constexpr uint64_t MIN_FEE_SHARE_PAYOUT{100'000};
/** Whether `split` is one a launch may have (FeeShare). */
bool CheckFeeSplit(const std::vector<FeeShare>& split);
/**
 * `coin` by `split` (CheckFeeSplit): each share the floor of its part, the roundings' remainder (less than a satoshi a
 * share) to the creator's share, else the burn's, else the first: the parts sum to `coin` exactly.
 */
std::vector<uint64_t> SplitCoin(const std::vector<FeeShare>& split, uint64_t coin);
/** The split of v5's destinations (FeeDestination, the burn's part of a split): what a launch made from v7 with them has. */
std::vector<FeeShare> SplitOfDestination(uint8_t destination, uint16_t burn_bps);
/**
 * The kind of the marker of a LaunchToken with a split (upgrade v7): its fields, `fee` (VARINT, as LAUNCH_V5_KIND),
 * `keep_control` and `split` (CheckFeeSplit); `destination` and `burn_bps` are v5's and not in it (0).
 */
inline constexpr uint8_t LAUNCH_V7_KIND{38};

/**
 * Launch an asset on a bonding curve (doc/layers.md "Launches"): register an asset (`reg`: the name revealed from
 * its reservation, as Register, its ticker, logo, display name and description) whose supply sits on a launch curve
 * priced in the chain's coin, fixed, with no control coin. Of the kind KIND (`fee` 0): supply 0 and LAUNCH_DECIMALS
 * in `reg`, the rules give it LAUNCH_SUPPLY and the curve the network's launch_fee. Of the kind
 * LAUNCH_WITH_PARAMS_KIND (from SidechainParams::launch_params_height; `fee` not 0): its supply and decimals are
 * `reg`'s (IsLaunchSupply), its curve's fee `fee` (millionths, MIN_LAUNCH_FEE..MAX_LAUNCH_FEE). Buyers pay the coin
 * into the curve and sellers get it back (orders of the block's batch through the curve's pool, SwapPools); once the
 * curve has raised SidechainParams::launch_graduation, its coin and the assets left move into the asset's pool with
 * the coin (the kind AddLiquidity makes), whose shares nobody holds: locked for good.
 * `creator` gets its part of the curve's fees (ClaimLaunch), and, if `allocation` (basis points of the supply, at
 * most MAX_LAUNCH_ALLOCATION) is not 0, that many more units once it graduated. `min_buy`, `max_buy` (the chain's
 * coin, 0: none) bound each buy. `first_buy` (0: none): the launch's own buy, an order of its block (its result
 * output, at least `first_min_out`; CHN back to `chn_to`).
 */
struct LaunchToken {
    static constexpr uint8_t KIND{22};
    Register reg;
    CScript creator;
    uint16_t allocation{0};
    uint64_t min_buy{0};
    uint64_t max_buy{0};
    uint64_t first_buy{0};
    uint64_t first_min_out{0};
    CScript chn_to;
    //! The curve's fee (millionths): 0 for a launch of the kind KIND (the network's), else of LAUNCH_WITH_PARAMS_KIND.
    uint32_t fee{0};
    //! From SidechainParams::fee_flow_height (a marker of LAUNCH_V5_KIND): where the creator's part of its fees goes
    //! (FeeDestination), the burn's part of a split (basis points), and whether its creator keeps the control coin.
    //! (No vesting: the allocation is the creator's at graduation, as before; user decision 2026-10-24.)
    uint8_t destination{0};
    uint16_t burn_bps{0};
    bool keep_control{false};
    //! From SidechainParams::v7_height (a marker of LAUNCH_V7_KIND): the split of its fees' tunable part (empty: v5's destination).
    std::vector<FeeShare> split;
    SERIALIZE_METHODS(LaunchToken, obj)
    {
        READWRITE(obj.reg, obj.reg.logo, obj.reg.display_name, obj.creator, obj.allocation, VARINT(obj.min_buy), VARINT(obj.max_buy), VARINT(obj.first_buy), VARINT(obj.first_min_out),
                  obj.chn_to);
    }
    friend bool operator==(const LaunchToken&, const LaunchToken&) = default;
    bool WithParams() const { return fee != 0; }
    /** Whether it is a launch of the fee-flow release's kind (LAUNCH_V5_KIND): any of its fields other than the network's defaults. */
    bool V5() const;
    /** Whether it has a split (LAUNCH_V7_KIND). */
    bool V7() const { return !split.empty(); }
};
/** The kind of the marker of a LaunchToken with its own parameters: its fields, then `fee` (VARINT). */
inline constexpr uint8_t LAUNCH_WITH_PARAMS_KIND{36};
/**
 * The kind of the marker of a LaunchToken of the fee-flow release (from SidechainParams::fee_flow_height): its fields,
 * `fee` (VARINT; 0: the network's, the supply and decimals then the network's too), `destination`, `burn_bps` and
 * `keep_control`. One way to write a marker: the network's defaults alone (the creator, no control) are a launch of
 * the kinds before.
 */
inline constexpr uint8_t LAUNCH_V5_KIND{37};
/** Where the creator's part of a launch's fees goes (from the curve, and from its locked position once graduated). */
enum class FeeDestination : uint8_t {
    CREATOR = 0, //!< to its creator, claimed (ClaimLaunch)
    BURN = 1,    //!< bought back in its own venue each block, and burned
    SPLIT = 2,   //!< burn_bps of it bought back and burned, the rest its creator's
};
inline constexpr uint8_t MAX_FEE_DESTINATION{2};
/**
 * From fee_flow_height a launch graduates into a locked full-range position of a concentrated pool of the coin and its
 * token at this fee tier (1 %, a fixed tier: GRADUATION_FEE), and the fees that position collects are split: this part
 * (millionths) its creator's (by its FeeDestination), the rest the chain's security (as a swap fee's security part).
 */
inline constexpr uint32_t GRADUATION_FEE{10'000};
inline constexpr uint32_t GRADUATED_CREATOR_SHARE{600'000};
/**
 * From fee_flow_height (user decision 2026-10-24), a pool of the chain's coin (asset0: constant product, stable,
 * weighted, concentrated; a launch curve already did so) takes its whole fee on the coin's side (State::ClearCoinSide):
 * a buy's fee out of the coin it pays in, a sale's out of the coin it gets; of the fee, the security part (15 %) to the
 * block's security, the rest to the pool's coin for its providers (a concentrated pool: its in-range positions' fee
 * growth in the coin, a locked position's split 60/40 by CollectLocked). Pools of two assets keep the rule before.
 */

/**
 * Put part of an existing asset's supply on a bonding curve (from SidechainParams::launch_params_height), by whoever
 * spends its control coin (which goes on, listed by the marker): `amount` units of it (IsLaunchSupply), taken in from
 * the transaction's asset coins, priced in the chain's coin as a launch's (3/4 sold on the curve, 1/4 into the locked
 * pool at graduation), at `fee` (millionths, MIN_LAUNCH_FEE..MAX_LAUNCH_FEE). The asset must never have been launched
 * and have no pool of the kind AddLiquidity makes with the coin; until it graduates no new pool of it is made. The
 * rest as LaunchToken: `creator`, `min_buy`, `max_buy`, `first_buy` and `first_min_out`, `chn_to`; no allocation.
 */
struct CurveAsset {
    static constexpr uint8_t KIND{28};
    AssetId asset;
    uint64_t amount{0};
    uint32_t fee{0};
    CScript creator;
    uint64_t min_buy{0};
    uint64_t max_buy{0};
    uint64_t first_buy{0};
    uint64_t first_min_out{0};
    CScript chn_to;
    SERIALIZE_METHODS(CurveAsset, obj)
    {
        READWRITE(obj.asset, VARINT(obj.amount), VARINT(obj.fee), obj.creator, VARINT(obj.min_buy), VARINT(obj.max_buy), VARINT(obj.first_buy), VARINT(obj.first_min_out), obj.chn_to);
    }
    friend bool operator==(const CurveAsset&, const CurveAsset&) = default;
};

/**
 * Pay a launch's creator what it is owed: its part of the curve's fees (CHN, by the coinbase to its script), and, once
 * the launch graduated, its allocation (the tokens, to the one result output, whose script must be the creator's).
 * Anyone may send it: nothing goes anywhere but to the creator.
 */
struct ClaimLaunch {
    static constexpr uint8_t KIND{23};
    AssetId token;
    SERIALIZE_METHODS(ClaimLaunch, obj) { READWRITE(obj.token); }
    friend bool operator==(const ClaimLaunch&, const ClaimLaunch&) = default;
};

/**
 * Open a position of concentrated liquidity (doc/layers.md "Concentrated liquidity") in a pool of that curve:
 * `liquidity` between the prices of two ticks (multiples of the pool's tick spacing), for at most `max0` and `max1`
 * of its assets (what the liquidity needs at the pool's price, rounded up, goes in; the rest back). Results: the
 * position (a token of its own, POSITION, id PositionId(txid): transferable), then what comes back of asset0 and
 * of asset1 (asset coins; CHN by the coinbase to `chn_to`), those of nothing left out.
 */
struct OpenPosition {
    static constexpr uint8_t KIND{24};
    uint256 pool;
    int32_t tick_lower{0};
    int32_t tick_upper{0};
    uint64_t liquidity_hi{0};
    uint64_t liquidity_lo{0};
    uint64_t max0{0};
    uint64_t max1{0};
    CScript chn_to;
    SERIALIZE_METHODS(OpenPosition, obj) { READWRITE(obj.pool, obj.tick_lower, obj.tick_upper, obj.liquidity_hi, obj.liquidity_lo, VARINT(obj.max0), VARINT(obj.max1), obj.chn_to); }
    friend bool operator==(const OpenPosition&, const OpenPosition&) = default;
    unsigned __int128 Liquidity() const { return (static_cast<unsigned __int128>(liquidity_hi) << 64) | liquidity_lo; }
};

/**
 * Change a position by whoever spends its token: add liquidity (`add`, for at most `limit0`, `limit1`, the rest back),
 * or take some out (for at least `limit0`, `limit1`), and collect the fees it earned (`collect`). `close`: all its
 * liquidity out and everything owed collected, the position (its token) gone. Results: what comes back or out, of
 * asset0 then asset1, those of nothing left out; CHN by the coinbase to `chn_to`. The token, unless closed, goes on
 * one of the outputs the marker lists.
 */
struct ChangePosition {
    static constexpr uint8_t KIND{25};
    uint256 position;
    bool add{false};
    uint64_t liquidity_hi{0};
    uint64_t liquidity_lo{0};
    uint64_t limit0{0};
    uint64_t limit1{0};
    bool collect{false};
    bool close{false};
    CScript chn_to;
    SERIALIZE_METHODS(ChangePosition, obj) { READWRITE(obj.position, obj.add, obj.liquidity_hi, obj.liquidity_lo, VARINT(obj.limit0), VARINT(obj.limit1), obj.collect, obj.close, obj.chn_to); }
    friend bool operator==(const ChangePosition&, const ChangePosition&) = default;
    unsigned __int128 Liquidity() const { return (static_cast<unsigned __int128>(liquidity_hi) << 64) | liquidity_lo; }
};

/**
 * A limit order (doc/layers.md "Order books"): `amount` of `asset_sell` for `asset_buy`, at `price_num/price_den` of
 * asset_buy per asset_sell at least, until the block at `expiry` (included). It rests in the book of the pair (a venue
 * of the batch, as a pool is: Curve::BOOK) and fills, wholly or in part, at the uniform price of the book's rounds,
 * never below its limit. Result: the order, a token of its own (ORDER, id OrderId(txid)), which collects what it got
 * and cancels it (SettleOrder).
 */
struct PlaceOrder {
    static constexpr uint8_t KIND{26};
    AssetId asset_sell;
    AssetId asset_buy;
    uint64_t amount{0};
    uint64_t price_num{0};
    uint64_t price_den{0};
    int32_t expiry{0};
    SERIALIZE_METHODS(PlaceOrder, obj) { READWRITE(obj.asset_sell, obj.asset_buy, VARINT(obj.amount), VARINT(obj.price_num), VARINT(obj.price_den), obj.expiry); }
    friend bool operator==(const PlaceOrder&, const PlaceOrder&) = default;
};

/**
 * Collect what a limit order got, by whoever spends its token; `cancel` (or the order expired, or filled): what is left
 * of what it sells too, and the order is gone. Results: the order's token back (unless gone), then what it got, then
 * what is left (asset coins; CHN by the coinbase to `chn_to`), amounts of nothing left out.
 */
struct SettleOrder {
    static constexpr uint8_t KIND{27};
    uint256 order;
    bool cancel{false};
    CScript chn_to;
    SERIALIZE_METHODS(SettleOrder, obj) { READWRITE(obj.order, obj.cancel, obj.chn_to); }
    friend bool operator==(const SettleOrder&, const SettleOrder&) = default;
};

using Operation = std::variant<Reserve, Register, Mint, UpdateAsset, Burn, Swap, AddLiquidity, RemoveLiquidity, CreateAuction, Bid, Collect, ReleaseAsset,
                               Route, RegisterOracle, Attest, CreateOracleContract, SettleOracleContract, SetLogo, SetDisplayName,
                               CreatePool, PoolDeposit, PoolWithdraw, SwapPools, LaunchToken, ClaimLaunch, OpenPosition, ChangePosition, PlaceOrder, SettleOrder, CurveAsset>;

/** The registration an operation makes: a Register's, or a LaunchToken's (nullptr: none). */
const Register* RegisterOf(const Operation& op);

/** Whether an operation is one of the DEX's (valid from SidechainParams::dex_height only); its pool kinds' included. */
bool IsDexOperation(const Operation& op);
/** Whether an operation is one of the DEX's pool kinds' (valid from SidechainParams::dex_curves_height only). */
bool IsCurveOperation(const Operation& op);
/**
 * Whether a transaction may be in a block at a height where the DEX's pool kinds are `active` or not: one
 * whose marker is of their kinds is not valid before (bad-ba-marker), as no such marker was.
 */
[[nodiscard]] bool CheckCurvesActivation(const CTransaction& tx, bool active, std::string& reject_reason);
/** Whether an operation is a launch with parameters (LaunchToken of LAUNCH_WITH_PARAMS_KIND, CurveAsset). */
bool IsLaunchParamsOperation(const Operation& op);
/**
 * Whether a transaction may be in a block at a height where launches with parameters are `active` or not: one whose
 * marker is of their kinds is not valid before (bad-ba-marker), as no such marker was.
 */
[[nodiscard]] bool CheckLaunchParamsActivation(const CTransaction& tx, bool active, std::string& reject_reason);
/**
 * Whether a transaction may be in a block at a height where allocations of up to 10 % are `active`
 * (SidechainParams::allocation10_height) or not: before it, a launch whose allocation is more than
 * MAX_LAUNCH_ALLOCATION_BEFORE_10 is not valid (bad-ba-launch), as it never was.
 */
[[nodiscard]] bool CheckAllocationActivation(const CTransaction& tx, bool active, std::string& reject_reason);
/** Whether a transaction may be in a block where the fee-flow release is `active` or not: a launch of LAUNCH_V5_KIND not before (bad-ba-marker). */
[[nodiscard]] bool CheckFeeFlowActivation(const CTransaction& tx, bool active, std::string& reject_reason);
/** Before v7_height (`active` false), a launch with a split (LAUNCH_V7_KIND) is not valid. */
[[nodiscard]] bool CheckFeeSplitActivation(const CTransaction& tx, bool active, std::string& reject_reason);

/** Whether an operation sets a logo (SetLogo, or a Register with one): valid from SidechainParams::logo_height only. */
bool IsLogoOperation(const Operation& op);
/**
 * Whether a transaction may be in a block at a height where logos are `active` or not: one whose
 * marker sets a logo is not valid before (bad-ba-logo-inactive), as no such marker was.
 */
[[nodiscard]] bool CheckLogoActivation(const CTransaction& tx, bool active, std::string& reject_reason);

/** Whether an operation sets a display name (SetDisplayName, or a Register with one): valid from SidechainParams::ticker_height only. */
bool IsDisplayNameOperation(const Operation& op);
/**
 * The rules of tickers and display names, at a height where they are `active` or not. Before: a marker
 * that sets a display name is not valid (bad-ba-display-name-inactive), as no such marker was. From:
 * a registration publishes its name (bad-ba-ticker-private), and the name is a ticker (IsTicker;
 * bad-ba-ticker). Assets registered before keep their names, whatever they are.
 */
[[nodiscard]] bool CheckTickerActivation(const CTransaction& tx, bool active, std::string& reject_reason);

/** The marker output of a transaction. */
struct Marker {
    std::optional<Operation> operation;
    //! The outputs that carry tokens or take results, in increasing order.
    std::vector<MarkerOutput> outputs;
    friend bool operator==(const Marker&, const Marker&) = default;
};

CScript MarkerScript(const Marker& marker);
/** Whether the script claims to be a BitAssets marker, well formed or not. */
bool IsMarkerScript(const CScript& script);
std::optional<Marker> ParseMarkerScript(const CScript& script);
/** The marker of a transaction: none if it has none; the error if it has a bad one, or several. */
std::optional<Marker> GetMarker(const CTransaction& tx, std::string* error = nullptr);

/** The hash of the name of an asset. */
AssetId HashName(const std::string& name);
/**
 * What a reservation commits to: HMAC-SHA256 with the nonce as key, of the name hash then the script
 * of the output that carries the reservation when it is made. Bound to that script, a copy of it made
 * by anyone else, with an output of their own, commits to nothing they can reveal; one with an output
 * of the maker's script is the maker's to spend.
 */
uint256 ReservationCommitment(const AssetId& name, const uint256& nonce, const CScript& script);
/** The pool of two assets: the same whatever their order. */
uint256 PoolId(const AssetId& a, const AssetId& b);
/** Whether a name can be published: 1 to MAX_NAME_TEXT_SIZE printable ASCII characters, no space at either end. */
bool IsAssetName(const std::string& name);
/**
 * Whether a name is a ticker: MIN_TICKER_SIZE to MAX_TICKER_SIZE characters, each an uppercase letter
 * A to Z or a digit 0 to 9. From SidechainParams::ticker_height, the name of every new asset is one
 * (the look-alike rules below apply too: no ticker is "ECX" or "CHN", "0X..." or holds what reads as a number).
 */
bool IsTicker(const std::string& name);
/**
 * Whether text can be the display name of an asset: 1 to MAX_DISPLAY_NAME_SIZE bytes of well-formed
 * UTF-8 (shortest forms, no surrogates), no space at either end, each code point one of those of
 * DISPLAY_NAME_RANGES (bitassets/displayname_table.h, generated from Unicode 15.0): assigned, graphic
 * (letters, digits, punctuation, symbols) or the space U+0020, and nothing that changes how the others
 * read: no control or format character (bidirectional overrides and isolates, zero-width spaces and
 * joiners, soft hyphen...), no other space, no default-ignorable or blank character (Hangul fillers,
 * braille blank), no combining mark, variation selector, conjoining jamo or character that composes
 * with the one before, and none that NFC changes. Text of these is in NFC, and is shown as it is. The
 * set is frozen: a later version of Unicode adds nothing to it (a soft fork could).
 */
bool IsDisplayName(const std::string& text);
/** Why text cannot be a display name (for those who tell the user); nullopt if it can. */
std::optional<std::string> WhyNotDisplayName(const std::string& text);

/**
 * Whether a name reads as another asset where assets are named (rpcutil's ParseAssetArg): CHN in any
 * case, a number ("1739-0029", "0001-1739-0029") or "0x" and anything. No asset is registered under
 * one (a private name is checked as far as it can be: CHN).
 */
bool ReadsAsAnotherAsset(const std::string& name);
/**
 * Whether a name holds what reads as the number of an asset where names are shown with their numbers
 * ("GOLD (#1739-0029)"): '#', '(' or ')', or four digits, a dash and four digits anywhere. Policy, not
 * consensus: the wallet refuses to reserve or publish one, and the mempool does not relay its
 * registration (ba-name-look-alike); a block may still hold one (a private name published by a block
 * producer), which every page shows number first ("#5555-1234 · \"GOLD (#1739-0029)\"").
 */
bool NameMimicsNumber(const std::string& name);
/** Whether text is shaped as a number ("1739-0029", "0000-1739-0029"): groups of four digits, two or three, with dashes. */
bool ShapedAsNumber(const std::string& text);

/** The number of an asset in the order of registration, shown as BitNames shows its numbers. */
std::string FormatSeq(uint32_t seq);
std::optional<uint32_t> ParseSeq(const std::string& text);

//
// The state.
//

/** A value of a field of an asset, with where it was set. */
template <typename T>
struct Stamped {
    std::optional<T> value;
    Txid txid;
    int32_t height{0};
    SERIALIZE_METHODS(Stamped, obj) { READWRITE(BA_OPT(obj.value), obj.txid, obj.height); }
    friend bool operator==(const Stamped&, const Stamped&) = default;
};

/** The fields of the data of an asset, in the order of AssetData: their numbers in the keys of the history. */
enum class DataField : uint8_t { COMMITMENT = 0, IPV4 = 1, IPV6 = 2, ENCRYPTION_KEY = 3, SIGNING_KEY = 4, INFO = 5 };
inline constexpr size_t DATA_FIELDS{6};
static_assert(DATA_FIELDS == 6, "AssetRecord serializes each count");

/**
 * An asset as it is now. Every value each field of its data has had is kept apart, one entry per
 * value (State::GetHistory), so that a change to it reads and writes a record of fixed size.
 */
/** Where a reservation was made: the script of its output then, and the height. */
struct ReservationOrigin {
    CScript script;
    int32_t height{0};
    SERIALIZE_METHODS(ReservationOrigin, obj) { READWRITE(obj.script, obj.height); }
    friend bool operator==(const ReservationOrigin&, const ReservationOrigin&) = default;
};

struct AssetRecord {
    uint32_t seq{0};
    Txid registration;
    int32_t height{0};
    //! The name, if its registration made it public.
    std::string text;
    uint8_t decimals{0};
    //! Units there are: all minted, less all burned.
    uint64_t supply{0};
    uint64_t minted{0};
    uint64_t burned{0};
    //! Whether the control coin was burned: no more can be minted, the data cannot change.
    bool fixed{false};
    //! Its data now.
    AssetData data;
    //! How many values each field has had (its registration's included): where its history ends.
    std::array<uint32_t, DATA_FIELDS> changes{};
    //! Auctions not collected that hold some of it: that sell it, or that took it in (bids paid it).
    //! Kept as auctions change (State::SetAuction), so that whether it can be retired is one look.
    uint32_t holding_auctions{0};
    //! Its pools that someone provides liquidity to (not amm::Abandoned), kept as pools change.
    uint32_t provided_pools{0};

    SERIALIZE_METHODS(AssetRecord, obj)
    {
        READWRITE(obj.seq, obj.registration, obj.height, obj.text, obj.decimals, obj.supply, obj.minted, obj.burned, obj.fixed,
                  obj.data, obj.changes[0], obj.changes[1], obj.changes[2], obj.changes[3], obj.changes[4], obj.changes[5],
                  obj.holding_auctions, obj.provided_pools);
    }
    friend bool operator==(const AssetRecord&, const AssetRecord&) = default;

    const AssetData& Current() const { return data; }
};

/** The logo of an asset as the state records it (its bytes are kept apart: State::GetLogoBytes). */
struct LogoRecord {
    //! SHA-256 of the bytes.
    uint256 hash;
    //! LogoFormat.
    uint8_t format{0};
    uint32_t size{0};
    int32_t height{0};
    Txid txid;
    SERIALIZE_METHODS(LogoRecord, obj) { READWRITE(obj.hash, obj.format, obj.size, obj.height, obj.txid); }
    friend bool operator==(const LogoRecord&, const LogoRecord&) = default;
};

/** The display name of an asset as the state records it, with where it was set. */
struct DisplayNameRecord {
    std::string text;
    int32_t height{0};
    Txid txid;
    SERIALIZE_METHODS(DisplayNameRecord, obj) { READWRITE(obj.text, obj.height, obj.txid); }
    friend bool operator==(const DisplayNameRecord&, const DisplayNameRecord&) = default;
};

/** Every value each field of the data of an asset has had, oldest first, with where it was set. */
struct AssetHistory {
    std::vector<Stamped<uint256>> commitment;
    std::vector<Stamped<SocketV4>> ipv4;
    std::vector<Stamped<SocketV6>> ipv6;
    std::vector<Stamped<EncryptionKey>> encryption_key;
    std::vector<Stamped<SigningKey>> signing_key;
    std::vector<Stamped<std::string>> info;
    friend bool operator==(const AssetHistory&, const AssetHistory&) = default;

    /** The data at the end of the block at `height`; nullopt before `registered`, the height of the registration. */
    std::optional<AssetData> AtHeight(int registered, int height) const;
};

/**
 * Launches (LaunchToken, CurveAsset; doc/dex-math.md "Launch curve"). A launch's supply S (units on the curve and in
 * its pool: LAUNCH_SUPPLY, a billion of LAUNCH_DECIMALS, for a launch of the kind LaunchToken::KIND; its own from
 * launch_params_height, IsLaunchSupply) splits into S·3/4 sold on its curve and S/4 that go with the raised coin into
 * its pool at graduation. The curve is x·y = k on virtual reserves from (V, Tv) = (G/2, S·9/8) to its end
 * (V + G, yE) = (3G/2, S·3/8), G = SidechainParams::launch_graduation: selling the 3/4 raises exactly G, and the pool
 * at graduation, (G, S/4), has the curve's last price, (V + G)²/k = 4G/S. In coin, the curve is the same for any S:
 * it starts at a value of 4G/9 for the whole supply and ends at 4G.
 */
inline constexpr uint8_t LAUNCH_DECIMALS{6};
inline constexpr uint64_t LAUNCH_SUPPLY{1'000'000'000'000'000};
inline constexpr uint64_t LAUNCH_CURVE_SUPPLY{LAUNCH_SUPPLY / 4 * 3};
inline constexpr uint64_t LAUNCH_POOL_SUPPLY{LAUNCH_SUPPLY / 4};
inline constexpr uint64_t LAUNCH_VIRTUAL_TOKENS{LAUNCH_SUPPLY / 8 * 9};
inline constexpr uint64_t LAUNCH_CURVE_END{LAUNCH_SUPPLY / 8 * 3};
static_assert(LAUNCH_VIRTUAL_TOKENS - LAUNCH_CURVE_END == LAUNCH_CURVE_SUPPLY && LAUNCH_CURVE_SUPPLY + LAUNCH_POOL_SUPPLY == LAUNCH_SUPPLY);
/** A launch's supply with parameters: a multiple of LAUNCH_SUPPLY_MULTIPLE (its parts exact), at least MIN_LAUNCH_SUPPLY, at most MAX_LAUNCH_SUPPLY (the virtual Tv = S·9/8 within MAX_AMOUNT). */
inline constexpr uint64_t LAUNCH_SUPPLY_MULTIPLE{8};
inline constexpr uint64_t MIN_LAUNCH_SUPPLY{1'000'000};
inline constexpr uint64_t MAX_LAUNCH_SUPPLY{MAX_AMOUNT / 9 * 8};
static_assert(MAX_LAUNCH_SUPPLY % LAUNCH_SUPPLY_MULTIPLE == 0 && MAX_LAUNCH_SUPPLY / 8 * 9 <= MAX_AMOUNT && LAUNCH_SUPPLY % LAUNCH_SUPPLY_MULTIPLE == 0);
/** Whether `supply` units can be a launch's (with parameters). */
constexpr bool IsLaunchSupply(uint64_t supply) { return supply % LAUNCH_SUPPLY_MULTIPLE == 0 && supply >= MIN_LAUNCH_SUPPLY && supply <= MAX_LAUNCH_SUPPLY; }
/** The fee of a launch's curve with parameters, in millionths: 0.3 % to 5 %. */
inline constexpr uint32_t MIN_LAUNCH_FEE{3'000};
inline constexpr uint32_t MAX_LAUNCH_FEE{50'000};
/**
 * Most a creator's allocation can be, in basis points of the supply, minted at graduation (shown as such): 10 % from
 * SidechainParams::allocation10_height, 5 % (MAX_LAUNCH_ALLOCATION_BEFORE_10) before it. A launch's supply and its
 * allocation together are within MAX_AMOUNT (IsLaunchAllocation; at 10 % every launch supply is, MAX_LAUNCH_SUPPLY too:
 * a bound kept should the most ever be raised). TODO(rug protection): the allocation is the creator's at graduation in one claim; a vesting or
 * lock of it is to come.
 */
inline constexpr uint16_t MAX_LAUNCH_ALLOCATION{1'000};
inline constexpr uint16_t MAX_LAUNCH_ALLOCATION_BEFORE_10{500};
/** The most a launch's allocation can be at a height where SidechainParams::Allocation10Active is `active` or not. */
constexpr uint16_t MaxLaunchAllocation(bool allocation10_active) { return allocation10_active ? MAX_LAUNCH_ALLOCATION : MAX_LAUNCH_ALLOCATION_BEFORE_10; }
/** The units an allocation of `allocation` basis points of `supply` mints: the floor of its share. */
constexpr uint64_t LaunchAllocationUnits(uint64_t supply, uint16_t allocation) { return static_cast<uint64_t>(static_cast<unsigned __int128>(supply) * allocation / 10'000); }
/** Whether a launch of `supply` units can have an allocation of `allocation` basis points: at most MAX_LAUNCH_ALLOCATION, the two within MAX_AMOUNT. */
constexpr bool IsLaunchAllocation(uint64_t supply, uint16_t allocation)
{
    return allocation <= MAX_LAUNCH_ALLOCATION && static_cast<unsigned __int128>(supply) + LaunchAllocationUnits(supply, allocation) <= MAX_AMOUNT;
}
/** The blocks the launches' log keeps (State::GetLaunchLog): about a day and a half of 15-second blocks. */
inline constexpr int32_t LAUNCH_LOG_BLOCKS{10'080};

/** A launch (LaunchToken), by its token. */
struct LaunchRecord {
    Txid txid;
    int32_t height{0};
    CScript creator;
    uint16_t allocation{0};
    uint64_t min_buy{0};
    uint64_t max_buy{0};
    //! The rules it was made under (SidechainParams then): the fee (millionths), the creator's part of it
    //! (millionths of the fee), what it graduates at, and the curve's virtual coin (half of that).
    uint32_t fee{0};
    uint32_t creator_share{0};
    //! The protocol's and the chain's security parts of its fee (millionths of it), as made.
    uint32_t protocol_share{0};
    uint32_t security_share{0};
    uint64_t graduation{0};
    uint64_t virtual_coin{0};
    //! Its curve's pool (PoolIdOf of the coin, the token and the launch's kind) until it graduates; then the token's pool with the coin.
    uint256 pool;
    //! The fees for the pool it graduates into, and the creator's not paid yet (the chain's coin), and the creator's paid.
    uint64_t pool_fees{0};
    uint64_t creator_fees{0};
    //! The protocol's (to the treasury) and the chain's security parts of its fees, in all.
    uint64_t protocol_fees{0};
    uint64_t security_fees{0};
    uint64_t creator_paid{0};
    //! -1 until it graduates; then the block, and the coin it had raised.
    int32_t graduated{-1};
    uint64_t raised_at_graduation{0};
    bool allocation_claimed{false};
    //! The coin traded in and out on its curve, ever, and its rounds.
    uint64_t volume{0};
    uint32_t trades{0};
    //! Its supply S on the curve and in its pool (units): 0 for one of LAUNCH_SUPPLY (a launch of LaunchToken::KIND,
    //! and every record written before launch_params_height, which ends before this field); and whether it put an
    //! existing asset on a curve (CurveAsset).
    uint64_t supply{0};
    bool from_asset{false};
    //! From fee_flow_height (`v5`; records before end before it): where its creator's part of the fees goes, the burn's
    //! part of a split, the blocks its allocation vests over from graduation (0: at once), whether its creator kept the
    //! control coin; its allocation paid so far, its tokens bought back and burned, the coin waiting to buy them back,
    //! and the locked position it graduated into (null: none, a pool of before).
    bool v5{false};
    uint8_t destination{0};
    uint16_t burn_bps{0};
    int32_t vesting{0};
    bool keep_control{false};
    uint64_t allocation_paid{0};
    uint64_t burned{0};
    uint64_t buyback{0};
    uint256 position;
    //! The creator's part of its locked position's fees in its own token, not paid yet (claimlaunch pays it).
    uint64_t creator_tokens{0};
    //! From v7_height (`v7`; records before end before it; v7 implies v5): the split of its fees' tunable part (its
    //! launch's, or its v5 destination's: SplitOfDestination), and by share the coin it was given in all and, of an
    //! address, what it is owed and not paid yet (so paid: given less owed; the creator's paid is creator_paid).
    bool v7{false};
    std::vector<FeeShare> split;
    std::vector<uint64_t> share_given;
    std::vector<uint64_t> share_owed;
    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << txid << height << creator << allocation << min_buy << max_buy << fee << creator_share << graduation << virtual_coin << pool << pool_fees << creator_fees
          << creator_paid << graduated << raised_at_graduation << allocation_claimed << volume << trades << protocol_share << security_share << protocol_fees << security_fees;
        // A launch of before (supply 0, not from an asset) is written as it was.
        if (supply != 0 || from_asset || v5) s << VARINT(supply) << from_asset;
        if (v5) s << destination << burn_bps << VARINT(static_cast<uint32_t>(vesting)) << keep_control << VARINT(allocation_paid) << VARINT(burned) << VARINT(buyback) << position << VARINT(creator_tokens);
        if (v7) s << split << share_given << share_owed;
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        s >> txid >> height >> creator >> allocation >> min_buy >> max_buy >> fee >> creator_share >> graduation >> virtual_coin >> pool >> pool_fees >> creator_fees >>
            creator_paid >> graduated >> raised_at_graduation >> allocation_claimed >> volume >> trades >> protocol_share >> security_share >> protocol_fees >> security_fees;
        supply = 0;
        from_asset = false;
        v5 = false;
        if (!s.empty()) s >> VARINT(supply) >> from_asset;
        if (!s.empty()) {
            uint32_t blocks{0};
            s >> destination >> burn_bps >> VARINT(blocks) >> keep_control >> VARINT(allocation_paid) >> VARINT(burned) >> VARINT(buyback) >> position >> VARINT(creator_tokens);
            vesting = static_cast<int32_t>(blocks);
            v5 = true;
        }
        v7 = false;
        split.clear();
        share_given.clear();
        share_owed.clear();
        if (!s.empty()) {
            s >> split >> share_given >> share_owed;
            v7 = true;
        }
    }
    /** The part of its allocation vested at `height` (units): all of it at graduation without vesting (or before v5), else linearly over `vesting` blocks from it. */
    uint64_t AllocationVested(int height) const
    {
        if (!Graduated() || allocation == 0) return 0;
        const uint64_t all{AllocationUnits()};
        if (!v5 || vesting <= 0) return all;
        const int64_t since{std::max<int64_t>(0, int64_t{height} - graduated)};
        if (since >= vesting) return all;
        return static_cast<uint64_t>(static_cast<unsigned __int128>(all) * static_cast<uint64_t>(since) / static_cast<uint64_t>(vesting));
    }
    friend bool operator==(const LaunchRecord&, const LaunchRecord&) = default;
    bool Graduated() const { return graduated >= 0; }
    /** Its supply S (units), and its parts: on the curve (3S/4), into the pool at graduation (S/4); the curve's virtual units (9S/8) and end (3S/8). */
    uint64_t Supply() const { return supply == 0 ? LAUNCH_SUPPLY : supply; }
    /** Whether its curve ends only at its end exactly (curves::LaunchCurve::strict_end): those made from launch_params_height, which write their supply. */
    bool StrictEnd() const { return supply != 0; }
    /** The bound of its curve's units (curves::LaunchCurve::start): its virtual units, for those made from launch_params_height. */
    uint64_t StartBound() const { return supply != 0 ? VirtualUnits() : 0; }
    uint64_t CurveSupply() const { return Supply() / 8 * 6; }
    uint64_t PoolSupply() const { return Supply() / 8 * 2; }
    uint64_t VirtualUnits() const { return Supply() / 8 * 9; }
    uint64_t CurveEnd() const { return Supply() / 8 * 3; }
    /** Its allocation in units: ⌊S·allocation/10000⌋ (exact for LAUNCH_SUPPLY). */
    uint64_t AllocationUnits() const { return LaunchAllocationUnits(Supply(), allocation); }
};

/** What the pages show of a launch's split (getlaunch, the public API): v5's destination mapped onto one before v7. */
struct FeeShareView {
    const char* kind{"creator"}; //!< "creator", "burn" or "address"
    CScript script;              //!< "address" only
    uint16_t bps{0};
    uint64_t paid{0};            //!< creator: claimed; burn: bought back (or on its way); address: paid by the coinbase
    uint64_t owed{0};            //!< creator: to claim; burn: waiting to buy back; address: owed, paid once MIN_FEE_SHARE_PAYOUT
};
/** None for a launch before the fee-flow release (v5). */
std::vector<FeeShareView> FeeSplitView(const LaunchRecord& record);

/** What a block did on a launch's curve (State::GetLaunchLog): the coin and tokens in and out, the fees, the price after, whether it graduated. */
struct LaunchLogEntry {
    uint64_t coin_in{0};
    uint64_t coin_out{0};
    uint64_t tokens_in{0};
    uint64_t tokens_out{0};
    uint64_t fees{0};
    uint32_t orders{0};
    //! The curve's coin and tokens after the block (virtual), or the pool's at graduation.
    uint64_t reserve_coin{0};
    uint64_t reserve_tokens{0};
    bool created{false};
    bool graduated{false};
    SERIALIZE_METHODS(LaunchLogEntry, obj) { READWRITE(obj.coin_in, obj.coin_out, obj.tokens_in, obj.tokens_out, obj.fees, obj.orders, obj.reserve_coin, obj.reserve_tokens, obj.created, obj.graduated); }
    friend bool operator==(const LaunchLogEntry&, const LaunchLogEntry&) = default;
};

/** A concentrated pool's price, tick, liquidity in range and fees per unit of liquidity (Q128), ever (Uniswap v3's slot0, liquidity, feeGrowthGlobal). */
struct ClState {
    uint256 sqrt_price;
    int32_t tick{0};
    uint64_t liquidity_hi{0};
    uint64_t liquidity_lo{0};
    uint256 fee_growth0;
    uint256 fee_growth1;
    SERIALIZE_METHODS(ClState, obj) { READWRITE(obj.sqrt_price, obj.tick, obj.liquidity_hi, obj.liquidity_lo, obj.fee_growth0, obj.fee_growth1); }
    friend bool operator==(const ClState&, const ClState&) = default;
    unsigned __int128 Liquidity() const { return (static_cast<unsigned __int128>(liquidity_hi) << 64) | liquidity_lo; }
    void SetLiquidity(unsigned __int128 l)
    {
        liquidity_hi = static_cast<uint64_t>(l >> 64);
        liquidity_lo = static_cast<uint64_t>(l);
    }
};

/** A tick that a position starts or ends at (Uniswap v3's Tick.Info): its liquidity in all, what crossing it changes, the fees on its other side. */
struct ClTick {
    uint64_t gross_hi{0};
    uint64_t gross_lo{0};
    //! The liquidity that comes in crossing it upward (out downward): |net|, and its sign.
    uint64_t net_hi{0};
    uint64_t net_lo{0};
    bool net_negative{false};
    uint256 outside0;
    uint256 outside1;
    SERIALIZE_METHODS(ClTick, obj) { READWRITE(obj.gross_hi, obj.gross_lo, obj.net_hi, obj.net_lo, obj.net_negative, obj.outside0, obj.outside1); }
    friend bool operator==(const ClTick&, const ClTick&) = default;
    unsigned __int128 Gross() const { return (static_cast<unsigned __int128>(gross_hi) << 64) | gross_lo; }
    unsigned __int128 NetAbs() const { return (static_cast<unsigned __int128>(net_hi) << 64) | net_lo; }
};

/** A position of concentrated liquidity (OpenPosition), by its id. */
struct ClPosition {
    uint256 pool;
    int32_t tick_lower{0};
    int32_t tick_upper{0};
    uint64_t liquidity_hi{0};
    uint64_t liquidity_lo{0};
    uint256 inside0_last;
    uint256 inside1_last;
    uint64_t owed0{0};
    uint64_t owed1{0};
    Txid txid;
    int32_t height{0};
    SERIALIZE_METHODS(ClPosition, obj) { READWRITE(obj.pool, obj.tick_lower, obj.tick_upper, obj.liquidity_hi, obj.liquidity_lo, obj.inside0_last, obj.inside1_last, obj.owed0, obj.owed1, obj.txid, obj.height); }
    friend bool operator==(const ClPosition&, const ClPosition&) = default;
    unsigned __int128 Liquidity() const { return (static_cast<unsigned __int128>(liquidity_hi) << 64) | liquidity_lo; }
};

/** The id of the position OpenPosition `txid` makes. */
uint256 PositionId(const Txid& txid);

/** A limit order resting in a book (PlaceOrder), by its id. Its side: it sells the book's asset0 (an ask) or asset1 (a bid). */
struct BookOrder {
    uint256 book;
    bool sells0{false};
    uint64_t amount{0};
    uint64_t remaining{0};
    //! Its limit: price_num/price_den of what it buys per unit of what it sells, at least.
    uint64_t price_num{0};
    uint64_t price_den{0};
    int32_t expiry{0};
    //! What it got and not collected yet, and in all.
    uint64_t proceeds{0};
    uint64_t got{0};
    Txid txid;
    int32_t height{0};
    //! The block in which a better and no smaller order took its place in a full side (0: never); it trades no more.
    int32_t evicted{0};
    template <typename Stream>
    void Serialize(Stream& s) const { s << book << sells0 << amount << remaining << price_num << price_den << expiry << proceeds << got << txid << height << evicted; }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        s >> book >> sells0 >> amount >> remaining >> price_num >> price_den >> expiry >> proceeds >> got >> txid >> height;
        // A record written before `evicted` (none on any network: a development build) reads as never evicted.
        evicted = 0;
        if (!s.empty()) s >> evicted;
    }
    friend bool operator==(const BookOrder&, const BookOrder&) = default;
};

/** The id of the order PlaceOrder `txid` makes; the id of the book of two assets (a pool record of Curve::BOOK). */
uint256 OrderId(const Txid& txid);
uint256 BookId(const AssetId& a, const AssetId& b);
/** Most open orders a side of a book holds, and most a round considers (the best). */
inline constexpr uint32_t MAX_BOOK_SIDE{1000};
inline constexpr size_t MAX_BOOK_ROUND{256};
/** Latest an order may expire, in blocks after the one that places it: about a year of 15-second blocks. */
inline constexpr int32_t MAX_ORDER_DURATION{2'102'400};
/** Index entries a look at a side reads at most (live orders and expired ones not swept yet). */
inline constexpr size_t MAX_BOOK_SCAN{8192};
/**
 * Expired orders a block's end takes out of their books' indexes at most, oldest first (the rest at the next blocks'
 * ends). A side never holds more than MAX_BOOK_SIDE entries, live or not: a placement on a full side first takes that
 * side's expired entries out itself.
 */
inline constexpr size_t MAX_BOOK_SWEEP{5000};
/**
 * The least an order of a pair with CHN is worth, in CHN (satoshis), on its CHN side, MinBookChn(height): 0.01 ECX before
 * SidechainParams::small_payout_height, the least payout (0.00005 ECX: about $0.50 at $10 000 an ECX) from it. A side
 * still holds MAX_BOOK_SIDE orders at most, a better and larger order evicting the worst: small orders cannot crowd a
 * book out, each holds what it sells until it is filled, settled or evicted.
 */
inline constexpr uint64_t MIN_BOOK_CHN_BEFORE_SMALL{1'000'000};
uint64_t MinBookChn(int height);

/**
 * The volatility of a pool with a dynamic fee (POOL_FLAG_DYNAMIC_FEE): the last block's clearing price (UQ64.64) and a
 * moving average of the change of the price from one block that traded to the next, in millionths (dex::PoolFee).
 */
struct FeeState {
    uint256 last_price;
    int32_t last_height{0};
    uint64_t volatility{0};
    SERIALIZE_METHODS(FeeState, obj) { READWRITE(obj.last_price, obj.last_height, obj.volatility); }
    friend bool operator==(const FeeState&, const FeeState&) = default;
};

/** A pool of two assets, `asset0` < `asset1`. */
struct Pool {
    AssetId asset0;
    AssetId asset1;
    uint64_t reserve0{0};
    uint64_t reserve1{0};
    //! Shares there are, MIN_LIQUIDITY of which nobody holds.
    uint64_t shares{0};
    Txid creation;
    int32_t height{0};
    //! What was traded in, of each asset, ever.
    uint64_t volume0{0};
    uint64_t volume1{0};
    uint32_t swaps{0};

    SERIALIZE_METHODS(Pool, obj) { READWRITE(obj.asset0, obj.asset1, obj.reserve0, obj.reserve1, obj.shares, obj.creation, obj.height, obj.volume0, obj.volume1, obj.swaps); }
    friend bool operator==(const Pool&, const Pool&) = default;
};

/** The curve of a pool (bitassets/curves.h). */
enum class Curve : uint8_t {
    CONSTANT_PRODUCT = 0, //!< x·y = k (Uniswap v2), the curve of every pool AddLiquidity makes
    STABLE = 1,           //!< Curve's StableSwap invariant, amplification A
    WEIGHTED = 2,         //!< x^w0 · y^w1 = k (Balancer), two weights
    LAUNCH = 3,           //!< a launch's bonding curve (LaunchToken): constant product of virtual reserves; made by a launch only
    CONCENTRATED = 4,     //!< concentrated liquidity (Uniswap v3): positions over price ranges, ticks
    BOOK = 5,             //!< the order book of a pair: limit orders (PlaceOrder), made by the first order
};
/** The curves CreatePool makes (LAUNCH is made by launches only). */
inline constexpr bool CreatableCurve(uint8_t curve) { return curve <= static_cast<uint8_t>(Curve::WEIGHTED) || curve == static_cast<uint8_t>(Curve::CONCENTRATED); }
/** PoolParams::flags: the fee rises with the pool's recent volatility (dex::PoolFee), within bounds. */
inline constexpr uint8_t POOL_FLAG_DYNAMIC_FEE{1};
inline constexpr uint8_t MAX_CURVE{static_cast<uint8_t>(Curve::CONCENTRATED)};
/** Fees are in millionths of what goes in. */
inline constexpr uint32_t FEE_DENOMINATOR{1'000'000};
/** The fee tiers a pool is made with: 0.01 %, 0.05 %, 0.30 %, 1.00 %. */
inline constexpr std::array<uint32_t, 4> FEE_TIERS{100, 500, 3'000, 10'000};
/** The fee of every pool AddLiquidity makes (SWAP_FEE_PER_MILLE): 0.30 %. */
inline constexpr uint32_t LEGACY_FEE{SWAP_FEE_PER_MILLE * 1000};
/** StableSwap's amplification: from 1 (nearly constant product) to 10000 (nearly constant sum). */
inline constexpr uint32_t MIN_AMP{1};
inline constexpr uint32_t MAX_AMP{10'000};
/** The weights (percent of one asset; the other has the rest) a weighted pool is made with; 50/50 is the constant product. */
inline constexpr std::array<uint8_t, 12> WEIGHTS{2, 5, 10, 20, 25, 40, 60, 75, 80, 90, 95, 98};
/**
 * The treasury (doc/layers.md "Fees of the DEX"): what the protocol's parts of the DEX's fees and of the launches' fees
 * accrued, per asset (the chain's coin: the null id), held by the chain itself: no key, no output. Also the security
 * parts of swap fees paid in an asset (not the chain's coin), which no coinbase can pay: held per asset until a rule
 * says what becomes of them. No rule ever spent from it. From SidechainParams::no_treasury_height there is no
 * treasury: nothing accrues (the protocol's parts go to the providers and the creators, an asset's security part stays
 * in its pool), its chain's coin was paid with that block's fees, and its entries stay as a record; assets held stay held.
 */
struct TreasuryEntry {
    //! The protocol's: what it holds of the asset, and what came in, from swaps and from launches.
    uint64_t balance{0};
    uint64_t from_swaps{0};
    uint64_t from_launches{0};
    //! The security parts of swap fees in this asset (never the chain's coin: those are paid with the block's fees).
    uint64_t security_held{0};
    SERIALIZE_METHODS(TreasuryEntry, obj) { READWRITE(obj.balance, obj.from_swaps, obj.from_launches, obj.security_held); }
    friend bool operator==(const TreasuryEntry&, const TreasuryEntry&) = default;
};
/** The security parts of the fees a block paid in the chain's coin (to the BMM bidder and the validators), and in all. */
struct SecurityFees {
    int32_t height{-1};
    uint64_t amount{0};
    uint64_t total{0};
    SERIALIZE_METHODS(SecurityFees, obj) { READWRITE(obj.height, obj.amount, obj.total); }
    friend bool operator==(const SecurityFees&, const SecurityFees&) = default;
};
/** The parts of a fee (millionths of it, each rounded down); the rest, the remainder, is the liquidity providers' (or the curve's pool's). */
struct FeeParts {
    uint64_t security{0};
    uint64_t protocol{0};
    uint64_t creator{0};
};
FeeParts SwapFeeParts(uint64_t fee, uint32_t security_share, uint32_t protocol_share);
FeeParts LaunchFeeParts(uint64_t fee, uint32_t creator_share, uint32_t protocol_share, uint32_t security_share);

/**
 * What kind of pool a pool is. A pool with none recorded (State::GetPoolParams gives the default) is a
 * constant-product pool at 0.30 %, as every pool AddLiquidity made was and is.
 */
struct PoolParams {
    uint8_t curve{0};
    uint32_t fee{LEGACY_FEE};
    uint32_t amp{0};
    //! WEIGHTED: the weight of asset0 in percent.
    uint8_t weight0{0};
    uint8_t flags{0};
    //! STABLE: the units of each asset count 10^scale of the smallest unit of the two (decimals told
    //! apart): set when the pool is made, from the assets' decimals, which never change.
    uint8_t scale0{0};
    uint8_t scale1{0};
    SERIALIZE_METHODS(PoolParams, obj) { READWRITE(obj.curve, obj.fee, obj.amp, obj.weight0, obj.flags, obj.scale0, obj.scale1); }
    friend bool operator==(const PoolParams&, const PoolParams&) = default;
    Curve GetCurve() const { return static_cast<Curve>(curve); }
    /** Whether this is the kind of every pool AddLiquidity makes (its id is PoolId). */
    bool Legacy() const { return curve == static_cast<uint8_t>(Curve::CONSTANT_PRODUCT) && fee == LEGACY_FEE && flags == 0; }
};

/**
 * The id of the pool of two assets of a kind: PoolId for a constant-product pool at 0.30 %; otherwise the
 * tagged hash of the assets (in order) and what the kind is made of (curve, fee, amplification, weight
 * of asset0, flags; not the scales, which follow from the assets).
 */
uint256 PoolIdOf(const AssetId& a, const AssetId& b, const PoolParams& params);
/** The kind of pool a launch made from fee_flow_height graduates into: concentrated, at GRADUATION_FEE (its id: PoolIdOf(CHN, token, it)). */
PoolParams GraduationParams();
/** Why the parameters of a CreatePool are not those of a pool (nullopt: they are); the weight is asset_a's. */
std::optional<std::string> WhyNotPoolKind(uint8_t curve, uint32_t fee, uint32_t amp, uint8_t weight_a, uint8_t flags);
/** The parameters of a pool of `a` and `b` made by `create` (the weight turned to asset0's); the scales from the decimals. */
PoolParams MakePoolParams(const CreatePool& create, uint8_t decimals0, uint8_t decimals1);

struct Auction {
    AssetId base;
    uint64_t base_amount{0};
    AssetId quote;
    uint64_t start_price{0};
    uint64_t end_price{0};
    int32_t start_height{0};
    int32_t duration{0};
    int32_t created{0};
    //! What is left to sell, and what bids paid in.
    uint64_t remaining{0};
    uint64_t proceeds{0};
    uint32_t bids{0};
    //! Collected: what was left and what came in went to the holder of the receipt.
    bool closed{false};
    //! The registrations of its two assets when it was made (null for CHN): an asset retired and
    //! registered again under its name is another asset, which the auction does not trade.
    Txid base_registration;
    Txid quote_registration;

    SERIALIZE_METHODS(Auction, obj)
    {
        READWRITE(obj.base, obj.base_amount, obj.quote, obj.start_price, obj.end_price, obj.start_height, obj.duration, obj.created,
                  obj.remaining, obj.proceeds, obj.bids, obj.closed, obj.base_registration, obj.quote_registration);
    }
    friend bool operator==(const Auction&, const Auction&) = default;

    //! In 64 bits: a start height near the top of 32 bits plus the duration would overflow.
    int64_t EndHeight() const { return int64_t{start_height} + duration - 1; }
    /** The price of all of it (base_amount) in the block at `height`, between the start and the end. */
    uint64_t PriceAt(int height) const;
    /** What a bid of `quote_amount` buys in the block at `height` (more than is left, if it is too much). */
    uint64_t BuysAt(int height, uint64_t quote_amount) const;
    /**
     * The least a bid in the block at `height` pays to buy all that is left: ceil(remaining * price
     * / base_amount), the smallest amount whose BuysAt is at least what is left. A bid of it may buy
     * more than is left (when one unit of the quote buys more than one of the base); what a bid that
     * buys more than is left pays, when it gets the rest back (Bid).
     */
    uint64_t CostOfRemaining(int height) const;
    /** Whether bids are taken in the block at `height`. */
    bool OpenAt(int height) const { return !closed && remaining > 0 && height >= start_height && height <= EndHeight(); }
    /** Whether the receipt can close it in the block at `height`. */
    bool CollectableAt(int height) const { return !closed && (height > EndHeight() || remaining == 0 || bids == 0); }
    /**
     * Whether it holds some of an asset, which keeps the asset from being retired: not collected, and
     * selling it, or having taken some of it in. One that sells something else for it and took none
     * of it in holds none.
     */
    bool Holds(const AssetId& asset) const { return !closed && (base == asset || (quote == asset && proceeds > 0)); }
};

/** Pool arithmetic, as the rules do it. */
namespace amm {
/** What `amount_in` buys from reserves `reserve_in` and `reserve_out`; 0 if nothing. */
uint64_t SwapOut(uint64_t reserve_in, uint64_t reserve_out, uint64_t amount_in);
/** What has to go in to get `amount_out` out; nullopt if the pool does not hold that much. */
std::optional<uint64_t> SwapIn(uint64_t reserve_in, uint64_t reserve_out, uint64_t amount_out);
/** What a deposit does (Provide). */
struct Deposit {
    //! The shares it gives (for a pool it opens, MIN_LIQUIDITY more are kept for good).
    uint64_t shares{0};
    //! What goes into the pool of each asset; the rest of what the deposit offers goes back.
    uint64_t take0{0};
    uint64_t take1{0};
    //! Whether it opens the pool: a new one, or one nobody provides liquidity to any more (Abandoned).
    bool opens{false};
};
/**
 * A deposit of at most `amount0` and `amount1` into a pool; nullopt
 * if it gives no shares. Into a pool someone provides liquidity to, it gives the shares of the side
 * that gives fewer, which goes in whole; of the other side, what those shares are worth, rounded up
 * (the pool never loses), unless what would go back is CHN under `min_chn` (MinChnPayout; it goes in too). A
 * pool nobody provides liquidity to (a new one, or one everyone left) is opened as a new pool is
 * made, with what it holds merged in: isqrt of the product of its reserves after, MIN_LIQUIDITY of
 * them kept for good; both amounts go in. Its price is then the deposit's, its dust aside: nobody's
 * deposit can be taken in at the price of the dust, and a deposit that comes after another opened
 * the pool goes in at that one's price, its excess back, or is refused by its least shares.
 */
std::optional<Deposit> Provide(const Pool& pool, uint64_t amount0, uint64_t amount1, uint64_t min_chn);
/** What `shares` are worth of a pool, rounded down. */
std::pair<uint64_t, uint64_t> Withdraw(const Pool& pool, uint64_t shares);
/** What `shares` take out of a pool: CHN below `min_chn` (MinChnPayout) is not taken out, it stays in the pool. */
std::pair<uint64_t, uint64_t> WithdrawPaid(const Pool& pool, uint64_t shares, uint64_t min_chn);
/**
 * How much worse, in percent, a trade's price is than the pool's price before it (the fee
 * included). Not a rule: what wallets warn about.
 */
double PriceImpact(uint64_t reserve_in, uint64_t reserve_out, uint64_t amount_in, uint64_t amount_out);
/** Whether nobody provides liquidity to a pool any more: it holds only what it keeps for good. */
inline bool Abandoned(const Pool& pool) { return pool.shares <= MIN_LIQUIDITY; }
} // namespace amm

namespace detail {
template <typename Stream, typename Key, typename Value>
void WriteChanges(Stream& s, const std::vector<std::pair<Key, std::optional<Value>>>& changes)
{
    WriteCompactSize(s, changes.size());
    for (const auto& [key, value] : changes) {
        s << key << value.has_value();
        if (value) s << *value;
    }
}
template <typename Stream, typename Key, typename Value>
void ReadChanges(Stream& s, std::vector<std::pair<Key, std::optional<Value>>>& changes)
{
    changes.clear();
    const uint64_t count{ReadCompactSize(s)};
    for (uint64_t i{0}; i < count; ++i) {
        Key key;
        bool has_value;
        s >> key >> has_value;
        std::optional<Value> value;
        if (has_value) s >> value.emplace();
        changes.emplace_back(std::move(key), std::move(value));
    }
}
} // namespace detail


/** What an operation did, for those who look: its results, in order. */
struct Result {
    AssetId asset;
    uint64_t amount{0};
    //! ASSET (CHN if `asset` is null), or LP: shares of the pool `asset` (CreatePool, PoolDeposit).
    Token::Kind kind{Token::Kind::ASSET};
};

struct Order;
namespace dex {
struct HopIn;
struct Clearing;
} // namespace dex
struct PoolSnapshot;
struct PoolAccumulator;
struct Observation;
struct OracleRecord;
struct Attestation;
struct OracleContract;

/**
 * The assets, on the store of the sidechain state (sidechain/store.h), one entry per key. Tables:
 * 0x30 what outputs carry, 0x31 assets, 0x32 reservations not revealed yet, 0x33 assets by number,
 * 0x34 pools, 0x35 auctions, 0x3d where reservations were made (ReservationOrigin), 0x3e the
 * history of the data of the assets (by asset, field, then position); single values 0x37 the next
 * number, 0x3f the layout of these tables (STORE_LAYOUT). Indexes, so that no rule reads a table
 * whole: 0x39 outputs by what they carry (its id, then its kind), 0x3a pools by asset, 0x3b
 * auctions not closed by asset. What a block changes,
 * the store's journal notes: its undo data.
 *
 * The DEX's (bitassets/dex.h), from SidechainParams::dex_height: 0x80 the price accumulators of the pools,
 * 0x81 their observations (by pool, then place in the ring), 0x86 oracles, 0x87 attestations (by oracle,
 * then event), 0x88 oracle contracts, 0x89 open contracts by the asset they hold; and, during a block
 * only (State::FinishBlock erases them), 0x82 the reserves of the pools the block changes as they were
 * before it, 0x83 the block's orders in their order, 0x84 how many there are, 0x85 their result outputs.
 * The DEX's pool kinds (from SidechainParams::dex_curves_height): 0x8a the kind of each pool made by
 * CreatePool of a kind other than the default (PoolParams; none: constant product at 0.30 %).
 *
 * Logos (from SidechainParams::logo_height): 0x90 the logo of each asset that has one (LogoRecord), 0x91
 * its bytes. No rule reads the bytes: they are kept for those who show them (getassetlogo), with the
 * state, so that a pruned node has them too; only the current logo of each asset is kept, and a
 * reorganization puts the one before back through the journal.
 *
 * Display names (from SidechainParams::ticker_height): 0x92 the display name of each asset that has one
 * (DisplayNameRecord). A table of its own, so that the layout of the others (STORE_LAYOUT) is the same.
 */
class State
{
public:
    /** Read only. */
    explicit State(const sidechain::StoreView& view) : m_view{&view}, m_overlay{nullptr} {}
    /** Read and write. */
    explicit State(sidechain::StoreOverlay& overlay) : m_view{&overlay}, m_overlay{&overlay} {}

    /**
     * Check a transaction against the state and, if it follows the rules, apply it. The CHN it pays
     * out are added to `payouts`, for the coinbase. A transaction that does not follow the rules,
     * or would take `payouts` (those of the block's transactions) beyond MAX_TX_PAYOUTS_PER_BLOCK,
     * changes nothing.
     * @param[out] released  CHN freed by retiring an asset (ReleaseAsset), for mainchain miners
     * @param[in]  queued    Layers: payouts of transactions still waiting for a block with the peg actions
     *                       (blocks of stake pay none), counted with `payouts` against MAX_TX_PAYOUTS_PER_BLOCK
     */
    [[nodiscard]] bool ApplyTx(const CTransaction& tx, int height, std::vector<CTxOut>& payouts, std::string& reject_reason, CAmount* released = nullptr, int reveal_depth = 0,
                               size_t queued = 0, bool dex = false);
    /**
     * The DEX: the end of a block's transactions (after the last ApplyTx of the block, with `dex`): clears
     * the block's orders (dex.h), fills their payouts in `payouts` (the vector their ApplyTx added to),
     * and updates the price accumulators of every pool the block changed, at the block's `time`. Never
     * fails: an order that cannot fill is refunded.
     */
    void FinishBlock(int height, uint32_t time, std::vector<CTxOut>& payouts, uint64_t queued = 0);
    /** Policy (the mempool): whether a transaction's order, alone against the pools as they are, gives its min_out (true if it has none). */
    bool OrderFillsAlone(const CTransaction& tx, std::string& reject_reason, int height) const;
    /**
     * Whether an asset is dead and can be retired; if not, why (`why`). By the rules of the block at `height` (without
     * one, by every rule: from v9_height a release also waits for the unsettled orders of the asset's books).
     */
    bool Releasable(const AssetId& asset, std::string* why = nullptr, std::optional<int> height = std::nullopt) const;
    /**
     * Whether ApplyTx would accept the transaction now (in a block with room for its payouts);
     * `results`, if given, gets what it would pay out, and `payouts` how many CHN payouts it gives rise to.
     */
    [[nodiscard]] bool CheckTx(const CTransaction& tx, int height, std::string& reject_reason, std::vector<Result>* results = nullptr, int reveal_depth = 0, size_t* payouts = nullptr,
                               bool dex = false) const;
    /** What the inputs of a transaction carry, in the order of the inputs. */
    std::vector<std::pair<uint32_t, Token>> SpentTokens(const CTransaction& tx) const;

    std::optional<Token> GetToken(const COutPoint& outpoint) const;
    std::optional<AssetRecord> GetAsset(const AssetId& asset) const;
    /** Every value each field of the data of an asset has had (read whole: for those who look). */
    AssetHistory GetHistory(const AssetId& asset) const;
    /** The data of an asset at the end of the block at `height`; nullopt if not registered then (or now). */
    std::optional<AssetData> DataAt(const AssetId& asset, int height) const;
    bool HasAsset(const AssetId& asset) const;
    std::optional<uint256> GetReservation(const Txid& id) const;
    std::optional<ReservationOrigin> GetReservationOrigin(const Txid& id) const;
    std::optional<Pool> GetPool(const uint256& id) const;
    std::optional<Auction> GetAuction(const Txid& id) const;
    std::optional<AssetId> AssetOfSeq(uint32_t seq) const;
    uint32_t NextSeq() const;
    /** The pool of two assets, if there is one. */
    std::optional<Pool> FindPool(const AssetId& a, const AssetId& b) const;
    void ForEachToken(const std::function<bool(const COutPoint&, const Token&)>& fn) const;
    void ForEachAsset(const std::function<bool(const AssetId&, const AssetRecord&)>& fn) const;
    void ForEachPool(const std::function<bool(const uint256&, const Pool&)>& fn) const;
    void ForEachAuction(const std::function<bool(const Txid&, const Auction&)>& fn) const;
    void ForEachReservation(const std::function<bool(const Txid&, const uint256&)>& fn) const;
    /** The logo of an asset, if it has one. */
    std::optional<LogoRecord> GetLogo(const AssetId& asset) const;
    /** The bytes of the logo of an asset, if it has one. */
    std::optional<std::vector<unsigned char>> GetLogoBytes(const AssetId& asset) const;
    /** The display name of an asset, if it has one. */
    std::optional<DisplayNameRecord> GetDisplayName(const AssetId& asset) const;
    /** The first output, by outpoint, that carries a token of this kind and id. */
    std::optional<COutPoint> FirstTokenOf(const uint256& id, Token::Kind kind) const;
    /** The pools of an asset, by id. */
    std::vector<std::pair<uint256, Pool>> PoolsOf(const AssetId& asset) const;
    /** The auctions not closed that sell an asset or sell for it, by id, `limit` at most. */
    std::vector<Txid> AuctionsOf(const AssetId& asset, size_t limit) const;
    /** How many pools and auctions there are (read whole: for information). */
    size_t PoolCount() const;
    /**
     * Layers: the CHN the pools and the auctions not collected hold (put in by markers, which burned them
     * out of the coins; paid out again by coinbases): a part of the coins the chain holds against the
     * mainchain's escrow (getsidechainstate's pegbalance). Read whole: for information.
     */
    CAmount HeldChn() const;

    // The DEX's tables (dex.h).
    std::optional<PoolAccumulator> GetAccumulator(const uint256& pool) const;
    /** The observation at a place of a pool's ring (count % TWAP_OBSERVATIONS). */
    std::optional<Observation> GetObservation(const uint256& pool, uint32_t place) const;
    std::optional<OracleRecord> GetOracle(const uint256& id) const;
    std::optional<Attestation> GetAttestation(const uint256& oracle, const uint256& event) const;
    std::optional<OracleContract> GetContract(const Txid& id) const;
    /** What kind of pool a pool is (curves.h): the default (constant product, 0.30 %) for one with none recorded. */
    PoolParams GetPoolParams(const uint256& pool) const;
    /** Whether a pool has a kind recorded (it was made by CreatePool, of a kind other than the default). */
    bool HasPoolParams(const uint256& pool) const;
    /** A launch, by its token. */
    std::optional<LaunchRecord> GetLaunch(const AssetId& token) const;
    /** Launches by the height that made them, oldest first, from (height, token) on, until `fn` returns false. */
    void ForEachLaunch(const std::function<bool(int32_t, const AssetId&)>& fn, int32_t from_height = 0) const;
    /** What the block at `height` did on each launch's curve. */
    std::vector<std::pair<AssetId, LaunchLogEntry>> GetLaunchLog(int32_t height) const;
    /** Whether an asset is on a launch curve that has not graduated (its pools are the curve's alone until then). */
    bool Launching(const AssetId& asset) const;
    // Concentrated liquidity (bitassets/clmath.h).
    std::optional<ClState> GetClState(const uint256& pool) const;
    std::optional<ClTick> GetClTick(const uint256& pool, int32_t tick) const;
    /** Whether a pool is one a launch graduated into with its locked position (from fee_flow_height). */
    bool IsLockedPool(const uint256& pool) const;
    std::optional<ClPosition> GetPosition(const uint256& id) const;
    /** The ticks of a pool with liquidity starting or ending there, in order, `max` at most. */
    std::vector<std::pair<int32_t, ClTick>> ClTicksOf(const uint256& pool, size_t max) const;
    /** The next tick with liquidity above `tick` (up) or at or below it (down), as Uniswap's walk takes them. */
    std::optional<int32_t> NextClTick(const uint256& pool, int32_t tick, bool up) const;
    /** The fees a position earned and not yet counted in its record (Uniswap's fee growth inside), and its amounts at the pool's price (rounded down). */
    std::pair<uint64_t, uint64_t> PositionFees(const ClPosition& position) const;
    // Order books.
    std::optional<BookOrder> GetOrder(const uint256& id) const;
    /** The open orders of a side of a book (asks: those selling asset0), best price first, `max` at most. */
    /**
     * The live orders of a side of a book, best first (by limit, then id): at most `max`, those that trade at `height`
     * (not expired, something left to sell); entries that do not are skipped before they count. At most MAX_BOOK_SCAN
     * entries read.
     */
    std::vector<std::pair<uint256, BookOrder>> BookSide(const uint256& book, bool asks, size_t max, int height) const;
    /** Takes the orders that expired at or before `height` out of their books' indexes, at most `budget`; how many. */
    size_t SweepBooks(int height, size_t budget = MAX_BOOK_SWEEP);
    /** The treasury's entry of an asset (the chain's coin: the null id); every entry. */
    std::optional<TreasuryEntry> GetTreasury(const AssetId& asset) const;
    void ForEachTreasury(const std::function<bool(const AssetId&, const TreasuryEntry&)>& fn) const;
    /** The security parts of the fees the block at `height` paid in the chain's coin (0 if none), and in all. */
    CAmount BlockSecurityFees(int height) const;
    SecurityFees GetSecurityFees() const;
    /** The orders in the index of a side of a book (live, or expired and not swept yet), as counted: at most MAX_BOOK_SIDE. */
    uint32_t BookCount(const uint256& book, bool asks) const;
    /** Every entry of a side's index, best first, live or not (at most MAX_BOOK_SCAN read). */
    std::vector<std::pair<uint256, BookOrder>> BookEntries(const uint256& book, bool asks) const;
    /**
     * A round of a book (doc/layers.md "Order books"): the takers `orders` (each all or nothing) and the book's best
     * resting orders (`resting`: those the round changed, by id, which it updates) at one uniform price; nullopt as
     * ClearPool's. `height`: orders that expired before it do not trade.
     */
    std::optional<dex::Clearing> ClearBook(const uint256& id, const Pool& pool, std::map<uint256, BookOrder>& resting, const std::vector<dex::HopIn>& orders, int height) const;
    // Dynamic fees.
    std::optional<FeeState> GetFeeState(const uint256& pool) const;
    /** The fee a pool charges in the block at `height` (its kind's; with POOL_FLAG_DYNAMIC_FEE, rising with its volatility). */
    uint32_t FeeOf(const uint256& pool, const PoolParams& params, int height) const;
    /** The same for a quote (the next block after the pool's last that traded). */
    uint32_t QuoteFee(const uint256& pool, const PoolParams& params) const;
    /**
     * A round of a concentrated pool (doc/layers.md "Concentrated liquidity"), on a copy of its state and of the ticks it
     * crosses (`ticks`: those the round changed, by tick), which it updates: the batch's uniform price with the net trade
     * walked through the pool's ticks (at most MAX_CL_CROSSINGS of them), the round's fees and roundings credited to the
     * liquidity in range. nullopt as ClearPool's.
     */
    /**
     * `take`: what the round takes out of its fees, of each asset (the security and protocol parts), at most what it kept
     * beyond the walk. `v9`: the rules from v9_height (the tick after a walk follows its last stretch's start; the rules
     * of the next block for a quote).
     */
    std::optional<dex::Clearing> ClearCl(const uint256& id, uint32_t fee, const Pool& pool, ClState& cl, std::map<int32_t, ClTick>& ticks, const std::vector<dex::HopIn>& orders, std::pair<uint64_t, uint64_t> take = {0, 0},
                                         bool v9 = true) const;
    /** Oracles by id, from `from` on, until `fn` returns false. */
    void ForEachOracle(const std::function<bool(const uint256&, const OracleRecord&)>& fn, const std::optional<uint256>& from = std::nullopt) const;
    void ForEachContract(const std::function<bool(const Txid&, const OracleContract&)>& fn) const;
    /** The attestations of an oracle, by event. */
    void ForEachAttestation(const uint256& oracle, const std::function<bool(const uint256&, const Attestation&)>& fn) const;

private:
    struct Plan;
    std::optional<Plan> MakePlan(const CTransaction& tx, int height, std::string& reject_reason, int reveal_depth) const;
    /** The DEX's part of MakePlan (dex.cpp). */
    bool PlanDex(const CTransaction& tx, const Operation& op, int height, Plan& plan, uint64_t& chn_taken, const std::function<void(const AssetId&, uint64_t)>& take,
                 std::string& reject_reason) const;
    /** Concentrated liquidity's part of MakePlan: OpenPosition, ChangePosition (CreatePool of that curve is PlanPools'). */
    bool PlanPosition(const CTransaction& tx, const Operation& op, int height, Plan& plan, const std::function<void(const AssetId&, uint64_t)>& take,
                      const std::function<void(Token::Kind, const uint256&, uint64_t)>& credit, const std::function<void(Token::Kind, const uint256&, uint64_t)>& debit,
                      std::string& reject_reason) const;
    /** Writes a book's resting orders a round changed: those filled leave its index and its count of open orders. */
    void CommitBook(const uint256& id, const std::map<uint256, BookOrder>& changed);
    /** The books a block placed orders in: their resting orders that cross trade (FinishBlock). */
    void CrossBooks(int height);
    /** Writes a pool's concentrated state, noting the price it had before its block (the price accumulators). */
    void SetClState(const uint256& pool, const ClState& state);
    /** Launches' part of MakePlan, after the registration's: the curve, its pool, the first buy. */
    //! `supply`: the launch's units (0: LAUNCH_SUPPLY); `from_asset`: an existing asset put on a curve (CurveAsset, as a launch of its fields).
    bool PlanLaunch(const CTransaction& tx, const LaunchToken& launch, uint64_t supply, bool from_asset, int height, Plan& plan, const std::function<void(const AssetId&, uint64_t)>& take,
                    std::string& reject_reason) const;
    /** A launch graduates (FinishBlock): its curve's coin and the tokens left into the token's pool with the coin, locked. */
    void Graduate(const AssetId& token, int height);
    /** Graduate's part from fee_flow_height: into a locked full-range position (false: it cannot, and the launch graduates as before). */
    bool GraduateLocked(const AssetId& token, LaunchRecord& record, uint64_t coin, uint64_t units, int height);
    /**
     * The fees a locked position (GraduateLocked) earned in the block, collected out of its pool: of each asset
     * GRADUATED_CREATOR_SHARE to the launch's creator (by its fee destination), the rest to the chain's security (the
     * chain's coin with the block's security fees, returned; in the token, none from fee_flow_height: ClearCoinSide).
     */
    uint64_t CollectLocked(const uint256& pool, const AssetId& token, int height);
    /** What a protocol order (Order::sink) gets or gets back: bought back and burned, or back to wait. */
    void PaySink(const Order& order, const AssetId& asset, uint64_t amount, uint64_t& security);
    /** The orders the protocol makes in a block from fee_flow_height: buybacks; what they take is taken out of waiting. */
    std::vector<Order> ProtocolOrders(int height);
public:
    /**
     * A round of a pool of the chain's coin with its fee on the coin's side (from fee_flow_height): the round of the
     * kind at no fee on the buys' coin less their fee (rounded up) and the sales' units whole, all at that round's one
     * price; then each sale's fee (rounded up) out of the coin it gets. Of all the fees, `security_share` (millionths;
     * rounded down) leaves for the chain's security (`security`), the rest stays in the pool's coin (a concentrated
     * pool: added to its fee growth in the coin, at the liquidity in range after the round; with none, to the pool).
     * Every order still gets the same price; min_out counts what it gets, net of the fee.
     */
    std::optional<dex::Clearing> ClearCoinSide(const uint256& id, const PoolParams& params, uint32_t fee, const Pool& pool, ClState* cl, std::map<int32_t, ClTick>* ticks,
                                               const std::vector<dex::HopIn>& orders, uint32_t security_share, uint64_t& security, bool v9 = true) const;
    /** Whether the pools of the chain's coin take their fees on its side (fee_flow_height reached: set by its first block's end). */
    bool CoinSideFees() const;
private:
    /** A launch's creator's part of its fees by its destination (v5): the coin to creator_fees or buyback, the token to creator_tokens or burned. */
    void CreditCreator(const AssetId& token, LaunchRecord& record, uint64_t coin, uint64_t units);
    /** v7: the same by the launch's split (FeeShare): the creator's claimed, the burn's bought back, an address's owed. */
    void CreditSplit(const AssetId& token, LaunchRecord& record, uint64_t coin, uint64_t units);
    /** v7: pay the addresses of the splits owed MIN_FEE_SHARE_PAYOUT or more, `room` of them at most (by token, then share). */
    void PayFeeShares(int height, size_t room, std::vector<CTxOut>& payouts);
    /** The DEX's pool kinds' part of MakePlan: CreatePool, PoolDeposit, PoolWithdraw. */
    bool PlanPools(const CTransaction& tx, const Operation& op, int height, Plan& plan, const std::function<void(const AssetId&, uint64_t)>& take,
                   const std::function<void(Token::Kind, const uint256&, uint64_t)>& debit, std::string& reject_reason) const;
    /** The batch clearing (dex.cpp). */
    void ClearOrders(const std::vector<Order>& orders, std::vector<CTxOut>& payouts, int height);
    /** Whether an order of this block holds an asset (keeps it from being retired). */
    bool OrderHolds(const AssetId& asset) const;

    // Writes, with the indexes kept in step.
    sidechain::StoreOverlay& Writable() const;
    void SetToken(const COutPoint& outpoint, const std::optional<Token>& token);
    void SetReservation(const Txid& id, const std::optional<uint256>& commitment);
    void SetPool(const uint256& id, const std::optional<Pool>& pool);
    void SetAuction(const Txid& id, const std::optional<Auction>& auction);
    /** Adds `delta` to a count of the record of an asset (AssetRecord::holding_auctions, provided_pools), if it is registered. */
    void Count(const AssetId& asset, uint32_t AssetRecord::*count, int delta);

    const sidechain::StoreView* m_view;
    sidechain::StoreOverlay* m_overlay;
    //! Whether the DEX's rules apply (ApplyTx, CheckTx, FinishBlock): pools changed are noted for the price accumulators.
    mutable bool m_dex{false};
};

/**
 * The layout of the tables of the assets in the store. A store of another layout (one written by an
 * earlier version) is not read: the node derives the state again from the blocks (StoreLayoutCurrent).
 *  1: the history of an asset's data in its record; outputs indexed by id only; closed auctions indexed.
 *  2: the history apart (0x3e); outputs indexed by id then kind; closed auctions out of the index;
 *     where each reservation was made (0x3d); the registrations of an auction's assets in it.
 *  3: in each asset's record, how many auctions hold some of it and how many of its pools have
 *     providers (AssetRecord::holding_auctions, provided_pools).
 *  4: no order of reservations (0x36, 0x38) nor index of reservations by commitment (0x3c).
 *  5: withdrawal refunds wait with the deposits, not behind the payouts of the transactions; derived
 *     again, so that a node that kept refunds in the old queue does not pay differently from one that
 *     syncs from scratch.
 *  (The search index, derived data, has a layout of its own: bitassets::index::INDEX_LAYOUT.)
 */
inline constexpr uint32_t STORE_LAYOUT{5};
/** Whether the assets in a store are in the layout of this version, or there are none yet. */
bool StoreLayoutCurrent(const sidechain::StoreView& view);

/** Whether the marker of a transaction, if any, is well formed. This depends on the transaction alone. */
[[nodiscard]] bool CheckMarker(const CTransaction& tx, std::string& reject_reason);
/** The outputs a transaction's marker lists: they carry no value, and are not dust. */
std::set<uint32_t> TokenOutputs(const CTransaction& tx);
/** Whether a script is one a token can be carried by, or CHN paid to: a standard one someone can spend. */
bool Spendable(const CScript& script);

/**
 * A view that notes what is read through it: the keys read one by one, and the prefixes read in key
 * order. Whatever a check made through it found, it finds again as long as no key it read, nor any key
 * under a prefix it read, changes (the mempool checks a transaction with a marker again only then:
 * CTxMemPool::m_bitassets_reads).
 */
class RecordingView : public sidechain::StoreView
{
public:
    explicit RecordingView(const sidechain::StoreView& base) : m_base{base} {}
    std::optional<sidechain::StoreBytes> Get(std::span<const unsigned char> key) const override
    {
        m_keys.emplace(key.begin(), key.end());
        return m_base.Get(key);
    }
    std::optional<std::pair<sidechain::StoreBytes, sidechain::StoreBytes>> Next(std::span<const unsigned char> from, std::span<const unsigned char> prefix) const override
    {
        m_prefixes.emplace(prefix.begin(), prefix.end());
        return m_base.Next(from, prefix);
    }
    const std::set<sidechain::StoreBytes>& Keys() const { return m_keys; }
    const std::set<sidechain::StoreBytes>& Prefixes() const { return m_prefixes; }

private:
    const sidechain::StoreView& m_base;
    mutable std::set<sidechain::StoreBytes> m_keys;
    mutable std::set<sidechain::StoreBytes> m_prefixes;
};

/**
 * Whether what a transaction's operation is checked against may depend on the height as well as on
 * the state: an auction made to start at a height, a bid, a registration (reveal depth), a collection,
 * and any operation not known to read no height (fail closed). The mempool checks those again at every
 * change of the tip.
 */
bool CheckDependsOnHeight(const CTransaction& tx);

} // namespace bitassets

#endif // BITCOIN_BITASSETS_STATE_H
