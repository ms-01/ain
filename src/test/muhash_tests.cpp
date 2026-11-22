// Copyright (c) 2020 Pieter Wuille
// Copyright (c) 2017-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <crypto/muhash.h>
#include <streams.h>
#include <test/setup_common.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <string>
#include <vector>

namespace {

//! Build a 32 byte element, consisting of the given prefix byte followed by zeroes.
std::vector<unsigned char> Element(unsigned char prefix)
{
    std::vector<unsigned char> element(32, 0x00);
    element[0] = prefix;
    return element;
}

//! Insert a fixed size element into a MuHash3072 instance.
void Insert(MuHash3072& muhash, const std::vector<unsigned char>& element)
{
    muhash.Insert(Span<const unsigned char>(element.data(), element.size()));
}

//! Remove a fixed size element from a MuHash3072 instance.
void Remove(MuHash3072& muhash, const std::vector<unsigned char>& element)
{
    muhash.Remove(Span<const unsigned char>(element.data(), element.size()));
}

//! Finalize a MuHash3072 instance and return the hex representation of the digest.
std::string FinalizeHex(const MuHash3072& muhash)
{
    // Finalize() does not modify the object, but the interface is not const,
    // so work on a copy.
    MuHash3072 copy{muhash};
    uint256 out;
    copy.Finalize(out);
    return out.GetHex();
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(muhash_tests, BasicTestingSetup)

//! The digest of the empty set is well defined.
BOOST_AUTO_TEST_CASE(muhash_empty)
{
    MuHash3072 muhash;
    BOOST_CHECK_EQUAL(FinalizeHex(muhash), "dd5ad2a105c2d29495f577245c357409002329b9f4d6182c0af3dc2f462555c8");
}

//! Fixed test vector, taken from the upstream reference implementation
//! (Bitcoin Core's test/functional/test_framework/muhash.py).
BOOST_AUTO_TEST_CASE(muhash_test_vector)
{
    MuHash3072 muhash;
    Insert(muhash, Element(0x00));
    Insert(muhash, Element(0x01));
    Remove(muhash, Element(0x02));

    BOOST_CHECK_EQUAL(FinalizeHex(muhash), "10d312b100cbd32ada024a6646e40d3482fcff103668d2625f10002a607d5863");
}

//! Fixed test vector for a two element set.
BOOST_AUTO_TEST_CASE(muhash_test_vector_two_elements)
{
    MuHash3072 muhash;
    Insert(muhash, Element(0x00));
    Insert(muhash, Element(0x01));

    BOOST_CHECK_EQUAL(FinalizeHex(muhash), "9c96c6aa15a783f3e6c3d61634e5b9579118f92e0bf562ca3506ddf6b43ac647");
}

//! Order independence: inserting the same elements in a different order
//! results in the same digest.
BOOST_AUTO_TEST_CASE(muhash_order_independence)
{
    const std::vector<unsigned char> a(32, 0x01);
    const std::vector<unsigned char> b(32, 0x02);
    const std::vector<unsigned char> c(32, 0x03);

    MuHash3072 muhash1;
    Insert(muhash1, a);
    Insert(muhash1, b);
    Insert(muhash1, c);

    MuHash3072 muhash2;
    Insert(muhash2, c);
    Insert(muhash2, a);
    Insert(muhash2, b);

    BOOST_CHECK_EQUAL(FinalizeHex(muhash1), FinalizeHex(muhash2));
}

//! Inserting and removing the same element restores the previous digest.
BOOST_AUTO_TEST_CASE(muhash_insert_remove)
{
    const std::vector<unsigned char> a(32, 0x01);
    const std::vector<unsigned char> b(32, 0x02);

    MuHash3072 muhash;
    const std::string empty_digest = FinalizeHex(muhash);

    Insert(muhash, a);
    Insert(muhash, b);
    Remove(muhash, a);
    Remove(muhash, b);

    BOOST_CHECK_EQUAL(FinalizeHex(muhash), empty_digest);
}

//! Serialization round trip.
BOOST_AUTO_TEST_CASE(muhash_serialization)
{
    MuHash3072 muhash;
    Insert(muhash, std::vector<unsigned char>(32, 0x01));
    Insert(muhash, std::vector<unsigned char>(32, 0x02));

    CDataStream ss(SER_DISK, PROTOCOL_VERSION);
    ss << muhash;

    MuHash3072 restored;
    ss >> restored;

    BOOST_CHECK_EQUAL(FinalizeHex(restored), FinalizeHex(muhash));
}

BOOST_AUTO_TEST_SUITE_END()
