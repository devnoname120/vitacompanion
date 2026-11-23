import pathlib
import re
import subprocess
import tempfile
import textwrap
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class TransferPoolRuntimeTests(unittest.TestCase):
    def test_four_slot_admission_lifecycle_and_path_conflicts(self):
        source = ROOT / "vendor" / "libftpvita" / "transfer_pool.c"
        header = ROOT / "vendor" / "libftpvita" / "transfer_pool.h"
        self.assertTrue(source.is_file())
        self.assertTrue(header.is_file())

        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = pathlib.Path(tmpdir)
            include = tmp / "include" / "psp2" / "kernel"
            include.mkdir(parents=True)
            (include / "threadmgr.h").write_text(
                textwrap.dedent(
                    """
                    #pragma once

                    typedef int SceUID;

                    SceUID sceKernelCreateMutex(const char *name, int attr,
                        int init_count, const void *option);
                    int sceKernelDeleteMutex(SceUID mutex_id);
                    int sceKernelLockMutex(SceUID mutex_id, int lock_count,
                        unsigned int *timeout);
                    int sceKernelUnlockMutex(SceUID mutex_id, int unlock_count);
                    """
                )
            )

            harness = tmp / "transfer_pool_harness.c"
            harness.write_text(
                textwrap.dedent(
                    r"""
                    #include "transfer_pool.h"

                    #include <psp2/kernel/threadmgr.h>
                    #include <stdint.h>
                    #include <stdio.h>
                    #include <stdlib.h>
                    #include <string.h>

                    static unsigned char storage[8][256];
                    static int allocation_calls;
                    static int free_calls;
                    static int fail_allocation_at;
                    static int create_mutex_result;
                    static int create_mutex_calls;
                    static int delete_mutex_calls;
                    static int lock_result;
                    static int lock_calls;
                    static int unlock_calls;

                    static void expect(int condition, int code)
                    {
                        if (condition)
                            return;
                        fprintf(stderr, "transfer pool assertion %d failed\n", code);
                        exit(code);
                    }

                    static void reset_mocks(void)
                    {
                        allocation_calls = 0;
                        free_calls = 0;
                        fail_allocation_at = -1;
                        create_mutex_result = 41;
                        create_mutex_calls = 0;
                        delete_mutex_calls = 0;
                        lock_result = 0;
                        lock_calls = 0;
                        unlock_calls = 0;
                        memset(storage, 0, sizeof(storage));
                    }

                    void *pool_test_malloc(size_t size)
                    {
                        int index = allocation_calls++;
                        if (index == fail_allocation_at || index >= 8 || size > sizeof(storage[0]))
                            return NULL;
                        return storage[index];
                    }

                    void pool_test_free(void *ptr)
                    {
                        if (ptr)
                            ++free_calls;
                    }

                    SceUID sceKernelCreateMutex(const char *name, int attr,
                        int init_count, const void *option)
                    {
                        (void)name;
                        (void)attr;
                        (void)init_count;
                        (void)option;
                        ++create_mutex_calls;
                        return create_mutex_result;
                    }

                    int sceKernelDeleteMutex(SceUID mutex_id)
                    {
                        expect(mutex_id == create_mutex_result, 1);
                        ++delete_mutex_calls;
                        return 0;
                    }

                    int sceKernelLockMutex(SceUID mutex_id, int lock_count,
                        unsigned int *timeout)
                    {
                        (void)mutex_id;
                        (void)lock_count;
                        (void)timeout;
                        ++lock_calls;
                        return lock_result;
                    }

                    int sceKernelUnlockMutex(SceUID mutex_id, int unlock_count)
                    {
                        (void)mutex_id;
                        (void)unlock_count;
                        ++unlock_calls;
                        return 0;
                    }

                    static void test_capacity_and_reuse(void)
                    {
                        void *buffers[5] = {0};
                        unsigned int slots[5] = {0};
                        int index;

                        reset_mocks();
                        expect(ftpvita_transfer_pool_init(64) == 0, 10);
                        expect(allocation_calls == FTPVITA_TRANSFER_POOL_SLOTS, 11);
                        expect(create_mutex_calls == 1, 12);
                        expect(ftpvita_transfer_pool_available() == 4, 13);

                        for (index = 0; index < 4; ++index) {
                            char path[32];
                            snprintf(path, sizeof(path), "/file-%d", index);
                            expect(ftpvita_transfer_pool_acquire(path,
                                FTPVITA_TRANSFER_WRITE, &buffers[index],
                                &slots[index]) == FTPVITA_TRANSFER_OK, 20 + index);
                            expect(buffers[index] != NULL, 30 + index);
                            if (index > 0)
                                expect(buffers[index] != buffers[index - 1], 40 + index);
                        }
                        expect(ftpvita_transfer_pool_available() == 0, 50);
                        expect(ftpvita_transfer_pool_acquire("/fifth",
                            FTPVITA_TRANSFER_WRITE, &buffers[4], &slots[4]) ==
                            FTPVITA_TRANSFER_NO_SLOT, 51);
                        expect(buffers[4] == NULL, 52);

                        ftpvita_transfer_pool_release(slots[1]);
                        expect(ftpvita_transfer_pool_available() == 1, 53);
                        expect(ftpvita_transfer_pool_acquire("/replacement",
                            FTPVITA_TRANSFER_WRITE, &buffers[4], &slots[4]) ==
                            FTPVITA_TRANSFER_OK, 54);
                        expect(slots[4] == slots[1], 55);
                        expect(buffers[4] == buffers[1], 56);

                        ftpvita_transfer_pool_release(slots[0]);
                        ftpvita_transfer_pool_release(slots[2]);
                        ftpvita_transfer_pool_release(slots[3]);
                        ftpvita_transfer_pool_release(slots[4]);
                        expect(ftpvita_transfer_pool_available() == 4, 57);
                        ftpvita_transfer_pool_fini();
                        expect(free_calls == 4, 58);
                        expect(delete_mutex_calls == 1, 59);
                    }

                    static void test_path_conflicts(void)
                    {
                        void *first = NULL;
                        void *second = NULL;
                        void *rejected = NULL;
                        unsigned int first_slot = 0;
                        unsigned int second_slot = 0;
                        unsigned int rejected_slot = 0;

                        reset_mocks();
                        expect(ftpvita_transfer_pool_init(64) == 0, 60);
                        expect(ftpvita_transfer_pool_acquire("/same",
                            FTPVITA_TRANSFER_READ, &first, &first_slot) ==
                            FTPVITA_TRANSFER_OK, 61);
                        expect(ftpvita_transfer_pool_acquire("/same",
                            FTPVITA_TRANSFER_READ, &second, &second_slot) ==
                            FTPVITA_TRANSFER_OK, 62);
                        expect(ftpvita_transfer_pool_acquire("/same",
                            FTPVITA_TRANSFER_WRITE, &rejected, &rejected_slot) ==
                            FTPVITA_TRANSFER_PATH_BUSY, 63);
                        ftpvita_transfer_pool_release(first_slot);
                        ftpvita_transfer_pool_release(second_slot);

                        expect(ftpvita_transfer_pool_acquire("/same",
                            FTPVITA_TRANSFER_WRITE, &first, &first_slot) ==
                            FTPVITA_TRANSFER_OK, 64);
                        expect(ftpvita_transfer_pool_acquire("/same",
                            FTPVITA_TRANSFER_READ, &rejected, &rejected_slot) ==
                            FTPVITA_TRANSFER_PATH_BUSY, 65);
                        expect(ftpvita_transfer_pool_acquire("/other",
                            FTPVITA_TRANSFER_READ, &second, &second_slot) ==
                            FTPVITA_TRANSFER_OK, 66);
                        ftpvita_transfer_pool_release(first_slot);
                        ftpvita_transfer_pool_release(second_slot);
                        ftpvita_transfer_pool_fini();
                    }

                    static void test_path_alias_conflicts(void)
                    {
                        void *first = NULL;
                        void *second = NULL;
                        void *rejected = NULL;
                        unsigned int first_slot = 0;
                        unsigned int second_slot = 0;
                        unsigned int rejected_slot = 0;

                        reset_mocks();
                        expect(ftpvita_transfer_pool_init(64) == 0, 90);

                        expect(ftpvita_transfer_pool_acquire(
                            "ux0:/data/File.bin", FTPVITA_TRANSFER_READ,
                            &first, &first_slot) == FTPVITA_TRANSFER_OK, 91);
                        expect(ftpvita_transfer_pool_acquire(
                            "UX0:/DATA/file.BIN", FTPVITA_TRANSFER_READ,
                            &second, &second_slot) == FTPVITA_TRANSFER_OK, 92);
                        expect(ftpvita_transfer_pool_acquire(
                            "ux0:/data/file.bin", FTPVITA_TRANSFER_WRITE,
                            &rejected, &rejected_slot) ==
                            FTPVITA_TRANSFER_PATH_BUSY, 93);
                        ftpvita_transfer_pool_release(first_slot);
                        ftpvita_transfer_pool_release(second_slot);

                        expect(ftpvita_transfer_pool_acquire(
                            "ux0:data/file.bin", FTPVITA_TRANSFER_WRITE,
                            &first, &first_slot) == FTPVITA_TRANSFER_OK, 94);
                        expect(ftpvita_transfer_pool_acquire(
                            "ux0:/data/file.bin", FTPVITA_TRANSFER_READ,
                            &rejected, &rejected_slot) ==
                            FTPVITA_TRANSFER_PATH_BUSY, 95);
                        ftpvita_transfer_pool_release(first_slot);

                        expect(ftpvita_transfer_pool_acquire(
                            "UX0:DATA/File.bin", FTPVITA_TRANSFER_WRITE,
                            &first, &first_slot) == FTPVITA_TRANSFER_OK, 96);
                        expect(ftpvita_transfer_pool_acquire(
                            "ux0:/data/file.bin", FTPVITA_TRANSFER_WRITE,
                            &rejected, &rejected_slot) ==
                            FTPVITA_TRANSFER_PATH_BUSY, 97);
                        ftpvita_transfer_pool_release(first_slot);

                        expect(ftpvita_transfer_pool_acquire(
                            "ux0:/data/file.bin", FTPVITA_TRANSFER_WRITE,
                            &first, &first_slot) == FTPVITA_TRANSFER_OK, 100);
                        expect(ftpvita_transfer_pool_acquire(
                            "ux0:/data//file.bin", FTPVITA_TRANSFER_READ,
                            &rejected, &rejected_slot) ==
                            FTPVITA_TRANSFER_PATH_BUSY, 101);
                        expect(ftpvita_transfer_pool_acquire(
                            "ux0:/data/./file.bin", FTPVITA_TRANSFER_READ,
                            &rejected, &rejected_slot) ==
                            FTPVITA_TRANSFER_PATH_BUSY, 102);
                        expect(ftpvita_transfer_pool_acquire(
                            "ux0:/data/sub/../file.bin", FTPVITA_TRANSFER_READ,
                            &rejected, &rejected_slot) ==
                            FTPVITA_TRANSFER_PATH_BUSY, 103);
                        ftpvita_transfer_pool_release(first_slot);

                        expect(ftpvita_transfer_pool_acquire(
                            "ux0:/file.bin", FTPVITA_TRANSFER_WRITE,
                            &first, &first_slot) == FTPVITA_TRANSFER_OK, 104);
                        expect(ftpvita_transfer_pool_acquire(
                            "ux0:/../../file.bin", FTPVITA_TRANSFER_READ,
                            &rejected, &rejected_slot) ==
                            FTPVITA_TRANSFER_PATH_BUSY, 105);
                        expect(ftpvita_transfer_pool_acquire(
                            "ux0:data/../../file.bin", FTPVITA_TRANSFER_READ,
                            &rejected, &rejected_slot) ==
                            FTPVITA_TRANSFER_PATH_BUSY, 106);
                        ftpvita_transfer_pool_release(first_slot);

                        expect(ftpvita_transfer_pool_acquire(
                            "ux0:/data/file.bin", FTPVITA_TRANSFER_READ,
                            &first, &first_slot) == FTPVITA_TRANSFER_OK, 107);
                        expect(ftpvita_transfer_pool_acquire(
                            "UX0:DATA//sub/.././File.BIN",
                            FTPVITA_TRANSFER_READ, &second, &second_slot) ==
                            FTPVITA_TRANSFER_OK, 108);
                        expect(ftpvita_transfer_pool_acquire(
                            "ux0:/data/file.bin", FTPVITA_TRANSFER_WRITE,
                            &rejected, &rejected_slot) ==
                            FTPVITA_TRANSFER_PATH_BUSY, 109);
                        ftpvita_transfer_pool_release(first_slot);
                        ftpvita_transfer_pool_release(second_slot);

                        expect(ftpvita_transfer_pool_acquire(
                            "ux0:/data/\xC3\x84.bin", FTPVITA_TRANSFER_WRITE,
                            &first, &first_slot) == FTPVITA_TRANSFER_OK, 98);
                        expect(ftpvita_transfer_pool_acquire(
                            "ux0:/data/\xC3\xA4.bin", FTPVITA_TRANSFER_WRITE,
                            &rejected, &rejected_slot) ==
                            FTPVITA_TRANSFER_PATH_BUSY, 99);
                        ftpvita_transfer_pool_release(first_slot);

                        expect(ftpvita_transfer_pool_acquire(
                            "ux0:/data/\xC3\x89.bin", FTPVITA_TRANSFER_READ,
                            &first, &first_slot) == FTPVITA_TRANSFER_OK, 110);
                        expect(ftpvita_transfer_pool_acquire(
                            "ux0:/data/\xC3\xA9.bin", FTPVITA_TRANSFER_READ,
                            &second, &second_slot) == FTPVITA_TRANSFER_OK, 111);
                        expect(ftpvita_transfer_pool_acquire(
                            "ux0:/data/\xC3\xA9.bin", FTPVITA_TRANSFER_WRITE,
                            &rejected, &rejected_slot) ==
                            FTPVITA_TRANSFER_PATH_BUSY, 112);
                        ftpvita_transfer_pool_release(first_slot);
                        ftpvita_transfer_pool_release(second_slot);

                        expect(ftpvita_transfer_pool_acquire(
                            "ux0:/data/\xC3.bin", FTPVITA_TRANSFER_WRITE,
                            &first, &first_slot) == FTPVITA_TRANSFER_OK, 113);
                        expect(ftpvita_transfer_pool_acquire(
                            "UX0:/DATA/\xC3.BIN", FTPVITA_TRANSFER_READ,
                            &rejected, &rejected_slot) ==
                            FTPVITA_TRANSFER_PATH_BUSY, 114);
                        expect(ftpvita_transfer_pool_acquire(
                            "ux0:/data/\xC4.bin", FTPVITA_TRANSFER_WRITE,
                            &second, &second_slot) == FTPVITA_TRANSFER_OK, 115);
                        ftpvita_transfer_pool_release(first_slot);
                        ftpvita_transfer_pool_release(second_slot);

                        expect(ftpvita_transfer_pool_acquire(
                            "ux0:/data/\xC8\xBF.bin", FTPVITA_TRANSFER_WRITE,
                            &first, &first_slot) == FTPVITA_TRANSFER_OK, 116);
                        expect(ftpvita_transfer_pool_acquire(
                            "ux0:/data/\xE2\xB1\xBE.bin",
                            FTPVITA_TRANSFER_READ, &rejected,
                            &rejected_slot) == FTPVITA_TRANSFER_PATH_BUSY, 117);
                        ftpvita_transfer_pool_release(first_slot);

                        ftpvita_transfer_pool_fini();
                    }

                    static void test_failure_cleanup_and_validation(void)
                    {
                        void *buffer = (void *)(uintptr_t)1;
                        unsigned int slot = 99;

                        reset_mocks();
                        fail_allocation_at = 2;
                        expect(ftpvita_transfer_pool_init(64) < 0, 70);
                        expect(allocation_calls == 3, 71);
                        expect(free_calls == 2, 72);
                        expect(delete_mutex_calls == 1, 73);
                        expect(ftpvita_transfer_pool_available() == 0, 74);

                        reset_mocks();
                        create_mutex_result = -77;
                        expect(ftpvita_transfer_pool_init(64) == -77, 75);
                        expect(allocation_calls == 0, 76);
                        expect(delete_mutex_calls == 0, 77);

                        reset_mocks();
                        expect(ftpvita_transfer_pool_init(0) < 0, 78);
                        expect(create_mutex_calls == 0, 79);
                        expect(ftpvita_transfer_pool_init(64) == 0, 80);
                        expect(ftpvita_transfer_pool_init(64) < 0, 81);
                        expect(ftpvita_transfer_pool_acquire(NULL,
                            FTPVITA_TRANSFER_WRITE, &buffer, &slot) < 0, 82);
                        expect(ftpvita_transfer_pool_acquire("/x", 99,
                            &buffer, &slot) < 0, 83);
                        expect(ftpvita_transfer_pool_acquire("/x",
                            FTPVITA_TRANSFER_WRITE, NULL, &slot) < 0, 84);
                        expect(ftpvita_transfer_pool_acquire("/x",
                            FTPVITA_TRANSFER_WRITE, &buffer, NULL) < 0, 85);

                        lock_result = -88;
                        buffer = (void *)(uintptr_t)1;
                        slot = 99;
                        expect(ftpvita_transfer_pool_acquire("/x",
                            FTPVITA_TRANSFER_WRITE, &buffer, &slot) == -88, 86);
                        expect(buffer == NULL, 87);
                        expect(slot == FTPVITA_TRANSFER_INVALID_SLOT, 88);
                        ftpvita_transfer_pool_fini();
                    }

                    int main(void)
                    {
                        test_capacity_and_reuse();
                        test_path_conflicts();
                        test_path_alias_conflicts();
                        test_failure_cleanup_and_validation();
                        return 0;
                    }
                    """
                )
            )

            pool_object = tmp / "transfer_pool.o"
            executable = tmp / "transfer_pool_harness"
            compiler = "cc"
            subprocess.run(
                [
                    compiler,
                    "-std=c99",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-Dmalloc=pool_test_malloc",
                    "-Dfree=pool_test_free",
                    "-I",
                    str(tmp / "include"),
                    "-I",
                    str(ROOT / "vendor" / "libftpvita"),
                    "-c",
                    str(source),
                    "-o",
                    str(pool_object),
                ],
                check=True,
            )
            subprocess.run(
                [
                    compiler,
                    "-std=c99",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(tmp / "include"),
                    "-I",
                    str(ROOT / "vendor" / "libftpvita"),
                    str(harness),
                    str(pool_object),
                    "-o",
                    str(executable),
                ],
                check=True,
            )
            subprocess.run([str(executable)], check=True)


class ProductionTransferPoolIntegrationTests(unittest.TestCase):
    def test_production_sources_and_build_boundary(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        net = (ROOT / "src" / "net.c").read_text()
        ftp = (ROOT / "vendor" / "libftpvita" / "ftpvita.c").read_text()
        pool = (ROOT / "vendor" / "libftpvita" / "transfer_pool.c").read_text()
        readme = (ROOT / "README.md").read_text()

        self.assertTrue((ROOT / "vendor" / "libftpvita" / "transfer_pool.c").is_file())
        self.assertTrue((ROOT / "vendor" / "libftpvita" / "transfer_pool.h").is_file())
        self.assertTrue(
            (ROOT / "vendor" / "libftpvita" / "unicode_upper_table.h").is_file()
        )
        self.assertIn("vendor/libftpvita/transfer_pool.c", cmake)
        self.assertIn("vendor/libftpvita/transfer_pool.h", cmake)
        self.assertIn("vendor/libftpvita/unicode_upper_table.h", cmake)
        self.assertIn("ftpvita_set_file_buf_size(64 * 1024)", net)
        self.assertIn("ftpvita_transfer_pool_init(file_buf_size)", ftp)
        self.assertIn("ftpvita_transfer_pool_fini()", ftp)
        self.assertIn("450 Transfer slot unavailable; retry later.", ftp)
        self.assertIn("450 File busy.", ftp)
        self.assertIn("transfer_path_key", pool)
        self.assertIn("transfer_unicode_upper", pool)
        self.assertIn("transfer_utf8_decode", pool)
        self.assertNotIn("tolower(", pool)
        self.assertNotIn("towupper(", pool)
        self.assertIn("four", readme.lower())
        self.assertIn("64 KiB", readme)
        self.assertIn("256 KiB", readme)
        self.assertIn("450 Transfer slot unavailable", readme)
        self.assertIn("Unicode case variants", readme)
        self.assertIn("omitted slash after the device colon", readme)
        self.assertIn("repeated", readme)
        self.assertIn("fixed one-to-one Unicode table", readme)

        production = cmake + net + ftp
        self.assertNotIn("VITACOMPANION_DIAGNOSTIC", production)
        self.assertNotIn("diag_log", production)
        self.assertNotIn("vitacompanion-fourway-diag.log", production)

    def test_unicode_upper_table_is_sorted_and_stable(self):
        table = (
            ROOT / "vendor" / "libftpvita" / "unicode_upper_table.h"
        ).read_text()
        rows = [
            (int(first, 16), int(last, 16), int(delta), int(step))
            for first, last, delta, step in re.findall(
                r"\{0x([0-9A-F]+), 0x([0-9A-F]+), (-?\d+), (\d+)\}",
                table,
            )
        ]

        self.assertIn("Unicode 13.0.0", table)
        self.assertEqual(len(rows), 183)
        self.assertEqual(
            sum(((last - first) // step) + 1 for first, last, _, step in rows),
            1158,
        )
        for index, (first, last, delta, step) in enumerate(rows):
            self.assertLessEqual(first, last)
            self.assertIn(step, (1, 2))
            self.assertGreaterEqual(first + delta, 0)
            self.assertLessEqual(last + delta, 0xFFFF)
            if index:
                self.assertLess(rows[index - 1][1], first)

        expected = {
            0x0061: 0x0041,
            0x00E4: 0x00C4,
            0x00E9: 0x00C9,
            0x023F: 0x2C7E,
            0x03C3: 0x03A3,
            0x044F: 0x042F,
            0xFF41: 0xFF21,
        }
        for codepoint, uppercase in expected.items():
            actual = codepoint
            for first, last, delta, step in rows:
                if first <= codepoint <= last:
                    if (codepoint - first) % step == 0:
                        actual = codepoint + delta
                    break
            self.assertEqual(actual, uppercase)

    def test_binary_upload_and_download_acquire_before_file_io(self):
        ftp = (ROOT / "vendor" / "libftpvita" / "ftpvita.c").read_text()
        receive_match = re.search(
            r"static void receive_file\(.*?^\}", ftp, re.DOTALL | re.MULTILINE
        )
        send_match = re.search(
            r"static void send_file\(.*?^\}", ftp, re.DOTALL | re.MULTILINE
        )
        self.assertIsNotNone(receive_match)
        self.assertIsNotNone(send_match)
        receive = receive_match.group(0)
        send = send_match.group(0)

        for function in (receive, send):
            self.assertIn("ftpvita_transfer_pool_acquire", function)
            self.assertIn("ftpvita_transfer_pool_release", function)
            self.assertLess(
                function.index("ftpvita_transfer_pool_acquire"),
                function.index("sceIoOpen"),
            )

        self.assertLess(
            receive.index("450 Transfer slot unavailable; retry later."),
            receive.index("sceIoOpen"),
        )
        self.assertLess(
            receive.index("450 File busy."),
            receive.index("sceIoOpen"),
        )
        self.assertLess(
            send.index("450 Transfer slot unavailable; retry later."),
            send.index("sceIoOpen"),
        )
        self.assertLess(
            send.index("450 File busy."),
            send.index("sceIoOpen"),
        )
        self.assertNotRegex(
            receive,
            r"buffer = malloc\(client->transfer_type == FTP_TRANSFER_TYPE_ASCII \?\s*ASCII_INPUT_SIZE : file_buf_size\)",
        )
        self.assertNotRegex(
            send,
            r"buffer = malloc\(client->transfer_type == FTP_TRANSFER_TYPE_ASCII \?\s*ASCII_INPUT_SIZE : file_buf_size\)",
        )

    def test_pool_lifecycle_is_inside_ftp_lifecycle(self):
        ftp = (ROOT / "vendor" / "libftpvita" / "ftpvita.c").read_text()
        init_match = re.search(r"int ftpvita_init_internal\(.*?^\}", ftp, re.DOTALL | re.MULTILINE)
        fini_match = re.search(r"void ftpvita_fini\(.*?^\}", ftp, re.DOTALL | re.MULTILINE)
        clients_match = re.search(
            r"static void client_list_thread_end\(\).*?^\}",
            ftp,
            re.DOTALL | re.MULTILINE,
        )
        self.assertIsNotNone(init_match)
        self.assertIsNotNone(fini_match)
        self.assertIsNotNone(clients_match)
        init = init_match.group(0)
        fini = fini_match.group(0)
        clients = clients_match.group(0)

        self.assertIn("ftpvita_transfer_pool_init(file_buf_size)", init)
        self.assertIn("ftpvita_transfer_pool_fini()", init)
        self.assertIn("ftpvita_transfer_pool_fini()", fini)
        self.assertIn("sceKernelWaitThreadEnd", clients)
        self.assertLess(
            fini.index("client_list_thread_end()"),
            fini.index("ftpvita_transfer_pool_fini()"),
        )

    def test_buffer_size_cannot_change_after_pool_initialization(self):
        ftp = (ROOT / "vendor" / "libftpvita" / "ftpvita.c").read_text()
        setter_match = re.search(
            r"void ftpvita_set_file_buf_size\(.*?^\}",
            ftp,
            re.DOTALL | re.MULTILINE,
        )
        self.assertIsNotNone(setter_match)
        setter = setter_match.group(0)
        self.assertIn("size > 0 && !ftp_initialized", setter)


if __name__ == "__main__":
    unittest.main()
