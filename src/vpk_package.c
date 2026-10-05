#include "vpk_package.h"

#include <miniz_tinfl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/sysmem.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define VPK_IN_SIZE (128 * 1024)
/* tinfl needs a power-of-two circular output buffer when streaming. */
#define VPK_OUT_SIZE (256 * 1024)
#define VPK_PATH_MAX 512

#define ZIP_LOCAL_HEADER_SIG 0x04034B50
#define ZIP_CENTRAL_HEADER_SIG 0x02014B50
#define ZIP_END_SIG 0x06054B50
#define ZIP_LOCAL_HEADER_SIZE 30
#define ZIP_CENTRAL_HEADER_SIZE 46
#define ZIP_END_SIZE 22
#define ZIP_MAX_COMMENT 0xFFFF
#define ZIP_FLAG_ENCRYPTED 0x0001
#define ZIP_METHOD_STORED 0
#define ZIP_METHOD_DEFLATED 8

typedef struct {
    tinfl_decompressor inflator;
    uint8_t in[VPK_IN_SIZE];
    uint8_t out[VPK_OUT_SIZE];
    char name[VPK_PATH_MAX];
    char path[VPK_PATH_MAX];
} vpk_work;

typedef struct {
    uint32_t method;
    uint32_t crc;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    SceOff data_offset;
} zip_entry;

static uint32_t read_le16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static vpk_work *work_alloc(SceUID *block)
{
    SceSize size = (sizeof(vpk_work) + 0xFFF) & ~0xFFF;
    void *base = NULL;

    *block = sceKernelAllocMemBlock("vitacompanion_vpk",
        SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, size, NULL);
    if (*block < 0)
        return NULL;

    if (sceKernelGetMemBlockBase(*block, &base) < 0)
    {
        sceKernelFreeMemBlock(*block);
        *block = -1;
        return NULL;
    }

    return base;
}

static int read_at(SceUID fd, void *buffer, SceSize size, SceOff offset)
{
    SceSize done = 0;

    while (done < size)
    {
        int result = sceIoPread(fd, (uint8_t *)buffer + done, size - done,
            offset + done);
        if (result <= 0)
            return VPK_ERROR_READ;
        done += (SceSize)result;
    }

    return 0;
}

static int write_all(SceUID fd, const void *buffer, SceSize size)
{
    SceSize done = 0;

    while (done < size)
    {
        int result = sceIoWrite(fd, (const uint8_t *)buffer + done,
            size - done);
        if (result <= 0)
            return VPK_ERROR_WRITE;
        done += (SceSize)result;
    }

    return 0;
}

/* Creates each parent of path, and path itself when include_last is set. */
static void make_dirs(char *path, bool include_last)
{
    char *p;

    for (p = path + 1; *p; ++p)
    {
        if (*p == '/')
        {
            *p = '\0';
            sceIoMkdir(path, 0777);
            *p = '/';
        }
    }

    if (include_last)
        sceIoMkdir(path, 0777);
}

static int remove_tree_at(char *path, size_t capacity)
{
    SceIoStat stat;
    SceIoDirent entry;
    SceUID dfd;
    size_t length;
    int result = 0;

    if (sceIoGetstat(path, &stat) < 0)
        return 0;

    if (!SCE_S_ISDIR(stat.st_mode))
        return sceIoRemove(path);

    dfd = sceIoDopen(path);
    if (dfd < 0)
        return dfd;

    length = strlen(path);
    memset(&entry, 0, sizeof(entry));
    while (sceIoDread(dfd, &entry) > 0)
    {
        size_t name_length = strlen(entry.d_name);
        int child_result;

        if (!strcmp(entry.d_name, ".") || !strcmp(entry.d_name, ".."))
            continue;

        if (length + 1 + name_length >= capacity)
        {
            result = VPK_ERROR_BAD_ENTRY;
            continue;
        }

        path[length] = '/';
        memcpy(path + length + 1, entry.d_name, name_length + 1);
        if (SCE_S_ISDIR(entry.d_stat.st_mode))
            child_result = remove_tree_at(path, capacity);
        else
            child_result = sceIoRemove(path);
        path[length] = '\0';

        if (child_result < 0)
            result = child_result;
        memset(&entry, 0, sizeof(entry));
    }
    sceIoDclose(dfd);

    if (result < 0)
        return result;
    return sceIoRmdir(path);
}

int vpk_remove_tree(const char *path)
{
    char buffer[VPK_PATH_MAX];

    if (!path || strlen(path) >= sizeof(buffer))
        return VPK_ERROR_BAD_ENTRY;

    strcpy(buffer, path);
    return remove_tree_at(buffer, sizeof(buffer));
}

static int copy_stored(vpk_work *work, SceUID in_fd, SceUID out_fd,
    const zip_entry *entry, uint32_t *crc)
{
    uint32_t remaining = entry->compressed_size;
    SceOff offset = entry->data_offset;

    if (entry->compressed_size != entry->uncompressed_size)
        return VPK_ERROR_CORRUPT;

    while (remaining > 0)
    {
        SceSize chunk = remaining < VPK_IN_SIZE ? remaining : VPK_IN_SIZE;
        int result = read_at(in_fd, work->in, chunk, offset);

        if (result < 0)
            return result;
        result = write_all(out_fd, work->in, chunk);
        if (result < 0)
            return result;

        *crc = vpk_crc32(*crc, work->in, chunk);
        offset += chunk;
        remaining -= chunk;
    }

    return 0;
}

static int inflate_entry(vpk_work *work, SceUID in_fd, SceUID out_fd,
    const zip_entry *entry, uint32_t *crc)
{
    uint32_t remaining_in = entry->compressed_size;
    SceOff offset = entry->data_offset;
    size_t in_pos = 0;
    size_t in_available = 0;
    size_t out_pos = 0;
    uint32_t written = 0;

    tinfl_init(&work->inflator);

    for (;;)
    {
        size_t in_bytes;
        size_t out_bytes;
        tinfl_status status;
        int result;

        if (in_available == 0 && remaining_in > 0)
        {
            SceSize chunk = remaining_in < VPK_IN_SIZE
                ? remaining_in : VPK_IN_SIZE;

            result = read_at(in_fd, work->in, chunk, offset);
            if (result < 0)
                return result;
            offset += chunk;
            remaining_in -= chunk;
            in_pos = 0;
            in_available = chunk;
        }

        in_bytes = in_available;
        out_bytes = VPK_OUT_SIZE - out_pos;
        status = tinfl_decompress(&work->inflator, work->in + in_pos,
            &in_bytes, work->out, work->out + out_pos, &out_bytes,
            remaining_in > 0 ? TINFL_FLAG_HAS_MORE_INPUT : 0);
        in_pos += in_bytes;
        in_available -= in_bytes;

        if (out_bytes > 0)
        {
            if (out_bytes > entry->uncompressed_size - written)
                return VPK_ERROR_CORRUPT;

            result = write_all(out_fd, work->out + out_pos,
                (SceSize)out_bytes);
            if (result < 0)
                return result;
            *crc = vpk_crc32(*crc, work->out + out_pos, out_bytes);
            written += (uint32_t)out_bytes;
            out_pos = (out_pos + out_bytes) & (VPK_OUT_SIZE - 1);
        }

        if (status == TINFL_STATUS_DONE)
            break;
        if (status < 0 ||
            (status == TINFL_STATUS_NEEDS_MORE_INPUT &&
                in_available == 0 && remaining_in == 0))
            return VPK_ERROR_CORRUPT;
    }

    return written == entry->uncompressed_size ? 0 : VPK_ERROR_CORRUPT;
}

static int extract_file(vpk_work *work, SceUID in_fd, const zip_entry *entry)
{
    uint32_t crc = 0;
    SceUID out_fd;
    int result;

    if (entry->method != ZIP_METHOD_STORED &&
        entry->method != ZIP_METHOD_DEFLATED)
        return VPK_ERROR_METHOD;

    out_fd = sceIoOpen(work->path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC,
        0777);
    if (out_fd < 0)
        return VPK_ERROR_WRITE;

    if (entry->method == ZIP_METHOD_STORED)
        result = copy_stored(work, in_fd, out_fd, entry, &crc);
    else
        result = inflate_entry(work, in_fd, out_fd, entry, &crc);

    if (sceIoClose(out_fd) < 0 && result == 0)
        result = VPK_ERROR_WRITE;
    if (result == 0 && crc != entry->crc)
        result = VPK_ERROR_CORRUPT;
    return result;
}

static int extract_archive(vpk_work *work, SceUID fd, const char *dest_dir)
{
    uint8_t header[ZIP_CENTRAL_HEADER_SIZE];
    const uint8_t *end = NULL;
    SceOff file_size;
    SceOff tail_size;
    SceOff position;
    SceOff central_end;
    uint32_t entry_count;
    uint32_t index;
    size_t dest_length = strlen(dest_dir);
    long i;
    int result;

    file_size = sceIoLseek(fd, 0, SCE_SEEK_END);
    if (file_size < ZIP_END_SIZE)
        return VPK_ERROR_NOT_ZIP;

    tail_size = file_size < ZIP_END_SIZE + ZIP_MAX_COMMENT
        ? file_size : ZIP_END_SIZE + ZIP_MAX_COMMENT;
    result = read_at(fd, work->in, (SceSize)tail_size, file_size - tail_size);
    if (result < 0)
        return result;

    for (i = (long)tail_size - ZIP_END_SIZE; i >= 0; --i)
    {
        if (read_le32(work->in + i) == ZIP_END_SIG)
        {
            end = work->in + i;
            break;
        }
    }
    if (!end)
        return VPK_ERROR_NOT_ZIP;

    entry_count = read_le16(end + 10);
    position = read_le32(end + 16);
    central_end = position + read_le32(end + 12);
    if (entry_count == 0xFFFF || read_le32(end + 12) == 0xFFFFFFFF ||
        read_le32(end + 16) == 0xFFFFFFFF)
        return VPK_ERROR_ZIP64;
    if (central_end > file_size)
        return VPK_ERROR_CORRUPT;

    if (dest_length + 2 >= VPK_PATH_MAX)
        return VPK_ERROR_BAD_ENTRY;
    strcpy(work->path, dest_dir);
    make_dirs(work->path, true);

    for (index = 0; index < entry_count; ++index)
    {
        uint32_t flags;
        uint32_t name_length;
        size_t path_length;
        zip_entry entry;
        uint8_t local[ZIP_LOCAL_HEADER_SIZE];
        SceOff local_offset;

        if (position + ZIP_CENTRAL_HEADER_SIZE > central_end)
            return VPK_ERROR_CORRUPT;
        result = read_at(fd, header, sizeof(header), position);
        if (result < 0)
            return result;
        if (read_le32(header) != ZIP_CENTRAL_HEADER_SIG)
            return VPK_ERROR_CORRUPT;

        flags = read_le16(header + 8);
        entry.method = read_le16(header + 10);
        entry.crc = read_le32(header + 16);
        entry.compressed_size = read_le32(header + 20);
        entry.uncompressed_size = read_le32(header + 24);
        name_length = read_le16(header + 28);
        local_offset = read_le32(header + 42);

        if (entry.compressed_size == 0xFFFFFFFF ||
            entry.uncompressed_size == 0xFFFFFFFF ||
            local_offset == 0xFFFFFFFF)
            return VPK_ERROR_ZIP64;
        if (name_length == 0 ||
            dest_length + 1 + name_length >= VPK_PATH_MAX)
            return VPK_ERROR_BAD_ENTRY;

        result = read_at(fd, work->name, name_length,
            position + ZIP_CENTRAL_HEADER_SIZE);
        if (result < 0)
            return result;
        work->name[name_length] = '\0';
        position += ZIP_CENTRAL_HEADER_SIZE + name_length +
            read_le16(header + 30) + read_le16(header + 32);

        if (flags & ZIP_FLAG_ENCRYPTED)
            return VPK_ERROR_ENCRYPTED;
        if (strlen(work->name) != name_length ||
            !vpk_entry_name_normalize(work->name))
            return VPK_ERROR_BAD_ENTRY;

        /* Length was checked against VPK_PATH_MAX above. */
        memcpy(work->path, dest_dir, dest_length);
        work->path[dest_length] = '/';
        memcpy(work->path + dest_length + 1, work->name, name_length + 1);
        path_length = dest_length + 1 + name_length;
        if (work->path[path_length - 1] == '/')
        {
            work->path[path_length - 1] = '\0';
            make_dirs(work->path, true);
            continue;
        }
        make_dirs(work->path, false);

        result = read_at(fd, local, sizeof(local), local_offset);
        if (result < 0)
            return result;
        if (read_le32(local) != ZIP_LOCAL_HEADER_SIG)
            return VPK_ERROR_CORRUPT;

        entry.data_offset = local_offset + ZIP_LOCAL_HEADER_SIZE +
            read_le16(local + 26) + read_le16(local + 28);
        if (entry.data_offset + entry.compressed_size > file_size)
            return VPK_ERROR_CORRUPT;

        result = extract_file(work, fd, &entry);
        if (result < 0)
            return result;
    }

    return 0;
}

int vpk_extract(const char *vpk_path, const char *dest_dir)
{
    SceUID block;
    SceUID fd;
    vpk_work *work;
    int result;

    work = work_alloc(&block);
    if (!work)
        return VPK_ERROR_NO_MEMORY;

    fd = sceIoOpen(vpk_path, SCE_O_RDONLY, 0);
    if (fd < 0)
    {
        sceKernelFreeMemBlock(block);
        return VPK_ERROR_OPEN;
    }

    result = extract_archive(work, fd, dest_dir);

    sceIoClose(fd);
    sceKernelFreeMemBlock(block);
    return result;
}

/*
 * Reads the title ID from param.sfo and writes sce_sys/package/head.bin when
 * the VPK did not include one; the promoter refuses packages without it.
 */
int vpk_prepare_package(const char *pkg_dir,
    char title_id[VPK_TITLE_ID_LENGTH + 1])
{
    char sfo_title_id[16];
    char content_id[49];
    SceIoStat stat;
    SceUID block;
    SceUID fd;
    vpk_work *work;
    int size;
    int result = 0;

    work = work_alloc(&block);
    if (!work)
        return VPK_ERROR_NO_MEMORY;

    snprintf(work->path, VPK_PATH_MAX, "%s/sce_sys/param.sfo", pkg_dir);
    fd = sceIoOpen(work->path, SCE_O_RDONLY, 0);
    if (fd < 0)
    {
        result = VPK_ERROR_PARAM_SFO;
        goto exit;
    }
    size = sceIoRead(fd, work->in, VPK_IN_SIZE);
    sceIoClose(fd);

    if (size <= 0 ||
        vpk_sfo_get_string(work->in, (size_t)size, "TITLE_ID",
            sfo_title_id, sizeof(sfo_title_id)) < 0)
    {
        result = VPK_ERROR_PARAM_SFO;
        goto exit;
    }
    if (!vpk_title_id_is_valid(sfo_title_id))
    {
        result = VPK_ERROR_TITLE_ID;
        goto exit;
    }
    strcpy(title_id, sfo_title_id);

    if (vpk_sfo_get_string(work->in, (size_t)size, "CONTENT_ID",
            content_id, sizeof(content_id)) < 0)
        content_id[0] = '\0';

    snprintf(work->path, VPK_PATH_MAX, "%s/sce_sys/package/head.bin",
        pkg_dir);
    if (sceIoGetstat(work->path, &stat) >= 0)
        goto exit;

    vpk_make_head_bin(title_id, content_id, work->out);
    make_dirs(work->path, false);
    fd = sceIoOpen(work->path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC,
        0777);
    if (fd < 0)
    {
        result = VPK_ERROR_WRITE;
        goto exit;
    }
    result = write_all(fd, work->out, VPK_HEAD_BIN_SIZE);
    if (sceIoClose(fd) < 0 && result == 0)
        result = VPK_ERROR_WRITE;

exit:
    sceKernelFreeMemBlock(block);
    return result;
}

const char *vpk_error_string(int error)
{
    switch (error)
    {
    case VPK_ERROR_NO_MEMORY:
        return "out of memory";
    case VPK_ERROR_OPEN:
        return "cannot open the VPK";
    case VPK_ERROR_READ:
        return "cannot read the VPK";
    case VPK_ERROR_WRITE:
        return "cannot write extracted files";
    case VPK_ERROR_NOT_ZIP:
        return "not a ZIP/VPK file";
    case VPK_ERROR_ZIP64:
        return "ZIP64 archives are not supported";
    case VPK_ERROR_ENCRYPTED:
        return "encrypted archives are not supported";
    case VPK_ERROR_METHOD:
        return "unsupported compression method";
    case VPK_ERROR_BAD_ENTRY:
        return "unsafe or overlong path in archive";
    case VPK_ERROR_CORRUPT:
        return "archive is corrupt";
    case VPK_ERROR_PARAM_SFO:
        return "missing or invalid sce_sys/param.sfo";
    case VPK_ERROR_TITLE_ID:
        return "invalid TITLE_ID in param.sfo";
    default:
        return NULL;
    }
}
