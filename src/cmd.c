#include "cmd.h"

#include "cmd_definitions.h"
#include "parser.h"

#include <psp2/kernel/modulemgr.h>
#include <stdbool.h>
#include <vitasdk.h>

#define CMD_PORT 1338
#define ARG_MAX (20)

extern int run;
extern int all_is_up;
extern int net_connected;

static SceUID loader_thid;
static int loader_sockfd;

void cmd_handle(char* cmd, unsigned int cmd_size, char* res_msg)
{
    char* arg_list[ARG_MAX];

    size_t arg_count = parse_cmd(cmd, cmd_size, arg_list, ARG_MAX);
    const cmd_definition* cmd_def = cmd_get_definition(arg_list[0]);

    if (cmd_def == NULL)
    {
        strcpy(res_msg, "Error: Unknown command.\n");
        return;
    }

    if (cmd_def->arg_count != arg_count - 1)
    {
        strcpy(res_msg, "Error: Incorrect number of arguments.\n");
        return;
    }

    cmd_def->executor(arg_list, arg_count, res_msg);
}

int cmd_thread(unsigned int args, void* argp)
{
    struct SceNetSockaddrIn loaderaddr;

    loader_sockfd = sceNetSocket("vitacompanion_cmd_sock", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);

    loaderaddr.sin_family = SCE_NET_AF_INET;
    loaderaddr.sin_addr.s_addr = sceNetHtonl(SCE_NET_INADDR_ANY);
    loaderaddr.sin_port = sceNetHtons(CMD_PORT);

    sceNetBind(loader_sockfd, (struct SceNetSockaddr*)&loaderaddr, sizeof(loaderaddr));

    sceNetListen(loader_sockfd, 128);

    while (run && net_connected)
    {
        struct SceNetSockaddrIn clientaddr;
        int client_sockfd;
        unsigned int addrlen = sizeof(clientaddr);

        client_sockfd = sceNetAccept(loader_sockfd, (struct SceNetSockaddr*)&clientaddr, &addrlen);
        if (client_sockfd >= 0)
        {
            char cmd[100] = { 0 };
            int size = sceNetRecv(client_sockfd, cmd, sizeof(cmd), 0);

            char res_msg[60] = { 0 };

            if (size >= 0)
                cmd_handle(cmd, (unsigned int)size, res_msg);

            sceNetSend(client_sockfd, res_msg, strlen(res_msg), 0);
            sceNetSocketClose(client_sockfd);
        }
        else
        {
            break;
        }
    }

    sceKernelExitDeleteThread(0);
    return 0;
}

void cmd_start()
{
    loader_thid = sceKernelCreateThread("vitacompanion_cmd_thread", cmd_thread, 0x40, 0x10000, 0, 0, NULL);
    sceKernelStartThread(loader_thid, 0, NULL);
}

void cmd_end()
{
    sceNetSocketClose(loader_sockfd);
    sceKernelWaitThreadEnd(loader_thid, NULL, NULL);
}