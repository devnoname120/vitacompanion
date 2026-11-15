#include "heap.h"

#include <psp2/kernel/clib.h>
#include <psp2/kernel/error.h>
#include <psp2/kernel/sysmem.h>
#include <stdint.h>

#define HEAP_PAGE_SIZE 4096U

/* VitaSDK names Sony's USER_RWDATA memblock type USER_RW. */
#define HEAP_MEMBLOCK_TYPE SCE_KERNEL_MEMBLOCK_TYPE_USER_RW

static SceUID heap_memblock_uid = -1;
static void *heap_base;
static SceSize heap_capacity;
static SceClibMspace heap_mspace;

static int heap_round_capacity(size_t requested_size, SceSize *rounded_size)
{
    const size_t max_sce_size = UINT32_MAX;

    if (!rounded_size || requested_size == 0
        || requested_size > max_sce_size - (HEAP_PAGE_SIZE - 1))
        return SCE_KERNEL_ERROR_INVALID_ARGUMENT_SIZE;

    *rounded_size = (SceSize)((requested_size + HEAP_PAGE_SIZE - 1)
        & ~(size_t)(HEAP_PAGE_SIZE - 1));
    return 0;
}

int heap_init(size_t requested_size)
{
    SceClibMspace mspace;
    SceSize capacity;
    SceUID memblock_uid;
    void *base = NULL;
    int result;

    result = heap_round_capacity(requested_size, &capacity);
    if (result < 0)
        return result;

    heap_term();

    memblock_uid = sceKernelAllocMemBlock("vitacompanion_heap",
        HEAP_MEMBLOCK_TYPE, capacity, NULL);
    if (memblock_uid < 0)
        return memblock_uid;

    result = sceKernelGetMemBlockBase(memblock_uid, &base);
    if (result < 0)
    {
        sceKernelFreeMemBlock(memblock_uid);
        return result;
    }

    mspace = sceClibMspaceCreate(base, capacity);
    if (!mspace)
    {
        sceKernelFreeMemBlock(memblock_uid);
        return SCE_KERNEL_ERROR_NO_MEMORY;
    }

    heap_memblock_uid = memblock_uid;
    heap_base = base;
    heap_capacity = capacity;
    heap_mspace = mspace;
    return 0;
}

void heap_term(void)
{
    SceClibMspace mspace = heap_mspace;
    SceUID memblock_uid = heap_memblock_uid;

    heap_mspace = NULL;
    heap_memblock_uid = -1;
    heap_base = NULL;
    heap_capacity = 0;

    if (mspace)
        sceClibMspaceDestroy(mspace);
    if (memblock_uid >= 0)
        sceKernelFreeMemBlock(memblock_uid);
}

void *malloc(size_t size)
{
    if (!heap_mspace)
        return NULL;
    return sceClibMspaceMalloc(heap_mspace, size);
}

void *calloc(size_t count, size_t size)
{
    if (!heap_mspace || (size != 0 && count > SIZE_MAX / size))
        return NULL;
    return sceClibMspaceCalloc(heap_mspace, count, size);
}

void *realloc(void *ptr, size_t size)
{
    if (!heap_mspace)
        return NULL;
    return sceClibMspaceRealloc(heap_mspace, ptr, size);
}

void free(void *ptr)
{
    if (heap_mspace)
        sceClibMspaceFree(heap_mspace, ptr);
}
