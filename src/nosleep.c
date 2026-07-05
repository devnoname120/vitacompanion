#include "nosleep.h"

#include "log.h"

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <stdbool.h>

extern int run;

static SceUID nosleep_thid = -1;
static volatile bool nosleep_enabled = true;
static volatile bool nosleep_locked = false;

static void nosleep_lock(void)
{
    if (!nosleep_locked)
    {
        int ret = sceKernelPowerLock(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
        LOG("sceKernelPowerLock: 0x%08X\n", ret);

        if (ret >= 0)
            nosleep_locked = true;
    }
}

static void nosleep_unlock(void)
{
    if (nosleep_locked)
    {
        int ret = sceKernelPowerUnlock(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
        LOG("sceKernelPowerUnlock: 0x%08X\n", ret);

        if (ret >= 0)
            nosleep_locked = false;
    }
}

static int nosleep_thread(unsigned int args, void *argp)
{
    while (run)
    {
        if (nosleep_enabled)
        {
            nosleep_lock();
            sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
        }
        else
        {
            nosleep_unlock();
        }

        sceKernelDelayThread(1000 * 1000);
    }

    nosleep_unlock();
    sceKernelExitDeleteThread(0);
    return 0;
}

void nosleep_start(void)
{
    nosleep_enabled = true;
    nosleep_thid = sceKernelCreateThread("vitacompanion_nosleep_thread", nosleep_thread, 0x40, 0x10000, 0, 0, NULL);
    sceKernelStartThread(nosleep_thid, 0, NULL);
}

void nosleep_end(void)
{
    nosleep_set_enabled(false);

    if (nosleep_thid >= 0)
    {
        sceKernelWaitThreadEnd(nosleep_thid, NULL, NULL);
        nosleep_thid = -1;
    }
}

void nosleep_set_enabled(bool enabled)
{
    nosleep_enabled = enabled;

    if (enabled)
    {
        nosleep_lock();
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
    }
    else
    {
        nosleep_unlock();
    }
}

bool nosleep_is_enabled(void)
{
    return nosleep_enabled;
}
