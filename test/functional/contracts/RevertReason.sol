// SPDX-License-Identifier: GPL-3.0

pragma solidity ^0.8.0;

// Contract used by the revert scenario of feature_evm_opcodes.py.
//
// It covers the three revert encodings that the EVM has to produce:
//   * `Error(string)`      -> selector 0x08c379a0
//   * `Panic(uint256)`     -> selector 0x4e487b71
//   * custom error         -> selector of `CustomFailure(uint256)`
contract RevertReason {
    uint256 public value;

    error CustomFailure(uint256 code);

    // Writes to storage and reverts afterwards: the storage write has to be
    // rolled back (state rollback on revert).
    function setThenRevert(uint256 newValue) public {
        value = newValue;
        revert("Function reverted");
    }

    function revertWithCustomError() public pure {
        revert CustomFailure(42);
    }

    // Division by zero triggers `Panic(uint256)` with the code 0x12.
    function triggerPanic(uint256 divisor) public pure returns (uint256) {
        return 1 / divisor;
    }
}
