#include <stdio.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/sysmodule.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/net/netctl.h>
#include <taihen.h>
#include <ftpvita.h>
#include <string.h>
#include <stdarg.h>
#define TAIPOOL_AS_STDLIB
#include <taipool.h>

#define MOD_PATH "ux0:data/tai/kplugin.skprx"
#define LOADER_PORT 1338

#define ENABLE_LOGGING 0

static void LOG(const char *str, ...)
{
#if ENABLE_LOGGING == 1
	static SceUID logfd = -1;
	if (logfd == -1) {
		logfd = sceIoOpen("ux0:dump/vitacompanion.txt", SCE_O_APPEND |
			SCE_O_CREAT | SCE_O_WRONLY, 0666);
	}
	char buff[256];
	va_list arglist;
	va_start(arglist, str);
	int len = vsnprintf(buff, sizeof(buff), str, arglist);
	va_end(arglist);
	sceIoWrite(logfd, buff, len);
#endif
}

static SceUID net_thid;
static int net_connected;
static int netctl_cb_id;
static int run;
static SceUID loader_thid;
static int loader_sockfd;

static int loader_thread(SceSize args, void *argp)
{
	SceNetSockaddrIn loaderaddr;

	loader_sockfd = sceNetSocket("vitacompanion_loader_sock",
		SCE_NET_AF_INET,
		SCE_NET_SOCK_STREAM,
		0);

	loaderaddr.sin_family = SCE_NET_AF_INET;
	loaderaddr.sin_addr.s_addr = sceNetHtonl(SCE_NET_INADDR_ANY);
	loaderaddr.sin_port = sceNetHtons(LOADER_PORT);

	sceNetBind(loader_sockfd, (SceNetSockaddr *)&loaderaddr, sizeof(loaderaddr));

	sceNetListen(loader_sockfd, 128);

	while (run) {
		SceNetSockaddrIn clientaddr;
		int client_sockfd;
		unsigned int addrlen = sizeof(clientaddr);

		client_sockfd = sceNetAccept(loader_sockfd, (SceNetSockaddr *)&clientaddr, &addrlen);
		if (client_sockfd >= 0) {
			sceNetSocketClose(client_sockfd);

			SceUID mod_id;
			tai_module_args_t arg1;
			arg1.size = sizeof(arg1);
			arg1.pid = KERNEL_PID;
			arg1.args = 0;
			arg1.argp = NULL;
			arg1.flags = 0;
			mod_id = taiLoadStartKernelModuleForUser(MOD_PATH, &arg1);
			if (mod_id < 0)
				continue;

			tai_module_args_t arg2;
			arg2.size = sizeof(arg2);
			arg2.pid = KERNEL_PID;
			arg2.args = 0;
			arg2.argp = NULL;
			arg2.flags = 0;
			taiStopUnloadKernelModuleForUser(mod_id, &arg2, NULL, NULL);
		} else {
			break;
		}
	}

	sceKernelExitDeleteThread(0);
	return 0;
}

static void loader_start()
{
	loader_thid = sceKernelCreateThread("vitacompanion_loader_thread",
		loader_thread, 0x40, 0x10000, 0, 0, NULL);

	run = 1;
	sceKernelStartThread(loader_thid, 0, NULL);
}

static void loader_end()
{
	run = 0;
	sceNetSocketClose(loader_sockfd);
	sceKernelWaitThreadEnd(loader_thid, NULL, NULL);
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

		loader_start();
		net_connected = 1;
	}
}

static void *netctl_cb(int event_type, void *arg)
{
	LOG("netctl cb: %d\n", event_type);

	if ((event_type == 1 || event_type == 2) && net_connected == 1) {
		ftpvita_fini();
		loader_end();
		net_connected = 0;
	} else if (event_type == 3 && !net_connected) { /* IP obtained */
		do_net_connected();
	}

	return NULL;
}

static int net_thread(SceSize args, void *argp)
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

void __unused _start() __attribute__ ((weak, alias ("module_start")));
int __unused module_start(SceSize argc, const void *args)
{
	taipool_init(1 * 1024 * 1024);

#if ENABLE_LOGGING == 1
	SceUID fd = sceIoOpen("ux0:dump/vitacompanion_log.txt",
		SCE_O_TRUNC | SCE_O_CREAT | SCE_O_WRONLY, 0666);
	sceIoClose(fd);
#endif

	net_thid = sceKernelCreateThread("vitacompanion_net_thread",
		net_thread, 0x40, 0x10000, 0, 0, NULL);

	run = 1;
	sceKernelStartThread(net_thid, 0, NULL);

	return SCE_KERNEL_START_SUCCESS;
}

int __unused module_stop(SceSize argc, const void *args)
{
	run = 0;
	sceKernelWaitThreadEnd(net_thid, NULL, NULL);

	sceNetCtlInetUnregisterCallback(netctl_cb_id);

	if (net_connected) {
		ftpvita_fini();
		loader_end();
	}

	taipool_term();

	return SCE_KERNEL_STOP_SUCCESS;
}
