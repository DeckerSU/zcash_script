// Copyright (c) 2018-2022 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "transaction_builder.h"

#include "main.h"
#include "proof_verifier.h"
#include "pubkey.h"
#include "rpc/protocol.h"
#include "script/sign.h"
#include "util/moneystr.h"
#include "zcash/Note.hpp"

#include <librustzcash.h>
#include <rust/ed25519.h>

uint256 ProduceZip244SignatureHash(
    const CTransaction& tx,
    const std::vector<CTxOut>& allPrevOutputs,
    const orchard::UnauthorizedBundle& orchardBundle)
{
    uint256 dataToBeSigned;
    PrecomputedTransactionData local(tx, allPrevOutputs);
    if (!zcash_builder_zip244_shielded_signature_digest(
        local.preTx.release(),
        orchardBundle.inner.get(),
        dataToBeSigned.begin()))
    {
        throw std::logic_error("ZIP 225 signature hash failed");
    }
    return dataToBeSigned;
}

namespace orchard {

Builder::Builder(
    bool spendsEnabled,
    bool outputsEnabled,
    uint256 anchor) : inner(nullptr, orchard_builder_free)
{
    inner.reset(orchard_builder_new(spendsEnabled, outputsEnabled, anchor.IsNull() ? nullptr : anchor.begin()));
}

bool Builder::AddSpend(orchard::SpendInfo spendInfo)
{
    if (!inner) {
        throw std::logic_error("orchard::Builder has already been used");
    }

    if (orchard_builder_add_spend(
        inner.get(),
        spendInfo.inner.release()))
    {
        hasActions = true;
        return true;
    } else {
        return false;
    }
}

void Builder::AddOutput(
    const std::optional<uint256>& ovk,
    const libzcash::OrchardRawAddress& to,
    CAmount value,
    const std::optional<std::array<unsigned char, ZC_MEMO_SIZE>>& memo)
{
    if (!inner) {
        throw std::logic_error("orchard::Builder has already been used");
    }

    orchard_builder_add_recipient(
        inner.get(),
        ovk.has_value() ? ovk->begin() : nullptr,
        to.inner.get(),
        value,
        memo.has_value() ? memo->data() : nullptr);

    hasActions = true;
}

std::optional<UnauthorizedBundle> Builder::Build() {
    if (!inner) {
        throw std::logic_error("orchard::Builder has already been used");
    }

    auto bundle = orchard_builder_build(inner.release());
    if (bundle == nullptr) {
        return std::nullopt;
    } else {
        return UnauthorizedBundle(bundle);
    }
}

std::optional<OrchardBundle> UnauthorizedBundle::ProveAndSign(
    const std::vector<libzcash::OrchardSpendingKey>& keys,
    uint256 sighash)
{
    if (!inner) {
        throw std::logic_error("orchard::UnauthorizedBundle has already been used");
    }

    std::vector<const OrchardSpendingKeyPtr*> pKeys;
    for (const auto& key : keys) {
        pKeys.push_back(key.inner.get());
    }

    auto authorizedBundle = orchard_unauthorized_bundle_prove_and_sign(
        inner.release(), pKeys.data(), pKeys.size(), sighash.begin());
    if (authorizedBundle == nullptr) {
        return std::nullopt;
    } else {
        return OrchardBundle(authorizedBundle);
    }
}

} // namespace orchard

SpendDescriptionInfo::SpendDescriptionInfo(
    libzcash::SaplingExpandedSpendingKey expsk,
    libzcash::SaplingNote note,
    uint256 anchor,
    SaplingWitness witness) : expsk(expsk), note(note), anchor(anchor), witness(witness)
{
    librustzcash_sapling_generate_r(alpha.begin());
}

std::optional<OutputDescription> OutputDescriptionInfo::Build(void* ctx) {
    auto cmu = this->note.cmu();
    if (!cmu) {
        return std::nullopt;
    }

    libzcash::SaplingNotePlaintext notePlaintext(this->note, this->memo);

    auto res = notePlaintext.encrypt(this->note.pk_d);
    if (!res) {
        return std::nullopt;
    }
    auto enc = res.value();
    auto encryptor = enc.second;

    libzcash::SaplingPaymentAddress address(this->note.d, this->note.pk_d);
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << address;
    std::vector<unsigned char> addressBytes(ss.begin(), ss.end());

    OutputDescription odesc;
    uint256 rcm = this->note.rcm();
    if (!librustzcash_sapling_output_proof(
            ctx,
            encryptor.get_esk().begin(),
            addressBytes.data(),
            rcm.begin(),
            this->note.value(),
            odesc.cv.begin(),
            odesc.zkproof.begin())) {
        return std::nullopt;
    }

    odesc.cmu = *cmu;
    odesc.ephemeralKey = encryptor.get_epk();
    odesc.encCiphertext = enc.first;

    libzcash::SaplingOutgoingPlaintext outPlaintext(this->note.pk_d, encryptor.get_esk());
    odesc.outCiphertext = outPlaintext.encrypt(
        this->ovk,
        odesc.cv,
        odesc.cmu,
        encryptor);

    return odesc;
}

JSDescription JSDescriptionInfo::BuildDeterministic(
    bool computeProof,
    uint256 *esk // payment disclosure
) {
    JSDescription jsdesc;
    jsdesc.vpub_old = vpub_old;
    jsdesc.vpub_new = vpub_new;
    jsdesc.anchor = anchor;

    std::array<libzcash::SproutNote, ZC_NUM_JS_OUTPUTS> notes;
    jsdesc.proof = ZCJoinSplit::prove(
        inputs,
        outputs,
        notes,
        jsdesc.ciphertexts,
        jsdesc.ephemeralKey,
        joinSplitPubKey,
        jsdesc.randomSeed,
        jsdesc.macs,
        jsdesc.nullifiers,
        jsdesc.commitments,
        vpub_old,
        vpub_new,
        anchor,
        computeProof,
        esk // payment disclosure
    );

    return jsdesc;
}

JSDescription JSDescriptionInfo::BuildRandomized(
    std::array<size_t, ZC_NUM_JS_INPUTS>& inputMap,
    std::array<size_t, ZC_NUM_JS_OUTPUTS>& outputMap,
    bool computeProof,
    uint256 *esk, // payment disclosure
    std::function<int(int)> gen
)
{
    // Randomize the order of the inputs and outputs
    inputMap = {0, 1};
    outputMap = {0, 1};

    assert(gen);

    MappedShuffle(inputs.begin(), inputMap.begin(), ZC_NUM_JS_INPUTS, gen);
    MappedShuffle(outputs.begin(), outputMap.begin(), ZC_NUM_JS_OUTPUTS, gen);

    return BuildDeterministic(computeProof, esk);
}

TransactionBuilderResult::TransactionBuilderResult(const CTransaction& tx) : maybeTx(tx) {}

TransactionBuilderResult::TransactionBuilderResult(const std::string& error) : maybeError(error) {}

bool TransactionBuilderResult::IsTx() { return maybeTx.has_value(); }

bool TransactionBuilderResult::IsError() { return maybeError.has_value(); }

CTransaction TransactionBuilderResult::GetTxOrThrow() {
    if (maybeTx.has_value()) {
        return maybeTx.value();
    } else {
        throw JSONRPCError(RPC_WALLET_ERROR, "Failed to build transaction: " + GetError());
    }
}

std::string TransactionBuilderResult::GetError() {
    if (maybeError.has_value()) {
        return maybeError.value();
    } else {
        // This can only happen if isTx() is true in which case we should not call getError()
        throw std::runtime_error("getError() was called in TransactionBuilderResult, but the result was not initialized as an error.");
    }
}

TransactionBuilder::TransactionBuilder(
    const Consensus::Params& consensusParams,
    int nHeight,
    std::optional<uint256> orchardAnchor,
    CKeyStore* keystore,
    CCoinsViewCache* coinsView,
    CCriticalSection* cs_coinsView) :
    consensusParams(consensusParams),
    nHeight(nHeight),
    keystore(keystore),
    coinsView(coinsView),
    cs_coinsView(cs_coinsView),
    orchardAnchor(orchardAnchor)
{
    mtx = CreateNewContextualCMutableTransaction(
            consensusParams, nHeight,
            !orchardAnchor.has_value() && nPreferredTxVersion < ZIP225_MIN_TX_VERSION);

    // Ignore the Orchard anchor if we can't use it yet.
    if (orchardAnchor.has_value() && mtx.nVersion >= ZIP225_MIN_TX_VERSION) {
        orchardBuilder = orchard::Builder(true, true, orchardAnchor.value());
    }
}

// This exception is thrown in certain scenarios when building JoinSplits fails.
struct JSDescException : public std::exception
{
    JSDescException (const std::string msg_) : msg(msg_) {}

    const char* what() { return msg.c_str(); }

private:
    std::string msg;
};

void TransactionBuilder::SetExpiryHeight(uint32_t nExpiryHeight)
{
    if (nExpiryHeight < nHeight || nExpiryHeight <= 0 || nExpiryHeight >= TX_EXPIRY_HEIGHT_THRESHOLD) {
        throw new std::runtime_error("TransactionBuilder::SetExpiryHeight: invalid expiry height");
    }
    mtx.nExpiryHeight = nExpiryHeight;
}

bool TransactionBuilder::SupportsOrchard() const {
    return orchardBuilder.has_value();
}

std::optional<uint256> TransactionBuilder::GetOrchardAnchor() const {
    return orchardAnchor;
}


bool TransactionBuilder::AddOrchardSpend(
    libzcash::OrchardSpendingKey sk,
    orchard::SpendInfo spendInfo)
{
    if (!orchardBuilder.has_value()) {
        // Try to give a useful error.
        if (!(jsInputs.empty() && jsOutputs.empty())) {
            throw std::runtime_error("TransactionBuilder cannot spend Orchard notes in a Sprout transaction");
        } else if (mtx.nVersion < ZIP225_MIN_TX_VERSION) {
            throw std::runtime_error("TransactionBuilder cannot spend Orchard notes before NU5 activation");
        } else {
            throw std::runtime_error("TransactionBuilder cannot spend Orchard notes without Orchard anchor");
        }
    }

    auto fromAddr = spendInfo.FromAddress();
    auto value = spendInfo.Value();
    auto res = orchardBuilder.value().AddSpend(std::move(spendInfo));
    if (res) {
        orchardSpendingKeys.push_back(sk);
        if (!firstOrchardSpendAddr.has_value()) {
            firstOrchardSpendAddr = fromAddr;
        }
        valueBalanceOrchard += value;
    }
    return res;
}

void TransactionBuilder::AddOrchardOutput(
    const std::optional<uint256>& ovk,
    const libzcash::OrchardRawAddress& to,
    CAmount value,
    const std::optional<std::array<unsigned char, ZC_MEMO_SIZE>>& memo)
{
    if (!orchardBuilder.has_value()) {
        // Try to give a useful error.
        if (!(jsInputs.empty() && jsOutputs.empty())) {
            throw std::runtime_error("TransactionBuilder cannot add Orchard output to Sprout transaction");
        } else if (mtx.nVersion < ZIP225_MIN_TX_VERSION) {
            throw std::runtime_error("TransactionBuilder cannot add Orchard output before NU5 activation");
        } else {
            throw std::runtime_error("TransactionBuilder cannot add Orchard output without Orchard anchor");
        }
    }

    orchardBuilder.value().AddOutput(ovk, to, value, memo);
    valueBalanceOrchard -= value;
}

void TransactionBuilder::AddSaplingSpend(
    libzcash::SaplingExpandedSpendingKey expsk,
    libzcash::SaplingNote note,
    uint256 anchor,
    SaplingWitness witness)
{
    // Sanity check: cannot add Sapling spend to pre-Sapling transaction
    if (mtx.nVersion < SAPLING_TX_VERSION) {
        throw std::runtime_error("TransactionBuilder cannot add Sapling spend to pre-Sapling transaction");
    }

    // Consistency check: all anchors must equal the first one
    if (spends.size() > 0 && spends[0].anchor != anchor) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Anchor does not match previously-added Sapling spends.");
    }

    spends.emplace_back(expsk, note, anchor, witness);
    mtx.valueBalanceSapling += note.value();
}

void TransactionBuilder::AddSaplingOutput(
    uint256 ovk,
    libzcash::SaplingPaymentAddress to,
    CAmount value,
    std::array<unsigned char, ZC_MEMO_SIZE> memo)
{
    // Sanity check: cannot add Sapling output to pre-Sapling transaction
    if (mtx.nVersion < SAPLING_TX_VERSION) {
        throw std::runtime_error("TransactionBuilder cannot add Sapling output to pre-Sapling transaction");
    }

    libzcash::Zip212Enabled zip_212_enabled = libzcash::Zip212Enabled::BeforeZip212;
    // We use nHeight = chainActive.Height() + 1 since the output will be included in the next block
    if (consensusParams.NetworkUpgradeActive(nHeight, Consensus::UPGRADE_CANOPY)) {
        zip_212_enabled = libzcash::Zip212Enabled::AfterZip212;
    }

    auto note = libzcash::SaplingNote(to, value, zip_212_enabled);
    outputs.emplace_back(ovk, note, memo);
    mtx.valueBalanceSapling -= value;
}

void TransactionBuilder::AddSproutInput(
    libzcash::SproutSpendingKey sk,
    libzcash::SproutNote note,
    SproutWitness witness)
{
    CheckOrSetUsingSprout();

    // Consistency check: all anchors must equal the first one
    if (!jsInputs.empty()) {
        if (jsInputs[0].witness.root() != witness.root()) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Anchor does not match previously-added Sprout spends.");
        }
    }

    jsInputs.emplace_back(witness, note, sk);
}

void TransactionBuilder::AddSproutOutput(
    libzcash::SproutPaymentAddress to,
    CAmount value,
    std::array<unsigned char, ZC_MEMO_SIZE> memo)
{
    CheckOrSetUsingSprout();

    libzcash::JSOutput jsOutput(to, value);
    jsOutput.memo = memo;
    jsOutputs.push_back(jsOutput);
}

void TransactionBuilder::AddTransparentInput(COutPoint utxo, CScript scriptPubKey, CAmount value)
{
    if (keystore == nullptr) {
        throw std::runtime_error("Cannot add transparent inputs to a TransactionBuilder without a keystore");
    }

    mtx.vin.emplace_back(utxo);
    tIns.emplace_back(value, scriptPubKey);
}

void TransactionBuilder::AddTransparentOutput(const CTxDestination& to, CAmount value)
{
    if (!IsValidDestination(to)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid output address, not a valid taddr.");
    }

    CScript scriptPubKey = GetScriptForDestination(to);
    CTxOut out(value, scriptPubKey);
    mtx.vout.push_back(out);
}

void TransactionBuilder::SetFee(CAmount fee)
{
    this->fee = fee;
}

// TODO: remove support for transparent change?
void TransactionBuilder::SendChangeTo(
        const libzcash::RecipientAddress& changeAddr,
        const uint256& ovk) {
    tChangeAddr = std::nullopt;
    orchardChangeAddr = std::nullopt;
    saplingChangeAddr = std::nullopt;
    sproutChangeAddr = std::nullopt;

    std::visit(match {
        [&](const CKeyID& keyId) {
            tChangeAddr = keyId;
        },
        [&](const CScriptID& scriptId) {
            tChangeAddr = scriptId;
        },
        [&](const libzcash::SaplingPaymentAddress& changeDest) {
            saplingChangeAddr = std::make_pair(ovk, changeDest);
        },
        [&](const libzcash::OrchardRawAddress& changeDest) {
            orchardChangeAddr = std::make_pair(ovk, changeDest);
        }
    }, changeAddr);
}

void TransactionBuilder::SendChangeToSprout(const libzcash::SproutPaymentAddress& zaddr) {
    tChangeAddr = std::nullopt;
    orchardChangeAddr = std::nullopt;
    saplingChangeAddr = std::nullopt;
    sproutChangeAddr = zaddr;
}

TransactionBuilderResult TransactionBuilder::Build()
{
    //
    // Consistency checks
    //

    // Valid change
    CAmount change = mtx.valueBalanceSapling + valueBalanceOrchard - fee;
    for (auto jsInput : jsInputs) {
        change += jsInput.note.value();
    }
    for (auto jsOutput : jsOutputs) {
        change -= jsOutput.value;
    }
    for (auto tIn : tIns) {
        change += tIn.nValue;
    }
    for (auto tOut : mtx.vout) {
        change -= tOut.nValue;
    }
    if (change < 0) {
        return TransactionBuilderResult("Change cannot be negative");
    }

    //
    // Change output
    //

    if (change > 0) {
        // Send change to the specified change address. If no change address
        // was set, send change to the first Sapling address given as input
        // if any; otherwise the first Sprout address given as input.
        // (A t-address can only be used as the change address if explicitly set.)
        if (orchardChangeAddr) {
            AddOrchardOutput(orchardChangeAddr->first, orchardChangeAddr->second, change, std::nullopt);
        } else if (saplingChangeAddr) {
            AddSaplingOutput(saplingChangeAddr->first, saplingChangeAddr->second, change);
        } else if (sproutChangeAddr) {
            AddSproutOutput(sproutChangeAddr.value(), change);
        } else if (tChangeAddr) {
            // tChangeAddr has already been validated.
            AddTransparentOutput(tChangeAddr.value(), change);
        } else if (firstOrchardSpendAddr.has_value()) {
            auto ovk = orchardSpendingKeys[0].ToFullViewingKey().ToInternalOutgoingViewingKey();
            AddOrchardOutput(ovk, firstOrchardSpendAddr.value(), change, std::nullopt);
        } else if (!spends.empty()) {
            auto fvk = spends[0].expsk.full_viewing_key();
            auto note = spends[0].note;
            libzcash::SaplingPaymentAddress changeAddr(note.d, note.pk_d);
            AddSaplingOutput(fvk.ovk, changeAddr, change);
        } else if (!jsInputs.empty()) {
            auto changeAddr = jsInputs[0].key.address();
            AddSproutOutput(changeAddr, change);
        } else {
            return TransactionBuilderResult("Could not determine change address");
        }
    }

    //
    // Orchard
    //

    std::optional<orchard::UnauthorizedBundle> orchardBundle;
    if (orchardBuilder.has_value() && orchardBuilder->HasActions()) {
        auto bundle = orchardBuilder->Build();
        if (bundle.has_value()) {
            orchardBundle = std::move(bundle);
        } else {
            return TransactionBuilderResult("Failed to build Orchard bundle");
        }
    }

    //
    // Sapling spends and outputs
    //

    auto ctx = librustzcash_sapling_proving_ctx_init();

    // Create Sapling SpendDescriptions
    for (auto spend : spends) {
        auto cm = spend.note.cmu();
        auto nf = spend.note.nullifier(
            spend.expsk.full_viewing_key(), spend.witness.position());
        if (!cm || !nf) {
            librustzcash_sapling_proving_ctx_free(ctx);
            return TransactionBuilderResult("Spend is invalid");
        }

        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
        ss << spend.witness.path();
        std::vector<unsigned char> witness(ss.begin(), ss.end());

        SpendDescription sdesc;
        uint256 rcm = spend.note.rcm();
        if (!librustzcash_sapling_spend_proof(
                ctx,
                spend.expsk.full_viewing_key().ak.begin(),
                spend.expsk.nsk.begin(),
                spend.note.d.data(),
                rcm.begin(),
                spend.alpha.begin(),
                spend.note.value(),
                spend.anchor.begin(),
                witness.data(),
                sdesc.cv.begin(),
                sdesc.rk.begin(),
                sdesc.zkproof.data())) {
            librustzcash_sapling_proving_ctx_free(ctx);
            return TransactionBuilderResult("Spend proof failed");
        }

        sdesc.anchor = spend.anchor;
        sdesc.nullifier = *nf;
        mtx.vShieldedSpend.push_back(sdesc);
    }

    // Create Sapling OutputDescriptions
    for (auto output : outputs) {
        // Check this out here as well to provide better logging.
        if (!output.note.cmu()) {
            librustzcash_sapling_proving_ctx_free(ctx);
            return TransactionBuilderResult("Output is invalid");
        }

        auto odesc = output.Build(ctx);
        if (!odesc) {
            librustzcash_sapling_proving_ctx_free(ctx);
            return TransactionBuilderResult("Failed to create output description");
        }

        mtx.vShieldedOutput.push_back(odesc.value());
    }

    //
    // Sprout JoinSplits
    //

    Ed25519SigningKey joinSplitPrivKey;
    ed25519_generate_keypair(&joinSplitPrivKey, &mtx.joinSplitPubKey);

    // Create Sprout JSDescriptions
    if (!jsInputs.empty() || !jsOutputs.empty()) {
        try {
            CreateJSDescriptions();
        } catch (JSDescException e) {
            librustzcash_sapling_proving_ctx_free(ctx);
            return TransactionBuilderResult(e.what());
        } catch (std::runtime_error e) {
            librustzcash_sapling_proving_ctx_free(ctx);
            throw e;
        }
    }

    //
    // Signatures
    //

    auto consensusBranchId = CurrentEpochBranchId(nHeight, consensusParams);

    // Empty output script.
    uint256 dataToBeSigned;
    try {
        if (orchardBundle.has_value()) {
            // Orchard is only usable with v5+ transactions.
            dataToBeSigned = ProduceZip244SignatureHash(mtx, tIns, orchardBundle.value());
        } else {
            CScript scriptCode;
            const PrecomputedTransactionData txdata(mtx, tIns);
            dataToBeSigned = SignatureHash(scriptCode, mtx, NOT_AN_INPUT, SIGHASH_ALL, 0, consensusBranchId, txdata);
        }
    } catch (std::ios_base::failure ex) {
        librustzcash_sapling_proving_ctx_free(ctx);
        return TransactionBuilderResult("Could not construct signature hash: " + std::string(ex.what()));
    } catch (std::logic_error ex) {
        librustzcash_sapling_proving_ctx_free(ctx);
        return TransactionBuilderResult("Could not construct signature hash: " + std::string(ex.what()));
    }

    if (orchardBundle.has_value()) {
        auto authorizedBundle = orchardBundle.value().ProveAndSign(
            orchardSpendingKeys, dataToBeSigned);
        if (authorizedBundle.has_value()) {
            mtx.orchardBundle = authorizedBundle.value();
        } else {
            return TransactionBuilderResult("Failed to create Orchard proof or signatures");
        }
    }

    // Create Sapling spendAuth and binding signatures
    for (size_t i = 0; i < spends.size(); i++) {
        librustzcash_sapling_spend_sig(
            spends[i].expsk.ask.begin(),
            spends[i].alpha.begin(),
            dataToBeSigned.begin(),
            mtx.vShieldedSpend[i].spendAuthSig.data());
    }
    librustzcash_sapling_binding_sig(
        ctx,
        mtx.valueBalanceSapling,
        dataToBeSigned.begin(),
        mtx.bindingSig.data());

    librustzcash_sapling_proving_ctx_free(ctx);

    // Create Sprout joinSplitSig
    if (!ed25519_sign(
        &joinSplitPrivKey,
        dataToBeSigned.begin(), 32,
        &mtx.joinSplitSig))
    {
        return TransactionBuilderResult("Failed to create Sprout joinSplitSig");
    }

    // Sanity check Sprout joinSplitSig
    if (!ed25519_verify(
        &mtx.joinSplitPubKey,
        &mtx.joinSplitSig,
        dataToBeSigned.begin(), 32))
    {
        return TransactionBuilderResult("Sprout joinSplitSig sanity check failed");
    }

    // Transparent signatures
    CTransaction txNewConst(mtx);
    const PrecomputedTransactionData txdata(txNewConst, tIns);
    for (int nIn = 0; nIn < mtx.vin.size(); nIn++) {
        auto tIn = tIns[nIn];
        SignatureData sigdata;
        bool signSuccess = ProduceSignature(
            TransactionSignatureCreator(
                keystore, &txNewConst, txdata, nIn, tIn.nValue, SIGHASH_ALL),
            tIn.scriptPubKey, sigdata, consensusBranchId);

        if (!signSuccess) {
            return TransactionBuilderResult("Failed to sign transaction");
        } else {
            UpdateTransaction(mtx, nIn, sigdata);
        }
    }

    return TransactionBuilderResult(CTransaction(mtx));
}

void TransactionBuilder::CheckOrSetUsingSprout()
{
    if (orchardBuilder.has_value()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Can't use Sprout with a v5 transaction.");
    } else {
        // Switch if necessary to a Sprout-supporting transaction format.
        auto txVersionInfo = CurrentTxVersionInfo(consensusParams, nHeight, true);
        mtx.nVersionGroupId = txVersionInfo.nVersionGroupId;
        mtx.nVersion        = txVersionInfo.nVersion;
        mtx.nConsensusBranchId = std::nullopt;
    }
}

void TransactionBuilder::CreateJSDescriptions()
{
    // Copy jsInputs and jsOutputs to more flexible containers
    std::deque<libzcash::JSInput> jsInputsDeque;
    for (auto jsInput : jsInputs) {
        jsInputsDeque.push_back(jsInput);
    }
    std::deque<libzcash::JSOutput> jsOutputsDeque;
    for (auto jsOutput : jsOutputs) {
        jsOutputsDeque.push_back(jsOutput);
    }

    // If we have no Sprout shielded inputs, then we do the simpler more-leaky
    // process where we just create outputs directly. We save the chaining logic,
    // at the expense of leaking the sums of pairs of output values in vpub_old.
    if (jsInputs.empty()) {
        // Create joinsplits, where each output represents a zaddr recipient.
        while (jsOutputsDeque.size() > 0) {
            // Default array entries are dummy inputs and outputs
            std::array<libzcash::JSInput, ZC_NUM_JS_INPUTS> vjsin;
            std::array<libzcash::JSOutput, ZC_NUM_JS_OUTPUTS> vjsout;
            uint64_t vpub_old = 0;

            for (int n = 0; n < ZC_NUM_JS_OUTPUTS && jsOutputsDeque.size() > 0; n++) {
                vjsout[n] = jsOutputsDeque.front();
                jsOutputsDeque.pop_front();

                // Funds are removed from the value pool and enter the private pool
                vpub_old += vjsout[n].value;
            }

            std::array<size_t, ZC_NUM_JS_INPUTS> inputMap;
            std::array<size_t, ZC_NUM_JS_OUTPUTS> outputMap;
            CreateJSDescription(vpub_old, 0, vjsin, vjsout, inputMap, outputMap);
        }
        return;
    }

    // At this point, we are guaranteed to have at least one input note.
    // Use address of first input note as the temporary change address.
    auto changeKey = jsInputsDeque.front().key;
    auto changeAddress = changeKey.address();

    CAmount jsChange = 0;          // this is updated after each joinsplit
    int changeOutputIndex = -1;    // this is updated after each joinsplit if jsChange > 0
    bool vpubOldProcessed = false; // updated when vpub_old for taddr inputs is set in first joinsplit
    bool vpubNewProcessed = false; // updated when vpub_new for miner fee and taddr outputs is set in last joinsplit

    CAmount valueOut = 0;
    for (auto jsInput : jsInputs) {
        valueOut += jsInput.note.value();
    }
    for (auto jsOutput : jsOutputs) {
        valueOut -= jsOutput.value;
    }
    CAmount vpubOldTarget = valueOut < 0 ? -valueOut : 0;
    CAmount vpubNewTarget = valueOut > 0 ? valueOut : 0;

    // Keep track of treestate within this transaction
    boost::unordered_map<uint256, SproutMerkleTree, SaltedTxidHasher> intermediates;
    std::vector<uint256> previousCommitments;

    while (!vpubNewProcessed) {
        // Default array entries are dummy inputs and outputs
        std::array<libzcash::JSInput, ZC_NUM_JS_INPUTS> vjsin;
        std::array<libzcash::JSOutput, ZC_NUM_JS_OUTPUTS> vjsout;
        uint64_t vpub_old = 0;
        uint64_t vpub_new = 0;

        // Set vpub_old in the first joinsplit
        if (!vpubOldProcessed) {
            vpub_old += vpubOldTarget; // funds flowing from public pool
            vpubOldProcessed = true;
        }

        CAmount jsInputValue = 0;
        uint256 jsAnchor;

        JSDescription prevJoinSplit;

        // Keep track of previous JoinSplit and its commitments
        if (mtx.vJoinSplit.size() > 0) {
            prevJoinSplit = mtx.vJoinSplit.back();
        }

        // If there is no change, the chain has terminated so we can reset the tracked treestate.
        if (jsChange == 0 && mtx.vJoinSplit.size() > 0) {
            intermediates.clear();
            previousCommitments.clear();
        }

        //
        // Consume change as the first input of the JoinSplit.
        //
        if (jsChange > 0) {
            // Update tree state with previous joinsplit
            SproutMerkleTree tree;
            {
                // assert that coinsView is not null
                assert(coinsView);
                // We do not check cs_coinView because we do not set this in testing
                // assert(cs_coinsView);
                LOCK(cs_coinsView);
                auto it = intermediates.find(prevJoinSplit.anchor);
                if (it != intermediates.end()) {
                    tree = it->second;
                } else if (!coinsView->GetSproutAnchorAt(prevJoinSplit.anchor, tree)) {
                    throw JSDescException("Could not find previous JoinSplit anchor");
                }
            }

            assert(changeOutputIndex != -1);
            assert(changeOutputIndex < prevJoinSplit.commitments.size());
            std::optional<SproutWitness> changeWitness;
            int n = 0;
            for (const uint256& commitment : prevJoinSplit.commitments) {
                tree.append(commitment);
                previousCommitments.push_back(commitment);
                if (!changeWitness && changeOutputIndex == n++) {
                    changeWitness = tree.witness();
                } else if (changeWitness) {
                    changeWitness.value().append(commitment);
                }
            }
            assert(changeWitness.has_value());
            jsAnchor = tree.root();
            intermediates.insert(std::make_pair(tree.root(), tree)); // chained js are interstitial (found in between block boundaries)

            // Decrypt the change note's ciphertext to retrieve some data we need
            ZCNoteDecryption decryptor(changeKey.receiving_key());
            auto hSig = ZCJoinSplit::h_sig(
                prevJoinSplit.randomSeed,
                prevJoinSplit.nullifiers,
                mtx.joinSplitPubKey);
            try {
                auto plaintext = libzcash::SproutNotePlaintext::decrypt(
                    decryptor,
                    prevJoinSplit.ciphertexts[changeOutputIndex],
                    prevJoinSplit.ephemeralKey,
                    hSig,
                    (unsigned char)changeOutputIndex);

                auto note = plaintext.note(changeAddress);
                vjsin[0] = libzcash::JSInput(changeWitness.value(), note, changeKey);

                jsInputValue += plaintext.value();

                LogPrint("zrpcunsafe", "spending change (amount=%s)\n", FormatMoney(plaintext.value()));

            } catch (const std::exception& e) {
                throw JSDescException("Error decrypting output note of previous JoinSplit");
            }
        }

        //
        // Consume spendable non-change notes
        //
        for (int n = (jsChange > 0) ? 1 : 0; n < ZC_NUM_JS_INPUTS && jsInputsDeque.size() > 0; n++) {
            auto jsInput = jsInputsDeque.front();
            jsInputsDeque.pop_front();

            // Add history of previous commitments to witness
            if (jsChange > 0) {
                for (const uint256& commitment : previousCommitments) {
                    jsInput.witness.append(commitment);
                }
                if (jsAnchor != jsInput.witness.root()) {
                    throw JSDescException("Witness for spendable note does not have same anchor as change input");
                }
            }

            // The jsAnchor is null if this JoinSplit is at the start of a new chain
            if (jsAnchor.IsNull()) {
                jsAnchor = jsInput.witness.root();
            }

            jsInputValue += jsInput.note.value();
            vjsin[n] = jsInput;
        }

        // Find recipient to transfer funds to
        libzcash::JSOutput recipient;
        if (jsOutputsDeque.size() > 0) {
            recipient = jsOutputsDeque.front();
            jsOutputsDeque.pop_front();
        }
        // `recipient` is now either a valid recipient, or a dummy output with value = 0

        // Reset change
        jsChange = 0;
        CAmount outAmount = recipient.value;

        // Set vpub_new in the last joinsplit (when there are no more notes to spend or zaddr outputs to satisfy)
        if (jsOutputsDeque.empty() && jsInputsDeque.empty()) {
            assert(!vpubNewProcessed);
            if (jsInputValue < vpubNewTarget) {
                throw JSDescException(strprintf("Insufficient funds for vpub_new %s", FormatMoney(vpubNewTarget)));
            }
            outAmount += vpubNewTarget;
            vpub_new += vpubNewTarget; // funds flowing back to public pool
            vpubNewProcessed = true;
            jsChange = jsInputValue - outAmount;
            assert(jsChange >= 0);
        } else {
            // This is not the last joinsplit, so compute change and any amount still due to the recipient
            if (jsInputValue > outAmount) {
                jsChange = jsInputValue - outAmount;
            } else if (outAmount > jsInputValue) {
                // Any amount due is owed to the recipient.  Let the miners fee get paid first.
                CAmount due = outAmount - jsInputValue;
                libzcash::JSOutput recipientDue(recipient.addr, due);
                recipientDue.memo = recipient.memo;
                jsOutputsDeque.push_front(recipientDue);

                // reduce the amount being sent right now to the value of all inputs
                recipient.value = jsInputValue;
            }
        }

        // create output for recipient
        assert(ZC_NUM_JS_OUTPUTS == 2); // If this changes, the logic here will need to be adjusted
        vjsout[0] = recipient;

        // create output for any change
        if (jsChange > 0) {
            vjsout[1] = libzcash::JSOutput(changeAddress, jsChange);

            LogPrint("zrpcunsafe", "generating note for change (amount=%s)\n", FormatMoney(jsChange));
        }

        std::array<size_t, ZC_NUM_JS_INPUTS> inputMap;
        std::array<size_t, ZC_NUM_JS_OUTPUTS> outputMap;
        CreateJSDescription(vpub_old, vpub_new, vjsin, vjsout, inputMap, outputMap);

        if (jsChange > 0) {
            changeOutputIndex = -1;
            for (size_t i = 0; i < outputMap.size(); i++) {
                if (outputMap[i] == 1) {
                    changeOutputIndex = i;
                }
            }
            assert(changeOutputIndex != -1);
        }
    }
}

void TransactionBuilder::CreateJSDescription(
    uint64_t vpub_old,
    uint64_t vpub_new,
    std::array<libzcash::JSInput, ZC_NUM_JS_INPUTS> vjsin,
    std::array<libzcash::JSOutput, ZC_NUM_JS_OUTPUTS> vjsout,
    std::array<size_t, ZC_NUM_JS_INPUTS>& inputMap,
    std::array<size_t, ZC_NUM_JS_OUTPUTS>& outputMap)
{
    LogPrint("zrpcunsafe", "CreateJSDescription: creating joinsplit at index %d (vpub_old=%s, vpub_new=%s, in[0]=%s, in[1]=%s, out[0]=%s, out[1]=%s)\n",
        mtx.vJoinSplit.size(),
        FormatMoney(vpub_old), FormatMoney(vpub_new),
        FormatMoney(vjsin[0].note.value()), FormatMoney(vjsin[1].note.value()),
        FormatMoney(vjsout[0].value), FormatMoney(vjsout[1].value));

    uint256 esk; // payment disclosure - secret

    // Generate the proof, this can take over a minute.
    assert(mtx.fOverwintered && (mtx.nVersion >= SAPLING_TX_VERSION));
    JSDescription jsdesc = JSDescriptionInfo(
            mtx.joinSplitPubKey,
            vjsin[0].witness.root(),
            vjsin,
            vjsout,
            vpub_old,
            vpub_new
    ).BuildRandomized(
            inputMap,
            outputMap,
            true, //!this->testmode,
            &esk); // parameter expects pointer to esk, so pass in address

    {
        auto verifier = ProofVerifier::Strict();
        if (!verifier.VerifySprout(jsdesc, mtx.joinSplitPubKey)) {
            throw std::runtime_error("error verifying joinsplit");
        }
    }

    mtx.vJoinSplit.push_back(jsdesc);

    // TODO: Sprout payment disclosure
}
