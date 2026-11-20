// Copyright (c) 2026 DeFi Blockchain Developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <node/mempool_persist.h>

#include <chainparams.h>
#include <clientversion.h>
#include <consensus/validation.h>
#include <dfi/customtx.h>
#include <fs.h>
#include <serialize.h>
#include <shutdown.h>
#include <streams.h>
#include <sync.h>
#include <txmempool.h>
#include <uint256.h>
#include <util/system.h>
#include <util/time.h>
#include <validation.h>

#include <cstdio>
#include <exception>
#include <limits>
#include <map>
#include <stdexcept>
#include <vector>

static const uint64_t MEMPOOL_DUMP_VERSION = 1;

// Scale factor used to convert microseconds into seconds for the dump log message.
static const double MICRO = 0.000001;

int64_t MempoolExpirySeconds(int64_t hours)
{
    if (hours <= 0) {
        return 0;
    }
    constexpr int64_t maxHours = std::numeric_limits<int64_t>::max() / 3600;
    if (hours > maxHours) {
        return std::numeric_limits<int64_t>::max();
    }
    return hours * 3600;
}

int64_t MempoolSizeBytes(int64_t megabytes)
{
    if (megabytes <= 0) {
        return 0;
    }
    constexpr int64_t maxMegabytes = std::numeric_limits<int64_t>::max() / 1000000;
    if (megabytes > maxMegabytes) {
        return std::numeric_limits<int64_t>::max();
    }
    return megabytes * 1000000;
}

bool IsMempoolTimeExpired(int64_t nTime, int64_t nNow, int64_t nExpiry)
{
    if (nExpiry < 0) {
        nExpiry = 0;
    }
    // The deadline is computed with a saturating subtraction, so that neither
    // the deadline nor the comparison below can overflow.
    int64_t deadline;
    if (nExpiry > 0 && nNow < std::numeric_limits<int64_t>::min() + nExpiry) {
        deadline = std::numeric_limits<int64_t>::min();
    } else {
        deadline = nNow - nExpiry;
    }
    // An entry is expired as soon as it is not newer than the deadline.
    return nTime <= deadline;
}

bool IsMempoolEntryExpired(int64_t nTime, int64_t nNow, int64_t nExpiryTimeout, int64_t nExpiryEVMTimeout, bool isEVMTx)
{
    const bool dvmExpired = IsMempoolTimeExpired(nTime, nNow, nExpiryTimeout);
    const bool evmExpired = IsMempoolTimeExpired(nTime, nNow, nExpiryEVMTimeout);
    // Keep the original DFI semantics: an EVM transaction is re-imported as long
    // as either the EVM or the DVM expiry window is still open.
    return !((isEVMTx && !evmExpired) || !dvmExpired);
}

bool LoadMempool(CTxMemPool &pool) {
    const CChainParams &chainparams = Params();
    const int64_t nExpiryTimeout = MempoolExpirySeconds(gArgs.GetArg("-mempoolexpiry", DEFAULT_MEMPOOL_DVM_EXPIRY));
    const int64_t nExpiryEVMTimeout = MempoolExpirySeconds(gArgs.GetArg("-mempoolexpiryevm", DEFAULT_MEMPOOL_EVM_EXPIRY));
    FILE *filestr = fsbridge::fopen(GetDataDir() / "mempool.dat", "rb");
    CAutoFile file(filestr, SER_DISK, CLIENT_VERSION);
    if (file.IsNull()) {
        LogPrintf("Failed to open mempool file from disk. Continuing anyway.\n");
        return false;
    }

    int64_t count = 0;
    int64_t expired = 0;
    int64_t failed = 0;
    int64_t already_there = 0;
    const int64_t nNow = GetTime();

    try {
        uint64_t version;
        file >> version;
        if (version != MEMPOOL_DUMP_VERSION) {
            return false;
        }
        uint64_t num;
        file >> num;
        while (num--) {
            CTransactionRef tx;
            int64_t nTime;
            int64_t nFeeDelta;
            file >> tx;
            file >> nTime;
            file >> nFeeDelta;

            CAmount amountdelta = nFeeDelta;
            if (amountdelta) {
                pool.PrioritiseTransaction(tx->GetHash(), amountdelta);
            }

            std::vector<unsigned char> metadata;
            CustomTxType txType = GuessCustomTxType(*tx, metadata, true);
            const auto isEVMTx = txType == CustomTxType::EvmTx || txType == CustomTxType::TransferDomain;

            CValidationState state;
            if (!IsMempoolEntryExpired(nTime, nNow, nExpiryTimeout, nExpiryEVMTimeout, isEVMTx)) {
                LOCK(cs_main);
                AcceptToMemoryPoolWithTime(chainparams,
                                           pool,
                                           state,
                                           tx,
                                           nullptr /* pfMissingInputs */,
                                           nTime,
                                           nullptr /* plTxnReplaced */,
                                           false /* bypass_limits */,
                                           0 /* nAbsurdFee */,
                                           false /* test_accept */);
                if (state.IsValid()) {
                    ++count;
                } else {
                    // mempool may contain the transaction already, e.g. from
                    // wallet(s) having loaded it while we were processing
                    // mempool transactions; consider these as valid, instead of
                    // failed, but mark them as 'already there'
                    if (pool.exists(tx->GetHash())) {
                        ++already_there;
                    } else {
                        ++failed;
                    }
                }
            } else {
                ++expired;
            }
            if (ShutdownRequested()) {
                return false;
            }
        }
        std::map<uint256, CAmount> mapDeltas;
        file >> mapDeltas;

        for (const auto &i : mapDeltas) {
            pool.PrioritiseTransaction(i.first, i.second);
        }
    } catch (const std::exception &e) {
        LogPrintf("Failed to deserialize mempool data on disk: %s. Continuing anyway.\n", e.what());
        return false;
    }

    LogPrintf("Imported mempool transactions from disk: %i succeeded, %i failed, %i expired, %i already there\n",
              count,
              failed,
              expired,
              already_there);
    return true;
}

bool DumpMempool(const CTxMemPool &pool) {
    int64_t start = GetTimeMicros();

    std::map<uint256, CAmount> mapDeltas;
    std::vector<TxMempoolInfo> vinfo;

    static Mutex dump_mutex;
    LOCK(dump_mutex);

    {
        LOCK(pool.cs);
        for (const auto &i : pool.mapDeltas) {
            mapDeltas[i.first] = i.second;
        }
        vinfo = pool.infoAll();
    }

    int64_t mid = GetTimeMicros();

    try {
        FILE *filestr = fsbridge::fopen(GetDataDir() / "mempool.dat.new", "wb");
        if (!filestr) {
            return false;
        }

        CAutoFile file(filestr, SER_DISK, CLIENT_VERSION);

        uint64_t version = MEMPOOL_DUMP_VERSION;
        file << version;

        file << (uint64_t)vinfo.size();
        for (const auto &i : vinfo) {
            file << *(i.tx);
            file << (int64_t)i.nTime;
            file << (int64_t)i.nFeeDelta;
            mapDeltas.erase(i.tx->GetHash());
        }

        file << mapDeltas;
        if (!FileCommit(file.Get())) {
            throw std::runtime_error("FileCommit failed");
        }
        file.fclose();
        RenameOver(GetDataDir() / "mempool.dat.new", GetDataDir() / "mempool.dat");
        int64_t last = GetTimeMicros();
        LogPrintf("Dumped mempool: %gs to copy, %gs to dump\n", (mid - start) * MICRO, (last - mid) * MICRO);
    } catch (const std::exception &e) {
        LogPrintf("Failed to dump mempool: %s. Continuing anyway.\n", e.what());
        return false;
    }
    return true;
}
