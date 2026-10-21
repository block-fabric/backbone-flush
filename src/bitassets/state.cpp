// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Inspired by BitAssets by LayerTwo Labs (plain-bitassets).

#include <bitassets/state.h>

#include <bitassets/clmath.h>
#include <bitassets/curves.h>
#include <bitassets/dex.h>
#include <bitassets/displayname_table.h>
#include <bitassets/logo.h>

#include <arith_uint256.h>
#include <chainparams.h>

#include <crypto/hmac_sha256.h>
#include <crypto/sha256.h>
#include <hash.h>
#include <pubkey.h>
#include <script/solver.h>
#include <streams.h>
#include <tinyformat.h>
#include <util/check.h>

#include <algorithm>

namespace bitassets {

namespace {
template <typename T>
void WriteOperation(DataStream& s, const T& op)
{
    s << T::KIND << op;
}

/**
 * A registration with a logo is a marker of its own kind: the registration, then the logo; one with a
 * display name another: the registration, the logo (empty if none), then the display name.
 */
void WriteOperation(DataStream& s, const Register& op)
{
    if (!op.display_name.empty()) {
        s << REGISTER_WITH_DISPLAY_NAME_KIND << op << op.logo << op.display_name;
    } else if (op.logo.empty()) {
        s << Register::KIND << op;
    } else {
        s << REGISTER_WITH_LOGO_KIND << op << op.logo;
    }
}

template <typename T>
void ReadOperation(DataStream& s, std::optional<Operation>& out)
{
    T op;
    s >> op;
    out = std::move(op);
}

/** Marker outputs: the index, then a kind byte (a token kind, or RESULT_KIND), then the id and amount of a token. */
constexpr uint8_t RESULT_KIND{0xFF};

void WriteOutputs(DataStream& s, const std::vector<MarkerOutput>& outputs)
{
    WriteCompactSize(s, outputs.size());
    for (const MarkerOutput& out : outputs) {
        s << VARINT(out.n);
        if (!out.token) {
            s << RESULT_KIND;
            continue;
        }
        s << static_cast<uint8_t>(out.token->kind) << out.token->id;
        if (Token::Divisible(out.token->kind)) s << VARINT(out.token->amount);
    }
}

void ReadOutputs(DataStream& s, std::vector<MarkerOutput>& outputs)
{
    const uint64_t count{ReadCompactSize(s)};
    if (count > MAX_MARKER_OUTPUTS) throw std::ios_base::failure("too many outputs");
    outputs.clear();
    for (uint64_t i{0}; i < count; ++i) {
        MarkerOutput out;
        uint8_t kind;
        s >> VARINT(out.n) >> kind;
        if (kind != RESULT_KIND) {
            // POSITION and ORDER (the DEX's pool kinds) parse as Token's own serialization does; a marker that names them is
            // not valid before their height (CheckCurvesActivation), as it never was.
            if (kind > static_cast<uint8_t>(Token::Kind::ORDER)) throw std::ios_base::failure("unknown token kind");
            Token& token{out.token.emplace()};
            token.kind = static_cast<Token::Kind>(kind);
            s >> token.id;
            token.amount = 1;
            if (Token::Divisible(token.kind)) s >> VARINT(token.amount);
        }
        outputs.push_back(out);
    }
}

bool GoodData(const AssetData& data)
{
    if (data.info && data.info->size() > MAX_INFO_SIZE) return false;
    if (data.encryption_key && !CPubKey{std::span{data.encryption_key->data(), data.encryption_key->size()}}.IsFullyValid()) return false;
    if (data.signing_key && !XOnlyPubKey{*data.signing_key}.IsFullyValid()) return false;
    return true;
}

bool GoodUpdates(const AssetUpdates& updates)
{
    AssetData set;
    if (updates.encryption_key.kind == UpdateKind::SET) set.encryption_key = updates.encryption_key.value;
    if (updates.signing_key.kind == UpdateKind::SET) set.signing_key = updates.signing_key.value;
    if (updates.info.kind == UpdateKind::SET) set.info = updates.info.value;
    return GoodData(set);
}

/** Calls `fn` for each field of the data of an asset: its number, and where it is in AssetData, AssetUpdates and AssetHistory. */
template <typename Fn>
void ForEachField(Fn&& fn)
{
    fn(DataField::COMMITMENT, &AssetData::commitment, &AssetUpdates::commitment, &AssetHistory::commitment);
    fn(DataField::IPV4, &AssetData::ipv4, &AssetUpdates::ipv4, &AssetHistory::ipv4);
    fn(DataField::IPV6, &AssetData::ipv6, &AssetUpdates::ipv6, &AssetHistory::ipv6);
    fn(DataField::ENCRYPTION_KEY, &AssetData::encryption_key, &AssetUpdates::encryption_key, &AssetHistory::encryption_key);
    fn(DataField::SIGNING_KEY, &AssetData::signing_key, &AssetUpdates::signing_key, &AssetHistory::signing_key);
    fn(DataField::INFO, &AssetData::info, &AssetUpdates::info, &AssetHistory::info);
}

template <typename T>
std::optional<T> AtHeightOf(const std::vector<Stamped<T>>& history, int height)
{
    std::optional<T> value;
    for (const Stamped<T>& entry : history) {
        if (entry.height > height) break;
        value = entry.value;
    }
    return value;
}

std::optional<std::vector<unsigned char>> MarkerData(const CScript& script)
{
    if (script.size() < 2 || script[0] != OP_RETURN) return std::nullopt;
    CScript::const_iterator pc{script.begin() + 1};
    opcodetype opcode;
    std::vector<unsigned char> data;
    if (!script.GetOp(pc, opcode, data) || opcode > OP_PUSHDATA4 || pc != script.end()) return std::nullopt;
    if (data.size() < sizeof(TAG) || !std::equal(std::begin(TAG), std::end(TAG), data.begin())) return std::nullopt;
    return data;
}

uint64_t Isqrt(unsigned __int128 n)
{
    if (n == 0) return 0;
    // Newton's method from above, in 128 bits.
    unsigned __int128 x{n}, y{(x + 1) / 2};
    while (y < x) {
        x = y;
        y = (x + n / x) / 2;
    }
    return static_cast<uint64_t>(x);
}

/** a + b, if it is at most MAX_AMOUNT. */
std::optional<uint64_t> Add(uint64_t a, uint64_t b)
{
    if (a > MAX_AMOUNT || b > MAX_AMOUNT - a) return std::nullopt;
    return a + b;
}
} // namespace

bool AssetUpdates::Empty() const
{
    return commitment.kind == UpdateKind::RETAIN && ipv4.kind == UpdateKind::RETAIN && ipv6.kind == UpdateKind::RETAIN &&
           encryption_key.kind == UpdateKind::RETAIN && signing_key.kind == UpdateKind::RETAIN && info.kind == UpdateKind::RETAIN;
}

CScript MarkerScript(const Marker& marker)
{
    DataStream s{};
    s.write(std::as_bytes(std::span{TAG}));
    s << MARKER_VERSION;
    if (!marker.operation) {
        s << uint8_t{0};
    } else {
        std::visit([&](const auto& op) { WriteOperation(s, op); }, *marker.operation);
    }
    WriteOutputs(s, marker.outputs);
    return CScript() << OP_RETURN << std::vector<unsigned char>{UCharCast(s.data()), UCharCast(s.data()) + s.size()};
}

bool IsMarkerScript(const CScript& script)
{
    if (script.size() < 2 || script[0] != OP_RETURN) return false;
    CScript::const_iterator pc{script.begin() + 1};
    opcodetype opcode;
    std::vector<unsigned char> data;
    if (!script.GetOp(pc, opcode, data) || opcode > OP_PUSHDATA4) return false;
    return data.size() >= sizeof(TAG) && std::equal(std::begin(TAG), std::end(TAG), data.begin());
}

std::optional<Marker> ParseMarkerScript(const CScript& script)
{
    const auto data{MarkerData(script)};
    if (!data) return std::nullopt;
    try {
        DataStream s{std::span{*data}.subspan(sizeof(TAG))};
        uint8_t version, kind;
        s >> version >> kind;
        if (version != MARKER_VERSION) return std::nullopt;
        Marker marker;
        switch (kind) {
        case 0: break;
        case Reserve::KIND: ReadOperation<Reserve>(s, marker.operation); break;
        case Register::KIND: ReadOperation<Register>(s, marker.operation); break;
        case Mint::KIND: ReadOperation<Mint>(s, marker.operation); break;
        case UpdateAsset::KIND: ReadOperation<UpdateAsset>(s, marker.operation); break;
        case Burn::KIND: ReadOperation<Burn>(s, marker.operation); break;
        case Swap::KIND: ReadOperation<Swap>(s, marker.operation); break;
        case AddLiquidity::KIND: ReadOperation<AddLiquidity>(s, marker.operation); break;
        case RemoveLiquidity::KIND: ReadOperation<RemoveLiquidity>(s, marker.operation); break;
        case CreateAuction::KIND: ReadOperation<CreateAuction>(s, marker.operation); break;
        case Bid::KIND: ReadOperation<Bid>(s, marker.operation); break;
        case Collect::KIND: ReadOperation<Collect>(s, marker.operation); break;
        case ReleaseAsset::KIND: ReadOperation<ReleaseAsset>(s, marker.operation); break;
        // The DEX's (valid from SidechainParams::dex_height: MakePlan).
        case Route::KIND: ReadOperation<Route>(s, marker.operation); break;
        case RegisterOracle::KIND: ReadOperation<RegisterOracle>(s, marker.operation); break;
        case Attest::KIND: ReadOperation<Attest>(s, marker.operation); break;
        case CreateOracleContract::KIND: ReadOperation<CreateOracleContract>(s, marker.operation); break;
        case SettleOracleContract::KIND: ReadOperation<SettleOracleContract>(s, marker.operation); break;
        // The DEX's pool kinds (valid from SidechainParams::dex_curves_height: CheckCurvesActivation).
        case CreatePool::KIND: ReadOperation<CreatePool>(s, marker.operation); break;
        case PoolDeposit::KIND: ReadOperation<PoolDeposit>(s, marker.operation); break;
        case PoolWithdraw::KIND: ReadOperation<PoolWithdraw>(s, marker.operation); break;
        case SwapPools::KIND: ReadOperation<SwapPools>(s, marker.operation); break;
        case LaunchToken::KIND: ReadOperation<LaunchToken>(s, marker.operation); break;
        case ClaimLaunch::KIND: ReadOperation<ClaimLaunch>(s, marker.operation); break;
        case OpenPosition::KIND: ReadOperation<OpenPosition>(s, marker.operation); break;
        case ChangePosition::KIND: ReadOperation<ChangePosition>(s, marker.operation); break;
        case PlaceOrder::KIND: ReadOperation<PlaceOrder>(s, marker.operation); break;
        case SettleOrder::KIND: ReadOperation<SettleOrder>(s, marker.operation); break;
        case SetLogo::KIND: ReadOperation<SetLogo>(s, marker.operation); break;
        case SetDisplayName::KIND: ReadOperation<SetDisplayName>(s, marker.operation); break;
        case REGISTER_WITH_DISPLAY_NAME_KIND: {
            Register reg;
            s >> reg >> reg.logo >> reg.display_name;
            // One without a display name is written as another kind (the check below).
            marker.operation = std::move(reg);
            break;
        }
        case REGISTER_WITH_LOGO_KIND: {
            Register reg;
            s >> reg >> reg.logo;
            // An empty one is written as a Register (the check below): one way to write a marker.
            marker.operation = std::move(reg);
            break;
        }
        default: return std::nullopt;
        }
        ReadOutputs(s, marker.outputs);
        if (!s.empty()) return std::nullopt;
        // Written back the same: there is one way to write a marker.
        if (MarkerScript(marker) != script) return std::nullopt;
        return marker;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<Marker> GetMarker(const CTransaction& tx, std::string* error)
{
    std::optional<Marker> found;
    bool seen{false};
    for (const CTxOut& out : tx.vout) {
        if (!IsMarkerScript(out.scriptPubKey)) continue;
        if (seen) {
            if (error) *error = "bad-ba-markers";
            return std::nullopt;
        }
        seen = true;
        found = ParseMarkerScript(out.scriptPubKey);
        if (!found) {
            if (error) *error = "bad-ba-marker";
            return std::nullopt;
        }
    }
    if (error) error->clear();
    return found;
}

AssetId HashName(const std::string& name)
{
    uint256 hash;
    CSHA256().Write(UCharCast(name.data()), name.size()).Finalize(hash.data());
    return hash;
}

uint256 ReservationCommitment(const AssetId& name, const uint256& nonce, const CScript& script)
{
    uint256 commitment;
    CHMAC_SHA256(nonce.data(), nonce.size()).Write(name.data(), name.size()).Write(script.data(), script.size()).Finalize(commitment.data());
    return commitment;
}

uint256 PoolId(const AssetId& a, const AssetId& b)
{
    HashWriter writer{};
    writer << std::string{"BitAssets pool"} << std::min(a, b) << std::max(a, b);
    return writer.GetSHA256();
}

uint256 OrderId(const Txid& txid)
{
    HashWriter writer{};
    writer << std::string{"Layers order"} << txid;
    return writer.GetSHA256();
}

uint256 BookId(const AssetId& a, const AssetId& b)
{
    PoolParams params;
    params.curve = static_cast<uint8_t>(Curve::BOOK);
    params.fee = 0;
    return PoolIdOf(a, b, params);
}

uint256 PositionId(const Txid& txid)
{
    HashWriter writer{};
    writer << std::string{"Layers position"} << txid;
    return writer.GetSHA256();
}

bool IsAssetName(const std::string& name)
{
    if (name.empty() || name.size() > MAX_NAME_TEXT_SIZE || name.front() == ' ' || name.back() == ' ') return false;
    return std::all_of(name.begin(), name.end(), [](char c) { return c >= 0x20 && c <= 0x7E; });
}

bool IsTicker(const std::string& name)
{
    if (name.size() < MIN_TICKER_SIZE || name.size() > MAX_TICKER_SIZE) return false;
    return std::all_of(name.begin(), name.end(), [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); });
}

namespace {
/** The next code point of well-formed UTF-8 at `i` (moved past it); nullopt if not well formed there. */
std::optional<uint32_t> NextCodePoint(const std::string& text, size_t& i)
{
    const auto byte{[&](size_t k) { return static_cast<uint8_t>(text[k]); }};
    const uint8_t b0{byte(i)};
    size_t len;
    uint32_t cp;
    if (b0 < 0x80) {
        ++i;
        return b0;
    } else if (b0 >= 0xC2 && b0 <= 0xDF) {
        len = 2;
        cp = b0 & 0x1F;
    } else if (b0 >= 0xE0 && b0 <= 0xEF) {
        len = 3;
        cp = b0 & 0x0F;
    } else if (b0 >= 0xF0 && b0 <= 0xF4) {
        len = 4;
        cp = b0 & 0x07;
    } else {
        return std::nullopt;
    }
    if (i + len > text.size()) return std::nullopt;
    for (size_t k{1}; k < len; ++k) {
        const uint8_t b{byte(i + k)};
        if ((b & 0xC0) != 0x80) return std::nullopt;
        cp = (cp << 6) | (b & 0x3F);
    }
    // Shortest forms only, no surrogates, nothing past U+10FFFF.
    if ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF)) return std::nullopt;
    i += len;
    return cp;
}

bool DisplayNameCodePoint(uint32_t cp)
{
    const auto* begin{std::begin(detail::DISPLAY_NAME_RANGES)};
    const auto* end{std::end(detail::DISPLAY_NAME_RANGES)};
    // The first range that ends at or after it.
    const auto* it{std::lower_bound(begin, end, cp, [](const uint32_t (&range)[2], uint32_t c) { return range[1] < c; })};
    return it != end && (*it)[0] <= cp;
}
} // namespace

std::optional<std::string> WhyNotDisplayName(const std::string& text)
{
    if (text.empty()) return "it is empty";
    if (text.size() > MAX_DISPLAY_NAME_SIZE) return strprintf("it is longer than %u bytes of UTF-8", MAX_DISPLAY_NAME_SIZE);
    if (text.front() == ' ' || text.back() == ' ') return "it starts or ends with a space";
    for (size_t i{0}; i < text.size();) {
        const size_t at{i};
        const auto cp{NextCodePoint(text, i)};
        if (!cp) return strprintf("it is not well-formed UTF-8 (at byte %u)", at);
        if (!DisplayNameCodePoint(*cp)) return strprintf("it holds U+%04X, which a display name may not: a control or format character (such as a bidirectional override or a zero-width one), a space other than U+0020, a combining mark, a character NFC changes, or one not assigned in Unicode 15.0", *cp);
    }
    return std::nullopt;
}

bool IsDisplayName(const std::string& text) { return !WhyNotDisplayName(text).has_value(); }

bool ReadsAsAnotherAsset(const std::string& name)
{
    const auto lower{[](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }};
    if (name.size() == 3 && lower(name[0]) == 'c' && lower(name[1]) == 'h' && lower(name[2]) == 'n') return true;
    if (name.size() >= 2 && name[0] == '0' && lower(name[1]) == 'x') return true;
    // Groups of four digits, two or three of them.
    if (name.size() != 9 && name.size() != 14) return false;
    for (size_t i{0}; i < name.size(); ++i) {
        const bool dash{i % 5 == 4};
        if (dash ? name[i] != '-' : (name[i] < '0' || name[i] > '9')) return false;
    }
    return true;
}

bool ShapedAsNumber(const std::string& text)
{
    if (text.size() != 9 && text.size() != 14) return false;
    for (size_t i{0}; i < text.size(); ++i) {
        const bool dash{i % 5 == 4};
        if (dash ? text[i] != '-' : (text[i] < '0' || text[i] > '9')) return false;
    }
    return true;
}

bool NameMimicsNumber(const std::string& name)
{
    if (name.find_first_of("#()") != std::string::npos) return true;
    const auto digit{[](char c) { return c >= '0' && c <= '9'; }};
    for (size_t i{0}; i + 9 <= name.size(); ++i) {
        bool shaped{name[i + 4] == '-'};
        for (size_t j{0}; shaped && j < 9; ++j) {
            if (j != 4 && !digit(name[i + j])) shaped = false;
        }
        if (shaped) return true;
    }
    return false;
}

namespace {
/** Whether an asset is CHN by name, in any case. */
bool IsChnName(const AssetId& asset)
{
    static const std::set<AssetId> chn{[] {
        std::set<AssetId> names;
        for (int mask{0}; mask < 8; ++mask) {
            std::string name{"chn"};
            for (int i{0}; i < 3; ++i) {
                if (mask & (1 << i)) name[i] = static_cast<char>(name[i] - 'a' + 'A');
            }
            names.insert(HashName(name));
        }
        return names;
    }()};
    return chn.contains(asset);
}

constexpr uint32_t SEQ_DISPLAY_OFFSET{23071990};
constexpr uint32_t SEQ_LOW{100000000};
} // namespace

std::string FormatSeq(uint32_t seq)
{
    const uint32_t hi{seq / SEQ_LOW};
    const uint32_t lo{(seq % SEQ_LOW + SEQ_DISPLAY_OFFSET) % SEQ_LOW};
    const std::string d{strprintf("%08u", lo)};
    const std::string first{d[4], d[3], d[1], d[6]};
    const std::string second{d[2], d[7], d[0], d[5]};
    if (hi > 0) return strprintf("%04u-%s-%s", hi, first, second);
    return first + "-" + second;
}

std::optional<uint32_t> ParseSeq(const std::string& text)
{
    std::vector<std::string> groups;
    size_t start{0};
    while (true) {
        const size_t end{std::min(text.find('-', start), text.size())};
        groups.push_back(text.substr(start, end - start));
        if (end == text.size()) break;
        start = end + 1;
    }
    if (groups.size() != 2 && groups.size() != 3) return std::nullopt;
    for (const std::string& group : groups) {
        if (group.size() != 4 || !std::all_of(group.begin(), group.end(), [](char c) { return c >= '0' && c <= '9'; })) return std::nullopt;
    }
    uint64_t hi{0};
    if (groups.size() == 3) hi = std::stoul(groups[0]);
    const std::string& first{groups[groups.size() - 2]};
    const std::string& second{groups[groups.size() - 1]};
    std::string d(8, '0');
    d[4] = first[0];
    d[3] = first[1];
    d[1] = first[2];
    d[6] = first[3];
    d[2] = second[0];
    d[7] = second[1];
    d[0] = second[2];
    d[5] = second[3];
    const uint64_t lo{std::stoul(d)};
    const uint64_t low{(lo + SEQ_LOW - SEQ_DISPLAY_OFFSET) % SEQ_LOW};
    const uint64_t seq{hi * SEQ_LOW + low};
    if (seq > std::numeric_limits<uint32_t>::max()) return std::nullopt;
    if (groups.size() == 3 && hi == 0) return std::nullopt;
    return static_cast<uint32_t>(seq);
}

std::optional<AssetData> AssetHistory::AtHeight(int registered, int at) const
{
    if (at < registered) return std::nullopt;
    AssetData data;
    ForEachField([&](DataField, auto data_field, auto, auto history_field) { data.*data_field = AtHeightOf(this->*history_field, at); });
    return data;
}

uint64_t Auction::PriceAt(int height) const
{
    if (duration <= 1 || height <= start_height) return start_price;
    if (height >= EndHeight()) return end_price;
    // In a straight line, rounded up: the seller never gets less than the line.
    const unsigned __int128 drop{static_cast<unsigned __int128>(start_price - end_price) * static_cast<uint32_t>(height - start_height) / static_cast<uint32_t>(duration - 1)};
    return start_price - static_cast<uint64_t>(drop);
}

uint64_t Auction::BuysAt(int height, uint64_t quote_amount) const
{
    const uint64_t price{PriceAt(height)};
    if (price == 0) return 0;
    // Rounded down: the buyer never gets more than paid for.
    const unsigned __int128 units{static_cast<unsigned __int128>(quote_amount) * base_amount / price};
    return units > MAX_AMOUNT ? MAX_AMOUNT : static_cast<uint64_t>(units);
}

uint64_t Auction::CostOfRemaining(int height) const
{
    if (base_amount == 0) return 0;
    const unsigned __int128 cost{(static_cast<unsigned __int128>(remaining) * PriceAt(height) + base_amount - 1) / base_amount};
    return static_cast<uint64_t>(std::min<unsigned __int128>(cost, MAX_AMOUNT));
}

namespace amm {
uint64_t SwapOut(uint64_t reserve_in, uint64_t reserve_out, uint64_t amount_in)
{
    if (reserve_in == 0 || reserve_out == 0 || amount_in == 0) return 0;
    // Uniswap v2: the fee stays in the pool, the product of the reserves never falls. In 256 bits:
    // amount (63 bits) times 997 (10 bits) times a reserve (63 bits) does not fit in 128.
    const arith_uint256 in_with_fee{arith_uint256{amount_in} * (1000 - SWAP_FEE_PER_MILLE)};
    const arith_uint256 out{in_with_fee * arith_uint256{reserve_out} / (arith_uint256{reserve_in} * 1000 + in_with_fee)};
    // Less than reserve_out, so it fits.
    return out.GetLow64();
}

std::optional<uint64_t> SwapIn(uint64_t reserve_in, uint64_t reserve_out, uint64_t amount_out)
{
    if (reserve_in == 0 || amount_out == 0 || amount_out >= reserve_out) return std::nullopt;
    const arith_uint256 in{arith_uint256{reserve_in} * arith_uint256{amount_out} * 1000 / (arith_uint256{reserve_out - amount_out} * (1000 - SWAP_FEE_PER_MILLE)) + 1};
    if (in > arith_uint256{MAX_AMOUNT}) return std::nullopt;
    return in.GetLow64();
}

std::optional<Deposit> Provide(const Pool& pool, uint64_t amount0, uint64_t amount1)
{
    if (amount0 == 0 || amount1 == 0 || amount0 > MAX_AMOUNT || amount1 > MAX_AMOUNT) return std::nullopt;
    if (Abandoned(pool)) {
        // As a new pool is made, with what it holds (the dust everyone left) merged in.
        const auto r0{Add(pool.reserve0, amount0)}, r1{Add(pool.reserve1, amount1)};
        if (!r0 || !r1) return std::nullopt;
        const uint64_t total{Isqrt(static_cast<unsigned __int128>(*r0) * *r1)};
        if (total <= MIN_LIQUIDITY) return std::nullopt;
        return Deposit{total - MIN_LIQUIDITY, amount0, amount1, /*opens=*/true};
    }
    if (pool.reserve0 == 0 || pool.reserve1 == 0) return std::nullopt;
    const unsigned __int128 s0{static_cast<unsigned __int128>(amount0) * pool.shares / pool.reserve0};
    const unsigned __int128 s1{static_cast<unsigned __int128>(amount1) * pool.shares / pool.reserve1};
    const unsigned __int128 shares{std::min(s0, s1)};
    if (shares == 0 || shares > MAX_AMOUNT) return std::nullopt;
    // What the shares are worth of a reserve, rounded up: at most the amount offered, as the shares
    // are at most what that amount gives.
    const auto worth{[&](uint64_t reserve) { return static_cast<uint64_t>((shares * reserve + pool.shares - 1) / pool.shares); }};
    Deposit deposit{static_cast<uint64_t>(shares), amount0, amount1, /*opens=*/false};
    if (s0 <= s1) {
        deposit.take1 = worth(pool.reserve1);
    } else {
        deposit.take0 = worth(pool.reserve0);
    }
    // CHN goes back by the coinbase: dust stays in the pool, for its providers.
    if (pool.asset0.IsNull() && amount0 - deposit.take0 < MIN_CHN_PAYOUT) deposit.take0 = amount0;
    if (pool.asset1.IsNull() && amount1 - deposit.take1 < MIN_CHN_PAYOUT) deposit.take1 = amount1;
    return deposit;
}

double PriceImpact(uint64_t reserve_in, uint64_t reserve_out, uint64_t amount_in, uint64_t amount_out)
{
    if (reserve_in == 0 || reserve_out == 0 || amount_in == 0) return 100.0;
    const double impact{100.0 * (1.0 - (static_cast<double>(amount_out) / amount_in) / (static_cast<double>(reserve_out) / reserve_in))};
    return std::clamp(impact, 0.0, 100.0);
}

std::pair<uint64_t, uint64_t> Withdraw(const Pool& pool, uint64_t shares)
{
    if (pool.shares == 0 || shares > pool.shares) return {0, 0};
    return {static_cast<uint64_t>(static_cast<unsigned __int128>(pool.reserve0) * shares / pool.shares),
            static_cast<uint64_t>(static_cast<unsigned __int128>(pool.reserve1) * shares / pool.shares)};
}

std::pair<uint64_t, uint64_t> WithdrawPaid(const Pool& pool, uint64_t shares)
{
    auto [out0, out1]{Withdraw(pool, shares)};
    if (pool.asset0.IsNull() && out0 < MIN_CHN_PAYOUT) out0 = 0;
    if (pool.asset1.IsNull() && out1 < MIN_CHN_PAYOUT) out1 = 0;
    return {out0, out1};
}
} // namespace amm

bool Spendable(const CScript& script)
{
    std::vector<std::vector<unsigned char>> solutions;
    switch (Solver(script, solutions)) {
    case TxoutType::PUBKEYHASH:
    case TxoutType::SCRIPTHASH:
    case TxoutType::WITNESS_V0_KEYHASH:
    case TxoutType::WITNESS_V0_SCRIPTHASH:
    case TxoutType::WITNESS_V1_TAPROOT:
        return true;
    default:
        return false;
    }
}

namespace {
/** The CHN destinations an operation names, if any. */
const CScript* ChnTo(const Operation& op)
{
    if (const auto* swap{std::get_if<Swap>(&op)}) return &swap->chn_to;
    if (const auto* remove{std::get_if<RemoveLiquidity>(&op)}) return &remove->chn_to;
    if (const auto* bid{std::get_if<Bid>(&op)}) return &bid->chn_to;
    if (const auto* collect{std::get_if<Collect>(&op)}) return &collect->chn_to;
    if (const auto* route{std::get_if<Route>(&op)}) return &route->chn_to;
    if (const auto* withdraw{std::get_if<PoolWithdraw>(&op)}) return &withdraw->chn_to;
    if (const auto* swap{std::get_if<SwapPools>(&op)}) return &swap->chn_to;
    if (const auto* launch{std::get_if<LaunchToken>(&op)}) return &launch->chn_to;
    if (const auto* open{std::get_if<OpenPosition>(&op)}) return &open->chn_to;
    if (const auto* change{std::get_if<ChangePosition>(&op)}) return &change->chn_to;
    if (const auto* settle{std::get_if<SettleOrder>(&op)}) return &settle->chn_to;
    return nullptr;
}
} // namespace

bool IsCurveOperation(const Operation& op)
{
    return std::holds_alternative<CreatePool>(op) || std::holds_alternative<PoolDeposit>(op) || std::holds_alternative<PoolWithdraw>(op) ||
           std::holds_alternative<SwapPools>(op) || std::holds_alternative<LaunchToken>(op) || std::holds_alternative<ClaimLaunch>(op) ||
           std::holds_alternative<OpenPosition>(op) || std::holds_alternative<ChangePosition>(op) || std::holds_alternative<PlaceOrder>(op) ||
           std::holds_alternative<SettleOrder>(op);
}

const Register* RegisterOf(const Operation& op)
{
    if (const auto* reg{std::get_if<Register>(&op)}) return reg;
    if (const auto* launch{std::get_if<LaunchToken>(&op)}) return &launch->reg;
    return nullptr;
}

bool IsDexOperation(const Operation& op)
{
    return std::holds_alternative<Route>(op) || std::holds_alternative<RegisterOracle>(op) || std::holds_alternative<Attest>(op) ||
           std::holds_alternative<CreateOracleContract>(op) || std::holds_alternative<SettleOracleContract>(op) || IsCurveOperation(op);
}

bool CheckCurvesActivation(const CTransaction& tx, bool active, std::string& reject_reason)
{
    if (active) return true;
    const auto marker{GetMarker(tx)};
    if (!marker) return true;
    // No marker of their kinds, nor one that names a position (a token kind they brought), was valid before.
    bool position{false};
    for (const MarkerOutput& out : marker->outputs) position |= out.token && (out.token->kind == Token::Kind::POSITION || out.token->kind == Token::Kind::ORDER);
    if ((marker->operation && IsCurveOperation(*marker->operation)) || position) {
        reject_reason = "bad-ba-marker";
        return false;
    }
    return true;
}

bool CheckMarker(const CTransaction& tx, std::string& reject_reason)
{
    const auto invalid{[&](const char* reason) {
        reject_reason = reason;
        return false;
    }};
    std::string error;
    const auto marker{GetMarker(tx, &error)};
    if (!error.empty()) return invalid(error.c_str());
    if (!marker) return true;
    if (tx.IsCoinBase()) return invalid("bad-ba-coinbase");
    size_t marker_index{0};
    for (size_t i{0}; i < tx.vout.size(); ++i) {
        if (IsMarkerScript(tx.vout[i].scriptPubKey)) marker_index = i;
    }
    if (!MoneyRange(tx.vout[marker_index].nValue)) return invalid("bad-ba-marker-value");
    for (size_t i{0}; i < marker->outputs.size(); ++i) {
        const MarkerOutput& out{marker->outputs[i]};
        if (out.n >= tx.vout.size() || out.n == marker_index || (i > 0 && out.n <= marker->outputs[i - 1].n)) return invalid("bad-ba-outputs");
        // A token is carried by an output of no value that someone can spend.
        if (tx.vout[out.n].nValue != 0 || !Spendable(tx.vout[out.n].scriptPubKey)) return invalid("bad-ba-token-output");
        if (out.token) {
            if (out.token->amount == 0 || out.token->amount > MAX_AMOUNT) return invalid("bad-ba-amount");
            if (!Token::Divisible(out.token->kind) && out.token->amount != 1) return invalid("bad-ba-amount");
            // Only reservations and receipts made by the transaction itself are named by a null id.
            const bool may_be_new{out.token->kind == Token::Kind::RESERVATION || out.token->kind == Token::Kind::RECEIPT};
            if (out.token->id.IsNull() && !may_be_new) return invalid("bad-ba-token-id");
        }
    }
    // The CHN the marker burns go into a pool or an auction: only an operation that puts CHN in may carry any.
    if (!marker->operation) {
        if (tx.vout[marker_index].nValue != 0) return invalid("bad-ba-marker-value");
        return true;
    }
    const Operation& op{*marker->operation};
    if (const CScript* chn_to{ChnTo(op)}; chn_to && !chn_to->empty() && !Spendable(*chn_to)) return invalid("bad-ba-chn-to");
    const auto positive{[](uint64_t amount) { return amount > 0 && amount <= MAX_AMOUNT; }};
    if (const auto* reg{RegisterOf(op)}) {
        if (reg->supply > MAX_AMOUNT || reg->decimals > MAX_DECIMALS) return invalid("bad-ba-supply");
        if (const auto* launch{std::get_if<LaunchToken>(&op)}) {
            // A launch's supply and decimals are the rules' (LAUNCH_SUPPLY, LAUNCH_DECIMALS); its name is public.
            if (reg->supply != 0 || reg->decimals != LAUNCH_DECIMALS || !reg->text) return invalid("bad-ba-launch");
            if (!Spendable(launch->creator) || launch->allocation > MAX_LAUNCH_ALLOCATION) return invalid("bad-ba-launch");
            if (launch->min_buy > MAX_AMOUNT || launch->max_buy > MAX_AMOUNT || (launch->max_buy != 0 && launch->max_buy < launch->min_buy)) return invalid("bad-ba-launch");
            if (launch->first_buy > MAX_AMOUNT || launch->first_min_out > MAX_AMOUNT || (launch->first_buy == 0 && launch->first_min_out != 0)) return invalid("bad-ba-launch");
            if (launch->first_buy > 0 && launch->chn_to.empty()) return invalid("bad-ba-chn-to");
        }
        if (!GoodData(reg->data)) return invalid("bad-ba-data");
        if (reg->text && (!IsAssetName(*reg->text) || HashName(*reg->text) != reg->name)) return invalid("bad-ba-name-text");
        if (reg->name.IsNull()) return invalid("bad-ba-name");
        if (!reg->logo.empty() && !CheckLogo(reg->logo, reject_reason)) return false;
        if (!reg->display_name.empty() && !IsDisplayName(reg->display_name)) return invalid("bad-ba-display-name");
    } else if (const auto* named{std::get_if<SetDisplayName>(&op)}) {
        if (named->asset.IsNull()) return invalid("bad-ba-display-name-asset");
        // Empty: the display name is removed.
        if (!named->display_name.empty() && !IsDisplayName(named->display_name)) return invalid("bad-ba-display-name");
    } else if (const auto* set{std::get_if<SetLogo>(&op)}) {
        if (set->asset.IsNull()) return invalid("bad-ba-logo-asset");
        // Empty: the logo is removed.
        if (!set->logo.empty() && !CheckLogo(set->logo, reject_reason)) return false;
    } else if (const auto* mint{std::get_if<Mint>(&op)}) {
        if (!positive(mint->amount) || mint->asset.IsNull()) return invalid("bad-ba-amount");
    } else if (const auto* update{std::get_if<UpdateAsset>(&op)}) {
        if (update->asset.IsNull() || update->updates.Empty() || !GoodUpdates(update->updates)) return invalid("bad-ba-data");
    } else if (const auto* burn{std::get_if<Burn>(&op)}) {
        if (burn->tokens.empty() || burn->tokens.size() > MAX_BURNS) return invalid("bad-ba-burn");
        for (const Token& token : burn->tokens) {
            if (token.kind == Token::Kind::LP || token.kind == Token::Kind::RECEIPT || token.kind == Token::Kind::POSITION || token.kind == Token::Kind::ORDER || token.id.IsNull()) return invalid("bad-ba-burn");
            if (token.amount == 0 || token.amount > MAX_AMOUNT || (!Token::Divisible(token.kind) && token.amount != 1)) return invalid("bad-ba-burn");
        }
    } else if (const auto* swap{std::get_if<Swap>(&op)}) {
        if (swap->asset_in == swap->asset_out || !positive(swap->amount_in) || swap->min_out > MAX_AMOUNT) return invalid("bad-ba-swap");
    } else if (const auto* add{std::get_if<AddLiquidity>(&op)}) {
        if (add->asset_a == add->asset_b || !positive(add->amount_a) || !positive(add->amount_b) || add->min_shares > MAX_AMOUNT) return invalid("bad-ba-liquidity");
    } else if (const auto* remove{std::get_if<RemoveLiquidity>(&op)}) {
        if (remove->asset_a == remove->asset_b || !positive(remove->shares) || remove->min_a > MAX_AMOUNT || remove->min_b > MAX_AMOUNT) return invalid("bad-ba-liquidity");
    } else if (const auto* create{std::get_if<CreateAuction>(&op)}) {
        if (create->base == create->quote || !positive(create->base_amount)) return invalid("bad-ba-auction");
        if (!positive(create->end_price) || create->start_price < create->end_price || create->start_price > MAX_AMOUNT) return invalid("bad-ba-auction-price");
        if (create->duration < 1 || create->duration > MAX_AUCTION_DURATION || create->start_height < 0) return invalid("bad-ba-auction-duration");
    } else if (const auto* bid{std::get_if<Bid>(&op)}) {
        if (!positive(bid->quote_amount) || bid->min_base > MAX_AMOUNT) return invalid("bad-ba-bid");
    } else if (const auto* route{std::get_if<Route>(&op)}) {
        if (route->path.size() < 2 || route->path.size() > MAX_ROUTE_ASSETS || !positive(route->amount_in) || route->min_out > MAX_AMOUNT) return invalid("bad-ba-route");
        // All different: no pool twice, no cycle.
        const std::set<AssetId> distinct{route->path.begin(), route->path.end()};
        if (distinct.size() != route->path.size()) return invalid("bad-ba-route");
    } else if (const auto* create{std::get_if<CreatePool>(&op)}) {
        if (create->asset_a == create->asset_b || !positive(create->amount_a) || !positive(create->amount_b) || create->min_shares > MAX_AMOUNT) return invalid("bad-ba-liquidity");
        if (WhyNotPoolKind(create->curve, create->fee, create->amp, create->weight_a, create->flags)) return invalid("bad-ba-pool-kind");
    } else if (const auto* deposit{std::get_if<PoolDeposit>(&op)}) {
        if (deposit->amount0 > MAX_AMOUNT || deposit->amount1 > MAX_AMOUNT || (deposit->amount0 == 0 && deposit->amount1 == 0) || deposit->min_shares > MAX_AMOUNT) {
            return invalid("bad-ba-liquidity");
        }
    } else if (const auto* withdraw{std::get_if<PoolWithdraw>(&op)}) {
        if (!positive(withdraw->shares) || withdraw->only > 2 || withdraw->min0 > MAX_AMOUNT || withdraw->min1 > MAX_AMOUNT) return invalid("bad-ba-liquidity");
    } else if (const auto* claim{std::get_if<ClaimLaunch>(&op)}) {
        if (claim->token.IsNull()) return invalid("bad-ba-launch");
    } else if (const auto* open{std::get_if<OpenPosition>(&op)}) {
        if (open->Liquidity() == 0 || open->tick_lower >= open->tick_upper || open->tick_lower < cl::MIN_TICK || open->tick_upper > cl::MAX_TICK) return invalid("bad-ba-position");
        if (open->max0 > MAX_AMOUNT || open->max1 > MAX_AMOUNT) return invalid("bad-ba-position");
    } else if (const auto* place{std::get_if<PlaceOrder>(&op)}) {
        if (place->asset_sell == place->asset_buy || !positive(place->amount) || place->price_num == 0 || place->price_den == 0 || place->price_num > MAX_AMOUNT ||
            place->price_den > MAX_AMOUNT) {
            return invalid("bad-ba-order");
        }
    } else if (const auto* change{std::get_if<ChangePosition>(&op)}) {
        if (change->limit0 > MAX_AMOUNT || change->limit1 > MAX_AMOUNT) return invalid("bad-ba-position");
        if ((change->add && change->close) || (change->Liquidity() == 0 && !change->collect && !change->close)) return invalid("bad-ba-position");
    } else if (const auto* swap_pools{std::get_if<SwapPools>(&op)}) {
        if (swap_pools->pools.empty() || swap_pools->pools.size() + 1 > MAX_ROUTE_ASSETS || !positive(swap_pools->amount_in) || swap_pools->min_out > MAX_AMOUNT) {
            return invalid("bad-ba-route");
        }
    } else if (const auto* oracle{std::get_if<RegisterOracle>(&op)}) {
        if (!dex::IsOracleName(oracle->name) || !XOnlyPubKey{oracle->pubkey}.IsFullyValid()) return invalid("bad-ba-oracle");
    } else if (const auto* contract{std::get_if<CreateOracleContract>(&op)}) {
        if (!positive(contract->amount) || contract->outcomes.empty() || contract->outcomes.size() > MAX_CONTRACT_OUTCOMES) return invalid("bad-ba-contract");
        if (!Spendable(contract->refund)) return invalid("bad-ba-contract-script");
        for (const OracleOutcome& outcome : contract->outcomes) {
            if (outcome.lo > outcome.hi) return invalid("bad-ba-contract");
            if (!Spendable(outcome.script)) return invalid("bad-ba-contract-script");
        }
    }
    return true;
}

std::set<uint32_t> TokenOutputs(const CTransaction& tx)
{
    std::set<uint32_t> outputs;
    const auto marker{GetMarker(tx)};
    if (!marker) return outputs;
    for (const MarkerOutput& out : marker->outputs) outputs.insert(out.n);
    return outputs;
}

bool IsLogoOperation(const Operation& op)
{
    if (std::holds_alternative<SetLogo>(op)) return true;
    const auto* reg{RegisterOf(op)};
    return reg && !reg->logo.empty();
}

bool CheckLogoActivation(const CTransaction& tx, bool active, std::string& reject_reason)
{
    if (active) return true;
    const auto marker{GetMarker(tx)};
    if (marker && marker->operation && IsLogoOperation(*marker->operation)) {
        reject_reason = "bad-ba-logo-inactive";
        return false;
    }
    return true;
}

bool IsDisplayNameOperation(const Operation& op)
{
    if (std::holds_alternative<SetDisplayName>(op)) return true;
    const auto* reg{RegisterOf(op)};
    return reg && !reg->display_name.empty();
}

bool CheckTickerActivation(const CTransaction& tx, bool active, std::string& reject_reason)
{
    const auto marker{GetMarker(tx)};
    if (!marker || !marker->operation) return true;
    if (!active) {
        if (IsDisplayNameOperation(*marker->operation)) {
            reject_reason = "bad-ba-display-name-inactive";
            return false;
        }
        return true;
    }
    if (const auto* reg{RegisterOf(*marker->operation)}) {
        // The ticker is the name: public, so that everyone sees what was taken.
        if (!reg->text) {
            reject_reason = "bad-ba-ticker-private";
            return false;
        }
        if (!IsTicker(*reg->text)) {
            reject_reason = "bad-ba-ticker";
            return false;
        }
    }
    return true;
}

bool CheckDependsOnHeight(const CTransaction& tx)
{
    const auto marker{GetMarker(tx)};
    if (!marker || !marker->operation) return false;
    // Fail closed: only the operations whose checks read no height (the heights they write into
    // records do not decide their validity) are left to the keys they read. Every other one, and any
    // operation added later, is checked again at every change of the tip: an auction made to start at
    // a height, a bid (its auction ends), a registration (its reveal depth, which a reorganization can
    // take back), a collection (its auction collectable from a height).
    const Operation& op{*marker->operation};
    const bool independent{std::holds_alternative<Reserve>(op) || std::holds_alternative<Mint>(op) || std::holds_alternative<UpdateAsset>(op) ||
                           std::holds_alternative<Burn>(op) || std::holds_alternative<Swap>(op) || std::holds_alternative<AddLiquidity>(op) ||
                           std::holds_alternative<RemoveLiquidity>(op) || std::holds_alternative<ReleaseAsset>(op) ||
                           // The DEX's (only from its height: the mempool checks every asset transaction again
                           // when the next block's rules change, RemoveStaleDrivechainTxs). An attestation signed for a
                           // height stays valid at every later one; a settlement's winner, once the next block can pay
                           // it, is fixed (an attestation after the expiry does not count).
                           std::holds_alternative<Route>(op) || std::holds_alternative<RegisterOracle>(op) || std::holds_alternative<Attest>(op) ||
                           std::holds_alternative<SettleOracleContract>(op) ||
                           // The pool kinds' read pools and their kinds, no height (a launch reveals a reservation: its depth).
                           (IsCurveOperation(op) && !std::holds_alternative<LaunchToken>(op))};
    return !independent;
}

struct State::Plan {
    std::vector<COutPoint> spent;
    std::vector<std::pair<COutPoint, Token>> created;
    std::vector<std::pair<AssetId, AssetRecord>> assets;
    //! Entries of the history of assets' data, added (keys and values).
    std::vector<std::pair<sidechain::StoreBytes, sidechain::StoreBytes>> history;
    std::optional<std::pair<Txid, uint256>> reservation_made;
    ReservationOrigin reservation_origin;
    std::vector<Txid> reservations_gone;
    std::optional<std::pair<uint32_t, AssetId>> seq;
    std::optional<std::pair<uint256, Pool>> pool;
    //! The kind of a pool CreatePool makes (not the default).
    std::optional<std::pair<uint256, PoolParams>> pool_params;
    //! The work of the pool kinds it adds to its block (MAX_BLOCK_CURVE_WORK).
    uint32_t curve_work{0};
    //! A launch made, or a claim's change of one.
    std::optional<std::pair<AssetId, LaunchRecord>> launch;
    bool launch_created{false};
    //! Concentrated liquidity: a pool's state, ticks written (nullopt: cleared), a position written (nullopt: closed).
    std::optional<std::pair<uint256, ClState>> cl_state;
    std::vector<std::pair<int32_t, std::optional<ClTick>>> cl_ticks;
    std::optional<std::pair<uint256, std::optional<ClPosition>>> position;
    //! An order placed or changed (nullopt: gone), and whether its book's resting orders cross at the block's end.
    std::optional<std::pair<uint256, std::optional<BookOrder>>> book_order;
    //! The worst order of a full side, out of its book for a better one.
    std::optional<std::pair<uint256, BookOrder>> evicted;
    //! Expired entries of a full side, taken out of its index by the order placed there.
    std::vector<uint256> swept;
    std::optional<std::pair<Txid, Auction>> auction;
    std::vector<CTxOut> payouts;
    std::vector<Result> results;
    //! Retiring an asset: the pools that go, and the CHN freed for mainchain miners.
    std::vector<uint256> pools_gone;
    std::optional<AssetId> asset_gone;
    CAmount released{0};
    //! An AddLiquidity: its two results (the shares, then what goes back) go to its two result
    //! outputs, the second carrying nothing if nothing, or CHN, goes back.
    bool returns{false};
    //! Whether the transaction is one of the assets (has a marker or spends tokens): it writes the layout.
    bool assets_tx{false};
    //! A Bid in an asset with an output for what comes back of it: after the other results, carrying
    //! that (nothing if nothing comes back).
    std::optional<Result> back;
    //! The DEX (dex.h): an order of the block's batch (its result output and payout filled in later).
    std::optional<Order> order;
    std::optional<std::pair<uint256, OracleRecord>> oracle;
    std::optional<std::pair<std::pair<uint256, uint256>, Attestation>> attestation;
    std::optional<std::pair<Txid, OracleContract>> contract;
    //! A settlement in an asset: the script its result output must have.
    std::optional<CScript> result_script;
    //! The logo an asset gets (empty bytes: none any more), and its format.
    struct Logo {
        AssetId asset;
        std::vector<unsigned char> bytes;
        LogoFormat format{LogoFormat::PNG};
    };
    std::optional<Logo> logo;
    //! The display name an asset gets (empty: none any more).
    std::optional<std::pair<AssetId, std::string>> display_name;
};

namespace {
const sidechain::Table<COutPoint, Token> TOKENS{0x30};
const sidechain::Table<uint256, AssetRecord> ASSETS{0x31};
const sidechain::Table<uint256, uint256> RESERVATIONS{0x32};
const sidechain::Table<uint32_t, uint256> SEQ{0x33};
const sidechain::Table<uint256, Pool> POOLS{0x34};
const sidechain::Table<uint256, Auction> AUCTIONS{0x35};
const sidechain::Cell<uint32_t> NEXT_SEQ{0x37, 0};
constexpr uint8_t TOKENS_BY_ID{0x39};
constexpr uint8_t POOLS_BY_ASSET{0x3a};
constexpr uint8_t AUCTIONS_BY_ASSET{0x3b};
const sidechain::Table<uint256, ReservationOrigin> ORIGINS{0x3d};
constexpr uint8_t HISTORY{0x3e};
const sidechain::Table<uint256, LogoRecord> LOGOS{0x90};
const sidechain::Table<uint256, std::vector<unsigned char>> LOGO_BYTES{0x91};
const sidechain::Table<uint256, DisplayNameRecord> DISPLAY_NAMES{0x92};
//! Missing: the layout before it was written down (1).
const sidechain::Cell<uint32_t> LAYOUT{0x3f, 1};

// The DEX's (dex.h), from SidechainParams::dex_height: none before.
const sidechain::Table<uint256, PoolAccumulator> ACCUMULATORS{0x80};
//! By pool, then place in the ring (uint32).
constexpr uint8_t OBSERVATIONS{0x81};
//! During a block only.
const sidechain::Table<uint256, PoolSnapshot> SNAPSHOTS{0x82};
const sidechain::Table<uint32_t, Order> ORDERS{0x83};
const sidechain::Cell<uint32_t> ORDER_COUNT{0x84, 0};
const sidechain::Table<COutPoint, uint8_t> PENDING_RESULTS{0x85};
const sidechain::Table<uint256, OracleRecord> ORACLES{0x86};
//! By oracle, then event.
constexpr uint8_t ATTESTATIONS{0x87};
const sidechain::Table<uint256, OracleContract> CONTRACTS{0x88};
//! Contracts not settled, by the asset they hold (CHN not indexed), then id.
constexpr uint8_t CONTRACTS_BY_ASSET{0x89};
// The DEX's pool kinds (curves.h), from SidechainParams::dex_curves_height: none before.
const sidechain::Table<uint256, PoolParams> POOL_PARAMS{0x8a};
//! During a block only: the work of the pool kinds its transactions added (MAX_BLOCK_CURVE_WORK).
const sidechain::Cell<uint32_t> CURVE_WORK{0x8b, 0};
//! Launches (LaunchToken), by token.
const sidechain::Table<uint256, LaunchRecord> LAUNCHES{0x8c};
//! What each block did on the launches' curves: by height (big-endian), then token (LaunchLogEntry); the last LAUNCH_LOG_BLOCKS.
constexpr uint8_t LAUNCH_LOG{0x8d};
//! Launches by the height that made them, then token.
constexpr uint8_t LAUNCHES_BY_HEIGHT{0x8e};
const sidechain::TableId LAUNCH_INDEXES_TAKEN[]{sidechain::TableId{LAUNCH_LOG}, sidechain::TableId{LAUNCHES_BY_HEIGHT}};
// Concentrated liquidity (clmath.h).
const sidechain::Table<uint256, ClState> CL_STATES{0x93};
//! Ticks with liquidity: by pool, then the tick in order (CL_TICKS) and in reverse order (CL_TICKS_DOWN, empty values).
constexpr uint8_t CL_TICKS{0x94};
constexpr uint8_t CL_TICKS_DOWN{0x97};
const sidechain::Table<uint256, ClPosition> CL_POSITIONS{0x95};
//! During a block only: the square root price a concentrated pool had before the block (the price accumulators).
const sidechain::Table<uint256, uint256> CL_SNAPSHOTS{0x96};
const sidechain::TableId CL_INDEXES_TAKEN[]{sidechain::TableId{CL_TICKS}, sidechain::TableId{CL_TICKS_DOWN}};
// Order books (Curve::BOOK).
const sidechain::Table<uint256, BookOrder> BOOK_ORDERS{0x98};
//! Open orders by book, side (0: asks, sell asset0; 1: bids), price (their limit, UQ128.128 big-endian: best first), then id.
constexpr uint8_t BOOK_INDEX{0x99};
//! During a block only: the books a block placed orders in (their resting orders cross at its end).
const sidechain::Table<uint256, uint8_t> BOOKS_TOUCHED{0x9a};
//! Orders by expiry (big-endian), then id: the end of each block takes those expired out of their books (SweepBooks).
constexpr uint8_t BOOK_EXPIRY{0x9b};
//! The entries of each side of each book's index (book, side).
constexpr uint8_t BOOK_COUNTS{0x9c};
//! Written with the first order: a store with orders and without it was made before the books' counts (StoreLayoutCurrent).
const sidechain::Cell<uint32_t> BOOKS_LAYOUT{0x9d, 0};
constexpr uint32_t BOOKS_LAYOUT_CURRENT{1};
const sidechain::TableId BOOK_INDEXES_TAKEN[]{sidechain::TableId{BOOK_INDEX}, sidechain::TableId{BOOK_EXPIRY}, sidechain::TableId{BOOK_COUNTS}};
sidechain::StoreBytes CountKey(const uint256& book, bool asks)
{
    sidechain::StoreBytes key{BOOK_COUNTS};
    sidechain::KeyCodec<uint256>::Encode(key, book);
    key.push_back(asks ? 0 : 1);
    return key;
}
uint32_t ReadCount(const sidechain::StoreView& view, const uint256& book, bool asks)
{
    const auto bytes{view.Get(CountKey(book, asks))};
    return bytes ? sidechain::DecodeValue<uint32_t>(*bytes) : 0;
}
/** One more or one less live order on a side (never below 0). */
void AddCount(sidechain::StoreOverlay& out, const uint256& book, bool asks, int delta)
{
    const uint32_t now{ReadCount(out, book, asks)};
    const uint32_t next{delta < 0 ? (now > 0 ? now - 1 : 0) : now + 1};
    if (next == 0) {
        out.Erase(CountKey(book, asks));
    } else {
        out.Put(CountKey(book, asks), sidechain::EncodeValue(next));
    }
}
sidechain::StoreBytes ExpiryKey(const uint256& id, int32_t expiry)
{
    sidechain::StoreBytes key{BOOK_EXPIRY};
    const uint32_t e{static_cast<uint32_t>(std::max(expiry, 0))};
    for (int shift{24}; shift >= 0; shift -= 8) key.push_back(static_cast<uint8_t>(e >> shift));
    sidechain::KeyCodec<uint256>::Encode(key, id);
    return key;
}
/** An order's place in its book's index. */
sidechain::StoreBytes BookKey(const uint256& id, const BookOrder& order)
{
    sidechain::StoreBytes key{BOOK_INDEX};
    sidechain::KeyCodec<uint256>::Encode(key, order.book);
    key.push_back(order.sells0 ? 0 : 1);
    // num/den in UQ128.128 (at most 2^63·2^128: it fits in 256 bits), big-endian.
    const arith_uint256 price{(arith_uint256{order.price_num} << 128) / arith_uint256{order.price_den}};
    for (int byte{31}; byte >= 0; --byte) key.push_back(static_cast<uint8_t>((price >> (8 * byte)).GetLow64() & 0xff));
    sidechain::KeyCodec<uint256>::Encode(key, id);
    return key;
}
//! Dynamic fees: the volatility of each pool that has one (FeeState).
const sidechain::Table<uint256, FeeState> FEE_STATES{0x8f};

/** A tick's place in key order: tick + 2^31, big-endian (down: the order reversed). */
sidechain::StoreBytes TickKey(uint8_t table, const uint256& pool, int32_t tick)
{
    sidechain::StoreBytes key{table};
    sidechain::KeyCodec<uint256>::Encode(key, pool);
    const uint32_t ordered{table == CL_TICKS_DOWN ? 0x7fffffffU - static_cast<uint32_t>(tick) : static_cast<uint32_t>(tick) ^ 0x80000000U};
    for (int shift{24}; shift >= 0; shift -= 8) key.push_back(static_cast<uint8_t>(ordered >> shift));
    return key;
}
int32_t TickOfKey(uint8_t table, std::span<const unsigned char> key)
{
    uint32_t ordered{0};
    for (size_t i{key.size() - 4}; i < key.size(); ++i) ordered = (ordered << 8) | key[i];
    return table == CL_TICKS_DOWN ? static_cast<int32_t>(0x7fffffffU - ordered) : static_cast<int32_t>(ordered ^ 0x80000000U);
}

/**
 * The most liquidity a tick may carry in all (Uniswap v3's maxLiquidityPerTick): 2^128 − 1 shared by every tick of the
 * spacing, so that the liquidity in range, a sum over ticks, never overflows 128 bits.
 */
unsigned __int128 MaxLiquidityPerTick(int32_t spacing)
{
    const int64_t lo{static_cast<int64_t>(-887272) / spacing * spacing}, hi{static_cast<int64_t>(887272) / spacing * spacing};
    const uint64_t ticks{static_cast<uint64_t>((hi - lo) / spacing + 1)};
    return (~static_cast<unsigned __int128>(0)) / ticks;
}
/** The liquidity in range after crossing a tick of net `net` (up: + net; down: − net); nullopt if it would go below 0 or past 128 bits. */
std::optional<unsigned __int128> CrossLiquidity(unsigned __int128 liquidity, __int128 net, bool down)
{
    const __int128 delta{down ? -net : net};
    if (delta < 0) {
        const unsigned __int128 take{static_cast<unsigned __int128>(-delta)};
        if (take > liquidity) return std::nullopt;
        return liquidity - take;
    }
    const unsigned __int128 add{static_cast<unsigned __int128>(delta)};
    if (add > ~static_cast<unsigned __int128>(0) - liquidity) return std::nullopt;
    return liquidity + add;
}

/** Dynamic fees: how the volatility of the blocks since the last that traded decays, and what it adds to the fee. */
constexpr uint64_t DYNAMIC_WINDOW{8};
constexpr int32_t DYNAMIC_HALF_LIFE{40};
constexpr uint32_t DYNAMIC_MAX_EXTRA{20'000};
uint64_t Decayed(const FeeState& fee, int height)
{
    const int32_t gap{height - fee.last_height};
    if (gap <= 1) return fee.volatility;
    const int halvings{std::min<int32_t>((gap - 1) / DYNAMIC_HALF_LIFE, 63)};
    return fee.volatility >> halvings;
}
/** Uniswap v3's fee growth inside a range of ticks (modulo 2^256). */
std::pair<arith_uint256, arith_uint256> FeeGrowthInside(const ClState& cl, int32_t lower, int32_t upper, const ClTick& lo, const ClTick& hi)
{
    const arith_uint256 g0{UintToArith256(cl.fee_growth0)}, g1{UintToArith256(cl.fee_growth1)};
    const arith_uint256 lo0{UintToArith256(lo.outside0)}, lo1{UintToArith256(lo.outside1)}, hi0{UintToArith256(hi.outside0)}, hi1{UintToArith256(hi.outside1)};
    const arith_uint256 below0{cl.tick >= lower ? lo0 : arith_uint256{g0 - lo0}}, below1{cl.tick >= lower ? lo1 : arith_uint256{g1 - lo1}};
    const arith_uint256 above0{cl.tick < upper ? hi0 : arith_uint256{g0 - hi0}}, above1{cl.tick < upper ? hi1 : arith_uint256{g1 - hi1}};
    return {g0 - below0 - above0, g1 - below1 - above1};
}
/** What a position's liquidity earned between two readings of the fee growth inside: ⌊Δ·L / 2^128⌋. */
uint64_t Earned(const arith_uint256& now, const uint256& last, unsigned __int128 liquidity)
{
    const curves::BigUint e{(curves::BigUint::FromArith(now - UintToArith256(last)) * curves::BigUint::U128(liquidity)) >> 128};
    const auto v{e.ToU128()};
    return v && *v <= MAX_AMOUNT ? static_cast<uint64_t>(*v) : MAX_AMOUNT;
}


sidechain::StoreBytes HeightKey(uint8_t table, int32_t height, const uint256& token)
{
    sidechain::StoreBytes key{table};
    // Big-endian, so that the keys go in the order of the heights.
    const uint32_t h{static_cast<uint32_t>(height)};
    for (int shift{24}; shift >= 0; shift -= 8) key.push_back(static_cast<uint8_t>(h >> shift));
    sidechain::KeyCodec<uint256>::Encode(key, token);
    return key;
}
sidechain::StoreBytes HeightPrefix(uint8_t table, int32_t height)
{
    sidechain::StoreBytes key{table};
    const uint32_t h{static_cast<uint32_t>(height)};
    for (int shift{24}; shift >= 0; shift -= 8) key.push_back(static_cast<uint8_t>(h >> shift));
    return key;
}
//! The bytes of the DEX's indexes, taken (sidechain::DuplicateTableIds) as the tables' are.
const sidechain::TableId DEX_INDEXES_TAKEN[]{sidechain::TableId{OBSERVATIONS}, sidechain::TableId{ATTESTATIONS}, sidechain::TableId{CONTRACTS_BY_ASSET}};

sidechain::StoreBytes Key2(uint8_t table, const uint256& a, const uint256& b)
{
    sidechain::StoreBytes key{table};
    sidechain::KeyCodec<uint256>::Encode(key, a);
    sidechain::KeyCodec<uint256>::Encode(key, b);
    return key;
}
/** The outputs that carry tokens of an id and a kind, in outpoint order, under this. */
sidechain::StoreBytes TokenIdPrefix(const uint256& id, Token::Kind kind)
{
    sidechain::StoreBytes key{TOKENS_BY_ID};
    sidechain::KeyCodec<uint256>::Encode(key, id);
    key.push_back(static_cast<uint8_t>(kind));
    return key;
}
sidechain::StoreBytes TokenIdKey(const Token& token, const COutPoint& outpoint)
{
    sidechain::StoreBytes key{TokenIdPrefix(token.id, token.kind)};
    sidechain::KeyCodec<COutPoint>::Encode(key, outpoint);
    return key;
}
sidechain::StoreBytes HistoryPrefix(const AssetId& asset, DataField field)
{
    sidechain::StoreBytes key{HISTORY};
    sidechain::KeyCodec<uint256>::Encode(key, asset);
    key.push_back(static_cast<uint8_t>(field));
    return key;
}
sidechain::StoreBytes HistoryKey(const AssetId& asset, DataField field, uint32_t n)
{
    sidechain::StoreBytes key{HistoryPrefix(asset, field)};
    sidechain::KeyCodec<uint32_t>::Encode(key, n);
    return key;
}
sidechain::StoreBytes Prefix(uint8_t table, const uint256& head)
{
    sidechain::StoreBytes key{table};
    sidechain::KeyCodec<uint256>::Encode(key, head);
    return key;
}
sidechain::StoreBytes ObservationKey(const uint256& pool, uint32_t place)
{
    sidechain::StoreBytes key{Prefix(OBSERVATIONS, pool)};
    sidechain::KeyCodec<uint32_t>::Encode(key, place);
    return key;
}
} // namespace

sidechain::StoreOverlay& State::Writable() const { return *Assert(m_overlay); }
std::optional<Token> State::GetToken(const COutPoint& outpoint) const { return TOKENS.Get(*m_view, outpoint); }
std::optional<AssetRecord> State::GetAsset(const AssetId& asset) const { return ASSETS.Get(*m_view, asset); }
bool State::HasAsset(const AssetId& asset) const { return ASSETS.Contains(*m_view, asset); }
std::optional<LogoRecord> State::GetLogo(const AssetId& asset) const { return LOGOS.Get(*m_view, asset); }
std::optional<std::vector<unsigned char>> State::GetLogoBytes(const AssetId& asset) const { return LOGO_BYTES.Get(*m_view, asset); }
std::optional<DisplayNameRecord> State::GetDisplayName(const AssetId& asset) const { return DISPLAY_NAMES.Get(*m_view, asset); }

AssetHistory State::GetHistory(const AssetId& asset) const
{
    AssetHistory history;
    ForEachField([&](DataField field, auto, auto, auto history_field) {
        auto& entries{history.*history_field};
        using Entry = typename std::decay_t<decltype(entries)>::value_type;
        m_view->ForEach(HistoryPrefix(asset, field), [&](const sidechain::StoreBytes&, const sidechain::StoreBytes& value) {
            entries.push_back(sidechain::DecodeValue<Entry>(value));
            return true;
        });
    });
    return history;
}

std::optional<AssetData> State::DataAt(const AssetId& asset, int height) const
{
    const auto record{GetAsset(asset)};
    if (!record || height < record->height) return std::nullopt;
    return GetHistory(asset).AtHeight(record->height, height);
}

bool StoreLayoutCurrent(const sidechain::StoreView& view)
{
    // Orders without the books' layout: written before their counts (a development build): derived again.
    if (const sidechain::StoreBytes prefix{0x98}; view.Next(prefix, prefix) && BOOKS_LAYOUT.Get(view) != BOOKS_LAYOUT_CURRENT) return false;
    if (LAYOUT.Get(view) == STORE_LAYOUT) return true;
    // Written with the first change to the assets: a store without it has none, or has them in an earlier layout.
    for (uint8_t table{0x30}; table <= 0x3e; ++table) {
        const sidechain::StoreBytes prefix{table};
        if (view.Next(prefix, prefix)) return false;
    }
    return true;
}
std::optional<uint256> State::GetReservation(const Txid& id) const { return RESERVATIONS.Get(*m_view, id.ToUint256()); }
std::optional<ReservationOrigin> State::GetReservationOrigin(const Txid& id) const { return ORIGINS.Get(*m_view, id.ToUint256()); }
std::optional<Pool> State::GetPool(const uint256& id) const { return POOLS.Get(*m_view, id); }
std::optional<Auction> State::GetAuction(const Txid& id) const { return AUCTIONS.Get(*m_view, id.ToUint256()); }
uint32_t State::NextSeq() const { return NEXT_SEQ.Get(*m_view); }
std::optional<Pool> State::FindPool(const AssetId& a, const AssetId& b) const { return GetPool(PoolId(a, b)); }
void State::ForEachToken(const std::function<bool(const COutPoint&, const Token&)>& fn) const { TOKENS.ForEach(*m_view, fn); }
void State::ForEachAsset(const std::function<bool(const AssetId&, const AssetRecord&)>& fn) const { ASSETS.ForEach(*m_view, fn); }
void State::ForEachPool(const std::function<bool(const uint256&, const Pool&)>& fn) const { POOLS.ForEach(*m_view, fn); }
void State::ForEachAuction(const std::function<bool(const Txid&, const Auction&)>& fn) const
{
    AUCTIONS.ForEach(*m_view, [&](const uint256& id, const Auction& a) { return fn(Txid::FromUint256(id), a); });
}
void State::ForEachReservation(const std::function<bool(const Txid&, const uint256&)>& fn) const
{
    RESERVATIONS.ForEach(*m_view, [&](const uint256& id, const uint256& c) { return fn(Txid::FromUint256(id), c); });
}

void State::SetToken(const COutPoint& outpoint, const std::optional<Token>& token)
{
    sidechain::StoreOverlay& out{Writable()};
    if (const auto old{GetToken(outpoint)}) out.Erase(TokenIdKey(*old, outpoint));
    if (token) {
        TOKENS.Put(out, outpoint, *token);
        out.Put(TokenIdKey(*token, outpoint), {});
    } else {
        TOKENS.Erase(out, outpoint);
    }
}

void State::SetReservation(const Txid& id, const std::optional<uint256>& commitment)
{
    sidechain::StoreOverlay& out{Writable()};
    if (commitment) {
        RESERVATIONS.Put(out, id.ToUint256(), *commitment);
    } else {
        RESERVATIONS.Erase(out, id.ToUint256());
    }
}

void State::Count(const AssetId& asset, uint32_t AssetRecord::*count, int delta)
{
    if (delta == 0 || asset.IsNull()) return;
    auto record{GetAsset(asset)};
    if (!record) return;
    uint32_t& n{(*record).*count};
    // Kept in step with the pools and auctions, it never goes below zero nor wraps.
    n = delta < 0 ? n - std::min<uint32_t>(n, -delta) : n + delta;
    ASSETS.Put(Writable(), asset, *record);
}

void State::SetPool(const uint256& id, const std::optional<Pool>& pool)
{
    sidechain::StoreOverlay& out{Writable()};
    const auto old{GetPool(id)};
    // The DEX: the reserves before the block's first change, for the price accumulators (FinishBlock).
    if (m_dex && !SNAPSHOTS.Contains(out, id)) SNAPSHOTS.Put(out, id, PoolSnapshot{old ? old->reserve0 : 0, old ? old->reserve1 : 0});
    // The count of each asset's pools that have providers.
    const int provided{(pool && !amm::Abandoned(*pool) ? 1 : 0) - (old && !amm::Abandoned(*old) ? 1 : 0)};
    if (provided != 0) {
        const Pool& either{pool ? *pool : *old};
        Count(either.asset0, &AssetRecord::provided_pools, provided);
        Count(either.asset1, &AssetRecord::provided_pools, provided);
    }
    if (old) {
        out.Erase(Key2(POOLS_BY_ASSET, old->asset0, id));
        out.Erase(Key2(POOLS_BY_ASSET, old->asset1, id));
    }
    if (pool) {
        POOLS.Put(out, id, *pool);
        out.Put(Key2(POOLS_BY_ASSET, pool->asset0, id), {});
        out.Put(Key2(POOLS_BY_ASSET, pool->asset1, id), {});
    } else {
        POOLS.Erase(out, id);
    }
}

void State::SetAuction(const Txid& id, const std::optional<Auction>& auction)
{
    sidechain::StoreOverlay& out{Writable()};
    const auto old{GetAuction(id)};
    // The count of the auctions that hold some of each of its assets: of the asset the auction was
    // made with (one retired since, and its name registered again, is another asset).
    if (old || auction) {
        const Auction& either{auction ? *auction : *old};
        for (const auto& [asset, registration] : {std::pair{either.base, either.base_registration}, std::pair{either.quote, either.quote_registration}}) {
            const int delta{(auction && auction->Holds(asset) ? 1 : 0) - (old && old->Holds(asset) ? 1 : 0)};
            if (delta == 0) continue;
            if (const auto record{GetAsset(asset)}; record && record->registration == registration) Count(asset, &AssetRecord::holding_auctions, delta);
        }
    }
    // Only auctions not closed are indexed: the rules look for those alone.
    if (old && !old->closed) {
        out.Erase(Key2(AUCTIONS_BY_ASSET, old->base, id.ToUint256()));
        out.Erase(Key2(AUCTIONS_BY_ASSET, old->quote, id.ToUint256()));
    }
    if (auction) {
        AUCTIONS.Put(out, id.ToUint256(), *auction);
        if (!auction->closed) {
            out.Put(Key2(AUCTIONS_BY_ASSET, auction->base, id.ToUint256()), {});
            out.Put(Key2(AUCTIONS_BY_ASSET, auction->quote, id.ToUint256()), {});
        }
    } else {
        AUCTIONS.Erase(out, id.ToUint256());
    }
}

std::optional<COutPoint> State::FirstTokenOf(const uint256& id, Token::Kind kind) const
{
    // The index has the kind before the outpoint: the first entry under both is the one.
    const sidechain::StoreBytes prefix{TokenIdPrefix(id, kind)};
    const auto first{m_view->Next(prefix, prefix)};
    if (!first) return std::nullopt;
    std::span<const unsigned char> in{first->first};
    in = in.subspan(prefix.size());
    return sidechain::KeyCodec<COutPoint>::Decode(in);
}

std::vector<std::pair<uint256, Pool>> State::PoolsOf(const AssetId& asset) const
{
    std::vector<std::pair<uint256, Pool>> pools;
    m_view->ForEach(Prefix(POOLS_BY_ASSET, asset), [&](const sidechain::StoreBytes& key, const sidechain::StoreBytes&) {
        std::span<const unsigned char> in{key};
        in = in.subspan(33);
        const uint256 id{sidechain::KeyCodec<uint256>::Decode(in)};
        pools.emplace_back(id, *Assert(GetPool(id)));
        return true;
    });
    return pools;
}

std::vector<Txid> State::AuctionsOf(const AssetId& asset, size_t limit) const
{
    std::vector<Txid> auctions;
    m_view->ForEach(Prefix(AUCTIONS_BY_ASSET, asset), [&](const sidechain::StoreBytes& key, const sidechain::StoreBytes&) {
        if (auctions.size() >= limit) return false;
        std::span<const unsigned char> in{key};
        in = in.subspan(33);
        auctions.push_back(Txid::FromUint256(sidechain::KeyCodec<uint256>::Decode(in)));
        return true;
    });
    return auctions;
}

size_t State::PoolCount() const
{
    size_t n{0};
    ForEachPool([&](const uint256&, const Pool&) { ++n; return true; });
    return n;
}

std::vector<std::pair<uint32_t, Token>> State::SpentTokens(const CTransaction& tx) const
{
    std::vector<std::pair<uint32_t, Token>> spent;
    if (tx.IsCoinBase()) return spent;
    for (uint32_t i{0}; i < tx.vin.size(); ++i) {
        if (const auto token{GetToken(tx.vin[i].prevout)}) spent.emplace_back(i, *token);
    }
    return spent;
}

std::optional<AssetId> State::AssetOfSeq(uint32_t seq) const
{
    const auto asset{SEQ.Get(*m_view, seq)};
    if (!asset) return std::nullopt;
    // A retired asset keeps its number; the name may since belong to an asset with another number.
    // The number names the asset only while that asset has it.
    const auto record{GetAsset(*asset)};
    if (!record || record->seq != seq) return std::nullopt;
    return asset;
}

bool State::Releasable(const AssetId& asset, std::string* why) const
{
    const auto no{[&](const char* reason) {
        if (why) *why = reason;
        return false;
    }};
    const auto record{GetAsset(asset)};
    if (!record) return no("no such asset");
    if (!record->fixed) return no("its supply is not fixed: its control coin exists");
    // What carries it, through the index of outputs by what they carry: one look per kind.
    if (FirstTokenOf(asset, Token::Kind::ASSET)) return no("someone holds some of it");
    if (FirstTokenOf(asset, Token::Kind::CONTROL)) return no("its control coin exists");
    // An auction that sells something else for it and has taken none of it in holds none of it: it
    // does not keep it alive (else a 1-unit auction that never starts would, for ever). Bids into it
    // are refused once the asset is gone (MakePlan). The record counts the others (Auction::Holds):
    // one look, however many auctions quote it.
    if (record->holding_auctions > 0) return no("an auction of it is not collected");
    if (record->provided_pools > 0) return no("a pool of it has liquidity providers");
    // The DEX (none before its height): what an order of this block trades through, or a contract holds.
    if (OrderHolds(asset)) return no("an order of this block trades it");
    if (const sidechain::StoreBytes prefix{Prefix(CONTRACTS_BY_ASSET, asset)}; m_view->Next(prefix, prefix)) return no("an oracle contract holds some of it");
    return true;
}

bool State::OrderHolds(const AssetId& asset) const
{
    bool holds{false};
    ORDERS.ForEach(*m_view, [&](const uint32_t&, const Order& order) {
        holds = std::find(order.path.begin(), order.path.end(), asset) != order.path.end();
        return !holds;
    });
    return holds;
}

std::optional<State::Plan> State::MakePlan(const CTransaction& tx, int height, std::string& reject_reason, int reveal_depth) const
{
    const auto invalid{[&](const char* reason) -> std::optional<Plan> {
        reject_reason = reason;
        return std::nullopt;
    }};
    if (!CheckMarker(tx, reject_reason)) return std::nullopt;
    const auto marker{GetMarker(tx)};
    const auto spent{SpentTokens(tx)};
    if (!marker && spent.empty()) return Plan{};
    // Spending tokens without a marker would destroy them.
    if (!marker) return invalid("bad-ba-tokens-lost");

    const Txid txid{tx.GetHash()};
    const Operation* op{marker->operation ? &*marker->operation : nullptr};
    // The DEX's operations are not known before its height: their markers were never valid.
    if (op && IsDexOperation(*op) && !m_dex) return invalid("bad-ba-marker");
    CAmount chn_in{0};
    for (size_t i{0}; i < tx.vout.size(); ++i) {
        if (IsMarkerScript(tx.vout[i].scriptPubKey)) chn_in = tx.vout[i].nValue;
    }

    // The books: for each token, what comes in (spent, created) less what goes out (outputs, taken in). They must balance.
    using Key = std::pair<Token::Kind, uint256>;
    std::map<Key, __int128> books;
    const auto credit{[&](Token::Kind kind, const uint256& id, uint64_t amount) { books[{kind, id}] += amount; }};
    const auto debit{[&](Token::Kind kind, const uint256& id, uint64_t amount) { books[{kind, id}] -= amount; }};
    for (const auto& [n, token] : spent) credit(token.kind, token.id, token.amount);
    const auto spends{[&](Token::Kind kind, const uint256& id) {
        return std::any_of(spent.begin(), spent.end(), [&](const auto& s) { return s.second.kind == kind && s.second.id == id; });
    }};
    // A token named by a null id is the transaction's own reservation or receipt.
    std::vector<std::pair<uint32_t, Token>> explicit_outputs;
    size_t result_outputs{0};
    for (const MarkerOutput& out : marker->outputs) {
        if (!out.token) {
            ++result_outputs;
            continue;
        }
        Token token{*out.token};
        if (token.id.IsNull()) token.id = txid.ToUint256();
        debit(token.kind, token.id, token.amount);
        explicit_outputs.emplace_back(out.n, token);
    }

    Plan plan;
    plan.assets_tx = true;
    for (const auto& [n, token] : spent) plan.spent.push_back(tx.vin[n].prevout);
    // CHN the operation takes in, which the marker burns.
    uint64_t chn_taken{0};
    // What the operation takes in of an asset (CHN or a token).
    const auto take{[&](const AssetId& asset, uint64_t amount) {
        if (asset.IsNull()) {
            chn_taken += amount;
        } else {
            debit(Token::Kind::ASSET, asset, amount);
        }
    }};
    // Pools and auctions trade CHN and registered assets.
    const auto tradable{[&](const AssetId& asset) { return asset.IsNull() || HasAsset(asset); }};
    // The registration of an asset now (null for CHN, and for an asset not registered).
    const auto registration{[&](const AssetId& asset) {
        if (asset.IsNull()) return Txid{};
        const auto record{GetAsset(asset)};
        return record ? record->registration : Txid{};
    }};

    if (op && m_dex && (std::holds_alternative<Swap>(*op) || IsDexOperation(*op)) && !std::holds_alternative<LaunchToken>(*op)) {
        // The DEX: swaps and routes are orders of the block's batch; oracles and their contracts; pools of every kind.
        if (std::holds_alternative<OpenPosition>(*op) || std::holds_alternative<ChangePosition>(*op)) {
            if (!PlanPosition(tx, *op, height, plan, take, credit, debit, reject_reason)) return std::nullopt;
        } else if (IsCurveOperation(*op) && !std::holds_alternative<SwapPools>(*op)) {
            if (!PlanPools(tx, *op, height, plan, take, debit, reject_reason)) return std::nullopt;
        } else if (!PlanDex(tx, *op, height, plan, chn_taken, take, reject_reason)) {
            return std::nullopt;
        }
    } else if (const auto* reserve{op ? std::get_if<Reserve>(op) : nullptr}) {
        credit(Token::Kind::RESERVATION, txid.ToUint256(), 1);
        plan.reservation_made = std::make_pair(txid, reserve->commitment);
        // Where it is made: the output that carries it (the books below hold it to exactly one).
        plan.reservation_origin.height = height;
        for (const auto& [n, token] : explicit_outputs) {
            if (token.kind == Token::Kind::RESERVATION && token.id == txid.ToUint256()) plan.reservation_origin.script = tx.vout[n].scriptPubKey;
        }
    } else if (const auto* reg{op ? RegisterOf(*op) : nullptr}) {
        const auto* launch{std::get_if<LaunchToken>(op)};
        // Assets registered before, in this block included, are taken.
        if (HasAsset(reg->name)) return invalid("bad-ba-name-taken");
        // A name that reads as another asset (CHN, a number, "0x...") would be mistaken for it.
        if ((reg->text && ReadsAsAnotherAsset(*reg->text)) || IsChnName(reg->name)) return invalid("bad-ba-name-reserved");
        // A reservation is bound to the script of its output: nobody but its maker can have made it,
        // so it reveals the name whatever other reservations there are.
        std::optional<Txid> reservation;
        std::optional<ReservationOrigin> origin;
        for (const auto& [n, token] : spent) {
            if (token.kind != Token::Kind::RESERVATION) continue;
            const Txid id{Txid::FromUint256(token.id)};
            const auto found{GetReservation(id)};
            if (!found) continue;
            const auto made{GetReservationOrigin(id)};
            if (made && *found == ReservationCommitment(reg->name, reg->nonce, made->script)) {
                reservation = id;
                origin = made;
                break;
            }
        }
        if (!reservation) return invalid("bad-ba-no-reservation");
        // Only once the reservation is deep enough: whoever makes this block saw the name and the
        // nonce of every registration waiting for it, and could have reserved the name under a nonce
        // of its own, in this very block, to register it first.
        if (int64_t{height} - origin->height < reveal_depth) return invalid("bad-ba-reservation-too-young");
        debit(Token::Kind::RESERVATION, reservation->ToUint256(), 1);
        plan.reservations_gone.push_back(*reservation);
        // A launch's token has no control coin and its whole supply is the curve's (and its pool's at graduation).
        if (!launch) {
            credit(Token::Kind::CONTROL, reg->name, 1);
            if (reg->supply > 0) credit(Token::Kind::ASSET, reg->name, reg->supply);
        }
        AssetRecord record;
        record.seq = NextSeq();
        record.registration = txid;
        record.height = height;
        if (reg->text) record.text = *reg->text;
        record.decimals = reg->decimals;
        record.supply = record.minted = launch ? LAUNCH_SUPPLY : reg->supply;
        record.fixed = launch != nullptr;
        // Its pool with the coin: the curve's until it graduates (counted as the pools with providers are).
        if (launch) record.provided_pools = 0;
        record.data = reg->data;
        // Each field starts its history with its value at the registration, set or not.
        ForEachField([&](DataField field, auto data_field, auto, auto) {
            using T = typename std::decay_t<decltype(reg->data.*data_field)>::value_type;
            plan.history.emplace_back(HistoryKey(reg->name, field, 0), sidechain::EncodeValue(Stamped<T>{reg->data.*data_field, txid, height}));
            record.changes[static_cast<size_t>(field)] = 1;
        });
        plan.assets.emplace_back(reg->name, std::move(record));
        plan.seq = std::make_pair(NextSeq(), reg->name);
        if (!reg->logo.empty()) {
            std::string why;
            plan.logo = Plan::Logo{reg->name, reg->logo, *Assert(CheckLogo(reg->logo, why))};
        }
        if (!reg->display_name.empty()) plan.display_name = std::make_pair(reg->name, reg->display_name);
        if (launch && !PlanLaunch(tx, *launch, height, plan, take, reject_reason)) return std::nullopt;
    } else if (const auto* mint{op ? std::get_if<Mint>(op) : nullptr}) {
        const auto found{GetAsset(mint->asset)};
        if (!found) return invalid("bad-ba-asset-unknown");
        if (!spends(Token::Kind::CONTROL, mint->asset)) return invalid("bad-ba-no-control");
        AssetRecord record{*found};
        const auto supply{Add(record.supply, mint->amount)};
        const auto minted{Add(record.minted, mint->amount)};
        if (!supply || !minted) return invalid("bad-ba-supply");
        record.supply = *supply;
        record.minted = *minted;
        credit(Token::Kind::ASSET, mint->asset, mint->amount);
        plan.assets.emplace_back(mint->asset, std::move(record));
    } else if (const auto* update{op ? std::get_if<UpdateAsset>(op) : nullptr}) {
        const auto found{GetAsset(update->asset)};
        if (!found) return invalid("bad-ba-asset-unknown");
        // The control coin of this asset, not of any.
        if (!spends(Token::Kind::CONTROL, update->asset)) return invalid("bad-ba-no-control");
        AssetRecord record{*found};
        // A field set or deleted gets a new value (none, if deleted), at the end of its history.
        ForEachField([&](DataField field, auto data_field, auto update_field, auto) {
            const auto& change{update->updates.*update_field};
            if (change.kind == UpdateKind::RETAIN) return;
            auto& value{record.data.*data_field};
            value = change.kind == UpdateKind::SET ? change.value : std::nullopt;
            using T = typename std::decay_t<decltype(value)>::value_type;
            uint32_t& n{record.changes[static_cast<size_t>(field)]};
            plan.history.emplace_back(HistoryKey(update->asset, field, n), sidechain::EncodeValue(Stamped<T>{value, txid, height}));
            ++n;
        });
        plan.assets.emplace_back(update->asset, std::move(record));
    } else if (const auto* set{op ? std::get_if<SetLogo>(op) : nullptr}) {
        if (!HasAsset(set->asset)) return invalid("bad-ba-asset-unknown");
        // The control coin of this asset, as for its data (UpdateAsset); the books hold the
        // transaction to output it again.
        if (!spends(Token::Kind::CONTROL, set->asset)) return invalid("bad-ba-no-control");
        Plan::Logo logo{set->asset, set->logo};
        if (set->logo.empty()) {
            if (!GetLogo(set->asset)) return invalid("bad-ba-logo-none");
        } else {
            std::string why;
            logo.format = *Assert(CheckLogo(set->logo, why));
        }
        plan.logo = std::move(logo);
    } else if (const auto* named{op ? std::get_if<SetDisplayName>(op) : nullptr}) {
        if (!HasAsset(named->asset)) return invalid("bad-ba-asset-unknown");
        // The control coin of this asset, as for its logo.
        if (!spends(Token::Kind::CONTROL, named->asset)) return invalid("bad-ba-no-control");
        if (named->display_name.empty() && !GetDisplayName(named->asset)) return invalid("bad-ba-display-name-none");
        plan.display_name = std::make_pair(named->asset, named->display_name);
    } else if (const auto* burn{op ? std::get_if<Burn>(op) : nullptr}) {
        std::map<AssetId, AssetRecord> changed;
        for (const Token& token : burn->tokens) {
            debit(token.kind, token.id, token.amount);
            if (token.kind == Token::Kind::RESERVATION) {
                if (!GetReservation(Txid::FromUint256(token.id))) return invalid("bad-ba-burn");
                plan.reservations_gone.push_back(Txid::FromUint256(token.id));
                continue;
            }
            auto it{changed.find(token.id)};
            if (it == changed.end()) {
                const auto known{GetAsset(token.id)};
                if (!known) return invalid("bad-ba-asset-unknown");
                it = changed.emplace(token.id, *known).first;
            }
            if (token.kind == Token::Kind::CONTROL) {
                it->second.fixed = true;
            } else {
                if (token.amount > it->second.supply) return invalid("bad-ba-burn");
                it->second.supply -= token.amount;
                const auto burned{Add(it->second.burned, token.amount)};
                if (!burned) return invalid("bad-ba-burn");
                it->second.burned = *burned;
            }
        }
        for (auto& [id, record] : changed) plan.assets.emplace_back(id, std::move(record));
    } else if (const auto* swap{op ? std::get_if<Swap>(op) : nullptr}) {
        const auto found{FindPool(swap->asset_in, swap->asset_out)};
        if (!found) return invalid("bad-ba-no-pool");
        Pool pool{*found};
        // A pool nobody provides liquidity to is closed: what went in could only buy dust, and stay there.
        if (amm::Abandoned(pool)) return invalid("bad-ba-pool-closed");
        const bool zero_in{pool.asset0 == swap->asset_in};
        uint64_t& reserve_in{zero_in ? pool.reserve0 : pool.reserve1};
        uint64_t& reserve_out{zero_in ? pool.reserve1 : pool.reserve0};
        const uint64_t out{amm::SwapOut(reserve_in, reserve_out, swap->amount_in)};
        if (out == 0 || out < swap->min_out || out >= reserve_out) return invalid("bad-ba-swap-price");
        if (swap->asset_out.IsNull() && out < MIN_CHN_PAYOUT) return invalid("bad-ba-chn-dust");
        const auto new_in{Add(reserve_in, swap->amount_in)};
        if (!new_in) return invalid("bad-ba-swap");
        reserve_in = *new_in;
        reserve_out -= out;
        uint64_t& volume{zero_in ? pool.volume0 : pool.volume1};
        volume = Add(volume, swap->amount_in).value_or(MAX_AMOUNT);
        ++pool.swaps;
        take(swap->asset_in, swap->amount_in);
        plan.results.push_back({swap->asset_out, out});
        plan.pool = std::make_pair(PoolId(swap->asset_in, swap->asset_out), pool);
    } else if (const auto* add{op ? std::get_if<AddLiquidity>(op) : nullptr}) {
        if (!tradable(add->asset_a) || !tradable(add->asset_b)) return invalid("bad-ba-asset-unknown");
        // A token on its launch curve trades there alone until it graduates into its pool.
        if (Launching(add->asset_a) || Launching(add->asset_b)) return invalid("bad-ba-launch-pooled");
        const uint256 id{PoolId(add->asset_a, add->asset_b)};
        Pool pool;
        if (const auto found{GetPool(id)}) {
            pool = *found;
        } else {
            pool.asset0 = std::min(add->asset_a, add->asset_b);
            pool.asset1 = std::max(add->asset_a, add->asset_b);
            pool.creation = txid;
            pool.height = height;
        }
        const bool a_first{pool.asset0 == add->asset_a};
        const uint64_t amount0{a_first ? add->amount_a : add->amount_b};
        const uint64_t amount1{a_first ? add->amount_b : add->amount_a};
        // The amounts are the most that goes in: at the pool's price, the rest back; or they open
        // the pool (amm::Provide). Whoever opens a pool first sets its price: a deposit made for
        // another price goes in at it, or gives fewer shares than it asks for and is refused.
        if (!Add(pool.reserve0, amount0) || !Add(pool.reserve1, amount1)) return invalid("bad-ba-liquidity");
        const auto deposit{amm::Provide(pool, amount0, amount1)};
        if (!deposit || deposit->shares < add->min_shares) return invalid("bad-ba-liquidity-price");
        if (deposit->opens) {
            // A pool opens, or reopens, with a deposit worth trading against, not dust.
            const uint64_t chn_after{pool.asset0.IsNull() ? pool.reserve0 + amount0 : pool.asset1.IsNull() ? pool.reserve1 + amount1 : MIN_OPEN_CHN};
            if (deposit->shares < MIN_OPEN_SHARES || chn_after < MIN_OPEN_CHN) return invalid("bad-ba-pool-too-small");
            // The MIN_LIQUIDITY shares nobody held (of the pool everyone left) give way to those of the new one.
            pool.shares = deposit->shares + MIN_LIQUIDITY;
        } else {
            const auto total{Add(pool.shares, deposit->shares)};
            if (!total) return invalid("bad-ba-liquidity");
            pool.shares = *total;
        }
        pool.reserve0 += deposit->take0;
        pool.reserve1 += deposit->take1;
        // All of what the transaction offers is taken in; what goes back is a result.
        take(add->asset_a, add->amount_a);
        take(add->asset_b, add->amount_b);
        const uint64_t back0{amount0 - deposit->take0}, back1{amount1 - deposit->take1};
        // Only the side that gives more shares has any back.
        if (back0 > 0 && back1 > 0) return invalid("bad-ba-liquidity");
        plan.results.push_back({id, deposit->shares});
        plan.results.push_back(back0 > 0 ? Result{pool.asset0, back0} : Result{pool.asset1, back1});
        plan.returns = true;
        plan.pool = std::make_pair(id, pool);
    } else if (const auto* remove{op ? std::get_if<RemoveLiquidity>(op) : nullptr}) {
        const uint256 id{PoolId(remove->asset_a, remove->asset_b)};
        const auto found{GetPool(id)};
        if (!found) return invalid("bad-ba-no-pool");
        Pool pool{*found};
        // The shares nobody holds stay.
        if (remove->shares > pool.shares - std::min(pool.shares, MIN_LIQUIDITY)) return invalid("bad-ba-liquidity");
        // A side that rounds to nothing (or to CHN dust, which stays in the pool) is left out, so that
        // a small provider can still leave; one side must give something.
        const auto [out0, out1]{amm::WithdrawPaid(pool, remove->shares)};
        const bool a_first{pool.asset0 == remove->asset_a};
        const uint64_t out_a{a_first ? out0 : out1}, out_b{a_first ? out1 : out0};
        if (out_a == 0 && out_b == 0) return invalid("bad-ba-liquidity-price");
        if (out_a < remove->min_a || out_b < remove->min_b) return invalid("bad-ba-liquidity-price");
        pool.reserve0 -= out0;
        pool.reserve1 -= out1;
        pool.shares -= remove->shares;
        debit(Token::Kind::LP, id, remove->shares);
        if (out_a > 0) plan.results.push_back({remove->asset_a, out_a});
        if (out_b > 0) plan.results.push_back({remove->asset_b, out_b});
        plan.pool = std::make_pair(id, pool);
    } else if (const auto* create{op ? std::get_if<CreateAuction>(op) : nullptr}) {
        if (!tradable(create->base) || !tradable(create->quote)) return invalid("bad-ba-asset-unknown");
        if (create->start_height < height) return invalid("bad-ba-auction-started");
        // An auction that never starts would hold what it sells, and the asset it quotes, for ever.
        if (create->start_height > height + MAX_AUCTION_DELAY) return invalid("bad-ba-auction-delay");
        // CHN sold are paid out by the coinbase, MIN_CHN_PAYOUT at least: less could never be bought,
        // nor taken back.
        if (create->base.IsNull() && create->base_amount < MIN_CHN_PAYOUT) return invalid("bad-ba-chn-dust");
        Auction auction;
        auction.base = create->base;
        auction.base_amount = create->base_amount;
        auction.quote = create->quote;
        auction.start_price = create->start_price;
        auction.end_price = create->end_price;
        auction.start_height = create->start_height;
        auction.duration = create->duration;
        auction.created = height;
        auction.remaining = create->base_amount;
        auction.base_registration = registration(create->base);
        auction.quote_registration = registration(create->quote);
        take(create->base, create->base_amount);
        // Its receipt: the transaction must output it.
        credit(Token::Kind::RECEIPT, txid.ToUint256(), 1);
        plan.auction = std::make_pair(txid, auction);
    } else if (const auto* bid{op ? std::get_if<Bid>(op) : nullptr}) {
        const auto found{GetAuction(bid->auction)};
        if (!found) return invalid("bad-ba-no-auction");
        Auction auction{*found};
        if (!auction.OpenAt(height)) return invalid("bad-ba-auction-closed");
        // Its assets as they were: one retired (an auction that took none of what it quotes does not
        // keep that from being retired), or retired and registered again, is not what it trades.
        // Collect needs no such rule: what it pays of the asset it quotes is what bids paid in, none.
        if ((registration(auction.base) != auction.base_registration || registration(auction.quote) != auction.quote_registration)) {
            return invalid("bad-ba-auction-asset-gone");
        }
        uint64_t bought{auction.BuysAt(height, bid->quote_amount)};
        // A bid that would buy more than is left buys all of it, for what that costs (CostOfRemaining),
        // and gets the rest back if it says where: in CHN, by the coinbase to `chn_to`; in an asset, to
        // a result output after that of what it buys (the transaction has one more result output than
        // what it buys needs, which carries nothing if nothing comes back). So a bid for all that is
        // left, made for the next block, still buys it in a later one, where the price is lower.
        const size_t bought_outputs{auction.base.IsNull() ? 0U : 1U};
        const bool back_output{!auction.quote.IsNull() && result_outputs == bought_outputs + 1};
        const bool gives_back{auction.quote.IsNull() ? !bid->chn_to.empty() : back_output};
        uint64_t paid{bid->quote_amount}, back{0};
        if (bought > auction.remaining && auction.remaining > 0 && gives_back) {
            // Never more than the bid: it buys more than is left, so it pays at least what that costs.
            paid = auction.CostOfRemaining(height);
            if (paid > bid->quote_amount) return invalid("bad-ba-bid-price");
            bought = auction.remaining;
            back = bid->quote_amount - paid;
            // CHN dust does not come back (the coinbase pays out MIN_CHN_PAYOUT at least): the auction keeps it.
            if (auction.quote.IsNull() && back < MIN_CHN_PAYOUT) {
                paid += back;
                back = 0;
            }
        }
        if (bought == 0 || bought > auction.remaining || bought < bid->min_base) return invalid("bad-ba-bid-price");
        // CHN are paid out MIN_CHN_PAYOUT at least: a bid buys no less, leaves none less to buy (nor to
        // take back), and pays no less (the proceeds, collected, would not be paid out).
        if (auction.base.IsNull() && (bought < MIN_CHN_PAYOUT || (bought < auction.remaining && auction.remaining - bought < MIN_CHN_PAYOUT))) {
            return invalid("bad-ba-chn-dust");
        }
        if (auction.quote.IsNull() && paid < MIN_CHN_PAYOUT) return invalid("bad-ba-chn-dust");
        const auto proceeds{Add(auction.proceeds, paid)};
        if (!proceeds) return invalid("bad-ba-bid");
        auction.remaining -= bought;
        auction.proceeds = *proceeds;
        ++auction.bids;
        take(auction.quote, bid->quote_amount);
        plan.results.push_back({auction.base, bought});
        if (auction.quote.IsNull() && back > 0) plan.results.push_back({auction.quote, back});
        if (back_output) plan.back = Result{auction.quote, back};
        plan.auction = std::make_pair(bid->auction, auction);
    } else if (const auto* collect{op ? std::get_if<Collect>(op) : nullptr}) {
        const auto found{GetAuction(collect->auction)};
        if (!found) return invalid("bad-ba-no-auction");
        Auction auction{*found};
        if (!auction.CollectableAt(height)) return invalid("bad-ba-auction-running");
        debit(Token::Kind::RECEIPT, collect->auction.ToUint256(), 1);
        // CHN dust is not paid out: it was burned when it went in.
        const auto paid{[&](const AssetId& asset, uint64_t amount) { return amount > 0 && !(asset.IsNull() && amount < MIN_CHN_PAYOUT); }};
        if (paid(auction.base, auction.remaining)) plan.results.push_back({auction.base, auction.remaining});
        if (paid(auction.quote, auction.proceeds)) plan.results.push_back({auction.quote, auction.proceeds});
        auction.remaining = 0;
        auction.proceeds = 0;
        auction.closed = true;
        plan.auction = std::make_pair(collect->auction, auction);
    }

    if (const auto* release{op ? std::get_if<ReleaseAsset>(op) : nullptr}) {
        std::string why;
        if (!Releasable(release->asset, &why)) return invalid("bad-ba-release-alive");
        plan.asset_gone = release->asset;
        std::map<AssetId, AssetRecord> others;
        // Its pools, in the order of their ids, through the index of pools by asset.
        std::vector<std::pair<uint256, Pool>> pools;
        m_view->ForEach(Prefix(POOLS_BY_ASSET, release->asset), [&](const sidechain::StoreBytes& key, const sidechain::StoreBytes&) {
            std::span<const unsigned char> in{key};
            in = in.subspan(33);
            const uint256 id{sidechain::KeyCodec<uint256>::Decode(in)};
            pools.emplace_back(id, *Assert(GetPool(id)));
            return true;
        });
        for (const auto& [id, pool] : pools) {
            plan.pools_gone.push_back(id);
            // What the pool holds of the other asset: CHN for mainchain miners, any other asset burned.
            const bool zero_is_it{pool.asset0 == release->asset};
            const AssetId& other{zero_is_it ? pool.asset1 : pool.asset0};
            const uint64_t left{zero_is_it ? pool.reserve1 : pool.reserve0};
            if (other.IsNull()) {
                plan.released += static_cast<CAmount>(left);
                if (!MoneyRange(plan.released)) return invalid("bad-ba-release");
            } else if (left > 0) {
                auto o{others.find(other)};
                if (o == others.end()) o = others.emplace(other, *Assert(GetAsset(other))).first;
                o->second.supply -= std::min(o->second.supply, left);
                o->second.burned = Add(o->second.burned, left).value_or(MAX_AMOUNT);
            }
        }
        for (auto& [id, record] : others) plan.assets.emplace_back(id, std::move(record));
    }

    // The CHN taken in are what the marker burns: no more, no less.
    if (static_cast<uint64_t>(chn_in) != chn_taken) return invalid("bad-ba-marker-value");

    // The results: CHN by the coinbase, tokens to the result outputs, in order; what each result
    // output carries (nothing, for one that takes CHN back from an AddLiquidity, or nothing back).
    std::vector<std::optional<Token>> result_tokens;
    if (plan.returns) {
        if (result_outputs != 2 || plan.results.size() != 2) return invalid("bad-ba-results");
        result_tokens.push_back(Token{Token::Kind::LP, plan.results[0].asset, plan.results[0].amount});
        const Result& back{plan.results[1]};
        if (back.amount == 0) {
            result_tokens.emplace_back();
        } else if (back.asset.IsNull()) {
            // Paid by the coinbase to the script of the second result output (AddLiquidity names no
            // other): CHN that went in, burned by the marker, comes out.
            size_t seen{0};
            for (const MarkerOutput& out : marker->outputs) {
                if (!out.token && seen++ == 1) plan.payouts.emplace_back(static_cast<CAmount>(back.amount), tx.vout[out.n].scriptPubKey);
            }
            result_tokens.emplace_back();
        } else {
            result_tokens.push_back(Token{Token::Kind::ASSET, back.asset, back.amount});
        }
    }
    for (const Result& result : plan.returns ? std::vector<Result>{} : plan.results) {
        if (result.kind == Token::Kind::ASSET && result.asset.IsNull()) {
            const CScript* chn_to{op ? ChnTo(*op) : nullptr};
            if (!chn_to || chn_to->empty()) return invalid("bad-ba-chn-to");
            if (result.amount > static_cast<uint64_t>(MAX_MONEY)) return invalid("bad-ba-chn-amount");
            plan.payouts.emplace_back(static_cast<CAmount>(result.amount), *chn_to);
            continue;
        }
        // An AddLiquidity's results are above; the others are asset coins, or shares (CreatePool, PoolDeposit).
        result_tokens.push_back(Token{result.kind, result.asset, result.amount});
    }
    if (plan.back) {
        if (plan.back->amount == 0) {
            result_tokens.emplace_back();
        } else {
            result_tokens.push_back(Token{Token::Kind::ASSET, plan.back->asset, plan.back->amount});
        }
    }
    if (plan.order) {
        // An order: one result output, which gets what comes out, or what went in back, at the block's end.
        if (result_outputs != 1 || !result_tokens.empty()) return invalid("bad-ba-results");
    } else if (result_tokens.size() != result_outputs) {
        return invalid("bad-ba-results");
    }
    for (const auto& [key, balance] : books) {
        if (balance != 0) return invalid(balance > 0 ? "bad-ba-tokens-lost" : "bad-ba-tokens-unbacked");
    }

    for (const auto& [n, token] : explicit_outputs) plan.created.emplace_back(COutPoint{txid, n}, token);
    size_t next_result{0};
    for (const MarkerOutput& out : marker->outputs) {
        if (out.token) continue;
        if (plan.order) {
            plan.order->result_n = out.n;
            continue;
        }
        // An oracle contract pays the winner's script, and nobody else.
        if (plan.result_script && tx.vout[out.n].scriptPubKey != *plan.result_script) return invalid("bad-ba-contract-payee");
        if (const auto& token{result_tokens[next_result++]}) plan.created.emplace_back(COutPoint{txid, out.n}, *token);
    }
    return plan;
}

bool State::PlanDex(const CTransaction& tx, const Operation& op, int height, Plan& plan, uint64_t& chn_taken, const std::function<void(const AssetId&, uint64_t)>& take,
                    std::string& reject_reason) const
{
    const auto invalid{[&](const char* reason) {
        reject_reason = reason;
        return false;
    }};
    const Txid txid{tx.GetHash()};
    if (std::holds_alternative<Swap>(op) || std::holds_alternative<Route>(op) || std::holds_alternative<SwapPools>(op)) {
        Order order;
        order.txid = txid;
        if (const auto* named{std::get_if<SwapPools>(&op)}) {
            // The pools named, each open, each holding what the hop before gave, no asset twice.
            const auto path{dex::PathOfPools(*this, named->asset_in, named->pools)};
            if (!path) return invalid("bad-ba-no-pool");
            for (const uint256& id : named->pools) {
                if (amm::Abandoned(*Assert(GetPool(id)))) return invalid("bad-ba-pool-closed");
            }
            order.path = *path;
            order.pools = named->pools;
            // Each hop through a pool whose curve is searched (stable, weighted) is work.
            for (const uint256& id : named->pools) {
                const Curve curve{GetPoolParams(id).GetCurve()};
                // A concentrated pool's round walks its ticks: twice the work of a searched curve.
                if (curve == Curve::CONCENTRATED || curve == Curve::BOOK) {
                    plan.curve_work += 2;
                } else if (curve != Curve::CONSTANT_PRODUCT && curve != Curve::LAUNCH) {
                    ++plan.curve_work;
                }
            }
            order.amount_in = named->amount_in;
            order.min_out = named->min_out;
            order.chn_to = named->chn_to;
        } else if (const auto* swap{std::get_if<Swap>(&op)}) {
            order.path = {swap->asset_in, swap->asset_out};
            order.amount_in = swap->amount_in;
            order.min_out = swap->min_out;
            order.chn_to = swap->chn_to;
        } else {
            const Route& route{std::get<Route>(op)};
            order.path = route.path;
            order.amount_in = route.amount_in;
            order.min_out = route.min_out;
            order.chn_to = route.chn_to;
        }
        // Every pool of the path, open (one closed or gone by the block's end: the order is refunded).
        for (size_t i{0}; order.pools.empty() && i + 1 < order.path.size(); ++i) {
            const auto pool{FindPool(order.path[i], order.path[i + 1])};
            if (!pool) return invalid("bad-ba-no-pool");
            if (amm::Abandoned(*pool)) return invalid("bad-ba-pool-closed");
        }
        // CHN out, or back, is paid to chn_to; CHN back is paid out, MIN_CHN_PAYOUT at least.
        const bool chn_end{order.path.front().IsNull() || order.path.back().IsNull()};
        if (chn_end && order.chn_to.empty()) return invalid("bad-ba-chn-to");
        if (order.path.front().IsNull() && order.amount_in < MIN_CHN_PAYOUT) return invalid("bad-ba-chn-dust");
        take(order.path.front(), order.amount_in);
        // Its place among the payouts; the amount at the block's end.
        if (chn_end) plan.payouts.emplace_back(0, order.chn_to);
        plan.order = std::move(order);
        return true;
    }
    if (const auto* reg{std::get_if<RegisterOracle>(&op)}) {
        // The key is the oracle: registered once, by whoever holds it (its proof, bound to this chain).
        const uint256 id{dex::OracleId(reg->pubkey)};
        if (GetOracle(id)) return invalid("bad-ba-oracle-taken");
        if (!dex::CheckRegistration(reg->name, reg->pubkey, Params().GetConsensus().hashGenesisBlock, reg->proof)) return invalid("bad-ba-oracle-proof");
        // The fee goes to mainchain miners, as CHN freed by retiring an asset do.
        chn_taken += ORACLE_REGISTRATION_FEE;
        plan.released += ORACLE_REGISTRATION_FEE;
        plan.oracle = std::make_pair(id, OracleRecord{reg->name, reg->pubkey, txid, height, 0});
        return true;
    }
    if (const auto* att{std::get_if<Attest>(&op)}) {
        const auto oracle{GetOracle(att->oracle)};
        if (!oracle) return invalid("bad-ba-no-oracle");
        if (att->height > height) return invalid("bad-ba-attestation-early");
        if (GetAttestation(att->oracle, att->event)) return invalid("bad-ba-attested");
        if (!dex::CheckAttestation(oracle->pubkey, att->oracle, att->event, att->value, att->height, att->signature)) return invalid("bad-ba-attestation-sig");
        OracleRecord record{*oracle};
        ++record.attestations;
        plan.oracle = std::make_pair(att->oracle, std::move(record));
        plan.attestation = std::make_pair(std::make_pair(att->oracle, att->event), Attestation{att->value, att->height, height, txid});
        return true;
    }
    if (const auto* create{std::get_if<CreateOracleContract>(&op)}) {
        if (!GetOracle(create->oracle)) return invalid("bad-ba-no-oracle");
        if (create->expiry <= height || int64_t{create->expiry} > int64_t{height} + MAX_CONTRACT_DURATION) return invalid("bad-ba-contract-expiry");
        if (!create->asset.IsNull() && !HasAsset(create->asset)) return invalid("bad-ba-asset-unknown");
        // CHN are paid out by the coinbase, MIN_CHN_PAYOUT at least.
        if (create->asset.IsNull() && (create->amount < MIN_CHN_PAYOUT || create->amount > static_cast<uint64_t>(MAX_MONEY))) return invalid("bad-ba-chn-dust");
        take(create->asset, create->amount);
        OracleContract contract;
        contract.oracle = create->oracle;
        contract.event = create->event;
        contract.asset = create->asset;
        contract.amount = create->amount;
        contract.outcomes = create->outcomes;
        contract.refund = create->refund;
        contract.expiry = create->expiry;
        contract.created = height;
        plan.contract = std::make_pair(txid, std::move(contract));
        return true;
    }
    if (const auto* settle{std::get_if<SettleOracleContract>(&op)}) {
        const auto found{GetContract(settle->contract)};
        if (!found) return invalid("bad-ba-no-contract");
        if (found->settled) return invalid("bad-ba-contract-settled");
        const auto winner{found->Winner(GetAttestation(found->oracle, found->event), height)};
        if (!winner) return invalid("bad-ba-contract-pending");
        OracleContract contract{*found};
        contract.settled = true;
        contract.paid_to = *winner;
        contract.settled_height = height;
        if (found->asset.IsNull()) {
            plan.payouts.emplace_back(static_cast<CAmount>(found->amount), *winner);
        } else {
            plan.results.push_back({found->asset, found->amount});
            plan.result_script = *winner;
        }
        plan.contract = std::make_pair(settle->contract, std::move(contract));
        return true;
    }
    return invalid("bad-ba-marker");
}

bool State::PlanPosition(const CTransaction& tx, const Operation& op, int height, Plan& plan, const std::function<void(const AssetId&, uint64_t)>& take,
                         const std::function<void(Token::Kind, const uint256&, uint64_t)>& credit, const std::function<void(Token::Kind, const uint256&, uint64_t)>& debit,
                         std::string& reject_reason) const
{
    const auto invalid{[&](const char* reason) {
        reject_reason = reason;
        return false;
    }};
    plan.curve_work = 1;
    const uint256 pool_id{std::holds_alternative<OpenPosition>(op) ? std::get<OpenPosition>(op).pool : [&] {
        const auto position{GetPosition(std::get<ChangePosition>(op).position)};
        return position ? position->pool : uint256{};
    }()};
    const auto found{GetPool(pool_id)};
    if (!found) return invalid(std::holds_alternative<OpenPosition>(op) ? "bad-ba-no-pool" : "bad-ba-no-position");
    const PoolParams params{GetPoolParams(pool_id)};
    if (params.GetCurve() != Curve::CONCENTRATED) return invalid("bad-ba-not-cl");
    Pool pool{*found};
    ClState cl{*Assert(GetClState(pool_id))};
    const arith_uint256 price{UintToArith256(cl.sqrt_price)};
    const int32_t spacing{cl::TickSpacing(params.fee)};
    // A tick's record after a change of the liquidity that starts (lower) or ends (upper) there by `delta` (Uniswap's Tick.update).
    const auto update_tick{[&](int32_t tick, bool upper, unsigned __int128 delta, bool add) -> std::optional<std::optional<ClTick>> {
        ClTick t{GetClTick(pool_id, tick).value_or(ClTick{})};
        const unsigned __int128 gross{t.Gross()};
        if (!add && gross < delta) return std::nullopt;
        if (add && gross > ~static_cast<unsigned __int128>(0) - delta) return std::nullopt;
        const unsigned __int128 after{add ? gross + delta : gross - delta};
        // Never more than a tick may carry (MaxLiquidityPerTick): the liquidity in range, a sum over ticks, fits.
        if (add && after > MaxLiquidityPerTick(spacing)) return std::nullopt;
        // A tick newly in use: the fees on its other side are taken as all of them below the price (Uniswap's convention).
        if (gross == 0) {
            t.outside0 = tick <= cl.tick ? cl.fee_growth0 : uint256{};
            t.outside1 = tick <= cl.tick ? cl.fee_growth1 : uint256{};
        }
        // net: + at the lower tick, − at the upper, of the liquidity added (the reverse removed).
        const bool plus{add != upper};
        __int128 net{t.net_negative ? -static_cast<__int128>(t.NetAbs()) : static_cast<__int128>(t.NetAbs())};
        net += plus ? static_cast<__int128>(delta) : -static_cast<__int128>(delta);
        const unsigned __int128 abs{static_cast<unsigned __int128>(net < 0 ? -net : net)};
        t.net_negative = net < 0;
        t.net_hi = static_cast<uint64_t>(abs >> 64);
        t.net_lo = static_cast<uint64_t>(abs);
        t.gross_hi = static_cast<uint64_t>(after >> 64);
        t.gross_lo = static_cast<uint64_t>(after);
        if (after == 0) return std::optional<ClTick>{};
        return std::optional<ClTick>{t};
    }};
    // A change of a position's liquidity: its ticks, the pool's liquidity in range, the fees it earned so far into its record.
    const auto modify{[&](ClPosition& position, unsigned __int128 delta, bool add) -> bool {
        if (delta > 0) {
            const auto lo{update_tick(position.tick_lower, false, delta, add)}, hi{update_tick(position.tick_upper, true, delta, add)};
            if (!lo || !hi) return false;
            plan.cl_ticks.emplace_back(position.tick_lower, *lo);
            plan.cl_ticks.emplace_back(position.tick_upper, *hi);
        }
        // Fees earned so far, with the ticks as they were before the change (a tick cleared reads as fresh).
        const ClTick lo_before{GetClTick(pool_id, position.tick_lower).value_or(ClTick{})}, hi_before{GetClTick(pool_id, position.tick_upper).value_or(ClTick{})};
        ClTick lo_now{lo_before}, hi_now{hi_before};
        for (const auto& [tick, value] : plan.cl_ticks) {
            if (tick == position.tick_lower && value) lo_now = *value;
            if (tick == position.tick_upper && value) hi_now = *value;
        }
        const bool fresh{position.Liquidity() == 0 && add};
        const auto [in0, in1]{FeeGrowthInside(cl, position.tick_lower, position.tick_upper, fresh ? lo_now : lo_before, fresh ? hi_now : hi_before)};
        if (!fresh) {
            const uint64_t e0{Earned(in0, position.inside0_last, position.Liquidity())}, e1{Earned(in1, position.inside1_last, position.Liquidity())};
            position.owed0 = position.owed0 > MAX_AMOUNT - e0 ? MAX_AMOUNT : position.owed0 + e0;
            position.owed1 = position.owed1 > MAX_AMOUNT - e1 ? MAX_AMOUNT : position.owed1 + e1;
        }
        position.inside0_last = ArithToUint256(in0);
        position.inside1_last = ArithToUint256(in1);
        const unsigned __int128 l{position.Liquidity()};
        if (!add && l < delta) return false;
        // A position never carries more than a tick may.
        if (add && delta > MaxLiquidityPerTick(spacing) - std::min(l, MaxLiquidityPerTick(spacing))) return false;
        const unsigned __int128 after{add ? l + delta : l - delta};
        position.liquidity_hi = static_cast<uint64_t>(after >> 64);
        position.liquidity_lo = static_cast<uint64_t>(after);
        // In range: the pool's liquidity now changes too, checked both ways.
        if (delta > 0 && position.tick_lower <= cl.tick && cl.tick < position.tick_upper) {
            const unsigned __int128 active{cl.Liquidity()};
            if (!add && active < delta) return false;
            if (add && delta > ~static_cast<unsigned __int128>(0) - active) return false;
            cl.SetLiquidity(add ? active + delta : active - delta);
        }
        return true;
    }};
    // What comes back or out: asset coins as results; CHN by the coinbase (ChnTo), no dust (it stays owed, or in the pool).
    const auto result{[&](const AssetId& asset, uint64_t amount) {
        if (amount > 0) plan.results.push_back({asset, amount});
    }};
    if (const auto* open{std::get_if<OpenPosition>(&op)}) {
        if (spacing == 0 || open->tick_lower % spacing != 0 || open->tick_upper % spacing != 0) return invalid("bad-ba-position-ticks");
        const unsigned __int128 delta{open->Liquidity()};
        ClPosition position;
        position.pool = pool_id;
        position.tick_lower = open->tick_lower;
        position.tick_upper = open->tick_upper;
        position.txid = tx.GetHash();
        position.height = height;
        const auto needed{cl::AmountsForLiquidity(price, cl::SqrtRatioAtTick(open->tick_lower), cl::SqrtRatioAtTick(open->tick_upper), delta, /*round_up=*/true)};
        if (!needed || needed->first > open->max0 || needed->second > open->max1) return invalid("bad-ba-position-price");
        if (needed->first > MAX_AMOUNT - pool.reserve0 || needed->second > MAX_AMOUNT - pool.reserve1) return invalid("bad-ba-position");
        if (!modify(position, delta, true)) return invalid("bad-ba-position");
        take(pool.asset0, open->max0);
        take(pool.asset1, open->max1);
        pool.reserve0 += needed->first;
        pool.reserve1 += needed->second;
        // The position's token: the first result (results are made by the operation, outside the books).
        const uint256 id{PositionId(tx.GetHash())};
        (void)credit;
        plan.results.push_back({id, 1, Token::Kind::POSITION});
        // What comes back: CHN under MIN_CHN_PAYOUT stays in the pool (nobody's), as a deposit's dust does.
        const uint64_t back0{open->max0 - needed->first}, back1{open->max1 - needed->second};
        if (pool.asset0.IsNull() && back0 < MIN_CHN_PAYOUT) {
            pool.reserve0 += back0;
        } else {
            result(pool.asset0, back0);
        }
        result(pool.asset1, back1);
        plan.pool = std::make_pair(pool_id, pool);
        plan.cl_state = std::make_pair(pool_id, cl);
        plan.position = std::make_pair(id, std::optional<ClPosition>{position});
        return true;
    }
    const ChangePosition& change{std::get<ChangePosition>(op)};
    ClPosition position{*Assert(GetPosition(change.position))};
    // Whoever spends its token: the books hold the token to an output, or to the close.
    const unsigned __int128 delta{change.close ? position.Liquidity() : change.Liquidity()};
    const arith_uint256 sa{cl::SqrtRatioAtTick(position.tick_lower)}, sb{cl::SqrtRatioAtTick(position.tick_upper)};
    if (change.add) {
        const auto needed{cl::AmountsForLiquidity(price, sa, sb, delta, /*round_up=*/true)};
        if (!needed || needed->first > change.limit0 || needed->second > change.limit1) return invalid("bad-ba-position-price");
        if (needed->first > MAX_AMOUNT - pool.reserve0 || needed->second > MAX_AMOUNT - pool.reserve1) return invalid("bad-ba-position");
        if (!modify(position, delta, true)) return invalid("bad-ba-position");
        take(pool.asset0, change.limit0);
        take(pool.asset1, change.limit1);
        pool.reserve0 += needed->first;
        pool.reserve1 += needed->second;
        const uint64_t back0{change.limit0 - needed->first}, back1{change.limit1 - needed->second};
        if (pool.asset0.IsNull() && back0 < MIN_CHN_PAYOUT) {
            pool.reserve0 += back0;
        } else {
            result(pool.asset0, back0);
        }
        result(pool.asset1, back1);
    } else {
        // Out: the liquidity's amounts at the price (rounded down), and, collected, all it is owed.
        const auto amounts{cl::AmountsForLiquidity(price, sa, sb, delta, /*round_up=*/false)};
        if (!amounts) return invalid("bad-ba-position");
        if (!modify(position, delta, false)) return invalid("bad-ba-position");
        uint64_t out0{amounts->first}, out1{amounts->second};
        if (out0 < change.limit0 || out1 < change.limit1) return invalid("bad-ba-position-price");
        if (change.collect || change.close) {
            out0 = out0 > MAX_AMOUNT - position.owed0 ? MAX_AMOUNT : out0 + position.owed0;
            out1 = out1 > MAX_AMOUNT - position.owed1 ? MAX_AMOUNT : out1 + position.owed1;
            position.owed0 = position.owed1 = 0;
        } else {
            // Liquidity taken out and its fees left to collect later.
        }
        // CHN dust is not paid: owed (or, closing, left in the pool).
        if (pool.asset0.IsNull() && out0 < MIN_CHN_PAYOUT) {
            if (!change.close) position.owed0 += out0;
            out0 = 0;
        }
        if (out0 > pool.reserve0 || out1 > pool.reserve1) return invalid("bad-ba-position");
        pool.reserve0 -= out0;
        pool.reserve1 -= out1;
        result(pool.asset0, out0);
        result(pool.asset1, out1);
    }
    // The token is taken in (so the transaction must spend it: the books refuse a change by anyone else), and given
    // back as the first result unless the position closes.
    debit(Token::Kind::POSITION, change.position, 1);
    if (change.close) {
        if (position.Liquidity() != 0) return invalid("bad-ba-position");
        plan.position = std::make_pair(change.position, std::optional<ClPosition>{});
    } else {
        plan.results.insert(plan.results.begin(), Result{change.position, 1, Token::Kind::POSITION});
        plan.position = std::make_pair(change.position, std::optional<ClPosition>{position});
    }
    plan.pool = std::make_pair(pool_id, pool);
    plan.cl_state = std::make_pair(pool_id, cl);
    return true;
}

bool State::PlanLaunch(const CTransaction& tx, const LaunchToken& launch, int height, Plan& plan, const std::function<void(const AssetId&, uint64_t)>& take, std::string& reject_reason) const
{
    const auto invalid{[&](const char* reason) {
        reject_reason = reason;
        return false;
    }};
    const Consensus::SidechainParams& rules{Params().GetConsensus().sidechain};
    if (rules.launch_graduation <= 0 || rules.launch_graduation % 2 != 0 || rules.launch_graduation > MAX_MONEY) return invalid("bad-ba-launch-rules");
    const AssetId& token{launch.reg.name};
    LaunchRecord record;
    record.txid = tx.GetHash();
    record.height = height;
    record.creator = launch.creator;
    record.allocation = launch.allocation;
    record.min_buy = launch.min_buy;
    record.max_buy = launch.max_buy;
    record.fee = rules.launch_fee;
    record.creator_share = rules.launch_creator_share;
    record.graduation = static_cast<uint64_t>(rules.launch_graduation);
    record.virtual_coin = record.graduation / 2;
    // Its curve: a pool of the coin (asset0: the null id) and the token, of the launch's kind, on the virtual reserves.
    PoolParams params;
    params.curve = static_cast<uint8_t>(Curve::LAUNCH);
    params.fee = record.fee;
    record.pool = PoolIdOf(CHN, token, params);
    // A launch made again under a name retired: its old record goes (a new one), as the asset did.
    Pool pool;
    pool.asset0 = CHN;
    pool.asset1 = token;
    pool.reserve0 = record.virtual_coin;
    pool.reserve1 = LAUNCH_VIRTUAL_TOKENS;
    // Open (above MIN_LIQUIDITY) with no shares anyone holds.
    pool.shares = MIN_LIQUIDITY + 1;
    pool.creation = record.txid;
    pool.height = height;
    plan.pool = std::make_pair(record.pool, pool);
    plan.pool_params = std::make_pair(record.pool, params);
    plan.launch = std::make_pair(token, record);
    plan.launch_created = true;
    // Its first buy: an order of the block through the curve, as SwapPools makes one.
    if (launch.first_buy > 0) {
        if (launch.first_buy < MIN_CHN_PAYOUT) return invalid("bad-ba-chn-dust");
        Order order;
        order.txid = record.txid;
        order.path = {CHN, token};
        order.pools = {record.pool};
        order.amount_in = launch.first_buy;
        order.min_out = launch.first_min_out;
        order.chn_to = launch.chn_to;
        take(CHN, launch.first_buy);
        plan.payouts.emplace_back(0, order.chn_to);
        plan.order = std::move(order);
    }
    return true;
}

bool State::PlanPools(const CTransaction& tx, const Operation& op, int height, Plan& plan, const std::function<void(const AssetId&, uint64_t)>& take,
                      const std::function<void(Token::Kind, const uint256&, uint64_t)>& debit, std::string& reject_reason) const
{
    const auto invalid{[&](const char* reason) {
        reject_reason = reason;
        return false;
    }};
    const Txid txid{tx.GetHash()};
    plan.curve_work = 1;
    // A pool nobody provides liquidity to (new, or everyone left) opens as AddLiquidity opens one: the
    // shares are the invariant's first degree at the reserves after, MIN_LIQUIDITY of them kept for good,
    // what it held merged in; a deposit worth trading against, not dust.
    const auto open{[&](const PoolParams& params, Pool& pool, uint64_t amount0, uint64_t amount1, uint64_t min_shares) -> std::optional<uint64_t> {
        if (amount0 == 0 || amount1 == 0 || amount0 > MAX_AMOUNT - pool.reserve0 || amount1 > MAX_AMOUNT - pool.reserve1) {
            invalid("bad-ba-liquidity");
            return std::nullopt;
        }
        const uint64_t r0{pool.reserve0 + amount0}, r1{pool.reserve1 + amount1};
        const auto total{curves::OpeningShares(params, r0, r1)};
        if (!total || *total <= MIN_LIQUIDITY || *total - MIN_LIQUIDITY < min_shares) {
            invalid("bad-ba-liquidity-price");
            return std::nullopt;
        }
        const uint64_t shares{*total - MIN_LIQUIDITY};
        const uint64_t chn_after{pool.asset0.IsNull() ? r0 : pool.asset1.IsNull() ? r1 : MIN_OPEN_CHN};
        if (shares < MIN_OPEN_SHARES || chn_after < MIN_OPEN_CHN) {
            invalid("bad-ba-pool-too-small");
            return std::nullopt;
        }
        pool.reserve0 = r0;
        pool.reserve1 = r1;
        pool.shares = *total;
        return shares;
    }};
    if (const auto* place{std::get_if<PlaceOrder>(&op)}) {
        // The first order of a block in a book: the book crosses at the block's end, a round's work (2 units, as a hop).
        plan.curve_work = BOOKS_TOUCHED.Get(*m_view, BookId(place->asset_sell, place->asset_buy)) ? 0 : 2;
        const auto tradable{[&](const AssetId& asset) { return asset.IsNull() || HasAsset(asset); }};
        if (!tradable(place->asset_sell) || !tradable(place->asset_buy)) return invalid("bad-ba-asset-unknown");
        if (place->expiry < height || int64_t{place->expiry} > int64_t{height} + MAX_ORDER_DURATION) return invalid("bad-ba-order-expiry");
        // CHN goes back and out by the coinbase: MIN_CHN_PAYOUT at least.
        if (place->asset_sell.IsNull() && place->amount < MIN_CHN_PAYOUT) return invalid("bad-ba-chn-dust");
        // Worth something: what it would buy at its limit (amount·num/den, in 128 bits) and, of a pair with CHN, its CHN
        // side at least MIN_BOOK_CHN; of another pair, a whole unit of what it sells at least. A side is no place for dust.
        const unsigned __int128 buys{static_cast<unsigned __int128>(place->amount) * place->price_num / place->price_den};
        if (buys == 0) return invalid("bad-ba-order-dust");
        if (place->asset_sell.IsNull() || place->asset_buy.IsNull()) {
            const unsigned __int128 chn{place->asset_sell.IsNull() ? place->amount : buys};
            if (chn < MIN_BOOK_CHN) return invalid("bad-ba-order-dust");
        } else {
            uint64_t unit{1};
            for (uint8_t i{0}, d{GetAsset(place->asset_sell)->decimals}; i < d && unit <= MAX_AMOUNT / 10; ++i) unit *= 10;
            if (place->amount < unit) return invalid("bad-ba-order-dust");
        }
        const uint256 book_id{BookId(place->asset_sell, place->asset_buy)};
        Pool book;
        if (const auto found{GetPool(book_id)}) {
            book = *found;
        } else {
            book.asset0 = std::min(place->asset_sell, place->asset_buy);
            book.asset1 = std::max(place->asset_sell, place->asset_buy);
            book.creation = txid;
            book.height = height;
            book.shares = MIN_LIQUIDITY;
            PoolParams params;
            params.curve = static_cast<uint8_t>(Curve::BOOK);
            params.fee = 0;
            plan.pool_params = std::make_pair(book_id, params);
        }
        BookOrder order;
        order.book = book_id;
        order.sells0 = place->asset_sell == book.asset0;
        order.amount = order.remaining = place->amount;
        order.price_num = place->price_num;
        order.price_den = place->price_den;
        order.expiry = place->expiry;
        order.txid = txid;
        order.height = height;
        // A side's index holds at most MAX_BOOK_SIDE entries, as counted (BOOK_COUNTS: placed, less those filled, settled,
        // swept or evicted). Full: first its own expired entries go (swept here, so no other book's backlog of expired
        // orders can keep it full); still full, an order takes the place of the side's worst live order only if it is
        // better (a lower limit; the same is not) and no smaller (it sells at least what the worst has left: the same asset,
        // so worth as much at least); the worst leaves the book, all it holds its holder's to settle. Anything else, or a
        // count the index does not show, is refused.
        bool evicts{false};
        uint32_t count{BookCount(book_id, order.sells0)};
        if (count >= MAX_BOOK_SIDE) {
            const auto entries{BookEntries(book_id, order.sells0)};
            if (entries.size() != count) return invalid("bad-ba-book-full");
            std::vector<std::pair<uint256, BookOrder>> live;
            for (const auto& [eid, e] : entries) {
                if (e.remaining > 0 && e.expiry >= height && e.evicted == 0) {
                    live.emplace_back(eid, e);
                } else {
                    plan.swept.push_back(eid);
                }
            }
            count -= plan.swept.size();
            if (count >= MAX_BOOK_SIDE) {
                const auto& [worst_id, worst]{live.back()};
                const bool better{curves::BigUint{order.price_num} * curves::BigUint{worst.price_den} < curves::BigUint{worst.price_num} * curves::BigUint{order.price_den}};
                if (!better || place->amount < worst.remaining) return invalid("bad-ba-book-full");
                BookOrder out{worst};
                out.evicted = height;
                plan.evicted = std::make_pair(worst_id, out);
                evicts = true;
            }
            book.shares -= std::min<uint64_t>(book.shares - std::min<uint64_t>(book.shares, MIN_LIQUIDITY), plan.swept.size());
        }
        uint64_t& reserve{order.sells0 ? book.reserve0 : book.reserve1};
        if (place->amount > MAX_AMOUNT - reserve) return invalid("bad-ba-order");
        reserve += place->amount;
        // Its open orders in the shares: open while it holds any (one in for one out, when it takes another's place).
        if (!evicts) book.shares += 1;
        take(place->asset_sell, place->amount);
        const uint256 id{OrderId(txid)};
        plan.results.push_back({id, 1, Token::Kind::ORDER});
        plan.pool = std::make_pair(book_id, book);
        plan.book_order = std::make_pair(id, std::optional<BookOrder>{order});
        return true;
    }
    if (const auto* settle{std::get_if<SettleOrder>(&op)}) {
        plan.curve_work = 0;
        const auto found{GetOrder(settle->order)};
        if (!found) return invalid("bad-ba-no-order");
        BookOrder order{*found};
        Pool book{*Assert(GetPool(order.book))};
        // The token is taken in: whoever spends it (the books refuse anyone else).
        debit(Token::Kind::ORDER, settle->order, 1);
        const AssetId sold{order.sells0 ? book.asset0 : book.asset1}, bought{order.sells0 ? book.asset1 : book.asset0};
        const bool closes{settle->cancel || order.expiry < height || order.remaining == 0 || order.evicted != 0};
        uint64_t proceeds{order.proceeds}, left{closes ? order.remaining : 0};
        // CHN dust is not paid out: it stays with the order until there is more, or with the book once it closes.
        if (bought.IsNull() && proceeds < MIN_CHN_PAYOUT) proceeds = 0;
        if (sold.IsNull() && left < MIN_CHN_PAYOUT) left = 0;
        if (!closes && proceeds == 0) return invalid("bad-ba-order-nothing");
        if (!closes) plan.results.push_back({settle->order, 1, Token::Kind::ORDER});
        if (proceeds > 0) plan.results.push_back({bought, proceeds});
        if (left > 0) plan.results.push_back({sold, left});
        uint64_t& r_sold{order.sells0 ? book.reserve0 : book.reserve1};
        uint64_t& r_bought{order.sells0 ? book.reserve1 : book.reserve0};
        if (proceeds > r_bought || left > r_sold) return invalid("bad-ba-order");
        r_bought -= proceeds;
        r_sold -= left;
        order.proceeds -= proceeds;
        if (closes) {
            // One open order less if it was still in its book (not filled, swept or evicted).
            if (m_view->Contains(BookKey(settle->order, *found))) book.shares -= std::min<uint64_t>(book.shares - MIN_LIQUIDITY, 1);
            order.remaining = 0;
            plan.book_order = std::make_pair(settle->order, std::optional<BookOrder>{});
        } else {
            plan.book_order = std::make_pair(settle->order, std::optional<BookOrder>{order});
        }
        plan.pool = std::make_pair(order.book, book);
        return true;
    }
    if (const auto* claim{std::get_if<ClaimLaunch>(&op)}) {
        plan.curve_work = 0;
        const auto found{GetLaunch(claim->token)};
        if (!found) return invalid("bad-ba-no-launch");
        LaunchRecord record{*found};
        bool paid{false};
        // Its part of the fees, CHN by the coinbase to its script (no dust: MIN_CHN_PAYOUT at least).
        if (record.creator_fees >= MIN_CHN_PAYOUT) {
            plan.payouts.emplace_back(static_cast<CAmount>(record.creator_fees), record.creator);
            record.creator_paid += record.creator_fees;
            record.creator_fees = 0;
            paid = true;
        }
        // Its allocation, once graduated: minted to the one result output, the creator's.
        if (record.Graduated() && !record.allocation_claimed && record.allocation > 0) {
            auto asset{*Assert(GetAsset(claim->token))};
            const uint64_t units{record.AllocationUnits()};
            asset.supply += units;
            asset.minted += units;
            plan.assets.emplace_back(claim->token, std::move(asset));
            plan.results.push_back({claim->token, units});
            plan.result_script = record.creator;
            record.allocation_claimed = true;
            paid = true;
        }
        if (!paid) return invalid("bad-ba-launch-nothing-owed");
        plan.launch = std::make_pair(claim->token, std::move(record));
        return true;
    }
    if (const auto* create{std::get_if<CreatePool>(&op)}) {
        const auto tradable{[&](const AssetId& asset) { return asset.IsNull() || HasAsset(asset); }};
        if (!tradable(create->asset_a) || !tradable(create->asset_b)) return invalid("bad-ba-asset-unknown");
        // A token on its launch curve trades there alone until it graduates into its pool.
        if (Launching(create->asset_a) || Launching(create->asset_b)) return invalid("bad-ba-launch-pooled");
        // The decimals of each asset (CHN's 8): a stable pool counts their units alike.
        const auto decimals{[&](const AssetId& asset) -> uint8_t { return asset.IsNull() ? 8 : Assert(GetAsset(asset))->decimals; }};
        const AssetId asset0{std::min(create->asset_a, create->asset_b)}, asset1{std::max(create->asset_a, create->asset_b)};
        const PoolParams params{MakePoolParams(*create, decimals(asset0), decimals(asset1))};
        const uint256 id{PoolIdOf(asset0, asset1, params)};
        Pool pool;
        if (const auto found{GetPool(id)}) {
            if (!amm::Abandoned(*found)) return invalid("bad-ba-pool-exists");
            pool = *found;
        } else {
            pool.asset0 = asset0;
            pool.asset1 = asset1;
            pool.creation = txid;
            pool.height = height;
        }
        const bool a_first{asset0 == create->asset_a};
        if (params.GetCurve() == Curve::CONCENTRATED) {
            // A concentrated pool: made at the price the amounts name (asset1 per asset0), nothing in; liquidity comes by positions.
            if (GetPool(id)) return invalid("bad-ba-pool-exists");
            const uint64_t amount0{a_first ? create->amount_a : create->amount_b}, amount1{a_first ? create->amount_b : create->amount_a};
            // ⌊sqrt(amount1·2^192 / amount0)⌋, the largest root whose square is at most it.
            const curves::BigUint target{(curves::BigUint{amount1} << 192) / curves::BigUint{amount0}};
            arith_uint256 lo{0}, hi{arith_uint256{1} << 161};
            while (lo < hi) {
                const arith_uint256 mid{lo + ((hi - lo + 1) >> 1)};
                const curves::BigUint m{curves::BigUint::FromArith(mid)};
                if (m * m <= target) {
                    lo = mid;
                } else {
                    hi = mid - 1;
                }
            }
            if (lo < cl::MinSqrtRatio() || lo >= cl::MaxSqrtRatio()) return invalid("bad-ba-cl-price");
            ClState state;
            state.sqrt_price = ArithToUint256(lo);
            state.tick = cl::TickAtSqrtRatio(lo);
            pool.reserve0 = pool.reserve1 = 0;
            // Open (above MIN_LIQUIDITY) with no shares: its positions are its providers.
            pool.shares = MIN_LIQUIDITY + 1;
            plan.pool = std::make_pair(id, pool);
            plan.pool_params = std::make_pair(id, params);
            plan.cl_state = std::make_pair(id, state);
            return true;
        }
        const auto shares{open(params, pool, a_first ? create->amount_a : create->amount_b, a_first ? create->amount_b : create->amount_a, create->min_shares)};
        if (!shares) return false;
        take(create->asset_a, create->amount_a);
        take(create->asset_b, create->amount_b);
        plan.results.push_back({id, *shares, Token::Kind::LP});
        plan.pool = std::make_pair(id, pool);
        if (!params.Legacy()) plan.pool_params = std::make_pair(id, params);
        return true;
    }
    if (const auto* deposit{std::get_if<PoolDeposit>(&op)}) {
        const auto found{GetPool(deposit->pool)};
        if (!found) return invalid("bad-ba-no-pool");
        Pool pool{*found};
        const PoolParams params{GetPoolParams(deposit->pool)};
        if (params.GetCurve() == Curve::LAUNCH) return invalid("bad-ba-launch-pool");
        if (params.GetCurve() == Curve::CONCENTRATED) return invalid("bad-ba-cl-pool");
        if (params.GetCurve() == Curve::BOOK) return invalid("bad-ba-book");
        uint64_t shares{0};
        if (amm::Abandoned(pool)) {
            const auto opened{open(params, pool, deposit->amount0, deposit->amount1, deposit->min_shares)};
            if (!opened) return false;
            shares = *opened;
        } else {
            const auto got{curves::DepositShares(params, FeeOf(deposit->pool, params, height), pool, deposit->amount0, deposit->amount1)};
            if (!got || *got < deposit->min_shares || *got > MAX_AMOUNT - pool.shares) return invalid("bad-ba-liquidity-price");
            shares = *got;
            pool.shares += shares;
            pool.reserve0 += deposit->amount0;
            pool.reserve1 += deposit->amount1;
        }
        if (deposit->amount0 > 0) take(pool.asset0, deposit->amount0);
        if (deposit->amount1 > 0) take(pool.asset1, deposit->amount1);
        plan.results.push_back({deposit->pool, shares, Token::Kind::LP});
        plan.pool = std::make_pair(deposit->pool, pool);
        return true;
    }
    if (const auto* withdraw{std::get_if<PoolWithdraw>(&op)}) {
        const auto found{GetPool(withdraw->pool)};
        if (!found) return invalid("bad-ba-no-pool");
        Pool pool{*found};
        // The shares nobody holds stay.
        if (withdraw->shares > pool.shares - std::min(pool.shares, MIN_LIQUIDITY)) return invalid("bad-ba-liquidity");
        const PoolParams params{GetPoolParams(withdraw->pool)};
        if (params.GetCurve() == Curve::LAUNCH) return invalid("bad-ba-launch-pool");
        if (params.GetCurve() == Curve::CONCENTRATED) return invalid("bad-ba-cl-pool");
        if (params.GetCurve() == Curve::BOOK) return invalid("bad-ba-book");
        auto [out0, out1]{curves::WithdrawOf(params, FeeOf(withdraw->pool, params, height), pool, withdraw->shares, withdraw->only)};
        // CHN under MIN_CHN_PAYOUT is not paid out: it stays in the pool, for its providers.
        if (pool.asset0.IsNull() && out0 < MIN_CHN_PAYOUT) out0 = 0;
        if (pool.asset1.IsNull() && out1 < MIN_CHN_PAYOUT) out1 = 0;
        if ((out0 == 0 && out1 == 0) || out0 < withdraw->min0 || out1 < withdraw->min1) return invalid("bad-ba-liquidity-price");
        pool.reserve0 -= out0;
        pool.reserve1 -= out1;
        pool.shares -= withdraw->shares;
        debit(Token::Kind::LP, withdraw->pool, withdraw->shares);
        if (out0 > 0) plan.results.push_back({pool.asset0, out0});
        if (out1 > 0) plan.results.push_back({pool.asset1, out1});
        plan.pool = std::make_pair(withdraw->pool, pool);
        return true;
    }
    return invalid("bad-ba-marker");
}

bool State::CheckTx(const CTransaction& tx, int height, std::string& reject_reason, std::vector<Result>* results, int reveal_depth, size_t* payouts, bool dex) const
{
    m_dex = dex;
    auto plan{MakePlan(tx, height, reject_reason, reveal_depth)};
    if (!plan) return false;
    if (results) *results = std::move(plan->results);
    if (payouts) *payouts = plan->payouts.size();
    return true;
}

bool State::ApplyTx(const CTransaction& tx, int height, std::vector<CTxOut>& payouts, std::string& reject_reason, CAmount* released, int reveal_depth, size_t queued, bool dex)
{
    m_dex = dex;
    // The DEX: the result output of an order of this block carries what the block's end gives it; it
    // cannot be spent in the block, before it carries anything.
    if (dex && !tx.IsCoinBase()) {
        for (const CTxIn& in : tx.vin) {
            if (PENDING_RESULTS.Contains(*m_view, in.prevout)) {
                reject_reason = "bad-ba-result-pending";
                return false;
            }
        }
    }
    const auto plan{MakePlan(tx, height, reject_reason, reveal_depth)};
    if (!plan) return false;
    // Room for payouts is bounded: what a block pays, the coinbase creates, and every node keeps what
    // waits. The miner leaves the transaction for a later block. Layers: with what still waits from
    // blocks that paid none (blocks of stake take no peg actions), so that what waits is never more
    // than the next block with the peg actions is sure to pay (sidechain::State::TakePayouts).
    if (!plan->payouts.empty() && queued + payouts.size() + plan->payouts.size() > MAX_TX_PAYOUTS_PER_BLOCK) {
        reject_reason = "bad-ba-block-payouts-full";
        return false;
    }
    // The DEX: room for orders is bounded, as for payouts; the miner leaves the transaction for a later block.
    if (plan->order && ORDER_COUNT.Get(*m_view) >= MAX_BLOCK_ORDERS) {
        reject_reason = "bad-ba-block-orders-full";
        return false;
    }
    // The DEX's pool kinds: their work is bounded the same way.
    const uint32_t work_before{plan->curve_work > 0 ? CURVE_WORK.Get(*m_view) : 0};
    if (plan->curve_work > 0 && work_before + plan->curve_work > MAX_BLOCK_CURVE_WORK) {
        reject_reason = "bad-ba-block-curve-work-full";
        return false;
    }
    sidechain::StoreOverlay& out{Writable()};
    if (plan->curve_work > 0) CURVE_WORK.Put(out, work_before + plan->curve_work);
    // In the order the rules have always applied a plan in.
    for (const COutPoint& outpoint : plan->spent) SetToken(outpoint, std::nullopt);
    for (const auto& [outpoint, token] : plan->created) SetToken(outpoint, token);
    if (plan->reservation_made) {
        SetReservation(plan->reservation_made->first, plan->reservation_made->second);
        ORIGINS.Put(out, plan->reservation_made->first.ToUint256(), plan->reservation_origin);
    }
    for (const Txid& id : plan->reservations_gone) {
        SetReservation(id, std::nullopt);
        ORIGINS.Erase(out, id.ToUint256());
    }
    // The layout of the assets in the store, with their first change (StoreLayoutCurrent).
    if (plan->assets_tx && LAYOUT.Get(out) != STORE_LAYOUT) LAYOUT.Put(out, STORE_LAYOUT);
    for (const auto& [id, record] : plan->assets) ASSETS.Put(out, id, record);
    for (const auto& [key, value] : plan->history) out.Put(key, value);
    if (plan->seq) {
        SEQ.Put(out, plan->seq->first, plan->seq->second);
        NEXT_SEQ.Put(out, NextSeq() + 1);
    }
    if (plan->pool) SetPool(plan->pool->first, plan->pool->second);
    if (plan->pool_params) POOL_PARAMS.Put(out, plan->pool_params->first, plan->pool_params->second);
    if (plan->cl_state) {
        const uint256& pool{plan->cl_state->first};
        SetClState(pool, plan->cl_state->second);
        for (const auto& [tick, value] : plan->cl_ticks) {
            if (value) {
                out.Put(TickKey(CL_TICKS, pool, tick), sidechain::EncodeValue(*value));
                out.Put(TickKey(CL_TICKS_DOWN, pool, tick), {});
            } else {
                out.Erase(TickKey(CL_TICKS, pool, tick));
                out.Erase(TickKey(CL_TICKS_DOWN, pool, tick));
            }
        }
    }
    if (plan->book_order) {
        const auto& [id, order]{*plan->book_order};
        const auto before{GetOrder(id)};
        if (order) {
            BOOK_ORDERS.Put(out, id, *order);
            // In the index while it has something left to sell.
            if (!before) {
                // Placed: into its book's index, its side's count (one for one when it took the worst's place), and the
                // count of its expiry.
                out.Put(BookKey(id, *order), {});
                if (!plan->evicted) AddCount(out, order->book, order->sells0, +1);
                BOOKS_TOUCHED.Put(out, order->book, 1);
                out.Put(ExpiryKey(id, order->expiry), {});
                if (BOOKS_LAYOUT.Get(out) != BOOKS_LAYOUT_CURRENT) BOOKS_LAYOUT.Put(out, BOOKS_LAYOUT_CURRENT);
            }
        } else {
            // Settled and gone: out of its book's index and count, if it was still there.
            if (before && out.Get(BookKey(id, *before))) {
                out.Erase(BookKey(id, *before));
                AddCount(out, before->book, before->sells0, -1);
            }
            BOOK_ORDERS.Erase(out, id);
        }
    }
    for (const uint256& id : plan->swept) {
        // Its expiry entry stays until the global sweep reaches it (which then finds it out of the index).
        if (const auto order{GetOrder(id)}; order && out.Get(BookKey(id, *order))) {
            out.Erase(BookKey(id, *order));
            AddCount(out, order->book, order->sells0, -1);
        }
    }
    if (plan->evicted) {
        const auto& [id, order]{*plan->evicted};
        out.Erase(BookKey(id, order));
        BOOK_ORDERS.Put(out, id, order);
    }
    if (plan->position) {
        if (plan->position->second) {
            CL_POSITIONS.Put(out, plan->position->first, *plan->position->second);
        } else {
            CL_POSITIONS.Erase(out, plan->position->first);
        }
    }
    if (plan->launch) {
        const auto& [token, record]{*plan->launch};
        LAUNCHES.Put(out, token, record);
        if (plan->launch_created) {
            out.Put(HeightKey(LAUNCHES_BY_HEIGHT, height, token), {});
            LaunchLogEntry entry;
            if (const auto bytes{out.Get(HeightKey(LAUNCH_LOG, height, token))}) entry = sidechain::DecodeValue<LaunchLogEntry>(*bytes);
            entry.created = true;
            entry.reserve_coin = record.virtual_coin;
            entry.reserve_tokens = LAUNCH_VIRTUAL_TOKENS;
            out.Put(HeightKey(LAUNCH_LOG, height, token), sidechain::EncodeValue(entry));
        }
    }
    for (const uint256& id : plan->pools_gone) {
        SetPool(id, std::nullopt);
        // Its kind goes with it (the pools AddLiquidity made have none recorded).
        if (POOL_PARAMS.Contains(out, id)) POOL_PARAMS.Erase(out, id);
    }
    // The name is free again; its number is not given again (lists skip it). Its history goes with it.
    if (plan->asset_gone) {
        ASSETS.Erase(out, *plan->asset_gone);
        std::vector<sidechain::StoreBytes> history;
        out.ForEach(Prefix(HISTORY, *plan->asset_gone), [&](const sidechain::StoreBytes& key, const sidechain::StoreBytes&) {
            history.push_back(key);
            return true;
        });
        for (const sidechain::StoreBytes& key : history) out.Erase(key);
        // And its logo: an asset registered again under the name starts without one.
        LOGOS.Erase(out, *plan->asset_gone);
        LOGO_BYTES.Erase(out, *plan->asset_gone);
        DISPLAY_NAMES.Erase(out, *plan->asset_gone);
    }
    if (plan->logo) {
        if (plan->logo->bytes.empty()) {
            LOGOS.Erase(out, plan->logo->asset);
            LOGO_BYTES.Erase(out, plan->logo->asset);
        } else {
            LogoRecord record;
            CSHA256().Write(plan->logo->bytes.data(), plan->logo->bytes.size()).Finalize(record.hash.data());
            record.format = static_cast<uint8_t>(plan->logo->format);
            record.size = static_cast<uint32_t>(plan->logo->bytes.size());
            record.height = height;
            record.txid = tx.GetHash();
            LOGOS.Put(out, plan->logo->asset, record);
            LOGO_BYTES.Put(out, plan->logo->asset, plan->logo->bytes);
        }
    }
    if (plan->display_name) {
        const auto& [asset, text] = *plan->display_name;
        if (text.empty()) {
            DISPLAY_NAMES.Erase(out, asset);
        } else {
            DISPLAY_NAMES.Put(out, asset, DisplayNameRecord{text, height, tx.GetHash()});
        }
    }
    if (released) *released = plan->released;
    if (plan->auction) SetAuction(plan->auction->first, plan->auction->second);
    // The DEX.
    if (plan->order) {
        Order order{*plan->order};
        // Its payout is its transaction's only one: the next place.
        if (!plan->payouts.empty()) order.payout = static_cast<int32_t>(payouts.size());
        const uint32_t n{ORDER_COUNT.Get(out)};
        PENDING_RESULTS.Put(out, COutPoint{order.txid, order.result_n}, 0);
        ORDERS.Put(out, n, order);
        ORDER_COUNT.Put(out, n + 1);
    }
    if (plan->oracle) ORACLES.Put(out, plan->oracle->first, plan->oracle->second);
    if (plan->attestation) out.Put(Key2(ATTESTATIONS, plan->attestation->first.first, plan->attestation->first.second), sidechain::EncodeValue(plan->attestation->second));
    if (plan->contract) {
        const auto& [id, contract]{*plan->contract};
        CONTRACTS.Put(out, id.ToUint256(), contract);
        if (!contract.asset.IsNull()) {
            const sidechain::StoreBytes key{Key2(CONTRACTS_BY_ASSET, contract.asset, id.ToUint256())};
            if (contract.settled) {
                out.Erase(key);
            } else {
                out.Put(key, {});
            }
        }
    }
    payouts.insert(payouts.end(), plan->payouts.begin(), plan->payouts.end());
    return true;
}

void State::FinishBlock(int height, uint32_t time, std::vector<CTxOut>& payouts)
{
    m_dex = true;
    sidechain::StoreOverlay& out{Writable()};
    // The pool kinds' work counts for its block only.
    if (CURVE_WORK.Get(out) != 0) CURVE_WORK.Put(out, 0);
    // The launches' log keeps the last LAUNCH_LOG_BLOCKS blocks.
    if (height >= LAUNCH_LOG_BLOCKS) {
        std::vector<sidechain::StoreBytes> old;
        out.ForEach(HeightPrefix(LAUNCH_LOG, height - LAUNCH_LOG_BLOCKS), [&](const sidechain::StoreBytes& key, const sidechain::StoreBytes&) {
            old.push_back(key);
            return true;
        });
        for (const auto& key : old) out.Erase(key);
    }
    std::vector<Order> orders;
    ORDERS.ForEach(out, [&](const uint32_t&, const Order& order) {
        orders.push_back(order);
        return true;
    });
    if (!orders.empty()) {
        ClearOrders(orders, payouts, height);
        // The places of CHN that are not paid (a CHN order filled, an order for CHN refunded): out of the
        // payouts, the last first, so that the others keep theirs. CHN paid are MIN_CHN_PAYOUT at least.
        std::vector<size_t> unused;
        for (uint32_t i{0}; i < orders.size(); ++i) {
            ORDERS.Erase(out, i);
            PENDING_RESULTS.Erase(out, COutPoint{orders[i].txid, orders[i].result_n});
            const int32_t at{orders[i].payout};
            if (at >= 0 && static_cast<size_t>(at) < payouts.size() && payouts[at].nValue == 0) unused.push_back(at);
        }
        std::sort(unused.rbegin(), unused.rend());
        for (const size_t at : unused) payouts.erase(payouts.begin() + at);
        ORDER_COUNT.Put(out, 0);
    }
    // Books with orders placed in the block: those that cross trade. Then the orders that expired out of their books.
    CrossBooks(height);
    SweepBooks(height);
    // The price accumulators of every pool the block changed (Uniswap v2): the price it had since its
    // last change, as the block found it, times the seconds since; then an observation of the block.
    std::vector<std::pair<uint256, PoolSnapshot>> changed;
    SNAPSHOTS.ForEach(out, [&](const uint256& id, const PoolSnapshot& snapshot) {
        changed.emplace_back(id, snapshot);
        return true;
    });
    for (const auto& [id, before] : changed) {
        SNAPSHOTS.Erase(out, id);
        const auto pool{GetPool(id)};
        if (!pool) continue;
        auto acc{ACCUMULATORS.Get(out, id)};
        if (!acc || acc->creation != pool->creation) {
            // A pool new to the accumulators (made, or first changed since the DEX's height): from now.
            acc = PoolAccumulator{};
            acc->creation = pool->creation;
            acc->last_time = time;
        } else if (time > acc->last_time) {
            const uint32_t elapsed{time - acc->last_time};
            // The prices of its kind (the reserves' ratios for constant product, as always; the curve's slope for the others;
            // a concentrated pool's square root price, squared, as it was before the block).
            const PoolParams params{GetPoolParams(id)};
            std::pair<uint256, uint256> prices;
            if (params.GetCurve() == Curve::BOOK) {
                // A book holds orders, not a curve: its reserves are no price. (No TWAP of books.)
                prices = {uint256{}, uint256{}};
            } else if (params.GetCurve() == Curve::CONCENTRATED) {
                const auto snapshot{CL_SNAPSHOTS.Get(out, id)};
                const auto now{GetClState(id)};
                const arith_uint256 sqrt{UintToArith256(snapshot ? *snapshot : now ? now->sqrt_price : uint256{})};
                prices = {cl::PriceQ64(sqrt), cl::PriceQ64(sqrt, true)};
            } else {
                prices = dex::PricesQ64(params, before.reserve0, before.reserve1);
            }
            const auto& [price0, price1]{prices};
            acc->price0_cumulative = dex::Accumulate(acc->price0_cumulative, price0, elapsed);
            acc->price1_cumulative = dex::Accumulate(acc->price1_cumulative, price1, elapsed);
            acc->last_time = time;
        }
        const Observation obs{height, acc->last_time, acc->price0_cumulative, acc->price1_cumulative, pool->reserve0, pool->reserve1};
        out.Put(ObservationKey(id, static_cast<uint32_t>(acc->count % TWAP_OBSERVATIONS)), sidechain::EncodeValue(obs));
        ++acc->count;
        ACCUMULATORS.Put(out, id, *acc);
        // A dynamic fee: the change of the price from the last block that traded to this one, into its moving average.
        const PoolParams kind{GetPoolParams(id)};
        if (kind.flags & POOL_FLAG_DYNAMIC_FEE) {
            uint256 now_price;
            if (kind.GetCurve() == Curve::CONCENTRATED) {
                const auto state{GetClState(id)};
                now_price = state ? cl::PriceQ64(UintToArith256(state->sqrt_price)) : uint256{};
            } else {
                now_price = dex::PricesQ64(kind, pool->reserve0, pool->reserve1).first;
            }
            FeeState fee{FEE_STATES.Get(out, id).value_or(FeeState{})};
            const arith_uint256 p1{UintToArith256(now_price)}, p0{UintToArith256(fee.last_price)};
            if (p0 != 0 && fee.last_height < height) {
                // |p1 − p0| / p0 in millionths, at most a whole (10^6).
                const arith_uint256 diff{p1 > p0 ? p1 - p0 : p0 - p1};
                const arith_uint256 change{diff * 1'000'000 / p0};
                const uint64_t step{change > arith_uint256{1'000'000} ? 1'000'000 : change.GetLow64()};
                fee.volatility = (Decayed(fee, height) * (DYNAMIC_WINDOW - 1) + step) / DYNAMIC_WINDOW;
            }
            fee.last_price = now_price;
            fee.last_height = height;
            FEE_STATES.Put(out, id, fee);
        }
    }
    // The prices of the concentrated pools before the block were for this block only.
    std::vector<uint256> cl_snapshots;
    CL_SNAPSHOTS.ForEach(out, [&](const uint256& id, const uint256&) {
        cl_snapshots.push_back(id);
        return true;
    });
    for (const uint256& id : cl_snapshots) CL_SNAPSHOTS.Erase(out, id);
}

void State::ClearOrders(const std::vector<Order>& orders, std::vector<CTxOut>& payouts, int height)
{
    const size_t n{orders.size()};
    using Pools = std::map<uint256, std::optional<Pool>>;
    const auto hop_pool{[&](size_t i, size_t hop) { return orders[i].PoolOfHop(hop); }};
    // The pools of the orders as the block's transactions left them, their kinds and fees (a launch curve's from its record).
    Pools start;
    struct Kind {
        PoolParams params;
        uint32_t fee{0};
        std::optional<curves::LaunchCurve> launch;
        AssetId token;
    };
    std::map<uint256, Kind> kinds;
    for (size_t i{0}; i < n; ++i) {
        for (size_t hop{0}; hop + 1 < orders[i].path.size(); ++hop) {
            const uint256 id{hop_pool(i, hop)};
            if (start.contains(id)) continue;
            const auto pool{GetPool(id)};
            start.emplace(id, pool);
            Kind kind;
            kind.params = GetPoolParams(id);
            kind.fee = FeeOf(id, kind.params, height);
            if (kind.params.GetCurve() == Curve::LAUNCH && pool) {
                kind.token = pool->asset1;
                if (const auto record{GetLaunch(pool->asset1)}) kind.launch = curves::LaunchCurve{record->fee, LAUNCH_CURVE_END, record->max_buy, record->min_buy};
            }
            kinds.emplace(id, kind);
        }
    }
    // What a pass does beyond the pools: a launch curve's fees, the coin it gives back to the orders it filled in part,
    // whether it reached its end, and what each did (its log).
    struct Effects {
        std::map<uint256, uint64_t> fees;
        std::set<uint256> ended;
        std::vector<uint64_t> refunds;
        std::map<uint256, LaunchLogEntry> log;
        //! Concentrated pools: their state and the ticks they crossed, as the pass left them.
        std::map<uint256, ClState> cl;
        std::map<uint256, std::map<int32_t, ClTick>> ticks;
        //! Books: their resting orders the pass changed.
        std::map<uint256, std::map<uint256, BookOrder>> books;
    };
    // The rounds of the pools whose curves are searched, by what they cleared: a pass that clears one again gets the same.
    struct RoundKey {
        uint256 pool;
        uint64_t reserve0, reserve1;
        std::vector<std::pair<bool, uint64_t>> ins;
        auto operator<=>(const RoundKey&) const = default;
    };
    std::map<RoundKey, std::optional<dex::Clearing>> rounds;
    // One pass: the orders in `active` cleared together on `pools`, a round per hop, the pools of a round
    // in the order of their ids; what each gets (nullopt: it misses).
    const auto pass{[&](const std::vector<bool>& active, Pools& pools, Effects& effects) {
        effects.refunds.assign(n, 0);
        std::vector<std::optional<uint64_t>> amount(n);
        for (size_t i{0}; i < n; ++i) {
            if (active[i]) amount[i] = orders[i].amount_in;
        }
        for (size_t hop{0}; hop + 1 < MAX_ROUTE_ASSETS; ++hop) {
            std::map<uint256, std::vector<size_t>> by_pool;
            for (size_t i{0}; i < n; ++i) {
                if (amount[i] && hop + 1 < orders[i].path.size()) by_pool[hop_pool(i, hop)].push_back(i);
            }
            for (const auto& [id, members] : by_pool) {
                std::optional<Pool>& pool{pools.at(id)};
                std::vector<dex::HopIn> ins;
                for (size_t i : members) ins.push_back({pool && pool->asset0 == orders[i].path[hop], *amount[i]});
                // A pool gone, or closed, since the order: it misses.
                // Of its kind (curves::ClearPoolOf: ClearPool's own result for the pools AddLiquidity makes). A round
                // the passes before cleared already (the same pool, reserves and orders) is not searched again.
                const Kind& kind{kinds.at(id)};
                std::optional<dex::Clearing> cleared;
                if (pool && !amm::Abandoned(*pool)) {
                    if (kind.params.GetCurve() == Curve::BOOK) {
                        // A book: its resting orders as this pass left them.
                        cleared = ClearBook(id, *pool, effects.books[id], ins, height);
                    } else if (kind.params.GetCurve() == Curve::CONCENTRATED) {
                        // A concentrated pool: its state and ticks as this pass left them.
                        auto it{effects.cl.find(id)};
                        if (it == effects.cl.end()) {
                            if (const auto state{GetClState(id)}) it = effects.cl.emplace(id, *state).first;
                        }
                        if (it != effects.cl.end()) cleared = ClearCl(id, kind.fee, *pool, it->second, effects.ticks[id], ins);
                    } else if (kind.params.GetCurve() == Curve::LAUNCH) {
                        // A launch curve: its coin is asset0. A buy paying the coin in at its first hop pays directly.
                        std::vector<bool> direct;
                        for (size_t i : members) direct.push_back(hop == 0 && orders[i].path[0].IsNull());
                        if (kind.launch) cleared = curves::ClearLaunch(*kind.launch, pool->reserve0, pool->reserve1, ins, direct);
                    } else {
                        RoundKey key{id, pool->reserve0, pool->reserve1, {}};
                        for (const dex::HopIn& in : ins) key.ins.emplace_back(in.zero_in, in.amount);
                        if (const auto memo{rounds.find(key)}; memo != rounds.end()) {
                            cleared = memo->second;
                        } else {
                            cleared = curves::ClearPoolOf(kind.params, kind.fee, pool->reserve0, pool->reserve1, ins);
                            if (kind.params.GetCurve() != Curve::CONSTANT_PRODUCT) rounds.emplace(std::move(key), cleared);
                        }
                    }
                }
                if (!cleared) {
                    for (size_t i : members) amount[i].reset();
                    continue;
                }
                pool->reserve0 = cleared->reserve0;
                pool->reserve1 = cleared->reserve1;
                if (kind.params.GetCurve() == Curve::LAUNCH) {
                    effects.fees[id] += cleared->fees;
                    if (cleared->reached_end) effects.ended.insert(id);
                    LaunchLogEntry& entry{effects.log[id]};
                    for (size_t j{0}; j < members.size(); ++j) {
                        if (cleared->outs[j] == 0) continue;
                        ++entry.orders;
                        if (ins[j].zero_in) {
                            entry.coin_in += ins[j].amount - cleared->refunds[j];
                            entry.tokens_out += cleared->outs[j];
                        } else {
                            entry.tokens_in += ins[j].amount;
                            entry.coin_out += cleared->outs[j];
                        }
                    }
                    entry.fees += cleared->fees;
                    entry.reserve_coin = pool->reserve0;
                    entry.reserve_tokens = pool->reserve1;
                }
                for (size_t j{0}; j < members.size(); ++j) {
                    uint64_t& volume{ins[j].zero_in ? pool->volume0 : pool->volume1};
                    volume = Add(volume, ins[j].amount).value_or(MAX_AMOUNT);
                    ++pool->swaps;
                    if (!cleared->refunds.empty()) effects.refunds[members[j]] += cleared->refunds[j];
                    if (cleared->outs[j] == 0) {
                        amount[members[j]].reset();
                    } else {
                        amount[members[j]] = cleared->outs[j];
                    }
                }
            }
        }
        // What comes out, against what each order asks (CHN: no less than is paid out).
        for (size_t i{0}; i < n; ++i) {
            if (amount[i] && (*amount[i] < orders[i].min_out || (orders[i].path.back().IsNull() && *amount[i] < MIN_CHN_PAYOUT))) amount[i].reset();
        }
        return amount;
    }};
    const auto pay{[&](const Order& order, const AssetId& asset, uint64_t amount) {
        if (asset.IsNull()) {
            if (order.payout >= 0 && static_cast<size_t>(order.payout) < payouts.size()) payouts[order.payout] = CTxOut{static_cast<CAmount>(amount), order.chn_to};
        } else {
            SetToken(COutPoint{order.txid, order.result_n}, Token{Token::Kind::ASSET, asset, amount});
        }
    }};
    const auto fill{[&](size_t i, uint64_t out, uint64_t back) {
        pay(orders[i], orders[i].path.back(), out);
        // What a launch curve at its end gave back of what was paid in (the chain's coin, by the coinbase).
        if (back > 0) pay(orders[i], CHN, back);
    }};
    const auto refund{[&](size_t i) { pay(orders[i], orders[i].path.front(), orders[i].amount_in); }};
    const auto commit{[&](const Pools& pools, const Effects& effects) {
        for (const auto& [id, pool] : pools) {
            if (pool && pool != start.at(id)) SetPool(id, *pool);
        }
        // Concentrated pools: their state and the ticks crossed.
        for (const auto& [id, state] : effects.cl) {
            if (GetClState(id) != state) SetClState(id, state);
        }
        for (const auto& [id, changed] : effects.ticks) {
            for (const auto& [tick, info] : changed) Writable().Put(TickKey(CL_TICKS, id, tick), sidechain::EncodeValue(info));
        }
        for (const auto& [id, changed] : effects.books) CommitBook(id, changed);
        // The launches' fees, split between the creator and the pool they graduate into; their log; graduations.
        sidechain::StoreOverlay& out{Writable()};
        for (const auto& [id, entry] : effects.log) {
            const Kind& kind{kinds.at(id)};
            auto record{GetLaunch(kind.token)};
            if (!record) continue;
            const uint64_t fees{effects.fees.contains(id) ? effects.fees.at(id) : 0};
            const uint64_t creator{static_cast<uint64_t>(static_cast<unsigned __int128>(fees) * record->creator_share / FEE_DENOMINATOR)};
            record->creator_fees += creator;
            record->pool_fees += fees - creator;
            record->volume = Add(record->volume, entry.coin_in + entry.coin_out).value_or(MAX_AMOUNT);
            ++record->trades;
            LAUNCHES.Put(out, kind.token, *record);
            LaunchLogEntry logged{entry};
            if (const auto bytes{out.Get(HeightKey(LAUNCH_LOG, height, kind.token))}) logged.created = sidechain::DecodeValue<LaunchLogEntry>(*bytes).created;
            out.Put(HeightKey(LAUNCH_LOG, height, kind.token), sidechain::EncodeValue(logged));
            const auto pool{GetPool(id)};
            // At the curve's end, or once it raised what it graduates at.
            if (pool && (effects.ended.contains(id) || pool->reserve0 >= record->virtual_coin + record->graduation)) Graduate(kind.token, height);
        }
    }};

    // Batch: clear every order together; take those that miss out and clear the rest again, from the start.
    std::vector<bool> active(n, true);
    for (int round{0}; round < MAX_CLEARING_PASSES; ++round) {
        Pools pools{start};
        Effects effects;
        const auto got{pass(active, pools, effects)};
        bool missed{false};
        for (size_t i{0}; i < n; ++i) {
            if (active[i] && !got[i]) {
                active[i] = false;
                missed = true;
            }
        }
        if (missed) continue;
        commit(pools, effects);
        for (size_t i{0}; i < n; ++i) {
            if (active[i]) {
                fill(i, *got[i], effects.refunds[i]);
            } else {
                refund(i);
            }
        }
        return;
    }
    // It did not settle: one after another, in the block's order, each alone against the pools as the
    // ones before left them; one that misses is refunded and leaves them as they were.
    Pools pools{start};
    Effects all;
    all.refunds.assign(n, 0);
    for (size_t i{0}; i < n; ++i) {
        Pools trial;
        for (size_t hop{0}; hop + 1 < orders[i].path.size(); ++hop) trial.emplace(hop_pool(i, hop), pools.at(hop_pool(i, hop)));
        std::vector<bool> one(n, false);
        one[i] = true;
        // From the concentrated pools as the orders before left them.
        Effects effects;
        effects.cl = all.cl;
        effects.ticks = all.ticks;
        effects.books = all.books;
        const auto got{pass(one, trial, effects)};
        if (!got[i]) {
            refund(i);
            continue;
        }
        for (auto& [id, pool] : trial) pools.at(id) = std::move(pool);
        for (const auto& [id, fee] : effects.fees) all.fees[id] += fee;
        all.ended.insert(effects.ended.begin(), effects.ended.end());
        all.cl = effects.cl;
        all.ticks = effects.ticks;
        all.books = effects.books;
        for (const auto& [id, entry] : effects.log) {
            LaunchLogEntry& e{all.log[id]};
            e.coin_in += entry.coin_in;
            e.coin_out += entry.coin_out;
            e.tokens_in += entry.tokens_in;
            e.tokens_out += entry.tokens_out;
            e.fees += entry.fees;
            e.orders += entry.orders;
            e.reserve_coin = entry.reserve_coin;
            e.reserve_tokens = entry.reserve_tokens;
        }
        fill(i, *got[i], effects.refunds[i]);
    }
    commit(pools, all);
}

uint32_t State::QuoteFee(const uint256& pool, const PoolParams& params) const
{
    const auto fee{GetFeeState(pool)};
    return FeeOf(pool, params, fee ? fee->last_height + 1 : 0);
}

namespace {
/** The most ticks a round of a concentrated pool crosses; the rest of its input finds no price. */
constexpr int MAX_CL_CROSSINGS{64};

/** A stretch of a walk through a concentrated pool: from a price to the next tick with liquidity, its liquidity, what it takes and gives in full. */
struct ClSegment {
    arith_uint256 start, target;
    unsigned __int128 liquidity{0};
    uint64_t in_before{0}, out_before{0}, in_full{0}, out_full{0};
    bool reaches{false};
    std::optional<int32_t> cross;
};

/** What `x` gives on a walk (the same as walking with x from the start: each stretch takes what reaching its end takes). */
uint64_t WalkOut(const std::vector<ClSegment>& segs, uint64_t x, bool zero_in)
{
    for (size_t i{0}; i < segs.size(); ++i) {
        const ClSegment& seg{segs[i]};
        const uint64_t rem{x - seg.in_before};
        if (seg.reaches && rem >= seg.in_full && i + 1 < segs.size()) continue;
        if (seg.reaches && rem >= seg.in_full) return seg.out_before + seg.out_full;
        return seg.out_before + cl::SwapStep(seg.start, seg.target, seg.liquidity, rem, zero_in).amount_out;
    }
    return 0;
}
} // namespace

std::optional<dex::Clearing> State::ClearCl(const uint256& id, uint32_t fee, const Pool& pool, ClState& cl, std::map<int32_t, ClTick>& ticks, const std::vector<dex::HopIn>& orders) const
{
    if (fee >= FEE_DENOMINATOR) return std::nullopt;
    uint64_t in0{0}, in1{0};
    for (const dex::HopIn& order : orders) {
        if (order.amount == 0 || order.amount > MAX_AMOUNT) return std::nullopt;
        uint64_t& sum{order.zero_in ? in0 : in1};
        if (order.amount > MAX_AMOUNT - sum) return std::nullopt;
        sum += order.amount;
    }
    if (in0 > MAX_AMOUNT - pool.reserve0 || in1 > MAX_AMOUNT - pool.reserve1) return std::nullopt;
    const uint64_t keep{FEE_DENOMINATOR - fee};
    const unsigned __int128 f{FEE_DENOMINATOR};
    const unsigned __int128 a_hat{static_cast<unsigned __int128>(in0) * keep}, b_hat{static_cast<unsigned __int128>(in1) * keep};
    const auto tick_at{[&](int32_t t) {
        if (const auto it{ticks.find(t)}; it != ticks.end()) return it->second;
        return GetClTick(id, t).value_or(ClTick{});
    }};
    const auto net_of{[](const ClTick& t) { return t.net_negative ? -static_cast<__int128>(t.NetAbs()) : static_cast<__int128>(t.NetAbs()); }};
    // The walk from the pool's price in one direction, for at most `max_in`, at most MAX_CL_CROSSINGS ticks crossed.
    bool broken{false};
    const auto walk{[&](bool zero_in, uint64_t max_in) {
        std::vector<ClSegment> segs;
        arith_uint256 price{UintToArith256(cl.sqrt_price)};
        int32_t tick{cl.tick};
        unsigned __int128 liquidity{cl.Liquidity()};
        uint64_t in_cum{0}, out_cum{0};
        const arith_uint256 limit{zero_in ? cl::MinSqrtRatio() + 1 : cl::MaxSqrtRatio() - 1};
        for (int crossed{0}; crossed <= MAX_CL_CROSSINGS; ++crossed) {
            const auto next{NextClTick(id, tick, /*up=*/!zero_in)};
            arith_uint256 target{next ? cl::SqrtRatioAtTick(*next) : limit};
            if (zero_in ? target < limit : target > limit) target = limit;
            const cl::Step full{cl::SwapStep(price, target, liquidity, max_in - in_cum, zero_in)};
            ClSegment seg{price, target, liquidity, in_cum, out_cum, full.amount_in, full.amount_out, full.sqrt_next == target, next};
            segs.push_back(seg);
            if (!seg.reaches || !next || crossed == MAX_CL_CROSSINGS || in_cum + full.amount_in > max_in) break;
            in_cum += full.amount_in;
            out_cum = out_cum > MAX_AMOUNT - full.amount_out ? MAX_AMOUNT : out_cum + full.amount_out;
            // Crossing the tick: its liquidity comes in going up, goes out going down. Never below zero, nor above the
            // cap (the ticks' caps make that impossible): a state that says otherwise trades nothing (no clamping).
            const auto crossed_l{CrossLiquidity(liquidity, net_of(tick_at(*next)), zero_in)};
            if (!crossed_l) {
                broken = true;
                break;
            }
            liquidity = *crossed_l;
            price = target;
            tick = zero_in ? *next - 1 : *next;
        }
        return segs;
    }};
    // The batch's uniform price, as ClearInvariant's: the largest net out the walk gives for the net in it gets.
    const uint64_t max_in0{static_cast<uint64_t>(a_hat / f)}, max_in1{static_cast<uint64_t>(b_hat / f)};
    // Â·dy (83 + 63 bits) overflows 128: in 256 bits; the quotient is below Â/F, so it fits in 64.
    const auto gain{[&](unsigned __int128 hat, unsigned __int128 other_hat, uint64_t d, uint64_t most) -> uint64_t {
        if (other_hat == 0) return most;
        const arith_uint256 num{UintToArith256(curves::BigUint::U128(hat).ToUint256()) * arith_uint256{d}};
        const arith_uint256 den{UintToArith256(curves::BigUint::U128(f * d + other_hat).ToUint256())};
        const arith_uint256 q{num / den};
        return q > arith_uint256{most} ? most : q.GetLow64();
    }};
    const auto x_gain{[&](uint64_t dy) { return gain(a_hat, b_hat, dy, max_in0); }};
    const auto y_gain{[&](uint64_t dx) { return gain(b_hat, a_hat, dx, max_in1); }};
    bool asset0_priced{true};
    unsigned __int128 num{0}, den{0};
    std::vector<ClSegment> segs;
    uint64_t swapped_in{0}, swapped_out{0};
    bool zero_walk{true};
    uint64_t dy{0};
    if (a_hat > 0 && max_in0 > 0) {
        segs = walk(true, max_in0);
        const uint64_t most{std::min<uint64_t>(WalkOut(segs, max_in0, true), pool.reserve1 > 0 ? pool.reserve1 - 1 : 0)};
        dy = curves::SearchMaxU64(0, most, [&](uint64_t d) { return WalkOut(segs, x_gain(d), true) >= d; });
    }
    if (a_hat > 0 && (dy > 0 || b_hat == 0)) {
        num = f * dy + b_hat;
        den = a_hat;
        swapped_in = x_gain(dy);
        swapped_out = dy > 0 || b_hat == 0 ? WalkOut(segs, swapped_in, true) : 0;
        if (dy == 0 && b_hat == 0) swapped_out = 0, swapped_in = 0;
    } else {
        uint64_t dx{0};
        if (b_hat > 0 && max_in1 > 0) {
            segs = walk(false, max_in1);
            const uint64_t most{std::min<uint64_t>(WalkOut(segs, max_in1, false), pool.reserve0 > 0 ? pool.reserve0 - 1 : 0)};
            dx = curves::SearchMaxU64(0, most, [&](uint64_t d) { return WalkOut(segs, y_gain(d), false) >= d; });
        }
        if (b_hat > 0 && (dx > 0 || a_hat == 0)) {
            asset0_priced = false;
            num = f * dx + a_hat;
            den = b_hat;
            zero_walk = false;
            swapped_in = dx > 0 ? y_gain(dx) : 0;
            swapped_out = dx > 0 ? WalkOut(segs, swapped_in, false) : 0;
        } else {
            num = b_hat;
            den = a_hat;
            segs.clear();
        }
    }
    dex::Clearing clearing;
    uint64_t out0{0}, out1{0};
    const arith_uint256 pnum{UintToArith256(curves::BigUint::U128(num).ToUint256())}, pden{UintToArith256(curves::BigUint::U128(den).ToUint256())}, f256{FEE_DENOMINATOR};
    for (const dex::HopIn& order : orders) {
        const arith_uint256 paid{arith_uint256{order.amount} * keep};
        const bool priced_side{order.zero_in == asset0_priced};
        arith_uint256 out;
        if (priced_side) {
            out = pden == 0 ? arith_uint256{} : paid * pnum / (pden * f256);
        } else {
            out = pnum == 0 ? arith_uint256{} : paid * pden / (pnum * f256);
        }
        const uint64_t got{out > arith_uint256{MAX_AMOUNT} ? MAX_AMOUNT : out.GetLow64()};
        clearing.outs.push_back(got);
        uint64_t& sum{order.zero_in ? out1 : out0};
        if (got > MAX_AMOUNT - sum) return std::nullopt;
        sum += got;
    }
    if (out0 > pool.reserve0 + in0 || out1 > pool.reserve1 + in1) return std::nullopt;
    // What the pool kept beyond the walk: the fees and the roundings, of each asset, for the liquidity in range.
    const __int128 net0{static_cast<__int128>(in0) - out0}, net1{static_cast<__int128>(in1) - out1};
    const __int128 surplus0{zero_walk ? net0 - static_cast<__int128>(swapped_in) : net0 + static_cast<__int128>(swapped_out)};
    const __int128 surplus1{zero_walk ? net1 + static_cast<__int128>(swapped_out) : net1 - static_cast<__int128>(swapped_in)};
    if (surplus0 < 0 || surplus1 < 0) return std::nullopt;
    if (broken) return std::nullopt;
    // The fees and roundings, credited to the liquidity of each stretch the walk went through, in proportion to the input
    // it took there, before the tick at its end is crossed (Uniswap v3 credits a step's fee to its liquidity, then
    // crosses): a position earns what the walk traded in its range. With no walk (the orders crossed each other), all to
    // the liquidity in range; with none in range, the pool keeps them (nobody's).
    const auto credit{[&](unsigned __int128 liquidity, unsigned __int128 part0, unsigned __int128 part1) {
        if (liquidity == 0) return;
        const curves::BigUint l{curves::BigUint::U128(liquidity)};
        cl.fee_growth0 = ArithToUint256(UintToArith256(cl.fee_growth0) + UintToArith256(((curves::BigUint::U128(part0) << 128) / l).ToUint256()));
        cl.fee_growth1 = ArithToUint256(UintToArith256(cl.fee_growth1) + UintToArith256(((curves::BigUint::U128(part1) << 128) / l).ToUint256()));
    }};
    const unsigned __int128 s0{static_cast<unsigned __int128>(surplus0)}, s1{static_cast<unsigned __int128>(surplus1)};
    // What each stretch took of the input, as the walk goes: its share of the surplus, the last the rest.
    std::vector<std::pair<size_t, uint64_t>> taken;
    {
        uint64_t rem{swapped_in};
        for (size_t i{0}; i < segs.size() && rem > 0; ++i) {
            const ClSegment& seg{segs[i]};
            const bool whole{seg.reaches && rem >= seg.in_full && i + 1 < segs.size()};
            const uint64_t used{whole ? seg.in_full : std::min(rem, cl::SwapStep(seg.start, seg.target, seg.liquidity, rem, zero_walk).amount_in)};
            taken.emplace_back(i, used);
            rem -= whole ? seg.in_full : rem;
            if (!whole) break;
        }
    }
    uint64_t total_taken{0};
    for (const auto& [i, used] : taken) total_taken += used;
    unsigned __int128 given0{0}, given1{0};
    const auto share{[&](size_t k) -> std::pair<unsigned __int128, unsigned __int128> {
        // The last stretch with liquidity gets the rest: nothing lost to the floors.
        size_t last{taken.size()};
        for (size_t m{taken.size()}; m-- > 0;) {
            if (segs[taken[m].first].liquidity > 0 && taken[m].second > 0) {
                last = m;
                break;
            }
        }
        if (k == last) return {s0 - given0, s1 - given1};
        if (total_taken == 0 || segs[taken[k].first].liquidity == 0) return {0, 0};
        const unsigned __int128 w{taken[k].second};
        const curves::BigUint big0{curves::BigUint::U128(s0) * curves::BigUint::U128(w) / curves::BigUint{total_taken}}, big1{curves::BigUint::U128(s1) * curves::BigUint::U128(w) / curves::BigUint{total_taken}};
        return {*big0.ToU128(), *big1.ToU128()};
    }};
    // The walk itself: the price, the tick, the liquidity in range, the ticks crossed (their outside fees turned over after
    // the stretch before them was credited).
    arith_uint256 price{UintToArith256(cl.sqrt_price)};
    int32_t tick{cl.tick};
    unsigned __int128 liquidity{cl.Liquidity()};
    bool any_credit{false};
    const auto cross{[&](int32_t t) -> bool {
        ClTick info{tick_at(t)};
        info.outside0 = ArithToUint256(UintToArith256(cl.fee_growth0) - UintToArith256(info.outside0));
        info.outside1 = ArithToUint256(UintToArith256(cl.fee_growth1) - UintToArith256(info.outside1));
        ticks[t] = info;
        const auto l{CrossLiquidity(liquidity, net_of(info), zero_walk)};
        if (!l) return false;
        liquidity = *l;
        return true;
    }};
    for (size_t k{0}; k < taken.size(); ++k) {
        const ClSegment& seg{segs[taken[k].first]};
        const auto [p0, p1]{share(k)};
        if (p0 > 0 || p1 > 0) {
            credit(seg.liquidity, p0, p1);
            any_credit |= seg.liquidity > 0;
            given0 += p0;
            given1 += p1;
        }
        const bool whole{k + 1 < taken.size()};
        if (whole && seg.cross) {
            if (!cross(*seg.cross)) return std::nullopt;
            price = seg.target;
            tick = zero_walk ? *seg.cross - 1 : *seg.cross;
            continue;
        }
        // The last stretch: where the price stops (on its tick: crossed, as Uniswap crosses a tick it lands on).
        const cl::Step step{cl::SwapStep(seg.start, seg.target, seg.liquidity, taken[k].second == 0 ? 0 : swapped_in - [&] {
            uint64_t before{0};
            for (size_t m{0}; m < k; ++m) before += taken[m].second;
            return before;
        }(), zero_walk)};
        price = step.sqrt_next;
        if (price == seg.target && seg.cross) {
            if (!cross(*seg.cross)) return std::nullopt;
            tick = zero_walk ? *seg.cross - 1 : *seg.cross;
        } else if (price != UintToArith256(cl.sqrt_price)) {
            tick = cl::TickAtSqrtRatio(price);
        }
    }
    // No walk (or none of it with liquidity): all to the liquidity in range.
    if (!any_credit) credit(liquidity, s0, s1);
    cl.sqrt_price = ArithToUint256(price);
    cl.tick = tick;
    cl.SetLiquidity(liquidity);
    clearing.reserve0 = pool.reserve0 + in0 - out0;
    clearing.reserve1 = pool.reserve1 + in1 - out1;
    return clearing;
}

std::optional<dex::Clearing> State::ClearBook(const uint256& id, const Pool& pool, std::map<uint256, BookOrder>& resting, const std::vector<dex::HopIn>& orders, int height) const
{
    uint64_t t0{0}, t1{0};
    for (const dex::HopIn& order : orders) {
        if (order.amount == 0 || order.amount > MAX_AMOUNT) return std::nullopt;
        uint64_t& sum{order.zero_in ? t0 : t1};
        if (order.amount > MAX_AMOUNT - sum) return std::nullopt;
        sum += order.amount;
    }
    if (t0 > MAX_AMOUNT - pool.reserve0 || t1 > MAX_AMOUNT - pool.reserve1) return std::nullopt;
    // The best resting orders of each side as this pass left them (expired ones out).
    const auto side{[&](bool asks) {
        std::vector<std::pair<uint256, BookOrder>> out;
        // Those this pass filled still read as live from the store: as many more, so they do not crowd out the rest.
        for (auto& [oid, order] : BookSide(id, asks, MAX_BOOK_ROUND + resting.size(), height)) {
            const auto it{resting.find(oid)};
            const BookOrder& now{it != resting.end() ? it->second : order};
            if (now.remaining > 0 && now.expiry >= height) out.emplace_back(oid, now);
        }
        return out;
    }};
    const auto asks{side(true)}, bids{side(false)};
    dex::Clearing clearing;
    clearing.outs.assign(orders.size(), 0);
    clearing.reserve0 = pool.reserve0 + t0;
    clearing.reserve1 = pool.reserve1 + t1;
    // Prices of asset0 in asset1 as rationals: an ask sells asset0 for at least num/den of asset1; a bid sells asset1 for at
    // least num/den of asset0, so it pays at most den/num of asset1 per asset0.
    struct Price {
        curves::BigUint num, den;
    };
    const auto less{[](const Price& a, const Price& b) { return a.num * b.den < b.num * a.den; }};
    std::vector<Price> candidates;
    for (const auto& [oid, o] : asks) candidates.push_back({curves::BigUint{o.price_num}, curves::BigUint{o.price_den}});
    for (const auto& [oid, o] : bids) candidates.push_back({curves::BigUint{o.price_den}, curves::BigUint{o.price_num}});
    if (candidates.empty()) {
        // No price: the takers miss (0 out); what came in is not the book's.
        clearing.reserve0 = pool.reserve0;
        clearing.reserve1 = pool.reserve1;
        return clearing;
    }
    std::sort(candidates.begin(), candidates.end(), less);
    // At a price P: what sells asset0 (takers and the asks at or under P) and what buys it (takers' and the bids' asset1, at
    // or over P, over P), in units of asset0 times P's denominator... compared as rationals.
    // Asks by their limit, lowest first, and bids by theirs (asset1 per asset0), highest first, with running sums: what
    // sells asset0 at a price P is the takers' and the asks' at or under P; what buys it, the takers' and the bids' asset1
    // at or over P. O(n log n) a round.
    std::vector<std::pair<Price, uint64_t>> ask_levels, bid_levels;
    for (const auto& [oid, o] : asks) ask_levels.push_back({{curves::BigUint{o.price_num}, curves::BigUint{o.price_den}}, o.remaining});
    for (const auto& [oid, o] : bids) bid_levels.push_back({{curves::BigUint{o.price_den}, curves::BigUint{o.price_num}}, o.remaining});
    std::stable_sort(ask_levels.begin(), ask_levels.end(), [&](const auto& a, const auto& b) { return less(a.first, b.first); });
    std::stable_sort(bid_levels.begin(), bid_levels.end(), [&](const auto& a, const auto& b) { return less(b.first, a.first); });
    std::vector<unsigned __int128> ask_sum(ask_levels.size() + 1, 0), bid_sum(bid_levels.size() + 1, 0);
    for (size_t i{0}; i < ask_levels.size(); ++i) ask_sum[i + 1] = ask_sum[i] + ask_levels[i].second;
    for (size_t i{0}; i < bid_levels.size(); ++i) bid_sum[i + 1] = bid_sum[i] + bid_levels[i].second;
    const auto supply{[&](const Price& p) {
        // The asks whose limit is not over P.
        const size_t n{static_cast<size_t>(std::partition_point(ask_levels.begin(), ask_levels.end(), [&](const auto& l) { return !less(p, l.first); }) - ask_levels.begin())};
        return curves::BigUint::U128(ask_sum[n] + t0);
    }};
    const auto spend{[&](const Price& p) {
        // The bids whose limit is not under P.
        const size_t n{static_cast<size_t>(std::partition_point(bid_levels.begin(), bid_levels.end(), [&](const auto& l) { return !less(l.first, p); }) - bid_levels.begin())};
        return curves::BigUint::U128(bid_sum[n] + t1);
    }};
    // The price that trades the most asset0 (min of supply and spend/P); then the least imbalance; then the lowest.
    std::optional<Price> best;
    curves::BigUint best_volume_num, best_volume_den{1}, best_gap_num, best_gap_den{1};
    for (const Price& p : candidates) {
        const curves::BigUint s{supply(p)}, d{spend(p)};
        // In asset0: supply s, demand d·den/num. volume = min; gap = |s − d·den/num|, both over num.
        const curves::BigUint s_n{s * p.num}, d_n{d * p.den};
        const curves::BigUint vol{s_n < d_n ? s_n : d_n};
        const curves::BigUint gap{s_n < d_n ? d_n - s_n : s_n - d_n};
        // Compare vol/num with best: cross-multiplied.
        const bool more{!best || vol * best_volume_den > best_volume_num * p.num};
        const bool same{best && vol * best_volume_den == best_volume_num * p.num};
        const bool tighter{same && gap * best_gap_den < best_gap_num * p.num};
        if (more || tighter) {
            best = p;
            best_volume_num = vol;
            best_volume_den = p.num;
            best_gap_num = gap;
            best_gap_den = p.num;
        }
    }
    if (best_volume_num.IsZero()) {
        clearing.reserve0 = pool.reserve0;
        clearing.reserve1 = pool.reserve1;
        return clearing;
    }
    const Price& p{*best};
    const auto fits{[](const curves::BigUint& v) { return std::min<uint64_t>(v.ToU128().value_or(MAX_AMOUNT) > MAX_AMOUNT ? MAX_AMOUNT : static_cast<uint64_t>(*v.ToU128()), MAX_AMOUNT); }};
    // The asset0 that trades: ⌊volume⌋.
    uint64_t left0{fits(best_volume_num / best_volume_den)};
    // The buy side first (takers, all or nothing, then the bids, best first): each gets ⌊paid/P⌋ of asset0 and pays ⌈got·P⌉.
    uint64_t delivered{0}, paid1{0}, taker_out0{0};
    std::vector<uint64_t> outs(orders.size(), 0);
    bool buyers_ok{true};
    for (size_t i{0}; i < orders.size(); ++i) {
        if (orders[i].zero_in) continue;
        const uint64_t got{fits(curves::BigUint{orders[i].amount} * p.den / p.num)};
        if (got > left0 || got == 0) {
            buyers_ok = false;
            continue;
        }
        outs[i] = got;
        left0 -= got;
        delivered += got;
        taker_out0 += got;
        paid1 += orders[i].amount;
    }
    if (!buyers_ok) {
        // Takers that pay asset1 fill whole or not at all: none of them does (they miss; the next pass goes without them).
        for (size_t i{0}; i < orders.size(); ++i) {
            if (!orders[i].zero_in) outs[i] = 0;
        }
        clearing.outs = outs;
        for (size_t i{0}; i < orders.size(); ++i) clearing.outs[i] = 0;
        clearing.reserve0 = pool.reserve0;
        clearing.reserve1 = pool.reserve1;
        return clearing;
    }
    for (const auto& [oid, o] : bids) {
        if (left0 == 0) break;
        if (less(Price{curves::BigUint{o.price_den}, curves::BigUint{o.price_num}}, p)) continue;
        const uint64_t most{fits(curves::BigUint{o.remaining} * p.den / p.num)};
        const uint64_t got{std::min(most, left0)};
        if (got == 0) continue;
        uint64_t pays{fits((curves::BigUint{got} * p.num + p.den - curves::BigUint{1}) / p.den)};
        pays = std::min(pays, o.remaining);
        BookOrder& now{resting.try_emplace(oid, o).first->second};
        now.remaining -= pays;
        now.proceeds += got;
        now.got += got;
        left0 -= got;
        delivered += got;
        paid1 += pays;
    }
    // The sell side delivers what the buy side got: takers first (all or nothing), then the asks, best first; each gets ⌊sold·P⌋.
    uint64_t to_sell{delivered}, taker_out1{0};
    for (size_t i{0}; i < orders.size(); ++i) {
        if (!orders[i].zero_in) continue;
        if (orders[i].amount > to_sell) {
            // A taker selling asset0 that the buyers cannot take whole: none of them fills.
            for (size_t k{0}; k < orders.size(); ++k) clearing.outs[k] = 0;
            clearing.reserve0 = pool.reserve0;
            clearing.reserve1 = pool.reserve1;
            return clearing;
        }
        outs[i] = fits(curves::BigUint{orders[i].amount} * p.num / p.den);
        to_sell -= orders[i].amount;
        taker_out1 += outs[i];
    }
    for (const auto& [oid, o] : asks) {
        if (to_sell == 0) break;
        if (less(p, Price{curves::BigUint{o.price_num}, curves::BigUint{o.price_den}})) continue;
        BookOrder& now{resting.try_emplace(oid, o).first->second};
        const uint64_t sells{std::min(now.remaining, to_sell)};
        const uint64_t gets{fits(curves::BigUint{sells} * p.num / p.den)};
        now.remaining -= sells;
        now.proceeds += gets;
        now.got += gets;
        to_sell -= sells;
    }
    // What the takers got leaves the book; what the makers got stays in it, theirs to collect. The roundings stay too.
    clearing.outs = outs;
    if (taker_out0 > clearing.reserve0 || taker_out1 > clearing.reserve1) return std::nullopt;
    clearing.reserve0 -= taker_out0;
    clearing.reserve1 -= taker_out1;
    return clearing;
}

void State::CommitBook(const uint256& id, const std::map<uint256, BookOrder>& changed)
{
    sidechain::StoreOverlay& out{Writable()};
    uint64_t filled{0};
    for (const auto& [oid, order] : changed) {
        const auto before{GetOrder(oid)};
        BOOK_ORDERS.Put(out, oid, order);
        // An order with nothing left to sell leaves the index (and the count of the book's open orders); it waits for its holder.
        if (order.remaining == 0 && before && before->remaining > 0 && out.Get(BookKey(oid, *before))) {
            out.Erase(BookKey(oid, *before));
            AddCount(out, before->book, before->sells0, -1);
            ++filled;
        }
    }
    if (filled > 0) {
        auto book{*Assert(GetPool(id))};
        book.shares -= std::min<uint64_t>(book.shares - std::min<uint64_t>(book.shares, MIN_LIQUIDITY), filled);
        SetPool(id, book);
    }
}

void State::CrossBooks(int height)
{
    sidechain::StoreOverlay& out{Writable()};
    std::vector<uint256> touched;
    BOOKS_TOUCHED.ForEach(out, [&](const uint256& id, const uint8_t&) {
        touched.push_back(id);
        return true;
    });
    for (const uint256& id : touched) {
        BOOKS_TOUCHED.Erase(out, id);
        const auto book{GetPool(id)};
        if (!book) continue;
        // The resting orders that cross one another trade at one price, as a round with no takers.
        std::map<uint256, BookOrder> changed;
        const auto cleared{ClearBook(id, *book, changed, {}, height)};
        if (!cleared || changed.empty()) continue;
        Pool after{*book};
        after.reserve0 = cleared->reserve0;
        after.reserve1 = cleared->reserve1;
        ++after.swaps;
        SetPool(id, after);
        CommitBook(id, changed);
    }
}

void State::Graduate(const AssetId& token, int height)
{
    sidechain::StoreOverlay& out{Writable()};
    LaunchRecord record{*Assert(GetLaunch(token))};
    if (record.Graduated()) return;
    const Pool curve{*Assert(GetPool(record.pool))};
    // What the curve raised (its coin less the virtual), the tokens it has left, and the fees kept for the pool.
    const uint64_t raised{curve.reserve0 - std::min(curve.reserve0, record.virtual_coin)};
    const uint64_t left{curve.reserve1 - std::min(curve.reserve1, LAUNCH_CURVE_END)};
    Pool pool;
    pool.asset0 = CHN;
    pool.asset1 = token;
    pool.reserve0 = raised + record.pool_fees;
    pool.reserve1 = left + LAUNCH_POOL_SUPPLY;
    // Every share locked: nobody holds any, and nobody ever can.
    pool.shares = Isqrt(static_cast<unsigned __int128>(pool.reserve0) * pool.reserve1);
    pool.creation = record.txid;
    pool.height = height;
    SetPool(record.pool, std::nullopt);
    if (POOL_PARAMS.Contains(out, record.pool)) POOL_PARAMS.Erase(out, record.pool);
    const uint256 id{PoolId(CHN, token)};
    SetPool(id, pool);
    record.graduated = height;
    record.raised_at_graduation = raised;
    record.pool_fees = 0;
    record.pool = id;
    LAUNCHES.Put(out, token, record);
    LaunchLogEntry entry;
    if (const auto bytes{out.Get(HeightKey(LAUNCH_LOG, height, token))}) entry = sidechain::DecodeValue<LaunchLogEntry>(*bytes);
    entry.graduated = true;
    entry.reserve_coin = pool.reserve0;
    entry.reserve_tokens = pool.reserve1;
    out.Put(HeightKey(LAUNCH_LOG, height, token), sidechain::EncodeValue(entry));
}

bool State::OrderFillsAlone(const CTransaction& tx, std::string& reject_reason) const
{
    const auto marker{GetMarker(tx)};
    if (!marker || !marker->operation) return true;
    std::vector<AssetId> path;
    uint64_t amount_in{0}, min_out{0};
    if (const auto* swap{std::get_if<Swap>(&*marker->operation)}) {
        path = {swap->asset_in, swap->asset_out};
        amount_in = swap->amount_in;
        min_out = swap->min_out;
    } else if (const auto* route{std::get_if<Route>(&*marker->operation)}) {
        path = route->path;
        amount_in = route->amount_in;
        min_out = route->min_out;
    } else if (const auto* named{std::get_if<SwapPools>(&*marker->operation)}) {
        const auto outs{dex::QuoteRoutePools(*this, named->asset_in, named->pools, named->amount_in, &path)};
        if (!outs || outs->back() < named->min_out || (path.back().IsNull() && outs->back() < MIN_CHN_PAYOUT)) {
            reject_reason = "ba-swap-price";
            return false;
        }
        return true;
    } else {
        return true;
    }
    const auto outs{dex::QuoteRoute(*this, path, amount_in)};
    if (!outs || outs->back() < min_out || (path.back().IsNull() && outs->back() < MIN_CHN_PAYOUT)) {
        reject_reason = "ba-swap-price";
        return false;
    }
    return true;
}

std::optional<PoolAccumulator> State::GetAccumulator(const uint256& pool) const { return ACCUMULATORS.Get(*m_view, pool); }
std::optional<Observation> State::GetObservation(const uint256& pool, uint32_t place) const
{
    const auto bytes{m_view->Get(ObservationKey(pool, place))};
    if (!bytes) return std::nullopt;
    return sidechain::DecodeValue<Observation>(*bytes);
}
std::optional<OracleRecord> State::GetOracle(const uint256& id) const { return ORACLES.Get(*m_view, id); }
std::optional<Attestation> State::GetAttestation(const uint256& oracle, const uint256& event) const
{
    const auto bytes{m_view->Get(Key2(ATTESTATIONS, oracle, event))};
    if (!bytes) return std::nullopt;
    return sidechain::DecodeValue<Attestation>(*bytes);
}
std::optional<OracleContract> State::GetContract(const Txid& id) const { return CONTRACTS.Get(*m_view, id.ToUint256()); }
PoolParams State::GetPoolParams(const uint256& pool) const { return POOL_PARAMS.Get(*m_view, pool).value_or(PoolParams{}); }
std::optional<LaunchRecord> State::GetLaunch(const AssetId& token) const { return LAUNCHES.Get(*m_view, token); }
std::optional<ClState> State::GetClState(const uint256& pool) const { return CL_STATES.Get(*m_view, pool); }
std::optional<ClTick> State::GetClTick(const uint256& pool, int32_t tick) const
{
    const auto bytes{m_view->Get(TickKey(CL_TICKS, pool, tick))};
    if (!bytes) return std::nullopt;
    return sidechain::DecodeValue<ClTick>(*bytes);
}
std::optional<ClPosition> State::GetPosition(const uint256& id) const { return CL_POSITIONS.Get(*m_view, id); }
std::vector<std::pair<int32_t, ClTick>> State::ClTicksOf(const uint256& pool, size_t max) const
{
    std::vector<std::pair<int32_t, ClTick>> ticks;
    m_view->ForEach(Prefix(CL_TICKS, pool), [&](const sidechain::StoreBytes& key, const sidechain::StoreBytes& value) {
        ticks.emplace_back(TickOfKey(CL_TICKS, key), sidechain::DecodeValue<ClTick>(value));
        return ticks.size() < max;
    });
    return ticks;
}
std::optional<int32_t> State::NextClTick(const uint256& pool, int32_t tick, bool up) const
{
    // Up: the first above `tick`; down: the first at or below it (Uniswap's nextInitializedTick).
    const uint8_t table{up ? CL_TICKS : CL_TICKS_DOWN};
    if (up && tick >= cl::MAX_TICK) return std::nullopt;
    const sidechain::StoreBytes from{TickKey(table, pool, up ? tick + 1 : tick)};
    const auto next{m_view->Next(from, Prefix(table, pool))};
    if (!next) return std::nullopt;
    return TickOfKey(table, next->first);
}
std::optional<FeeState> State::GetFeeState(const uint256& pool) const { return FEE_STATES.Get(*m_view, pool); }
std::optional<BookOrder> State::GetOrder(const uint256& id) const { return BOOK_ORDERS.Get(*m_view, id); }
std::vector<std::pair<uint256, BookOrder>> State::BookSide(const uint256& book, bool asks, size_t max, int height) const
{
    std::vector<std::pair<uint256, BookOrder>> side;
    if (max == 0) return side;
    sidechain::StoreBytes prefix{BOOK_INDEX};
    sidechain::KeyCodec<uint256>::Encode(prefix, book);
    prefix.push_back(asks ? 0 : 1);
    size_t scanned{0};
    m_view->ForEach(prefix, [&](const sidechain::StoreBytes& key, const sidechain::StoreBytes&) {
        std::span<const unsigned char> in{key};
        in = in.subspan(key.size() - 32);
        const uint256 id{sidechain::KeyCodec<uint256>::Decode(in)};
        // Expired (not swept yet) or filled: skipped before it counts.
        if (const auto order{GetOrder(id)}; order && order->remaining > 0 && order->expiry >= height && order->evicted == 0) side.emplace_back(id, *order);
        return side.size() < max && ++scanned < MAX_BOOK_SCAN;
    });
    return side;
}
uint32_t State::BookCount(const uint256& book, bool asks) const { return ReadCount(*m_view, book, asks); }
std::vector<std::pair<uint256, BookOrder>> State::BookEntries(const uint256& book, bool asks) const
{
    std::vector<std::pair<uint256, BookOrder>> side;
    sidechain::StoreBytes prefix{BOOK_INDEX};
    sidechain::KeyCodec<uint256>::Encode(prefix, book);
    prefix.push_back(asks ? 0 : 1);
    m_view->ForEach(prefix, [&](const sidechain::StoreBytes& key, const sidechain::StoreBytes&) {
        std::span<const unsigned char> in{key};
        in = in.subspan(key.size() - 32);
        const uint256 id{sidechain::KeyCodec<uint256>::Decode(in)};
        if (const auto order{GetOrder(id)}) side.emplace_back(id, *order);
        return side.size() < MAX_BOOK_SCAN;
    });
    return side;
}
size_t State::SweepBooks(int height, size_t budget)
{
    sidechain::StoreOverlay& out{Writable()};
    std::vector<std::pair<sidechain::StoreBytes, uint256>> due;
    out.ForEach(sidechain::StoreBytes{BOOK_EXPIRY}, [&](const sidechain::StoreBytes& key, const sidechain::StoreBytes&) {
        const uint32_t expiry{(uint32_t{key[1]} << 24) | (uint32_t{key[2]} << 16) | (uint32_t{key[3]} << 8) | key[4]};
        if (static_cast<int64_t>(expiry) > height || due.size() >= budget) return false;
        std::span<const unsigned char> in{key};
        in = in.subspan(5);
        due.emplace_back(key, sidechain::KeyCodec<uint256>::Decode(in));
        return true;
    });
    for (const auto& [key, id] : due) {
        out.Erase(key);
        const auto order{GetOrder(id)};
        if (!order) continue;
        const sidechain::StoreBytes place{BookKey(id, *order)};
        // Still open in its book: out of it (it stays, its holder's to settle), one open order less.
        if (out.Get(place)) {
            out.Erase(place);
            AddCount(out, order->book, order->sells0, -1);
            if (auto book{GetPool(order->book)}) {
                if (book->shares > MIN_LIQUIDITY) --book->shares;
                SetPool(order->book, *book);
            }
        }
    }
    return due.size();
}
uint32_t State::FeeOf(const uint256& pool, const PoolParams& params, int height) const
{
    if (!(params.flags & POOL_FLAG_DYNAMIC_FEE)) return params.fee;
    const auto fee{GetFeeState(pool)};
    if (!fee) return params.fee;
    // Half the recent average change of the price per block, in millionths: a pool whose price moves 1 % a block
    // charges 0.5 % more; at most DYNAMIC_MAX_EXTRA more.
    return params.fee + static_cast<uint32_t>(std::min<uint64_t>(Decayed(*fee, height) / 2, DYNAMIC_MAX_EXTRA));
}


std::pair<uint64_t, uint64_t> State::PositionFees(const ClPosition& position) const
{
    const auto cl{GetClState(position.pool)};
    const auto lo{GetClTick(position.pool, position.tick_lower)}, hi{GetClTick(position.pool, position.tick_upper)};
    if (!cl || !lo || !hi) return {position.owed0, position.owed1};
    const auto [in0, in1]{FeeGrowthInside(*cl, position.tick_lower, position.tick_upper, *lo, *hi)};
    const auto add{[](uint64_t a, uint64_t b) { return a > MAX_AMOUNT - b ? MAX_AMOUNT : a + b; }};
    return {add(position.owed0, Earned(in0, position.inside0_last, position.Liquidity())), add(position.owed1, Earned(in1, position.inside1_last, position.Liquidity()))};
}

void State::SetClState(const uint256& pool, const ClState& state)
{
    sidechain::StoreOverlay& out{Writable()};
    if (m_dex && !CL_SNAPSHOTS.Contains(out, pool)) {
        const auto before{GetClState(pool)};
        CL_SNAPSHOTS.Put(out, pool, before ? before->sqrt_price : state.sqrt_price);
    }
    CL_STATES.Put(out, pool, state);
}
bool State::Launching(const AssetId& asset) const
{
    if (asset.IsNull()) return false;
    const auto launch{GetLaunch(asset)};
    return launch && !launch->Graduated();
}
void State::ForEachLaunch(const std::function<bool(int32_t, const AssetId&)>& fn, int32_t from_height) const
{
    m_view->ForEach(sidechain::StoreBytes{LAUNCHES_BY_HEIGHT}, [&](const sidechain::StoreBytes& key, const sidechain::StoreBytes&) {
        int32_t height{0};
        for (int i{1}; i <= 4; ++i) height = static_cast<int32_t>((static_cast<uint32_t>(height) << 8) | key[i]);
        if (height < from_height) return true;
        std::span<const unsigned char> in{key};
        in = in.subspan(5);
        return fn(height, sidechain::KeyCodec<uint256>::Decode(in));
    });
}
std::vector<std::pair<AssetId, LaunchLogEntry>> State::GetLaunchLog(int32_t height) const
{
    std::vector<std::pair<AssetId, LaunchLogEntry>> entries;
    m_view->ForEach(HeightPrefix(LAUNCH_LOG, height), [&](const sidechain::StoreBytes& key, const sidechain::StoreBytes& value) {
        std::span<const unsigned char> in{key};
        in = in.subspan(5);
        entries.emplace_back(sidechain::KeyCodec<uint256>::Decode(in), sidechain::DecodeValue<LaunchLogEntry>(value));
        return true;
    });
    return entries;
}
bool State::HasPoolParams(const uint256& pool) const { return POOL_PARAMS.Contains(*m_view, pool); }
void State::ForEachOracle(const std::function<bool(const uint256&, const OracleRecord&)>& fn, const std::optional<uint256>& from) const { ORACLES.ForEach(*m_view, fn, from); }
void State::ForEachContract(const std::function<bool(const Txid&, const OracleContract&)>& fn) const
{
    CONTRACTS.ForEach(*m_view, [&](const uint256& id, const OracleContract& contract) { return fn(Txid::FromUint256(id), contract); });
}
void State::ForEachAttestation(const uint256& oracle, const std::function<bool(const uint256&, const Attestation&)>& fn) const
{
    m_view->ForEach(Prefix(ATTESTATIONS, oracle), [&](const sidechain::StoreBytes& key, const sidechain::StoreBytes& value) {
        std::span<const unsigned char> in{key};
        in = in.subspan(33);
        return fn(sidechain::KeyCodec<uint256>::Decode(in), sidechain::DecodeValue<Attestation>(value));
    });
}

CAmount State::HeldChn() const
{
    CAmount held{0};
    ForEachPool([&](const uint256& id, const Pool& pool) {
        // A launch curve holds what it raised: its coin less the virtual.
        if (pool.asset0 == CHN && GetPoolParams(id).GetCurve() == Curve::LAUNCH) {
            const auto launch{GetLaunch(pool.asset1)};
            held += static_cast<CAmount>(pool.reserve0 - std::min(pool.reserve0, launch ? launch->virtual_coin : 0));
            return true;
        }
        if (pool.asset0 == CHN) held += static_cast<CAmount>(pool.reserve0);
        if (pool.asset1 == CHN) held += static_cast<CAmount>(pool.reserve1);
        return true;
    });
    // The launches' fees not paid out yet: for the pools they graduate into, and their creators'.
    LAUNCHES.ForEach(*m_view, [&](const uint256&, const LaunchRecord& launch) {
        held += static_cast<CAmount>(launch.pool_fees + launch.creator_fees);
        return true;
    });
    ForEachAuction([&](const Txid&, const Auction& auction) {
        if (auction.closed) return true;
        if (auction.base == CHN) held += static_cast<CAmount>(auction.remaining);
        if (auction.quote == CHN) held += static_cast<CAmount>(auction.proceeds);
        return true;
    });
    // The DEX: CHN oracle contracts hold, until settled.
    ForEachContract([&](const Txid&, const OracleContract& contract) {
        if (!contract.settled && contract.asset == CHN) held += static_cast<CAmount>(contract.amount);
        return true;
    });
    return held;
}

} // namespace bitassets
