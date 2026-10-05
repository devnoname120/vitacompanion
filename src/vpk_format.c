#include "vpk_format.h"

#include <stdio.h>
#include <string.h>

#define SFO_MAGIC 0x46535000
#define SFO_HEADER_SIZE 20
#define SFO_ENTRY_SIZE 16
#define SFO_FORMAT_UTF8_SPECIAL 0x0004
#define SFO_FORMAT_UTF8 0x0204

#define PKG_METADATA_OFFSET 0x280
#define PKG_METADATA_COUNT 11
#define PKG_METADATA_SIZE 0x190
#define PKG_MAIN_HMAC_OFFSET 0x100
#define PKG_METADATA_HMAC_OFFSET 0x410
#define PKG_FULL_HMAC_OFFSET 0x420
#define PKG_TOTAL_SIZE 0x19000

typedef struct {
    uint32_t state[5];
    uint64_t length;
    uint8_t block[64];
    size_t block_used;
} sha1_context;

static uint32_t read_le16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
        ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

uint32_t vpk_crc32(uint32_t crc, const void *data, size_t size)
{
    static uint32_t table[256];
    static bool table_ready;
    const uint8_t *bytes = data;
    size_t i;

    if (!table_ready)
    {
        uint32_t n;

        for (n = 0; n < 256; ++n)
        {
            uint32_t c = n;
            int bit;

            for (bit = 0; bit < 8; ++bit)
                c = (c & 1) ? 0xEDB88320U ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
        table_ready = true;
    }

    crc = ~crc;
    for (i = 0; i < size; ++i)
        crc = table[(crc ^ bytes[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

int vpk_sfo_get_string(const uint8_t *sfo, size_t sfo_size,
    const char *key, char *value, size_t value_size)
{
    uint64_t key_table;
    uint64_t data_table;
    uint32_t count;
    uint32_t i;

    if (!sfo || !key || !value || value_size == 0 ||
        sfo_size < SFO_HEADER_SIZE || read_le32(sfo) != SFO_MAGIC)
        return -1;

    key_table = read_le32(sfo + 8);
    data_table = read_le32(sfo + 12);
    count = read_le32(sfo + 16);
    if (count > (sfo_size - SFO_HEADER_SIZE) / SFO_ENTRY_SIZE)
        return -1;

    for (i = 0; i < count; ++i)
    {
        const uint8_t *entry = sfo + SFO_HEADER_SIZE + i * SFO_ENTRY_SIZE;
        uint64_t key_offset = key_table + read_le16(entry);
        uint32_t format = read_le16(entry + 2);
        uint64_t length = read_le32(entry + 4);
        uint64_t data_offset = data_table + read_le32(entry + 12);
        size_t copy_length;

        if (key_offset >= sfo_size ||
            !memchr(sfo + key_offset, '\0', sfo_size - (size_t)key_offset) ||
            strcmp((const char *)sfo + key_offset, key) != 0)
            continue;

        if ((format != SFO_FORMAT_UTF8 && format != SFO_FORMAT_UTF8_SPECIAL) ||
            data_offset > sfo_size || length > sfo_size - data_offset)
            return -1;

        copy_length = (size_t)length < value_size - 1
            ? (size_t)length : value_size - 1;
        memcpy(value, sfo + data_offset, copy_length);
        value[copy_length] = '\0';
        return 0;
    }

    return -1;
}

bool vpk_title_id_is_valid(const char *title_id)
{
    size_t i;

    if (!title_id || strlen(title_id) != VPK_TITLE_ID_LENGTH)
        return false;

    for (i = 0; i < VPK_TITLE_ID_LENGTH; ++i)
    {
        char c = title_id[i];

        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
            return false;
    }

    return true;
}

static uint32_t rotl32(uint32_t value, int shift)
{
    return (value << shift) | (value >> (32 - shift));
}

static void sha1_transform(sha1_context *ctx, const uint8_t block[64])
{
    uint32_t w[80];
    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t e = ctx->state[4];
    int i;

    for (i = 0; i < 16; ++i)
        w[i] = read_be32(block + i * 4);
    for (; i < 80; ++i)
        w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    for (i = 0; i < 80; ++i)
    {
        uint32_t f;
        uint32_t k;
        uint32_t t;

        if (i < 20)
        {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        }
        else if (i < 40)
        {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        }
        else if (i < 60)
        {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        }
        else
        {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }

        t = rotl32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rotl32(b, 30);
        b = a;
        a = t;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
}

static void sha1_init(sha1_context *ctx)
{
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0;
    ctx->length = 0;
    ctx->block_used = 0;
}

static void sha1_update(sha1_context *ctx, const uint8_t *data, size_t size)
{
    size_t i;

    for (i = 0; i < size; ++i)
    {
        ctx->block[ctx->block_used++] = data[i];
        if (ctx->block_used == 64)
        {
            sha1_transform(ctx, ctx->block);
            ctx->block_used = 0;
        }
    }
    ctx->length += size;
}

static void sha1_final(sha1_context *ctx, uint8_t digest[20])
{
    uint64_t bit_length = ctx->length * 8;
    uint8_t padding = 0x80;
    uint8_t length_bytes[8];
    int i;

    sha1_update(ctx, &padding, 1);
    padding = 0;
    while (ctx->block_used != 56)
        sha1_update(ctx, &padding, 1);

    for (i = 0; i < 8; ++i)
        length_bytes[i] = (uint8_t)(bit_length >> (56 - i * 8));
    sha1_update(ctx, length_bytes, 8);

    for (i = 0; i < 20; ++i)
        digest[i] = (uint8_t)(ctx->state[i / 4] >> (24 - (i % 4) * 8));
}

/* "PKG HMAC algorithm" from https://www.psdevwiki.com/ps3/PKG_files */
static void pkg_hmac(const uint8_t *data, size_t size, uint8_t hmac[16])
{
    sha1_context ctx;
    uint8_t sha1[20];
    uint8_t buffer[64];

    sha1_init(&ctx);
    sha1_update(&ctx, data, size);
    sha1_final(&ctx, sha1);

    memset(buffer, 0, sizeof(buffer));
    memcpy(&buffer[0], &sha1[4], 8);
    memcpy(&buffer[8], &sha1[4], 8);
    memcpy(&buffer[16], &sha1[12], 4);
    buffer[20] = sha1[16];
    buffer[21] = sha1[1];
    buffer[22] = sha1[2];
    buffer[23] = sha1[3];
    memcpy(&buffer[24], &buffer[16], 8);

    sha1_init(&ctx);
    sha1_update(&ctx, buffer, sizeof(buffer));
    sha1_final(&ctx, sha1);
    memcpy(hmac, sha1, 16);
}

static void write_be32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static void write_be64(uint8_t *p, uint64_t value)
{
    write_be32(p, (uint32_t)(value >> 32));
    write_be32(p + 4, (uint32_t)value);
}

/*
 * Writes a PKG metadata entry (identifier, data size, data) whose data
 * starts with the given big-endian words and is zero-filled after them.
 * Returns the position of the next entry.
 */
static uint8_t *write_metadata(uint8_t *p, uint32_t id, uint32_t size,
    const uint32_t *words, size_t word_count)
{
    size_t i;

    write_be32(&p[0], id);
    write_be32(&p[4], size);
    for (i = 0; i < word_count; ++i)
        write_be32(&p[8 + i * 4], words[i]);

    return p + 8 + size;
}

/*
 * Builds the fake PKG header ("head.bin") that the package promoter needs to
 * install an unpacked application. Field names and offsets follow
 * https://www.psdevwiki.com/ps3/PKG_files. The header describes a small
 * package; the digests and signatures of its nonexistent data stay zero and
 * only the three PKG HMACs of the extended header are filled in.
 */
int vpk_make_head_bin(const char *title_id, const char *content_id,
    uint8_t head_bin[VPK_HEAD_BIN_SIZE])
{
    static const uint32_t drm_type[] = { 2 };
    static const uint32_t content_type[] = { 0x15 }; /* PSP2GD, ux0:app */
    static const uint32_t package_flags[] = { 0x0E };
    static const uint32_t package_size[] = { 0, PKG_TOTAL_SIZE };
    /* make_package_npdrm revision 1967, package version 01.00 */
    static const uint32_t revision[] = { 0x19670100 };
    static const uint32_t unknown_9[] = { 0, 0x00240000 };
    static const uint32_t item_info[] = { 0, 0xB0 };
    static const uint32_t unknown_data_info[] = { 0x590, 0x320, 0, 0, 0,
        0x01010101 };
    static const uint32_t entirety_info[] = { 0xA30, 0x60, 0xC2000000, 1 };
    static const uint32_t publishing_tools[] = { 0x01720000 };
    char fallback_content_id[48];
    uint8_t *p;

    if (!vpk_title_id_is_valid(title_id) || !head_bin)
        return -1;

    if (!content_id || content_id[0] == '\0')
    {
        snprintf(fallback_content_id, sizeof(fallback_content_id),
            "EP9000-%s_00-0000000000000000", title_id);
        content_id = fallback_content_id;
    }

    memset(head_bin, 0, VPK_HEAD_BIN_SIZE);

    /* Main header: debug (non-finalized) package. */
    write_be32(&head_bin[0x00], 0x7F504B47); /* ".PKG" */
    write_be32(&head_bin[0x04], 0x00000001); /* pkg_revision, pkg_type */
    write_be32(&head_bin[0x08], PKG_METADATA_OFFSET);
    write_be32(&head_bin[0x0C], PKG_METADATA_COUNT);
    write_be32(&head_bin[0x10], PKG_METADATA_SIZE);
    write_be32(&head_bin[0x14], 3); /* item_count */
    write_be64(&head_bin[0x18], PKG_TOTAL_SIZE);
    write_be64(&head_bin[0x20], 0xA90); /* data_offset */
    write_be64(&head_bin[0x28], 0x310); /* data_size */
    strncpy((char *)&head_bin[0x30], content_id, 0x30);

    /* Extended header. */
    write_be32(&head_bin[0xC0], 0x7F657874); /* ".ext" */
    write_be32(&head_bin[0xC4], 1);
    write_be32(&head_bin[0xC8], 0x40);  /* ext_hdr_size */
    write_be32(&head_bin[0xCC], 0x180); /* ext_data_size */
    write_be32(&head_bin[0xD0], PKG_MAIN_HMAC_OFFSET);
    write_be32(&head_bin[0xD4], PKG_METADATA_HMAC_OFFSET);
    write_be64(&head_bin[0xD8], 0xDE0); /* tail_offset */
    write_be32(&head_bin[0xE4], 0xC0000002); /* pkg_key_id: PS Vita */
    write_be32(&head_bin[0xE8], PKG_FULL_HMAC_OFFSET);

    /* Metadata, followed by its zeroed digest. */
    p = &head_bin[PKG_METADATA_OFFSET];
    p = write_metadata(p, 0x1, 4, drm_type, 1);
    p = write_metadata(p, 0x2, 4, content_type, 1);
    p = write_metadata(p, 0x3, 4, package_flags, 1);
    p = write_metadata(p, 0x4, 8, package_size, 2);
    p = write_metadata(p, 0x5, 4, revision, 1);
    p = write_metadata(p, 0x8, 8, NULL, 0);
    p = write_metadata(p, 0x9, 8, unknown_9, 2);
    p = write_metadata(p, 0xD, 0x28, item_info, 2);
    p = write_metadata(p, 0xF, 0x48, unknown_data_info, 6);
    p = write_metadata(p, 0x10, 0x38, entirety_info, 4);
    write_metadata(p, 0x11, 0x28, publishing_tools, 1);

    pkg_hmac(head_bin, PKG_MAIN_HMAC_OFFSET,
        &head_bin[PKG_MAIN_HMAC_OFFSET]);
    pkg_hmac(&head_bin[PKG_METADATA_OFFSET], PKG_METADATA_SIZE - 0x40,
        &head_bin[PKG_METADATA_HMAC_OFFSET]);
    pkg_hmac(head_bin, PKG_FULL_HMAC_OFFSET,
        &head_bin[PKG_FULL_HMAC_OFFSET]);

    return 0;
}

/*
 * Converts backslash separators and rejects names that could escape the
 * extraction directory. A trailing slash marks a directory entry.
 */
bool vpk_entry_name_normalize(char *name)
{
    char *component;
    char *p;

    if (!name || name[0] == '\0')
        return false;

    for (p = name; *p; ++p)
    {
        if (*p == '\\')
            *p = '/';
        else if (*p == ':')
            return false;
    }

    if (name[0] == '/')
        return false;

    component = name;
    for (p = name;; ++p)
    {
        if (*p == '/' || *p == '\0')
        {
            size_t length = (size_t)(p - component);

            if ((length == 0 && *p != '\0') ||
                (length == 1 && component[0] == '.') ||
                (length == 2 && component[0] == '.' && component[1] == '.'))
                return false;

            if (*p == '\0')
                break;
            component = p + 1;
        }
    }

    return true;
}
