LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES := usbd.c

#LOCAL_CFLAGS := -g

LOCAL_MODULE := usbd
LOCAL_MODULE_TAGS := eng

LOCAL_STATIC_LIBRARIES := \
libm \
liblog \
libstdc++ \
libcutils \
libc

LOCAL_FORCE_STATIC_EXECUTABLE := false

include $(BUILD_EXECUTABLE)
