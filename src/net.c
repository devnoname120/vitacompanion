#include "net.h"

#include "cmd.h"
#include "log.h"

#include <ftpvita.h>
#include <stdbool.h>
#include <stdint.h>
#include <taihen.h>
#include <vitasdk.h>

#define NETCTL_LIBRARY_NID 0xC6083AE1u
#define NETCTL_GET_STATE_NID 0x6D26AC68u
#define NETWORK_POLL_US (1000 * 1000)

extern volatile int run;

volatile int net_connected;

SceUID net_thid = -1;
static bool command_started;
static bool ftp_started;

int net_start(void)
{
    int result;

    net_thid = sceKernelCreateThread("vitacompanion_net_thread", net_thread, 0x40, 0x10000, 0, 0, NULL);
    if (net_thid < 0)
        return net_thid;

    result = sceKernelStartThread(net_thid, 0, NULL);
    if (result < 0)
    {
        sceKernelDeleteThread(net_thid);
        net_thid = -1;
        return result;
    }

    return 0;
}

void net_end()
{
    net_connected = 0;
    if (command_started)
    {
        cmd_end();
        command_started = false;
    }
    if (ftp_started)
    {
        ftpvita_fini();
        ftp_started = false;
    }
}

static void do_net_connected(void)
{
    /* An occupied FTP port (e.g. VitaShell) should not block commands. */
    if (!command_started)
    {
        int result = cmd_start();
        command_started = result >= 0;
        LOG("command listener: 0x%08X\n", result);
    }
    if (ftp_started || !run)
        return;

    char vita_ip[16];
    unsigned short vita_port;
#if ENABLE_LOGGING == 1
    ftpvita_set_info_log_cb(LOG_CALLBACK);
    ftpvita_set_debug_log_cb(LOG_CALLBACK);
#endif

    ftpvita_set_file_buf_size(64 * 1024);

    int result = ftpvita_init_existing_network(vita_ip, &vita_port);
    LOG("FTP listener: 0x%08X\n", result);
    if (result >= 0)
    {
        ftpvita_add_device("ux0:");
        ftpvita_add_device("ur0:");
        ftpvita_add_device("uma0:");
        ftpvita_add_device("imc0:");
        ftpvita_add_device("xmc0:");
        ftpvita_add_device("grw0:");

        ftp_started = true;
    }
}

int net_thread(unsigned int args, void* argp)
{
    int (*get_state)(int *) = NULL;
#if ENABLE_LOGGING == 1
    int last_state = -1;
    int last_result = -1;
#endif

    (void)args;
    (void)argp;

    /* SceShell owns Net and NetCtl. Calling sceNetCtlInit before Shell's
     * IPC client exists can crash its failure cleanup on firmware 3.60.
     * Resolve the query after the library loads. The query returns an error
     * until Shell has initialized the service. */
    while (run)
    {
        if (!get_state)
        {
            uintptr_t address = 0;
            int result = taiGetModuleExportFunc("SceLibNetCtl",
                NETCTL_LIBRARY_NID, NETCTL_GET_STATE_NID, &address);
            if (result >= 0 && address != 0)
            {
                get_state = (int (*)(int *))address;
                LOG("NetCtl state query resolved\n");
            }
        }

        if (get_state)
        {
            int state = SCE_NETCTL_STATE_DISCONNECTED;
            int result = get_state(&state);
#if ENABLE_LOGGING == 1
            if (result != last_result || state != last_state)
            {
                LOG("NetCtl state query: 0x%08X state=%d\n", result, state);
                last_result = result;
                last_state = state;
            }
#endif
            if (result >= 0 && state == SCE_NETCTL_STATE_CONNECTED && run)
            {
                net_connected = 1;
                do_net_connected();
            }
            else if (net_connected)
            {
                net_end();
            }
        }
        if (run)
            sceKernelDelayThread(NETWORK_POLL_US);
    }

    return 0;
}
