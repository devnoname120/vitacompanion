#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BMP_HEADER_SIZE 54

uint32_t bmp_row_stride(uint32_t width);
void bmp_write_header(uint8_t header[BMP_HEADER_SIZE], uint32_t width,
    uint32_t height);
bool screen_pixelformat_is_supported(uint32_t pixelformat);
void screen_convert_row(const uint8_t *src, uint8_t *dst, uint32_t width,
    uint32_t pixelformat);
