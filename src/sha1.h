#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t data[64];
    uint32_t data_length;
    uint64_t bit_length;
    uint32_t state[5];
} vitacompanion_sha1_context;

void vitacompanion_sha1_init(vitacompanion_sha1_context *context);
void vitacompanion_sha1_update(vitacompanion_sha1_context *context,
    const void *data, size_t length);
void vitacompanion_sha1_final(vitacompanion_sha1_context *context,
    uint8_t digest[20]);
void vitacompanion_sha1_digest(const void *data, size_t length,
    uint8_t digest[20]);
