// Copyright (c) 2013-2014 The Bitcoin Core developers
// Copyright (c) 2016-2022 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "clientversion.h"
#include "consensus/validation.h"
#include "fs.h"
#include "main.h"
#include "proof_verifier.h"
#include "test/test_bitcoin.h"
#include "util/time.h"
#include "zcash/Proof.hpp"

#include <cstdio>

#include <boost/test/unit_test.hpp>


BOOST_FIXTURE_TEST_SUITE(CheckBlock_tests, BasicTestingSetup)

bool read_block(const std::string& filename, CBlock& block)
{
    fs::path testFile = fs::current_path() / "data" / filename;
#ifdef TEST_DATA_DIR
    if (!fs::exists(testFile))
    {
        testFile = fs::path(BOOST_PP_STRINGIZE(TEST_DATA_DIR)) / filename;
    }
#endif
    FILE* fp = fsbridge::fopen(testFile, "rb");
    if (!fp) return false;

    fseek(fp, 8, SEEK_SET); // skip msgheader/size

    CAutoFile filein(fp, SER_DISK, CLIENT_VERSION);
    if (filein.IsNull()) return false;

    filein >> block;

    return true;
}

BOOST_AUTO_TEST_CASE(May15)
{
    // Putting a 1MB binary file in the git repository is not a great
    // idea, so this test is only run if you manually download
    // test/data/Mar12Fork.dat from
    // http://sourceforge.net/projects/bitcoin/files/Bitcoin/blockchain/Mar12Fork.dat/download
    FixedClock::SetGlobal();

    // Test as if the time is exactly 2013-05-15 00:00:00Z
    int64_t tMay15 = 1368576000;
    FixedClock::Instance()->Set(std::chrono::seconds(tMay15));

    CBlock forkingBlock;
    if (read_block("Mar12Fork.dat", forkingBlock))
    {
        CValidationState state;

        // After May 15'th, big blocks are OK:
        forkingBlock.nTime = tMay15; // Invalidates PoW
        auto verifier = ProofVerifier::Strict();
        BOOST_CHECK(CheckBlock(forkingBlock, state, Params(), verifier, false, false, true));
    }

    SystemClock::SetGlobal();
}

BOOST_AUTO_TEST_SUITE_END()
