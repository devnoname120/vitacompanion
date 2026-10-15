#include "reboot.h"

#include <psp2/kernel/threadmgr.h>
#include <stddef.h>
#include <stdint.h>
#include <taihen.h>
#include <vitacompanion_kernel.h>

#define SHELL_UTIL_LIBRARY_NID 0xD2B1C8AE
#define SHELL_REQUEST_COLD_RESET_NID 0x636544FB
#define REBOOT_WATCHDOG_DELAY_US (3 * 1000 * 1000)

typedef int (*shell_request_cold_reset_fn)(int unknown);

static int run_reboot_escalation(void)
{
    sceKernelDelayThread(REBOOT_WATCHDOG_DELAY_US);
    vitaCompanionKernelRequestReboot();
    sceKernelDelayThread(REBOOT_WATCHDOG_DELAY_US);
    return vitaCompanionKernelForceReboot();
}

static int reboot_watchdog_thread(unsigned int args, void *argp)
{
    (void)args;
    (void)argp;

    run_reboot_escalation();
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
    int watchdog_result;
    int result;

    watchdog_result = arm_reboot_watchdog();

    // VitaSDK does not expose the private Shell reboot export.
    result = taiGetModuleExportFunc("SceShellSvc", SHELL_UTIL_LIBRARY_NID,
        SHELL_REQUEST_COLD_RESET_NID, &function);
    if (result >= 0 && function != 0)
        result = ((shell_request_cold_reset_fn)function)(0);
    else if (result >= 0)
        result = -1;

    if (watchdog_result < 0)
        return run_reboot_escalation();

    return result;
}
