// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <addresstype.h>
#include <chainparams.h>
#include <core_io.h>
#include <drivechain/sidechain.h>
#include <key_io.h>
#include <pos/staking.h>
#include <rpc/util.h>
#include <sidechain/state.h>
#include <util/moneystr.h>
#include <util/strencodings.h>
#include <util/translation.h>
#include <wallet/coincontrol.h>
#include <wallet/rpc/util.h>
#include <wallet/spend.h>
#include <wallet/wallet.h>

#include <univalue.h>

namespace wallet {
namespace {

/** Make, sign and send a transaction with a single output of `value` and the script `script`. */
CTransactionRef SendScript(CWallet& wallet, const CScript& script, CAmount value)
{
    std::vector<CRecipient> recipients{{CNoDestination{script}, value, /*fSubtractFeeFromAmount=*/false}};
    CCoinControl coin_control;
    auto res{CreateTransaction(wallet, recipients, /*change_pos=*/std::nullopt, coin_control, /*sign=*/true)};
    if (!res) throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, util::ErrorString(res).original);
    wallet.CommitTransaction(res->tx, /*replaces_txid=*/std::nullopt, /*comment=*/std::nullopt, /*comment_to=*/std::nullopt);
    return res->tx;
}

/**
 * Make, sign and send a bond to `pubkey` of `amount`, owned by `owner`, who signs it: its amount and
 * the first coin the transaction spends, which only coin selection knows. The transaction is made
 * with a signature of the same size in its place (the fee stays right), then the owner's goes in,
 * then the inputs are signed.
 */
CTransactionRef SendBond(CWallet& wallet, const uint256& pubkey, const uint160& owner, const std::array<unsigned char, 64>& pop, CAmount amount)
{
    const std::vector<unsigned char> placeholder(CPubKey::COMPACT_SIGNATURE_SIZE, 0);
    std::vector<CRecipient> recipients{{CNoDestination{pos::BondScript(pubkey, owner, pop, placeholder)}, amount, /*fSubtractFeeFromAmount=*/false}};
    CCoinControl coin_control;
    auto res{CreateTransaction(wallet, recipients, /*change_pos=*/std::nullopt, coin_control, /*sign=*/false)};
    if (!res) throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, util::ErrorString(res).original);
    CMutableTransaction tx{*res->tx};
    {
        std::string signature_base64;
        const SigningResult signing{wallet.SignMessage(pos::BondMessage(pubkey, amount, tx.vin.at(0).prevout, Params().GetConsensus().hashGenesisBlock), PKHash{owner}, signature_base64)};
        if (signing != SigningResult::OK) throw JSONRPCError(RPC_WALLET_ERROR, "This wallet cannot sign for the bond's owner: " + SigningResultString(signing));
        const auto signature{*Assert(DecodeBase64(signature_base64))};
        const CScript placeholder_script{pos::BondScript(pubkey, owner, pop, placeholder)};
        bool replaced{false};
        for (CTxOut& out : tx.vout) {
            if (out.scriptPubKey != placeholder_script || replaced) continue;
            out.scriptPubKey = pos::BondScript(pubkey, owner, pop, signature);
            replaced = true;
        }
        if (!replaced) throw JSONRPCError(RPC_WALLET_ERROR, "The bond output is missing from the transaction made");
    }
    if (!WITH_LOCK(wallet.cs_wallet, return wallet.SignTransaction(tx))) throw JSONRPCError(RPC_WALLET_ERROR, "The wallet could not sign the bond's transaction");
    const CTransactionRef signed_tx{MakeTransactionRef(std::move(tx))};
    wallet.CommitTransaction(signed_tx, /*replaces_txid=*/std::nullopt, /*comment=*/std::nullopt, /*comment_to=*/std::nullopt);
    return signed_tx;
}

} // namespace

RPCMethod getdepositaddress()
{
    return RPCMethod{
        "getdepositaddress",
        "Returns a deposit address: what to give the wallet of the mainchain as the destination of a deposit to this\n"
        "sidechain. It names this sidechain and ends in a checksum, so that the mainchain wallet refuses it if it is\n"
        "mistyped or used for another sidechain.",
        {
            {"address", RPCArg::Type::STR, RPCArg::DefaultHint{"a new address of this wallet"}, "The address of this chain to credit"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR, "depositaddress", "The deposit address"},
            {RPCResult::Type::STR, "address", "The address of this chain that deposits to it are credited to"},
            {RPCResult::Type::NUM, "slot", "The slot of this sidechain on the mainchain"},
        }},
        RPCExamples{HelpExampleCli("getdepositaddress", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return UniValue::VNULL;
    const auto slot{pwallet->chain().getSidechainSlot()};
    if (!slot) throw JSONRPCError(RPC_MISC_ERROR, "This chain is not running as a sidechain");
    std::string address;
    if (request.params[0].isNull()) {
        const auto dest{pwallet->GetNewDestination(OutputType::BECH32, "deposit")};
        if (!dest) throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, util::ErrorString(dest).original);
        address = EncodeDestination(*dest);
    } else {
        address = request.params[0].get_str();
        if (!IsValidDestinationString(address)) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Not an address of this chain");
    }
    UniValue result(UniValue::VOBJ);
    result.pushKV("depositaddress", drivechain::FormatDepositAddress({*slot, address}));
    result.pushKV("address", address);
    result.pushKV("slot", *slot);
    return result;
},
    };
}

RPCMethod createwithdrawal()
{
    return RPCMethod{
        "createwithdrawal",
        "Withdraw coins to the mainchain. The coins leave this wallet at once. They are paid on the mainchain once the\n"
        "withdrawal is part of a withdrawal bundle that the miners of the mainchain have voted through, which takes long.\n"
        "Until the withdrawal is in a bundle it can be taken back with refundwithdrawal." +
        HELP_REQUIRING_PASSPHRASE,
        {
            {"mainchainaddress", RPCArg::Type::STR, RPCArg::Optional::NO, "The address on the mainchain to pay"},
            {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The amount in " + CURRENCY_UNIT + " to pay on the mainchain"},
            {"mainchainfee", RPCArg::Type::AMOUNT, RPCArg::Default{"0.0001"}, "The fee in " + CURRENCY_UNIT + " offered to the miners of the mainchain, on top of the amount. Withdrawals that offer more are bundled first."},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "txid", "The id of the transaction"},
            {RPCResult::Type::NUM, "vout", "The output that burned the coins; with the txid it identifies the withdrawal"},
            {RPCResult::Type::STR, "refundaddress", "The address of this wallet that a refund of the withdrawal would pay"},
        }},
        RPCExamples{HelpExampleCli("createwithdrawal", "\"mainchainaddress\" 1.5")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return UniValue::VNULL;
    pwallet->BlockUntilSyncedToCurrentChain();

    const CAmount amount{AmountFromValue(request.params[1])};
    const CAmount main_fee{request.params[2].isNull() ? 10000 : AmountFromValue(request.params[2])};
    if (amount <= 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "The amount must be positive");

    // Decoded by this node for its mainchain network (MainchainScript), never taken from the mainchain node.
    const auto main_script{pwallet->chain().getMainchainScript(request.params[0].get_str())};
    if (!main_script) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, util::ErrorString(main_script).original);
    if (main_script->size() > sidechain::MAX_MAIN_SCRIPT_SIZE) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "The script of this mainchain address is too long for a withdrawal");
    // Checked here, not left to the mempool: with -walletbroadcast=0 the wallet keeps the transaction
    // without offering it, and one that no block can take would hold its inputs until abandoned.
    if (const CAmount min_withdrawal{Params().GetConsensus().sidechain.min_withdrawal}; amount < min_withdrawal) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("The amount is below the minimum withdrawal of %s %s", FormatMoney(min_withdrawal), CURRENCY_UNIT));
    }

    EnsureWalletIsUnlocked(*pwallet);

    // A refund is paid to a key of this wallet, which has to sign the request for it.
    const auto refund_dest{pwallet->GetNewDestination(OutputType::BECH32, "withdrawal refund")};
    if (!refund_dest) throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, util::ErrorString(refund_dest).original);
    const WitnessV0KeyHash* keyhash{std::get_if<WitnessV0KeyHash>(&*refund_dest)};
    if (!keyhash) throw JSONRPCError(RPC_WALLET_ERROR, "The wallet cannot make a P2WPKH address for the refund");

    const CScript script{sidechain::WithdrawalScript(main_fee, uint160{*keyhash}, *main_script)};
    const CTransactionRef tx{SendScript(*pwallet, script, amount + main_fee)};
    if (CommittedTransactionRefused(*pwallet, tx)) {
        pwallet->AbandonTransaction(tx->GetHash());
        throw JSONRPCError(RPC_WALLET_ERROR, "The withdrawal was not accepted into the mempool; the amount may be below the minimum");
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", tx->GetHash().GetHex());
    for (uint32_t n{0}; n < tx->vout.size(); ++n) {
        if (tx->vout[n].scriptPubKey == script) result.pushKV("vout", n);
    }
    result.pushKV("refundaddress", EncodeDestination(*refund_dest));
    return result;
},
    };
}

RPCMethod refundwithdrawal()
{
    return RPCMethod{
        "refundwithdrawal",
        "Take back a withdrawal that this wallet made, as long as it is not in a withdrawal bundle. The first anchored block\n"
        "after the request (a block that takes the peg actions) pays the coins, the fee for the mainchain included, to the refund address of the withdrawal." +
        HELP_REQUIRING_PASSPHRASE,
        {
            {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The id of the transaction that made the withdrawal"},
            {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "The output of that transaction that burned the coins"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "txid", "The id of the transaction that asks for the refund"},
            {RPCResult::Type::STR_AMOUNT, "amount", "The amount that will be refunded"},
            {RPCResult::Type::STR, "refundaddress", "The address that will be paid"},
        }},
        RPCExamples{HelpExampleCli("refundwithdrawal", "\"txid\" 0")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return UniValue::VNULL;
    pwallet->BlockUntilSyncedToCurrentChain();

    const COutPoint outpoint{Txid::FromUint256(ParseHashV(request.params[0], "txid")), request.params[1].getInt<uint32_t>()};
    std::optional<sidechain::Withdrawal> withdrawal;
    {
        LOCK(pwallet->cs_wallet);
        const CWalletTx* wtx{pwallet->GetWalletTx(outpoint.hash)};
        if (!wtx || outpoint.n >= wtx->GetTx()->vout.size()) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "This wallet has no such transaction output");
        withdrawal = sidechain::ParseWithdrawalOutput(wtx->GetTx()->vout[outpoint.n]);
    }
    if (!withdrawal) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "This transaction output is not a withdrawal");

    EnsureWalletIsUnlocked(*pwallet);

    std::string signature_base64;
    const SigningResult signing{pwallet->SignMessage(sidechain::RefundMessage(outpoint), PKHash{withdrawal->refund_keyhash}, signature_base64)};
    if (signing != SigningResult::OK) throw JSONRPCError(RPC_WALLET_ERROR, "This wallet does not have the refund key of the withdrawal: " + SigningResultString(signing));
    sidechain::RefundRequest refund;
    refund.withdrawal = outpoint;
    refund.signature = *Assert(DecodeBase64(signature_base64));

    const CTransactionRef tx{SendScript(*pwallet, sidechain::RefundScript(refund), 0)};
    if (CommittedTransactionRefused(*pwallet, tx)) {
        pwallet->AbandonTransaction(tx->GetHash());
        throw JSONRPCError(RPC_WALLET_ERROR, "The refund was not accepted into the mempool: the withdrawal is not mined yet, is in a bundle, was paid, or was already refunded");
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", tx->GetHash().GetHex());
    result.pushKV("amount", ValueFromAmount(withdrawal->Burned()));
    result.pushKV("refundaddress", EncodeDestination(WitnessV0KeyHash{withdrawal->refund_keyhash}));
    return result;
},
    };
}


namespace {
uint256 ParseValidatorKey(const UniValue& value)
{
    const std::vector<unsigned char> bytes{ParseHexV(value, "validator_pubkey")};
    if (bytes.size() != 32) throw JSONRPCError(RPC_INVALID_PARAMETER, "validator_pubkey must be an x-only key: 32 bytes in hex");
    return uint256{std::span<const unsigned char>{bytes}};
}

interfaces::Chain::Staking EnsureStaking(CWallet& wallet)
{
    auto staking{wallet.chain().getStaking()};
    if (!staking) throw JSONRPCError(RPC_MISC_ERROR, "This chain has no staking (FastBlocks)");
    return std::move(*staking);
}

std::optional<interfaces::Chain::Bond> FindBond(const interfaces::Chain::Staking& staking, const uint256& pubkey)
{
    for (const auto& bond : staking.bonds) {
        if (bond.pubkey == pubkey) return bond;
    }
    return std::nullopt;
}

UniValue BondJSON(const interfaces::Chain::Bond& bond)
{
    UniValue entry(UniValue::VOBJ);
    entry.pushKV("validator", HexStr(bond.pubkey));
    entry.pushKV("owner", bond.owner.IsNull() ? std::string{} : EncodeDestination(WitnessV0KeyHash{bond.owner}));
    entry.pushKV("bonded", ValueFromAmount(bond.bonded));
    CAmount unbonding{0};
    UniValue unbonds(UniValue::VARR);
    for (const auto& [amount, release] : bond.unbonds) {
        unbonding += amount;
        UniValue item(UniValue::VOBJ);
        item.pushKV("amount", ValueFromAmount(amount));
        item.pushKV("releaseanchor", release);
        unbonds.push_back(std::move(item));
    }
    entry.pushKV("unbonding", ValueFromAmount(unbonding));
    entry.pushKV("unbonds", std::move(unbonds));
    entry.pushKV("unbondsequence", static_cast<uint64_t>(bond.unbond_seq));
    entry.pushKV("eligible", bond.eligible);
    entry.pushKV("inset", bond.in_set);
    entry.pushKV("jaileduntilepoch", bond.jailed_until_epoch);
    entry.pushKV("banned", bond.banned);
    return entry;
}
/** The owner of a bond to `pubkey` that this wallet made and that waits in the mempool, if any. */
std::optional<uint160> PendingBondOwner(const CWallet& wallet, const uint256& pubkey) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    AssertLockHeld(wallet.cs_wallet);
    for (const auto& [txid, wtx] : wallet.mapWallet) {
        if (!wtx.InMempool()) continue;
        for (const CTxOut& out : wtx.GetTx()->vout) {
            if (!pos::IsBondScript(out.scriptPubKey)) continue;
            const auto bond{pos::ParseBond(out)};
            if (bond && bond->pubkey == pubkey && !bond->owner.IsNull() && wallet.IsMine(GetScriptForDestination(WitnessV0KeyHash{bond->owner}))) return bond->owner;
        }
    }
    return std::nullopt;
}
} // namespace

RPCMethod createbond()
{
    return RPCMethod{
        "createbond",
        "Bond coins of this wallet to a validator key of FastBlocks: the coins leave the wallet into the chain's state, and the key\n"
        "takes part in the validator sets made at the anchors after (from the epoch after next; on eCash mainnet, whose sets are fixed set_lead_seconds before their epoch, about 33 hours later, or an hour or two during the bootstrap to epoch 72) while its bond is at least the\n"
        "minimum stake. The wallet owns the bond: it alone can unbond it (createunbond), and is paid what it unbonds. The validator\n"
        "key signs the bond (a proof of possession): by the node's block producer if it holds the key, or given as pop." +
        HELP_REQUIRING_PASSPHRASE,
        {
            {"validator_pubkey", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The validator's x-only key"},
            {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The amount in " + CURRENCY_UNIT + " to bond"},
            {"pop", RPCArg::Type::STR_HEX, RPCArg::DefaultHint{"signed by this node's validator key"}, "The proof of possession: the validator key's BIP340 signature of the bond's owner (signbondpop on the validator's node)"},
            {"owner", RPCArg::Type::STR, RPCArg::DefaultHint{"the bond's owner, or that of a bond of this wallet waiting for a block, or a new address of this wallet"}, "The P2WPKH address of this wallet that owns the bond (the pop names it: required with pop for a first bond)"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "txid", "The id of the transaction"},
            {RPCResult::Type::STR, "owner", "The address that owns the bond"},
            {RPCResult::Type::STR_AMOUNT, "bonded", "What the validator will have bonded"},
            {RPCResult::Type::BOOL, "eligible", "Whether that is enough to take part"},
        }},
        RPCExamples{HelpExampleCli("createbond", "\"<pubkey>\" 1")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return UniValue::VNULL;
    pwallet->BlockUntilSyncedToCurrentChain();
    const uint256 pubkey{ParseValidatorKey(request.params[0])};
    const CAmount amount{AmountFromValue(request.params[1])};
    if (amount <= 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "The amount must be positive");
    const auto staking{EnsureStaking(*pwallet)};
    const auto existing{FindBond(staking, pubkey)};
    if (existing && existing->banned) throw JSONRPCError(RPC_INVALID_PARAMETER, "This validator is banned: it signed two blocks for a slot");

    EnsureWalletIsUnlocked(*pwallet);
    // A bond of this wallet to the key that waits in the mempool names the owner the next must name
    // too (once it is mined, a bond naming another would never be).
    const std::optional<uint160> pending_owner{WITH_LOCK(pwallet->cs_wallet, return PendingBondOwner(*pwallet, pubkey))};
    // The owner: the bond's if it has one (more bonds name the same), else the pending bond's, else the
    // one given, else a new address.
    std::optional<uint160> given_owner;
    if (!request.params[3].isNull()) {
        const CTxDestination dest{DecodeDestination(request.params[3].get_str())};
        const WitnessV0KeyHash* keyhash{std::get_if<WitnessV0KeyHash>(&dest)};
        if (!keyhash) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "The owner must be a P2WPKH address");
        if (!WITH_LOCK(pwallet->cs_wallet, return pwallet->IsMine(dest))) throw JSONRPCError(RPC_WALLET_ERROR, "The owner is not an address of this wallet");
        given_owner = uint160{*keyhash};
    }
    uint160 owner;
    if (existing && !existing->owner.IsNull()) {
        owner = existing->owner;
        if (!WITH_LOCK(pwallet->cs_wallet, return pwallet->IsMine(GetScriptForDestination(WitnessV0KeyHash{owner})))) throw JSONRPCError(RPC_WALLET_ERROR, "Another wallet owns the bond of this validator");
    } else if (pending_owner) {
        owner = *pending_owner;
    } else if (given_owner) {
        owner = *given_owner;
    } else if (!request.params[2].isNull()) {
        // A proof of possession names its owner: a new address could not match it.
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Give the owner the proof of possession was signed for (the owner argument)");
    } else {
        const auto dest{pwallet->GetNewDestination(OutputType::BECH32, "bond")};
        if (!dest) throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, util::ErrorString(dest).original);
        const WitnessV0KeyHash* keyhash{std::get_if<WitnessV0KeyHash>(&*dest)};
        if (!keyhash) throw JSONRPCError(RPC_WALLET_ERROR, "The wallet cannot make a P2WPKH address for the bond's owner");
        owner = uint160{*keyhash};
    }
    if (given_owner && *given_owner != owner) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("The bond of this validator is owned by %s%s: a bond must name it", EncodeDestination(WitnessV0KeyHash{owner}),
                                                           existing && !existing->owner.IsNull() ? "" : " (a bond waiting for a block names it)"));
    }
    std::array<unsigned char, 64> pop;
    if (!request.params[2].isNull()) {
        const std::vector<unsigned char> bytes{ParseHexV(request.params[2], "pop")};
        if (bytes.size() != 64) throw JSONRPCError(RPC_INVALID_PARAMETER, "pop must be a 64-byte signature in hex");
        std::copy(bytes.begin(), bytes.end(), pop.begin());
    } else {
        const auto signed_pop{pwallet->chain().signBondPop(pubkey, owner)};
        if (!signed_pop) {
            const std::string address{EncodeDestination(WitnessV0KeyHash{owner})};
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("This node does not hold the validator key: run signbondpop %s %s on its node, then createbond %s %s <pop> %s (with %s as owner)",
                                                                address, HexStr(pubkey), HexStr(pubkey), FormatMoney(amount), address, address));
        }
        pop = *signed_pop;
    }
    if (!pos::CheckBondPop(pubkey, owner, pop, Params().GetConsensus().hashGenesisBlock)) throw JSONRPCError(RPC_INVALID_PARAMETER, "The proof of possession is not the validator key's signature for this owner");

    // The owner signs every bond (the first one too: its proof of possession is public).
    const CTransactionRef tx{SendBond(*pwallet, pubkey, owner, pop, amount)};
    if (CommittedTransactionRefused(*pwallet, tx)) {
        pwallet->AbandonTransaction(tx->GetHash());
        throw JSONRPCError(RPC_WALLET_ERROR, "The bond was not accepted into the mempool");
    }
    const CAmount bonded{(existing ? existing->bonded : 0) + amount};
    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", tx->GetHash().GetHex());
    result.pushKV("owner", EncodeDestination(WitnessV0KeyHash{owner}));
    result.pushKV("bonded", ValueFromAmount(bonded));
    result.pushKV("eligible", bonded >= staking.min_stake || (existing && existing->genesis_stake > 0));
    return result;
},
    };
}

RPCMethod createunbond()
{
    return RPCMethod{
        "createunbond",
        "Unbond coins this wallet bonded to a validator key of FastBlocks: they leave the validator's stake at once (its weight in the\n"
        "sets made from then on), and are paid to the bond's owner, in a coinbase, 1008 anchors later (about a week at an anchor\n"
        "every ten minutes): until then a proof that the validator signed two blocks for a slot can still take a share of them. What is left\n"
        "bonded must be nothing or at least the minimum stake." +
        HELP_REQUIRING_PASSPHRASE,
        {
            {"validator_pubkey", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The validator's x-only key"},
            {"amount", RPCArg::Type::AMOUNT, RPCArg::DefaultHint{"all"}, "The amount in " + CURRENCY_UNIT + " to unbond, or \"all\": all that is bonded when the unbond is mined (whatever was added since)"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "txid", "The id of the transaction"},
            {RPCResult::Type::STR_AMOUNT, "amount", "The amount unbonded (for all: what is bonded now)"},
            {RPCResult::Type::BOOL, "all", "Whether it unbonds all that is bonded when it is mined"},
            {RPCResult::Type::STR, "owner", "The address it will be paid to"},
            {RPCResult::Type::NUM, "releaseanchor", "About when: the anchor count it is paid at (if mined before the next anchor)"},
        }},
        RPCExamples{HelpExampleCli("createunbond", "\"<pubkey>\" 1")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return UniValue::VNULL;
    pwallet->BlockUntilSyncedToCurrentChain();
    const uint256 pubkey{ParseValidatorKey(request.params[0])};
    const auto staking{EnsureStaking(*pwallet)};
    const auto bond{FindBond(staking, pubkey)};
    if (!bond || bond->owner.IsNull() || bond->bonded <= 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "This validator has nothing bonded");
    const bool all{request.params[1].isNull() || (request.params[1].isStr() && request.params[1].get_str() == "all")};
    const CAmount amount{all ? bond->bonded : AmountFromValue(request.params[1])};
    if (amount <= 0 || amount > bond->bonded) throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("The amount must be positive and at most what is bonded (%s %s)", FormatMoney(bond->bonded), CURRENCY_UNIT));
    if (!all && amount != bond->bonded && bond->bonded - amount < staking.min_stake) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("What stays bonded must be nothing or at least the minimum stake of %s %s", FormatMoney(staking.min_stake), CURRENCY_UNIT));
    }

    // One unbond of a validator per block: a second with the same sequence could never be mined. Only
    // one in the mempool counts: one the mempool dropped (the state moved under it) never will be.
    {
        LOCK(pwallet->cs_wallet);
        for (const auto& [txid, wtx] : pwallet->mapWallet) {
            if (!wtx.InMempool()) continue;
            for (const CTxOut& out : wtx.GetTx()->vout) {
                const auto pending{pos::ParseUnbond(out)};
                if (pending && pending->pubkey == pubkey) {
                    throw JSONRPCError(RPC_WALLET_ERROR, strprintf("An unbond of this validator (%s) waits for a block: try again once it is mined", txid.GetHex()));
                }
            }
        }
    }

    EnsureWalletIsUnlocked(*pwallet);
    pos::Unbond unbond;
    unbond.pubkey = pubkey;
    // All of it: the form that unbonds all that is bonded when it is mined, so that a bond added
    // meanwhile leaves no remainder under the minimum stake.
    unbond.amount = all ? pos::UNBOND_ALL : amount;
    unbond.sequence = bond->unbond_seq;
    std::string signature_base64;
    const SigningResult signing{pwallet->SignMessage(pos::UnbondMessage(pubkey, unbond.amount, unbond.sequence, bond->first_bond, bond->first_bond_anchor, Params().GetConsensus().hashGenesisBlock), PKHash{bond->owner}, signature_base64)};
    if (signing != SigningResult::OK) throw JSONRPCError(RPC_WALLET_ERROR, "This wallet does not own the bond: " + SigningResultString(signing));
    unbond.signature = *Assert(DecodeBase64(signature_base64));

    const CTransactionRef tx{SendScript(*pwallet, pos::UnbondScript(unbond), 0)};
    if (CommittedTransactionRefused(*pwallet, tx)) {
        pwallet->AbandonTransaction(tx->GetHash());
        throw JSONRPCError(RPC_WALLET_ERROR, "The unbond was not accepted into the mempool");
    }
    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", tx->GetHash().GetHex());
    result.pushKV("amount", ValueFromAmount(amount));
    result.pushKV("all", all);
    result.pushKV("owner", EncodeDestination(WitnessV0KeyHash{bond->owner}));
    result.pushKV("releaseanchor", staking.anchors + static_cast<uint64_t>(Params().GetConsensus().pos.unbonding_anchors));
    return result;
},
    };
}

RPCMethod listbonds()
{
    return RPCMethod{
        "listbonds",
        "Lists the bonds this wallet owns on FastBlocks (or every validator's, with all): what is bonded, what is unbonding and when it is paid.",
        {
            {"all", RPCArg::Type::BOOL, RPCArg::Default{false}, "List every validator in the chain's state, not only this wallet's"},
        },
        RPCResult{RPCResult::Type::ARR, "", "",
        {
            {RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR_HEX, "validator", "The validator's x-only key"},
                {RPCResult::Type::STR, "owner", "The address that owns the bond"},
                {RPCResult::Type::STR_AMOUNT, "bonded", "Coins bonded"},
                {RPCResult::Type::STR_AMOUNT, "unbonding", "Coins unbonded, not paid yet"},
                {RPCResult::Type::ARR, "unbonds", "Them",
                {
                    {RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_AMOUNT, "amount", "The coins"},
                        {RPCResult::Type::NUM, "releaseanchor", "Paid at the anchor with this count"},
                    }},
                }},
                {RPCResult::Type::NUM, "unbondsequence", "The sequence the next unbond has"},
                {RPCResult::Type::BOOL, "eligible", "Whether its stake lets it take part"},
                {RPCResult::Type::BOOL, "inset", "Whether it is in the validator set of the current epoch"},
                {RPCResult::Type::NUM, "jaileduntilepoch", "Left out of the sets made before this epoch (0: never jailed)"},
                {RPCResult::Type::BOOL, "banned", "Signed two blocks for a slot: never in a set again"},
            }},
        }},
        RPCExamples{HelpExampleCli("listbonds", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return UniValue::VNULL;
    pwallet->BlockUntilSyncedToCurrentChain();
    const bool all{!request.params[0].isNull() && request.params[0].get_bool()};
    const auto staking{EnsureStaking(*pwallet)};
    UniValue result(UniValue::VARR);
    LOCK(pwallet->cs_wallet);
    for (const auto& bond : staking.bonds) {
        if (!all && (bond.owner.IsNull() || !pwallet->IsMine(GetScriptForDestination(WitnessV0KeyHash{bond.owner})))) continue;
        result.push_back(BondJSON(bond));
    }
    return result;
},
    };
}

} // namespace wallet
