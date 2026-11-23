#!/usr/bin/env python3
# Copyright (c) DeFi Blockchain Developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Functional tests for the EVM precompiles.

The address -> precompile registration lives in
``lib/ain-evm/src/precompiles/mod.rs`` (``MetachainPrecompiles``) and is
wired into the executor in ``lib/ain-evm/src/executor.rs``.  Until now it
was only covered by the Rust unit tests, i.e. a regression of the
registration (a wrong address, a dropped dispatch arm, a changed gas
constant) would not have been caught by any black-box test.

This test reaches every registered precompile through a running node:

  * ``0x..01`` ``ecrecover``, ``0x..02`` ``sha256``, ``0x..03`` ``ripemd160``,
    ``0x..04`` ``identity``, ``0x..05`` ``modexp``, ``0x..06`` ``bn128Add``,
    ``0x..07`` ``bn128Mul``, ``0x..08`` ``bn128Pairing`` and ``0x..09``
    ``blake2f`` are called and their return data is compared against the
    expected bytes,
  * ``0x..0a`` (DST20 token split) is covered by ``feature_evm_token_split.py``
    and ``feature_evm_token_split_invalid_address.py``; it is only checked for
    the "precompiles are not accounts" property here,
  * ``0x..0b`` is **not** a precompile and must fall through to a normal,
    empty account.

The return data is asserted twice: once directly via ``eth_call`` and once
through a deployed caller contract that performs a ``CALL`` and emits the
returned 64 byte window with ``LOG1``.

The gas is always asserted as a *delta* between two otherwise identical
transactions:

  * **Primary (offset free)**: a transaction whose ``to`` *is* the precompile
    address versus an identical transaction to an account without code.  Both
    transactions have the same intrinsic gas and the plain callee executes
    nothing, therefore ``gasUsed(precompile) - gasUsed(plain)`` is exactly the
    precompile cost.  No ``CALL`` opcode is involved, so neither an
    EIP-2929 account access cost nor a memory expansion asymmetry can leak
    into the delta.
  * **Secondary (documented offset)**: the same measurement through a
    deployed caller contract.  Here the baseline target is a contract with a
    one byte ``STOP`` runtime.  A ``CALL`` to a regular account is charged
    ``gas_account_access_cold = 2600`` (``Config::shanghai()``), while a
    ``CALL`` to a precompile is charged the warm cost
    ``gas_storage_read_warm = 100`` because the pinned ``evm`` crate treats
    precompiles as warm (``src/executor/stack/executor.rs``,
    ``StackExecutor::is_cold``: ``!is_precompile && self.state.is_cold(..)``).
    The baseline callee consumes 0 gas, therefore the expected delta is
    ``<precompile cost> - 2500``.  The 2500 is a documented constant offset
    (the cold vs. warm account access difference), not a fitted value.

The expected gas values are derived from the source constants:

  * ``lib/ain-evm/src/precompiles/simple.rs``: ``identity`` 15 + 3/word,
    ``sha256`` 60 + 12/word, ``ripemd160`` 600 + 120/word, ``ecrecover``
    3000 (flat),
  * ``lib/ain-evm/src/precompiles/bn128.rs``: ``bn128Add`` 150, ``bn128Mul``
    6000, ``bn128Pairing`` 45000 + 34000 per pair,
  * ``lib/ain-evm/src/precompiles/blake2.rs``: 1 gas per round,
  * ``lib/ain-evm/src/precompiles/modexp.rs``: EIP-2565 with a minimum of
    200, multiplied by 20 for an even modulus (a deviation from
    go-ethereum that is tracked in a separate issue and only documented
    here).

The return data vectors are the consensus test vectors of the node itself
(``lib/ain-evm/testdata/``), so the functional test and the Rust unit tests
assert exactly the same expected bytes.
"""

from test_framework.test_framework import DefiTestFramework
from test_framework.util import assert_equal
from test_framework.evm_key_pair import EvmKeyPair

# Gas price (10 gwei) and gas limits used for the regular transactions.
GAS_PRICE = 10_000_000_000
TX_GAS = 1_000_000
DEPLOY_GAS = 3_000_000

# The registered precompiles (``hash(a) = H160::from_low_u64_be(a)``).
ECRECOVER = "0x0000000000000000000000000000000000000001"
SHA256 = "0x0000000000000000000000000000000000000002"
RIPEMD160 = "0x0000000000000000000000000000000000000003"
IDENTITY = "0x0000000000000000000000000000000000000004"
MODEXP = "0x0000000000000000000000000000000000000005"
BN128_ADD = "0x0000000000000000000000000000000000000006"
BN128_MUL = "0x0000000000000000000000000000000000000007"
BN128_PAIRING = "0x0000000000000000000000000000000000000008"
BLAKE2F = "0x0000000000000000000000000000000000000009"
DST20_TOKEN_SPLIT = "0x000000000000000000000000000000000000000a"

# The next address is **not** registered as a precompile.
NOT_A_PRECOMPILE = "0x000000000000000000000000000000000000000b"

# A plain account without code, used as the baseline of the offset free
# measurement.  It is created in `setup()` so that the account creation can
# never influence the measured delta.
PLAIN_ACCOUNT = "0x00000000000000000000000000000000000000ee"

# The difference between the cold account access cost (2600) and the warm
# account access cost (100) that is charged for a precompile.
CALL_ACCESS_OFFSET = 2500

# Size of the memory window the caller contract reads the return data from.
OUTPUT_SIZE = 64
OUTPUT_OFFSET = 0x100
# Offset the caller contract copies the call data to.
INPUT_OFFSET = 0x200

# Runtime code of the baseline target: a single ``STOP``.
RUNTIME_STOP = bytes([0x00])

# The return data vectors.  The ``ecrecover``, ``bn128`` and ``blake2f``
# vectors are the consensus test vectors of the node itself
# (``lib/ain-evm/testdata/``), the ``sha256`` and ``ripemd160`` vectors are
# the well known hash vectors of "abc".
SHA256_ABC = bytes.fromhex(
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
)
RIPEMD160_ABC = bytes.fromhex(
    "0000000000000000000000008eb208f7e05d987a9b044a8e98c6b087f15a0bfc"
)
# ``ecRecover.json`` / "ValidKey"
ECRECOVER_INPUT = bytes.fromhex(
    "18c547e4f7b0f325ad1e56f57e26c745b09a3e503d86e00e5255ff7f715d3d1c"
    "000000000000000000000000000000000000000000000000000000000000001c"
    "73b1693892219d736caba55bdb67216e485557ea6b6af75f37096c9aa6a5a75fe"
    "eb940b1d03b21e36b0e47e79769f095fe2ab855bd91e3a38756b7d75a9c4549"
)
ECRECOVER_OUTPUT = bytes.fromhex(
    "000000000000000000000000a94f5374fce5edbc8e2a8697c15331677e6ebf0b"
)
# ``common_bnadd.json``
BN128_ADD_INPUT = bytes.fromhex(
    "089142debb13c461f61523586a60732d8b69c5b38a3380a74da7b2961d867dbf"
    "2d5fc7bbc013c16d7945f190b232eacc25da675c0eb093fe6b9f1b4b4e107b362"
    "5f8c89ea3437f44f8fc8b6bfbb6312074dc6f983809a5e809ff4e1d076dd5850b"
    "38c7ced6e4daef9c4347f370d6d8b58f4b1d8dc61a3c59d651a0644a2a27cf"
)
BN128_ADD_OUTPUT = bytes.fromhex(
    "0a6678fd675aa4d8f0d03a1feb921a27f38ebdcb860cc083653519655acd6d79"
    "172fd5b3b2bfdd44e43bcec3eace9347608f9f0a16f1e184cb3f52e6f259cbeb"
)
# ``common_bnmul.json`` / "bnmul_0_0"
BN128_MUL_INPUT = bytes.fromhex(
    "089142debb13c461f61523586a60732d8b69c5b38a3380a74da7b2961d867dbf"
    "2d5fc7bbc013c16d7945f190b232eacc25da675c0eb093fe6b9f1b4b4e107b36"
    "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
)
BN128_MUL_OUTPUT = bytes.fromhex(
    "0bf982b98a2757878c051bfe7eee228b12bc69274b918f08d9fcb21e9184ddc10"
    "b17c77cbf3c19d5d27e18cbd4a8c336afb488d0e92c18d56e64dd4ea5c437e6"
)
# ``common_bnpair.json`` / "bnpair_1_0", one pair.
BN128_PAIRING_ONE_PAIR_INPUT = bytes.fromhex(
    "089142debb13c461f61523586a60732d8b69c5b38a3380a74da7b2961d867dbf"
    "2d5fc7bbc013c16d7945f190b232eacc25da675c0eb093fe6b9f1b4b4e107b36"
    "29f2c1dbcc614745f242077001ec9edd475acdab9ab435770d456bd22bbd2abf"
    "268683f9b1be0bde4508e2e25e51f6b44da3546e87524337d506fd03c4ff7ce0"
    "1851abe58ef4e08916bec8034ca62c04cd08340ab6cc525e6170634092622165"
    "1b71422869c92e49465200ca19033a8aa425f955be3d8329c4475503e45c00e1"
)
# ``common_bnpair.json`` / "bnpair_2_0", two pairs.
BN128_PAIRING_TWO_PAIRS_INPUT = bytes.fromhex(
    "1a4db8abfea95acd4ac4bdf3d9082de13dcb9b2a73d6aa98a296a7d7d1009a8a"
    "1e8a143f9ed8661a223e211c4f70b1ac1d5057d71e31f182637b6f26de9a5919"
    "0e817e9fb476d397b0f09aa07d290cc830ceac3ef815c5b8f45caf2cdafd90e2"
    "09888f93aab917c7b6a9f8ee7174ff889961ff3509256873a774926c0c5b9a0f"
    "1005b845c9f53465d9371efb0eb934ecc8db144d466eaf06614e89c77305c355"
    "01951dfd5ce7f374b05b2b2a0737a5251ce4d9f53f0ee7fe79399aa09d6bfe94"
    "14e57a7979f8f2d78a1c7dc790476265240fff65e18344caa11b0698a574d441"
    "2bb862b7c0f8283e05e10a9b6e3bd19e5b69f6988b3f470692f26c379c331321"
    "26a4d85b3499d830033f0388c18b8e7b824b5b0b57db1fe366a435b0c1a53c60"
    "00ed1afda982e3f8d71da60ede43dd562ae63597ec91b1cba21e1c9fd5b8fbd8"
    "1795c6b9f6debf45b77638122c7333a59aaa2e7511f526f496feb84c20f65155"
    "2581dd3c6f3d05c78234b559d784f502fa5dc0900ae7475ba88716e5ac3d499d"
)
# The pairing product of these vectors is not one, so the precompile returns
# zero.  An empty input is the only pairing check with the result one.
BN128_PAIRING_OUTPUT = bytes(32)
# ``blake2F.json``, 12 rounds, the BLAKE2b-512 hash of "abc".
BLAKE2F_INPUT = bytes.fromhex(
    "0000000c48c9bdf267e6096a3ba7ca8485ae67bb2bf894fe72f36e3cf1361d5f"
    "3af54fa5d182e6ad7f520e511f6c3e2b8c68059b6bbd41fbabd9831f79217e13"
    "19cde05b61626300000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000"
    "000000000300000000000000000000000000000001"
)
assert len(BLAKE2F_INPUT) == 213
BLAKE2F_OUTPUT = bytes.fromhex(
    "ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d1"
    "7d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923"
)


def push2(value: int) -> bytes:
    """Encode `value` as ``PUSH2 <value>``."""
    if value > 0xFFFF:
        raise ValueError("value does not fit into two bytes")
    return bytes([0x61]) + value.to_bytes(2, "big")


def build_caller_runtime(target: bytes, call_data: bytes) -> bytes:
    """Build the runtime code of the generic caller contract.

    The runtime code is

        CODECOPY(call_data) -> CALL(target) -> LOG1(64 byte window) -> STOP

    The call data is appended to the runtime code and copied into the memory
    with ``CODECOPY``, therefore the deployed code depends on the call data
    and on the target address.  The ``CALL`` output region is a fixed 64 byte
    window at ``0x100`` and the call data lives at ``0x200``, so the memory
    expansion and the ``LOG1`` payload are identical for two callers that only
    differ in their target address.
    """
    length = len(call_data)
    # The code offset of the appended call data.  All pushes have a fixed
    # width, so the offset is known before the code is assembled.
    code_offset = 59
    code = b""
    # CODECOPY(destination = 0x200, offset = code_offset, length = length)
    code += push2(length)
    code += push2(code_offset)
    code += push2(INPUT_OFFSET)
    code += bytes([0x39])  # CODECOPY
    # CALL(gas, target, value = 0, args = 0x200, args length, ret = 0x100, 64)
    code += push2(OUTPUT_SIZE)
    code += push2(OUTPUT_OFFSET)
    code += push2(length)
    code += push2(INPUT_OFFSET)
    code += push2(0)
    code += bytes([0x73]) + target  # PUSH20 <target>
    code += bytes([0x5A])  # GAS
    code += bytes([0xF1])  # CALL
    # LOG1(offset = 0x100, size = 64, topic = 0)
    code += push2(0)
    code += push2(OUTPUT_SIZE)
    code += push2(OUTPUT_OFFSET)
    code += bytes([0xA1])  # LOG1
    code += bytes([0x00])  # STOP
    if len(code) != code_offset:
        raise AssertionError("unexpected caller code length")
    return code + call_data


def build_init_code(runtime: bytes) -> str:
    """Build init code that deploys `runtime` as the contract code.

    The init code copies the runtime code to the memory and returns it, which
    makes the deployed code exactly the given runtime code.
    """
    length = len(runtime)
    if length < 0x100:
        push_length = bytes([0x60, length])
    elif length < 0x10000:
        push_length = bytes([0x61, (length >> 8) & 0xFF, length & 0xFF])
    else:
        raise ValueError("runtime code too long")
    # PUSH <length> PUSH <offset> PUSH 0 CODECOPY PUSH <length> PUSH 0 RETURN
    prefix_length = 2 * len(push_length) + 8
    init_code = (
        push_length
        + bytes([0x60, prefix_length])
        + bytes.fromhex("600039")
        + push_length
        + bytes.fromhex("6000f3")
    )
    return "0x" + (init_code + runtime).hex()


class EVMPrecompileTest(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [
            [
                "-dummypos=0",
                "-txnotokens=0",
                "-amkheight=50",
                "-bayfrontheight=51",
                "-dakotaheight=51",
                "-eunosheight=80",
                "-fortcanningheight=82",
                "-fortcanninghillheight=84",
                "-fortcanningroadheight=86",
                "-fortcanningcrunchheight=88",
                "-fortcanningspringheight=90",
                "-fortcanninggreatworldheight=94",
                "-fortcanningepilogueheight=96",
                "-grandcentralheight=101",
                "-metachainheight=105",
                "-subsidytest=1",
                "-evmestimategaserrorratio=0",
            ],
        ]

    def setup(self):
        self.address = self.nodes[0].get_genesis_keys().ownerAuthAddress

        # Generate chain
        self.nodes[0].generate(105)

        self.nodes[0].getbalance()
        self.nodes[0].utxostoaccount({self.address: "201@DFI"})
        self.nodes[0].setgov(
            {
                "ATTRIBUTES": {
                    "v0/params/feature/evm": "true",
                    "v0/params/feature/transferdomain": "true",
                    "v0/transferdomain/dvm-evm/enabled": "true",
                    "v0/transferdomain/dvm-evm/src-formats": ["p2pkh", "bech32"],
                    "v0/transferdomain/dvm-evm/dest-formats": ["erc55"],
                    "v0/transferdomain/evm-dvm/src-formats": ["erc55"],
                    "v0/transferdomain/evm-dvm/auth-formats": ["bech32-erc55"],
                    "v0/transferdomain/evm-dvm/dest-formats": ["p2pkh", "bech32"],
                }
            }
        )
        self.nodes[0].generate(2)

        self.w3 = self.nodes[0].w3
        self.evm_key_pair = EvmKeyPair.from_node(self.nodes[0])
        self.sender = self.evm_key_pair.address
        self.privkey = self.evm_key_pair.privkey

        self.nodes[0].transferdomain(
            [
                {
                    "src": {"address": self.address, "amount": "50@DFI", "domain": 2},
                    "dst": {
                        "address": self.sender,
                        "amount": "50@DFI",
                        "domain": 3,
                    },
                    "singlekeycheck": False,
                }
            ]
        )
        self.nodes[0].generate(1)

        # Create the plain accounts that are used as the baseline of the
        # offset free measurement.  The accounts exist from the very
        # beginning, so their creation can never influence a gas delta.
        self.send_tx(to=PLAIN_ACCOUNT)
        self.send_tx(to=NOT_A_PRECOMPILE)

        # The baseline target of the contract based measurement: a deployed
        # contract with a one byte ``STOP`` runtime.
        self.baseline_contract = self.deploy_raw(RUNTIME_STOP)

    # ------------------------------------------------------------------
    # helpers
    # ------------------------------------------------------------------

    def checksum(self, address: str) -> str:
        """Return the checksummed hex address of the given address."""
        return self.w3.to_checksum_address(address)

    def send_tx(self, to=None, data="0x", gas=TX_GAS, value=0):
        """Sign, send and mine a transaction, return its receipt."""
        tx = {
            "chainId": self.w3.eth.chain_id,
            "nonce": self.w3.eth.get_transaction_count(self.sender),
            "gas": gas,
            "gasPrice": GAS_PRICE,
        }
        if to is not None:
            tx["to"] = self.checksum(to)
        if data and data != "0x":
            tx["data"] = data
        if value:
            tx["value"] = value
        signed = self.w3.eth.account.sign_transaction(tx, self.privkey)
        txid = self.w3.eth.send_raw_transaction(signed.rawTransaction)
        self.nodes[0].generate(1)
        return self.w3.eth.wait_for_transaction_receipt(txid)

    def deploy_raw(self, runtime: bytes) -> str:
        """Deploy a contract with hand written runtime code."""
        receipt = self.send_tx(to=None, data=build_init_code(runtime), gas=DEPLOY_GAS)
        assert_equal(receipt["status"], 1)
        return receipt["contractAddress"]

    def deploy_caller(self, target: str, call_data: bytes) -> str:
        """Deploy a caller contract that calls `target` with `call_data`."""
        runtime = build_caller_runtime(bytes.fromhex(target[2:]), call_data)
        return self.deploy_raw(runtime)

    def eth_call(self, to: str, data: bytes) -> bytes:
        """Call `to` with `data` and return the returned bytes."""
        result = self.w3.eth.call(
            {
                "from": self.sender,
                "to": self.checksum(to),
                "data": "0x" + data.hex(),
            }
        )
        return bytes(result)

    def direct_delta(self, target: str, data: bytes) -> int:
        """Return the gas a top level call to `target` charges (offset free).

        The baseline is an identical transaction to an account without code:
        the intrinsic gas is the same and the plain callee executes nothing,
        so the difference is exactly the cost of the callee.
        """
        hex_data = "0x" + data.hex()
        used_target = self.send_tx(to=target, data=hex_data)["gasUsed"]
        used_plain = self.send_tx(to=PLAIN_ACCOUNT, data=hex_data)["gasUsed"]
        return used_target - used_plain

    def contract_delta(self, target: str, data: bytes):
        """Return the gas delta and the output window of the caller contract.

        The baseline is the same caller contract that calls the ``STOP``
        contract.  The expected delta is the precompile cost minus the
        documented ``CALL_ACCESS_OFFSET`` (cold vs. warm account access).
        """
        caller = self.deploy_caller(target, data)
        baseline = self.deploy_caller(self.baseline_contract, data)

        receipt = self.send_tx(to=caller)
        assert_equal(receipt["status"], 1)
        used_target = receipt["gasUsed"]
        used_baseline = self.send_tx(to=baseline)["gasUsed"]

        logs = receipt["logs"]
        assert_equal(len(logs), 1)
        payload = bytes(logs[0]["data"])
        assert_equal(len(payload), OUTPUT_SIZE)

        return used_target - used_baseline, payload

    def assert_precompile(self, target, data, expected_output, expected_gas):
        """Assert the return data and the gas cost of a precompile.

        The return data is asserted directly (``eth_call``) and through the
        deployed caller contract, the gas is asserted with the offset free
        delta and with the contract based delta.
        """
        assert_equal(self.eth_call(target, data), expected_output)

        assert_equal(self.direct_delta(target, data), expected_gas)

        delta, payload = self.contract_delta(target, data)
        assert_equal(
            payload,
            expected_output + bytes(OUTPUT_SIZE - len(expected_output)),
        )
        assert_equal(delta, expected_gas - CALL_ACCESS_OFFSET)

    # ------------------------------------------------------------------
    # scenarios
    # ------------------------------------------------------------------

    def test_precompiles_are_not_accounts(self):
        """The precompile addresses hold neither code nor a balance."""
        for address in [
            ECRECOVER,
            SHA256,
            RIPEMD160,
            IDENTITY,
            MODEXP,
            BN128_ADD,
            BN128_MUL,
            BN128_PAIRING,
            BLAKE2F,
            DST20_TOKEN_SPLIT,
        ]:
            assert_equal(self.w3.eth.get_code(self.checksum(address)), b"")
            assert_equal(self.w3.eth.get_balance(self.checksum(address)), 0)

        # The address behind the last registered precompile is a plain,
        # empty account.
        assert_equal(self.w3.eth.get_code(self.checksum(NOT_A_PRECOMPILE)), b"")
        assert_equal(self.w3.eth.get_balance(self.checksum(NOT_A_PRECOMPILE)), 0)

    def test_identity(self):
        """``identity`` returns its input and costs 15 + 3 per word."""
        data = bytes(range(32))
        # 15 + 3 * ceil(32 / 32)
        self.assert_precompile(IDENTITY, data, data, 18)

    def test_sha256(self):
        """``sha256`` returns the SHA-256 digest and costs 60 + 12 per word."""
        data = b"abc"
        # 60 + 12 * ceil(3 / 32)
        self.assert_precompile(SHA256, data, SHA256_ABC, 72)

    def test_ripemd160(self):
        """``ripemd160`` returns the padded digest and costs 600 + 120 per word."""
        data = b"abc"
        # 600 + 120 * ceil(3 / 32)
        self.assert_precompile(RIPEMD160, data, RIPEMD160_ABC, 720)

    def test_ecrecover(self):
        """``ecrecover`` recovers the address and costs 3000 gas."""
        # The cost is flat: 3000 + 0 * ceil(128 / 32).
        self.assert_precompile(ECRECOVER, ECRECOVER_INPUT, ECRECOVER_OUTPUT, 3000)

    def test_ecrecover_invalid_v(self):
        """``ecrecover`` returns no data for an invalid ``v`` but charges 3000."""
        # The recovery id is the last byte of the second word and must be
        # 27 or 28 (``lib/ain-evm/src/precompiles/simple.rs``).  The cost is
        # recorded before the check, so the gas is charged nevertheless.
        data = bytearray(ECRECOVER_INPUT)
        data[63] = 29
        data = bytes(data)

        assert_equal(self.eth_call(ECRECOVER, data), b"")
        assert_equal(self.direct_delta(ECRECOVER, data), 3000)

        # The caller contract gets an empty output window.
        delta, payload = self.contract_delta(ECRECOVER, data)
        assert_equal(payload, bytes(OUTPUT_SIZE))
        assert_equal(delta, 3000 - CALL_ACCESS_OFFSET)

    def test_modexp(self):
        """``modexp`` computes the modular exponentiation.

        The cost follows EIP-2565 with a minimum of 200 gas for both odd and
        even moduli once DF25 is active (regtest default: ``DF25Height = 1``).
        """
        # 3^5 mod 7 = 5
        data = (
            (1).to_bytes(32, "big")
            + (1).to_bytes(32, "big")
            + (1).to_bytes(32, "big")
            + bytes([3, 5, 7])
        )
        self.assert_precompile(MODEXP, data, bytes([5]), 200)

        # 3^5 mod 8 = 3, even modulus also costs 200 gas at/above DF25Height.
        data = (
            (1).to_bytes(32, "big")
            + (1).to_bytes(32, "big")
            + (1).to_bytes(32, "big")
            + bytes([3, 5, 8])
        )
        self.assert_precompile(MODEXP, data, bytes([3]), 200)

    def test_bn128_add(self):
        """``bn128Add`` adds two points and costs 150 gas."""
        self.assert_precompile(BN128_ADD, BN128_ADD_INPUT, BN128_ADD_OUTPUT, 150)

    def test_bn128_mul(self):
        """``bn128Mul`` multiplies a point and costs 6000 gas."""
        self.assert_precompile(BN128_MUL, BN128_MUL_INPUT, BN128_MUL_OUTPUT, 6000)

    def test_bn128_pairing(self):
        """``bn128Pairing`` checks a pairing and costs 45000 + 34000 per pair."""
        # An empty input is a valid pairing check without any pair, the
        # result is one.
        self.assert_precompile(BN128_PAIRING, b"", bytes(31) + bytes([1]), 45000)

        # One pair: 45000 + 34000.
        self.assert_precompile(
            BN128_PAIRING, BN128_PAIRING_ONE_PAIR_INPUT, BN128_PAIRING_OUTPUT, 79000
        )

        # Two pairs: 45000 + 2 * 34000.
        self.assert_precompile(
            BN128_PAIRING, BN128_PAIRING_TWO_PAIRS_INPUT, BN128_PAIRING_OUTPUT, 113000
        )

    def test_blake2f(self):
        """``blake2f`` runs the compression function and costs 1 gas per round."""
        # 12 rounds
        self.assert_precompile(BLAKE2F, BLAKE2F_INPUT, BLAKE2F_OUTPUT, 12)

    def test_blake2f_invalid_length(self):
        """``blake2f`` rejects an input that is not exactly 213 bytes long."""
        # The input length is validated before the cost is recorded, so the
        # call fails without charging the precompile cost.  The caller frame
        # reverts and consumes the whole gas limit; only the failure is
        # asserted here.
        data = BLAKE2F_INPUT[:-1]
        receipt = self.send_tx(to=BLAKE2F, data="0x" + data.hex())
        assert_equal(receipt["status"], 0)
        assert_equal(receipt["gasUsed"], TX_GAS)

    def test_bn128_pairing_invalid_length(self):
        """``bn128Pairing`` rejects an input that is not a multiple of 192."""
        # The input length is validated before the cost is recorded
        # (``lib/ain-evm/src/precompiles/bn128.rs``), so the call fails
        # without charging the precompile cost.  Only the failure is
        # asserted here.
        data = bytes(100)
        receipt = self.send_tx(to=BN128_PAIRING, data="0x" + data.hex())
        assert_equal(receipt["status"], 0)
        assert_equal(receipt["gasUsed"], TX_GAS)

    def test_unknown_address_is_not_a_precompile(self):
        """An unregistered address falls through to a normal, empty account."""
        data = bytes(32)
        assert_equal(self.eth_call(NOT_A_PRECOMPILE, data), b"")
        assert_equal(self.direct_delta(NOT_A_PRECOMPILE, data), 0)

        delta, payload = self.contract_delta(NOT_A_PRECOMPILE, data)
        assert_equal(payload, bytes(OUTPUT_SIZE))
        assert_equal(delta, 0)

    def run_test(self):
        self.setup()

        self.test_precompiles_are_not_accounts()
        self.test_identity()
        self.test_sha256()
        self.test_ripemd160()
        self.test_ecrecover()
        self.test_ecrecover_invalid_v()
        self.test_modexp()
        self.test_bn128_add()
        self.test_bn128_mul()
        self.test_bn128_pairing()
        self.test_blake2f()
        self.test_blake2f_invalid_length()
        self.test_bn128_pairing_invalid_length()
        self.test_unknown_address_is_not_a_precompile()


if __name__ == "__main__":
    EVMPrecompileTest().main()
