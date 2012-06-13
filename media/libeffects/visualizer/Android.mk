LOCAL_PATH:= $(call my-dir)

# Visualizer library
include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
	EffectVisualizer.cpp

LOCAL_CFLAGS+= -O2

LOCAL_CFLAGS+= -fno-strict-aliasing

LOCAL_SHARED_LIBRARIES := \
	libcutils \
	libdl

LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/soundfx
LOCAL_MODULE:= libvisualizer

LOCAL_C_INCLUDES := \
	$(call include-path-for, graphics corecg) \
	system/media/audio_effects/include


include $(BUILD_SHARED_LIBRARY)
