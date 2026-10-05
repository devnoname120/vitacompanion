#pragma once

#include <stdint.h>

#define VITACOMPANION_KERNEL_ABI_VERSION 3
#define VITACOMPANION_KERNEL_MODULE_NAME "vitacompanion_kernel"
/* This syscall lost its fifth, stack-passed argument on device. */
#define VITACOMPANION_TOUCH_ACTIVE_FLAG 0x100

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pixelformat;
} vitacompanion_screen_info;

int vitaCompanionKernelGetApiVersion(void);
int vitaCompanionKernelSetButtons(uint32_t buttons, int pressed);
int vitaCompanionKernelSetAnalog(int stick, int x, int y, int active);
int vitaCompanionKernelSetTouch(int port, int slot_and_active, int x, int y);
int vitaCompanionKernelReset(void);
/*
 * Describes the displayed frame and returns its size in bytes, which is
 * width * height * 4. If dst holds that many bytes, the whole frame is copied
 * there right after a vblank so the capture does not tear.
 */
int vitaCompanionKernelScreenCapture(vitacompanion_screen_info *info,
    void *dst, uint32_t dst_size);
