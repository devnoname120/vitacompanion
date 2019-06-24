#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <taihen.h>
#define TAIPOOL_AS_STDLIB
#include "cmd.h"
#include "main.h"
#include "net.h"

#include <taipool.h>

extern SceUID net_thid;
extern int all_is_up;
extern int net_connected;

int run;

void __unused _start() __attribute__((weak, alias("module_start")));
int __unused module_start(SceSize argc, const void* args)
{
    // Required for ftpvita
    taipool_init(1 * 1024 * 1024);

#if ENABLE_LOGGING == 1
    SceUID fd = sceIoOpen("ux0:dump/vitacompanion_log.txt", SCE_O_TRUNC | SCE_O_CREAT | SCE_O_WRONLY, 0666);
    sceIoClose(fd);
#endif
    run = 1;
    net_start();

    return SCE_KERNEL_START_SUCCESS;
}

int __unused module_stop(SceSize argc, const void* args)
{
    run = 0;
    sceKernelWaitThreadEnd(net_thid, NULL, NULL);

    if (all_is_up)
    {
        net_end();
        cmd_end();
    }

    taipool_term();

    return SCE_KERNEL_STOP_SUCCESS;
}
