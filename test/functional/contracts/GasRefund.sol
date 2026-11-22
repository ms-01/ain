// SPDX-License-Identifier: GPL-3.0

pragma solidity ^0.8.0;

// Contract used by the gas refund scenarios of feature_evm_opcodes.py.
//
// The storage slots 0..9 hold the test values.  `clearOne()` and
// `clearAll()` are called once with a pre-set (non-zero) value and once
// with the slots zero already, so that the gas difference of the two runs
// is exactly the difference of the `SSTORE` costs.
contract GasRefund {
    uint256[10] public slots;

    function setOne(uint256 value) public {
        slots[0] = value;
    }

    function clearOne() public {
        slots[0] = 0;
    }

    function setAll(uint256 value) public {
        for (uint256 i = 0; i < slots.length; i++) {
            slots[i] = value;
        }
    }

    function clearAll() public {
        for (uint256 i = 0; i < slots.length; i++) {
            slots[i] = 0;
        }
    }
}
