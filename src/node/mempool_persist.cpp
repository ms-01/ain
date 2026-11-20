// Copyright (c) 2026 DeFi Blockchain Developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <node/mempool_persist.h>

#include <chainparams.h>
#include <clientversion.h>
#include <consensus/validation.h>
#include <dfi/customtx.h>
#include <fs.h>
#include <policy/policy.h>
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

uint64_t MaxMempoolFileEntries(int64_t maxMempoolBytes)
{
    if (maxMempoolBytes <= 0) {
        return 0;
    }
    const uint64_t bytes = static_cast<uint64_t>(maxMempoolBytes);
    return bytes / MIN_MEMPOOL_ENTRY_SIZE + MEMPOOL_FILE_ENTRY_MARGIN;
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

bool IsPlausibleMempoolTime(int64_t nTime, int64_t nNow)
{
    // A timestamp before the unix epoch can only come from a corrupted file.
    if (nTime < 0) {
        return false;
    }
    if (nTime > nNow) {
        // The difference is computed in the unsigned domain to avoid an
        // overflow for extreme values.
        const uint64_t drift = static_cast<uint64_t>(nTime) - static_cast<uint64_t>(nNow);
        return drift <= static_cast<uint64_t>(MAX_MEMPOOL_TIME_DRIFT);
    }
    return true;
}

bool IsValidMempoolFeeDelta(const CAmount &feeDelta)
{
    return MoneyRange(feeDelta);
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
    int64_t skipped_deltas = 0;
    const int64_t nNow = GetTime();
    const uint64_t maxEntries = MaxMempoolFileEntries(MempoolSizeBytes(gArgs.GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE)));

    try {
        uint64_t version;
        file >> version;
        if (version != MEMPOOL_DUMP_VERSION) {
            LogPrintf("Mempool file has an unsupported version (%u). Continuing anyway.\n", version);
            return false;
        }
        uint64_t num;
        file >> num;
        if (num > maxEntries) {
            LogPrintf("Mempool file claims %u transactions, which exceeds the maximum of %u allowed for the configured mempool size. Continuing anyway.\n",
                      num,
                      maxEntries);
            return false;
        }
        while (num--) {
            CTransactionRef tx;
            int64_t nTime;
            int64_t nFeeDelta;
            file >> tx;
            file >> nTime;
            file >> nFeeDelta;

            const uint256 txid = tx->GetHash();

            std::vector<unsigned char> metadata;
            CustomTxType txType = GuessCustomTxType(*tx, metadata, true);
            const auto isEVMTx = txType == CustomTxType::EvmTx || txType == CustomTxType::TransferDomain;

            if (!IsPlausibleMempoolTime(nTime, nNow)) {
                LogPrintf("Mempool entry %s has an implausible timestamp (%d). Skipping it.\n", txid.ToString(), nTime);
                ++failed;
            } else if (IsMempoolEntryExpired(nTime, nNow, nExpiryTimeout, nExpiryEVMTimeout, isEVMTx)) {
                ++expired;
            } else {
                CValidationState state;
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
                    if (pool.exists(txid)) {
                        ++already_there;
                    } else {
                        ++failed;
                    }
                }
            }

            // The fee delta is only applied for transactions that are part of the
            // mempool, so that a corrupted file cannot inject priorities for
            // transactions that do not exist.
            if (nFeeDelta != 0) {
                const CAmount amountdelta = nFeeDelta;
                if (!IsValidMempoolFeeDelta(amountdelta)) {
                    LogPrintf("Mempool entry %s has an out of range fee delta (%d). Ignoring it.\n", txid.ToString(), amountdelta);
                    ++skipped_deltas;
                } else if (pool.exists(txid)) {
                    pool.PrioritiseTransaction(txid, amountdelta);
                } else {
                    LogPrintf("Mempool entry %s is not part of the mempool, ignoring its fee delta.\n", txid.ToString());
                    ++skipped_deltas;
                }
            }
            if (ShutdownRequested()) {
                return false;
            }
        }

        // The fee deltas of transactions that are not part of the mempool are
        // stored at the end of the file. The number of entries is read
        // explicitly, so that a corrupted file cannot force an unbounded
        // allocation while the map is deserialized.
        const uint64_t numDeltas = ReadCompactSize(file);
        if (numDeltas > MAX_MEMPOOL_FILE_DELTAS) {
            LogPrintf("Mempool file claims %u fee deltas, which exceeds the maximum of %u allowed. Continuing anyway.\n",
                      numDeltas,
                      MAX_MEMPOOL_FILE_DELTAS);
            return false;
        }
        for (uint64_t i = 0; i < numDeltas; ++i) {
            uint256 txid;
            CAmount amountdelta;
            file >> txid;
            file >> amountdelta;
            if (!IsValidMempoolFeeDelta(amountdelta)) {
                LogPrintf("Mempool file has an out of range fee delta (%d) for %s. Ignoring it.\n", amountdelta, txid.ToString());
                ++skipped_deltas;
                continue;
            }
            if (!pool.exists(txid)) {
                LogPrintf("Mempool file has a fee delta for %s, which is not part of the mempool. Ignoring it.\n", txid.ToString());
                ++skipped_deltas;
                continue;
            }
            pool.PrioritiseTransaction(txid, amountdelta);
        }
    } catch (const std::exception &e) {
        LogPrintf("Failed to deserialize mempool data on disk: %s. Continuing anyway.\n", e.what());
        return false;
    }

    LogPrintf("Imported mempool transactions from disk: %i succeeded, %i failed, %i expired, %i already there, %i fee deltas skipped\n",
              count,
              failed,
              expired,
              already_there,
              skipped_deltas);
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

    const fs::path tmpPath = GetDataDir() / "mempool.dat.new";
    const fs::path mempoolPath = GetDataDir() / "mempool.dat";
    bool tmpFileCreated = false;

    try {
        FILE *filestr = fsbridge::fopen(tmpPath, "wb");
        if (!filestr) {
            LogPrintf("Failed to open the mempool file for writing. Continuing anyway.\n");
            return false;
        }
        tmpFileCreated = true;

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
        // The result of the close has to be checked: a failing close can mean
        // that the data was not written to disk completely.
        if (file.fclose() != 0) {
            throw std::runtime_error("Failed to close the mempool file");
        }
        if (!RenameOver(tmpPath, mempoolPath)) {
            throw std::runtime_error("Failed to rename the mempool file");
        }
        tmpFileCreated = false;

        int64_t last = GetTimeMicros();
        LogPrintf("Dumped mempool: %gs to copy, %gs to dump\n", (mid - start) * MICRO, (last - mid) * MICRO);
    } catch (const std::exception &e) {
        LogPrintf("Failed to dump mempool: %s. Continuing anyway.\n", e.what());
        if (tmpFileCreated) {
            // Do not leave a stale (and possibly truncated) temporary file behind.
            try {
                fs::remove(tmpPath);
            } catch (const std::exception &cleanupError) {
                LogPrintf("Failed to remove the temporary mempool file: %s\n", cleanupError.what());
            }
        }
        return false;
    }
    return true;
}
