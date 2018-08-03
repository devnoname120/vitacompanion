#include <psp2/kernel/modulemgr.h>
#include <vitasdk.h>
#include "cmd.h"

#define LOADER_PORT 1338

extern int run;

static SceUID loader_thid;
static int loader_sockfd;

int cmd_handle(char *cmd, unsigned int size, char *res_msg) {
	if(!strcmp(cmd, "destroy\n")) {
		sceAppMgrDestroyOtherApp();

		char *msg = "Apps destroyed.\n";
		strncpy(res_msg, msg, strlen(msg));
	} else if(!strncmp(cmd, "launch ", strlen("launch "))) {
		char uri[32];

		// FIXME Dirty fix for trailing '\n'
		cmd[7 + 9] = '\0';
		snprintf(uri, 32, "psgm:play?titleid=%s", cmd + 7);

		for (int i = 0; i < 40; i++) {
			if (sceAppMgrLaunchAppByUri(0xFFFFF, uri) != 0) {
				break;
			}
			sceKernelDelayThread(10000);
		}

		char *msg = "Launched.\n";
		strncpy(res_msg, msg, strlen(msg));
	} else {
		char *msg = "Unknown command\n";
		strncpy(res_msg, msg, strlen(msg));
	}
}

int cmd_thread(unsigned int args, void *argp)
{
	struct SceNetSockaddrIn loaderaddr;

	loader_sockfd = sceNetSocket("vitacompanion_cmd_sock",
		SCE_NET_AF_INET,
		SCE_NET_SOCK_STREAM,
		0);

	loaderaddr.sin_family = SCE_NET_AF_INET;
	loaderaddr.sin_addr.s_addr = sceNetHtonl(SCE_NET_INADDR_ANY);
	loaderaddr.sin_port = sceNetHtons(LOADER_PORT);

	sceNetBind(loader_sockfd, (struct SceNetSockaddr *)&loaderaddr, sizeof(loaderaddr));

	sceNetListen(loader_sockfd, 128);

	while (run) {
		struct SceNetSockaddrIn clientaddr;
		int client_sockfd;
		unsigned int addrlen = sizeof(clientaddr);

		client_sockfd = sceNetAccept(loader_sockfd, (struct SceNetSockaddr *)&clientaddr, &addrlen);
		if (client_sockfd >= 0) {
		    char cmd[100] = {0};
            int size = sceNetRecv(client_sockfd, cmd, sizeof(cmd), 0);

            char res_msg[30] = "\n";

            if (size >= 0)
				cmd_handle(cmd, (unsigned int)size, res_msg);

            sceNetSend(client_sockfd, res_msg, strlen(res_msg), 0);
            sceNetSocketClose(client_sockfd);
		} else {
			break;
		}
	}

	sceKernelExitDeleteThread(0);
	return 0;
}

void cmd_start()
{
	loader_thid = sceKernelCreateThread("vitacompanion_cmd_thread",
                                        cmd_thread, 0x40, 0x10000, 0, 0, NULL);

	run = 1;
	sceKernelStartThread(loader_thid, 0, NULL);
}

void cmd_end()
{
	run = 0;
	sceNetSocketClose(loader_sockfd);
	sceKernelWaitThreadEnd(loader_thid, NULL, NULL);
}