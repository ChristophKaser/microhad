# The resolver is shared with shell/Python; Make command-line overrides win.
# Source trusted host settings through the shell loader; never include the env
# file as Make syntax or generate a Makefile containing credentials.
workspace_config_quote = '$(subst ','"'"',$(1))'
workspace_config_get = $(shell $(if $(filter undefined,$(origin $(1))),,$(1)=$(call workspace_config_quote,$($(1)))) sh "$(ZIGGURAT_ROOT)/scripts/workspace-env.sh" --get $(1) || printf workspace-config-error)
# Normalize even explicit/environment relative paths against the workspace,
# matching shell and Python. Their selected values still win over the env file.
override CROSSTOOL_NG_ROOT := $(call workspace_config_get,CROSSTOOL_NG_ROOT)
ifneq ($(filter workspace-config-error,$(CROSSTOOL_NG_ROOT)),)
$(error Cannot load workspace compiler settings)
endif
ifeq ($(strip $(CROSSTOOL_NG_ROOT)),)
$(error CROSSTOOL_NG_ROOT must not be empty)
endif
ifeq ($(origin LINK_MODE),undefined)
LINK_MODE := $(call workspace_config_get,LINK_MODE)
endif
ifneq ($(words $(LINK_MODE)),1)
$(error LINK_MODE must be static or dynamic)
endif
ifneq ($(filter static dynamic,$(LINK_MODE)),$(LINK_MODE))
$(error LINK_MODE must be static or dynamic)
endif
override STOCK_ROOTFS := $(call workspace_config_get,STOCK_ROOTFS)
override STOCK_LINK_DIR := $(call workspace_config_get,STOCK_LINK_DIR)
TARGET_PREFIX ?= $(CROSSTOOL_NG_PREFIX)
TARGET_TOOLCHAIN_ROOT := $(CROSSTOOL_NG_ROOT)
TARGET_BUILD_SUBDIR := crosstool-ng/$(if $(filter dynamic,$(LINK_MODE)),dynamic/)
TARGET_SPEC_NAME := lexra-crosstool-ng-uclibc$(if $(filter dynamic,$(LINK_MODE)),-dynamic)
TARGET_CC ?= $(TARGET_TOOLCHAIN_ROOT)/bin/$(TARGET_PREFIX)gcc
TARGET_CXX ?= $(TARGET_TOOLCHAIN_ROOT)/bin/$(TARGET_PREFIX)g++
TARGET_AR ?= $(TARGET_TOOLCHAIN_ROOT)/bin/$(TARGET_PREFIX)ar
TARGET_NM ?= $(TARGET_TOOLCHAIN_ROOT)/bin/$(TARGET_PREFIX)nm
TARGET_STRIP ?= $(TARGET_TOOLCHAIN_ROOT)/bin/$(TARGET_PREFIX)strip
TARGET_READELF ?= $(TARGET_TOOLCHAIN_ROOT)/bin/$(TARGET_PREFIX)readelf
export LINK_MODE CROSSTOOL_NG_ROOT STOCK_ROOTFS TARGET_PREFIX TARGET_CC TARGET_CXX TARGET_AR TARGET_NM TARGET_STRIP TARGET_READELF

ifeq ($(origin CRASH_SNAPSHOTS),undefined)
CRASH_SNAPSHOTS := $(call workspace_config_get,CRASH_SNAPSHOTS)
endif
ifneq ($(words $(CRASH_SNAPSHOTS)),1)
$(error CRASH_SNAPSHOTS must be true or false)
endif
ifneq ($(filter true false,$(CRASH_SNAPSHOTS)),$(CRASH_SNAPSHOTS))
$(error CRASH_SNAPSHOTS must be true or false)
endif
export CRASH_SNAPSHOTS

# Resolve local LLVM defaults and workspace overrides consistently with Python.
override CLANG := $(call workspace_config_get,CLANG)
override CLANGXX := $(call workspace_config_get,CLANGXX)
override LLVM_AR := $(call workspace_config_get,LLVM_AR)
override LLVM_STRIP := $(call workspace_config_get,LLVM_STRIP)
