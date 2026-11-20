// Copyright (c) 2012-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <netbase.h>
#include <net_permissions.h>
#include <streams.h>
#include <test/setup_common.h>
#include <util/strencodings.h>

#include <algorithm>
#include <string>
#include <vector>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(netbase_tests, BasicTestingSetup)

static CNetAddr ResolveIP(const char* ip)
{
    CNetAddr addr;
    LookupHost(ip, addr, false);
    return addr;
}

static CSubNet ResolveSubNet(const char* subnet)
{
    CSubNet ret;
    LookupSubNet(subnet, ret);
    return ret;
}

static CNetAddr CreateInternal(const char* host)
{
    CNetAddr addr;
    addr.SetInternal(host);
    return addr;
}

BOOST_AUTO_TEST_CASE(netbase_networks)
{
    BOOST_CHECK(ResolveIP("127.0.0.1").GetNetwork()                              == NET_UNROUTABLE);
    BOOST_CHECK(ResolveIP("::1").GetNetwork()                                    == NET_UNROUTABLE);
    BOOST_CHECK(ResolveIP("8.8.8.8").GetNetwork()                                == NET_IPV4);
    BOOST_CHECK(ResolveIP("2001::8888").GetNetwork()                             == NET_IPV6);
    BOOST_CHECK(ResolveIP("FD87:D87E:EB43:edb1:8e4:3588:e546:35ca").GetNetwork() == NET_ONION);
    BOOST_CHECK(CreateInternal("foo.com").GetNetwork()                           == NET_INTERNAL);

}

BOOST_AUTO_TEST_CASE(netbase_properties)
{

    BOOST_CHECK(ResolveIP("127.0.0.1").IsIPv4());
    BOOST_CHECK(ResolveIP("::FFFF:192.168.1.1").IsIPv4());
    BOOST_CHECK(ResolveIP("::1").IsIPv6());
    BOOST_CHECK(ResolveIP("10.0.0.1").IsRFC1918());
    BOOST_CHECK(ResolveIP("192.168.1.1").IsRFC1918());
    BOOST_CHECK(ResolveIP("172.31.255.255").IsRFC1918());
    BOOST_CHECK(ResolveIP("2001:0DB8::").IsRFC3849());
    BOOST_CHECK(ResolveIP("169.254.1.1").IsRFC3927());
    BOOST_CHECK(ResolveIP("2002::1").IsRFC3964());
    BOOST_CHECK(ResolveIP("FC00::").IsRFC4193());
    BOOST_CHECK(ResolveIP("2001::2").IsRFC4380());
    BOOST_CHECK(ResolveIP("2001:10::").IsRFC4843());
    BOOST_CHECK(ResolveIP("2001:20::").IsRFC7343());
    BOOST_CHECK(ResolveIP("FE80::").IsRFC4862());
    BOOST_CHECK(ResolveIP("64:FF9B::").IsRFC6052());
    BOOST_CHECK(ResolveIP("FD87:D87E:EB43:edb1:8e4:3588:e546:35ca").IsTor());
    BOOST_CHECK(ResolveIP("127.0.0.1").IsLocal());
    BOOST_CHECK(ResolveIP("::1").IsLocal());
    BOOST_CHECK(ResolveIP("8.8.8.8").IsRoutable());
    BOOST_CHECK(ResolveIP("2001::1").IsRoutable());
    BOOST_CHECK(ResolveIP("127.0.0.1").IsValid());
    BOOST_CHECK(CreateInternal("FD6B:88C0:8724:edb1:8e4:3588:e546:35ca").IsInternal());
    BOOST_CHECK(CreateInternal("bar.com").IsInternal());

}

bool static TestSplitHost(std::string test, std::string host, int port)
{
    std::string hostOut;
    int portOut = -1;
    SplitHostPort(test, portOut, hostOut);
    return hostOut == host && port == portOut;
}

BOOST_AUTO_TEST_CASE(netbase_splithost)
{
    BOOST_CHECK(TestSplitHost("defichain.io", "defichain.io", -1));
    BOOST_CHECK(TestSplitHost("[defichain.io]", "defichain.io", -1));
    BOOST_CHECK(TestSplitHost("defichain.io:80", "defichain.io", 80));
    BOOST_CHECK(TestSplitHost("[defichain.io]:80", "defichain.io", 80));
    BOOST_CHECK(TestSplitHost("127.0.0.1", "127.0.0.1", -1));
    BOOST_CHECK(TestSplitHost("127.0.0.1:8555", "127.0.0.1", 8555));
    BOOST_CHECK(TestSplitHost("[127.0.0.1]", "127.0.0.1", -1));
    BOOST_CHECK(TestSplitHost("[127.0.0.1]:8555", "127.0.0.1", 8555));
    BOOST_CHECK(TestSplitHost("::ffff:127.0.0.1", "::ffff:127.0.0.1", -1));
    BOOST_CHECK(TestSplitHost("[::ffff:127.0.0.1]:8555", "::ffff:127.0.0.1", 8555));
    BOOST_CHECK(TestSplitHost("[::]:8555", "::", 8555));
    BOOST_CHECK(TestSplitHost("::8555", "::8555", -1));
    BOOST_CHECK(TestSplitHost(":8555", "", 8555));
    BOOST_CHECK(TestSplitHost("[]:8555", "", 8555));
    BOOST_CHECK(TestSplitHost("", "", -1));
}

bool static TestParse(std::string src, std::string canon)
{
    CService addr(LookupNumeric(src.c_str(), 65535));
    return canon == addr.ToString();
}

BOOST_AUTO_TEST_CASE(netbase_lookupnumeric)
{
    BOOST_CHECK(TestParse("127.0.0.1", "127.0.0.1:65535"));
    BOOST_CHECK(TestParse("127.0.0.1:8555", "127.0.0.1:8555"));
    BOOST_CHECK(TestParse("::ffff:127.0.0.1", "127.0.0.1:65535"));
    BOOST_CHECK(TestParse("::", "[::]:65535"));
    BOOST_CHECK(TestParse("[::]:8555", "[::]:8555"));
    BOOST_CHECK(TestParse("[127.0.0.1]", "127.0.0.1:65535"));
    BOOST_CHECK(TestParse(":::", "[::]:0"));

    // verify that an internal address fails to resolve
    BOOST_CHECK(TestParse("[fd6b:88c0:8724:1:2:3:4:5]", "[::]:0"));
    // and that a one-off resolves correctly
    BOOST_CHECK(TestParse("[fd6c:88c0:8724:1:2:3:4:5]", "[fd6c:88c0:8724:1:2:3:4:5]:65535"));
}

BOOST_AUTO_TEST_CASE(onioncat_test)
{

    // values from https://web.archive.org/web/20121122003543/http://www.cypherpunk.at/onioncat/wiki/OnionCat
    CNetAddr addr1(ResolveIP("5wyqrzbvrdsumnok.onion"));
    CNetAddr addr2(ResolveIP("FD87:D87E:EB43:edb1:8e4:3588:e546:35ca"));
    BOOST_CHECK(addr1 == addr2);
    BOOST_CHECK(addr1.IsTor());
    BOOST_CHECK(addr1.ToStringIP() == "5wyqrzbvrdsumnok.onion");
    BOOST_CHECK(addr1.IsRoutable());

}

BOOST_AUTO_TEST_CASE(netbase_torv3_onion)
{
    // Test vector: the Tor v3 onion address of the ed25519 public key
    // 0x010203...20.
    const std::string onion = "aebagbafaydqqcikbmga2dqpcaireeyuculbogazdinryhi6d4qcmeqd.onion";
    CNetAddr addr(ResolveIP(onion.c_str()));

    BOOST_CHECK(addr.IsTor());
    BOOST_CHECK(addr.IsOnion());
    BOOST_CHECK(addr.IsTorV3());
    BOOST_CHECK(!addr.IsIPv4());
    BOOST_CHECK(!addr.IsIPv6());
    BOOST_CHECK(!addr.IsI2P());
    BOOST_CHECK(!addr.IsInternal());
    BOOST_CHECK(addr.IsValid());
    BOOST_CHECK(addr.IsRoutable());
    BOOST_CHECK_EQUAL(addr.GetNetwork(), NET_ONION);
    BOOST_CHECK_EQUAL(addr.GetNetClass(), NET_ONION);
    BOOST_CHECK_EQUAL(addr.ToStringIP(), onion);
    BOOST_CHECK(!addr.IsAddrV1Compatible());

    // The raw address is the 32 byte ed25519 public key.
    const std::vector<unsigned char> bytes = addr.GetAddrBytes();
    BOOST_CHECK_EQUAL(bytes.size(), 32U);
    for (size_t i = 0; i < bytes.size(); ++i) {
        BOOST_CHECK_EQUAL(bytes[i], i + 1);
    }

    // Tor v3 addresses are grouped by the first four bits of the address.
    BOOST_CHECK(addr.GetGroup() == std::vector<unsigned char>({(unsigned char)NET_ONION, (unsigned char)(0x01 | 0x0f)}));

    // The service is printed without brackets, like every other onion address.
    CService service(addr, 8555);
    BOOST_CHECK_EQUAL(service.ToString(), onion + ":8555");
    BOOST_CHECK_EQUAL(service.GetPort(), 8555);

    // The address is case insensitive.
    std::string upper = onion;
    for (size_t i = 0; i + 6 < upper.size(); ++i) {
        if (upper[i] >= 'a' && upper[i] <= 'z') {
            upper[i] = upper[i] - 'a' + 'A';
        }
    }
    CNetAddr upper_addr(ResolveIP(upper.c_str()));
    BOOST_CHECK(upper_addr.IsTorV3());
    BOOST_CHECK(upper_addr == addr);
    BOOST_CHECK_EQUAL(upper_addr.ToStringIP(), onion);
}

BOOST_AUTO_TEST_CASE(netbase_torv3_onion_invalid)
{
    // A modified checksum must be rejected.
    BOOST_CHECK(!ResolveIP("aebagbafayaqqcikbmga2dqpcaireeyuculbogazdinryhi6d4qcmeqd.onion").IsTorV3());
    // A wrong version byte must be rejected (the checksum is computed over the
    // version byte as well).
    BOOST_CHECK(!ResolveIP("aebagbafaydqqcikbmga2dqpcaireeyuculbogazdinryhi6d4qipzqc.onion").IsTorV3());
    BOOST_CHECK(!ResolveIP("aebagbafaydqqcikbmga2dqpcaireeyuculbogazdinryhi6d4qhwlqe.onion").IsTorV3());
    // A wrong length must be rejected.
    BOOST_CHECK(!ResolveIP("aebagbafaydqqcikbmga2dqpcaireeyuculbogazdinryhi6d4qcmeq.onion").IsTorV3());
    BOOST_CHECK(!ResolveIP("aebagbafaydqqcikbmga2dqpcaireeyuculbogazdinryhi6d4qcmeqdd.onion").IsTorV3());
    // Invalid base32 characters must be rejected.
    BOOST_CHECK(!ResolveIP("aebagbafaydqqcikbmga2dqpcaireeyuculbogazdinryhi6d4qcmeq1.onion").IsTorV3());
    // The legacy Tor v2 (OnionCat) address is still accepted.
    BOOST_CHECK(ResolveIP("5wyqrzbvrdsumnok.onion").IsTor());
    BOOST_CHECK(!ResolveIP("5wyqrzbvrdsumnok.onion").IsTorV3());
}

BOOST_AUTO_TEST_CASE(netbase_i2p)
{
    // Test vector: the I2P destination of the bytes 0x00..0x1f.
    const std::string i2p = "aaaqeayeaudaocajbifqydiob4ibceqtcqkrmfyydenbwha5dypq.b32.i2p";
    CNetAddr addr(ResolveIP(i2p.c_str()));

    BOOST_CHECK(addr.IsI2P());
    BOOST_CHECK(!addr.IsTor());
    BOOST_CHECK(!addr.IsOnion());
    BOOST_CHECK(!addr.IsTorV3());
    BOOST_CHECK(!addr.IsIPv4());
    BOOST_CHECK(!addr.IsIPv6());
    BOOST_CHECK(addr.IsValid());
    BOOST_CHECK(addr.IsRoutable());
    BOOST_CHECK_EQUAL(addr.GetNetwork(), NET_I2P);
    BOOST_CHECK_EQUAL(addr.GetNetClass(), NET_I2P);
    BOOST_CHECK_EQUAL(addr.ToStringIP(), i2p);
    BOOST_CHECK(!addr.IsAddrV1Compatible());
    BOOST_CHECK_EQUAL(addr.GetAddrBytes().size(), 32U);

    // I2P addresses are grouped by the first four bits of the address.
    BOOST_CHECK(addr.GetGroup() == std::vector<unsigned char>({(unsigned char)NET_I2P, (unsigned char)(0x00 | 0x0f)}));

    // A wrong length or an invalid checksum must be rejected.
    BOOST_CHECK(!ResolveIP("aaaqeayeaudaocajbifqydiob4ibceqtcqkrmfyydenbwha5dyp.b32.i2p").IsI2P());
    BOOST_CHECK(!ResolveIP("aaaqeayeaudaocajbifqydiob4ibceqtcqkrmfyydenbwha5dypqq.b32.i2p").IsI2P());
}

BOOST_AUTO_TEST_CASE(netbase_parsenetwork_i2p)
{
    BOOST_CHECK_EQUAL(ParseNetwork("i2p"), NET_I2P);
    BOOST_CHECK_EQUAL(ParseNetwork("I2P"), NET_I2P);
    BOOST_CHECK_EQUAL(GetNetworkName(NET_I2P), "i2p");
    BOOST_CHECK_EQUAL(GetNetworkName(NET_ONION), "onion");
}

BOOST_AUTO_TEST_CASE(netbase_v1_serialization)
{
    // The legacy (pre-BIP155) serialization and the addrman keys of the
    // networks with a v1 representation must stay byte-identical.
    CDataStream stream(SER_NETWORK, PROTOCOL_VERSION);
    stream << CService(ResolveIP("1.2.3.4"), 1234);
    BOOST_CHECK_EQUAL(HexStr(std::vector<unsigned char>(stream.begin(), stream.end())), "00000000000000000000ffff0102030404d2");

    CDataStream stream6(SER_NETWORK, PROTOCOL_VERSION);
    stream6 << CService(ResolveIP("2001:db8::1"), 1234);
    BOOST_CHECK_EQUAL(HexStr(std::vector<unsigned char>(stream6.begin(), stream6.end())), "20010db800000000000000000000000104d2");

    CDataStream stream_onion(SER_NETWORK, PROTOCOL_VERSION);
    stream_onion << CService(ResolveIP("5wyqrzbvrdsumnok.onion"), 1234);
    BOOST_CHECK_EQUAL(HexStr(std::vector<unsigned char>(stream_onion.begin(), stream_onion.end())), "fd87d87eeb43edb108e43588e54635ca04d2");

    // The addrman key of an IPv4 address is unchanged as well.
    const std::vector<unsigned char> key = CService(ResolveIP("1.2.3.4"), 1234).GetKey();
    BOOST_CHECK_EQUAL(key.size(), 18U);
    BOOST_CHECK_EQUAL(HexStr(key), "00000000000000000000ffff0102030404d2");
}

BOOST_AUTO_TEST_CASE(netbase_onion_subnet)
{
    // Tor v3 onion addresses have no legacy (pre-BIP155) representation and
    // are therefore not supported by the subnet model; they must be rejected
    // explicitly instead of silently matching every onion address.
    const std::string onion = "aebagbafaydqqcikbmga2dqpcaireeyuculbogazdinryhi6d4qcmeqd.onion";
    CSubNet subnet;
    BOOST_CHECK(!LookupSubNet(onion.c_str(), subnet));
    BOOST_CHECK(!subnet.IsValid());
    BOOST_CHECK(!CSubNet(ResolveIP(onion.c_str())).IsValid());
    BOOST_CHECK(!CSubNet(ResolveIP(onion.c_str())).Match(ResolveIP(onion.c_str())));

    // The same holds for I2P addresses.
    const std::string i2p = "aaaqeayeaudaocajbifqydiob4ibceqtcqkrmfyydenbwha5dypq.b32.i2p";
    CSubNet i2p_subnet;
    BOOST_CHECK(!LookupSubNet(i2p.c_str(), i2p_subnet));
    BOOST_CHECK(!CSubNet(ResolveIP(i2p.c_str())).IsValid());
}

BOOST_AUTO_TEST_CASE(subnet_test)
{

    BOOST_CHECK(ResolveSubNet("1.2.3.0/24") == ResolveSubNet("1.2.3.0/255.255.255.0"));
    BOOST_CHECK(ResolveSubNet("1.2.3.0/24") != ResolveSubNet("1.2.4.0/255.255.255.0"));
    BOOST_CHECK(ResolveSubNet("1.2.3.0/24").Match(ResolveIP("1.2.3.4")));
    BOOST_CHECK(!ResolveSubNet("1.2.2.0/24").Match(ResolveIP("1.2.3.4")));
    BOOST_CHECK(ResolveSubNet("1.2.3.4").Match(ResolveIP("1.2.3.4")));
    BOOST_CHECK(ResolveSubNet("1.2.3.4/32").Match(ResolveIP("1.2.3.4")));
    BOOST_CHECK(!ResolveSubNet("1.2.3.4").Match(ResolveIP("5.6.7.8")));
    BOOST_CHECK(!ResolveSubNet("1.2.3.4/32").Match(ResolveIP("5.6.7.8")));
    BOOST_CHECK(ResolveSubNet("::ffff:127.0.0.1").Match(ResolveIP("127.0.0.1")));
    BOOST_CHECK(ResolveSubNet("1:2:3:4:5:6:7:8").Match(ResolveIP("1:2:3:4:5:6:7:8")));
    BOOST_CHECK(!ResolveSubNet("1:2:3:4:5:6:7:8").Match(ResolveIP("1:2:3:4:5:6:7:9")));
    BOOST_CHECK(ResolveSubNet("1:2:3:4:5:6:7:0/112").Match(ResolveIP("1:2:3:4:5:6:7:1234")));
    BOOST_CHECK(ResolveSubNet("192.168.0.1/24").Match(ResolveIP("192.168.0.2")));
    BOOST_CHECK(ResolveSubNet("192.168.0.20/29").Match(ResolveIP("192.168.0.18")));
    BOOST_CHECK(ResolveSubNet("1.2.2.1/24").Match(ResolveIP("1.2.2.4")));
    BOOST_CHECK(ResolveSubNet("1.2.2.110/31").Match(ResolveIP("1.2.2.111")));
    BOOST_CHECK(ResolveSubNet("1.2.2.20/26").Match(ResolveIP("1.2.2.63")));
    // All-Matching IPv6 Matches arbitrary IPv4 and IPv6
    BOOST_CHECK(ResolveSubNet("::/0").Match(ResolveIP("1:2:3:4:5:6:7:1234")));
    BOOST_CHECK(ResolveSubNet("::/0").Match(ResolveIP("1.2.3.4")));
    // All-Matching IPv4 does not Match IPv6
    BOOST_CHECK(!ResolveSubNet("0.0.0.0/0").Match(ResolveIP("1:2:3:4:5:6:7:1234")));
    // Invalid subnets Match nothing (not even invalid addresses)
    BOOST_CHECK(!CSubNet().Match(ResolveIP("1.2.3.4")));
    BOOST_CHECK(!ResolveSubNet("").Match(ResolveIP("4.5.6.7")));
    BOOST_CHECK(!ResolveSubNet("bloop").Match(ResolveIP("0.0.0.0")));
    BOOST_CHECK(!ResolveSubNet("bloop").Match(ResolveIP("hab")));
    // Check valid/invalid
    BOOST_CHECK(ResolveSubNet("1.2.3.0/0").IsValid());
    BOOST_CHECK(!ResolveSubNet("1.2.3.0/-1").IsValid());
    BOOST_CHECK(ResolveSubNet("1.2.3.0/32").IsValid());
    BOOST_CHECK(!ResolveSubNet("1.2.3.0/33").IsValid());
    BOOST_CHECK(ResolveSubNet("1:2:3:4:5:6:7:8/0").IsValid());
    BOOST_CHECK(ResolveSubNet("1:2:3:4:5:6:7:8/33").IsValid());
    BOOST_CHECK(!ResolveSubNet("1:2:3:4:5:6:7:8/-1").IsValid());
    BOOST_CHECK(ResolveSubNet("1:2:3:4:5:6:7:8/128").IsValid());
    BOOST_CHECK(!ResolveSubNet("1:2:3:4:5:6:7:8/129").IsValid());
    BOOST_CHECK(!ResolveSubNet("fuzzy").IsValid());

    //CNetAddr constructor test
    BOOST_CHECK(CSubNet(ResolveIP("127.0.0.1")).IsValid());
    BOOST_CHECK(CSubNet(ResolveIP("127.0.0.1")).Match(ResolveIP("127.0.0.1")));
    BOOST_CHECK(!CSubNet(ResolveIP("127.0.0.1")).Match(ResolveIP("127.0.0.2")));
    BOOST_CHECK(CSubNet(ResolveIP("127.0.0.1")).ToString() == "127.0.0.1/32");

    CSubNet subnet = CSubNet(ResolveIP("1.2.3.4"), 32);
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.2.3.4/32");
    subnet = CSubNet(ResolveIP("1.2.3.4"), 8);
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.0.0.0/8");
    subnet = CSubNet(ResolveIP("1.2.3.4"), 0);
    BOOST_CHECK_EQUAL(subnet.ToString(), "0.0.0.0/0");

    subnet = CSubNet(ResolveIP("1.2.3.4"), ResolveIP("255.255.255.255"));
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.2.3.4/32");
    subnet = CSubNet(ResolveIP("1.2.3.4"), ResolveIP("255.0.0.0"));
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.0.0.0/8");
    subnet = CSubNet(ResolveIP("1.2.3.4"), ResolveIP("0.0.0.0"));
    BOOST_CHECK_EQUAL(subnet.ToString(), "0.0.0.0/0");

    BOOST_CHECK(CSubNet(ResolveIP("1:2:3:4:5:6:7:8")).IsValid());
    BOOST_CHECK(CSubNet(ResolveIP("1:2:3:4:5:6:7:8")).Match(ResolveIP("1:2:3:4:5:6:7:8")));
    BOOST_CHECK(!CSubNet(ResolveIP("1:2:3:4:5:6:7:8")).Match(ResolveIP("1:2:3:4:5:6:7:9")));
    BOOST_CHECK(CSubNet(ResolveIP("1:2:3:4:5:6:7:8")).ToString() == "1:2:3:4:5:6:7:8/128");

    subnet = ResolveSubNet("1.2.3.4/255.255.255.255");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.2.3.4/32");
    subnet = ResolveSubNet("1.2.3.4/255.255.255.254");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.2.3.4/31");
    subnet = ResolveSubNet("1.2.3.4/255.255.255.252");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.2.3.4/30");
    subnet = ResolveSubNet("1.2.3.4/255.255.255.248");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.2.3.0/29");
    subnet = ResolveSubNet("1.2.3.4/255.255.255.240");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.2.3.0/28");
    subnet = ResolveSubNet("1.2.3.4/255.255.255.224");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.2.3.0/27");
    subnet = ResolveSubNet("1.2.3.4/255.255.255.192");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.2.3.0/26");
    subnet = ResolveSubNet("1.2.3.4/255.255.255.128");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.2.3.0/25");
    subnet = ResolveSubNet("1.2.3.4/255.255.255.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.2.3.0/24");
    subnet = ResolveSubNet("1.2.3.4/255.255.254.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.2.2.0/23");
    subnet = ResolveSubNet("1.2.3.4/255.255.252.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.2.0.0/22");
    subnet = ResolveSubNet("1.2.3.4/255.255.248.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.2.0.0/21");
    subnet = ResolveSubNet("1.2.3.4/255.255.240.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.2.0.0/20");
    subnet = ResolveSubNet("1.2.3.4/255.255.224.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.2.0.0/19");
    subnet = ResolveSubNet("1.2.3.4/255.255.192.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.2.0.0/18");
    subnet = ResolveSubNet("1.2.3.4/255.255.128.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.2.0.0/17");
    subnet = ResolveSubNet("1.2.3.4/255.255.0.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.2.0.0/16");
    subnet = ResolveSubNet("1.2.3.4/255.254.0.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.2.0.0/15");
    subnet = ResolveSubNet("1.2.3.4/255.252.0.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.0.0.0/14");
    subnet = ResolveSubNet("1.2.3.4/255.248.0.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.0.0.0/13");
    subnet = ResolveSubNet("1.2.3.4/255.240.0.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.0.0.0/12");
    subnet = ResolveSubNet("1.2.3.4/255.224.0.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.0.0.0/11");
    subnet = ResolveSubNet("1.2.3.4/255.192.0.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.0.0.0/10");
    subnet = ResolveSubNet("1.2.3.4/255.128.0.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.0.0.0/9");
    subnet = ResolveSubNet("1.2.3.4/255.0.0.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.0.0.0/8");
    subnet = ResolveSubNet("1.2.3.4/254.0.0.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "0.0.0.0/7");
    subnet = ResolveSubNet("1.2.3.4/252.0.0.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "0.0.0.0/6");
    subnet = ResolveSubNet("1.2.3.4/248.0.0.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "0.0.0.0/5");
    subnet = ResolveSubNet("1.2.3.4/240.0.0.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "0.0.0.0/4");
    subnet = ResolveSubNet("1.2.3.4/224.0.0.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "0.0.0.0/3");
    subnet = ResolveSubNet("1.2.3.4/192.0.0.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "0.0.0.0/2");
    subnet = ResolveSubNet("1.2.3.4/128.0.0.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "0.0.0.0/1");
    subnet = ResolveSubNet("1.2.3.4/0.0.0.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "0.0.0.0/0");

    subnet = ResolveSubNet("1:2:3:4:5:6:7:8/ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1:2:3:4:5:6:7:8/128");
    subnet = ResolveSubNet("1:2:3:4:5:6:7:8/ffff:0000:0000:0000:0000:0000:0000:0000");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1::/16");
    subnet = ResolveSubNet("1:2:3:4:5:6:7:8/0000:0000:0000:0000:0000:0000:0000:0000");
    BOOST_CHECK_EQUAL(subnet.ToString(), "::/0");
    subnet = ResolveSubNet("1.2.3.4/255.255.232.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.2.0.0/255.255.232.0");
    subnet = ResolveSubNet("1:2:3:4:5:6:7:8/ffff:ffff:ffff:fffe:ffff:ffff:ffff:ff0f");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1:2:3:4:5:6:7:8/ffff:ffff:ffff:fffe:ffff:ffff:ffff:ff0f");

}

BOOST_AUTO_TEST_CASE(netbase_getgroup)
{

    BOOST_CHECK(ResolveIP("127.0.0.1").GetGroup() == std::vector<unsigned char>({0})); // Local -> !Routable()
    BOOST_CHECK(ResolveIP("257.0.0.1").GetGroup() == std::vector<unsigned char>({0})); // !Valid -> !Routable()
    BOOST_CHECK(ResolveIP("10.0.0.1").GetGroup() == std::vector<unsigned char>({0})); // RFC1918 -> !Routable()
    BOOST_CHECK(ResolveIP("169.254.1.1").GetGroup() == std::vector<unsigned char>({0})); // RFC3927 -> !Routable()
    BOOST_CHECK(ResolveIP("1.2.3.4").GetGroup() == std::vector<unsigned char>({(unsigned char)NET_IPV4, 1, 2})); // IPv4
    BOOST_CHECK(ResolveIP("::FFFF:0:102:304").GetGroup() == std::vector<unsigned char>({(unsigned char)NET_IPV4, 1, 2})); // RFC6145
    BOOST_CHECK(ResolveIP("64:FF9B::102:304").GetGroup() == std::vector<unsigned char>({(unsigned char)NET_IPV4, 1, 2})); // RFC6052
    BOOST_CHECK(ResolveIP("2002:102:304:9999:9999:9999:9999:9999").GetGroup() == std::vector<unsigned char>({(unsigned char)NET_IPV4, 1, 2})); // RFC3964
    BOOST_CHECK(ResolveIP("2001:0:9999:9999:9999:9999:FEFD:FCFB").GetGroup() == std::vector<unsigned char>({(unsigned char)NET_IPV4, 1, 2})); // RFC4380
    BOOST_CHECK(ResolveIP("FD87:D87E:EB43:edb1:8e4:3588:e546:35ca").GetGroup() == std::vector<unsigned char>({(unsigned char)NET_ONION, 239})); // Tor
    BOOST_CHECK(ResolveIP("2001:470:abcd:9999:9999:9999:9999:9999").GetGroup() == std::vector<unsigned char>({(unsigned char)NET_IPV6, 32, 1, 4, 112, 175})); //he.net
    BOOST_CHECK(ResolveIP("2001:2001:9999:9999:9999:9999:9999:9999").GetGroup() == std::vector<unsigned char>({(unsigned char)NET_IPV6, 32, 1, 32, 1})); //IPv6

    // baz.net sha256 hash: 12929400eb4607c4ac075f087167e75286b179c693eb059a01774b864e8fe505
    std::vector<unsigned char> internal_group = {NET_INTERNAL, 0x12, 0x92, 0x94, 0x00, 0xeb, 0x46, 0x07, 0xc4, 0xac, 0x07};
    BOOST_CHECK(CreateInternal("baz.net").GetGroup() == internal_group);
}

BOOST_AUTO_TEST_CASE(netbase_parsenetwork)
{
    BOOST_CHECK_EQUAL(ParseNetwork("ipv4"), NET_IPV4);
    BOOST_CHECK_EQUAL(ParseNetwork("ipv6"), NET_IPV6);
    BOOST_CHECK_EQUAL(ParseNetwork("onion"), NET_ONION);
    BOOST_CHECK_EQUAL(ParseNetwork("tor"), NET_ONION);

    BOOST_CHECK_EQUAL(ParseNetwork("IPv4"), NET_IPV4);
    BOOST_CHECK_EQUAL(ParseNetwork("IPv6"), NET_IPV6);
    BOOST_CHECK_EQUAL(ParseNetwork("ONION"), NET_ONION);
    BOOST_CHECK_EQUAL(ParseNetwork("TOR"), NET_ONION);

    BOOST_CHECK_EQUAL(ParseNetwork(":)"), NET_UNROUTABLE);
    BOOST_CHECK_EQUAL(ParseNetwork("tÖr"), NET_UNROUTABLE);
    BOOST_CHECK_EQUAL(ParseNetwork("\xfe\xff"), NET_UNROUTABLE);
    BOOST_CHECK_EQUAL(ParseNetwork(""), NET_UNROUTABLE);
}

BOOST_AUTO_TEST_CASE(netpermissions_test)
{
    std::string error;
    NetWhitebindPermissions whitebindPermissions;
    NetWhitelistPermissions whitelistPermissions;

    // Detect invalid white bind
    BOOST_CHECK(!NetWhitebindPermissions::TryParse("", whitebindPermissions, error));
    BOOST_CHECK(error.find("Cannot resolve -whitebind address") != std::string::npos);
    BOOST_CHECK(!NetWhitebindPermissions::TryParse("127.0.0.1", whitebindPermissions, error));
    BOOST_CHECK(error.find("Need to specify a port with -whitebind") != std::string::npos);
    BOOST_CHECK(!NetWhitebindPermissions::TryParse("", whitebindPermissions, error));

    // If no permission flags, assume backward compatibility
    BOOST_CHECK(NetWhitebindPermissions::TryParse("1.2.3.4:32", whitebindPermissions, error));
    BOOST_CHECK(error.empty());
    BOOST_CHECK_EQUAL(whitebindPermissions.m_flags, PF_ISIMPLICIT);
    BOOST_CHECK(NetPermissions::HasFlag(whitebindPermissions.m_flags, PF_ISIMPLICIT));
    NetPermissions::ClearFlag(whitebindPermissions.m_flags, PF_ISIMPLICIT);
    BOOST_CHECK(!NetPermissions::HasFlag(whitebindPermissions.m_flags, PF_ISIMPLICIT));
    BOOST_CHECK_EQUAL(whitebindPermissions.m_flags, PF_NONE);
    NetPermissions::AddFlag(whitebindPermissions.m_flags, PF_ISIMPLICIT);
    BOOST_CHECK(NetPermissions::HasFlag(whitebindPermissions.m_flags, PF_ISIMPLICIT));

    // Can set one permission
    BOOST_CHECK(NetWhitebindPermissions::TryParse("bloom@1.2.3.4:32", whitebindPermissions, error));
    BOOST_CHECK_EQUAL(whitebindPermissions.m_flags, PF_BLOOMFILTER);
    BOOST_CHECK(NetWhitebindPermissions::TryParse("@1.2.3.4:32", whitebindPermissions, error));
    BOOST_CHECK_EQUAL(whitebindPermissions.m_flags, PF_NONE);

    // Happy path, can parse flags
    BOOST_CHECK(NetWhitebindPermissions::TryParse("bloom,forcerelay@1.2.3.4:32", whitebindPermissions, error));
    // forcerelay should also activate the relay permission
    BOOST_CHECK_EQUAL(whitebindPermissions.m_flags, PF_BLOOMFILTER | PF_FORCERELAY | PF_RELAY);
    BOOST_CHECK(NetWhitebindPermissions::TryParse("bloom,relay,noban@1.2.3.4:32", whitebindPermissions, error));
    BOOST_CHECK_EQUAL(whitebindPermissions.m_flags, PF_BLOOMFILTER | PF_RELAY | PF_NOBAN);
    BOOST_CHECK(NetWhitebindPermissions::TryParse("bloom,forcerelay,noban@1.2.3.4:32", whitebindPermissions, error));
    BOOST_CHECK(NetWhitebindPermissions::TryParse("all@1.2.3.4:32", whitebindPermissions, error));
    BOOST_CHECK_EQUAL(whitebindPermissions.m_flags, PF_ALL);

    // Allow dups
    BOOST_CHECK(NetWhitebindPermissions::TryParse("bloom,relay,noban,noban@1.2.3.4:32", whitebindPermissions, error));
    BOOST_CHECK_EQUAL(whitebindPermissions.m_flags, PF_BLOOMFILTER | PF_RELAY | PF_NOBAN);

    // Allow empty
    BOOST_CHECK(NetWhitebindPermissions::TryParse("bloom,relay,,noban@1.2.3.4:32", whitebindPermissions, error));
    BOOST_CHECK_EQUAL(whitebindPermissions.m_flags, PF_BLOOMFILTER | PF_RELAY | PF_NOBAN);
    BOOST_CHECK(NetWhitebindPermissions::TryParse(",@1.2.3.4:32", whitebindPermissions, error));
    BOOST_CHECK_EQUAL(whitebindPermissions.m_flags, PF_NONE);
    BOOST_CHECK(NetWhitebindPermissions::TryParse(",,@1.2.3.4:32", whitebindPermissions, error));
    BOOST_CHECK_EQUAL(whitebindPermissions.m_flags, PF_NONE);

    // Detect invalid flag
    BOOST_CHECK(!NetWhitebindPermissions::TryParse("bloom,forcerelay,oopsie@1.2.3.4:32", whitebindPermissions, error));
    BOOST_CHECK(error.find("Invalid P2P permission") != std::string::npos);

    // Check whitelist error
    BOOST_CHECK(!NetWhitelistPermissions::TryParse("bloom,forcerelay,noban@1.2.3.4:32", whitelistPermissions, error));
    BOOST_CHECK(error.find("Invalid netmask specified in -whitelist") != std::string::npos);

    // Happy path for whitelist parsing
    BOOST_CHECK(NetWhitelistPermissions::TryParse("noban@1.2.3.4", whitelistPermissions, error));
    BOOST_CHECK_EQUAL(whitelistPermissions.m_flags, PF_NOBAN);
    BOOST_CHECK(NetWhitelistPermissions::TryParse("bloom,forcerelay,noban,relay@1.2.3.4/32", whitelistPermissions, error));
    BOOST_CHECK_EQUAL(whitelistPermissions.m_flags, PF_BLOOMFILTER | PF_FORCERELAY | PF_NOBAN | PF_RELAY);
    BOOST_CHECK(error.empty());
    BOOST_CHECK_EQUAL(whitelistPermissions.m_subnet.ToString(), "1.2.3.4/32");
    BOOST_CHECK(NetWhitelistPermissions::TryParse("bloom,forcerelay,noban,relay,mempool@1.2.3.4/32", whitelistPermissions, error));

    const auto strings = NetPermissions::ToStrings(PF_ALL);
    BOOST_CHECK_EQUAL(strings.size(), 5);
    BOOST_CHECK(std::find(strings.begin(), strings.end(), "bloomfilter") != strings.end());
    BOOST_CHECK(std::find(strings.begin(), strings.end(), "forcerelay") != strings.end());
    BOOST_CHECK(std::find(strings.begin(), strings.end(), "relay") != strings.end());
    BOOST_CHECK(std::find(strings.begin(), strings.end(), "noban") != strings.end());
    BOOST_CHECK(std::find(strings.begin(), strings.end(), "mempool") != strings.end());
}

BOOST_AUTO_TEST_SUITE_END()
