#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VPK_HEAD_BIN_SIZE 0x430
#define VPK_TITLE_ID_LENGTH 9

uint32_t vpk_crc32(uint32_t crc, const void *data, size_t size);
int vpk_sfo_get_string(const uint8_t *sfo, size_t sfo_size,
    const char *key, char *value, size_t value_size);
bool vpk_title_id_is_valid(const char *title_id);
int vpk_make_head_bin(const char *title_id, const char *content_id,
    uint8_t head_bin[VPK_HEAD_BIN_SIZE]);
bool vpk_entry_name_normalize(char *name);
