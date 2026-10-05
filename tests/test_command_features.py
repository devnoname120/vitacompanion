import pathlib
import subprocess
import tempfile
import textwrap
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


def compile_and_run(source, support_sources, headers=None, include_dirs=()):
    with tempfile.TemporaryDirectory() as tmpdir:
        tmp = pathlib.Path(tmpdir)
        source_path = tmp / "test.c"
        executable_path = tmp / "test"
        source_path.write_text(textwrap.dedent(source))
        for relative_path, contents in (headers or {}).items():
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
                "-I",
                str(ROOT / "include"),
                *(item for include_dir in include_dirs
                  for item in ("-I", str(ROOT / include_dir))),
                str(source_path),
                *(str(ROOT / source_name) for source_name in support_sources),
                "-o",
                str(executable_path),
            ],
            check=True,
            cwd=ROOT,
        )
        subprocess.run([str(executable_path)], check=True, cwd=ROOT)


class CommandFeatureTests(unittest.TestCase):
    def test_reboot_prefers_shell_shutdown_with_forced_fallback(self):
        command_source = (ROOT / "src" / "cmd_definitions.c").read_text()
        reboot_start = command_source.index("void cmd_reboot")
        reboot_end = command_source.index("void cmd_screen")
        reboot_handler = command_source[reboot_start:reboot_end]
        cmake_source = (ROOT / "CMakeLists.txt").read_text()

        self.assertIn("reboot_request();", reboot_handler)
        self.assertNotIn("scePowerRequestColdReset", reboot_handler)
        self.assertIn("src/reboot.c", cmake_source)
        self.assertIn("taihen_stub", cmake_source)

        compile_and_run(
            r"""
            #include "reboot.h"

            #include <psp2/kernel/threadmgr.h>
            #include <stdint.h>
            #include <string.h>

            #define SHELL_UTIL_LIBRARY_NID 0xD2B1C8AE
            #define SHELL_REQUEST_COLD_RESET_NID 0x636544FB
            #define WATCHDOG_DELAY_US (5 * 1000 * 1000)

            static SceKernelThreadEntry watchdog_entry;
            static int call_index;
            static int create_order;
            static int start_order;
            static int resolve_order;
            static int shell_order;
            static int forced_order;
            static int create_result;
            static int start_result;
            static int resolve_result;
            static int shell_result;
            static int forced_result;
            static int delete_count;
            static int delay_us;
            static int exit_delete_count;
            static int shell_argument;

            static int shell_request_cold_reset(int argument)
            {
                shell_order = ++call_index;
                shell_argument = argument;
                return shell_result;
            }

            static void reset_state(void)
            {
                watchdog_entry = 0;
                call_index = 0;
                create_order = 0;
                start_order = 0;
                resolve_order = 0;
                shell_order = 0;
                forced_order = 0;
                create_result = 7;
                start_result = 0;
                resolve_result = 0;
                shell_result = 0;
                forced_result = -77;
                delete_count = 0;
                delay_us = 0;
                exit_delete_count = 0;
                shell_argument = -1;
            }

            SceUID sceKernelCreateThread(const char *name,
                SceKernelThreadEntry entry, int priority,
                unsigned int stack_size, unsigned int attributes,
                int cpu_affinity_mask, const void *option)
            {
                (void)priority;
                (void)stack_size;
                (void)attributes;
                (void)cpu_affinity_mask;
                (void)option;
                if (strcmp(name, "vitacompanion_reboot_watchdog") != 0)
                    return -1;
                create_order = ++call_index;
                watchdog_entry = entry;
                return create_result;
            }

            int sceKernelStartThread(SceUID thread_id,
                unsigned int argument_size, const void *arguments)
            {
                (void)thread_id;
                (void)argument_size;
                (void)arguments;
                start_order = ++call_index;
                return start_result;
            }

            int sceKernelDeleteThread(SceUID thread_id)
            {
                (void)thread_id;
                ++delete_count;
                return 0;
            }

            int sceKernelDelayThread(unsigned int delay)
            {
                delay_us = (int)delay;
                return 0;
            }

            int sceKernelExitDeleteThread(int status)
            {
                ++exit_delete_count;
                return status;
            }

            int scePowerRequestColdReset(void)
            {
                forced_order = ++call_index;
                return forced_result;
            }

            int taiGetModuleExportFunc(const char *module_name,
                uint32_t library_nid, uint32_t function_nid,
                uintptr_t *function)
            {
                resolve_order = ++call_index;
                if (strcmp(module_name, "SceShellSvc") != 0 ||
                    library_nid != SHELL_UTIL_LIBRARY_NID ||
                    function_nid != SHELL_REQUEST_COLD_RESET_NID)
                    return -2;
                if (resolve_result < 0)
                    return resolve_result;
                *function = (uintptr_t)shell_request_cold_reset;
                return 0;
            }

            int main(void)
            {
                reset_state();
                if (reboot_request() != 0 ||
                    create_order != 1 || start_order != 2 ||
                    resolve_order != 3 || shell_order != 4 ||
                    forced_order != 0 || shell_argument != 0 ||
                    watchdog_entry == 0)
                    return 1;

                if (watchdog_entry(0, 0) != 0 ||
                    delay_us != WATCHDOG_DELAY_US ||
                    forced_order != 5 || exit_delete_count != 1)
                    return 2;

                reset_state();
                resolve_result = -3;
                if (reboot_request() != forced_result ||
                    shell_order != 0 || forced_order != 4)
                    return 3;

                reset_state();
                shell_result = -4;
                if (reboot_request() != forced_result ||
                    shell_order != 4 || forced_order != 5)
                    return 4;

                reset_state();
                create_result = -5;
                if (reboot_request() != forced_result ||
                    start_order != 0 || resolve_order != 0 ||
                    forced_order != 2)
                    return 5;

                reset_state();
                start_result = -6;
                if (reboot_request() != forced_result ||
                    delete_count != 1 || resolve_order != 0 ||
                    forced_order != 3)
                    return 6;

                return 0;
            }
            """,
            ("src/reboot.c",),
            {
                "psp2/kernel/threadmgr.h": r"""
                    #ifndef TEST_THREADMGR_H
                    #define TEST_THREADMGR_H
                    typedef int SceUID;
                    typedef int (*SceKernelThreadEntry)(
                        unsigned int, void *);
                    SceUID sceKernelCreateThread(const char *name,
                        SceKernelThreadEntry entry, int priority,
                        unsigned int stack_size,
                        unsigned int attributes,
                        int cpu_affinity_mask, const void *option);
                    int sceKernelStartThread(SceUID thread_id,
                        unsigned int argument_size,
                        const void *arguments);
                    int sceKernelDeleteThread(SceUID thread_id);
                    int sceKernelDelayThread(unsigned int delay);
                    int sceKernelExitDeleteThread(int status);
                    #endif
                """,
                "psp2/power.h": r"""
                    #ifndef TEST_POWER_H
                    #define TEST_POWER_H
                    int scePowerRequestColdReset(void);
                    #endif
                """,
                "taihen.h": r"""
                    #ifndef TEST_TAIHEN_H
                    #define TEST_TAIHEN_H
                    #include <stdint.h>
                    int taiGetModuleExportFunc(const char *module_name,
                        uint32_t library_nid, uint32_t function_nid,
                        uintptr_t *function);
                    #endif
                """,
            },
        )

    def test_command_chains_accept_optional_trailing_semicolons(self):
        compile_and_run(
            r"""
            #include "parser.h"

            #include <stdlib.h>
            #include <string.h>

            int main(void)
            {
                char chain[] =
                    " press cross; wait 100ms; release cross; \r\n";
                char single[] = "version\n";
                char *commands[8] = {0};
                char *args[4] = {0};
                size_t count = 0;

                if (!parse_cmd_chain(chain, sizeof(chain) - 1,
                        commands, 8, &count) || count != 3)
                    return 1;

                if (parse_cmd(commands[0], strlen(commands[0]),
                        args, 4) != 2 ||
                    strcmp(args[0], "press") != 0 ||
                    strcmp(args[1], "cross") != 0)
                    return 2;

                memset(commands, 0, sizeof(commands));
                count = 0;
                if (!parse_cmd_chain(single, sizeof(single) - 1,
                        commands, 8, &count) || count != 1 ||
                    strcmp(commands[0], "version") != 0)
                    return 3;

                return 0;
            }
            """,
            ("src/parser.c",),
        )

    def test_command_chain_limit_is_reported(self):
        compile_and_run(
            r"""
            #include "parser.h"

            int main(void)
            {
                char chain[] = "version;version\n";
                char *commands[1] = {0};
                size_t count = 0;

                return parse_cmd_chain(chain, sizeof(chain) - 1,
                    commands, 1, &count) ? 1 : 0;
            }
            """,
            ("src/parser.c",),
        )

    def test_wait_units_are_milliseconds_and_seconds_only(self):
        compile_and_run(
            r"""
            #include "parser.h"

            #include <stdint.h>

            static int expect_valid(const char *value, uint32_t expected)
            {
                uint32_t actual = 0;
                return !parse_wait_duration_ms(value, &actual) ||
                    actual != expected;
            }

            int main(void)
            {
                uint32_t value;

                if (expect_valid("1000ms", 1000) ||
                    expect_valid("3s", 3000) ||
                    expect_valid("0ms", 0))
                    return 1;

                if (parse_wait_duration_ms("5f", &value) ||
                    parse_wait_duration_ms("1000", &value) ||
                    parse_wait_duration_ms("1.5s", &value) ||
                    parse_wait_duration_ms("3S", &value) ||
                    parse_wait_duration_ms("4294968s", &value))
                    return 2;

                return 0;
            }
            """,
            ("src/parser.c",),
        )

    def test_input_parser_covers_buttons_sticks_touches_and_reset(self):
        compile_and_run(
            r"""
            #include <vitacompanion_input.h>

            #include <stdint.h>

            int main(void)
            {
                vitacompanion_input_action action;
                char *cross[] = {"press", "cross"};
                char *stick[] = {"press", "left-stick", "0", "255"};
                char *touch[] = {
                    "press", "front-touch", "3", "1919", "1087"
                };
                char *release_touch[] = {
                    "release", "front-touch", "3"
                };
                char *release_all[] = {"release", "all"};
                char *bad_stick[] = {
                    "press", "right-stick", "256", "0"
                };
                char *bad_touch[] = {
                    "press", "rear-touch", "4", "0", "0"
                };

                if (vitacompanion_parse_press(cross, 2, &action) < 0 ||
                    action.type != VITACOMPANION_INPUT_BUTTON ||
                    action.data.button.mask != 0x00004000 ||
                    !action.active)
                    return 1;

                if (vitacompanion_parse_press(stick, 4, &action) < 0 ||
                    action.type != VITACOMPANION_INPUT_ANALOG ||
                    action.data.analog.stick != VITACOMPANION_STICK_LEFT ||
                    action.data.analog.x != 0 ||
                    action.data.analog.y != 255)
                    return 2;

                if (vitacompanion_parse_press(touch, 5, &action) < 0 ||
                    action.type != VITACOMPANION_INPUT_TOUCH ||
                    !action.active ||
                    action.data.touch.slot != 3 ||
                    action.data.touch.x != 1919 ||
                    action.data.touch.y != 1087)
                    return 3;

                if (vitacompanion_parse_release(
                        release_touch, 3, &action) < 0 ||
                    action.type != VITACOMPANION_INPUT_TOUCH ||
                    action.active)
                    return 4;

                if (vitacompanion_parse_release(
                        release_all, 2, &action) < 0 ||
                    action.type != VITACOMPANION_INPUT_RESET)
                    return 5;

                if (vitacompanion_parse_press(
                        bad_stick, 4, &action) >= 0 ||
                    vitacompanion_parse_press(
                        bad_touch, 5, &action) >= 0)
                    return 6;

                return 0;
            }
            """,
            ("src/input_parse.c",),
        )

    def test_version_comes_from_loaded_module_metadata(self):
        source = (ROOT / "src" / "version.c").read_text()
        self.assertIn("sceKernelGetModuleIdByAddr", source)
        self.assertIn("sceKernelGetModuleInfo", source)
        self.assertRegex(
            source,
            r"info\.modver\[1\].*info\.modver\[0\]",
        )

    def test_quit_replaces_kill_and_destroy_commands(self):
        source = (ROOT / "src" / "cmd_definitions.c").read_text()
        self.assertIn('{.name = "quit"', source)
        self.assertIn('strcmp(arg_list[1], "all")', source)
        self.assertNotIn('{.name = "kill"', source)
        self.assertNotIn('{.name = "destroy"', source)

    def test_kernel_touch_contacts_keep_stable_ids(self):
        source = (ROOT / "kernel" / "touch_patch.c").read_text()
        self.assertIn("SYNTHETIC_TOUCH_ID_BASE + slot", source)
        self.assertIn("current->reportNum++", source)
        self.assertIn("points[slot].active", source)

    def test_kernel_touch_patch_handles_batches_and_port_limits(self):
        compile_and_run(
            r"""
            #include "touch_patch.h"

            #include <string.h>

            static int report_matches(const SceTouchReport *report,
                unsigned int id, int x, int y)
            {
                unsigned int i;

                if (report->id != id || report->force != 0x80 ||
                    report->x != x || report->y != y || report->info != 0)
                    return 0;
                for (i = 0; i < sizeof(report->reserved); ++i)
                {
                    if (report->reserved[i] != 0)
                        return 0;
                }
                return 1;
            }

            int main(void)
            {
                vitacompanion_touch_point points[VITACOMPANION_TOUCH_SLOTS];
                SceTouchData front[2];
                SceTouchData rear;

                memset(points, 0, sizeof(points));
                memset(front, 0, sizeof(front));
                memset(&rear, 0, sizeof(rear));

                if (sizeof(SceTouchReport) != 0x10 ||
                    sizeof(SceTouchData) != 0x90)
                    return 1;

                points[0].active = 1;
                points[0].x = 120;
                points[0].y = 240;
                points[2].active = 1;
                points[2].x = 960;
                points[2].y = 544;

                front[0].timeStamp = 123;
                front[0].status = 0x12345678;
                front[0].reportNum = 1;
                front[0].report[0].id = 7;
                front[1].timeStamp = 456;
                front[1].reportNum = 5;

                if (vitacompanion_patch_touch_data(
                        VITACOMPANION_TOUCH_FRONT, front, 2, points) != 3)
                    return 2;
                if (front[0].timeStamp != 123 ||
                    front[0].status != 0x12345678 ||
                    front[0].reportNum != 3 ||
                    front[0].report[0].id != 7 ||
                    !report_matches(&front[0].report[1], 0x70, 120, 240) ||
                    !report_matches(&front[0].report[2], 0x72, 960, 544))
                    return 3;
                if (front[1].timeStamp != 456 || front[1].reportNum != 6 ||
                    !report_matches(&front[1].report[5], 0x70, 120, 240))
                    return 4;

                rear.reportNum = 3;
                if (vitacompanion_patch_touch_data(
                        VITACOMPANION_TOUCH_REAR, &rear, 1, points) != 1 ||
                    rear.reportNum != 4 ||
                    !report_matches(&rear.report[3], 0x70, 120, 240))
                    return 5;

                rear.reportNum = 4;
                if (vitacompanion_patch_touch_data(
                        VITACOMPANION_TOUCH_REAR, &rear, 1, points) != 0 ||
                    rear.reportNum != 4 ||
                    vitacompanion_patch_touch_data(
                        VITACOMPANION_TOUCH_FRONT, front, 0, points) != 0 ||
                    vitacompanion_patch_touch_data(2, front, 2, points) != 0 ||
                    vitacompanion_patch_touch_data(
                        VITACOMPANION_TOUCH_FRONT, 0, 2, points) != 0 ||
                    vitacompanion_patch_touch_data(
                        VITACOMPANION_TOUCH_FRONT, front, 2, 0) != 0)
                    return 6;

                return 0;
            }
            """,
            ("kernel/touch_patch.c",),
            headers={
                "psp2/touch.h": r"""
                    #ifndef TEST_PSP2_TOUCH_H
                    #define TEST_PSP2_TOUCH_H

                    #include <stdint.h>

                    #define SCE_TOUCH_MAX_REPORT 8

                    typedef struct SceTouchReport {
                        uint8_t id;
                        uint8_t force;
                        int16_t x;
                        int16_t y;
                        uint8_t reserved[8];
                        uint16_t info;
                    } SceTouchReport;

                    typedef struct SceTouchData {
                        uint64_t timeStamp;
                        uint32_t status;
                        uint32_t reportNum;
                        SceTouchReport report[SCE_TOUCH_MAX_REPORT];
                    } SceTouchData;

                    #endif
                """,
            },
            include_dirs=("kernel",),
        )

    def test_kernel_hooks_region_ext_user_exports(self):
        source = (ROOT / "kernel" / "main.c").read_text()
        cmake = (ROOT / "CMakeLists.txt").read_text()

        self.assertIn("0x3E4F4A81", source)
        self.assertIn("0x2CF6D7E2", source)
        self.assertIn("0x9F0ACAF9", source)
        self.assertIn("ksceKernelCopyFromUser", source)
        self.assertIn("ksceKernelCopyToUser", source)
        self.assertIn("ksceKernelPowerTick", source)
        self.assertIn("SceKernelSuspendForDriver_stub", cmake)
        self.assertIn("SceSysmemForDriver_stub", cmake)

    def test_input_requires_the_expected_kernel_api(self):
        compile_and_run(
            r"""
            #include "input.h"
            #include <vitacompanion_kernel.h>

            static int api_version = -1;
            static int api_version_call_count;
            static int reset_count;

            int vitaCompanionKernelGetApiVersion(void)
            {
                ++api_version_call_count;
                return api_version;
            }

            int vitaCompanionKernelSetButtons(
                uint32_t buttons, int pressed)
            {
                (void)buttons;
                (void)pressed;
                return 0;
            }

            int vitaCompanionKernelSetAnalog(
                int stick, int x, int y, int active)
            {
                (void)stick;
                (void)x;
                (void)y;
                (void)active;
                return 0;
            }

            int vitaCompanionKernelSetTouch(
                int port, int slot_and_active, int x, int y)
            {
                (void)port;
                (void)slot_and_active;
                (void)x;
                (void)y;
                return 0;
            }

            int vitaCompanionKernelReset(void)
            {
                ++reset_count;
                return 0;
            }

            int main(void)
            {
                if (input_start() >= 0 ||
                    input_is_ready() ||
                    api_version_call_count != 1)
                    return 1;

                input_end();
                if (reset_count != 0)
                    return 2;

                api_version = VITACOMPANION_KERNEL_ABI_VERSION;
                if (input_start() < 0 ||
                    !input_is_ready() ||
                    api_version_call_count != 2)
                    return 3;

                input_end();
                if (input_is_ready() || reset_count != 1)
                    return 4;

                return 0;
            }
            """,
            ("src/input.c",),
        )

    def test_touch_active_flag_uses_four_argument_kernel_call(self):
        compile_and_run(
            r"""
            #include "input.h"
            #include <vitacompanion_input.h>
            #include <vitacompanion_kernel.h>

            static int last_port = -1;
            static int last_slot_and_active = -1;
            static int last_x = -1;
            static int last_y = -1;

            int vitaCompanionKernelGetApiVersion(void)
            {
                return VITACOMPANION_KERNEL_ABI_VERSION;
            }

            int vitaCompanionKernelSetButtons(uint32_t buttons, int pressed)
            {
                (void)buttons;
                (void)pressed;
                return 0;
            }

            int vitaCompanionKernelSetAnalog(
                int stick, int x, int y, int active)
            {
                (void)stick;
                (void)x;
                (void)y;
                (void)active;
                return 0;
            }

            int vitaCompanionKernelSetTouch(
                int port, int slot_and_active, int x, int y)
            {
                last_port = port;
                last_slot_and_active = slot_and_active;
                last_x = x;
                last_y = y;
                return 0;
            }

            int vitaCompanionKernelReset(void)
            {
                return 0;
            }

            int main(void)
            {
                vitacompanion_input_action action;
                char *press[] = {"press", "front-touch", "2", "960", "544"};
                char *release[] = {"release", "front-touch", "2"};

                if (input_start() < 0 ||
                    vitacompanion_parse_press(press, 5, &action) < 0 ||
                    input_apply(&action) < 0 ||
                    last_port != VITACOMPANION_TOUCH_FRONT ||
                    last_slot_and_active !=
                        (VITACOMPANION_TOUCH_ACTIVE_FLAG | 2) ||
                    last_x != 960 || last_y != 544)
                    return 1;

                if (vitacompanion_parse_release(release, 3, &action) < 0 ||
                    input_apply(&action) < 0 ||
                    last_slot_and_active != 2)
                    return 2;

                input_end();
                return 0;
            }
            """,
            ("src/input.c", "src/input_parse.c"),
        )

    def test_kernel_module_is_a_required_taihen_dependency(self):
        input_source = (ROOT / "src" / "input.c").read_text()
        cmake_source = (ROOT / "CMakeLists.txt").read_text()
        readme = (ROOT / "README.md").read_text()

        self.assertNotIn("taiLoadStartKernelModule", input_source)
        self.assertNotIn("_vshKernelSearchModuleByName", input_source)
        self.assertNotIn("VITACOMPANION_KERNEL_PATH", cmake_source)
        self.assertIn(
            "${CMAKE_CURRENT_BINARY_DIR}/libvitacompanion_kernel_stub.a",
            cmake_source,
        )
        self.assertNotIn(
            "libvitacompanion_kernel_stub_weak.a",
            cmake_source,
        )
        self.assertIn(
            "*KERNEL\nur0:tai/vitacompanion_kernel.skprx",
            readme,
        )
        self.assertIn(
            "The kernel module is required",
            readme,
        )


if __name__ == "__main__":
    unittest.main()
