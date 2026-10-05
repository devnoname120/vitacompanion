#pragma once

#include <stddef.h>

#define SCREENSHOT_DEFAULT_PATH "ux0:data/vitacompanion_screenshot.bmp"

int screenshot_save(const char *path, char *res_msg, size_t res_size);
