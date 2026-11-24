package=solc
$(package)_version=0.8.28
$(package)_download_path=https://github.com/ethereum/solidity/releases/download/v$($(package)_version)/

# NOTE: solc gets invoked on the BUILD OS during compile. So, we don't
# care about HOST OS, unlike all other dependencies.
# That is: solc is run on the BUILD OS, not targeted to run on the
# HOST OS.

ifeq ($(build_os)-$(build_arch),linux-x86_64)
  $(package)_file_name=solc-static-linux
  $(package)_sha256_hash=9a0fb7e0db2c0641dbae1c5cc645dc686820c83af516226abb1c0a2f76636f25
endif

# Note: solc only provides binaries for darwin amd64, however since rosetta is
# expected to be present, this should work through emulation without additional config.
ifeq ($(build_os),darwin)
  $(package)_file_name=solc-macos
  $(package)_sha256_hash=81515b0e53deaa266d549545ccaac0a5a96e6d4e8201c77f673b2c710976d9ea
endif

ifeq ($($(package)_file_name),)
    $(error Unsupported build platform: $(BUILD))
endif

define $(package)_extract_cmds
  mkdir -p $$($(package)_extract_dir) && \
  echo "$$($(package)_sha256_hash)  $$($(package)_source)" > $$($(package)_extract_dir)/.$$($(package)_file_name).hash && \
  $(build_SHA256SUM) -c $$($(package)_extract_dir)/.$$($(package)_file_name).hash
endef

define $(package)_set_vars
  $(package)_ROOT="$($(package)_staging_dir)/$(host_prefix)/bin"
endef

define $(package)_build_cmds
  mkdir -p $($(package)_ROOT) && \
  cp $($(package)_source) $($(package)_ROOT)/$(package) && \
  chmod +x $($(package)_ROOT)/$(package)
endef
