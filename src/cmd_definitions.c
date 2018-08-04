#include <stdbool.h>
#include <vitasdk.h>
#include <stdio.h>
#include "cmd_definitions.h"

#define COUNT_OF(arr) (sizeof(arr)/sizeof(arr[0]))

const cmd_definition cmd_definitions[] = {
    {
        .name = "destroy",
        .arg_count = 0,
        .executor = &cmd_destroy
    },
    {
        .name = "launch",
        .arg_count = 1,
        .executor = &cmd_launch
    }
};

const cmd_definition *cmd_get_definition(char *cmd_name) {
    for (unsigned int i=0; i < COUNT_OF(cmd_definitions); i ++) {
        if (!strcmp(cmd_name, cmd_definitions[i].name)) {
            return &(cmd_definitions[i]);
        }
    }

    return NULL;
}

void cmd_destroy(char **arg_list, size_t arg_count, char *res_msg) {
    sceAppMgrDestroyOtherApp();

    char *msg = "Apps destroyed.\n";
    strncpy(res_msg, msg, strlen(msg));
}

void cmd_launch(char **arg_list, size_t arg_count, char *res_msg) {
    char uri[32];

    snprintf(uri, 32, "psgm:play?titleid=%s", arg_list[1]);

    for (int i = 0; i < 40; i++) {
        if (sceAppMgrLaunchAppByUri(0xFFFFF, uri) != 0) {
            break;
        }
        sceKernelDelayThread(10000);
    }

    char *msg = "Launched.\n";
    strncpy(res_msg, msg, strlen(msg));
}