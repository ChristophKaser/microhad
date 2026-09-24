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

WARNINGS := -Wall -Wextra -Werror -pedantic
# GCC 3.4 in the supplied RSDK has no -std=gnu++03 switch; its default is
# C++98/C++03-compatible. Keep target flags explicit and separate from host.
HOST_CXXFLAGS := -std=gnu++03 $(WARNINGS) -Iinclude -Ithird_party/mbedtls/include -O2 -DMICROHAD_MBEDTLS_ENABLED=1
TARGET_CXXFLAGS := $(WARNINGS) -Iinclude -O2 -ffunction-sections -fdata-sections -DMICROHAD_MIPS_TARGET=1 -DMICROHAD_MBEDTLS_ENABLED=1
TARGET_CFLAGS := $(WARNINGS) -O2 -ffunction-sections -fdata-sections
LDFLAGS := -lpthread
SOURCES := src/main.cpp src/gateway_config.cpp src/ash.cpp src/json_util.cpp src/storage.cpp src/http_server.cpp src/gateway.cpp src/gateway_runner.cpp src/notification_worker.cpp src/zcl.cpp src/fcm.cpp src/https_client.cpp src/web_push.cpp
HOST_BUILD := build/host
MIPS_BUILD := build/mips
HOST_BIN := $(HOST_BUILD)/microhad-zigbee-native
MIPS_BIN := $(MIPS_BUILD)/microhad-zigbee-native
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
UNIT_TEST_SOURCES := test/unit_gateway.cpp test/unit_gateway_config.cpp test/unit_fcm.cpp test/unit_web_push.cpp test/test_main.cpp
INTEGRATION_TEST_SOURCES := test/integration_gateway.cpp test/integration_http_server.cpp test/integration_lifecycle.cpp test/integration_main.cpp

.PHONY: all host mips mbedtls-host mbedtls-mips unit-test integration-test target-verifier-test test verify-target clean

all: host

host: mbedtls-host $(HOST_BIN)

$(HOST_BIN): $(SOURCES) $(HOST_C_OBJECTS) $(HOST_MBEDTLS_LIBS) $(wildcard include/*.h)
	@mkdir -p $(HOST_BUILD)
	$(HOST_CXX) $(HOST_CXXFLAGS) $(SOURCES) $(HOST_C_OBJECTS) -o $@ $(HOST_MBEDTLS_LIBS) $(LDFLAGS)

mips: mbedtls-mips $(MIPS_BIN) verify-target

mbedtls-host: $(HOST_MBEDTLS_LIBS)

$(HOST_MBEDTLS_LIBS): $(MBEDTLS_DIR)/CMakeLists.txt
	$(CMAKE) -S $(MBEDTLS_DIR) -B $(HOST_MBEDTLS_BUILD) -DCMAKE_C_COMPILER=$(HOST_CC) -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DMBEDTLS_FATAL_WARNINGS=OFF
	$(CMAKE) --build $(HOST_MBEDTLS_BUILD) --target mbedtls mbedx509 mbedcrypto

mbedtls-mips: $(MIPS_MBEDTLS_LIBS)

$(MIPS_MBEDTLS_LIBS): $(MBEDTLS_DIR)/CMakeLists.txt
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

verify-target: $(MIPS_BIN)
	$(TARGET_READELF) -h $(MIPS_BIN) | grep -q 'Data:.*big endian'
	$(TARGET_READELF) -h $(MIPS_BIN) | grep -q 'Machine:.*MIPS'

$(UNIT_TEST_BIN): $(UNIT_TEST_SOURCES) src/gateway_config.cpp src/ash.cpp src/json_util.cpp src/storage.cpp src/gateway.cpp src/notification_worker.cpp src/zcl.cpp src/fcm.cpp src/https_client.cpp src/web_push.cpp $(HOST_C_OBJECTS) $(HOST_MBEDTLS_LIBS) $(wildcard include/*.h)
	@mkdir -p $(HOST_BUILD)
	$(HOST_CXX) $(HOST_CXXFLAGS) $(UNIT_TEST_SOURCES) src/gateway_config.cpp src/ash.cpp src/json_util.cpp src/storage.cpp src/gateway.cpp src/notification_worker.cpp src/zcl.cpp src/fcm.cpp src/https_client.cpp src/web_push.cpp $(HOST_C_OBJECTS) -o $@ $(HOST_MBEDTLS_LIBS) $(LDFLAGS)

$(INTEGRATION_TEST_BIN): $(INTEGRATION_TEST_SOURCES) src/ash.cpp src/json_util.cpp src/storage.cpp src/http_server.cpp src/gateway.cpp src/gateway_runner.cpp src/notification_worker.cpp src/zcl.cpp src/fcm.cpp src/https_client.cpp src/web_push.cpp $(HOST_C_OBJECTS) $(HOST_MBEDTLS_LIBS) $(wildcard include/*.h)
	@mkdir -p $(HOST_BUILD)
	$(HOST_CXX) $(HOST_CXXFLAGS) $(INTEGRATION_TEST_SOURCES) src/ash.cpp src/json_util.cpp src/storage.cpp src/http_server.cpp src/gateway.cpp src/gateway_runner.cpp src/notification_worker.cpp src/zcl.cpp src/fcm.cpp src/https_client.cpp src/web_push.cpp $(HOST_C_OBJECTS) -o $@ $(HOST_MBEDTLS_LIBS) $(LDFLAGS)

unit-test: mbedtls-host $(UNIT_TEST_BIN)
	$(UNIT_TEST_BIN)

integration-test: mbedtls-host $(INTEGRATION_TEST_BIN)
	$(INTEGRATION_TEST_BIN)

target-verifier-test: $(HOST_BIN)
	@if $(MAKE) --no-print-directory verify-target MIPS_BIN=$(HOST_BIN) TARGET_READELF=$(HOST_READELF) >/dev/null 2>&1; then \
		echo "target verifier accepted the host executable"; exit 1; \
	else \
		echo "target verifier rejects the host executable"; \
	fi

test: unit-test integration-test target-verifier-test

clean:
	rm -rf build
