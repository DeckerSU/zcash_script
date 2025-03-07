#ifndef ZCASH_RUST_INCLUDE_LIBRUSTZCASH_H
#define ZCASH_RUST_INCLUDE_LIBRUSTZCASH_H

#include "rust/types.h"

#include <stddef.h>
#include <stdint.h>

#ifndef __cplusplus
  #include <assert.h>
  #include <stdalign.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

    void librustzcash_to_scalar(const unsigned char *input, unsigned char *result);

    void librustzcash_ask_to_ak(const unsigned char *ask, unsigned char *result);

    void librustzcash_nsk_to_nk(const unsigned char *nsk, unsigned char *result);

    void librustzcash_crh_ivk(const unsigned char *ak, const unsigned char *nk, unsigned char *result);

    bool librustzcash_check_diversifier(const unsigned char *diversifier);

    bool librustzcash_ivk_to_pkd(const unsigned char *ivk, const unsigned char *diversifier, unsigned char *result);

    /// Loads the zk-SNARK parameters into memory and saves
    /// paths as necessary. Only called once.
    void librustzcash_init_zksnark_params(
        const codeunit* spend_path,
        size_t spend_path_len,
        const codeunit* output_path,
        size_t output_path_len,
        const codeunit* sprout_path,
        size_t sprout_path_len,
        bool load_proving_keys
    );

    /// Writes the "uncommitted" note value for empty leaves
    /// of the merkle tree. `result` must be a valid pointer
    /// to 32 bytes which will be written.
    void librustzcash_tree_uncommitted(
        unsigned char *result
    );

    /// Computes a merkle tree hash for a given depth.
    /// The `depth` parameter should not be larger than
    /// 62.
    ///
    /// `a` and `b` each must be of length 32, and must each
    /// be scalars of BLS12-381.
    ///
    /// The result of the merkle tree hash is placed in
    /// `result`, which must also be of length 32.
    void librustzcash_merkle_hash(
        size_t depth,
        const unsigned char *a,
        const unsigned char *b,
        unsigned char *result
    );

    /// Computes the signature for each Spend description, given the key
    /// `ask`, the re-randomization `ar`, the 32-byte sighash `sighash`,
    /// and an output `result` buffer of 64-bytes for the signature.
    ///
    /// This function will fail if the provided `ask` or `ar` are invalid.
    bool librustzcash_sapling_spend_sig(
        const unsigned char *ask,
        const unsigned char *ar,
        const unsigned char *sighash,
        unsigned char *result
    );

    /// Creates a Sapling proving context. Please free this when you're done.
    void * librustzcash_sapling_proving_ctx_init();

    /// This function (using the proving context) constructs a Spend proof
    /// given the necessary witness information. It outputs `cv` (the value
    /// commitment) and `rk` (so that you don't have to compute it) along
    /// with the proof.
    bool librustzcash_sapling_spend_proof(
        void *ctx,
        const unsigned char *ak,
        const unsigned char *nsk,
        const unsigned char *diversifier,
        const unsigned char *rcm,
        const unsigned char *ar,
        const uint64_t value,
        const unsigned char *anchor,
        const unsigned char *witness,
        unsigned char *cv,
        unsigned char *rk,
        unsigned char *zkproof
    );

    /// This function (using the proving context) constructs an Output
    /// proof given the necessary witness information. It outputs `cv`
    /// and the `zkproof`.
    bool librustzcash_sapling_output_proof(
        void *ctx,
        const unsigned char *esk,
        const unsigned char *payment_address,
        const unsigned char *rcm,
        const uint64_t value,
        unsigned char *cv,
        unsigned char *zkproof
    );

    /// This function (using the proving context) constructs a binding
    /// signature. You must provide the intended valueBalance so that
    /// we can internally check consistency.
    bool librustzcash_sapling_binding_sig(
        const void *ctx,
        int64_t valueBalance,
        const unsigned char *sighash,
        unsigned char *result
    );

    /// Frees a Sapling proving context returned from
    /// `librustzcash_sapling_proving_ctx_init`.
    void librustzcash_sapling_proving_ctx_free(void *);

    /// Compute a Sapling nullifier.
    ///
    /// The `diversifier` parameter must be 11 bytes in length.
    /// The `pk_d`, `r`, `ak` and `nk` parameters must be of length 32.
    /// The result is also of length 32 and placed in `result`.
    /// Returns false if the diversifier or pk_d is not valid
    bool librustzcash_sapling_compute_nf(
        const unsigned char *diversifier,
        const unsigned char *pk_d,
        const uint64_t value,
        const unsigned char *rcm,
        const unsigned char *ak,
        const unsigned char *nk,
        const uint64_t position,
        unsigned char *result
    );

    /// Compute a Sapling commitment.
    ///
    /// The `diversifier` parameter must be 11 bytes in length.
    /// The `pk_d` and `r` parameters must be of length 32.
    /// The result is also of length 32 and placed in `result`.
    /// Returns false if the diversifier or pk_d is not valid
    bool librustzcash_sapling_compute_cmu(
        bool zip216_enabled,
        const unsigned char *diversifier,
        const unsigned char *pk_d,
        const uint64_t value,
        const unsigned char *rcm,
        unsigned char *result
    );

    /// Compute [sk] [8] P for some 32-byte
    /// point P, and 32-byte Fs. If P or sk
    /// are invalid, returns false. Otherwise,
    /// the result is written to the 32-byte
    /// `result` buffer.
    bool librustzcash_sapling_ka_agree(
        bool zip216_enabled,
        const unsigned char *p,
        const unsigned char *sk,
        unsigned char *result
    );

    /// Compute g_d = GH(diversifier) and returns
    /// false if the diversifier is invalid.
    /// Computes [esk] g_d and writes the result
    /// to the 32-byte `result` buffer. Returns
    /// false if `esk` is not a valid scalar.
    bool librustzcash_sapling_ka_derivepublic(
        const unsigned char *diversifier,
        const unsigned char *esk,
        unsigned char *result
    );

    /// Generate uniformly random scalar in Jubjub.
    /// The result is of length 32.
    void librustzcash_sapling_generate_r(
        unsigned char *result
    );

    /// Sprout JoinSplit proof generation.
    void librustzcash_sprout_prove(
        unsigned char *proof_out,

        const unsigned char *phi,
        const unsigned char *rt,
        const unsigned char *h_sig,

        const unsigned char *in_sk1,
        uint64_t in_value1,
        const unsigned char *in_rho1,
        const unsigned char *in_r1,
        const unsigned char *in_auth1,

        const unsigned char *in_sk2,
        uint64_t in_value2,
        const unsigned char *in_rho2,
        const unsigned char *in_r2,
        const unsigned char *in_auth2,

        const unsigned char *out_pk1,
        uint64_t out_value1,
        const unsigned char *out_r1,

        const unsigned char *out_pk2,
        uint64_t out_value2,
        const unsigned char *out_r2,

        uint64_t vpub_old,
        uint64_t vpub_new
    );

    /// Sprout JoinSplit proof verification.
    bool librustzcash_sprout_verify(
        const unsigned char *proof,
        const unsigned char *rt,
        const unsigned char *h_sig,
        const unsigned char *mac1,
        const unsigned char *mac2,
        const unsigned char *nf1,
        const unsigned char *nf2,
        const unsigned char *cm1,
        const unsigned char *cm2,
        uint64_t vpub_old,
        uint64_t vpub_new
    );

    /// Derive the master ExtendedSpendingKey from a seed.
    void librustzcash_zip32_sapling_xsk_master(
        const unsigned char *seed,
        size_t seedlen,
        unsigned char *xsk_master
    );

    /// Derive a child ExtendedSpendingKey from a parent.
    void librustzcash_zip32_sapling_xsk_derive(
        const unsigned char *xsk_parent,
        uint32_t i,
        unsigned char *xsk_i
    );

    /// Derive a internal ExtendedSpendingKey from an external key
    void librustzcash_zip32_sapling_xsk_derive_internal(
        const unsigned char *xsk_external,
        unsigned char *xsk_internal
    );

    /// Derive a child ExtendedFullViewingKey from a parent.
    bool librustzcash_zip32_sapling_xfvk_derive(
        const unsigned char *xfvk_parent,
        uint32_t i,
        unsigned char *xfvk_i
    );

    /**
     * Derive the Sapling internal FVK corresponding to the given
     * Sapling external FVK.
     */
    void librustzcash_zip32_sapling_derive_internal_fvk(
        const unsigned char *fvk,
        const unsigned char *dk,
        unsigned char *fvk_ret,
        unsigned char *dk_ret
    );

    /**
     * Derive a PaymentAddress from a (SaplingFullViewingKey, DiversifierKey)
     * pair.  Returns 'false' if no valid address can be derived for the
     * specified diversifier index.
     *
     * Arguments:
     * - fvk: [c_uchar; 96] the serialized form of a Sapling full viewing key
     * - dk: [c_uchar; 32] the byte representation of a Sapling diversifier key
     * - j: [c_uchar; 11] the 88-bit diversifier index, encoded in little-endian
     *   order
     * - addr_ret: [c_uchar; 43] array to which the returned address will be
     *   written, if the specified diversifier index `j` produces a valid
     *   address.
     */
    bool librustzcash_zip32_sapling_address(
        const unsigned char *fvk,
        const unsigned char *dk,
        const unsigned char *j,
        unsigned char *addr_ret
    );

    /**
     * Derive a PaymentAddress from a (SaplingFullViewingKey, DiversifierKey)
     * pair by searching the space of valid diversifiers starting at
     * diversifier index `j`. This will always return a valid address along
     * with the diversifier index that produced the address unless no addresses
     * can be derived at any diversifier index >= `j`, in which case this
     * function will return `false`.
     *
     * Arguments:
     * - fvk: [c_uchar; 96] the serialized form of a Sapling full viewing key
     * - dk: [c_uchar; 32] the byte representation of a Sapling diversifier key
     * - j: [c_uchar; 11] the 88-bit diversifier index at which to start
     *   searching, encoded in little-endian order
     * - j_ret: [c_uchar; 11] array that will store the diversifier index at
     *   which the returned address was found
     * - addr_ret: [c_uchar; 43] array to which the returned address will be
     *   written
     */
    bool librustzcash_zip32_find_sapling_address(
        const unsigned char *fvk,
        const unsigned char *dk,
        const unsigned char *j,
        unsigned char *j_ret,
        unsigned char *addr_ret
    );

    /**
     * Decrypts a Sapling diversifier using the specified diversifier key
     * to obtain the diversifier index `j` at which the diversifier was
     * derived.
     *
     * Arguments:
     * - dk: [c_uchar; 32] the byte representation of a Sapling diversifier key
     * - addr: [c_uchar; 11] the bytes of the diversifier
     * - j_ret: [c_uchar; 11] array that will store the resulting diversifier index
     */
    void librustzcash_sapling_diversifier_index(
        const unsigned char *dk,
        const unsigned char *d,
        unsigned char *j_ret
    );

    /// Fills the provided buffer with random bytes. This is intended to
    /// be a cryptographically secure RNG; it uses Rust's `OsRng`, which
    /// is implemented in terms of the `getrandom` crate. The first call
    /// to this function may block until sufficient randomness is available.
    void librustzcash_getrandom(
        unsigned char *buf,
        size_t buf_len
    );
#ifdef __cplusplus
}
#endif

#endif // ZCASH_RUST_INCLUDE_LIBRUSTZCASH_H
