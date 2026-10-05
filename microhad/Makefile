# The gateway is an old big-endian MIPS/uClibc system.  Never substitute a
# host compiler for the target build: a host executable is not deployable.
RSDK_ROOT := ../rsdk-4.4.7-4181-EB-2.6.30-0.9.30-m32u-140129
TARGET_CXX := $(RSDK_ROOT)/bin/mips-linux-g++
TARGET_CC := $(RSDK_ROOT)/bin/mips-linux-gcc
TARGET_AR := $(RSDK_ROOT)/bin/mips-linux-ar
TARGET_STRIP := $(RSDK_ROOT)/bin/mips-linux-strip
TARGET_READELF := $(RSDK_ROOT)/bin/mips-linux-readelf
HOST_CXX ?= g++
HOST_CC ?= gcc
HOST_READELF ?= readelf
CMAKE ?= cmake
BUILD_REVISION := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)

ZIGGURAT ?= 0
ifneq ($(words $(ZIGGURAT)),1)
$(error ZIGGURAT must be 0 or 1)
endif
ifneq ($(filter 0 1,$(ZIGGURAT)),$(ZIGGURAT))
$(error ZIGGURAT must be 0 or 1)
endif
MATTER ?= 0
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
MATTER_PLATFORM_CHECK ?= ../ziggurat-compilation/scripts/check-matter-platform.py
MATTER_SDK_ROOT ?= ../ziggurat-compilation/build/matter-sdk/source
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
# GCC 3.4 in the supplied RSDK has no -std=gnu++03 switch; its default is
# C++98/C++03-compatible. Keep target flags explicit and separate from host.
HOST_CXXFLAGS := -std=gnu++03 $(WARNINGS) -Iinclude -Ithird_party/mbedtls/include -O2 -DMICROHAD_ZIGGURAT_ENABLED=$(ZIGGURAT) -DMICROHAD_MBEDTLS_ENABLED=1 -DMICROHAD_BUILD_REVISION=\"$(BUILD_REVISION)\"
TARGET_CXXFLAGS := $(WARNINGS) -Iinclude -Ithird_party/mbedtls/include -O2 -ffunction-sections -fdata-sections -DMICROHAD_MIPS_TARGET=1 -DMICROHAD_ZIGGURAT_ENABLED=$(ZIGGURAT) -DMICROHAD_MBEDTLS_ENABLED=1 -DMICROHAD_BUILD_REVISION=\"$(BUILD_REVISION)\"
HOST_CXXFLAGS += -DMICROHAD_MATTER_ENABLED=$(MATTER)
TARGET_CXXFLAGS += -DMICROHAD_MATTER_ENABLED=$(MATTER)
TARGET_CFLAGS := $(WARNINGS) -O2 -ffunction-sections -fdata-sections
LDFLAGS := -lpthread -lrt
SOURCES := src/main.cpp src/matter_websocket.cpp src/matter_protocol.cpp src/matter_client.cpp src/matter_worker.cpp src/gateway_config.cpp src/ash.cpp src/json_util.cpp src/storage.cpp src/network_backup.cpp $(ZIGGURAT_SOURCES) src/http_server.cpp src/gateway.cpp src/gateway_runner.cpp src/notification_worker.cpp src/zcl.cpp src/sensor_history.cpp src/log_ring.cpp src/fcm.cpp src/https_client.cpp src/web_push.cpp src/external_http_device.cpp src/device_shelly.cpp src/device_fritz_box.cpp
HOST_BUILD := build/host$(BACKEND_SUFFIX)
MIPS_BUILD := build/mips$(BACKEND_SUFFIX)
HOST_BIN := $(HOST_BUILD)/microhad-zigbee-native
MIPS_BIN := $(MIPS_BUILD)/microhad-zigbee-native
MIPS_MATTER_MANIFEST := $(MIPS_BIN).matter-manifest
HOST_CXXFLAGS += -DMICROHAD_TEST_BINARY=\"$(HOST_BIN)\"
TLS_C_SOURCES := src/fcm_signer.c src/https_client.c src/vapid_key.c src/web_push_crypto.c src/tls_server.c
HOST_C_OBJECTS := $(patsubst src/%.c,$(HOST_BUILD)/%.o,$(TLS_C_SOURCES))
MIPS_C_OBJECTS := $(patsubst src/%.c,$(MIPS_BUILD)/%.o,$(TLS_C_SOURCES))
MBEDTLS_DIR := third_party/mbedtls
HOST_MBEDTLS_BUILD := $(HOST_BUILD)/mbedtls
MIPS_MBEDTLS_BUILD := $(MIPS_BUILD)/mbedtls
HOST_MBEDTLS_LIBS := $(HOST_MBEDTLS_BUILD)/library/libmbedtls.a $(HOST_MBEDTLS_BUILD)/library/libmbedx509.a $(HOST_MBEDTLS_BUILD)/library/libmbedcrypto.a
MIPS_MBEDTLS_LIBS := $(MIPS_MBEDTLS_BUILD)/library/libmbedtls.a $(MIPS_MBEDTLS_BUILD)/library/libmbedx509.a $(MIPS_MBEDTLS_BUILD)/library/libmbedcrypto.a
UNIT_TEST_BIN := $(HOST_BUILD)/unit_tests
INTEGRATION_TEST_BIN := $(HOST_BUILD)/integration_tests
UNIT_TEST_SOURCES := $(ZIGGURAT_TEST_SOURCES) test/unit_matter_websocket.cpp test/unit_matter_protocol.cpp test/unit_matter_client.cpp test/unit_network_backup.cpp test/unit_gateway.cpp test/unit_gateway_config.cpp test/unit_fcm.cpp test/unit_web_push.cpp test/unit_sensor_history.cpp test/unit_log_ring.cpp test/unit_external_http_device.cpp test/test_main.cpp
INTEGRATION_TEST_SOURCES := test/integration_gateway.cpp test/integration_http_server.cpp test/integration_lifecycle.cpp test/integration_outbound.cpp test/integration_main.cpp

.PHONY: all host mips webui rng-seed mbedtls-host mbedtls-mips unit-test integration-test target-verifier-test test verify-target clean

all: host

host: mbedtls-host $(HOST_BIN)

webui:
	cd webui && npm ci && npm run build

$(HOST_BIN): $(SOURCES) $(HOST_C_OBJECTS) $(HOST_MBEDTLS_LIBS) $(wildcard include/*.h)
	@mkdir -p $(HOST_BUILD)
	$(HOST_CXX) $(HOST_CXXFLAGS) $(SOURCES) $(HOST_C_OBJECTS) -o $@ $(HOST_MBEDTLS_LIBS) $(LDFLAGS)

mips: mbedtls-mips $(MIPS_BIN) verify-target

rng-seed: build/rng-seed

build/rng-seed: src/rng_seed.c
	@test -x $(TARGET_CC) || (echo "RSDK compiler not found: $(TARGET_CC)"; exit 1)
	@mkdir -p build
	$(TARGET_CC) -std=gnu99 -Os -Wall -Wextra -Werror -static -msoft-float $< -o $@
	$(TARGET_STRIP) $@
ifeq ($(MATTER),1)
mips: $(MIPS_MATTER_MANIFEST)

$(MIPS_MATTER_MANIFEST): $(MIPS_BIN) Makefile
	@{ printf '%s\n' 'format=microhad-native-build-v1' 'ziggurat=1' 'matter=1'; \
	  printf 'binary_sha256='; sha256sum "$(MIPS_BIN)" | awk '{print $$1}'; } > "$@.tmp"
	@mv "$@.tmp" "$@"
endif

mbedtls-host: $(HOST_MBEDTLS_LIBS)

$(HOST_MBEDTLS_LIBS): $(MBEDTLS_DIR)/CMakeLists.txt $(MBEDTLS_DIR)/include/mbedtls/config.h $(MBEDTLS_DIR)/include/mbedtls/microhad_config.h
	$(CMAKE) -S $(MBEDTLS_DIR) -B $(HOST_MBEDTLS_BUILD) -DCMAKE_C_COMPILER=$(HOST_CC) -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DMBEDTLS_FATAL_WARNINGS=OFF
	$(CMAKE) --build $(HOST_MBEDTLS_BUILD) --target mbedtls mbedx509 mbedcrypto

mbedtls-mips: $(MIPS_MBEDTLS_LIBS)

$(MIPS_MBEDTLS_LIBS): $(MBEDTLS_DIR)/CMakeLists.txt $(MBEDTLS_DIR)/include/mbedtls/config.h $(MBEDTLS_DIR)/include/mbedtls/microhad_config.h
	@test -x $(TARGET_CC) || (echo "RSDK compiler not found: $(TARGET_CC)"; exit 1)
	$(CMAKE) -S $(MBEDTLS_DIR) -B $(MIPS_MBEDTLS_BUILD) -DCMAKE_C_COMPILER=$(TARGET_CC) -DCMAKE_AR=$(TARGET_AR) -DCMAKE_C_FLAGS='-O2 -std=gnu99 -ffunction-sections -fdata-sections' -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DMBEDTLS_FATAL_WARNINGS=OFF
	$(CMAKE) --build $(MIPS_MBEDTLS_BUILD) --target mbedtls mbedx509 mbedcrypto

$(HOST_BUILD)/%.o: src/%.c $(HOST_MBEDTLS_LIBS)
	@mkdir -p $(HOST_BUILD)
	$(HOST_CC) -std=gnu99 $(WARNINGS) -O2 -I$(MBEDTLS_DIR)/include -c $< -o $@

$(MIPS_BUILD)/%.o: src/%.c $(MIPS_MBEDTLS_LIBS)
	@mkdir -p $(MIPS_BUILD)
	$(TARGET_CC) -std=gnu99 $(TARGET_CFLAGS) -I$(MBEDTLS_DIR)/include -c $< -o $@

$(MIPS_BIN): $(SOURCES) $(wildcard include/*.h) $(MIPS_MBEDTLS_LIBS) $(MIPS_C_OBJECTS)
	@test -x $(TARGET_CXX) || (echo "RSDK compiler not found: $(TARGET_CXX)"; exit 1)
	@mkdir -p $(MIPS_BUILD)
	$(TARGET_CXX) $(TARGET_CXXFLAGS) -static -Wl,--gc-sections $(SOURCES) $(MIPS_C_OBJECTS) -o $@ $(MIPS_MBEDTLS_LIBS) $(LDFLAGS)
	$(TARGET_STRIP) $@

# Recompile after changing feature definitions or build recipes.
$(HOST_BIN) $(MIPS_BIN) $(UNIT_TEST_BIN) $(INTEGRATION_TEST_BIN): Makefile

define verify_elf
$(1) -h $(2) | grep -q 'Data:.*big endian' && \
$(1) -h $(2) | grep -q 'Machine:.*MIPS'
endef

verify-target: $(MIPS_BIN)
	@$(call verify_elf,$(TARGET_READELF),$(MIPS_BIN))

$(UNIT_TEST_BIN): $(UNIT_TEST_SOURCES) src/matter_websocket.cpp src/matter_protocol.cpp src/matter_client.cpp src/matter_worker.cpp src/gateway_config.cpp src/ash.cpp src/json_util.cpp src/storage.cpp src/network_backup.cpp $(ZIGGURAT_SOURCES) src/gateway.cpp src/gateway_runner.cpp src/notification_worker.cpp src/zcl.cpp src/sensor_history.cpp src/log_ring.cpp src/fcm.cpp src/https_client.cpp src/web_push.cpp src/external_http_device.cpp src/device_shelly.cpp src/device_fritz_box.cpp $(HOST_C_OBJECTS) $(HOST_MBEDTLS_LIBS) $(wildcard include/*.h)
	@mkdir -p $(HOST_BUILD)
	$(HOST_CXX) $(HOST_CXXFLAGS) $(UNIT_TEST_SOURCES) src/matter_websocket.cpp src/matter_protocol.cpp src/matter_client.cpp src/matter_worker.cpp src/gateway_config.cpp src/ash.cpp src/json_util.cpp src/storage.cpp src/network_backup.cpp $(ZIGGURAT_SOURCES) src/gateway.cpp src/gateway_runner.cpp src/notification_worker.cpp src/zcl.cpp src/sensor_history.cpp src/log_ring.cpp src/fcm.cpp src/https_client.cpp src/web_push.cpp src/external_http_device.cpp src/device_shelly.cpp src/device_fritz_box.cpp $(HOST_C_OBJECTS) -o $@ $(HOST_MBEDTLS_LIBS) $(LDFLAGS)

$(INTEGRATION_TEST_BIN): $(INTEGRATION_TEST_SOURCES) src/matter_websocket.cpp src/matter_protocol.cpp src/matter_client.cpp src/matter_worker.cpp src/ash.cpp src/json_util.cpp src/storage.cpp src/network_backup.cpp $(ZIGGURAT_SOURCES) src/http_server.cpp src/gateway.cpp src/gateway_runner.cpp src/notification_worker.cpp src/zcl.cpp src/sensor_history.cpp src/log_ring.cpp src/fcm.cpp src/https_client.cpp src/web_push.cpp src/external_http_device.cpp src/device_shelly.cpp src/device_fritz_box.cpp $(HOST_C_OBJECTS) $(HOST_MBEDTLS_LIBS) $(wildcard include/*.h)
	@mkdir -p $(HOST_BUILD)
	$(HOST_CXX) $(HOST_CXXFLAGS) $(INTEGRATION_TEST_SOURCES) src/matter_websocket.cpp src/matter_protocol.cpp src/matter_client.cpp src/matter_worker.cpp src/ash.cpp src/json_util.cpp src/storage.cpp src/network_backup.cpp $(ZIGGURAT_SOURCES) src/http_server.cpp src/gateway.cpp src/gateway_runner.cpp src/notification_worker.cpp src/zcl.cpp src/sensor_history.cpp src/log_ring.cpp src/fcm.cpp src/https_client.cpp src/web_push.cpp src/external_http_device.cpp src/device_shelly.cpp src/device_fritz_box.cpp $(HOST_C_OBJECTS) -o $@ $(HOST_MBEDTLS_LIBS) $(LDFLAGS)

unit-test: mbedtls-host $(HOST_BIN) $(UNIT_TEST_BIN)
	$(UNIT_TEST_BIN)

integration-test: mbedtls-host $(HOST_BIN) $(INTEGRATION_TEST_BIN)
	$(INTEGRATION_TEST_BIN)
ifeq ($(ZIGGURAT),1)
	python3 scripts/test_ziggurat_native_peer.py
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
# controller service is built separately in ziggurat-compilation.
.PHONY: matter-platform-check matter-host-check matter-mips-check matter-build-flags-test
matter-platform-check:
	python3 "$(MATTER_PLATFORM_CHECK)" mips --sdk "$(MATTER_SDK_ROOT)" --rsdk "$(RSDK_ROOT)"

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
