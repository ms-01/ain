// Copyright (c) 2026 DeFi Blockchain Developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#ifndef DEFI_NODE_MEMPOOL_PERSIST_H
#define DEFI_NODE_MEMPOOL_PERSIST_H

#include <amount.h>

#include <cstdint>

class CTxMemPool;

/**
 * Smallest serialized size (in bytes) that is assumed for a single entry of
 * mempool.dat. It is used to derive an upper bound for the number of entries a
 * mempool.dat file may legitimately contain.
 */
static constexpr uint64_t MIN_MEMPOOL_ENTRY_SIZE = 32;

/**
 * Generous margin that is added to the derived upper bound of entries, so that
 * legitimate mempool.dat files are never rejected.
 */
static constexpr uint64_t MEMPOOL_FILE_ENTRY_MARGIN = 100000;

/**
 * Upper bound for the number of fee deltas that are read from the end of
 * mempool.dat. Without such a bound a corrupted file could force a huge
 * allocation while the map is deserialized.
 */
static constexpr uint64_t MAX_MEMPOOL_FILE_DELTAS = 100000;

/**
 * Maximum tolerated difference (in seconds) between the acceptance time stored
 * in mempool.dat and the current time. Timestamps that are further in the
 * future can only originate from a corrupted file and are not imported.
 */
static constexpr int64_t MAX_MEMPOOL_TIME_DRIFT = 24 * 60 * 60;

/**
 * Upper bound for the number of transactions that may be imported from
 * mempool.dat. The bound is derived from the configured mempool size and a
 * generous margin, so that only clearly impossible values are rejected.
 */
uint64_t MaxMempoolFileEntries(int64_t maxMempoolBytes);

/**
 * Check whether the acceptance time stored in mempool.dat is plausible.
 *
 * Timestamps before the unix epoch and timestamps that are absurdly far in the
 * future can only originate from a corrupted file.
 */
bool IsPlausibleMempoolTime(int64_t nTime, int64_t nNow);

/**
 * Check whether a fee delta read from mempool.dat is within the valid amount
 * range.
 */
bool IsValidMempoolFeeDelta(const CAmount &feeDelta);

/**
 * Dump the mempool to disk.
 */
bool DumpMempool(const CTxMemPool &pool);

/**
 * Load the mempool from disk.
 */
bool LoadMempool(CTxMemPool &pool);

#endif // DEFI_NODE_MEMPOOL_PERSIST_H
