#pragma once

#include "vpk_format.h"

#include <stddef.h>

int vpk_install(const char *vpk_path,
    char title_id[VPK_TITLE_ID_LENGTH + 1]);
int vpk_install_format_error(int error, char *buffer, size_t buffer_size);
