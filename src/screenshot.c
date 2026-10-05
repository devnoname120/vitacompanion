#include "screenshot.h"

#include "screenshot_format.h"

#include <psp2/io/fcntl.h>
#include <psp2/kernel/sysmem.h>
#include <stdio.h>
#include <vitacompanion_kernel.h>

#define SCREENSHOT_BATCH_ROWS 16

static int write_all(SceUID fd, const uint8_t *buffer, SceSize size)
{
    SceSize done = 0;

    while (done < size)
    {
        int result = sceIoWrite(fd, buffer + done, size - done);
        if (result <= 0)
            return -1;
        done += (SceSize)result;
    }

    return 0;
}

/* Writes the displayed frame to path as a BMP and describes the result. */
int screenshot_save(const char *path, char *res_msg, size_t res_size)
{
    vitacompanion_screen_info info;
    uint8_t header[BMP_HEADER_SIZE];
    uint8_t *frame;
    uint8_t *bmp_rows;
    uint32_t frame_size;
    uint32_t stride;
    uint32_t row;
    void *base = NULL;
    SceUID block;
    SceUID fd;
    int result;

    result = vitaCompanionKernelScreenCapture(&info, NULL, 0);
    if (result < 0)
    {
        snprintf(res_msg, res_size,
            "Error: cannot read the framebuffer (0x%08X).\n", result);
        return result;
    }
    if (!screen_pixelformat_is_supported(info.pixelformat))
    {
        snprintf(res_msg, res_size,
            "Error: unsupported pixel format 0x%08X.\n",
            (unsigned int)info.pixelformat);
        return -1;
    }

    frame_size = (uint32_t)result;
    stride = bmp_row_stride(info.width);
    /* The frame plus a batch of converted rows, in 4 KiB pages. */
    block = sceKernelAllocMemBlock("vitacompanion_screenshot",
        SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
        (frame_size + SCREENSHOT_BATCH_ROWS * stride + 0xFFF) & ~0xFFFU,
        NULL);
    if (block < 0 || sceKernelGetMemBlockBase(block, &base) < 0)
    {
        if (block >= 0)
            sceKernelFreeMemBlock(block);
        snprintf(res_msg, res_size, "Error: out of memory.\n");
        return -1;
    }
    frame = base;
    bmp_rows = frame + frame_size;

    /* Capture the whole frame at once; the size check catches mode changes. */
    result = vitaCompanionKernelScreenCapture(&info, frame, frame_size);
    if (result >= 0 && (uint32_t)result != frame_size)
        result = -1;
    if (result < 0)
    {
        sceKernelFreeMemBlock(block);
        snprintf(res_msg, res_size,
            "Error: cannot read the framebuffer (0x%08X).\n", result);
        return result;
    }

    fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0)
    {
        sceKernelFreeMemBlock(block);
        snprintf(res_msg, res_size, "Error: cannot create %s.\n", path);
        return fd;
    }

    bmp_write_header(header, info.width, info.height);
    result = write_all(fd, header, sizeof(header));

    for (row = 0; row < info.height && result >= 0;
        row += SCREENSHOT_BATCH_ROWS)
    {
        uint32_t count = info.height - row < SCREENSHOT_BATCH_ROWS
            ? info.height - row : SCREENSHOT_BATCH_ROWS;
        uint32_t i;

        for (i = 0; i < count; ++i)
            screen_convert_row(frame + (row + i) * info.width * 4,
                bmp_rows + i * stride, info.width, info.pixelformat);
        result = write_all(fd, bmp_rows, count * stride);
    }

    if (sceIoClose(fd) < 0 && result >= 0)
        result = -1;
    sceKernelFreeMemBlock(block);

    if (result < 0)
    {
        sceIoRemove(path);
        snprintf(res_msg, res_size,
            "Error: screenshot failed (0x%08X).\n", result);
        return result;
    }

    snprintf(res_msg, res_size, "Saved %s (%ux%u).\n", path,
        (unsigned int)info.width, (unsigned int)info.height);
    return 0;
}
