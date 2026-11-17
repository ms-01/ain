// Copyright (c) 2019-2024 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#ifndef DEFI_INDEX_DISKTXPOS_H
#define DEFI_INDEX_DISKTXPOS_H

#include <flatfile.h>
#include <serialize.h>

/**
 * Location of a transaction inside a block file.
 *
 * The position of the block itself is given by the inherited FlatFilePos, the
 * offset of the transaction inside the serialized block (after the block
 * header) is given by nTxOffset.
 */
struct CDiskTxPos : public FlatFilePos
{
    unsigned int nTxOffset{0}; // after header

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITEAS(FlatFilePos, *this);
        READWRITE(VARINT(nTxOffset));
    }

    CDiskTxPos(const FlatFilePos& blockIn, unsigned int nTxOffsetIn) : FlatFilePos(blockIn.nFile, blockIn.nPos), nTxOffset(nTxOffsetIn) {
    }

    CDiskTxPos() {
        SetNull();
    }

    void SetNull() {
        FlatFilePos::SetNull();
        nTxOffset = 0;
    }
};

#endif // DEFI_INDEX_DISKTXPOS_H
