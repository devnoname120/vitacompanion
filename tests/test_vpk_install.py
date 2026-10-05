import hashlib
import pathlib
import random
import struct
import subprocess
import tempfile
import textwrap
import unittest
import zipfile


ROOT = pathlib.Path(__file__).resolve().parents[1]

# Minimal host implementation of the Vita file APIs used by vpk_package.c.
HOST_SHIM = r"""
#pragma once

#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef int SceUID;
typedef unsigned int SceSize;
typedef int64_t SceOff;
typedef int SceMode;

#define SCE_O_RDONLY 0x0001
#define SCE_O_WRONLY 0x0002
#define SCE_O_CREAT 0x0200
#define SCE_O_TRUNC 0x0400
#define SCE_SEEK_END 2
#define SCE_S_ISDIR(m) S_ISDIR(m)
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RW 0x0C20D060

typedef struct {
    SceMode st_mode;
    SceOff st_size;
} SceIoStat;

typedef struct {
    SceIoStat d_stat;
    char d_name[256];
} SceIoDirent;

static void *shim_blocks[16];
static DIR *shim_dirs[16];
static char shim_dir_paths[16][512];

static inline SceUID sceIoOpen(const char *file, int flags, SceMode mode)
{
    int host_flags = (flags & SCE_O_WRONLY) ? O_WRONLY : O_RDONLY;

    if (flags & SCE_O_CREAT)
        host_flags |= O_CREAT;
    if (flags & SCE_O_TRUNC)
        host_flags |= O_TRUNC;
    (void)mode;
    return open(file, host_flags, 0666);
}

static inline int sceIoClose(SceUID fd) { return close(fd); }

static inline int sceIoRead(SceUID fd, void *data, SceSize size)
{
    return (int)read(fd, data, size);
}

static inline int sceIoWrite(SceUID fd, const void *data, SceSize size)
{
    return (int)write(fd, data, size);
}

static inline SceOff sceIoLseek(SceUID fd, SceOff offset, int whence)
{
    return lseek(fd, offset, whence);
}

static inline int sceIoPread(SceUID fd, void *data, SceSize size,
    SceOff offset)
{
    return (int)pread(fd, data, size, offset);
}

static inline int sceIoMkdir(const char *dir, SceMode mode)
{
    return mkdir(dir, mode);
}

static inline int sceIoRmdir(const char *path) { return rmdir(path); }
static inline int sceIoRemove(const char *path) { return remove(path); }

static inline int sceIoGetstat(const char *file, SceIoStat *stat_out)
{
    struct stat st;

    if (stat(file, &st) < 0)
        return -1;
    stat_out->st_mode = st.st_mode;
    stat_out->st_size = st.st_size;
    return 0;
}

static inline SceUID sceIoDopen(const char *dirname)
{
    int i;

    for (i = 0; i < 16; ++i)
    {
        if (!shim_dirs[i])
        {
            shim_dirs[i] = opendir(dirname);
            if (!shim_dirs[i])
                return -1;
            snprintf(shim_dir_paths[i], sizeof(shim_dir_paths[i]), "%s",
                dirname);
            return i;
        }
    }
    return -1;
}

static inline int sceIoDread(SceUID fd, SceIoDirent *dir)
{
    struct dirent *entry = readdir(shim_dirs[fd]);
    char path[1024];

    if (!entry)
        return 0;
    strncpy(dir->d_name, entry->d_name, sizeof(dir->d_name) - 1);
    dir->d_name[sizeof(dir->d_name) - 1] = 0;
    snprintf(path, sizeof(path), "%s/%s", shim_dir_paths[fd],
        entry->d_name);
    sceIoGetstat(path, &dir->d_stat);
    return 1;
}

static inline int sceIoDclose(SceUID fd)
{
    closedir(shim_dirs[fd]);
    shim_dirs[fd] = NULL;
    return 0;
}

static inline SceUID sceKernelAllocMemBlock(const char *name, int type,
    SceSize size, void *opt)
{
    int i;

    (void)name;
    (void)type;
    (void)opt;
    for (i = 0; i < 16; ++i)
    {
        if (!shim_blocks[i])
        {
            shim_blocks[i] = malloc(size);
            return shim_blocks[i] ? i : -1;
        }
    }
    return -1;
}

static inline int sceKernelGetMemBlockBase(SceUID uid, void **base)
{
    *base = shim_blocks[uid];
    return 0;
}

static inline int sceKernelFreeMemBlock(SceUID uid)
{
    free(shim_blocks[uid]);
    shim_blocks[uid] = NULL;
    return 0;
}
"""


def compile_and_run(source, tmp, *args):
    shim = tmp / "shim"
    for header in (
        "psp2/io/fcntl.h",
        "psp2/io/stat.h",
        "psp2/io/dirent.h",
        "psp2/kernel/sysmem.h",
    ):
        path = shim / header
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "vita_host_shim.h"\n')
    (shim / "vita_host_shim.h").write_text(HOST_SHIM)

    source_path = tmp / "test.c"
    executable_path = tmp / "test"
    miniz_object = tmp / "miniz_tinfl.o"
    source_path.write_text(textwrap.dedent(source))
    subprocess.run(
        [
            "cc",
            "-c",
            "-w",
            "-DNDEBUG",
            "-DMINIZ_NO_MALLOC",
            str(ROOT / "vendor" / "miniz" / "miniz_tinfl.c"),
            "-o",
            str(miniz_object),
        ],
        check=True,
    )
    subprocess.run(
        [
            "cc",
            "-std=gnu99",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(shim),
            "-I",
            str(ROOT / "src"),
            "-I",
            str(ROOT / "vendor" / "miniz"),
            str(source_path),
            str(ROOT / "src" / "vpk_format.c"),
            str(ROOT / "src" / "vpk_package.c"),
            str(miniz_object),
            "-o",
            str(executable_path),
        ],
        check=True,
    )
    return subprocess.run(
        [str(executable_path), *map(str, args)], capture_output=True, text=True, cwd=tmp
    )


def pkg_hmac(data):
    sha1 = hashlib.sha1(data).digest()
    buf = bytearray(64)
    buf[0:8] = sha1[4:12]
    buf[8:16] = sha1[4:12]
    buf[16:20] = sha1[12:16]
    buf[20] = sha1[16]
    buf[21:24] = sha1[1:4]
    buf[24:32] = buf[16:24]
    return hashlib.sha1(bytes(buf)).digest()[:16]


def expected_head_bin(title_id, content_id):
    """Reference head.bin built from the layout on psdevwiki's PKG_files page."""
    if not content_id:
        content_id = f"EP9000-{title_id}_00-0000000000000000"
    head = bytearray(0x430)
    struct.pack_into(
        ">IIIIIIQQQ", head, 0, 0x7F504B47, 1, 0x280, 11, 0x190, 3, 0x19000, 0xA90, 0x310
    )
    head[0x30:0x60] = content_id.encode().ljust(48, b"\0")[:48]
    struct.pack_into(
        ">IIIIIIQIII",
        head,
        0xC0,
        0x7F657874,
        1,
        0x40,
        0x180,
        0x100,
        0x410,
        0xDE0,
        0,
        0xC0000002,
        0x420,
    )
    metadata = [
        (0x1, 4, [2]),
        (0x2, 4, [0x15]),
        (0x3, 4, [0x0E]),
        (0x4, 8, [0, 0x19000]),
        (0x5, 4, [0x19670100]),
        (0x8, 8, []),
        (0x9, 8, [0, 0x00240000]),
        (0xD, 0x28, [0, 0xB0]),
        (0xF, 0x48, [0x590, 0x320, 0, 0, 0, 0x01010101]),
        (0x10, 0x38, [0xA30, 0x60, 0xC2000000, 1]),
        (0x11, 0x28, [0x01720000]),
    ]
    offset = 0x280
    for identifier, size, words in metadata:
        struct.pack_into(f">II{len(words)}I", head, offset, identifier, size, *words)
        offset += 8 + size
    assert offset == 0x280 + 0x190 - 0x40
    head[0x100:0x110] = pkg_hmac(bytes(head[:0x100]))
    head[0x410:0x420] = pkg_hmac(bytes(head[0x280:0x3D0]))
    head[0x420:0x430] = pkg_hmac(bytes(head[:0x420]))
    return bytes(head)


def make_sfo(entries):
    keys = b""
    data = b""
    index = b""
    for key, value in entries:
        if isinstance(value, int):
            fmt, raw, max_len = 0x0404, struct.pack("<I", value), 4
        else:
            raw = value.encode() + b"\0"
            fmt, max_len = 0x0204, (len(raw) + 3) & ~3
        index += struct.pack("<HHIII", len(keys), fmt, len(raw), max_len, len(data))
        keys += key.encode() + b"\0"
        data += raw.ljust(max_len, b"\0")
    keys = keys.ljust((len(keys) + 3) & ~3, b"\0")
    key_table = 20 + len(index)
    data_table = key_table + len(keys)
    header = struct.pack(
        "<IIIII", 0x46535000, 0x101, key_table, data_table, len(entries)
    )
    return header + index + keys + data


EXTRACT_PROGRAM = r"""
#include "vpk_package.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    char title_id[VPK_TITLE_ID_LENGTH + 1] = {0};
    int result;

    if (argc < 3)
        return 100;

    if (!strcmp(argv[1], "remove"))
    {
        printf("%d\n", vpk_remove_tree(argv[2]));
        return 0;
    }

    result = vpk_extract(argv[1], argv[2]);
    if (result >= 0)
        result = vpk_prepare_package(argv[2], title_id);
    printf("%d %s\n", result, title_id);
    return 0;
}
"""


class VpkInstallTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = pathlib.Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def run_extract(self, archive, dest="pkg"):
        result = compile_and_run(EXTRACT_PROGRAM, self.tmp, archive, dest)
        self.assertEqual(result.returncode, 0, result.stderr)
        code, _, title_id = result.stdout.strip().partition(" ")
        return int(code), title_id

    def test_head_bin_matches_independent_reference(self):
        result = compile_and_run(
            r"""
            #include "vpk_format.h"

            #include <stdio.h>

            int main(void)
            {
                uint8_t head[VPK_HEAD_BIN_SIZE];
                FILE *f;

                if (vpk_make_head_bin("VITA00001", "", head) < 0)
                    return 1;
                f = fopen("a.bin", "wb");
                fwrite(head, 1, sizeof(head), f);
                fclose(f);

                if (vpk_make_head_bin("ABCD12345",
                        "UP0001-ABCD12345_00-0123456789ABCDEF", head) < 0)
                    return 2;
                f = fopen("b.bin", "wb");
                fwrite(head, 1, sizeof(head), f);
                fclose(f);

                return vpk_make_head_bin("bad", "", head) < 0 ? 0 : 3;
            }
            """,
            self.tmp,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            (self.tmp / "a.bin").read_bytes(), expected_head_bin("VITA00001", "")
        )
        self.assertEqual(
            (self.tmp / "b.bin").read_bytes(),
            expected_head_bin("ABCD12345", "UP0001-ABCD12345_00-0123456789ABCDEF"),
        )

    def test_sfo_title_id_and_entry_names(self):
        (self.tmp / "param.sfo").write_bytes(
            make_sfo(
                [
                    ("APP_VER", "01.00"),
                    ("ATTRIBUTE", 0x8000),
                    ("TITLE", "Test App"),
                    ("TITLE_ID", "VITA00001"),
                ]
            )
        )
        result = compile_and_run(
            r"""
            #include "vpk_format.h"

            #include <stdio.h>
            #include <string.h>

            static int safe(const char *name, const char *expected)
            {
                char buffer[64];

                strcpy(buffer, name);
                if (!vpk_entry_name_normalize(buffer))
                    return 0;
                return strcmp(buffer, expected) == 0;
            }

            static int unsafe(const char *name)
            {
                char buffer[64];

                strcpy(buffer, name);
                return !vpk_entry_name_normalize(buffer);
            }

            int main(void)
            {
                uint8_t sfo[4096];
                char value[32];
                FILE *f = fopen("param.sfo", "rb");
                size_t size = fread(sfo, 1, sizeof(sfo), f);

                fclose(f);
                if (vpk_sfo_get_string(sfo, size, "TITLE_ID", value,
                        sizeof(value)) < 0 || strcmp(value, "VITA00001"))
                    return 1;
                if (vpk_sfo_get_string(sfo, size, "TITLE", value, 5) < 0 ||
                    strcmp(value, "Test"))
                    return 2;
                if (vpk_sfo_get_string(sfo, size, "ATTRIBUTE", value,
                        sizeof(value)) >= 0 ||
                    vpk_sfo_get_string(sfo, size, "CONTENT_ID", value,
                        sizeof(value)) >= 0 ||
                    vpk_sfo_get_string(sfo, 30, "TITLE_ID", value,
                        sizeof(value)) >= 0)
                    return 3;

                if (!vpk_title_id_is_valid("VITA00001") ||
                    vpk_title_id_is_valid("vita00001") ||
                    vpk_title_id_is_valid("VITA0001") ||
                    vpk_title_id_is_valid("VITA000012"))
                    return 4;

                if (!safe("eboot.bin", "eboot.bin") ||
                    !safe("sce_sys\\icon0.png", "sce_sys/icon0.png") ||
                    !safe("sce_sys/", "sce_sys/") ||
                    !safe("a/..b/c", "a/..b/c"))
                    return 5;

                if (!unsafe("") || !unsafe("/eboot.bin") ||
                    !unsafe("../eboot.bin") || !unsafe("a/../../b") ||
                    !unsafe("a\\..\\b") || !unsafe("ux0:app/x") ||
                    !unsafe("a//b") || !unsafe("./a"))
                    return 6;

                return 0;
            }
            """,
            self.tmp,
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def make_vpk(self, name, extra=(), head_bin=None):
        rng = random.Random(1234)
        files = {
            "eboot.bin": bytes(rng.getrandbits(8) for _ in range(300_000)),
            "sce_sys/param.sfo": make_sfo(
                [
                    ("CONTENT_ID", "UP0001-VITA00001_00-0000000000000000"),
                    ("TITLE_ID", "VITA00001"),
                ]
            ),
            "sce_sys/livearea/contents/template.xml": b"<livearea/>\n" * 50_000,
            "data/empty.txt": b"",
            "data/stored.bin": bytes(range(256)) * 2000,
        }
        if head_bin is not None:
            files["sce_sys/package/head.bin"] = head_bin
        path = self.tmp / name
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr(zipfile.ZipInfo("sce_sys/"), b"")
            for file_name, data in files.items():
                method = (
                    zipfile.ZIP_STORED
                    if file_name.endswith("stored.bin")
                    else zipfile.ZIP_DEFLATED
                )
                archive.writestr(file_name, data, compress_type=method)
            for file_name, data in extra:
                archive.writestr(file_name, data)
        return path, files

    def test_extracts_vpk_and_creates_head_bin(self):
        archive, files = self.make_vpk("app.vpk")

        code, title_id = self.run_extract(archive)

        self.assertEqual((code, title_id), (0, "VITA00001"))
        for file_name, data in files.items():
            self.assertEqual(
                (self.tmp / "pkg" / file_name).read_bytes(), data, file_name
            )
        self.assertEqual(
            (self.tmp / "pkg/sce_sys/package/head.bin").read_bytes(),
            expected_head_bin("VITA00001", "UP0001-VITA00001_00-0000000000000000"),
        )

    def test_keeps_head_bin_shipped_in_vpk(self):
        archive, _ = self.make_vpk("app.vpk", head_bin=b"original")

        code, _ = self.run_extract(archive)

        self.assertEqual(code, 0)
        self.assertEqual(
            (self.tmp / "pkg/sce_sys/package/head.bin").read_bytes(), b"original"
        )

    def test_rejects_path_traversal(self):
        archive, _ = self.make_vpk("evil.vpk", extra=[("../escape.txt", b"x")])

        code, _ = self.run_extract(archive)

        self.assertEqual(code, -9)
        self.assertFalse((self.tmp / "escape.txt").exists())

    def test_rejects_corrupted_data(self):
        archive, _ = self.make_vpk("corrupt.vpk")
        data = bytearray(archive.read_bytes())
        offset = data.index(bytes(range(256))) + 100
        data[offset] ^= 0xFF
        archive.write_bytes(bytes(data))

        code, _ = self.run_extract(archive)

        self.assertEqual(code, -10)

    def test_rejects_non_zip_and_missing_title(self):
        (self.tmp / "plain.vpk").write_bytes(b"not a zip" * 100)
        self.assertEqual(self.run_extract(self.tmp / "plain.vpk")[0], -5)

        with zipfile.ZipFile(self.tmp / "nosfo.vpk", "w") as archive:
            archive.writestr("eboot.bin", b"x")
        self.assertEqual(self.run_extract(self.tmp / "nosfo.vpk", "pkg2")[0], -11)

    def test_remove_tree_deletes_nested_directories(self):
        nested = self.tmp / "tree/a/b/c"
        nested.mkdir(parents=True)
        (nested / "file.txt").write_text("x")
        (self.tmp / "tree/top.txt").write_text("y")

        result = compile_and_run(EXTRACT_PROGRAM, self.tmp, "remove", "tree")

        self.assertEqual(result.stdout.strip(), "0", result.stderr)
        self.assertFalse((self.tmp / "tree").exists())


if __name__ == "__main__":
    unittest.main()
