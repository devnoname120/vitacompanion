import pathlib
import subprocess
import tempfile
import textwrap
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
VENDOR = ROOT / "vendor" / "libftpvita"


def compile_and_run(source):
    with tempfile.TemporaryDirectory() as tmpdir:
        tmp = pathlib.Path(tmpdir)
        source_path = tmp / "test.c"
        exe_path = tmp / "test"
        source_path.write_text(textwrap.dedent(source))
        subprocess.run(
            [
                "cc",
                "-std=c99",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(VENDOR),
                str(source_path),
                str(VENDOR / "ftpvita_path.c"),
                "-o",
                str(exe_path),
            ],
            check=True,
            cwd=ROOT,
        )
        subprocess.run([str(exe_path)], check=True, cwd=ROOT)


class LibftpvitaCompatTests(unittest.TestCase):
    def test_vita_paths_are_normalized_for_file_commands(self):
        compile_and_run(
            r"""
            #include "ftpvita_path.h"

            #include <stdio.h>
            #include <stdlib.h>
            #include <string.h>

            static void expect_path(const char *cur_path, const char *arg, const char *expected)
            {
                char actual[256];
                ftpvita_path_from_command_arg(cur_path, arg, actual, sizeof(actual));
                if (strcmp(actual, expected) != 0) {
                    fprintf(stderr, "for arg %s expected %s, got %s\n", arg, expected, actual);
                    exit(1);
                }
            }

            int main(void)
            {
                expect_path("/", "/ux0:/data/bootstrap.self\r\n", "/ux0:/data/bootstrap.self");
                expect_path("/", "ux0:/data/bootstrap.self\r\n", "/ux0:/data/bootstrap.self");
                expect_path("/", "uma0:/media/file.bin\r\n", "/uma0:/media/file.bin");
                expect_path("/ux0:/data", "bootstrap.self\r\n", "/ux0:/data/bootstrap.self");
                expect_path("/ux0:/", "data/bootstrap.self\r\n", "/ux0:/data/bootstrap.self");
                return 0;
            }
            """
        )

    def test_list_paths_are_normalized_and_list_options_are_ignored(self):
        compile_and_run(
            r"""
            #include "ftpvita_path.h"

            #include <stdio.h>
            #include <stdlib.h>
            #include <string.h>

            static void expect_list_path(const char *cur_path, const char *arg, const char *expected)
            {
                char actual[256];
                ftpvita_path_from_list_args(cur_path, arg, actual, sizeof(actual));
                if (strcmp(actual, expected) != 0) {
                    fprintf(stderr, "for list arg %s expected %s, got %s\n", arg, expected, actual);
                    exit(1);
                }
            }

            int main(void)
            {
                expect_list_path("/ux0:/data", "", "/ux0:/data");
                expect_list_path("/", "ux0:/data/\r\n", "/ux0:/data/");
                expect_list_path("/", "/ux0:/data/\r\n", "/ux0:/data/");
                expect_list_path("/", "-la ux0:/data/\r\n", "/ux0:/data/");
                expect_list_path("/ux0:/data", "-a\r\n", "/ux0:/data");
                expect_list_path("/ux0:/", "data/\r\n", "/ux0:/data/");
                return 0;
            }
            """
        )

    def test_epsv_response_uses_extended_passive_mode_format(self):
        compile_and_run(
            r"""
            #include "ftpvita_path.h"

            #include <stdio.h>
            #include <stdlib.h>
            #include <string.h>

            int main(void)
            {
                char actual[128];
                ftpvita_format_epsv_response(actual, sizeof(actual), 65000);
                if (strcmp(actual, "229 Entering Extended Passive Mode (|||65000|)\r\n") != 0) {
                    fprintf(stderr, "unexpected EPSV reply: %s\n", actual);
                    exit(1);
                }
                return 0;
            }
            """
        )

    def test_ftp_command_table_wires_compatibility_commands(self):
        source = (VENDOR / "ftpvita.c").read_text()
        self.assertIn("ftpvita_path_from_list_args", source)
        self.assertIn("add_entry(EPSV)", source)
        self.assertIn("add_entry(NLST)", source)


if __name__ == "__main__":
    unittest.main()
