#include <ftpvita.h>
#include <vitasdk.h>

#include "log.h"
#include "cmd_handler.h"
#include "net_handler.h"

extern int run;

int net_connected;

SceUID net_thid;
static int netctl_cb_id;

void net_start() {
    net_thid = sceKernelCreateThread("vitacompanion_net_thread",
                                     net_thread, 0x40, 0x10000, 0, 0, NULL);

    run = 1;
    sceKernelStartThread(net_thid, 0, NULL);
}

void net_end() {
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

	if (ftpvita_init(vita_ip, &vita_port) >= 0) {
		ftpvita_add_device("ux0:");
		ftpvita_add_device("ur0:");

        cmd_start();
		net_connected = 1;
	}
}

static void *netctl_cb(int event_type, void *arg)
{
	LOG("netctl cb: %d\n", event_type);

	if ((event_type == 1 || event_type == 2) && net_connected == 1) {
		ftpvita_fini();
        cmd_end();
		net_connected = 0;
	} else if (event_type == 3 && !net_connected) { /* IP obtained */
		do_net_connected();
	}

	return NULL;
}

int net_thread(unsigned int args, void *argp)
{
	int ret;

	sceKernelDelayThread(3 * 1000 * 1000);

	ret = sceNetCtlInit();
	LOG("sceNetCtlInit: 0x%08X\n", ret);

	ret = sceNetCtlInetRegisterCallback(netctl_cb, NULL, &netctl_cb_id);
	LOG("sceNetCtlInetRegisterCallback: 0x%08X\n", ret);

	while (run) {
		sceNetCtlCheckCallback();
		sceKernelDelayThread(1000 * 1000);
	}

	return 0;
}