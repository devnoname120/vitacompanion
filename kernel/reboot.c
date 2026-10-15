#include "reboot.h"

#include <psp2kern/kernel/syscon.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr/event_flags.h>
#include <stddef.h>
#include <stdint.h>

#define POWER_EVENT_FLAG_NAME "ScePowerMain"
#define POWER_EVENT_FLAG_BITS 0x80
#define EVENT_FLAG_VISIBILITY_LEVEL 7
#define EVENT_FLAG_LIST_RETRIES 3
#define PAGE_SIZE 0x1000

typedef struct {
    SceSize size;
    SceUID event_flag_id;
    char name[32];
    uint32_t attributes;
    uint32_t initial_pattern;
    uint32_t current_pattern;
    int waiting_threads;
} threadmgr_event_flag_info;

typedef char threadmgr_event_flag_info_size[
    sizeof(threadmgr_event_flag_info) == 0x38 ? 1 : -1];

/* VitaSDK does not expose the event-flag enumeration export. */
int SceThreadmgrForDriver_5870C73A(int visibility_level,
    threadmgr_event_flag_info *infos, SceSize size, SceSize *count);

static SceUID power_event_flag = -1;

static int power_event_name_matches(const char *name)
{
    static const char expected[] = POWER_EVENT_FLAG_NAME;
    int index = 0;

    if (!name)
        return 0;

    while (expected[index] != '\0')
    {
        if (name[index] != expected[index])
            return 0;
        ++index;
    }

    return name[index] == '\0';
}

static int allocate_event_flag_infos(int count, SceUID *memblock_uid,
    threadmgr_event_flag_info **infos, SceSize *buffer_size)
{
    SceSize allocation_size;
    int result;

    if (count <= 0 ||
        (uint32_t)count > UINT32_MAX / sizeof(threadmgr_event_flag_info))
        return -1;

    *buffer_size = (SceSize)count * sizeof(threadmgr_event_flag_info);
    if (*buffer_size > UINT32_MAX - (PAGE_SIZE - 1))
        return -1;

    allocation_size = (*buffer_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    *memblock_uid = ksceKernelAllocMemBlock(
        "vitacompanion_reboot_flags",
        SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW,
        allocation_size, NULL);
    if (*memblock_uid < 0)
        return *memblock_uid;

    result = ksceKernelGetMemBlockBase(*memblock_uid, (void **)infos);
    if (result < 0)
    {
        ksceKernelFreeMemBlock(*memblock_uid);
        *memblock_uid = -1;
        return result;
    }

    return 0;
}

static int resolve_power_event_flag(void)
{
    int attempt;

    for (attempt = 0; attempt < EVENT_FLAG_LIST_RETRIES; ++attempt)
    {
        SceUID memblock_uid = -1;
        threadmgr_event_flag_info *infos = NULL;
        SceSize buffer_size = 0;
        SceSize actual = 0;
        int total;
        int result;
        int index;

        total = SceThreadmgrForDriver_5870C73A(
            EVENT_FLAG_VISIBILITY_LEVEL, NULL, 0, NULL);
        if (total < 0)
            return total;
        if (total == 0)
            return -1;

        result = allocate_event_flag_infos(
            total, &memblock_uid, &infos, &buffer_size);
        if (result < 0)
            return result;

        result = SceThreadmgrForDriver_5870C73A(
            EVENT_FLAG_VISIBILITY_LEVEL, infos, buffer_size, &actual);
        if (result < 0)
        {
            ksceKernelFreeMemBlock(memblock_uid);
            return result;
        }
        if (result > (int)actual)
        {
            ksceKernelFreeMemBlock(memblock_uid);
            continue;
        }

        for (index = 0; index < (int)actual; ++index)
        {
            if (power_event_name_matches(infos[index].name))
            {
                power_event_flag = infos[index].event_flag_id;
                ksceKernelFreeMemBlock(memblock_uid);
                return 0;
            }
        }

        ksceKernelFreeMemBlock(memblock_uid);
        return -1;
    }

    return -1;
}

int vitacompanion_reboot_request_power_event(void)
{
    int result;

    if (power_event_flag < 0)
    {
        result = resolve_power_event_flag();
        if (result < 0)
            return result;
    }

    result = ksceKernelSetEventFlag(
        power_event_flag, POWER_EVENT_FLAG_BITS);
    if (result >= 0)
        return result;

    power_event_flag = -1;
    result = resolve_power_event_flag();
    if (result < 0)
        return result;

    return ksceKernelSetEventFlag(
        power_event_flag, POWER_EVENT_FLAG_BITS);
}

int vitacompanion_reboot_force_syscon(void)
{
    return ksceSysconSetPowerMode(2, 0);
}
