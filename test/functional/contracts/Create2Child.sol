// SPDX-License-Identifier: GPL-3.0

pragma solidity ^0.8.0;

// Child contract used by the CREATE2 scenario of feature_evm_opcodes.py.
// The constructor takes no arguments so that the creation (init) code is
// fully deterministic and the CREATE2 address can be computed independently
// in the test.
contract Create2Child {
    function value() public pure returns (uint256) {
        return 42;
    }
}
