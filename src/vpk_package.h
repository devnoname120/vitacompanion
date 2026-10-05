#pragma once

#include "vpk_format.h"

#include <stddef.h>

#define VPK_ERROR_NO_MEMORY -1
#define VPK_ERROR_OPEN -2
#define VPK_ERROR_READ -3
#define VPK_ERROR_WRITE -4
#define VPK_ERROR_NOT_ZIP -5
#define VPK_ERROR_ZIP64 -6
#define VPK_ERROR_ENCRYPTED -7
#define VPK_ERROR_METHOD -8
#define VPK_ERROR_BAD_ENTRY -9
#define VPK_ERROR_CORRUPT -10
#define VPK_ERROR_PARAM_SFO -11
#define VPK_ERROR_TITLE_ID -12

int vpk_remove_tree(const char *path);
int vpk_extract(const char *vpk_path, const char *dest_dir);
int vpk_prepare_package(const char *pkg_dir,
    char title_id[VPK_TITLE_ID_LENGTH + 1]);
const char *vpk_error_string(int error);
