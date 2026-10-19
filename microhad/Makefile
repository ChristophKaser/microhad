# The gateway is an old big-endian MIPS/uClibc system. Never substitute a
# host compiler for the target build: a host executable is not deployable.
MICROHAD_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
ZIGGURAT_ROOT := $(abspath $(MICROHAD_ROOT)/..)
include $(ZIGGURAT_ROOT)/config/toolchains.mk
include $(ZIGGURAT_ROOT)/config/toolchain-select.mk
BUILD_REVISION := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)

ZIGGURAT ?= 1
ifneq ($(words $(ZIGGURAT)),1)
$(error ZIGGURAT must be 0 or 1)
endif
ifneq ($(filter 0 1,$(ZIGGURAT)),$(ZIGGURAT))
$(error ZIGGURAT must be 0 or 1)
endif
MATTER ?= $(if $(filter 0,$(ZIGGURAT)),0,1)
ifneq ($(words $(MATTER)),1)
$(error MATTER must be 0 or 1)
endif
ifneq ($(filter 0 1,$(MATTER)),$(MATTER))
$(error MATTER must be 0 or 1)
endif
ifeq ($(MATTER),1)
ifneq ($(ZIGGURAT),1)
$(error MATTER=1 requires ZIGGURAT=1 for the shared Thread radio)
endif
endif
ZIGGURAT_SOURCES :=
ZIGGURAT_TEST_SOURCES :=
ifeq ($(ZIGGURAT),1)
ZIGGURAT_SOURCES := src/ziggurat_client.cpp
ZIGGURAT_TEST_SOURCES := test/unit_ziggurat.cpp
BACKEND_SUFFIX := -ziggurat
endif
ifeq ($(MATTER),1)
BACKEND_SUFFIX := $(BACKEND_SUFFIX)-matter
endif

WARNINGS := -Wall -Wextra -Werror -pedantic
TARGET_OPTIMIZATION ?= $(if $(filter crosstool-ng,$(TOOLCHAIN)),-Os,-O2)
# Both target compilers support the C++98 dialect used by this C++03 daemon.
HOST_CXXFLAGS = -std=gnu++03 $(WARNINGS) -Iinclude $(TLS_CPPFLAGS) -O2 -DMICROHAD_ZIGGURAT_ENABLED=$(ZIGGURAT) -DMICROHAD_MBEDTLS_ENABLED=1 -DMICROHAD_BUILD_REVISION=\"$(BUILD_REVISION)\"
TARGET_CXXFLAGS = -std=gnu++98 $(WARNINGS) -Iinclude $(TLS_CPPFLAGS) $(TARGET_OPTIMIZATION) -ffunction-sections -fdata-sections -DMICROHAD_MIPS_TARGET=1 -DMICROHAD_ZIGGURAT_ENABLED=$(ZIGGURAT) -DMICROHAD_MBEDTLS_ENABLED=1 -DMICROHAD_BUILD_REVISION=\"$(BUILD_REVISION)\"
HOST_CXXFLAGS += -DMICROHAD_MATTER_ENABLED=$(MATTER)
TARGET_CXXFLAGS += -DMICROHAD_MATTER_ENABLED=$(MATTER)
TARGET_CFLAGS := $(WARNINGS) $(TARGET_OPTIMIZATION) -ffunction-sections -fdata-sections
TARGET_LINK_FLAGS := $(if $(filter dynamic,$(LINK_MODE)),,-static)
LDFLAGS := -lpthread -lrt
SOURCES := src/main.cpp src/matter_websocket.cpp src/matter_protocol.cpp src/matter_cbor.cpp src/matter_client.cpp src/matter_worker.cpp src/gateway_config.cpp src/companion_discovery.cpp src/ash.cpp src/json_util.cpp src/storage.cpp src/network_backup.cpp $(ZIGGURAT_SOURCES) src/http_server.cpp src/gateway.cpp src/gateway_runner.cpp src/notification_worker.cpp src/zcl.cpp src/sensor_history.cpp src/syslog_log_reader.cpp src/fcm.cpp src/https_client.cpp src/web_push.cpp src/external_http_device.cpp src/device_shelly.cpp src/device_fritz_box.cpp
HOST_BUILD := build/host$(BACKEND_SUFFIX)
MIPS_BUILD := build/$(TARGET_BUILD_SUBDIR)mips$(BACKEND_SUFFIX)
HOST_BIN := $(HOST_BUILD)/microhad
HOST_TEST_BIN := $(HOST_BUILD)/microhad-test
MIPS_BIN := $(MIPS_BUILD)/microhad
MIPS_MATTER_MANIFEST := $(MIPS_BIN).matter-manifest
HOST_CXXFLAGS += -DMICROHAD_TEST_BINARY=\"$(HOST_BIN)\"
TLS_C_SOURCES := src/crypto_init.c src/fcm_signer.c src/https_client.c src/vapid_key.c src/web_push_crypto.c src/tls_server.c
HOST_C_OBJECTS := $(patsubst src/%.c,$(HOST_BUILD)/%.o,$(TLS_C_SOURCES))
MIPS_C_OBJECTS := $(patsubst src/%.c,$(MIPS_BUILD)/%.o,$(TLS_C_SOURCES))
MBEDTLS_DIR := dependencies/mbedtls
TLS_CONFIG := $(abspath config/mbedtls_user_config.h)
PSA_CONFIG := $(abspath config/psa_crypto_config.h)
TLS_CPPFLAGS = -isystem $(MBEDTLS_DIR)/include -isystem $(MBEDTLS_DIR)/tf-psa-crypto/include -isystem $(MBEDTLS_DIR)/tf-psa-crypto/drivers/builtin/include -DMBEDTLS_USER_CONFIG_FILE='"$(TLS_CONFIG)"' -DTF_PSA_CRYPTO_CONFIG_FILE='"$(PSA_CONFIG)"'
TLS_CMAKE_FLAGS := -DMBEDTLS_USER_CONFIG_FILE=$(TLS_CONFIG) -DTF_PSA_CRYPTO_CONFIG_FILE=$(PSA_CONFIG) -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DMBEDTLS_FATAL_WARNINGS=OFF
# Submodule HEAD changes must invalidate the libraries even when upstream
# preserves timestamps on individual source files. All these checkouts are pinned.
TLS_SUBMODULE_HEADS := $(foreach dep,$(MBEDTLS_DIR) $(MBEDTLS_DIR)/tf-psa-crypto $(MBEDTLS_DIR)/framework $(MBEDTLS_DIR)/tf-psa-crypto/framework,$(wildcard $(shell git -C $(dep) rev-parse --absolute-git-dir 2>/dev/null)/HEAD))
TLS_DEPENDENCIES := $(TLS_SUBMODULE_HEADS) Makefile $(TLS_CONFIG) $(PSA_CONFIG) $(MBEDTLS_DIR)/CMakeLists.txt $(MBEDTLS_DIR)/tf-psa-crypto/CMakeLists.txt
HOST_MBEDTLS_BUILD := $(HOST_BUILD)/mbedtls-v4
MIPS_MBEDTLS_BUILD := $(MIPS_BUILD)/mbedtls-v4
HOST_MBEDTLS_LIBS := $(HOST_MBEDTLS_BUILD)/library/libmbedtls.a $(HOST_MBEDTLS_BUILD)/library/libmbedx509.a $(HOST_MBEDTLS_BUILD)/library/libtfpsacrypto.a
MIPS_MBEDTLS_LIBS := $(MIPS_MBEDTLS_BUILD)/library/libmbedtls.a $(MIPS_MBEDTLS_BUILD)/library/libmbedx509.a $(MIPS_MBEDTLS_BUILD)/library/libtfpsacrypto.a
UNIT_TEST_BIN := $(HOST_BUILD)/unit_tests
INTEGRATION_TEST_BIN := $(HOST_BUILD)/integration_tests
UNIT_TEST_SOURCES := $(ZIGGURAT_TEST_SOURCES) test/unit_matter_websocket.cpp test/unit_matter_protocol.cpp test/unit_matter_client.cpp test/unit_network_backup.cpp test/unit_gateway.cpp test/unit_gateway_config.cpp test/unit_companion_discovery.cpp test/unit_fcm.cpp test/unit_web_push.cpp test/unit_sensor_history.cpp test/unit_log_ring.cpp test/unit_syslog_log_reader.cpp test/unit_external_http_device.cpp test/test_main.cpp
INTEGRATION_TEST_SOURCES := test/integration_gateway.cpp test/integration_http_server.cpp test/integration_lifecycle.cpp test/integration_outbound.cpp test/integration_main.cpp

.PHONY: all host mips webui rng-seed mbedtls-host mbedtls-mips unit-test integration-test target-verifier-test test verify-target clean

all: host

host: mbedtls-host $(HOST_BIN)

webui:
	cd webui && npm ci && npm run build

$(HOST_BIN): $(SOURCES) $(HOST_C_OBJECTS) $(HOST_MBEDTLS_LIBS) $(wildcard include/*.h)
	@mkdir -p $(HOST_BUILD)
	$(HOST_CXX) $(HOST_CXXFLAGS) $(SOURCES) $(HOST_C_OBJECTS) -o $@ $(HOST_MBEDTLS_LIBS) $(LDFLAGS)

$(HOST_TEST_BIN): $(SOURCES) $(HOST_C_OBJECTS) $(HOST_MBEDTLS_LIBS) $(wildcard include/*.h)
	@mkdir -p $(HOST_BUILD)
	$(HOST_CXX) $(HOST_CXXFLAGS) -DMICROHAD_TESTING=1 $(SOURCES) $(HOST_C_OBJECTS) -o $@ $(HOST_MBEDTLS_LIBS) $(LDFLAGS)

mips: mbedtls-mips $(MIPS_BIN) verify-target

rng-seed: build/$(TARGET_BUILD_SUBDIR)rng-seed

build/$(TARGET_BUILD_SUBDIR)rng-seed: src/rng_seed.c
	@test -x $(TARGET_CC) || (echo "Selected target compiler not found: $(TARGET_CC)"; exit 1)
	@mkdir -p $(dir $@)
	$(TARGET_CC) -std=gnu99 -Os -Wall -Wextra -Werror -static -msoft-float $< -o $@
	$(TARGET_STRIP) $@
	python3 $(ZIGGURAT_ROOT)/scripts/verify-gateway-target.py "$@"
ifeq ($(MATTER),1)
mips: $(MIPS_MATTER_MANIFEST)

$(MIPS_MATTER_MANIFEST): $(MIPS_BIN) Makefile
	@{ printf '%s\n' 'format=microhad-build-v1' 'ziggurat=1' 'matter=1'; \
	  printf 'binary_sha256='; sha256sum "$(MIPS_BIN)" | awk '{print $$1}'; } > "$@.tmp"
	@mv "$@.tmp" "$@"
endif

mbedtls-host: $(HOST_MBEDTLS_LIBS)

$(HOST_MBEDTLS_LIBS) &: $(TLS_DEPENDENCIES)
	$(CMAKE) -S $(MBEDTLS_DIR) -B $(HOST_MBEDTLS_BUILD) -DCMAKE_C_COMPILER=$(HOST_CC) $(TLS_CMAKE_FLAGS)
	$(CMAKE) --build $(HOST_MBEDTLS_BUILD) --target mbedtls mbedx509 tfpsacrypto
	@touch $(HOST_MBEDTLS_LIBS)

mbedtls-mips: $(MIPS_MBEDTLS_LIBS)

$(MIPS_MBEDTLS_LIBS) &: $(TLS_DEPENDENCIES)
	@test -x $(TARGET_CC) || (echo "Selected target compiler not found: $(TARGET_CC)"; exit 1)
	$(CMAKE) -S $(MBEDTLS_DIR) -B $(MIPS_MBEDTLS_BUILD) -DCMAKE_C_COMPILER=$(TARGET_CC) -DCMAKE_AR=$(TARGET_AR) -DCMAKE_C_FLAGS='$(TARGET_OPTIMIZATION) -std=gnu99 -ffunction-sections -fdata-sections' $(TLS_CMAKE_FLAGS)
	$(CMAKE) --build $(MIPS_MBEDTLS_BUILD) --target mbedtls mbedx509 tfpsacrypto
	@touch $(MIPS_MBEDTLS_LIBS)

$(HOST_BUILD)/%.o: src/%.c $(HOST_MBEDTLS_LIBS)
	@mkdir -p $(HOST_BUILD)
	$(HOST_CC) -std=gnu99 $(WARNINGS) -O2 $(TLS_CPPFLAGS) -c $< -o $@

$(MIPS_BUILD)/%.o: src/%.c $(MIPS_MBEDTLS_LIBS)
	@mkdir -p $(MIPS_BUILD)
	$(TARGET_CC) -std=gnu99 $(TARGET_CFLAGS) $(TLS_CPPFLAGS) -c $< -o $@

$(MIPS_BIN): $(SOURCES) $(wildcard include/*.h) $(MIPS_MBEDTLS_LIBS) $(MIPS_C_OBJECTS)
	@test -x $(TARGET_CXX) || (echo "Selected target compiler not found: $(TARGET_CXX)"; exit 1)
	@mkdir -p $(MIPS_BUILD)
	$(TARGET_CXX) $(TARGET_CXXFLAGS) $(TARGET_LINK_FLAGS) -Wl,--gc-sections $(SOURCES) $(MIPS_C_OBJECTS) -o $@ $(MIPS_MBEDTLS_LIBS) $(LDFLAGS)
	$(TARGET_STRIP) $@

# Recompile after changing feature definitions or build recipes.
$(HOST_BIN) $(MIPS_BIN) $(UNIT_TEST_BIN) $(INTEGRATION_TEST_BIN): Makefile

define verify_elf
$(1) -h $(2) | grep -q 'Data:.*big endian' && \
$(1) -h $(2) | grep -q 'Machine:.*MIPS'
endef

verify-target: $(MIPS_BIN)
	@python3 $(ZIGGURAT_ROOT)/scripts/verify-gateway-target.py --link-mode $(LINK_MODE) "$(MIPS_BIN)"

$(UNIT_TEST_BIN): $(UNIT_TEST_SOURCES) src/matter_websocket.cpp src/matter_protocol.cpp src/matter_cbor.cpp src/matter_client.cpp src/matter_worker.cpp src/gateway_config.cpp src/companion_discovery.cpp src/ash.cpp src/json_util.cpp src/storage.cpp src/network_backup.cpp $(ZIGGURAT_SOURCES) src/gateway.cpp src/gateway_runner.cpp src/notification_worker.cpp src/zcl.cpp src/sensor_history.cpp src/log_ring.cpp src/syslog_log_reader.cpp src/fcm.cpp src/https_client.cpp src/web_push.cpp src/external_http_device.cpp src/device_shelly.cpp src/device_fritz_box.cpp $(HOST_C_OBJECTS) $(HOST_MBEDTLS_LIBS) $(wildcard include/*.h)
	@mkdir -p $(HOST_BUILD)
	$(HOST_CXX) $(HOST_CXXFLAGS) -DMICROHAD_TESTING=1 $(UNIT_TEST_SOURCES) src/matter_websocket.cpp src/matter_protocol.cpp src/matter_cbor.cpp src/matter_client.cpp src/matter_worker.cpp src/gateway_config.cpp src/companion_discovery.cpp src/ash.cpp src/json_util.cpp src/storage.cpp src/network_backup.cpp $(ZIGGURAT_SOURCES) src/gateway.cpp src/gateway_runner.cpp src/notification_worker.cpp src/zcl.cpp src/sensor_history.cpp src/log_ring.cpp src/syslog_log_reader.cpp src/fcm.cpp src/https_client.cpp src/web_push.cpp src/external_http_device.cpp src/device_shelly.cpp src/device_fritz_box.cpp $(HOST_C_OBJECTS) -o $@ $(HOST_MBEDTLS_LIBS) $(LDFLAGS)

$(INTEGRATION_TEST_BIN): $(INTEGRATION_TEST_SOURCES) src/gateway_config.cpp src/companion_discovery.cpp src/matter_websocket.cpp src/matter_protocol.cpp src/matter_cbor.cpp src/matter_client.cpp src/matter_worker.cpp src/ash.cpp src/json_util.cpp src/storage.cpp src/network_backup.cpp $(ZIGGURAT_SOURCES) src/http_server.cpp src/gateway.cpp src/gateway_runner.cpp src/notification_worker.cpp src/zcl.cpp src/sensor_history.cpp src/log_ring.cpp src/syslog_log_reader.cpp src/fcm.cpp src/https_client.cpp src/web_push.cpp src/external_http_device.cpp src/device_shelly.cpp src/device_fritz_box.cpp $(HOST_C_OBJECTS) $(HOST_MBEDTLS_LIBS) $(wildcard include/*.h)
	@mkdir -p $(HOST_BUILD)
	$(HOST_CXX) $(HOST_CXXFLAGS) -DMICROHAD_TESTING=1 $(INTEGRATION_TEST_SOURCES) src/gateway_config.cpp src/companion_discovery.cpp src/matter_websocket.cpp src/matter_protocol.cpp src/matter_cbor.cpp src/matter_client.cpp src/matter_worker.cpp src/ash.cpp src/json_util.cpp src/storage.cpp src/network_backup.cpp $(ZIGGURAT_SOURCES) src/http_server.cpp src/gateway.cpp src/gateway_runner.cpp src/notification_worker.cpp src/zcl.cpp src/sensor_history.cpp src/log_ring.cpp src/syslog_log_reader.cpp src/fcm.cpp src/https_client.cpp src/web_push.cpp src/external_http_device.cpp src/device_shelly.cpp src/device_fritz_box.cpp $(HOST_C_OBJECTS) -o $@ $(HOST_MBEDTLS_LIBS) $(LDFLAGS)

unit-test: mbedtls-host $(HOST_BIN) $(UNIT_TEST_BIN)
	$(UNIT_TEST_BIN)

integration-test: mbedtls-host $(HOST_BIN) $(INTEGRATION_TEST_BIN) $(if $(filter 1,$(ZIGGURAT)),$(HOST_TEST_BIN))
	$(INTEGRATION_TEST_BIN)
ifeq ($(ZIGGURAT),1)
	MICROHAD_TEST_BINARY=$(abspath $(HOST_TEST_BIN)) python3 scripts/test_ziggurat_native_peer.py
endif

target-verifier-test: $(UNIT_TEST_BIN)
	@if $(call verify_elf,$(HOST_READELF),$(UNIT_TEST_BIN)) >/dev/null 2>&1; then \
		echo "target verifier accepted the host executable"; exit 1; \
	else \
		echo "target verifier rejects the host executable"; \
	fi

test: unit-test integration-test target-verifier-test

clean:
	rm -rf build

.PHONY: test-backends
test-backends:
	$(MAKE) ZIGGURAT=0 MATTER=0 host test
	$(MAKE) ZIGGURAT=1 MATTER=0 host test

# The Matter native build is the gateway IPC/API bridge. The paired Rust
# controller service is built separately in micro-thread-matter-daemon/.
.PHONY: matter-host-check matter-mips-check matter-build-flags-test
ifeq ($(MATTER),1)
$(HOST_BIN) $(UNIT_TEST_BIN) $(INTEGRATION_TEST_BIN): | matter-host-check
$(MIPS_BIN): | matter-mips-check
matter-host-check:
	@echo 'Matter native IPC, commissioning, Thread, and device-control APIs enabled; build the paired Rust controller service separately. See MATTER_INTEGRATION.md.'
matter-mips-check:
	@echo 'Matter native IPC, commissioning, Thread, and device-control APIs enabled; build the paired Rust controller service separately. See MATTER_INTEGRATION.md.'
endif

matter-build-flags-test:
	python3 scripts/test_matter_build_flags.py

# Pinned TinyCBOR submodule, built as C99 with the legacy target compiler.
CBOR_DIR := dependencies/tinycbor/src
CBOR_PLATFORM := include/matter_cbor_platform.h
CBOR_NAMES := cborencoder cborparser cborparser_float cborencoder_float
HOST_C_OBJECTS += $(addprefix $(HOST_BUILD)/tinycbor-,$(addsuffix .o,$(CBOR_NAMES)))
MIPS_C_OBJECTS += $(addprefix $(MIPS_BUILD)/tinycbor-,$(addsuffix .o,$(CBOR_NAMES)))
$(HOST_BUILD)/tinycbor-%.o: $(CBOR_DIR)/%.c $(wildcard $(CBOR_DIR)/*.h) $(CBOR_PLATFORM)
	@mkdir -p $(HOST_BUILD)
	$(HOST_CC) -std=gnu99 -O2 -include $(CBOR_PLATFORM) -I$(CBOR_DIR) -c $< -o $@
$(MIPS_BUILD)/tinycbor-%.o: $(CBOR_DIR)/%.c $(wildcard $(CBOR_DIR)/*.h) $(CBOR_PLATFORM)
	@mkdir -p $(MIPS_BUILD)
	$(TARGET_CC) -std=gnu99 $(TARGET_OPTIMIZATION) -ffunction-sections -fdata-sections -include $(CBOR_PLATFORM) -I$(CBOR_DIR) -c $< -o $@
$(HOST_BIN) $(HOST_TEST_BIN) $(UNIT_TEST_BIN) $(INTEGRATION_TEST_BIN): $(addprefix $(HOST_BUILD)/tinycbor-,$(addsuffix .o,$(CBOR_NAMES)))
$(MIPS_BIN): $(addprefix $(MIPS_BUILD)/tinycbor-,$(addsuffix .o,$(CBOR_NAMES)))

$(HOST_BIN) $(HOST_TEST_BIN) $(UNIT_TEST_BIN) $(INTEGRATION_TEST_BIN) $(MIPS_BIN): include/matter_wire_shapes.inc
$(UNIT_TEST_BIN): include/matter_wire_vectors.inc

# Keep compiler changes from reusing objects or CMake caches from another SDK.
ifneq ($(filter mips mbedtls-mips verify-target rng-seed $(MIPS_BIN),$(MAKECMDGOALS)),)
.PHONY: toolchain-force
toolchain-force:
$(MIPS_BUILD)/.toolchain: toolchain-force
	@python3 $(ZIGGURAT_ROOT)/scripts/toolchain-build.py "$(MICROHAD_ROOT)/$(MIPS_BUILD)"
$(MIPS_BIN) $(MIPS_C_OBJECTS) $(MIPS_MBEDTLS_LIBS) build/$(TARGET_BUILD_SUBDIR)rng-seed: $(MIPS_BUILD)/.toolchain
# Apply target optimization changes to cached C objects and TLS libraries.
$(MIPS_C_OBJECTS) $(MIPS_MBEDTLS_LIBS): Makefile
endif

$(MBEDTLS_DIR)/CMakeLists.txt $(MBEDTLS_DIR)/tf-psa-crypto/CMakeLists.txt:
	@echo "Initialize TLS dependencies: git submodule update --init --recursive microhad/dependencies/mbedtls"; exit 1

$(HOST_C_OBJECTS) $(MIPS_C_OBJECTS): Makefile

# Independent host verification; requires Python's cryptography package.
.PHONY: crypto-interop-test
$(HOST_BUILD)/crypto-interop: test/crypto_interop.c $(HOST_C_OBJECTS) $(HOST_MBEDTLS_LIBS) Makefile
	$(HOST_CC) -std=gnu99 $(WARNINGS) -O2 $< $(HOST_C_OBJECTS) -o $@ $(HOST_MBEDTLS_LIBS) $(LDFLAGS)

crypto-interop-test: $(HOST_BUILD)/crypto-interop
	python3 scripts/test_crypto_interop.py $(HOST_BUILD)/crypto-interop
