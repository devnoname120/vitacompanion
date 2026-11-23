#include "main.h"

#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#if ENABLE_LOGGING == 1
#include <psp2/io/fcntl.h>
#endif
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "cmd.h"
#include "heap.h"
#include "input.h"
#include "log.h"
#include "net.h"
#include "nosleep.h"

extern SceUID net_thid;

volatile int run;

void __unused _start() __attribute__((weak, alias("module_start")));
int __unused module_start(SceSize argc, const void* args)
{
    int result;

    (void)argc;
    (void)args;

    result = heap_init(1 * 1024 * 1024);
    LOG("VitaCompanion startup, heap: 0x%08X\n", result);
    if (result < 0)
        return result;

    result = input_start();
    if (result < 0)
    {
        LOG("input_start failed: 0x%08X\n", result);
        heap_term();
        return result;
    }

#if ENABLE_LOGGING == 1
    SceUID fd = sceIoOpen("ux0:dump/vitacompanion_log.txt", SCE_O_TRUNC | SCE_O_CREAT | SCE_O_WRONLY, 0666);
    sceIoClose(fd);
#endif
    run = 1;
    result = nosleep_start();
    if (result < 0)
    {
        LOG("nosleep_start failed: 0x%08X\n", result);
        run = 0;
        input_end();
        heap_term();
        return result;
    }
    result = net_start();
    if (result < 0)
    {
        LOG("net_start failed: 0x%08X\n", result);
        run = 0;
        nosleep_end();
        input_end();
        heap_term();
        return result;
    }

    LOG("VitaCompanion worker started\n");
    return SCE_KERNEL_START_SUCCESS;
}

int __unused module_stop(SceSize argc, const void* args)
{
    (void)argc;
    (void)args;

    run = 0;
    sceKernelWaitThreadEnd(net_thid, NULL, NULL);

    net_end();
    nosleep_end();
    input_end();
    heap_term();

    return SCE_KERNEL_STOP_SUCCESS;
}
