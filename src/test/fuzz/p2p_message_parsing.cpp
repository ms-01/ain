// Copyright (c) 2020-2026 The Bitcoin Core developers
// Copyright (c) DeFi Blockchain Developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <blockencodings.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <protocol.h>
#include <streams.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <version.h>

#include <cstdint>
#include <string>
#include <vector>

FUZZ_TARGET(p2p_message_parsing)
{
    FuzzedDataProvider fdp(buffer.data(), buffer.size());

    const int proto_version = fdp.ConsumeIntegral<int>();
    const uint8_t msg_choice = fdp.ConsumeIntegralInRange<uint8_t>(0, 11);
    const auto payload = fdp.ConsumeRemainingBytes<uint8_t>();

    CDataStream ds(SER_NETWORK, proto_version);
    if (!payload.empty()) {
        ds.write(reinterpret_cast<const char*>(payload.data()), payload.size());
    }

    try {
        switch (msg_choice) {
        case 0: {
            CMessageHeader::MessageStartChars magic = {0xf9, 0xbe, 0xb4, 0xd9};
            CMessageHeader hdr(magic);
            ds >> hdr;
            (void)hdr.IsValid(magic);
            (void)hdr.GetCommand();
            break;
        }
        case 1: {
            int version = 0;
            uint64_t services = 0;
            int64_t timestamp = 0;
            CAddress addr_recv;
            CAddress addr_from;
            uint64_t nonce = 0;
            std::string subver;
            int start_height = 0;
            bool relay = true;
            ds >> version >> services >> timestamp >> addr_recv;
            if (!ds.empty()) {
                ds >> addr_from >> nonce >> subver >> start_height;
            }
            if (!ds.empty()) {
                ds >> relay;
            }
            break;
        }
        case 2: {
            std::vector<CAddress> addrs;
            ds >> addrs;
            break;
        }
        case 3: {
            std::vector<CAddress> addrs;
            (void)DecodeAddrV2Message(ds, addrs);
            break;
        }
        case 4: {
            std::vector<CInv> invs;
            ds >> invs;
            break;
        }
        case 5: {
            std::vector<CBlockHeader> headers;
            ds >> headers;
            break;
        }
        case 6: {
            CBlockHeaderAndShortTxIDs cmpctblock;
            ds >> cmpctblock;
            break;
        }
        case 7: {
            BlockTransactions blocktxn;
            ds >> blocktxn;
            break;
        }
        case 8: {
            BlockTransactionsRequest req;
            ds >> req;
            break;
        }
        case 9: {
            CMutableTransaction mtx;
            ds >> mtx;
            break;
        }
        case 10: {
            int64_t feefilter = 0;
            ds >> feefilter;
            break;
        }
        case 11: {
            bool announce = false;
            uint64_t version = 0;
            ds >> announce >> version;
            break;
        }
        }
    } catch (const std::ios_base::failure&) {
    }
}
