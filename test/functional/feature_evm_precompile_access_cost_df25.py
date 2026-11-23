#!/usr/bin/env python3
# Copyright (c) DeFi Blockchain Developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test the DF25Height activation gating of the EIP-2929 precompile access cost.

Before DF25Height, the first access to a precompile address inside a
transaction is charged with the *warm* account access cost (100 gas), because
the pinned ``evm`` crate treats precompiles as permanently warm
(``StackExecutor::is_cold``: ``!is_precompile && self.state.is_cold(..)``).
At and above DF25Height, the first access to a precompile address is charged
with the EIP-2929 *cold* account access cost (2600 gas), exactly like
go-ethereum does for every address.

The test measures the gas of the access through a deployed caller contract:

  * **contract based delta**: the gas difference between a caller contract
    that calls the precompile and an otherwise identical caller contract that
    calls a plain contract with a one byte ``STOP`` runtime.  Both calls
    access their target for the first time, therefore the delta is the
    precompile cost plus the difference between the cold and the warm account
    access cost:

      - below DF25Height: ``<precompile cost> + 100 - 2600``
      - at/above DF25Height: ``<precompile cost>``

  * **second access**: the gas difference between a caller contract that calls
    the precompile twice and one that calls it once.  The second access to the
    same address is warm (100 gas) on both sides of the activation, so the
    delta is ``<precompile cost> + 100`` in both cases.

  * **top level transaction**: a transaction whose ``to`` *is* the precompile
    address is not affected by the activation, because the transaction target
    is pre-warmed by EIP-2929.  The offset free delta against a plain account
    stays ``<precompile cost>``.

``eth_estimateGas`` is cross-checked against the mined ``gasUsed`` on both
sides of the activation.  The estimate is evaluated against the reference
block, the mined transaction lands in the next block, therefore the
measurements are taken at least two blocks away from the activation height.
"""

from test_framework.evm_key_pair import EvmKeyPair
from test_framework.test_framework import DefiTestFramework
from test_framework.util import assert_equal

# The activation height of the new precompile access pricing.  It is chosen
# high enough that all the measurements below the activation height fit into
# the blocks that are mined before it.
DF25_HEIGHT = 160

GAS_PRICE = 10_000_000_000
TX_GAS = 1_000_000
DEPLOY_GAS = 3_000_000

# The registered precompiles (``hash(a) = H160::from_low_u64_be(a)``).
IDENTITY = "0x0000000000000000000000000000000000000004"

# A plain account without code, used as the baseline of the offset free
# measurement.
PLAIN_ACCOUNT = "0x00000000000000000000000000000000000000ee"

# The cost of the identity precompile for an empty input
# (``lib/ain-evm/src/precompiles/simple.rs``: 15 + 3 per word).
PRECOMPILE_COST = 15

# The EIP-2929 account access costs (``Config::shanghai()``).
COLD_ACCESS_COST = 2600
WARM_ACCESS_COST = 100
# The difference between the cold and the warm account access cost.
CALL_ACCESS_OFFSET = COLD_ACCESS_COST - WARM_ACCESS_COST

# The gas of the stack setup of an additional ``CALL``: five ``PUSH2`` (3 gas
# each), one ``PUSH20`` (3 gas) and one ``PUSH3`` (3 gas).  It is part of the
# second access measurement, because the caller contract of the second access
# contains one more ``CALL`` than the caller contract of the single access.
EXTRA_CALL_SETUP_COST = 21

# Size of the memory window the caller contract reads the return data from.
OUTPUT_SIZE = 64
OUTPUT_OFFSET = 0x100

# The gas the caller contract forwards to the precompile.  A fixed amount
# keeps the caller independent of the remaining gas.
CALL_GAS = 100_000

# The tolerance of the ``eth_estimateGas`` cross-check.  The estimate is the
# minimal gas limit for which the transaction succeeds, the mined ``gasUsed``
# is the gas the transaction actually consumed, therefore the two differ by
# the (small) amount of gas that is left over at the end of the transaction.
ESTIMATE_TOLERANCE = 500


def push2(value: int) -> bytes:
    """Encode `value` as ``PUSH2 <value>``."""
    if value > 0xFFFF:
        raise ValueError("value does not fit into two bytes")
    return bytes([0x61]) + value.to_bytes(2, "big")


def push_value(value: int) -> bytes:
    """Encode `value` with the shortest possible ``PUSH<n>``."""
    if value > 0xFFFFFF:
        raise ValueError("value does not fit into three bytes")
    if value <= 0xFF:
        return bytes([0x60]) + value.to_bytes(1, "big")
    if value <= 0xFFFF:
        return push2(value)
    return bytes([0x62]) + value.to_bytes(3, "big")


def build_caller_runtime(target: bytes, calls: int) -> bytes:
    """Build the runtime code of a caller contract.

    The runtime code performs `calls` ``CALL``s to `target` and emits the
    64 byte return window of the last call with ``LOG1``:

        CALL(target) [CALL(target) ...] -> LOG1(64 byte window) -> STOP

    The call data is empty, therefore the runtime code does not depend on any
    offset and the memory expansion is identical for every variant.
    """
    if calls < 1:
        raise ValueError("at least one call is required")
    code = b""
    for _ in range(calls):
        # CALL(gas, target, value = 0, args = 0, args length = 0, ret = 0x100, 64)
        code += push2(OUTPUT_SIZE)
        code += push2(OUTPUT_OFFSET)
        code += push2(0)
        code += push2(0)
        code += push2(0)
        code += bytes([0x73]) + target  # PUSH20 <target>
        code += push_value(CALL_GAS)
        code += bytes([0xF1])  # CALL
    # LOG1(offset = 0x100, size = 64, topic = 0)
    code += push2(0)
    code += push2(OUTPUT_SIZE)
    code += push2(OUTPUT_OFFSET)
    code += bytes([0xA1])  # LOG1
    code += bytes([0x00])  # STOP
    return code


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


class EvmPrecompileAccessCostDF25Test(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [
            [
                "-dummypos=0",
                "-txnotokens=0",
                "-amkheight=50",
                "-bayfrontheight=51",
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
                "-df23height=105",
                "-df24height=105",
                f"-df25height={DF25_HEIGHT}",
                "-subsidytest=1",
                "-evmestimategaserrorratio=0",
            ],
        ]

    def setup(self):
        self.address = self.nodes[0].get_genesis_keys().ownerAuthAddress
        self.eth_address = EvmKeyPair.from_node(self.nodes[0])
        self.w3 = self.nodes[0].w3
        self.sender = self.w3.to_checksum_address(self.eth_address.address)
        self.privkey = self.eth_address.privkey

        self.nodes[0].generate(105)
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
                    "v0/transferdomain/evm-dvm/dest-formats": ["bech32", "p2pkh"],
                }
            }
        )
        self.nodes[0].generate(2)

        self.nodes[0].utxostoaccount({self.address: "100@DFI"})
        self.nodes[0].generate(1)
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

        # Touch the plain account so its creation never affects gas deltas.
        self.send_tx(to=PLAIN_ACCOUNT)

        # The baseline target of the contract based measurement: a deployed
        # contract with a one byte ``STOP`` runtime.
        self.baseline_contract = self.deploy_raw(bytes([0x00]))

    # ------------------------------------------------------------------
    # helpers
    # ------------------------------------------------------------------

    def send_tx(self, to=None, data="0x", gas=TX_GAS, value=0):
        """Sign, send and mine a transaction, return its receipt."""
        tx = {
            "chainId": self.w3.eth.chain_id,
            "nonce": self.w3.eth.get_transaction_count(self.sender),
            "gas": gas,
            "gasPrice": GAS_PRICE,
        }
        if to is not None:
            tx["to"] = self.w3.to_checksum_address(to)
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

    def deploy_caller(self, target: str, calls: int = 1) -> str:
        """Deploy a caller contract that calls `target` `calls` times."""
        runtime = build_caller_runtime(bytes.fromhex(target[2:]), calls)
        return self.deploy_raw(runtime)

    def direct_delta(self, target: str) -> int:
        """Return the gas a top level call to `target` charges (offset free)."""
        used_target = self.send_tx(to=target)["gasUsed"]
        used_plain = self.send_tx(to=PLAIN_ACCOUNT)["gasUsed"]
        return used_target - used_plain

    def contract_delta(self, target: str):
        """Return the gas delta and the output window of the caller contract.

        The baseline is the same caller contract that calls the ``STOP``
        contract.
        """
        caller = self.deploy_caller(target, calls=1)
        baseline = self.deploy_caller(self.baseline_contract, calls=1)

        receipt = self.send_tx(to=caller)
        assert_equal(receipt["status"], 1)
        used_target = receipt["gasUsed"]
        used_baseline = self.send_tx(to=baseline)["gasUsed"]

        logs = receipt["logs"]
        assert_equal(len(logs), 1)
        payload = bytes(logs[0]["data"])
        assert_equal(len(payload), OUTPUT_SIZE)

        return used_target - used_baseline, payload

    def second_access_delta(self, target: str) -> int:
        """Return the gas of the *second* access to `target` in one tx.

        The caller contract that calls `target` twice is compared with the
        caller contract that calls it once.  The difference is the cost of the
        second access (the warm account access cost) plus the cost of the
        precompile itself.
        """
        twice = self.deploy_caller(target, calls=2)
        once = self.deploy_caller(target, calls=1)

        receipt = self.send_tx(to=twice)
        assert_equal(receipt["status"], 1)
        used_twice = receipt["gasUsed"]
        used_once = self.send_tx(to=once)["gasUsed"]

        return used_twice - used_once

    def estimate_gas(self, to: str) -> int:
        """Return the ``eth_estimateGas`` result for a call to `to`."""
        return self.w3.eth.estimate_gas(
            {"from": self.sender, "to": self.w3.to_checksum_address(to)}
        )

    def assert_estimate_matches(self, target: str):
        """Assert that ``eth_estimateGas`` agrees with the mined ``gasUsed``.

        The caller contract is measured once with ``eth_estimateGas`` and once
        as a mined transaction; the two must agree up to the gas that is left
        over at the end of the transaction.
        """
        caller = self.deploy_caller(target, calls=1)
        estimated = self.estimate_gas(caller)
        receipt = self.send_tx(to=caller)
        assert_equal(receipt["status"], 1)
        used = receipt["gasUsed"]
        assert (
            abs(estimated - used) <= ESTIMATE_TOLERANCE
        ), f"eth_estimateGas ({estimated}) does not agree with the mined gasUsed ({used})"
        return used

    # ------------------------------------------------------------------
    # scenarios
    # ------------------------------------------------------------------

    def run_test(self):
        self.setup()

        # --- Below DF25Height ---
        assert self.nodes[0].getblockcount() < DF25_HEIGHT - 2

        # The top level transaction to a precompile is not affected: the
        # transaction target is pre-warmed by EIP-2929.
        assert_equal(self.direct_delta(IDENTITY), PRECOMPILE_COST)

        # The first access to the precompile inside a transaction is charged
        # with the warm account access cost, therefore the caller contract is
        # *cheaper* than the caller contract of the baseline contract.
        delta, payload = self.contract_delta(IDENTITY)
        assert_equal(payload, bytes(OUTPUT_SIZE))
        assert_equal(
            delta,
            PRECOMPILE_COST + WARM_ACCESS_COST - COLD_ACCESS_COST,
        )

        # The second access to the same precompile is warm (100 gas).
        assert_equal(
            self.second_access_delta(IDENTITY),
            EXTRA_CALL_SETUP_COST + WARM_ACCESS_COST + PRECOMPILE_COST,
        )

        # eth_estimateGas agrees with the mined gasUsed.
        self.assert_estimate_matches(IDENTITY)

        # --- Activate DF25Height ---
        blocks_needed = DF25_HEIGHT + 2 - self.nodes[0].getblockcount()
        assert blocks_needed > 0
        self.nodes[0].generate(blocks_needed)
        assert self.nodes[0].getblockcount() >= DF25_HEIGHT + 2

        # --- At/above DF25Height ---
        # The top level transaction to a precompile is still not affected.
        assert_equal(self.direct_delta(IDENTITY), PRECOMPILE_COST)

        # The first access to the precompile is charged with the cold account
        # access cost, exactly like the access to the baseline contract.
        delta, payload = self.contract_delta(IDENTITY)
        assert_equal(payload, bytes(OUTPUT_SIZE))
        assert_equal(delta, PRECOMPILE_COST)

        # The second access to the same precompile is still warm (100 gas).
        assert_equal(
            self.second_access_delta(IDENTITY),
            EXTRA_CALL_SETUP_COST + WARM_ACCESS_COST + PRECOMPILE_COST,
        )

        # eth_estimateGas agrees with the mined gasUsed.
        self.assert_estimate_matches(IDENTITY)


if __name__ == "__main__":
    EvmPrecompileAccessCostDF25Test().main()
