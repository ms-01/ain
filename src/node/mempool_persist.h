// Copyright (c) 2026 DeFi Blockchain Developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#ifndef DEFI_NODE_MEMPOOL_PERSIST_H
#define DEFI_NODE_MEMPOOL_PERSIST_H

#include <amount.h>

#include <cstdint>

class CTxMemPool;

/**
 * Dump the mempool to disk.
 */
bool DumpMempool(const CTxMemPool &pool);

/**
 * Load the mempool from disk.
 */
bool LoadMempool(CTxMemPool &pool);

#endif // DEFI_NODE_MEMPOOL_PERSIST_H
