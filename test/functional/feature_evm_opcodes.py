#!/usr/bin/env python3
# Copyright (c) 2014-2019 The Bitcoin Core developers
# Copyright (c) DeFi Blockchain Developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test EVM opcode and edge-case semantics.

The scenarios covered here are the execution level behaviours that are not
owned by any other functional test:

  * ``CREATE2``: deterministic contract address and salt collision,
  * revert semantics: revert data (``Error(string)``, ``Panic(uint256)`` and
    custom errors), state rollback, receipt status and ``eth_estimateGas``,
  * gas refunds: ``SSTORE`` clear refund, refund cap and ``SELFDESTRUCT``,
  * ``EIP-3541``: deployment of runtime code starting with ``0xEF``,
  * hard execution failures: invalid opcode, stack overflow, out of gas,
  * memory expansion gas cost.

Every hard execution failure is asserted twice: the mined transaction must
fail (receipt status ``0x0``, the whole gas limit consumed, the state change
rolled back) *and* the failure mode itself must be observable, i.e. the
``eth_call`` on the same contract must report the expected EVM error
(``InvalidCode(Opcode(254))``, ``StackOverflow``, ``OutOfGas``).  Without the
second assertion a scenario could pass with a completely different failure
mode.

Gas assertions are always *deltas* between two otherwise identical
transactions (same calldata, same code path, only the pre-state or the
memory offset differs).  Absolute ``gasUsed`` values are never asserted
because they depend on the compiler output and on the intrinsic gas rules.
"""

from test_framework.test_framework import DefiTestFramework
from test_framework.util import assert_equal
from test_framework.evm_contract import EVMContract
from test_framework.evm_key_pair import EvmKeyPair
from web3.utils import get_create_address

# Gas price (10 gwei) and gas limit used for the regular transactions.
GAS_PRICE = 10_000_000_000
TX_GAS = 1_000_000

# Gas limit used for the transactions that are expected to run out of gas.
FAILURE_GAS = 100_000

# Runtime code that writes 1 into storage slot 0 and then executes the
# invalid opcode 0xFE.  The storage write has to be rolled back.
RUNTIME_INVALID_AFTER_SSTORE = bytes.fromhex("6001600055fe")

# Runtime code with the invalid opcode 0xFE.
RUNTIME_INVALID = bytes.fromhex("fe")

# Runtime code that writes 1 into storage slot 0 and then pushes 1025 values
# onto the stack, which exceeds the stack limit of 1024 items
# (`ExitError::StackOverflow`).  The storage write has to be rolled back.
RUNTIME_STACK_OVERFLOW_AFTER_SSTORE = bytes.fromhex("6001600055" + "6001" * 1025)

# Runtime code that writes 1 into storage slot 0 and then runs into an
# endless loop: the `JUMPDEST` sits at offset 5 and the `JUMP` jumps back to
# it, so the frame runs out of gas (`ExitError::OutOfGas`).  The storage
# write has to be rolled back.
RUNTIME_OUT_OF_GAS_AFTER_SSTORE = bytes.fromhex("6001600055" + "5b" + "6005" + "56")

# Runtime code that stores a zero word at memory offset 0 (one memory word).
RUNTIME_MSTORE_LOW = bytes.fromhex("600060005200")

# Runtime code that stores a zero word at memory offset 0x1000, i.e. it
# expands the memory to 0x1000 + 32 bytes = 129 words.
RUNTIME_MSTORE_HIGH = bytes.fromhex("60006110005200")

# Runtime code starting with 0xEF (EIP-3541 forbids deploying it).
RUNTIME_EOF_MAGIC = bytes.fromhex("ef00")


def runtime_selfdestruct(beneficiary: bytes) -> bytes:
    """Runtime code that self destructs and sends the balance to `beneficiary`."""
    return bytes.fromhex("73") + beneficiary + bytes.fromhex("ff")


def runtime_push20_stop(beneficiary: bytes) -> bytes:
    """Runtime code with the same prefix as `runtime_selfdestruct` that stops."""
    return bytes.fromhex("73") + beneficiary + bytes.fromhex("00")


def build_init_code(runtime: bytes) -> str:
    """Build init code that deploys `runtime` as the contract code.

    The init code copies the runtime code to memory and returns it, which
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


class EVMOpcodeTest(DefiTestFramework):
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

        # Deploy all contracts before the rollback height is taken, so that
        # they survive the `rollback_to()` calls of the test methods.
        self.factory, self.factory_abi = self.deploy_solidity(
            "Create2Factory.sol", "Create2Factory"
        )
        self.reverter, self.reverter_abi = self.deploy_solidity(
            "RevertReason.sol", "RevertReason"
        )
        self.gas_refund, self.gas_refund_abi = self.deploy_solidity(
            "GasRefund.sol", "GasRefund"
        )

        self.invalid_contract = self.deploy_raw(RUNTIME_INVALID_AFTER_SSTORE)
        self.invalid_opcode_contract = self.deploy_raw(RUNTIME_INVALID)
        self.stack_overflow_contract = self.deploy_raw(
            RUNTIME_STACK_OVERFLOW_AFTER_SSTORE
        )
        self.out_of_gas_contract = self.deploy_raw(RUNTIME_OUT_OF_GAS_AFTER_SSTORE)
        self.mstore_low_contract = self.deploy_raw(RUNTIME_MSTORE_LOW)
        self.mstore_high_contract = self.deploy_raw(RUNTIME_MSTORE_HIGH)
        self.selfdestruct_contract = self.deploy_raw(
            runtime_selfdestruct(bytes.fromhex(self.sender[2:]))
        )
        self.selfdestruct_baseline_contract = self.deploy_raw(
            runtime_push20_stop(bytes.fromhex(self.sender[2:]))
        )

        self.start_height = self.nodes[0].getblockcount()

    # ------------------------------------------------------------------
    # helpers
    # ------------------------------------------------------------------

    def deploy_solidity(self, file_name: str, contract_name: str):
        """Compile and deploy a Solidity contract.

        Returns the address of the contract and its ABI.
        """
        abi, bytecode, _ = EVMContract.from_file(file_name, contract_name).compile()
        compiled = self.w3.eth.contract(abi=abi, bytecode=bytecode)
        tx = compiled.constructor().build_transaction(
            {
                "chainId": self.w3.eth.chain_id,
                "nonce": self.w3.eth.get_transaction_count(self.sender),
                "gasPrice": GAS_PRICE,
                "gas": TX_GAS,
            }
        )
        signed = self.w3.eth.account.sign_transaction(tx, self.privkey)
        txid = self.w3.eth.send_raw_transaction(signed.rawTransaction)
        self.nodes[0].generate(1)
        receipt = self.w3.eth.wait_for_transaction_receipt(txid)
        assert_equal(receipt["status"], 1)
        return receipt["contractAddress"], abi

    def deploy_raw(self, runtime: bytes) -> str:
        """Deploy a contract with hand written runtime code."""
        receipt = self.send_tx(to=None, data=build_init_code(runtime))
        assert_equal(receipt["status"], 1)
        return receipt["contractAddress"]

    def send_tx(self, to=None, data="0x", gas=TX_GAS, value=0):
        """Sign, send and mine a transaction, return its receipt."""
        tx = {
            "chainId": self.w3.eth.chain_id,
            "nonce": self.w3.eth.get_transaction_count(self.sender),
            "gas": gas,
            "gasPrice": GAS_PRICE,
        }
        if to is not None:
            tx["to"] = to
        if data and data != "0x":
            tx["data"] = data
        if value:
            tx["value"] = value
        signed = self.w3.eth.account.sign_transaction(tx, self.privkey)
        txid = self.w3.eth.send_raw_transaction(signed.rawTransaction)
        self.nodes[0].generate(1)
        return self.w3.eth.wait_for_transaction_receipt(txid)

    def call_contract(self, function, *args):
        """Call a contract function, return the receipt of the mined call."""
        tx = function(*args).build_transaction(
            {
                "chainId": self.w3.eth.chain_id,
                "nonce": self.w3.eth.get_transaction_count(self.sender),
                "gasPrice": GAS_PRICE,
                "gas": TX_GAS,
            }
        )
        signed = self.w3.eth.account.sign_transaction(tx, self.privkey)
        txid = self.w3.eth.send_raw_transaction(signed.rawTransaction)
        self.nodes[0].generate(1)
        return self.w3.eth.wait_for_transaction_receipt(txid)

    def eth_call_error(self, to: str, gas: int = FAILURE_GAS) -> str:
        """Call the contract with `eth_call` and return the error message.

        The call is expected to fail; the message of the raised error holds
        the exit reason of the EVM, e.g. "exit error OutOfGas".
        """
        error = self.catch_error(
            self.w3.eth.call,
            {"from": self.sender, "to": to, "gas": gas},
        )
        message = str(error)
        extra = getattr(error, "message", None)
        if isinstance(extra, str):
            message = f"{message} {extra}"
        return message

    def catch_error(self, fun, *args, **kwargs):
        """Run `fun` and return the raised exception (fails if none is raised)."""
        try:
            fun(*args, **kwargs)
        except Exception as e:
            return e
        raise AssertionError("No exception raised")

    def checksum(self, address: bytes) -> str:
        """Return the checksummed hex address of the given address bytes."""
        return self.w3.to_checksum_address(address.hex())

    def storage_slot(self, address: str, slot: int = 0) -> int:
        """Return the value of a storage slot as an integer."""
        return int.from_bytes(self.w3.eth.get_storage_at(address, slot), "big")

    # ------------------------------------------------------------------
    # scenarios
    # ------------------------------------------------------------------

    def test_create2_deterministic_address(self):
        """CREATE2 deploys at the address derived from factory, salt and init code."""
        self.rollback_to(self.start_height)

        child_abi, child_bytecode, child_runtime = EVMContract.from_file(
            "Create2Child.sol", "Create2Child"
        ).compile()
        init_code = bytes.fromhex(child_bytecode)
        salt = bytes.fromhex("01" * 32)

        # The CREATE2 address is keccak256(0xff ++ factory ++ salt ++
        # keccak256(init_code))[12:] and is computed independently here.
        expected_address = self.w3.keccak(
            b"\xff" + bytes.fromhex(self.factory[2:]) + salt + self.w3.keccak(init_code)
        )[12:]

        factory = self.w3.eth.contract(address=self.factory, abi=self.factory_abi)

        # `eth_call` returns the address that the factory computes.
        returned_address = factory.functions.deploy(salt, init_code).call(
            {"from": self.sender}
        )
        assert_equal(
            bytes.fromhex(returned_address[2:]).lower(), expected_address.lower()
        )

        receipt = self.call_contract(factory.functions.deploy, salt, init_code)
        assert_equal(receipt["status"], 1)

        # The deployed code is the runtime code of the child contract.
        deployed_code = self.w3.eth.get_code(self.checksum(expected_address))
        assert_equal(deployed_code, bytes.fromhex(child_runtime))

        # The contract is functional.
        child = self.w3.eth.contract(
            address=self.checksum(expected_address), abi=child_abi
        )
        assert_equal(child.functions.value().call(), 42)

    def test_create2_salt_collision(self):
        """A second deployment with the same salt and init code fails."""
        self.rollback_to(self.start_height)

        _, child_bytecode, child_runtime = EVMContract.from_file(
            "Create2Child.sol", "Create2Child"
        ).compile()
        init_code = bytes.fromhex(child_bytecode)
        salt = bytes.fromhex("02" * 32)

        expected_address = self.w3.keccak(
            b"\xff" + bytes.fromhex(self.factory[2:]) + salt + self.w3.keccak(init_code)
        )[12:]

        factory = self.w3.eth.contract(address=self.factory, abi=self.factory_abi)

        first = self.call_contract(factory.functions.deploy, salt, init_code)
        assert_equal(first["status"], 1)
        assert_equal(
            self.w3.eth.get_code(self.checksum(expected_address)),
            bytes.fromhex(child_runtime),
        )

        # The second deployment with the same salt and init code collides. The
        # factory turns the zero address returned by `create2` into an
        # explicit revert.
        second = self.call_contract(factory.functions.deploy, salt, init_code)
        assert_equal(second["status"], 0)

        # The factory reverts with an explicit reason.
        error = self.catch_error(
            factory.functions.deploy(salt, init_code).call, {"from": self.sender}
        )
        assert "CREATE2_FAILED".encode().hex() in error.data

        # The code at the address is unchanged.
        assert_equal(
            self.w3.eth.get_code(self.checksum(expected_address)),
            bytes.fromhex(child_runtime),
        )

    def test_revert_reason_and_state_rollback(self):
        """Reverts expose their data, roll back the state and fail the estimate."""
        self.rollback_to(self.start_height)

        contract = self.w3.eth.contract(address=self.reverter, abi=self.reverter_abi)

        # `Error(string)`: the revert data starts with the selector 0x08c379a0
        # and contains the ABI encoded reason.
        error = self.catch_error(
            contract.functions.setThenRevert(1).call, {"from": self.sender}
        )
        assert_equal(error.data[:10], "0x08c379a0")
        assert "Function reverted".encode().hex() in error.data

        # `Panic(uint256)`: the revert data starts with the selector
        # 0x4e487b71 and the panic code 0x12 (division by zero).
        error = self.catch_error(
            contract.functions.triggerPanic(0).call, {"from": self.sender}
        )
        assert_equal(error.data[:10], "0x4e487b71")
        assert_equal(error.data[-4:], "0012")

        # Custom error: the revert data starts with the selector of
        # `CustomFailure(uint256)`.
        selector = self.w3.keccak(text="CustomFailure(uint256)")[:4].hex()
        error = self.catch_error(
            contract.functions.revertWithCustomError().call, {"from": self.sender}
        )
        assert_equal(error.data[:10], selector)

        # `eth_estimateGas` fails for the reverting call.
        self.catch_error(
            contract.functions.setThenRevert(1).estimate_gas, {"from": self.sender}
        )

        # The transaction is mined, but fails and does not change the state.
        receipt = self.call_contract(contract.functions.setThenRevert, 1)
        assert_equal(receipt["status"], 0)
        assert_equal(self.storage_slot(self.reverter), 0)
        assert_equal(contract.functions.value().call(), 0)

        # The chain keeps producing valid blocks.
        height = self.nodes[0].getblockcount()
        self.nodes[0].generate(1)
        assert_equal(self.nodes[0].getblockcount(), height + 1)

    def test_gas_refund_clearing_storage_slot(self):
        """Clearing a slot that is non-zero at transaction start refunds 4800 gas.

        The refund is derived from the EIP-3529 rules that the node uses
        (``Config::shanghai()``):

          * ``SSTORE_RESET`` (2900) plus the EIP-2929 cold surcharge (2100)
            are charged for the reset of a slot that is non-zero at the
            beginning of the transaction,
          * clearing the slot grants the ``SSTORE_CLEARS`` refund (4800),
          * the refund is capped at ``gasUsed / 5`` and the total used gas
            includes the intrinsic gas (21000), so the cap is not reached
            here.

        The two measurements use the very same call (same calldata, same code
        path) and only differ in the value the slot has at the beginning of
        the transaction, therefore all common costs cancel out:

          * slot non-zero: 2900 + 2100 charged, 4800 refunded,
          * slot zero:     100 (warm read) + 2100 (cold surcharge) charged,
            no refund.
        """
        self.rollback_to(self.start_height)

        contract = self.w3.eth.contract(
            address=self.gas_refund, abi=self.gas_refund_abi
        )

        # The slot holds a non-zero value at the beginning of the transaction.
        receipt = self.call_contract(contract.functions.setOne, 1)
        assert_equal(receipt["status"], 1)
        assert_equal(self.storage_slot(self.gas_refund, 0), 1)

        used_non_zero = self.call_contract(contract.functions.clearOne)["gasUsed"]
        assert_equal(self.storage_slot(self.gas_refund, 0), 0)

        # The same call with a slot that is zero already.
        self.rollback_to(self.start_height)
        used_zero = self.call_contract(contract.functions.clearOne)["gasUsed"]

        # The difference of the two runs is the difference of the `SSTORE`
        # costs (5000 - 2200 = 2800) minus the refund (4800).
        assert_equal(used_zero - used_non_zero, 2000)

        # The refund is exactly the EIP-3529 `SSTORE_CLEARS` refund and it is
        # not capped: total used gas = used_zero + 2800.
        total_used_gas = used_zero + 2800
        refund = total_used_gas - used_non_zero
        assert_equal(refund, 4800)
        assert refund <= total_used_gas // 5

    def test_gas_refund_cap(self):
        """The refund is capped at one fifth of the total used gas.

        Clearing ten slots grants a refund of 10 * 4800 = 48000 gas, which is
        more than one fifth of the total used gas of the transaction.  The
        applied refund is therefore the cap, i.e. ``total_used_gas / 5``.
        """
        self.rollback_to(self.start_height)

        contract = self.w3.eth.contract(
            address=self.gas_refund, abi=self.gas_refund_abi
        )

        receipt = self.call_contract(contract.functions.setAll, 1)
        assert_equal(receipt["status"], 1)

        used_non_zero = self.call_contract(contract.functions.clearAll)["gasUsed"]

        # The same call with all slots zero already.
        self.rollback_to(self.start_height)
        used_zero = self.call_contract(contract.functions.clearAll)["gasUsed"]

        # Every slot costs 5000 gas when it is cleared and 2200 gas when it is
        # written with the value it already has.
        total_used_gas = used_zero + 10 * 2800
        refund = total_used_gas - used_non_zero

        # The refund is capped at one fifth of the total used gas ...
        assert refund < 10 * 4800
        # ... and the applied refund is exactly that cap.
        assert_equal(refund, total_used_gas // 5)

    def test_selfdestruct_gives_no_refund(self):
        """`SELFDESTRUCT` does not grant a refund (EIP-3529)."""
        self.rollback_to(self.start_height)

        used_selfdestruct = self.send_tx(to=self.selfdestruct_contract)["gasUsed"]
        # The contract is gone after the self destruct.
        assert_equal(self.w3.eth.get_code(self.selfdestruct_contract), b"")

        # The baseline contract has the same runtime code prefix and only
        # stops instead of self destructing.
        used_baseline = self.send_tx(to=self.selfdestruct_baseline_contract)["gasUsed"]

        # `SELFDESTRUCT` costs 5000 gas (the beneficiary is the sender, which
        # is warm and exists, so there is neither a cold surcharge nor a new
        # account surcharge) and EIP-3529 removed the 24000 gas refund.
        assert_equal(used_selfdestruct - used_baseline, 5000)

    def test_eip3541_rejects_ef_prefix(self):
        """Runtime code starting with 0xEF must not be deployed (EIP-3541)."""
        self.rollback_to(self.start_height)

        nonce = self.w3.eth.get_transaction_count(self.sender)
        expected_address = get_create_address(self.sender, nonce)

        receipt = self.send_tx(to=None, data=build_init_code(RUNTIME_EOF_MAGIC))
        assert_equal(receipt["status"], 0)

        # No code was deployed and the nonce was consumed.  The node reports
        # the create address in the receipt of a failed deployment as well
        # (on Ethereum mainnet the field is null for a failed deployment),
        # but the address stays empty.
        assert_equal(receipt["contractAddress"], expected_address)
        assert_equal(self.w3.eth.get_code(expected_address), b"")
        assert_equal(self.w3.eth.get_transaction_count(self.sender), nonce + 1)

    def test_invalid_opcode_stack_overflow_out_of_gas(self):
        """Invalid opcode, stack overflow and out of gas fail without changes."""
        self.rollback_to(self.start_height)

        height = self.nodes[0].getblockcount()

        # Invalid opcode: the storage write before the invalid opcode has to
        # be rolled back.
        receipt = self.send_tx(to=self.invalid_contract, gas=FAILURE_GAS)
        assert_equal(receipt["status"], 0)
        assert_equal(self.storage_slot(self.invalid_contract), 0)

        # The invalid opcode consumes the whole gas of the frame.
        assert_equal(receipt["gasUsed"], FAILURE_GAS)

        # The failure mode is observable: the invalid opcode 0xFE aborts the
        # execution.  The node reports it as `InvalidCode(Opcode(254))`,
        # because the runtime dispatches every opcode it does not implement
        # to the handler, which rejects it with `ExitError::InvalidCode`.
        message = self.eth_call_error(self.invalid_contract)
        assert "exit error InvalidCode(Opcode(254))" in message, message

        # A contract that consists of the invalid opcode only fails as well.
        receipt = self.send_tx(to=self.invalid_opcode_contract, gas=FAILURE_GAS)
        assert_equal(receipt["status"], 0)
        assert_equal(receipt["gasUsed"], FAILURE_GAS)
        # The failure mode is observable as well and is the same as above:
        # the invalid opcode is rejected with `ExitError::InvalidCode`.
        message = self.eth_call_error(self.invalid_opcode_contract)
        assert "exit error InvalidCode(Opcode(254))" in message, message

        # Stack overflow (the stack limit is 1024 items, the runtime code
        # pushes 1025 values): the storage write before the overflow has to
        # be rolled back.
        receipt = self.send_tx(to=self.stack_overflow_contract, gas=FAILURE_GAS)
        assert_equal(receipt["status"], 0)
        assert_equal(self.storage_slot(self.stack_overflow_contract), 0)
        assert_equal(receipt["gasUsed"], FAILURE_GAS)

        # The failure mode is observable: the stack limit is enforced.
        assert "exit error StackOverflow" in self.eth_call_error(
            self.stack_overflow_contract
        )

        # Out of gas: the storage write before the endless loop has to be
        # rolled back and the whole gas limit is consumed.
        receipt = self.send_tx(to=self.out_of_gas_contract, gas=FAILURE_GAS)
        assert_equal(receipt["status"], 0)
        assert_equal(self.storage_slot(self.out_of_gas_contract), 0)
        assert_equal(receipt["gasUsed"], FAILURE_GAS)

        # The failure mode is observable: the endless loop runs out of gas.
        assert "exit error OutOfGas" in self.eth_call_error(self.out_of_gas_contract)

        # The chain is still valid and produces blocks.
        self.nodes[0].generate(1)
        assert self.nodes[0].getblockcount() > height

    def test_memory_expansion_gas_cost(self):
        """The memory expansion follows the EVM cost formula.

        The memory cost is ``3 * words + words * words / 512``:

          * offset 0x0000: 1 word  -> 3 * 1 + 1 / 512 = 3 gas,
          * offset 0x1000: 129 words (0x1000 + 32 bytes) ->
            3 * 129 + 129 * 129 / 512 = 387 + 32 = 419 gas.

        The difference of the two runs is therefore 419 - 3 = 416 gas.
        """
        self.rollback_to(self.start_height)

        used_low = self.send_tx(to=self.mstore_low_contract)["gasUsed"]
        used_high = self.send_tx(to=self.mstore_high_contract)["gasUsed"]

        assert_equal(used_high - used_low, 416)

    def run_test(self):
        self.setup()

        self.test_create2_deterministic_address()
        self.test_create2_salt_collision()
        self.test_revert_reason_and_state_rollback()
        self.test_gas_refund_clearing_storage_slot()
        self.test_gas_refund_cap()
        self.test_selfdestruct_gives_no_refund()
        self.test_eip3541_rejects_ef_prefix()
        self.test_invalid_opcode_stack_overflow_out_of_gas()
        self.test_memory_expansion_gas_cost()


if __name__ == "__main__":
    EVMOpcodeTest().main()
