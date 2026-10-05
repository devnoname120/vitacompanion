#include "screenshot_format.h"

#include <string.h>

/* SCE_DISPLAY_PIXELFORMAT_* values, copied so host tests need no SDK. */
#define PIXELFORMAT_A8B8G8R8 0x00000000U
#define PIXELFORMAT_A2B10G10R10 0x60800000U

static void write_le16(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void write_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

uint32_t bmp_row_stride(uint32_t width)
{
    return (width * 3 + 3) & ~3U;
}

/* 24-bit top-down BMP, so rows can be written in framebuffer order. */
void bmp_write_header(uint8_t header[BMP_HEADER_SIZE], uint32_t width,
    uint32_t height)
{
    uint32_t image_size = bmp_row_stride(width) * height;

    memset(header, 0, BMP_HEADER_SIZE);
    header[0] = 'B';
    header[1] = 'M';
    write_le32(header + 2, BMP_HEADER_SIZE + image_size);
    write_le32(header + 10, BMP_HEADER_SIZE);
    write_le32(header + 14, 40);
    write_le32(header + 18, width);
    write_le32(header + 22, (uint32_t)-(int32_t)height);
    write_le16(header + 26, 1);
    write_le16(header + 28, 24);
    write_le32(header + 34, image_size);
}

bool screen_pixelformat_is_supported(uint32_t pixelformat)
{
    return pixelformat == PIXELFORMAT_A8B8G8R8 ||
        pixelformat == PIXELFORMAT_A2B10G10R10;
}

/* Converts one framebuffer row to BMP's BGR byte order. */
void screen_convert_row(const uint8_t *src, uint8_t *dst, uint32_t width,
    uint32_t pixelformat)
{
    uint32_t x;

    if (pixelformat == PIXELFORMAT_A2B10G10R10)
    {
        for (x = 0; x < width; ++x)
        {
            const uint8_t *p = src + x * 4;
            uint32_t pixel = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);

            dst[x * 3 + 0] = (uint8_t)(pixel >> 22);
            dst[x * 3 + 1] = (uint8_t)(pixel >> 12);
            dst[x * 3 + 2] = (uint8_t)(pixel >> 2);
        }
    }
    else
    {
        for (x = 0; x < width; ++x)
        {
            dst[x * 3 + 0] = src[x * 4 + 2];
            dst[x * 3 + 1] = src[x * 4 + 1];
            dst[x * 3 + 2] = src[x * 4 + 0];
        }
    }

    for (x = width * 3; x < bmp_row_stride(width); ++x)
        dst[x] = 0;
}
