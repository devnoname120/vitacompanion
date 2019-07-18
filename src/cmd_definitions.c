#include <stdbool.h>
#include <vitasdk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
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
    },
    {
        .name = "reboot",
        .arg_count = 0,
        .executor = &cmd_reboot
    },
    {
        .name = "screen",
        .arg_count = 1,
        .executor = &cmd_screen
    },
    {
        .name = "kmodstart",
        .arg_count = 1,
        .executor = &cmd_load_start_kernel
    },
        {
        .name = "kmodstop",
        .arg_count = 1,
        .executor = &cmd_stop_unload_kernel
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
    strcpy(res_msg, "Apps destroyed.\n");
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

    strcpy(res_msg, "Launched.\n");
}

void cmd_reboot(char **arg_list, size_t arg_count, char *res_msg) {
    scePowerRequestColdReset();
    strcpy(res_msg, "Rebooting...\n");
}

void cmd_screen(char **arg_list, size_t arg_count, char *res_msg) {
    char *state = arg_list[1];

    if (!strcmp(state, "on")) {
        scePowerRequestDisplayOn();
        strcpy(res_msg, "Turning display on...\n");
    } else if (!strcmp(state, "off")) {
        scePowerRequestDisplayOff();
        strcpy(res_msg, "Turning display off...\n");
    } else {
        strcpy(res_msg, "Error: param should be 'on' or 'off'\n");
    }
}

void cmd_load_start_kernel(char **arg_list, size_t arg_count, char *res_msg) {
    char *module_path = arg_list[1];

    tai_module_args_t argg;
	argg.size = sizeof(argg);
	argg.pid = KERNEL_PID;
	argg.args = 0;
	argg.argp = NULL;
	argg.flags = 0;
	int mod_id = taiLoadStartKernelModuleForUser(module_path, &argg);

    if (mod_id >= 0)
        strcpy(res_msg, "%d\n", mod_id);
    else
        strcpy(res_msg, "Error: module loading failed: 0x%08X\n", mod_id);
}

void cmd_stop_unload_kernel(char **arg_list, size_t arg_count, char *res_msg) {
    int module_id = atoi(arg_list[1]);

    tai_module_args_t argg;
    argg.size = sizeof(argg);
    argg.pid = KERNEL_PID;
    argg.args = 0;
    argg.argp = NULL;
    argg.flags = 0;
    int ret = taiStopUnloadKernelModuleForUser(mod_id, &argg, NULL, NULL);
    printf("Stop unload module: 0x%08X\n", ret);

    if (ret >= 0)
        strcpy(res_msg, "Success.\n");
    else
        strcpy(res_msg, "Error: module unloading failed: 0x%08X\n", ret);