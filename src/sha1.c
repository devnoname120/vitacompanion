#include "sha1.h"

#include <string.h>

#define ROTATE_LEFT(value, bits) \
    (((value) << (bits)) | ((value) >> (32 - (bits))))

static void transform(vitacompanion_sha1_context *context,
    const uint8_t data[64])
{
    uint32_t words[80];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t temporary;
    unsigned int i;

    for (i = 0; i < 16; ++i)
    {
        unsigned int offset = i * 4;

        words[i] = ((uint32_t)data[offset] << 24)
            | ((uint32_t)data[offset + 1] << 16)
            | ((uint32_t)data[offset + 2] << 8)
            | (uint32_t)data[offset + 3];
    }
    for (; i < 80; ++i)
        words[i] = ROTATE_LEFT(words[i - 3] ^ words[i - 8]
                ^ words[i - 14] ^ words[i - 16], 1);

    a = context->state[0];
    b = context->state[1];
    c = context->state[2];
    d = context->state[3];
    e = context->state[4];

    for (i = 0; i < 20; ++i)
    {
        temporary = ROTATE_LEFT(a, 5)
            + ((b & c) ^ (~b & d)) + e + 0x5A827999 + words[i];
        e = d;
        d = c;
        c = ROTATE_LEFT(b, 30);
        b = a;
        a = temporary;
    }
    for (; i < 40; ++i)
    {
        temporary = ROTATE_LEFT(a, 5)
            + (b ^ c ^ d) + e + 0x6ED9EBA1 + words[i];
        e = d;
        d = c;
        c = ROTATE_LEFT(b, 30);
        b = a;
        a = temporary;
    }
    for (; i < 60; ++i)
    {
        temporary = ROTATE_LEFT(a, 5)
            + ((b & c) ^ (b & d) ^ (c & d))
            + e + 0x8F1BBCDC + words[i];
        e = d;
        d = c;
        c = ROTATE_LEFT(b, 30);
        b = a;
        a = temporary;
    }
    for (; i < 80; ++i)
    {
        temporary = ROTATE_LEFT(a, 5)
            + (b ^ c ^ d) + e + 0xCA62C1D6 + words[i];
        e = d;
        d = c;
        c = ROTATE_LEFT(b, 30);
        b = a;
        a = temporary;
    }

    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
}

void vitacompanion_sha1_init(vitacompanion_sha1_context *context)
{
    context->data_length = 0;
    context->bit_length = 0;
    context->state[0] = 0x67452301;
    context->state[1] = 0xEFCDAB89;
    context->state[2] = 0x98BADCFE;
    context->state[3] = 0x10325476;
    context->state[4] = 0xC3D2E1F0;
}

void vitacompanion_sha1_update(vitacompanion_sha1_context *context,
    const void *data, size_t length)
{
    const uint8_t *bytes = data;
    size_t i;

    for (i = 0; i < length; ++i)
    {
        context->data[context->data_length++] = bytes[i];
        if (context->data_length == sizeof(context->data))
        {
            transform(context, context->data);
            context->bit_length += 512;
            context->data_length = 0;
        }
    }
}

void vitacompanion_sha1_final(vitacompanion_sha1_context *context,
    uint8_t digest[20])
{
    uint32_t i = context->data_length;

    context->data[i++] = 0x80;
    if (i > 56)
    {
        while (i < 64)
            context->data[i++] = 0;
        transform(context, context->data);
        memset(context->data, 0, 56);
    }
    else
    {
        while (i < 56)
            context->data[i++] = 0;
    }

    context->bit_length += (uint64_t)context->data_length * 8;
    for (i = 0; i < 8; ++i)
        context->data[63 - i] =
            (uint8_t)(context->bit_length >> (i * 8));
    transform(context, context->data);

    for (i = 0; i < 4; ++i)
    {
        digest[i] = (uint8_t)(context->state[0] >> (24 - i * 8));
        digest[i + 4] = (uint8_t)(context->state[1] >> (24 - i * 8));
        digest[i + 8] = (uint8_t)(context->state[2] >> (24 - i * 8));
        digest[i + 12] = (uint8_t)(context->state[3] >> (24 - i * 8));
        digest[i + 16] = (uint8_t)(context->state[4] >> (24 - i * 8));
    }
}

void vitacompanion_sha1_digest(const void *data, size_t length,
    uint8_t digest[20])
{
    vitacompanion_sha1_context context;

    vitacompanion_sha1_init(&context);
    vitacompanion_sha1_update(&context, data, length);
    vitacompanion_sha1_final(&context, digest);
}
