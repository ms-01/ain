// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#ifndef DEFI_ADDRDB_H
#define DEFI_ADDRDB_H

#include <fs.h>
#include <serialize.h>
#include <univalue.h>

#include <string>
#include <map>
#include <vector>

class CSubNet;
class CAddrMan;
class CDataStream;
class CAddress;

/** File name of the anchor peer database (inside the network data directory). */
static const char* const ANCHORS_DATABASE_FILENAME = "anchors.dat";

/**
 * Maximum number of operator-configured anchor peers (block-relay anchors).
 *
 * Anchor peers are prioritized for the limited block-relay-only outbound
 * slots. They are a connectivity/priority concept only: blocks and headers
 * received from an anchor peer are validated exactly like blocks and headers
 * received from any other peer.
 */
static const size_t MAX_BLOCK_RELAY_ONLY_ANCHORS = 2;

typedef enum BanReason
{
    BanReasonUnknown          = 0,
    BanReasonNodeMisbehaving  = 1,
    BanReasonManuallyAdded    = 2
} BanReason;

class CBanEntry
{
public:
    static const int CURRENT_VERSION=1;
    int nVersion;
    int64_t nCreateTime;
    int64_t nBanUntil;
    uint8_t banReason;

    CBanEntry()
    {
        SetNull();
    }

    explicit CBanEntry(int64_t nCreateTimeIn)
    {
        SetNull();
        nCreateTime = nCreateTimeIn;
    }

    explicit CBanEntry(int64_t n_create_time_in, BanReason ban_reason_in) : CBanEntry(n_create_time_in)
    {
        banReason = ban_reason_in;
    }

    /**
     * Create a ban entry from its JSON representation (as created by ToJson()).
     * @param[in] json A JSON representation of a ban entry.
     * @throw std::runtime_error if the JSON does not have the expected fields.
     */
    explicit CBanEntry(const UniValue& json);

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(this->nVersion);
        READWRITE(nCreateTime);
        READWRITE(nBanUntil);
        READWRITE(banReason);
    }

    void SetNull()
    {
        nVersion = CBanEntry::CURRENT_VERSION;
        nCreateTime = 0;
        nBanUntil = 0;
        banReason = BanReasonUnknown;
    }

    std::string banReasonToString() const
    {
        switch (banReason) {
        case BanReasonNodeMisbehaving:
            return "node misbehaving";
        case BanReasonManuallyAdded:
            return "manually added";
        default:
            return "unknown";
        }
    }

    /**
     * Map a ban reason string back to its enum value. Unknown strings are
     * mapped to BanReasonUnknown, so that a banlist written by another
     * implementation never aborts the load.
     */
    static BanReason banReasonFromString(const std::string& reason)
    {
        if (reason == "node misbehaving") return BanReasonNodeMisbehaving;
        if (reason == "manually added") return BanReasonManuallyAdded;
        return BanReasonUnknown;
    }

    /**
     * Generate a JSON representation of this ban entry.
     * @return JSON suitable for passing to the `CBanEntry(const UniValue&)` constructor.
     */
    UniValue ToJson() const;
};

typedef std::map<CSubNet, CBanEntry> banmap_t;

/** Access to the (IP) address database (peers.dat) */
class CAddrDB
{
private:
    fs::path pathAddr;
public:
    CAddrDB();
    bool Write(const CAddrMan& addr);
    bool Read(CAddrMan& addr);
    static bool Read(CAddrMan& addr, CDataStream& ssPeers);
};

/** Access to the banlist databases (banlist.json and the legacy banlist.dat) */
class CBanDB
{
private:
    /**
     * JSON key under which the data is stored in the json database.
     */
    static constexpr const char* JSON_KEY = "banned_nets";

    const fs::path m_banlist_dat;
    const fs::path m_banlist_json;
public:
    explicit CBanDB(fs::path ban_list_path);
    bool Write(const banmap_t& banSet);

    /**
     * Read the banlist from disk.
     *
     * The human-readable `banlist.json` is the primary file. If it does not
     * exist, the legacy binary `banlist.dat` is read once as a migration
     * fallback (and never written again).
     *
     * @param[out] banSet The loaded list. Set if `true` is returned, otherwise it is left
     * in an undefined state.
     * @param[out] dirty Indicates whether the loaded list needs flushing to disk. Set if
     * `true` is returned, otherwise it is left in an undefined state.
     * @return true on success
     */
    bool Read(banmap_t& banSet, bool& dirty);
};

/**
 * Read the operator-configured anchor peers (block-relay anchors) from disk.
 * The file contains one "ip:port" entry per line. Empty lines and lines
 * starting with '#' are ignored. Invalid, unspecified and duplicate entries
 * are skipped individually, so that a single broken entry never discards the
 * remaining valid anchors. A missing or unreadable file yields an empty list.
 *
 * @param[in] anchors_db_path Path of the anchors file.
 * @return The list of valid anchor peer addresses (at most
 *         MAX_BLOCK_RELAY_ONLY_ANCHORS entries).
 */
std::vector<CAddress> ReadAnchors(const fs::path& anchors_db_path);

/**
 * Write the operator-configured anchor peers (block-relay anchors) to disk.
 *
 * The file is written atomically (temporary file + rename).
 *
 * @param[in] anchors_db_path Path of the anchors file.
 * @param[in] anchors The anchor peer addresses to store.
 */
void DumpAnchors(const fs::path& anchors_db_path, const std::vector<CAddress>& anchors);

#endif // DEFI_ADDRDB_H
