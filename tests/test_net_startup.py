import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class ShellNetworkStartupTests(unittest.TestCase):
    def test_wait_for_shell_then_start_and_retry_each_listener_independently(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            (work / "vitasdk.h").write_text("""
                #include <stddef.h>
                typedef int SceUID;
                #define SCE_NETCTL_STATE_DISCONNECTED 0
                #define SCE_NETCTL_STATE_CONNECTED 3
                SceUID sceKernelCreateThread(const char *, int (*)(unsigned, void *),
                    int, int, int, int, const void *);
                int sceKernelStartThread(SceUID, unsigned, const void *);
                int sceKernelDeleteThread(SceUID);
                int sceKernelDelayThread(unsigned);
            """)
            (work / "taihen.h").write_text("""
                #include <stdint.h>
                int taiGetModuleExportFunc(const char *, uint32_t, uint32_t, uintptr_t *);
            """)
            (work / "ftpvita.h").write_text("""
                void ftpvita_set_file_buf_size(unsigned);
                int ftpvita_init_existing_network(char *, unsigned short *);
                int ftpvita_add_device(const char *);
                void ftpvita_fini(void);
            """)
            (work / "test.c").write_text(r"""
                #include "net.h"
                #include <assert.h>
                #include <stdint.h>
                #include <string.h>

                volatile int run = 1;
                extern volatile int net_connected;
                static unsigned step, resolve_calls, command_starts, ftp_starts;
                static unsigned command_stops, ftp_stops, devices;
                static int command_online, ftp_online;

                static int get_state(int *state)
                {
                    assert(step > 0);
                    if (step < 3 || step == 9)
                        return (int)0x80412101;
                    *state = step == 6 ? 0 : 3;
                    return 0;
                }

                int taiGetModuleExportFunc(const char *name, uint32_t library,
                    uint32_t symbol, uintptr_t *address)
                {
                    assert(strcmp(name, "SceLibNetCtl") == 0);
                    assert(library == 0xC6083AE1 && symbol == 0x6D26AC68);
                    ++resolve_calls;
                    if (step == 0)
                        return -1;
                    *address = (uintptr_t)get_state;
                    return 0;
                }

                int cmd_start(void)
                {
                    assert(net_connected && !command_online);
                    assert(step == 3 || step == 7 || step == 8);
                    ++command_starts;
                    if (step == 7)
                        return -1;
                    command_online = 1;
                    return 0;
                }

                void cmd_end(void)
                {
                    assert(command_online && !net_connected);
                    ++command_stops;
                    command_online = 0;
                }

                int ftpvita_init_existing_network(char *ip, unsigned short *port)
                {
                    (void)ip;
                    (void)port;
                    assert(net_connected && !ftp_online);
                    assert(step == 3 || step == 4 || step == 7);
                    ++ftp_starts;
                    if (step == 3)
                        return -1;
                    ftp_online = 1;
                    return 0;
                }

                void ftpvita_fini(void)
                {
                    assert(ftp_online && !command_online && !net_connected);
                    ++ftp_stops;
                    ftp_online = 0;
                }

                void ftpvita_set_file_buf_size(unsigned size)
                { assert(size == 64 * 1024); }
                int ftpvita_add_device(const char *name)
                { assert(name && ftp_online); ++devices; return 1; }

                int sceKernelDelayThread(unsigned duration)
                {
                    assert(duration == 1000 * 1000);
                    switch (step) {
                    case 0: case 1: case 2:
                        assert(!net_connected && !command_starts && !ftp_starts);
                        break;
                    case 3: assert(command_online && !ftp_online); break;
                    case 4: case 5: assert(command_online && ftp_online); break;
                    case 6: assert(!command_online && !ftp_online); break;
                    case 7: assert(!command_online && ftp_online); break;
                    case 8: assert(command_online && ftp_online); break;
                    case 9: assert(!command_online && !ftp_online); run = 0; break;
                    default: assert(0);
                    }
                    ++step;
                    return 0;
                }

                int sceKernelCreateThread(const char *name,
                    int (*entry)(unsigned, void *), int priority, int stack,
                    int attr, int affinity, const void *option)
                {
                    (void)name; (void)entry; (void)priority; (void)stack;
                    (void)attr; (void)affinity; (void)option;
                    assert(0);
                    return -1;
                }
                int sceKernelStartThread(int thread, unsigned size, const void *arg)
                { (void)thread; (void)size; (void)arg; assert(0); return -1; }
                int sceKernelDeleteThread(int thread)
                { (void)thread; assert(0); return -1; }

                int main(void)
                {
                    assert(net_thread(0, 0) == 0);
                    net_end();
                    assert(resolve_calls == 2 && step == 10);
                    assert(command_starts == 3 && ftp_starts == 3);
                    assert(command_stops == 2 && ftp_stops == 2);
                    assert(devices == 12);
                    return 0;
                }
            """)
            executable = work / "test"
            subprocess.run(
                ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-I", str(work),
                 "-I", str(ROOT / "src"), str(work / "test.c"),
                 str(ROOT / "src" / "net.c"), "-o", str(executable)],
                check=True,
            )
            subprocess.run([str(executable)], check=True)
