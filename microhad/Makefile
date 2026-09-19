# The gateway is an old big-endian MIPS/uClibc system.  Never substitute a
# host compiler for the target build: a host executable is not deployable.
RSDK_ROOT := ../rsdk-4.4.7-4181-EB-2.6.30-0.9.30-m32u-140129
TARGET_CXX := $(RSDK_ROOT)/bin/mips-linux-g++
TARGET_STRIP := $(RSDK_ROOT)/bin/mips-linux-strip
TARGET_READELF := $(RSDK_ROOT)/bin/mips-linux-readelf
HOST_CXX ?= g++

WARNINGS := -Wall -Wextra -Werror -pedantic
# GCC 3.4 in the supplied RSDK has no -std=gnu++03 switch; its default is
# C++98/C++03-compatible. Keep target flags explicit and separate from host.
HOST_CXXFLAGS := -std=gnu++03 $(WARNINGS) -Iinclude -O2
TARGET_CXXFLAGS := $(WARNINGS) -Iinclude -O2
LDFLAGS :=
SOURCES := src/main.cpp src/ash.cpp src/storage.cpp src/http_server.cpp src/gateway.cpp
HOST_BUILD := build/host
MIPS_BUILD := build/mips
HOST_BIN := $(HOST_BUILD)/microhad-zigbee-native
MIPS_BIN := $(MIPS_BUILD)/microhad-zigbee-native
TEST_BIN := $(HOST_BUILD)/unit_tests

.PHONY: all host mips test verify-target clean

all: host

host: $(HOST_BIN)

$(HOST_BIN): $(SOURCES) $(wildcard include/*.h)
	@mkdir -p $(HOST_BUILD)
	$(HOST_CXX) $(HOST_CXXFLAGS) $(SOURCES) -o $@ $(LDFLAGS)

mips: $(MIPS_BIN) verify-target

$(MIPS_BIN): $(SOURCES) $(wildcard include/*.h)
	@test -x $(TARGET_CXX) || (echo "RSDK compiler not found: $(TARGET_CXX)"; exit 1)
	@mkdir -p $(MIPS_BUILD)
	$(TARGET_CXX) $(TARGET_CXXFLAGS) -static $(SOURCES) -o $@
	$(TARGET_STRIP) $@

verify-target: $(MIPS_BIN)
	$(TARGET_READELF) -h $(MIPS_BIN) | grep -q 'Data:.*big endian'
	$(TARGET_READELF) -h $(MIPS_BIN) | grep -q 'Machine:.*MIPS'

$(TEST_BIN): test/unit_tests.cpp src/ash.cpp src/storage.cpp src/gateway.cpp $(wildcard include/*.h)
	@mkdir -p $(HOST_BUILD)
	$(HOST_CXX) $(HOST_CXXFLAGS) test/unit_tests.cpp src/ash.cpp src/storage.cpp src/gateway.cpp -o $@

test: $(TEST_BIN)
	$(TEST_BIN)

clean:
	rm -rf build
