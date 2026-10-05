#include "cmd.h"

#include "cmd_definitions.h"
#include "parser.h"

#include <psp2/kernel/modulemgr.h>
#include <stdbool.h>
#include <vitasdk.h>

#ifndef CMD_PORT
#define CMD_PORT 1338
#endif

#if CMD_PORT < 1 || CMD_PORT > 65535
#error CMD_PORT must be between 1 and 65535
#endif
#define ARG_MAX (20)
#define CMD_MAX (32)
#define CMD_REQUEST_MAX (2048)
#define CMD_RES_MAX (8192)
#define CMD_IO_TIMEOUT_US (15 * 1000 * 1000)
#define CMD_START_TIMEOUT_MS 5000
/* Plain reset if the shell has not restarted the console by then. */
#define CMD_REBOOT_FALLBACK_US (15 * 1000 * 1000)

/* SceShell's own restart; see src/shellutil_stub.S. */
int sceShellUtilRequestColdReset(int flags);

extern volatile int run;
extern volatile int all_is_up;
extern volatile int net_connected;

static SceUID loader_thid = -1;
static SceUID loader_client_mtx = -1;
static int loader_sockfd = -1;
static int loader_client_sockfd = -1;
static volatile int loader_stopping;
static volatile int loader_start_state;
static volatile int reboot_requested;

typedef struct {
    const cmd_definition* definition;
    char* args[ARG_MAX];
    size_t arg_count;
} parsed_command;

static int cmd_send_all(int socket, const char* message)
{
    unsigned int sent = 0;
    unsigned int length = (unsigned int)strlen(message);

    while (sent < length)
    {
        int result = sceNetSend(socket, message + sent, length - sent, 0);
        if (result <= 0)
            return result < 0 ? result : -1;
        sent += (unsigned int)result;
    }

    return (int)sent;
}

static int cmd_receive_request(int socket, char* request,
    unsigned int capacity)
{
    unsigned int used = 0;

    while (used < capacity)
    {
        int received = sceNetRecv(
            socket, request + used, capacity - used, 0);
        unsigned int i;

        if (received <= 0)
            return used > 0 ? (int)used : received;

        for (i = 0; i < (unsigned int)received; ++i)
        {
            if (request[used + i] == '\n' ||
                request[used + i] == '\r')
                return (int)(used + i + 1);
        }

        used += (unsigned int)received;
    }

    return (int)used;
}

static void response_append(char* response, const char* addition)
{
    size_t used = strlen(response);
    size_t available;

    if (used >= CMD_RES_MAX - 1)
        return;

    available = CMD_RES_MAX - used - 1;
    strncat(response, addition, available);
}

void cmd_handle(char* cmd, unsigned int cmd_size, char* res_msg)
{
    char* command_strings[CMD_MAX] = {0};
    parsed_command commands[CMD_MAX] = {0};
    size_t command_count = 0;
    size_t command_index;

    res_msg[0] = '\0';
    if (!parse_cmd_chain(cmd, cmd_size, command_strings, CMD_MAX,
        &command_count))
    {
        strcpy(res_msg, "Error: Too many chained commands.\n");
        return;
    }

    if (command_count == 0)
    {
        strcpy(res_msg, "Error: Empty command.\n");
        return;
    }

    for (command_index = 0; command_index < command_count; ++command_index)
    {
        parsed_command* parsed = &commands[command_index];

        parsed->arg_count = parse_cmd(command_strings[command_index],
            strlen(command_strings[command_index]), parsed->args, ARG_MAX);
        if (parsed->arg_count == 0)
        {
            strcpy(res_msg, "Error: Empty command.\n");
            return;
        }

        parsed->definition = cmd_get_definition(parsed->args[0]);
        if (parsed->definition == NULL)
        {
            strcpy(res_msg, "Error: Unknown command.\n");
            return;
        }

        if (parsed->arg_count - 1 <
                parsed->definition->min_arg_count ||
            parsed->arg_count - 1 >
                parsed->definition->max_arg_count)
        {
            strcpy(res_msg, "Error: Incorrect number of arguments.\n");
            return;
        }

        if (parsed->definition->validator &&
            !parsed->definition->validator(parsed->args,
                parsed->arg_count, res_msg))
            return;
    }

    for (command_index = 0; command_index < command_count; ++command_index)
    {
        char command_response[2048] = {0};
        parsed_command* parsed = &commands[command_index];

        parsed->definition->executor(
            parsed->args, parsed->arg_count, command_response);
        response_append(res_msg, command_response);
    }
}

int cmd_thread(unsigned int args, void* argp)
{
    struct SceNetSockaddrIn loaderaddr = {0};
    int result;

    (void)args;
    (void)argp;

    loader_sockfd = sceNetSocket("vitacompanion_cmd_sock", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (loader_sockfd < 0)
        goto exit;

    loaderaddr.sin_family = SCE_NET_AF_INET;
    loaderaddr.sin_addr.s_addr = sceNetHtonl(SCE_NET_INADDR_ANY);
    loaderaddr.sin_port = sceNetHtons(CMD_PORT);

    result = sceNetBind(loader_sockfd, (struct SceNetSockaddr*)&loaderaddr, sizeof(loaderaddr));
    if (result < 0)
        goto close_and_exit;

    result = sceNetListen(loader_sockfd, 8);
    if (result < 0)
        goto close_and_exit;
    loader_start_state = 1;

    while (run && net_connected && !loader_stopping)
    {
        struct SceNetSockaddrIn clientaddr = {0};
        int client_sockfd;
        unsigned int addrlen = sizeof(clientaddr);

        client_sockfd = sceNetAccept(loader_sockfd, (struct SceNetSockaddr*)&clientaddr, &addrlen);
        if (client_sockfd >= 0)
        {
            int timeout_us = CMD_IO_TIMEOUT_US;
            char cmd[CMD_REQUEST_MAX + 1] = { 0 };
            char res_msg[CMD_RES_MAX] = { 0 };
            int size;

            sceNetSetsockopt(client_sockfd, SCE_NET_SOL_SOCKET,
                SCE_NET_SO_SNDTIMEO, &timeout_us, sizeof(timeout_us));
            sceNetSetsockopt(client_sockfd, SCE_NET_SOL_SOCKET,
                SCE_NET_SO_RCVTIMEO, &timeout_us, sizeof(timeout_us));

            sceKernelLockMutex(loader_client_mtx, 1, NULL);
            if (loader_stopping)
            {
                sceKernelUnlockMutex(loader_client_mtx, 1);
                sceNetSocketClose(client_sockfd);
                break;
            }
            loader_client_sockfd = client_sockfd;
            sceKernelUnlockMutex(loader_client_mtx, 1);

            size = cmd_receive_request(
                client_sockfd, cmd, CMD_REQUEST_MAX);

            if (size > 0)
            {
                cmd[size] = '\0';
                if (size == CMD_REQUEST_MAX &&
                    cmd[size - 1] != '\n' &&
                    cmd[size - 1] != '\r')
                    strcpy(res_msg, "Error: Command request is too long.\n");
                else
                    cmd_handle(cmd, (unsigned int)size, res_msg);
            }

            if (res_msg[0] != '\0')
                cmd_send_all(client_sockfd, res_msg);

            sceKernelLockMutex(loader_client_mtx, 1, NULL);
            loader_client_sockfd = -1;
            sceKernelUnlockMutex(loader_client_mtx, 1);
            sceNetSocketClose(client_sockfd);

            if (reboot_requested)
            {
                /*
                 * Restart now that the reply has been sent. SceShell's own
                 * restart disconnects Wi-Fi first; after a plain
                 * scePowerRequestColdReset the Vita can drop off Wi-Fi
                 * shortly after boot and not reconnect.
                 */
                sceShellUtilRequestColdReset(0);
                sceKernelDelayThread(CMD_REBOOT_FALLBACK_US);
                scePowerRequestColdReset();
                break;
            }
        }
        else if (loader_stopping)
        {
            break;
        }
        else
        {
            sceKernelDelayThread(100 * 1000);
        }
    }

    goto exit;

close_and_exit:
    sceNetSocketClose(loader_sockfd);
    loader_sockfd = -1;

exit:
    if (loader_start_state == 0)
        loader_start_state = -1;
    sceKernelExitDeleteThread(0);
    return 0;
}

int cmd_start()
{
    int result;
    int waited_ms;

    if (loader_thid >= 0)
        return -1;

    loader_client_mtx = sceKernelCreateMutex(
        "vitacompanion_cmd_client_mutex", 0, 0, NULL);
    if (loader_client_mtx < 0)
        return loader_client_mtx;

    loader_thid = sceKernelCreateThread("vitacompanion_cmd_thread", cmd_thread, 0x40, 0x10000, 0, 0, NULL);
    if (loader_thid < 0)
    {
        result = loader_thid;
        sceKernelDeleteMutex(loader_client_mtx);
        loader_client_mtx = -1;
        return result;
    }

    loader_sockfd = -1;
    loader_client_sockfd = -1;
    loader_stopping = 0;
    loader_start_state = 0;
    result = sceKernelStartThread(loader_thid, 0, NULL);
    if (result < 0)
    {
        sceKernelDeleteThread(loader_thid);
        sceKernelDeleteMutex(loader_client_mtx);
        loader_thid = -1;
        loader_client_mtx = -1;
        return result;
    }

    for (waited_ms = 0;
        waited_ms < CMD_START_TIMEOUT_MS && loader_start_state == 0;
        waited_ms += 10)
        sceKernelDelayThread(10 * 1000);

    if (loader_start_state != 1)
    {
        loader_stopping = 1;
        if (loader_sockfd >= 0)
            sceNetSocketClose(loader_sockfd);
        sceKernelWaitThreadEnd(loader_thid, NULL, NULL);
        sceKernelDeleteMutex(loader_client_mtx);
        loader_thid = -1;
        loader_sockfd = -1;
        loader_client_mtx = -1;
        return -1;
    }

    return 0;
}

/* Reboots once the current response has been sent. */
void cmd_request_reboot()
{
    reboot_requested = 1;
}

void cmd_end()
{
    const int abort_flags = SCE_NET_SOCKET_ABORT_FLAG_RCV_PRESERVATION |
        SCE_NET_SOCKET_ABORT_FLAG_SND_PRESERVATION;

    if (loader_thid < 0)
        return;

    loader_stopping = 1;
    if (loader_sockfd >= 0)
        sceNetSocketClose(loader_sockfd);

    if (loader_client_mtx >= 0)
    {
        sceKernelLockMutex(loader_client_mtx, 1, NULL);
        if (loader_client_sockfd >= 0)
            sceNetSocketAbort(loader_client_sockfd, abort_flags);
        sceKernelUnlockMutex(loader_client_mtx, 1);
    }

    sceKernelWaitThreadEnd(loader_thid, NULL, NULL);
    if (loader_client_mtx >= 0)
        sceKernelDeleteMutex(loader_client_mtx);

    loader_thid = -1;
    loader_sockfd = -1;
    loader_client_sockfd = -1;
    loader_client_mtx = -1;
    loader_start_state = 0;
}
