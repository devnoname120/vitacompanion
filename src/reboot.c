#include "reboot.h"

#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <stddef.h>
#include <stdint.h>
#include <taihen.h>

#define SHELL_UTIL_LIBRARY_NID 0xD2B1C8AE
#define SHELL_REQUEST_COLD_RESET_NID 0x636544FB
#define REBOOT_WATCHDOG_DELAY_US (5 * 1000 * 1000)

typedef int (*shell_request_cold_reset_fn)(int unknown);

static int reboot_watchdog_thread(unsigned int args, void *argp)
{
    (void)args;
    (void)argp;

    sceKernelDelayThread(REBOOT_WATCHDOG_DELAY_US);
    scePowerRequestColdReset();
    return sceKernelExitDeleteThread(0);
}

static int arm_reboot_watchdog(void)
{
    SceUID thread_id;
    int result;

    thread_id = sceKernelCreateThread("vitacompanion_reboot_watchdog",
        reboot_watchdog_thread, 0x40, 0x1000, 0, 0, NULL);
    if (thread_id < 0)
        return thread_id;

    result = sceKernelStartThread(thread_id, 0, NULL);
    if (result < 0)
    {
        sceKernelDeleteThread(thread_id);
        return result;
    }

    return 0;
}

int reboot_request(void)
{
    uintptr_t function = 0;
    int result;

    result = arm_reboot_watchdog();
    if (result < 0)
        return scePowerRequestColdReset();

    // VitaSDK does not expose the private Shell reboot export.
    result = taiGetModuleExportFunc("SceShellSvc", SHELL_UTIL_LIBRARY_NID,
        SHELL_REQUEST_COLD_RESET_NID, &function);
    if (result < 0 || function == 0)
        return scePowerRequestColdReset();

    result = ((shell_request_cold_reset_fn)function)(0);
    if (result < 0)
        return scePowerRequestColdReset();

    return result;
}
