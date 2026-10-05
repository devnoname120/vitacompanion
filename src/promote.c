#include "promote.h"

#include "promote_head_template.h"
#include "sha1.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/promoterutil.h>
#include <psp2/sysmodule.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROMOTE_PATH_CAPACITY 1024
#define PROMOTE_SFO_MAX_SIZE (1024 * 1024)
#define SFO_HEADER_SIZE 20
#define SFO_ENTRY_SIZE 16
#define SFO_MAGIC 0x46535000
#define SFO_STRING_TYPE 2
#define HEAD_CONTENT_ID_OFFSET 0x30
#define HEAD_CONTENT_ID_SIZE 48
#define HEAD_HEADER_LENGTH_OFFSET 0xD0
#define HEAD_INFO_OFFSET_OFFSET 0x08
#define HEAD_INFO_LENGTH_OFFSET 0x10
#define HEAD_INFO_DIGEST_OFFSET 0xD4
#define HEAD_TOTAL_LENGTH_OFFSET 0xE8
#define HEAD_DIGEST_SIZE 16

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)data[0]
        | ((uint32_t)data[1] << 8)
        | ((uint32_t)data[2] << 16)
        | ((uint32_t)data[3] << 24);
}

static uint32_t read_be32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24)
        | ((uint32_t)data[1] << 16)
        | ((uint32_t)data[2] << 8)
        | (uint32_t)data[3];
}

static int range_is_valid(size_t size, size_t offset, size_t length)
{
    return offset <= size && length <= size - offset;
}

static int make_child_path(char *destination, size_t capacity,
    const char *root, const char *suffix)
{
    size_t root_length;
    size_t suffix_length;
    int separator;

    if (!destination || !root || !suffix || capacity == 0)
        return VITACOMPANION_PROMOTE_ERROR_PATH_TOO_LONG;

    root_length = strlen(root);
    suffix_length = strlen(suffix);
    separator = root_length > 0 && root[root_length - 1] != '/';
    if (root_length == 0 || root_length >= capacity
        || (size_t)separator > capacity - root_length - 1
        || suffix_length > capacity - root_length
            - (size_t)separator - 1)
        return VITACOMPANION_PROMOTE_ERROR_PATH_TOO_LONG;

    memcpy(destination, root, root_length);
    if (separator)
        destination[root_length++] = '/';
    memcpy(destination + root_length, suffix, suffix_length + 1);
    return 0;
}

static int read_file(const char *path, uint8_t **data, size_t *size)
{
    SceIoStat stat;
    SceUID descriptor;
    uint8_t *buffer;
    size_t used = 0;
    int result;
    int close_result;

    memset(&stat, 0, sizeof(stat));
    result = sceIoGetstat(path, &stat);
    if (result < 0 || !SCE_S_ISREG(stat.st_mode))
        return VITACOMPANION_PROMOTE_ERROR_INVALID_LAYOUT;
    if (stat.st_size == 0
        || (uint64_t)stat.st_size > PROMOTE_SFO_MAX_SIZE)
        return VITACOMPANION_PROMOTE_ERROR_INVALID_SFO;

    buffer = malloc((size_t)stat.st_size);
    if (!buffer)
        return VITACOMPANION_PROMOTE_ERROR_NO_MEMORY;

    descriptor = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (descriptor < 0)
    {
        free(buffer);
        return descriptor;
    }

    while (used < (size_t)stat.st_size)
    {
        size_t remaining = (size_t)stat.st_size - used;

        result = sceIoRead(descriptor, buffer + used,
            (SceSize)remaining);
        if (result < 0)
            break;
        if (result == 0)
        {
            result = VITACOMPANION_PROMOTE_ERROR_INCOMPLETE_IO;
            break;
        }
        if ((size_t)result > remaining)
        {
            result = VITACOMPANION_PROMOTE_ERROR_INCOMPLETE_IO;
            break;
        }
        used += (size_t)result;
    }

    close_result = sceIoClose(descriptor);
    if (result >= 0 && close_result < 0)
        result = close_result;
    if (result < 0)
    {
        free(buffer);
        return result;
    }

    *data = buffer;
    *size = used;
    return 0;
}

static int get_sfo_string(const uint8_t *data, size_t size,
    const char *key, char *destination, size_t capacity)
{
    uint32_t key_offset;
    uint32_t value_offset;
    uint32_t count;
    size_t entries_end;
    size_t key_length;
    uint32_t index;

    if (!data || !key || !destination || capacity == 0
        || size < SFO_HEADER_SIZE || read_le32(data) != SFO_MAGIC)
        return VITACOMPANION_PROMOTE_ERROR_INVALID_SFO;

    key_offset = read_le32(data + 8);
    value_offset = read_le32(data + 12);
    count = read_le32(data + 16);
    if (count > (size - SFO_HEADER_SIZE) / SFO_ENTRY_SIZE)
        return VITACOMPANION_PROMOTE_ERROR_INVALID_SFO;

    entries_end = SFO_HEADER_SIZE + (size_t)count * SFO_ENTRY_SIZE;
    if (key_offset < entries_end || key_offset > value_offset
        || value_offset > size)
        return VITACOMPANION_PROMOTE_ERROR_INVALID_SFO;

    destination[0] = '\0';
    key_length = strlen(key);
    for (index = 0; index < count; ++index)
    {
        const uint8_t *entry = data + SFO_HEADER_SIZE
            + (size_t)index * SFO_ENTRY_SIZE;
        uint16_t name_offset = read_le16(entry);
        uint8_t type = entry[3];
        uint32_t value_size = read_le32(entry + 4);
        uint32_t total_size = read_le32(entry + 8);
        uint32_t data_offset = read_le32(entry + 12);
        size_t name_position;
        size_t value_position;
        const uint8_t *name_end;
        size_t source_length;
        size_t copy_length;

        if (name_offset >= value_offset - key_offset
            || data_offset > size - value_offset)
            return VITACOMPANION_PROMOTE_ERROR_INVALID_SFO;
        name_position = (size_t)key_offset + name_offset;
        value_position = (size_t)value_offset + data_offset;
        name_end = memchr(data + name_position, '\0',
            (size_t)value_offset - name_position);
        if (!name_end)
            return VITACOMPANION_PROMOTE_ERROR_INVALID_SFO;
        if ((size_t)(name_end - (data + name_position)) != key_length
            || memcmp(data + name_position, key, key_length) != 0)
            continue;

        if (type != SFO_STRING_TYPE || value_size > total_size
            || !range_is_valid(size, value_position, total_size))
            return VITACOMPANION_PROMOTE_ERROR_INVALID_SFO;

        source_length = 0;
        while (source_length < value_size
            && data[value_position + source_length] != '\0')
            ++source_length;
        copy_length = source_length < capacity - 1
            ? source_length : capacity - 1;
        memcpy(destination, data + value_position, copy_length);
        destination[copy_length] = '\0';
        return 1;
    }

    return 0;
}

static int title_id_is_valid(const char *title_id)
{
    size_t index;

    if (strlen(title_id) != 9)
        return 0;
    for (index = 0; index < 9; ++index)
    {
        if (title_id[index] >= 'a' && title_id[index] <= 'z')
            return 0;
    }
    return 1;
}

static void fpkg_hmac(const uint8_t *data, size_t length,
    uint8_t hmac[HEAD_DIGEST_SIZE])
{
    uint8_t digest[20];
    uint8_t block[64];

    vitacompanion_sha1_digest(data, length, digest);
    memset(block, 0, sizeof(block));
    memcpy(block, digest + 4, 8);
    memcpy(block + 8, digest + 4, 8);
    memcpy(block + 16, digest + 12, 4);
    block[20] = digest[16];
    memcpy(block + 21, digest + 1, 3);
    memcpy(block + 24, block + 16, 8);
    vitacompanion_sha1_digest(block, sizeof(block), digest);
    memcpy(hmac, digest, HEAD_DIGEST_SIZE);
}

static int authenticate_head(uint8_t *head, size_t size)
{
    uint32_t offset;
    uint32_t length;
    uint32_t output;
    uint8_t hmac[HEAD_DIGEST_SIZE];

    if (!range_is_valid(size, HEAD_HEADER_LENGTH_OFFSET, sizeof(uint32_t))
        || !range_is_valid(size, HEAD_INFO_OFFSET_OFFSET, sizeof(uint32_t))
        || !range_is_valid(size, HEAD_INFO_LENGTH_OFFSET, sizeof(uint32_t))
        || !range_is_valid(size, HEAD_INFO_DIGEST_OFFSET, sizeof(uint32_t))
        || !range_is_valid(size, HEAD_TOTAL_LENGTH_OFFSET, sizeof(uint32_t)))
        return VITACOMPANION_PROMOTE_ERROR_INVALID_LAYOUT;

    length = read_be32(head + HEAD_HEADER_LENGTH_OFFSET);
    if (!range_is_valid(size, 0, length)
        || !range_is_valid(size, length, HEAD_DIGEST_SIZE))
        return VITACOMPANION_PROMOTE_ERROR_INVALID_LAYOUT;
    fpkg_hmac(head, length, hmac);
    memcpy(head + length, hmac, sizeof(hmac));

    offset = read_be32(head + HEAD_INFO_OFFSET_OFFSET);
    length = read_be32(head + HEAD_INFO_LENGTH_OFFSET);
    output = read_be32(head + HEAD_INFO_DIGEST_OFFSET);
    if (length < 64 || !range_is_valid(size, offset, length - 64)
        || !range_is_valid(size, output, HEAD_DIGEST_SIZE))
        return VITACOMPANION_PROMOTE_ERROR_INVALID_LAYOUT;
    fpkg_hmac(head + offset, length - 64, hmac);
    memcpy(head + output, hmac, sizeof(hmac));

    length = read_be32(head + HEAD_TOTAL_LENGTH_OFFSET);
    if (!range_is_valid(size, 0, length)
        || !range_is_valid(size, length, HEAD_DIGEST_SIZE))
        return VITACOMPANION_PROMOTE_ERROR_INVALID_LAYOUT;
    fpkg_hmac(head, length, hmac);
    memcpy(head + length, hmac, sizeof(hmac));
    return 0;
}

static int write_file_atomically(const char *temporary_path,
    const char *destination_path, const uint8_t *data, size_t size)
{
    SceUID descriptor;
    size_t written = 0;
    int result = 0;
    int close_result;

    descriptor = sceIoOpen(temporary_path,
        SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (descriptor < 0)
        return descriptor;

    while (written < size)
    {
        size_t remaining = size - written;

        result = sceIoWrite(descriptor, data + written,
            (SceSize)remaining);
        if (result < 0)
            break;
        if (result == 0)
        {
            result = VITACOMPANION_PROMOTE_ERROR_INCOMPLETE_IO;
            break;
        }
        if ((size_t)result > remaining)
        {
            result = VITACOMPANION_PROMOTE_ERROR_INCOMPLETE_IO;
            break;
        }
        written += (size_t)result;
    }

    close_result = sceIoClose(descriptor);
    if (result >= 0 && close_result < 0)
        result = close_result;
    if (result < 0)
    {
        sceIoRemove(temporary_path);
        return result;
    }

    result = sceIoRename(temporary_path, destination_path);
    if (result < 0)
        sceIoRemove(temporary_path);
    return result;
}

static int prepare_package_head(const char *path)
{
    char parameter_path[PROMOTE_PATH_CAPACITY];
    char package_path[PROMOTE_PATH_CAPACITY];
    char head_path[PROMOTE_PATH_CAPACITY];
    char temporary_path[PROMOTE_PATH_CAPACITY];
    char title_id[12];
    char content_id[HEAD_CONTENT_ID_SIZE];
    char fallback_content_id[HEAD_CONTENT_ID_SIZE];
    uint8_t *sfo = NULL;
    size_t sfo_size = 0;
    uint8_t head[VITACOMPANION_HEAD_TEMPLATE_SIZE];
    SceIoStat stat;
    const char *selected_content_id;
    size_t content_id_length;
    int result;

    result = make_child_path(parameter_path, sizeof(parameter_path), path,
        "sce_sys/param.sfo");
    if (result < 0)
        return result;
    result = make_child_path(package_path, sizeof(package_path), path,
        "sce_sys/package");
    if (result < 0)
        return result;
    result = make_child_path(head_path, sizeof(head_path), path,
        "sce_sys/package/head.bin");
    if (result < 0)
        return result;
    result = make_child_path(temporary_path, sizeof(temporary_path), path,
        "sce_sys/package/head.bin.tmp");
    if (result < 0)
        return result;

    memset(&stat, 0, sizeof(stat));
    if (sceIoGetstat(head_path, &stat) >= 0)
        return SCE_S_ISREG(stat.st_mode)
            ? 0 : VITACOMPANION_PROMOTE_ERROR_INVALID_LAYOUT;

    result = read_file(parameter_path, &sfo, &sfo_size);
    if (result < 0)
        return result;

    result = get_sfo_string(sfo, sfo_size, "TITLE_ID",
        title_id, sizeof(title_id));
    if (result <= 0 || !title_id_is_valid(title_id))
    {
        free(sfo);
        return result < 0 ? result
            : VITACOMPANION_PROMOTE_ERROR_INVALID_TITLE_ID;
    }

    result = get_sfo_string(sfo, sfo_size, "CONTENT_ID",
        content_id, sizeof(content_id));
    free(sfo);
    if (result < 0)
        return result;

    if (result == 0 || content_id[0] == '\0')
    {
        result = snprintf(fallback_content_id, sizeof(fallback_content_id),
            "EP9000-%s_00-0000000000000000", title_id);
        if (result < 0 || (size_t)result >= sizeof(fallback_content_id))
            return VITACOMPANION_PROMOTE_ERROR_INVALID_TITLE_ID;
        selected_content_id = fallback_content_id;
    }
    else
    {
        selected_content_id = content_id;
    }

    memcpy(head, vitacompanion_head_template, sizeof(head));
    memset(head + HEAD_CONTENT_ID_OFFSET, 0, HEAD_CONTENT_ID_SIZE);
    content_id_length = strlen(selected_content_id);
    if (content_id_length > HEAD_CONTENT_ID_SIZE)
        content_id_length = HEAD_CONTENT_ID_SIZE;
    memcpy(head + HEAD_CONTENT_ID_OFFSET,
        selected_content_id, content_id_length);

    result = authenticate_head(head, sizeof(head));
    if (result < 0)
        return result;

    result = sceIoMkdir(package_path, 0777);
    if (result < 0)
    {
        memset(&stat, 0, sizeof(stat));
        if (sceIoGetstat(package_path, &stat) < 0
            || !SCE_S_ISDIR(stat.st_mode))
            return result;
    }

    return write_file_atomically(temporary_path, head_path,
        head, sizeof(head));
}

static int load_paf(void)
{
    static uint32_t arguments[] = {
        0x180000, UINT32_MAX, UINT32_MAX, 1, UINT32_MAX, UINT32_MAX
    };
    int module_result = -1;
    SceSysmoduleOpt option;

    memset(&option, 0, sizeof(option));
    option.flags = sizeof(option);
    option.result = &module_result;
    option.unused[0] = -1;
    option.unused[1] = -1;
    return sceSysmoduleLoadModuleInternalWithArg(
        SCE_SYSMODULE_INTERNAL_PAF, sizeof(arguments), arguments, &option);
}

static int unload_paf(void)
{
    SceSysmoduleOpt option;

    memset(&option, 0, sizeof(option));
    return sceSysmoduleUnloadModuleInternalWithArg(
        SCE_SYSMODULE_INTERNAL_PAF, 0, NULL, &option);
}

static void retain_first_error(int *result, int cleanup_result)
{
    if (*result >= 0 && cleanup_result < 0)
        *result = cleanup_result;
}

int promote_directory(const char *path)
{
    SceIoStat stat;
    int result;
    int paf_loaded = 0;
    int promoter_loaded = 0;
    int promoter_initialized = 0;

    if (!path || path[0] == '\0')
        return VITACOMPANION_PROMOTE_ERROR_NOT_DIRECTORY;

    memset(&stat, 0, sizeof(stat));
    result = sceIoGetstat(path, &stat);
    if (result < 0)
        return result;
    if (!SCE_S_ISDIR(stat.st_mode))
        return VITACOMPANION_PROMOTE_ERROR_NOT_DIRECTORY;

    result = prepare_package_head(path);
    if (result < 0)
        return result;

    result = load_paf();
    if (result < 0)
        return result;
    paf_loaded = 1;

    result = sceSysmoduleLoadModuleInternal(
        SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
    if (result < 0)
        goto cleanup;
    promoter_loaded = 1;

    result = scePromoterUtilityInit();
    if (result < 0)
        goto cleanup;
    promoter_initialized = 1;

    result = scePromoterUtilityPromotePkgWithRif(path, 1);

cleanup:
    if (promoter_initialized)
        retain_first_error(&result, scePromoterUtilityExit());
    if (promoter_loaded)
        retain_first_error(&result, sceSysmoduleUnloadModuleInternal(
            SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL));
    if (paf_loaded)
        retain_first_error(&result, unload_paf());
    return result;
}
