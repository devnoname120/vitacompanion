#include "net.h"

#include "cmd.h"
#include "log.h"

#include <ftpvita.h>
#include <vitasdk.h>

extern int run;

int all_is_up;
int net_connected;

SceUID net_thid;
static int netctl_cb_id;

void net_start()
{
    net_thid = sceKernelCreateThread("vitacompanion_net_thread", net_thread, 0x40, 0x10000, 0, 0, NULL);
    sceKernelStartThread(net_thid, 0, NULL);
}

void net_end()
{
    sceNetCtlInetUnregisterCallback(netctl_cb_id);
    ftpvita_fini();
}

static void do_net_connected()
{
    char vita_ip[16];
    unsigned short int vita_port;

    LOG("do_net_connected\n");

#if ENABLE_LOGGING == 1
    ftpvita_set_info_log_cb(LOG);
    ftpvita_set_debug_log_cb(LOG);
#endif

    ftpvita_set_file_buf_size(512 * 1024);

    if (ftpvita_init(vita_ip, &vita_port) >= 0)
    {
        ftpvita_add_device("ux0:");
        ftpvita_add_device("ur0:");
        ftpvita_add_device("uma0:");
        ftpvita_add_device("imc0:");

        cmd_start();
        all_is_up = 1;
    }
}

static void* netctl_cb(int event_type, void* arg)
{
    LOG("netctl cb: %d\n", event_type);

    // TODO sceNetCtlInetGetResult

    if ((event_type == 1 || event_type == 2) && all_is_up == 1)
    {
        net_connected = 0;
        ftpvita_fini();
        cmd_end();
        all_is_up = 0;
    }
    else if (event_type == 3 && !all_is_up)
    { /* IP obtained */
        net_connected = 1;
        do_net_connected();
    }

    return NULL;
}

int net_thread(unsigned int args, void* argp)
{
    int ret;

    sceKernelDelayThread(3 * 1000 * 1000);

    ret = sceNetCtlInit();
    LOG("sceNetCtlInit: 0x%08X\n", ret);

    // If already connected to Wifi
    int state;
    sceNetCtlInetGetState(&state);
    LOG("sceNetCtlInetGetState: 0x%08X\n", state);
    netctl_cb(state, NULL);

    // FIXME: Add a mutex here, network status might change right before the callback is registered

    ret = sceNetCtlInetRegisterCallback(netctl_cb, NULL, &netctl_cb_id);
    LOG("sceNetCtlInetRegisterCallback: 0x%08X\n", ret);

    while (run)
    {
        sceNetCtlCheckCallback();
        sceKernelDelayThread(1000 * 1000);
    }

    return 0;
}
