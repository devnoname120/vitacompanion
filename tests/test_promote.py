import pathlib
import subprocess
import tempfile
import textwrap
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


def compile_and_run(source, support_sources, headers):
    with tempfile.TemporaryDirectory() as tmpdir:
        tmp = pathlib.Path(tmpdir)
        source_path = tmp / "test.c"
        executable_path = tmp / "test"
        source_path.write_text(textwrap.dedent(source))

        for relative_path, contents in headers.items():
            header_path = tmp / relative_path
            header_path.parent.mkdir(parents=True, exist_ok=True)
            header_path.write_text(textwrap.dedent(contents))

        subprocess.run(
            [
                "cc",
                "-std=c99",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(tmp),
                "-I",
                str(ROOT / "src"),
                str(source_path),
                *(str(ROOT / source_name) for source_name in support_sources),
                "-o",
                str(executable_path),
            ],
            check=True,
            cwd=ROOT,
        )
        subprocess.run([str(executable_path)], check=True, cwd=ROOT)


class PromoteTests(unittest.TestCase):
    def test_directory_preparation_and_promoter_lifecycle(self):
        compile_and_run(
            r"""
            #include "promote.h"
            #include "sha1.h"

            #include <psp2/io/fcntl.h>
            #include <psp2/io/stat.h>
            #include <psp2/promoterutil.h>
            #include <psp2/sysmodule.h>
            #include <stdint.h>
            #include <stdio.h>
            #include <string.h>

            #define ROOT_PATH "ux0:data/promote-test"
            #define PARAM_PATH ROOT_PATH "/sce_sys/param.sfo"
            #define PACKAGE_PATH ROOT_PATH "/sce_sys/package"
            #define HEAD_PATH PACKAGE_PATH "/head.bin"
            #define HEAD_TEMP_PATH PACKAGE_PATH "/head.bin.tmp"

            typedef struct __attribute__((packed)) {
                uint32_t magic;
                uint32_t version;
                uint32_t key_offset;
                uint32_t value_offset;
                uint32_t count;
            } TestSfoHeader;

            typedef struct __attribute__((packed)) {
                uint16_t name_offset;
                uint8_t alignment;
                uint8_t type;
                uint32_t value_size;
                uint32_t total_size;
                uint32_t data_offset;
            } TestSfoEntry;

            static uint8_t sfo_data[512];
            static size_t sfo_size;
            static uint8_t written_head[2048];
            static size_t written_head_size;
            static size_t read_offset;
            static int root_is_directory;
            static int param_exists;
            static int head_exists;
            static int package_exists;
            static int module_calls;
            static int promoter_load_result;
            static int promoter_init_result;
            static int promoter_result;
            static char sequence[32];
            static size_t sequence_length;

            static void append_sequence(char value)
            {
                sequence[sequence_length++] = value;
                sequence[sequence_length] = '\0';
            }

            static void reset_state(void)
            {
                memset(sfo_data, 0, sizeof(sfo_data));
                memset(written_head, 0, sizeof(written_head));
                memset(sequence, 0, sizeof(sequence));
                sfo_size = 0;
                written_head_size = 0;
                read_offset = 0;
                root_is_directory = 1;
                param_exists = 1;
                head_exists = 0;
                package_exists = 0;
                module_calls = 0;
                promoter_load_result = 0;
                promoter_init_result = 0;
                promoter_result = 0;
                sequence_length = 0;
            }

            static void build_sfo(const char *title_id, const char *content_id)
            {
                TestSfoHeader *header = (TestSfoHeader *)sfo_data;
                TestSfoEntry *entries =
                    (TestSfoEntry *)(sfo_data + sizeof(*header));
                uint32_t count = content_id ? 2 : 1;
                uint32_t key_offset = sizeof(*header) +
                    count * sizeof(*entries);
                uint32_t title_key_offset = 0;
                uint32_t content_key_offset = 9;
                uint32_t value_offset;
                uint32_t cursor;

                memset(sfo_data, 0, sizeof(sfo_data));
                memcpy(sfo_data + key_offset, "TITLE_ID\0", 9);
                if (content_id)
                    memcpy(sfo_data + key_offset + content_key_offset,
                        "CONTENT_ID\0", 11);

                value_offset = key_offset + (content_id ? 20 : 9);
                value_offset = (value_offset + 3) & ~3U;
                cursor = value_offset;

                header->magic = 0x46535000;
                header->version = 0x00000101;
                header->key_offset = key_offset;
                header->value_offset = value_offset;
                header->count = count;

                entries[0].name_offset = title_key_offset;
                entries[0].alignment = 4;
                entries[0].type = 2;
                entries[0].value_size = (uint32_t)strlen(title_id) + 1;
                entries[0].total_size = 12;
                entries[0].data_offset = 0;
                memcpy(sfo_data + cursor, title_id,
                    entries[0].value_size);
                cursor += entries[0].total_size;

                if (content_id)
                {
                    entries[1].name_offset = content_key_offset;
                    entries[1].alignment = 4;
                    entries[1].type = 2;
                    entries[1].value_size =
                        (uint32_t)strlen(content_id) + 1;
                    entries[1].total_size = 48;
                    entries[1].data_offset = entries[0].total_size;
                    memcpy(sfo_data + cursor, content_id,
                        entries[1].value_size);
                    cursor += entries[1].total_size;
                }

                sfo_size = cursor;
            }

            int sceIoGetstat(const char *path, SceIoStat *stat)
            {
                memset(stat, 0, sizeof(*stat));
                if (strcmp(path, ROOT_PATH) == 0 ||
                    strcmp(path, ROOT_PATH ".vpk") == 0)
                {
                    stat->st_mode = root_is_directory
                        ? SCE_S_IFDIR : SCE_S_IFREG;
                    return 0;
                }
                if (strcmp(path, PARAM_PATH) == 0 && param_exists)
                {
                    stat->st_mode = SCE_S_IFREG;
                    stat->st_size = sfo_size;
                    return 0;
                }
                if (strcmp(path, PACKAGE_PATH) == 0 && package_exists)
                {
                    stat->st_mode = SCE_S_IFDIR;
                    return 0;
                }
                if (strcmp(path, HEAD_PATH) == 0 && head_exists)
                {
                    stat->st_mode = SCE_S_IFREG;
                    stat->st_size = 1072;
                    return 0;
                }
                return -2;
            }

            SceUID sceIoOpen(const char *path, int flags, SceMode mode)
            {
                (void)mode;
                if (strcmp(path, PARAM_PATH) == 0 &&
                    flags == SCE_O_RDONLY && param_exists)
                {
                    read_offset = 0;
                    return 10;
                }
                if (strcmp(path, HEAD_TEMP_PATH) == 0 &&
                    (flags & SCE_O_WRONLY) != 0)
                {
                    written_head_size = 0;
                    return 20;
                }
                return -3;
            }

            int sceIoRead(SceUID fd, void *buffer, SceSize size)
            {
                size_t remaining;
                size_t amount;

                if (fd != 10)
                    return -4;
                remaining = sfo_size - read_offset;
                amount = size < remaining ? size : remaining;
                if (amount > 7)
                    amount = 7;
                memcpy(buffer, sfo_data + read_offset, amount);
                read_offset += amount;
                return (int)amount;
            }

            int sceIoWrite(SceUID fd, const void *buffer, SceSize size)
            {
                size_t amount = size;

                if (fd != 20)
                    return -5;
                if (amount > 13)
                    amount = 13;
                memcpy(written_head + written_head_size, buffer, amount);
                written_head_size += amount;
                return (int)amount;
            }

            int sceIoClose(SceUID fd)
            {
                return fd == 10 || fd == 20 ? 0 : -6;
            }

            int sceIoMkdir(const char *path, SceMode mode)
            {
                (void)mode;
                if (strcmp(path, PACKAGE_PATH) != 0)
                    return -7;
                if (package_exists)
                    return -8;
                package_exists = 1;
                return 0;
            }

            int sceIoRename(const char *old_path, const char *new_path)
            {
                if (strcmp(old_path, HEAD_TEMP_PATH) != 0 ||
                    strcmp(new_path, HEAD_PATH) != 0)
                    return -9;
                head_exists = 1;
                return 0;
            }

            int sceIoRemove(const char *path)
            {
                return strcmp(path, HEAD_TEMP_PATH) == 0 ? 0 : -10;
            }

            int sceSysmoduleLoadModuleInternalWithArg(
                SceSysmoduleInternalModuleId id, SceSize args,
                void *argp, const SceSysmoduleOpt *option)
            {
                const uint32_t *values = argp;

                ++module_calls;
                append_sequence('P');
                if (id != SCE_SYSMODULE_INTERNAL_PAF ||
                    args != 6 * sizeof(uint32_t) || !values || !option ||
                    values[0] != 0x180000 || values[3] != 1 ||
                    option->flags != (int)sizeof(*option) ||
                    !option->result)
                    return -11;
                return 0;
            }

            int sceSysmoduleUnloadModuleInternalWithArg(
                SceSysmoduleInternalModuleId id, SceSize args,
                void *argp, const SceSysmoduleOpt *option)
            {
                ++module_calls;
                append_sequence('Q');
                return id == SCE_SYSMODULE_INTERNAL_PAF && args == 0 &&
                    !argp && option ? 0 : -12;
            }

            int sceSysmoduleLoadModuleInternal(
                SceSysmoduleInternalModuleId id)
            {
                ++module_calls;
                append_sequence('L');
                if (id != SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL)
                    return -13;
                return promoter_load_result;
            }

            int sceSysmoduleUnloadModuleInternal(
                SceSysmoduleInternalModuleId id)
            {
                ++module_calls;
                append_sequence('U');
                return id == SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL
                    ? 0 : -14;
            }

            int scePromoterUtilityInit(void)
            {
                append_sequence('I');
                return promoter_init_result;
            }

            int scePromoterUtilityPromotePkgWithRif(
                const char *path, int sync)
            {
                append_sequence('R');
                if (strcmp(path, ROOT_PATH) != 0 || sync != 1)
                    return -15;
                return promoter_result;
            }

            int scePromoterUtilityExit(void)
            {
                append_sequence('E');
                return 0;
            }

            static int expect_bytes(size_t offset, const uint8_t *expected,
                size_t length)
            {
                return memcmp(written_head + offset, expected, length) == 0;
            }

            int main(void)
            {
                static const uint8_t sha1_abc[20] = {
                    0xA9, 0x99, 0x3E, 0x36, 0x47, 0x06, 0x81, 0x6A,
                    0xBA, 0x3E, 0x25, 0x71, 0x78, 0x50, 0xC2, 0x6C,
                    0x9C, 0xD0, 0xD8, 0x9D
                };
                static const uint8_t fallback_hmac_header[16] = {
                    0x9F, 0x18, 0x73, 0x58, 0x47, 0xF4, 0x28, 0x41,
                    0xBA, 0xB5, 0x83, 0xEE, 0xFB, 0x9A, 0xB5, 0xD3
                };
                static const uint8_t fallback_hmac_info[16] = {
                    0x51, 0x51, 0x31, 0x57, 0x32, 0xFE, 0xE0, 0x19,
                    0xBB, 0xB5, 0xCA, 0xE3, 0x42, 0xB4, 0x4D, 0xC2
                };
                static const uint8_t fallback_hmac_all[16] = {
                    0xF1, 0x94, 0x08, 0x6F, 0x25, 0xD4, 0xB5, 0x5A,
                    0x11, 0x26, 0xBA, 0xA6, 0x6B, 0xD9, 0xB0, 0xD2
                };
                static const uint8_t explicit_hmac_header[16] = {
                    0x17, 0xD3, 0xE0, 0x6F, 0x58, 0x37, 0xE7, 0xB0,
                    0xE0, 0xE7, 0x4C, 0x70, 0x7D, 0x29, 0xEB, 0x83
                };
                static const uint8_t explicit_hmac_all[16] = {
                    0x09, 0xDD, 0xE1, 0x70, 0x2A, 0x58, 0x12, 0x59,
                    0x63, 0xAC, 0x58, 0xDC, 0x07, 0xB8, 0x93, 0x6B
                };
                uint8_t digest[20];
                int result;

                vitacompanion_sha1_digest("abc", 3, digest);
                if (memcmp(digest, sha1_abc, sizeof(digest)) != 0)
                    return 1;

                reset_state();
                build_sfo("TEST00001", 0);
                root_is_directory = 0;
                result = promote_directory(ROOT_PATH ".vpk");
                if (result != VITACOMPANION_PROMOTE_ERROR_NOT_DIRECTORY ||
                    module_calls != 0 || written_head_size != 0)
                    return 2;

                reset_state();
                build_sfo("TEST00001", 0);
                head_exists = 1;
                result = promote_directory(ROOT_PATH);
                if (result != 0 || strcmp(sequence, "PLIREUQ") != 0 ||
                    written_head_size != 0)
                    return 3;

                reset_state();
                build_sfo("TEST00001", 0);
                result = promote_directory(ROOT_PATH);
                if (result != 0 || strcmp(sequence, "PLIREUQ") != 0 ||
                    written_head_size != 1072 || !head_exists ||
                    memcmp(written_head + 0x30,
                        "EP9000-TEST00001_00-0000000000000000", 36) != 0 ||
                    !expect_bytes(0x100, fallback_hmac_header, 16) ||
                    !expect_bytes(0x410, fallback_hmac_info, 16) ||
                    !expect_bytes(0x420, fallback_hmac_all, 16))
                    return 4;

                reset_state();
                build_sfo("TEST00001",
                    "UP0001-TEST00001_00-CONTENTID0000001");
                result = promote_directory(ROOT_PATH);
                if (result != 0 ||
                    memcmp(written_head + 0x30,
                        "UP0001-TEST00001_00-CONTENTID0000001", 36) != 0 ||
                    !expect_bytes(0x100, explicit_hmac_header, 16) ||
                    !expect_bytes(0x410, fallback_hmac_info, 16) ||
                    !expect_bytes(0x420, explicit_hmac_all, 16))
                    return 5;

                reset_state();
                build_sfo("TEST00001", 0);
                param_exists = 0;
                result = promote_directory(ROOT_PATH);
                if (result != VITACOMPANION_PROMOTE_ERROR_INVALID_LAYOUT ||
                    module_calls != 0 || written_head_size != 0)
                    return 6;

                reset_state();
                build_sfo("test00001", 0);
                result = promote_directory(ROOT_PATH);
                if (result != VITACOMPANION_PROMOTE_ERROR_INVALID_TITLE_ID ||
                    module_calls != 0 || written_head_size != 0)
                    return 7;

                reset_state();
                build_sfo("TEST00001", 0);
                head_exists = 1;
                promoter_result = -99;
                result = promote_directory(ROOT_PATH);
                if (result != -99 || strcmp(sequence, "PLIREUQ") != 0)
                    return 8;

                reset_state();
                build_sfo("TEST00001", 0);
                head_exists = 1;
                promoter_init_result = -98;
                result = promote_directory(ROOT_PATH);
                if (result != -98 || strcmp(sequence, "PLIUQ") != 0)
                    return 9;

                reset_state();
                build_sfo("TEST00001", 0);
                head_exists = 1;
                promoter_load_result = -97;
                result = promote_directory(ROOT_PATH);
                if (result != -97 || strcmp(sequence, "PLQ") != 0)
                    return 10;

                return 0;
            }
            """,
            ("src/promote.c", "src/promote_head_template.c", "src/sha1.c"),
            {
                "psp2/io/fcntl.h": r"""
                    #ifndef TEST_PSP2_IO_FCNTL_H
                    #define TEST_PSP2_IO_FCNTL_H
                    #include <stddef.h>
                    typedef int SceUID;
                    typedef unsigned int SceMode;
                    #ifndef TEST_SCE_SIZE_DEFINED
                    #define TEST_SCE_SIZE_DEFINED
                    typedef size_t SceSize;
                    #endif
                    #define SCE_O_RDONLY 0x0001
                    #define SCE_O_WRONLY 0x0002
                    #define SCE_O_CREAT  0x0200
                    #define SCE_O_TRUNC  0x0400
                    SceUID sceIoOpen(const char *, int, SceMode);
                    int sceIoRead(SceUID, void *, SceSize);
                    int sceIoWrite(SceUID, const void *, SceSize);
                    int sceIoClose(SceUID);
                    int sceIoRename(const char *, const char *);
                    int sceIoRemove(const char *);
                    #endif
                """,
                "psp2/io/stat.h": r"""
                    #ifndef TEST_PSP2_IO_STAT_H
                    #define TEST_PSP2_IO_STAT_H
                    #include <stdint.h>
                    #include <psp2/io/fcntl.h>
                    typedef struct SceIoStat {
                        uint32_t st_mode;
                        uint64_t st_size;
                    } SceIoStat;
                    #define SCE_S_IFMT  0xF000
                    #define SCE_S_IFREG 0x2000
                    #define SCE_S_IFDIR 0x1000
                    #define SCE_S_ISREG(m) (((m) & SCE_S_IFMT) == SCE_S_IFREG)
                    #define SCE_S_ISDIR(m) (((m) & SCE_S_IFMT) == SCE_S_IFDIR)
                    int sceIoGetstat(const char *, SceIoStat *);
                    int sceIoMkdir(const char *, SceMode);
                    #endif
                """,
                "psp2/promoterutil.h": r"""
                    #ifndef TEST_PSP2_PROMOTERUTIL_H
                    #define TEST_PSP2_PROMOTERUTIL_H
                    int scePromoterUtilityInit(void);
                    int scePromoterUtilityExit(void);
                    int scePromoterUtilityPromotePkgWithRif(
                        const char *, int);
                    #endif
                """,
                "psp2/sysmodule.h": r"""
                    #ifndef TEST_PSP2_SYSMODULE_H
                    #define TEST_PSP2_SYSMODULE_H
                    #include <stddef.h>
                    #ifndef TEST_SCE_SIZE_DEFINED
                    #define TEST_SCE_SIZE_DEFINED
                    typedef size_t SceSize;
                    #endif
                    typedef enum SceSysmoduleInternalModuleId {
                        SCE_SYSMODULE_INTERNAL_PAF = 0x80000008,
                        SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL = 0x80000024
                    } SceSysmoduleInternalModuleId;
                    typedef struct SceSysmoduleOpt {
                        int flags;
                        int *result;
                        int unused[2];
                    } SceSysmoduleOpt;
                    int sceSysmoduleLoadModuleInternal(
                        SceSysmoduleInternalModuleId);
                    int sceSysmoduleUnloadModuleInternal(
                        SceSysmoduleInternalModuleId);
                    int sceSysmoduleLoadModuleInternalWithArg(
                        SceSysmoduleInternalModuleId, SceSize,
                        void *, const SceSysmoduleOpt *);
                    int sceSysmoduleUnloadModuleInternalWithArg(
                        SceSysmoduleInternalModuleId, SceSize,
                        void *, const SceSysmoduleOpt *);
                    #endif
                """,
            },
        )

    def test_promote_command_is_documented_and_rejects_files_explicitly(self):
        command_source = (ROOT / "src" / "cmd_definitions.c").read_text()
        cmake_source = (ROOT / "CMakeLists.txt").read_text()
        readme = (ROOT / "README.md").read_text()

        self.assertIn('{.name = "promote"', command_source)
        self.assertIn("Only directory paths are supported", command_source)
        self.assertIn("src/promote.c", cmake_source)
        self.assertIn("ScePromoterUtil_stub", cmake_source)
        self.assertIn("| `promote`", readme)


if __name__ == "__main__":
    unittest.main()
