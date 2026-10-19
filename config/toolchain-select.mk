# The resolver is shared with shell/Python; Make command-line overrides win.
# Source trusted host settings through the shell loader; never include the env
# file as Make syntax or generate a Makefile containing credentials.
workspace_config_quote = '$(subst ','"'"',$(1))'
workspace_config_get = $(shell $(if $(filter undefined,$(origin $(1))),,$(1)=$(call workspace_config_quote,$($(1)))) sh "$(ZIGGURAT_ROOT)/scripts/workspace-env.sh" --get $(1) || printf workspace-config-error)
ifeq ($(origin TOOLCHAIN),undefined)
TOOLCHAIN := $(call workspace_config_get,TOOLCHAIN)
endif
ifneq ($(filter rsdk crosstool-ng,$(TOOLCHAIN)),$(TOOLCHAIN))
$(error TOOLCHAIN must be rsdk or crosstool-ng)
endif
ifneq ($(words $(TOOLCHAIN)),1)
$(error TOOLCHAIN must be rsdk or crosstool-ng)
endif
# Normalize even explicit/environment relative paths against the workspace,
# matching shell and Python. Their selected values still win over the env file.
override RSDK_ROOT := $(call workspace_config_get,RSDK_ROOT)
override CROSSTOOL_NG_ROOT := $(call workspace_config_get,CROSSTOOL_NG_ROOT)
ifneq ($(filter workspace-config-error,$(RSDK_ROOT) $(CROSSTOOL_NG_ROOT)),)
$(error Cannot load workspace compiler settings)
endif
ifeq ($(strip $(RSDK_ROOT)),)
$(error RSDK_ROOT must not be empty)
endif
ifeq ($(strip $(CROSSTOOL_NG_ROOT)),)
$(error CROSSTOOL_NG_ROOT must not be empty)
endif
TARGET_PREFIX ?= $(if $(filter rsdk,$(TOOLCHAIN)),$(RSDK_PREFIX),$(CROSSTOOL_NG_PREFIX))
TARGET_TOOLCHAIN_ROOT := $(if $(filter rsdk,$(TOOLCHAIN)),$(RSDK_ROOT),$(CROSSTOOL_NG_ROOT))
TARGET_BUILD_SUBDIR := $(if $(filter crosstool-ng,$(TOOLCHAIN)),crosstool-ng/)
TARGET_CC ?= $(TARGET_TOOLCHAIN_ROOT)/bin/$(TARGET_PREFIX)gcc
TARGET_CXX ?= $(TARGET_TOOLCHAIN_ROOT)/bin/$(TARGET_PREFIX)g++
TARGET_AR ?= $(TARGET_TOOLCHAIN_ROOT)/bin/$(TARGET_PREFIX)ar
TARGET_NM ?= $(TARGET_TOOLCHAIN_ROOT)/bin/$(TARGET_PREFIX)nm
TARGET_STRIP ?= $(TARGET_TOOLCHAIN_ROOT)/bin/$(TARGET_PREFIX)strip
TARGET_READELF ?= $(TARGET_TOOLCHAIN_ROOT)/bin/$(TARGET_PREFIX)readelf
export TOOLCHAIN RSDK_ROOT CROSSTOOL_NG_ROOT TARGET_PREFIX TARGET_CC TARGET_CXX TARGET_AR TARGET_NM TARGET_STRIP TARGET_READELF
