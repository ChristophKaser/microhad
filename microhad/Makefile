# The gateway is an old big-endian MIPS/uClibc system.  Never substitute a
# host compiler for the target build: a host executable is not deployable.
RSDK_ROOT := ../rsdk-4.4.7-4181-EB-2.6.30-0.9.30-m32u-140129
TARGET_CXX := $(RSDK_ROOT)/bin/mips-linux-g++
TARGET_CC := $(RSDK_ROOT)/bin/mips-linux-gcc
TARGET_AR := $(RSDK_ROOT)/bin/mips-linux-ar
TARGET_STRIP := $(RSDK_ROOT)/bin/mips-linux-strip
TARGET_READELF := $(RSDK_ROOT)/bin/mips-linux-readelf
HOST_CXX ?= g++

WARNINGS := -Wall -Wextra -Werror -pedantic
# GCC 3.4 in the supplied RSDK has no -std=gnu++03 switch; its default is
# C++98/C++03-compatible. Keep target flags explicit and separate from host.
HOST_CXXFLAGS := -std=gnu++03 $(WARNINGS) -Iinclude -O2
TARGET_CXXFLAGS := $(WARNINGS) -Iinclude -O2 -DMICROHAD_MIPS_TARGET=1
LDFLAGS :=
SOURCES := src/main.cpp src/ash.cpp src/storage.cpp src/http_server.cpp src/gateway.cpp src/zcl.cpp src/fcm.cpp src/https_client.cpp src/web_push.cpp
HOST_BUILD := build/host
MIPS_BUILD := build/mips
HOST_BIN := $(HOST_BUILD)/microhad-zigbee-native
MIPS_BIN := $(MIPS_BUILD)/microhad-zigbee-native
MIPS_C_OBJECTS := $(MIPS_BUILD)/fcm_signer.o $(MIPS_BUILD)/https_client.o $(MIPS_BUILD)/vapid_key.o $(MIPS_BUILD)/web_push_crypto.o $(MIPS_BUILD)/tls_server.o
MBEDTLS_DIR := third_party/mbedtls
MBEDTLS_LIBS := $(MBEDTLS_DIR)/library/libmbedtls.a $(MBEDTLS_DIR)/library/libmbedx509.a $(MBEDTLS_DIR)/library/libmbedcrypto.a
TEST_BIN := $(HOST_BUILD)/unit_tests

.PHONY: all host mips mbedtls-mips test verify-target clean

all: host

host: $(HOST_BIN)

$(HOST_BIN): $(SOURCES) $(wildcard include/*.h)
	@mkdir -p $(HOST_BUILD)
	$(HOST_CXX) $(HOST_CXXFLAGS) $(SOURCES) -o $@ $(LDFLAGS)

mips: mbedtls-mips $(MIPS_BIN) verify-target

mbedtls-mips:
	$(MAKE) -C $(MBEDTLS_DIR)/library clean
	$(MAKE) -C $(MBEDTLS_DIR)/library CC=$(TARGET_CC) AR=$(TARGET_AR) CFLAGS='-O2 -std=gnu99'

$(MIPS_BUILD)/fcm_signer.o: src/fcm_signer.c $(MBEDTLS_LIBS)
	@mkdir -p $(MIPS_BUILD)
	$(TARGET_CC) -std=gnu99 $(WARNINGS) -I$(MBEDTLS_DIR)/include -c $< -o $@

$(MIPS_BUILD)/https_client.o: src/https_client.c $(MBEDTLS_LIBS)
	@mkdir -p $(MIPS_BUILD)
	$(TARGET_CC) -std=gnu99 $(WARNINGS) -I$(MBEDTLS_DIR)/include -c $< -o $@

$(MIPS_BUILD)/vapid_key.o: src/vapid_key.c $(MBEDTLS_LIBS)
	@mkdir -p $(MIPS_BUILD)
	$(TARGET_CC) -std=gnu99 $(WARNINGS) -I$(MBEDTLS_DIR)/include -c $< -o $@

$(MIPS_BUILD)/web_push_crypto.o: src/web_push_crypto.c $(MBEDTLS_LIBS)
	@mkdir -p $(MIPS_BUILD)
	$(TARGET_CC) -std=gnu99 $(WARNINGS) -I$(MBEDTLS_DIR)/include -c $< -o $@

$(MIPS_BUILD)/tls_server.o: src/tls_server.c $(MBEDTLS_LIBS)
	@mkdir -p $(MIPS_BUILD)
	$(TARGET_CC) -std=gnu99 $(WARNINGS) -I$(MBEDTLS_DIR)/include -c $< -o $@

$(MIPS_BIN): $(SOURCES) $(wildcard include/*.h) $(MBEDTLS_LIBS) $(MIPS_C_OBJECTS)
	@test -x $(TARGET_CXX) || (echo "RSDK compiler not found: $(TARGET_CXX)"; exit 1)
	@mkdir -p $(MIPS_BUILD)
	$(TARGET_CXX) $(TARGET_CXXFLAGS) -static $(SOURCES) $(MIPS_C_OBJECTS) -o $@ $(MBEDTLS_LIBS)
	$(TARGET_STRIP) $@

verify-target: $(MIPS_BIN)
	$(TARGET_READELF) -h $(MIPS_BIN) | grep -q 'Data:.*big endian'
	$(TARGET_READELF) -h $(MIPS_BIN) | grep -q 'Machine:.*MIPS'

$(TEST_BIN): test/unit_tests.cpp src/ash.cpp src/storage.cpp src/gateway.cpp src/zcl.cpp src/fcm.cpp src/https_client.cpp src/web_push.cpp $(wildcard include/*.h)
	@mkdir -p $(HOST_BUILD)
	$(HOST_CXX) $(HOST_CXXFLAGS) test/unit_tests.cpp src/ash.cpp src/storage.cpp src/gateway.cpp src/zcl.cpp src/fcm.cpp src/https_client.cpp src/web_push.cpp -o $@

test: $(TEST_BIN)
	$(TEST_BIN)

clean:
	rm -rf build
