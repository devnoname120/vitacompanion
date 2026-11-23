import pathlib
import re
import subprocess
import tempfile
import textwrap
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class ReleaseConfigurationTests(unittest.TestCase):
    def test_release_version_is_1_11(self):
        user_exports = (ROOT / "vitacompanion.yml").read_text()
        kernel_exports = (ROOT / "kernel" / "vitacompanion_kernel.yml").read_text()

        for exports in (user_exports, kernel_exports):
            self.assertRegex(exports, r"(?m)^\s+major: 1$")
            self.assertRegex(exports, r"(?m)^\s+minor: 11$")
            self.assertNotRegex(exports, r"(?m)^\s+minor: 10$")

    def test_logging_is_compiled_out_by_default(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        main_header = (ROOT / "src" / "main.h").read_text()
        log_header = (ROOT / "src" / "log.h").read_text()
        log_source = (ROOT / "src" / "log.c").read_text()
        net_source = (ROOT / "src" / "net.c").read_text()
        ftp_source = (ROOT / "vendor" / "libftpvita" / "ftpvita.c").read_text()

        self.assertRegex(
            cmake,
            r"(?s)option\(VITACOMPANION_ENABLE_LOGGING.*?OFF\)",
        )
        self.assertIn(
            "ENABLE_LOGGING=${VITACOMPANION_LOGGING_VALUE}",
            cmake,
        )
        self.assertIn(
            "FTPVITA_ENABLE_LOGGING=${VITACOMPANION_LOGGING_VALUE}",
            cmake,
        )
        self.assertIn("#ifndef ENABLE_LOGGING", main_header)
        self.assertIn("#if ENABLE_LOGGING == 1", log_header)
        self.assertIn("#define LOG(...) ((void)0)", log_header)
        self.assertIn("void LOG_CALLBACK(const char *str);", log_header)
        self.assertIn("#if ENABLE_LOGGING == 1", log_source)
        self.assertIn("void LOG_CALLBACK(const char *str)", log_source)
        self.assertIn("ftpvita_set_info_log_cb(LOG_CALLBACK);", net_source)
        self.assertIn("ftpvita_set_debug_log_cb(LOG_CALLBACK);", net_source)
        self.assertIn("#ifndef FTPVITA_ENABLE_LOGGING", ftp_source)
        self.assertIn("#if FTPVITA_ENABLE_LOGGING == 1", ftp_source)
        self.assertIn("#define INFO(...) ((void)0)", ftp_source)
        self.assertIn("#define DEBUG(...) ((void)0)", ftp_source)

        with tempfile.TemporaryDirectory() as tmpdir:
            source_path = pathlib.Path(tmpdir) / "test.c"
            executable_path = pathlib.Path(tmpdir) / "test"
            source_path.write_text(
                textwrap.dedent(
                    r"""
                    #include "log.h"

                    static int calls;

                    int main(void)
                    {
                        LOG("value=%d\n", ++calls);
                        return calls;
                    }
                    """
                )
            )
            subprocess.run(
                [
                    "cc",
                    "-std=c99",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-DENABLE_LOGGING=0",
                    "-I",
                    str(ROOT / "src"),
                    str(source_path),
                    "-o",
                    str(executable_path),
                ],
                check=True,
                cwd=ROOT,
            )
            subprocess.run([str(executable_path)], check=True, cwd=ROOT)

    def test_production_targets_have_no_diagnostic_hooks(self):
        production_files = [ROOT / "CMakeLists.txt"]
        for directory in ("src", "include", "kernel", "vendor"):
            production_files.extend((ROOT / directory).rglob("*"))

        forbidden = (
            "vitacompanion_heap_fail",
            "heap_init=0x",
            "HEAP_FAILURE_DIAGNOSTIC",
        )
        for path in production_files:
            if not path.is_file():
                continue
            source = path.read_text(errors="ignore")
            for marker in forbidden:
                self.assertNotIn(marker, source, path)


if __name__ == "__main__":
    unittest.main()
