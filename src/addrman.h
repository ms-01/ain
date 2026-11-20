// Copyright (c) 2012 Pieter Wuille
// Copyright (c) 2012-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#ifndef DEFI_ADDRMAN_H
#define DEFI_ADDRMAN_H

#include <netaddress.h>
#include <protocol.h>
#include <random.h>
#include <sync.h>
#include <timedata.h>
#include <util/system.h>

#include <ios>
#include <map>
#include <optional>
#include <set>
#include <stdint.h>
#include <string>
#include <vector>

/**
 * The format of the on-disk addrman (the file is named `peers.dat`).
 *
 * The first byte of the file is the format byte:
 *
 *  - 1: the legacy format. Every address is encoded in the legacy
 *    (pre-BIP155) format, i.e. every address has a 16 byte representation.
 *  - 2: the BIP155-capable format. The addresses that have no legacy
 *    representation (Tor v3 onion addresses and I2P destinations) are encoded
 *    in the BIP155 (ADDRv2) format.
 *
 * The layout of the file is identical in both formats; only the encoding of
 * the entries differs.
 */
static constexpr uint8_t ADDRMAN_FILE_FORMAT_LEGACY{1};
static constexpr uint8_t ADDRMAN_FILE_FORMAT_ADDRV2{2};

/**
 * The bit in the per-address disk version word that marks an address that is
 * encoded in the BIP155 (ADDRv2) format.
 *
 * The word itself is the client version, exactly as the previous
 * implementation wrote it; only this bit is interpreted, the remaining bits
 * are ignored. This mirrors the way upstream Bitcoin Core repurposes the
 * stored version word of an address.
 */
static constexpr uint32_t DISK_VERSION_ADDRV2{1 << 29};

/**
 * Thrown when the addrman file has a format that this version of the node does
 * not understand. The caller is expected to back the file up and to continue
 * with an empty address manager.
 */
class InvalidAddrManVersionError : public std::ios_base::failure
{
public:
    explicit InvalidAddrManVersionError(const std::string& msg) : std::ios_base::failure(msg) {}
};

/**
 * Extended statistics about a CAddress
 */
class CAddrInfo : public CAddress
{
public:
    //! last try whatsoever by us (memory only)
    int64_t nLastTry{0};

    //! last counted attempt (memory only)
    int64_t nLastCountAttempt{0};

private:
    //! where knowledge about this address first came from
    CNetAddr source;

    //! last successful connection by us
    int64_t nLastSuccess{0};

    //! connection attempts since last successful attempt
    int nAttempts{0};

    //! reference count in new sets (memory only)
    int nRefCount{0};

    //! in tried set? (memory only)
    bool fInTried{false};

    //! position in vRandom
    int nRandomPos{-1};

    friend class CAddrMan;

    /**
     * The address in the BIP155 (ADDRv2) body format: the timestamp, the
     * service flags, the network id, the address and the port.
     */
    template <typename Stream>
    void SerializeAddrV2Body(Stream& s) const
    {
        ::Serialize(s, nTime);
        uint64_t nServicesInt = nServices;
        ::WriteCompactSize(s, nServicesInt);
        ::Serialize(s, GetBIP155Network());
        const std::vector<unsigned char> vBytes = GetAddrV2Bytes();
        ::WriteCompactSize(s, vBytes.size());
        if (!vBytes.empty()) {
            s.write(reinterpret_cast<const char*>(vBytes.data()), vBytes.size());
        }
        ::Serialize(s, WrapBigEndian(port));
    }

    //! The counterpart of SerializeAddrV2Body().
    template <typename Stream>
    void UnserializeAddrV2Body(Stream& s)
    {
        ::Unserialize(s, nTime);
        uint64_t nServicesInt{0};
        nServicesInt = ::ReadCompactSize(s);
        nServices = static_cast<ServiceFlags>(nServicesInt);
        uint8_t nNetwork{0};
        ::Unserialize(s, nNetwork);
        std::vector<unsigned char> vBytes = ReadAddrV2Bytes(s);
        if (!SetFromBIP155(nNetwork, vBytes.data(), vBytes.size())) {
            throw std::ios_base::failure("Corrupt CAddrMan serialization: invalid address.");
        }
        ::Unserialize(s, WrapBigEndian(port));
    }

    //! A network address in the BIP155 (ADDRv2) format: the network id and the address.
    template <typename Stream>
    static void SerializeNetAddrV2(Stream& s, const CNetAddr& addr)
    {
        ::Serialize(s, addr.GetBIP155Network());
        const std::vector<unsigned char> vBytes = addr.GetAddrV2Bytes();
        ::WriteCompactSize(s, vBytes.size());
        if (!vBytes.empty()) {
            s.write(reinterpret_cast<const char*>(vBytes.data()), vBytes.size());
        }
    }

    //! The counterpart of SerializeNetAddrV2().
    template <typename Stream>
    static void UnserializeNetAddrV2(Stream& s, CNetAddr& addr)
    {
        uint8_t nNetwork{0};
        ::Unserialize(s, nNetwork);
        const std::vector<unsigned char> vBytes = ReadAddrV2Bytes(s);
        if (!addr.SetFromBIP155(nNetwork, vBytes.data(), vBytes.size())) {
            throw std::ios_base::failure("Corrupt CAddrMan serialization: invalid source address.");
        }
    }

    /**
     * Read the address bytes of a BIP155 (ADDRv2) entry. The length is
     * validated, so that a corrupt file cannot cause an unbounded allocation.
     */
    template <typename Stream>
    static std::vector<unsigned char> ReadAddrV2Bytes(Stream& s)
    {
        uint64_t nSize{0};
        nSize = ::ReadCompactSize(s);
        if (nSize > MAX_ADDRV2_SIZE) {
            throw std::ios_base::failure("Corrupt CAddrMan serialization: address too large.");
        }
        std::vector<unsigned char> vBytes(nSize);
        if (nSize > 0) {
            s.read(reinterpret_cast<char*>(vBytes.data()), nSize);
        }
        return vBytes;
    }

public:

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITEAS(CAddress, *this);
        READWRITE(source);
        READWRITE(nLastSuccess);
        READWRITE(nAttempts);
    }

    /**
     * Serialize the entry in the BIP155-capable disk format.
     *
     * The entry starts with the per-address disk version word (the client
     * version, with the DISK_VERSION_ADDRV2 bit set if the address is encoded
     * in the BIP155 format), followed by the address itself, the source of the
     * entry and its statistics.
     *
     * The source of the entry is encoded in the BIP155 format as well if it
     * has no legacy representation: the "new" bucket of an entry is derived
     * from the group of its source, so the source must survive a restart
     * unchanged.
     */
    template <typename Stream>
    void SerializeAddrV2(Stream& s) const
    {
        const bool addr_v2 = !IsAddrV1Compatible();
        uint32_t nMarker = static_cast<uint32_t>(s.GetVersion());
        if (addr_v2) {
            nMarker |= DISK_VERSION_ADDRV2;
        }
        ::Serialize(s, nMarker);
        if (addr_v2) {
            SerializeAddrV2Body(s);
        } else {
            SerializeBody(s, CSerActionSerialize(), static_cast<int>(nMarker));
        }
        const bool source_v2 = !source.IsAddrV1Compatible();
        ::Serialize(s, static_cast<uint8_t>(source_v2 ? 1 : 0));
        if (source_v2) {
            SerializeNetAddrV2(s, source);
        } else {
            ::Serialize(s, source);
        }
        ::Serialize(s, nLastSuccess);
        ::Serialize(s, nAttempts);
    }

    /**
     * Deserialize an entry that was written in the BIP155-capable disk format.
     */
    template <typename Stream>
    void UnserializeAddrV2(Stream& s)
    {
        Init();
        uint32_t nMarker{0};
        ::Unserialize(s, nMarker);
        if (nMarker & DISK_VERSION_ADDRV2) {
            UnserializeAddrV2Body(s);
        } else {
            SerializeBody(s, CSerActionUnserialize(), static_cast<int>(nMarker));
        }
        uint8_t nSourceFormat{0};
        ::Unserialize(s, nSourceFormat);
        switch (nSourceFormat) {
        case 0:
            ::Unserialize(s, source);
            break;
        case 1:
            UnserializeNetAddrV2(s, source);
            break;
        default:
            throw std::ios_base::failure("Corrupt CAddrMan serialization: unknown source format.");
        }
        ::Unserialize(s, nLastSuccess);
        ::Unserialize(s, nAttempts);
    }

    CAddrInfo(const CAddress &addrIn, const CNetAddr &addrSource) : CAddress(addrIn), source(addrSource)
    {
    }

    CAddrInfo() : CAddress(), source()
    {
    }

    //! Calculate in which "tried" bucket this entry belongs
    int GetTriedBucket(const uint256 &nKey) const;

    //! Calculate in which "new" bucket this entry belongs, given a certain source
    int GetNewBucket(const uint256 &nKey, const CNetAddr& src) const;

    //! Calculate in which "new" bucket this entry belongs, using its default source
    int GetNewBucket(const uint256 &nKey) const
    {
        return GetNewBucket(nKey, source);
    }

    //! Calculate in which position of a bucket to store this entry.
    int GetBucketPosition(const uint256 &nKey, bool fNew, int nBucket) const;

    //! Determine whether the statistics about this entry are bad enough so that it can just be deleted
    bool IsTerrible(int64_t nNow = GetAdjustedTime()) const;

    //! Calculate the relative chance this entry should be given when selecting nodes to connect to
    double GetChance(int64_t nNow = GetAdjustedTime()) const;
};

/** Stochastic address manager
 *
 * Design goals:
 *  * Keep the address tables in-memory, and asynchronously dump the entire table to peers.dat.
 *  * Make sure no (localized) attacker can fill the entire table with his nodes/addresses.
 *
 * To that end:
 *  * Addresses are organized into buckets.
 *    * Addresses that have not yet been tried go into 1024 "new" buckets.
 *      * Based on the address range (/16 for IPv4) of the source of information, 64 buckets are selected at random.
 *      * The actual bucket is chosen from one of these, based on the range in which the address itself is located.
 *      * One single address can occur in up to 8 different buckets to increase selection chances for addresses that
 *        are seen frequently. The chance for increasing this multiplicity decreases exponentially.
 *      * When adding a new address to a full bucket, a randomly chosen entry (with a bias favoring less recently seen
 *        ones) is removed from it first.
 *    * Addresses of nodes that are known to be accessible go into 256 "tried" buckets.
 *      * Each address range selects at random 8 of these buckets.
 *      * The actual bucket is chosen from one of these, based on the full address.
 *      * When adding a new good address to a full bucket, a randomly chosen entry (with a bias favoring less recently
 *        tried ones) is evicted from it, back to the "new" buckets.
 *    * Bucket selection is based on cryptographic hashing, using a randomly-generated 256-bit key, which should not
 *      be observable by adversaries.
 *    * Several indexes are kept for high performance. Defining DEBUG_ADDRMAN will introduce frequent (and expensive)
 *      consistency checks for the entire data structure.
 */

//! total number of buckets for tried addresses
#define ADDRMAN_TRIED_BUCKET_COUNT_LOG2 8

//! total number of buckets for new addresses
#define ADDRMAN_NEW_BUCKET_COUNT_LOG2 10

//! maximum allowed number of entries in buckets for new and tried addresses
#define ADDRMAN_BUCKET_SIZE_LOG2 6

//! over how many buckets entries with tried addresses from a single group (/16 for IPv4) are spread
#define ADDRMAN_TRIED_BUCKETS_PER_GROUP 8

//! over how many buckets entries with new addresses originating from a single group are spread
#define ADDRMAN_NEW_BUCKETS_PER_SOURCE_GROUP 64

//! in how many buckets for entries with new addresses a single address may occur
#define ADDRMAN_NEW_BUCKETS_PER_ADDRESS 8

//! how old addresses can maximally be
#define ADDRMAN_HORIZON_DAYS 30

//! after how many failed attempts we give up on a new node
#define ADDRMAN_RETRIES 3

//! how many successive failures are allowed ...
#define ADDRMAN_MAX_FAILURES 10

//! ... in at least this many days
#define ADDRMAN_MIN_FAIL_DAYS 7

//! how recent a successful connection should be before we allow an address to be evicted from tried
#define ADDRMAN_REPLACEMENT_HOURS 4

//! the maximum percentage of nodes to return in a getaddr call
#define ADDRMAN_GETADDR_MAX_PCT 23

//! the maximum number of nodes to return in a getaddr call
#define ADDRMAN_GETADDR_MAX 2500

//! Convenience
#define ADDRMAN_TRIED_BUCKET_COUNT (1 << ADDRMAN_TRIED_BUCKET_COUNT_LOG2)
#define ADDRMAN_NEW_BUCKET_COUNT (1 << ADDRMAN_NEW_BUCKET_COUNT_LOG2)
#define ADDRMAN_BUCKET_SIZE (1 << ADDRMAN_BUCKET_SIZE_LOG2)

//! the maximum number of tried addr collisions to store
#define ADDRMAN_SET_TRIED_COLLISION_SIZE 10

//! the maximum time we'll spend trying to resolve a tried table collision, in seconds
static const int64_t ADDRMAN_TEST_WINDOW = 40*60; // 40 minutes

/**
 * Stochastical (IP) address manager
 */
class CAddrMan
{
protected:
    //! critical section to protect the inner data structures
    mutable CCriticalSection cs;

private:
    //! last used nId
    int nIdCount GUARDED_BY(cs);

    //! table with information about all nIds
    std::map<int, CAddrInfo> mapInfo GUARDED_BY(cs);

    //! find an nId based on its network address
    std::map<CNetAddr, int> mapAddr GUARDED_BY(cs);

    //! randomly-ordered vector of all nIds
    std::vector<int> vRandom GUARDED_BY(cs);

    // number of "tried" entries
    int nTried GUARDED_BY(cs);

    //! list of "tried" buckets
    int vvTried[ADDRMAN_TRIED_BUCKET_COUNT][ADDRMAN_BUCKET_SIZE] GUARDED_BY(cs);

    //! number of (unique) "new" entries
    int nNew GUARDED_BY(cs);

    //! list of "new" buckets
    int vvNew[ADDRMAN_NEW_BUCKET_COUNT][ADDRMAN_BUCKET_SIZE] GUARDED_BY(cs);

    //! last time Good was called (memory only)
    int64_t nLastGood GUARDED_BY(cs);

    //! Holds addrs inserted into tried table that collide with existing entries. Test-before-evict discipline used to resolve these collisions.
    std::set<int> m_tried_collisions;

protected:
    //! secret key to randomize bucket select with
    uint256 nKey;

    //! Source of random numbers for randomization in inner loops
    FastRandomContext insecure_rand;

    //! Find an entry.
    CAddrInfo* Find(const CNetAddr& addr, int *pnId = nullptr) EXCLUSIVE_LOCKS_REQUIRED(cs);

    //! find an entry, creating it if necessary.
    //! nTime and nServices of the found node are updated, if necessary.
    CAddrInfo* Create(const CAddress &addr, const CNetAddr &addrSource, int *pnId = nullptr) EXCLUSIVE_LOCKS_REQUIRED(cs);

    //! Swap two elements in vRandom.
    void SwapRandom(unsigned int nRandomPos1, unsigned int nRandomPos2) EXCLUSIVE_LOCKS_REQUIRED(cs);

    //! Move an entry from the "new" table(s) to the "tried" table
    void MakeTried(CAddrInfo& info, int nId) EXCLUSIVE_LOCKS_REQUIRED(cs);

    //! Delete an entry. It must not be in tried, and have refcount 0.
    void Delete(int nId) EXCLUSIVE_LOCKS_REQUIRED(cs);

    //! Clear a position in a "new" table. This is the only place where entries are actually deleted.
    void ClearNew(int nUBucket, int nUBucketPos) EXCLUSIVE_LOCKS_REQUIRED(cs);

    //! Mark an entry "good", possibly moving it from "new" to "tried".
    void Good_(const CService &addr, bool test_before_evict, int64_t time) EXCLUSIVE_LOCKS_REQUIRED(cs);

    //! Add an entry to the "new" table.
    bool Add_(const CAddress &addr, const CNetAddr& source, int64_t nTimePenalty) EXCLUSIVE_LOCKS_REQUIRED(cs);

    //! Mark an entry as attempted to connect.
    void Attempt_(const CService &addr, bool fCountFailure, int64_t nTime) EXCLUSIVE_LOCKS_REQUIRED(cs);

    //! Select an address to connect to, if newOnly is set to true, only the new table is selected from.
    CAddrInfo Select_(bool newOnly) EXCLUSIVE_LOCKS_REQUIRED(cs);

    //! See if any to-be-evicted tried table entries have been tested and if so resolve the collisions.
    void ResolveCollisions_() EXCLUSIVE_LOCKS_REQUIRED(cs);

    //! Return a random to-be-evicted tried table address.
    CAddrInfo SelectTriedCollision_() EXCLUSIVE_LOCKS_REQUIRED(cs);

#ifdef DEBUG_ADDRMAN
    //! Perform consistency check. Returns an error code or zero.
    int Check_() EXCLUSIVE_LOCKS_REQUIRED(cs);
#endif

    //! Select several addresses at once.
    void GetAddr_(std::vector<CAddress> &vAddr, std::optional<Network> network, size_t max_addresses, size_t max_pct) EXCLUSIVE_LOCKS_REQUIRED(cs);

    //! Mark an entry as currently-connected-to.
    void Connected_(const CService &addr, int64_t nTime) EXCLUSIVE_LOCKS_REQUIRED(cs);

    //! Update an entry's service bits.
    void SetServices_(const CService &addr, ServiceFlags nServices) EXCLUSIVE_LOCKS_REQUIRED(cs);

    /**
     * Whether the addrman contains an address or a source that has no legacy
     * (pre-BIP155) representation and therefore needs the BIP155 encoding.
     */
    bool HasAddrV2Entries_() const EXCLUSIVE_LOCKS_REQUIRED(cs)
    {
        for (const auto& entry : mapInfo) {
            const CAddrInfo& info = entry.second;
            if (!info.IsAddrV1Compatible() || !info.source.IsAddrV1Compatible()) {
                return true;
            }
        }
        return false;
    }

    //! Serialize a single entry of the addrman in the given file format.
    template<typename Stream>
    static void SerializeEntry(Stream& s, const CAddrInfo& info, uint8_t nFormat)
    {
        if (nFormat == ADDRMAN_FILE_FORMAT_ADDRV2) {
            info.SerializeAddrV2(s);
        } else {
            s << info;
        }
    }

    //! Deserialize a single entry of the addrman from the given file format.
    template<typename Stream>
    static void UnserializeEntry(Stream& s, CAddrInfo& info, uint8_t nFormat)
    {
        if (nFormat == ADDRMAN_FILE_FORMAT_ADDRV2) {
            info.UnserializeAddrV2(s);
        } else {
            s >> info;
        }
    }

public:
    /**
     * The format the addrman is written in: the legacy format as long as it
     * contains no address (and no source) that needs the BIP155 encoding, the
     * BIP155-capable format as soon as it does.
     */
    uint8_t GetFileFormat() const
    {
        LOCK(cs);
        return HasAddrV2Entries_() ? ADDRMAN_FILE_FORMAT_ADDRV2 : ADDRMAN_FILE_FORMAT_LEGACY;
    }

    /**
     * serialized format:
     * * format byte (1: legacy, 2: BIP155-capable)
     * * 0x20 + nKey (serialized as if it were a vector, for backward compatibility)
     * * nNew
     * * nTried
     * * number of "new" buckets XOR 2**30
     * * all nNew addrinfos in vvNew
     * * all nTried addrinfos in vvTried
     * * for each bucket:
     *   * number of elements
     *   * for each element: index
     *
     * 2**30 is xorred with the number of buckets to make addrman deserializer v0 detect it
     * as incompatible. This is necessary because it did not check the version number on
     * deserialization.
     *
     * The format byte selects the encoding of the entries: in the legacy
     * format (1) every address is encoded in the legacy (pre-BIP155) format,
     * in the BIP155-capable format (2) the addresses that have no legacy
     * representation (Tor v3 onion addresses and I2P destinations) are encoded
     * in the BIP155 (ADDRv2) format. The layout of the file is the same in
     * both formats.
     *
     * Notice that vvTried, mapAddr and vVector are never encoded explicitly;
     * they are instead reconstructed from the other information.
     *
     * vvNew is serialized, but only used if ADDRMAN_UNKNOWN_BUCKET_COUNT didn't change,
     * otherwise it is reconstructed as well.
     *
     * This format is more complex, but significantly smaller (at most 1.5 MiB), and supports
     * changes to the ADDRMAN_ parameters without breaking the on-disk structure.
     *
     * We don't use ADD_SERIALIZE_METHODS since the serialization and deserialization code has
     * very little in common.
     */
    template<typename Stream>
    void Serialize(Stream &s) const
    {
        LOCK(cs);

        // The legacy format is used as long as the addrman contains no
        // address that needs the BIP155 encoding, so that the file stays
        // readable by the nodes that do not support it.
        const uint8_t nFormat = HasAddrV2Entries_() ? ADDRMAN_FILE_FORMAT_ADDRV2 : ADDRMAN_FILE_FORMAT_LEGACY;
        s << nFormat;
        s << ((unsigned char)32);
        s << nKey;
        s << nNew;
        s << nTried;

        int nUBuckets = ADDRMAN_NEW_BUCKET_COUNT ^ (1 << 30);
        s << nUBuckets;
        std::map<int, int> mapUnkIds;
        int nIds = 0;
        for (const auto& entry : mapInfo) {
            mapUnkIds[entry.first] = nIds;
            const CAddrInfo &info = entry.second;
            if (info.nRefCount) {
                assert(nIds != nNew); // this means nNew was wrong, oh ow
                SerializeEntry(s, info, nFormat);
                nIds++;
            }
        }
        nIds = 0;
        for (const auto& entry : mapInfo) {
            const CAddrInfo &info = entry.second;
            if (info.fInTried) {
                assert(nIds != nTried); // this means nTried was wrong, oh ow
                SerializeEntry(s, info, nFormat);
                nIds++;
            }
        }
        for (int bucket = 0; bucket < ADDRMAN_NEW_BUCKET_COUNT; bucket++) {
            int nSize = 0;
            for (int i = 0; i < ADDRMAN_BUCKET_SIZE; i++) {
                if (vvNew[bucket][i] != -1)
                    nSize++;
            }
            s << nSize;
            for (int i = 0; i < ADDRMAN_BUCKET_SIZE; i++) {
                if (vvNew[bucket][i] != -1) {
                    int nIndex = mapUnkIds[vvNew[bucket][i]];
                    s << nIndex;
                }
            }
        }
    }

    template<typename Stream>
    void Unserialize(Stream& s)
    {
        LOCK(cs);

        Clear();

        unsigned char nVersion;
        s >> nVersion;
        if (nVersion != ADDRMAN_FILE_FORMAT_LEGACY && nVersion != ADDRMAN_FILE_FORMAT_ADDRV2) {
            throw InvalidAddrManVersionError("Unsupported format of the addrman database: " + std::to_string(nVersion));
        }
        unsigned char nKeySize;
        s >> nKeySize;
        if (nKeySize != 32) throw std::ios_base::failure("Incorrect keysize in addrman deserialization");
        s >> nKey;
        s >> nNew;
        s >> nTried;
        int nUBuckets = 0;
        s >> nUBuckets;
        if (nVersion != 0) {
            nUBuckets ^= (1 << 30);
        }

        if (nNew > ADDRMAN_NEW_BUCKET_COUNT * ADDRMAN_BUCKET_SIZE) {
            throw std::ios_base::failure("Corrupt CAddrMan serialization, nNew exceeds limit.");
        }

        if (nTried > ADDRMAN_TRIED_BUCKET_COUNT * ADDRMAN_BUCKET_SIZE) {
            throw std::ios_base::failure("Corrupt CAddrMan serialization, nTried exceeds limit.");
        }

        // Deserialize entries from the new table.
        for (int n = 0; n < nNew; n++) {
            CAddrInfo &info = mapInfo[n];
            UnserializeEntry(s, info, nVersion);
            mapAddr[info] = n;
            info.nRandomPos = vRandom.size();
            vRandom.push_back(n);
            if (nUBuckets != ADDRMAN_NEW_BUCKET_COUNT) {
                // In case the new table data cannot be used (bucket count wrong),
                // immediately try to give them a reference based on their primary source address.
                int nUBucket = info.GetNewBucket(nKey);
                int nUBucketPos = info.GetBucketPosition(nKey, true, nUBucket);
                if (vvNew[nUBucket][nUBucketPos] == -1) {
                    vvNew[nUBucket][nUBucketPos] = n;
                    info.nRefCount++;
                }
            }
        }
        nIdCount = nNew;

        // Deserialize entries from the tried table.
        int nLost = 0;
        for (int n = 0; n < nTried; n++) {
            CAddrInfo info;
            UnserializeEntry(s, info, nVersion);
            int nKBucket = info.GetTriedBucket(nKey);
            int nKBucketPos = info.GetBucketPosition(nKey, false, nKBucket);
            if (vvTried[nKBucket][nKBucketPos] == -1) {
                info.nRandomPos = vRandom.size();
                info.fInTried = true;
                vRandom.push_back(nIdCount);
                mapInfo[nIdCount] = info;
                mapAddr[info] = nIdCount;
                vvTried[nKBucket][nKBucketPos] = nIdCount;
                nIdCount++;
            } else {
                nLost++;
            }
        }
        nTried -= nLost;

        // Deserialize positions in the new table (if possible).
        for (int bucket = 0; bucket < nUBuckets; bucket++) {
            int nSize = 0;
            s >> nSize;
            for (int n = 0; n < nSize; n++) {
                int nIndex = 0;
                s >> nIndex;
                if (nIndex >= 0 && nIndex < nNew) {
                    CAddrInfo &info = mapInfo[nIndex];
                    int nUBucketPos = info.GetBucketPosition(nKey, true, bucket);
                    if (nUBuckets == ADDRMAN_NEW_BUCKET_COUNT && vvNew[bucket][nUBucketPos] == -1 && info.nRefCount < ADDRMAN_NEW_BUCKETS_PER_ADDRESS) {
                        info.nRefCount++;
                        vvNew[bucket][nUBucketPos] = nIndex;
                    }
                }
            }
        }

        // Prune new entries with refcount 0 (as a result of collisions).
        int nLostUnk = 0;
        for (std::map<int, CAddrInfo>::const_iterator it = mapInfo.begin(); it != mapInfo.end(); ) {
            if (it->second.fInTried == false && it->second.nRefCount == 0) {
                std::map<int, CAddrInfo>::const_iterator itCopy = it++;
                Delete(itCopy->first);
                nLostUnk++;
            } else {
                it++;
            }
        }
        if (nLost + nLostUnk > 0) {
            LogPrint(BCLog::ADDRMAN, "addrman lost %i new and %i tried addresses due to collisions\n", nLostUnk, nLost);
        }

        Check();
    }

    void Clear()
    {
        LOCK(cs);
        std::vector<int>().swap(vRandom);
        nKey = insecure_rand.rand256();
        for (size_t bucket = 0; bucket < ADDRMAN_NEW_BUCKET_COUNT; bucket++) {
            for (size_t entry = 0; entry < ADDRMAN_BUCKET_SIZE; entry++) {
                vvNew[bucket][entry] = -1;
            }
        }
        for (size_t bucket = 0; bucket < ADDRMAN_TRIED_BUCKET_COUNT; bucket++) {
            for (size_t entry = 0; entry < ADDRMAN_BUCKET_SIZE; entry++) {
                vvTried[bucket][entry] = -1;
            }
        }

        nIdCount = 0;
        nTried = 0;
        nNew = 0;
        nLastGood = 1; //Initially at 1 so that "never" is strictly worse.
        mapInfo.clear();
        mapAddr.clear();
    }

    CAddrMan()
    {
        Clear();
    }

    ~CAddrMan()
    {
        nKey.SetNull();
    }

    //! Return the number of (unique) addresses in all tables.
    size_t size() const
    {
        LOCK(cs); // TODO: Cache this in an atomic to avoid this overhead
        return vRandom.size();
    }

    //! Consistency check
    void Check()
    {
#ifdef DEBUG_ADDRMAN
        {
            LOCK(cs);
            int err;
            if ((err=Check_()))
                LogPrintf("ADDRMAN CONSISTENCY CHECK FAILED!!! err=%i\n", err);
        }
#endif
    }

    //! Add a single address.
    bool Add(const CAddress &addr, const CNetAddr& source, int64_t nTimePenalty = 0)
    {
        LOCK(cs);
        bool fRet = false;
        Check();
        fRet |= Add_(addr, source, nTimePenalty);
        Check();
        if (fRet) {
            LogPrint(BCLog::ADDRMAN, "Added %s from %s: %i tried, %i new\n", addr.ToStringIPPort(), source.ToString(), nTried, nNew);
        }
        return fRet;
    }

    //! Add multiple addresses.
    bool Add(const std::vector<CAddress> &vAddr, const CNetAddr& source, int64_t nTimePenalty = 0)
    {
        LOCK(cs);
        int nAdd = 0;
        Check();
        for (std::vector<CAddress>::const_iterator it = vAddr.begin(); it != vAddr.end(); it++)
            nAdd += Add_(*it, source, nTimePenalty) ? 1 : 0;
        Check();
        if (nAdd) {
            LogPrint(BCLog::ADDRMAN, "Added %i addresses from %s: %i tried, %i new\n", nAdd, source.ToString(), nTried, nNew);
        }
        return nAdd > 0;
    }

    //! Mark an entry as accessible.
    void Good(const CService &addr, bool test_before_evict = true, int64_t nTime = GetAdjustedTime())
    {
        LOCK(cs);
        Check();
        Good_(addr, test_before_evict, nTime);
        Check();
    }

    //! Mark an entry as connection attempted to.
    void Attempt(const CService &addr, bool fCountFailure, int64_t nTime = GetAdjustedTime())
    {
        LOCK(cs);
        Check();
        Attempt_(addr, fCountFailure, nTime);
        Check();
    }

    //! See if any to-be-evicted tried table entries have been tested and if so resolve the collisions.
    void ResolveCollisions()
    {
        LOCK(cs);
        Check();
        ResolveCollisions_();
        Check();
    }

    //! Randomly select an address in tried that another address is attempting to evict.
    CAddrInfo SelectTriedCollision()
    {
        CAddrInfo ret;
        {
            LOCK(cs);
            Check();
            ret = SelectTriedCollision_();
            Check();
        }
        return ret;
    }

    /**
     * Choose an address to connect to.
     */
    CAddrInfo Select(bool newOnly = false)
    {
        CAddrInfo addrRet;
        {
            LOCK(cs);
            Check();
            addrRet = Select_(newOnly);
            Check();
        }
        return addrRet;
    }

    /**
     * Count the known addresses per network class.
     *
     * The counter iterates the map of unique network addresses, so every entry
     * corresponds to exactly one known address. Only publicly routable
     * networks are reported (NET_IPV4, NET_IPV6 and NET_ONION); the sum of the
     * returned counts is therefore the total number of known addresses.
     */
    std::map<enum Network, int64_t> CountByNetwork() const
    {
        LOCK(cs);
        std::map<enum Network, int64_t> counts;
        // Always report every routable network, even when the count is zero.
        counts[NET_IPV4] = 0;
        counts[NET_IPV6] = 0;
        counts[NET_ONION] = 0;
        for (const auto& entry : mapAddr) {
            const enum Network net{entry.first.GetNetClass()};
            if (net == NET_UNROUTABLE || net == NET_INTERNAL) continue;
            counts[net]++;
        }
        return counts;
    }

    //! Return a bunch of addresses, selected at random.
    //! If `network` is set, only addresses of that network are returned.
    //! At most `max_addresses` addresses are returned and at most `max_pct`
    //! percent of the known addresses.
    std::vector<CAddress> GetAddr(std::optional<Network> network = std::nullopt, size_t max_addresses = ADDRMAN_GETADDR_MAX, size_t max_pct = ADDRMAN_GETADDR_MAX_PCT)
    {
        Check();
        std::vector<CAddress> vAddr;
        {
            LOCK(cs);
            GetAddr_(vAddr, network, max_addresses, max_pct);
        }
        Check();
        return vAddr;
    }

    //! Mark an entry as currently-connected-to.
    void Connected(const CService &addr, int64_t nTime = GetAdjustedTime())
    {
        LOCK(cs);
        Check();
        Connected_(addr, nTime);
        Check();
    }

    void SetServices(const CService &addr, ServiceFlags nServices)
    {
        LOCK(cs);
        Check();
        SetServices_(addr, nServices);
        Check();
    }

};

#endif // DEFI_ADDRMAN_H
