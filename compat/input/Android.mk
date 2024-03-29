LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
include $(LOCAL_PATH)/../Android.common.mk

HYBRIS_PATH := $(LOCAL_PATH)/../../hybris

LOCAL_CFLAGS += -std=gnu++0x

LOCAL_SRC_FILES:= input_compatibility_layer.cpp

LOCAL_MODULE:= libis_compat_layer
LOCAL_MODULE_TAGS := optional

LOCAL_SHARED_LIBRARIES := \
	libinput \
	libcutils \
	libutils \
	libgui \
	libandroidfw

ifeq ($(shell test $(ANDROID_VERSION_MAJOR) -ge 10 && echo true),true)
static_libraries := \
	libskia
else
LOCAL_SHARED_LIBRARIES += \
	libskia
endif

LOCAL_C_INCLUDES := \
	$(HYBRIS_PATH)/include \
	external/skia/include/core

HAS_LIBINPUTSERVICE := $(shell test $(ANDROID_VERSION_MAJOR) -eq 4 -a $(ANDROID_VERSION_MINOR) -gt 2 && echo true)
ifeq ($(HAS_LIBINPUTSERVICE),true)
LOCAL_SHARED_LIBRARIES += libinputservice
LOCAL_C_INCLUDES += frameworks/base/services/input
endif

HAS_LIBINPUTFLINGER := $(shell test $(ANDROID_VERSION_MAJOR) -ge 5 && echo true)
ifeq ($(HAS_LIBINPUTFLINGER),true)
LOCAL_SHARED_LIBRARIES += libinputflinger libinputservice
LOCAL_C_INCLUDES += \
	frameworks/base/libs/input \
	frameworks/native/services
endif




include $(BUILD_SHARED_LIBRARY)

include $(CLEAR_VARS)

HYBRIS_PATH := $(LOCAL_PATH)/../../hybris

LOCAL_CFLAGS += -std=gnu++0x

LOCAL_SRC_FILES:= \
	direct_input_test.cpp

LOCAL_MODULE:= direct_input_test
LOCAL_MODULE_TAGS := optional
ifdef TARGET_2ND_ARCH
LOCAL_MULTILIB := both
LOCAL_MODULE_STEM_32 := $(if $(filter false,$(BOARD_UBUNTU_PREFER_32_BIT)),$(LOCAL_MODULE)$(TARGET_2ND_ARCH_MODULE_SUFFIX),$(LOCAL_MODULE))
LOCAL_MODULE_STEM_64 := $(if $(filter false,$(BOARD_UBUNTU_PREFER_32_BIT)),$(LOCAL_MODULE),$(LOCAL_MODULE)_64)
endif

LOCAL_C_INCLUDES := \
	$(HYBRIS_PATH)/include \
	bionic \
	external/libcxx/include \
	external/gtest/include \
	external/skia/include/core

LOCAL_SHARED_LIBRARIES := \
	libis_compat_layer \
	libcutils \
	libutils \
	libgui \
	libandroidfw

static_libraries := \
	libgtest \
	libgtest_main


ifeq ($(shell test $(ANDROID_VERSION_MAJOR) -ge 10 && echo true),true)
static_libraries += \
	libskia
else
LOCAL_SHARED_LIBRARIES += \
	libskia
endif

include $(BUILD_EXECUTABLE)
