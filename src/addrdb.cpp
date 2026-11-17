// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <addrdb.h>

#include <addrman.h>
#include <chainparams.h>
#include <clientversion.h>
#include <fs.h>
#include <hash.h>
#include <netaddress.h>
#include <netbase.h>
#include <random.h>
#include <streams.h>
#include <tinyformat.h>
#include <univalue.h>
#include <util/system.h>

CBanEntry::CBanEntry(const UniValue& json)
    : nVersion(json["version"].getInt<int>()), nCreateTime(json["ban_created"].getInt<int64_t>()),
      nBanUntil(json["banned_until"].getInt<int64_t>())
{
    if (nVersion != CBanEntry::CURRENT_VERSION) {
        throw std::runtime_error(strprintf("Unsupported banlist entry version: %d", nVersion));
    }
    // The ban reason is a chain extension, upstream does not persist it. Missing
    // or unknown reasons degrade to BanReasonUnknown instead of failing the load.
    banReason = CBanEntry::banReasonFromString(json["ban_reason"].isStr() ? json["ban_reason"].get_str() : std::string());
}

UniValue CBanEntry::ToJson() const
{
    UniValue json(UniValue::VOBJ);
    json.pushKV("version", nVersion);
    json.pushKV("ban_created", nCreateTime);
    json.pushKV("banned_until", nBanUntil);
    json.pushKV("ban_reason", banReasonToString());
    return json;
}

namespace {

static const char* BANMAN_JSON_ADDR_KEY = "address";

/**
 * Convert a `banmap_t` object to a JSON array.
 * @param[in] bans Bans list to convert.
 * @return a JSON array, similar to the one returned by the `listbanned` RPC. Suitable for
 * passing to `BanMapFromJson()`.
 */
UniValue BanMapToJson(const banmap_t& bans)
{
    UniValue bans_json(UniValue::VARR);
    for (const auto& it : bans) {
        const auto& address = it.first;
        const auto& ban_entry = it.second;
        UniValue j = ban_entry.ToJson();
        j.pushKV(BANMAN_JSON_ADDR_KEY, address.ToString());
        bans_json.push_back(j);
    }
    return bans_json;
}

/**
 * Convert a JSON array to a `banmap_t` object.
 *
 * Invalid entries are skipped individually (upstream #22362), so that a single
 * broken entry never discards the remaining valid bans.
 *
 * @param[in] bans_json JSON to convert, must be as returned by `BanMapToJson()`.
 * @param[out] bans Bans list to create from the JSON.
 * @param[out] skipped Number of entries that could not be parsed.
 */
void BanMapFromJson(const UniValue& bans_json, banmap_t& bans, size_t& skipped)
{
    skipped = 0;
    for (const auto& ban_entry_json : bans_json.getValues()) {
        try {
            if (!ban_entry_json.isObject()) {
                throw std::runtime_error("Banlist entry is not an object");
            }
            CSubNet subnet;
            const std::string subnet_str = ban_entry_json[BANMAN_JSON_ADDR_KEY].get_str();
            // LookupSubNet() never resolves hostnames, it only parses numeric
            // addresses and special (onion) addresses.
            if (!LookupSubNet(subnet_str.c_str(), subnet)) {
                throw std::runtime_error(strprintf("Cannot parse banned address or subnet: %s", subnet_str));
            }
            bans.insert_or_assign(subnet, CBanEntry{ban_entry_json});
        } catch (const std::exception& e) {
            ++skipped;
            LogPrintf("Banlist: skipping invalid entry: %s\n", e.what());
        }
    }
}

/**
 * Write a JSON document to disk, replacing the target file atomically.
 *
 * The content is written to a temporary file first and renamed into place, so
 * that an interrupted write can never leave a truncated file behind.
 */
bool WriteJsonFile(const fs::path& path, const UniValue& value)
{
    const std::string contents = value.write(2) + "\n";
    const fs::path pathTmp = fs::PathFromString(fs::PathToString(path) + ".tmp");

    FILE* file = fsbridge::fopen(pathTmp, "wb");
    if (file == nullptr) {
        return error("%s: Failed to open file %s", __func__, fs::PathToString(pathTmp));
    }
    const size_t written = fwrite(contents.data(), 1, contents.size(), file);
    if (written != contents.size() || fflush(file) != 0) {
        fclose(file);
        fs::remove(pathTmp);
        return error("%s: Failed to write file %s", __func__, fs::PathToString(pathTmp));
    }
    fclose(file);

    if (!RenameOver(pathTmp, path)) {
        fs::remove(pathTmp);
        return error("%s: Rename-into-place failed", __func__);
    }
    return true;
}

/**
 * Read a whole file into a string.
 */
bool ReadFileToString(const fs::path& path, std::string& out)
{
    FILE* file = fsbridge::fopen(path, "rb");
    if (file == nullptr) {
        return error("%s: Failed to open file %s", __func__, fs::PathToString(path));
    }
    char buffer[4096];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        out.append(buffer, n);
    }
    const bool failed = ferror(file) != 0;
    fclose(file);
    if (failed) {
        return error("%s: Failed to read file %s", __func__, fs::PathToString(path));
    }
    return true;
}

template <typename Stream, typename Data>
bool SerializeDB(Stream& stream, const Data& data)
{
    // Write and commit header, data
    try {
        CHashWriter hasher(SER_DISK, CLIENT_VERSION);
        stream << Params().MessageStart() << data;
        hasher << Params().MessageStart() << data;
        stream << hasher.GetHash();
    } catch (const std::exception& e) {
        return error("%s: Serialize or I/O error - %s", __func__, e.what());
    }

    return true;
}

template <typename Data>
bool SerializeFileDB(const std::string& prefix, const fs::path& path, const Data& data)
{
    // Generate random temporary filename
    unsigned short randv = 0;
    GetRandBytes((unsigned char*)&randv, sizeof(randv));
    std::string tmpfn = strprintf("%s.%04x", prefix, randv);

    // open temp output file, and associate with CAutoFile
    fs::path pathTmp = GetDataDir() / tmpfn;
    FILE *file = fsbridge::fopen(pathTmp, "wb");
    CAutoFile fileout(file, SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull()) {
        fileout.fclose();
        remove(pathTmp);
        return error("%s: Failed to open file %s", __func__, fs::PathToString(pathTmp));
    }

    // Serialize
    if (!SerializeDB(fileout, data)) {
        fileout.fclose();
        remove(pathTmp);
        return false;
    }
    if (!FileCommit(fileout.Get())) {
        fileout.fclose();
        remove(pathTmp);
        return error("%s: Failed to flush file %s", __func__, fs::PathToString(pathTmp));
    }
    fileout.fclose();

    // replace existing file, if any, with new file
    if (!RenameOver(pathTmp, path)) {
        remove(pathTmp);
        return error("%s: Rename-into-place failed", __func__);
    }

    return true;
}

template <typename Stream, typename Data>
bool DeserializeDB(Stream& stream, Data& data, bool fCheckSum = true)
{
    try {
        CHashVerifier<Stream> verifier(&stream);
        // de-serialize file header (network specific magic number) and ..
        unsigned char pchMsgTmp[4];
        verifier >> pchMsgTmp;
        // ... verify the network matches ours
        if (memcmp(pchMsgTmp, Params().MessageStart(), sizeof(pchMsgTmp)))
            return error("%s: Invalid network magic number", __func__);

        // de-serialize data
        verifier >> data;

        // verify checksum
        if (fCheckSum) {
            uint256 hashTmp;
            stream >> hashTmp;
            if (hashTmp != verifier.GetHash()) {
                return error("%s: Checksum mismatch, data corrupted", __func__);
            }
        }
    }
    catch (const std::exception& e) {
        return error("%s: Deserialize or I/O error - %s", __func__, e.what());
    }

    return true;
}

template <typename Data>
bool DeserializeFileDB(const fs::path& path, Data& data)
{
    // open input file, and associate with CAutoFile
    FILE *file = fsbridge::fopen(path, "rb");
    CAutoFile filein(file, SER_DISK, CLIENT_VERSION);
    if (filein.IsNull())
        return error("%s: Failed to open file %s", __func__, fs::PathToString(path));

    return DeserializeDB(filein, data);
}

}

CBanDB::CBanDB(fs::path ban_list_path)
    : m_banlist_dat(fs::PathFromString(fs::PathToString(ban_list_path) + ".dat")),
      m_banlist_json(fs::PathFromString(fs::PathToString(ban_list_path) + ".json"))
{
}

bool CBanDB::Write(const banmap_t& banSet)
{
    UniValue root(UniValue::VOBJ);
    root.pushKV(JSON_KEY, BanMapToJson(banSet));
    return WriteJsonFile(m_banlist_json, root);
}

bool CBanDB::Read(banmap_t& banSet, bool& dirty)
{
    if (fs::exists(m_banlist_json)) {
        std::string contents;
        if (!ReadFileToString(m_banlist_json, contents)) {
            banSet.clear();
            dirty = true;
            return false;
        }
        UniValue root;
        if (!root.read(contents) || !root.isObject() || !root[JSON_KEY].isArray()) {
            LogPrintf("Banlist: cannot parse %s, recreating the banlist\n", fs::PathToString(m_banlist_json));
            banSet.clear();
            dirty = true;
            return false;
        }
        size_t skipped = 0;
        BanMapFromJson(root[JSON_KEY], banSet, skipped);
        // Rewrite the file if single entries had to be dropped, so that the
        // invalid entries do not survive the next restart.
        dirty = skipped > 0;
        return true;
    }

    // No JSON banlist (yet): fall back to the legacy binary banlist.dat once.
    // The legacy file is only ever read, never written and never removed.
    if (fs::exists(m_banlist_dat)) {
        LogPrintf("Banlist: %s is deprecated, migrating the banlist to %s\n",
            fs::PathToString(m_banlist_dat), fs::PathToString(m_banlist_json));
        banmap_t legacy;
        if (!DeserializeFileDB(m_banlist_dat, legacy)) {
            LogPrintf("Banlist: cannot read %s, recreating the banlist\n", fs::PathToString(m_banlist_dat));
            banSet.clear();
            dirty = true;
            return false;
        }
        banSet = std::move(legacy);
        // The migrated bans have to be written to the JSON file.
        dirty = true;
        return true;
    }

    // No banlist at all.
    banSet.clear();
    dirty = true;
    return false;
}

CAddrDB::CAddrDB()
{
    pathAddr = GetDataDir() / "peers.dat";
}

bool CAddrDB::Write(const CAddrMan& addr)
{
    return SerializeFileDB("peers", pathAddr, addr);
}

bool CAddrDB::Read(CAddrMan& addr)
{
    return DeserializeFileDB(pathAddr, addr);
}

bool CAddrDB::Read(CAddrMan& addr, CDataStream& ssPeers)
{
    bool ret = DeserializeDB(ssPeers, addr, false);
    if (!ret) {
        // Ensure addrman is left in a clean state
        addr.Clear();
    }
    return ret;
}
