// SPDX-License-Identifier: GPL-3.0

pragma solidity ^0.8.0;

// Contract used by the EIP-3529 dirty-slot gas refund scenarios of
// feature_evm_opcodes.py.
//
// Kept separate from `GasRefund.sol` so that existing deployed bytecode and
// gas delta assertions remain untouched.  Compiled without the optimizer so
// that both `SSTORE` instructions in `setThenClear()` and `modifyThenRestore()`
// are emitted without elision.
contract GasRefundDirty {
    uint256 public slot;

    function set(uint256 value) public {
        slot = value;
    }

    function setThenClear() public {
        slot = 1;
        slot = 0;
    }

    function modifyThenRestore() public {
        slot = 2;
        slot = 1;
    }
}
