// Copyright (c) 2026 DeFi Blockchain Developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#ifndef DEFI_NODE_MEMPOOL_PERSIST_H
#define DEFI_NODE_MEMPOOL_PERSIST_H

#include <amount.h>

#include <cstdint>

class CTxMemPool;

/**
 * Convert an expiry given in hours into seconds.
 *
 * The conversion saturates instead of overflowing, so that an operator
 * supplied (or corrupted) value can never turn into an undefined behaviour
 * in the signed arithmetic that follows.
 */
int64_t MempoolExpirySeconds(int64_t hours);

/**
 * Convert a mempool size given in megabytes into bytes, saturating instead of
 * overflowing.
 */
int64_t MempoolSizeBytes(int64_t megabytes);

/**
 * Check whether a mempool entry with the given acceptance time has expired.
 *
 * The comparison is performed without any signed overflow: the deadline is
 * computed with a saturating subtraction and the acceptance time is only
 * compared against it.
 */
bool IsMempoolTimeExpired(int64_t nTime, int64_t nNow, int64_t nExpiry);

/**
 * Check whether a mempool entry has expired, honouring the DFI specific EVM
 * and DVM expiry windows.
 *
 * An EVM transaction is only considered expired once *both* the EVM and the
 * DVM expiry window have passed, which is the behaviour of the original
 * (pre-hardening) implementation.
 */
bool IsMempoolEntryExpired(int64_t nTime, int64_t nNow, int64_t nExpiryTimeout, int64_t nExpiryEVMTimeout, bool isEVMTx);

/**
 * Dump the mempool to disk.
 */
bool DumpMempool(const CTxMemPool &pool);

/**
 * Load the mempool from disk.
 */
bool LoadMempool(CTxMemPool &pool);

#endif // DEFI_NODE_MEMPOOL_PERSIST_H
