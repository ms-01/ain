package=protobuf
$(package)_version=27.3
$(package)_download_path=https://github.com/protocolbuffers/protobuf/releases/download/v$($(package)_version)/

# NOTE: protoc gets invoked on the BUILD OS during compile. So, we don't
# care about HOST OS, unlike all other dependencies. 
# That is: protoc is run on the BUILD OS, not targeted to run on the 
# HOST OS.

ifeq ($(build_os)-$(build_arch),linux-x86_64)
  $(package)_file_name=protoc-$($(package)_version)-linux-x86_64.zip
  $(package)_sha256_hash=6dab2adab83f915126cab53540d48957c40e9e9023969c3e84d44bfb936c7741
endif
ifeq ($(build_os)-$(build_arch),linux-aarch64)
  $(package)_file_name=protoc-$($(package)_version)-linux-aarch_64.zip
  $(package)_sha256_hash=bdad36f3ad7472281d90568c4956ea2e203c216e0de005c6bd486f1920f2751c
endif
ifeq ($(build_os),darwin)
  $(package)_file_name=protoc-$($(package)_version)-osx-universal_binary.zip
  $(package)_sha256_hash=414f95a8629e5e79c6e2f2aab476b0502c4e50e374657b6a7f880e54c11ed973
endif

ifeq ($($(package)_file_name),)
    $(error Unsupported build platform: $(BUILD))
endif

define $(package)_extract_cmds
  mkdir -p $$($(package)_extract_dir) && echo "$$($(package)_sha256_hash)  $$($(package)_source)" > $$($(package)_extract_dir)/.$$($(package)_file_name).hash &&  $(build_SHA256SUM) -c $$($(package)_extract_dir)/.$$($(package)_file_name).hash && unzip $$($(package)_source)
endef

define $(package)_set_vars
  $(package)_ROOT="$($(package)_staging_dir)/$(host_prefix)"
endef

define $(package)_preprocess_cmds
  rm -f readme.txt
endef

define $(package)_build_cmds
  mkdir -p $($(package)_ROOT) && \
  mv bin include $($(package)_ROOT)/
endef

