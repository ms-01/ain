// Copyright (c) 2016-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <util/rbf.h>

#include <primitives/transaction.h>

bool SignalsOptInRBF(const CTransaction &tx)
{
    // BIP 431: TRUC (version=3) transactions are implicitly replaceable.
    if (tx.nVersion == 3) {
        return true;
    }
    for (const CTxIn &txin : tx.vin) {
        if (txin.nSequence <= MAX_BIP125_RBF_SEQUENCE) {
            return true;
        }
    }
    return false;
}
