#pragma once

#include <stdint.h>

#define VITACOMPANION_KERNEL_ABI_VERSION 3
#define VITACOMPANION_KERNEL_MODULE_NAME "vitacompanion_kernel"
/* This syscall lost its fifth, stack-passed argument on device. */
#define VITACOMPANION_TOUCH_ACTIVE_FLAG 0x100

int vitaCompanionKernelGetApiVersion(void);
int vitaCompanionKernelSetButtons(uint32_t buttons, int pressed);
int vitaCompanionKernelSetAnalog(int stick, int x, int y, int active);
int vitaCompanionKernelSetTouch(int port, int slot_and_active, int x, int y);
int vitaCompanionKernelReset(void);
int vitaCompanionKernelRequestReboot(void);
int vitaCompanionKernelForceReboot(void);
