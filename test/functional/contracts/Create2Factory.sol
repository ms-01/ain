// SPDX-License-Identifier: GPL-3.0

pragma solidity ^0.8.0;

// Factory used by the CREATE2 scenario of feature_evm_opcodes.py.
//
// `create2` returns the zero address when the deployment fails (for example
// because the salt is already in use) *without* reverting. The zero address
// is therefore converted into an explicit revert so that the failure is
// observable through the transaction receipt (status 0x0) and through the
// revert reason.
contract Create2Factory {
    function deploy(bytes32 salt, bytes memory initCode) public returns (address addr) {
        assembly {
            addr := create2(0, add(initCode, 0x20), mload(initCode), salt)
        }
        require(addr != address(0), "CREATE2_FAILED");
    }
}
