#include <vitacompanion_input.h>
#include <vitacompanion_kernel.h>

#include "touch_patch.h"

#include <psp2kern/ctrl.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/suspend.h>
#include <psp2kern/kernel/sysmem/data_transfers.h>
#include <psp2kern/kernel/threadmgr.h>
#include <stdint.h>
#include <taihen.h>

#define CTRL_REFRESH_US (8 * 1000)
#define CTRL_EMULATION_SAMPLES 32
#define TOUCH_EXT_MAX_BUFFERS 64
#define SCE_TOUCH_USER_LIBRARY_NID 0x3E4F4A81
#define SCE_TOUCH_PEEK_REGION_EXT_NID 0x2CF6D7E2
#define SCE_TOUCH_READ_REGION_EXT_NID 0x9F0ACAF9
#define USER_BUTTON_MASK 0x0000FFFF

typedef struct {
    int active;
    uint8_t x;
    uint8_t y;
} analog_state;

static SceUID state_mutex = -1;
static SceUID ctrl_thread_id = -1;
static volatile int ctrl_thread_running;
static uint32_t button_state;
static analog_state analog_states[2];
static vitacompanion_touch_point
    touch_states[2][VITACOMPANION_TOUCH_SLOTS];

static SceUID touch_hook_ids[4] = {-1, -1, -1, -1};
static SceUID touch_ext_hook_ids[2] = {-1, -1};
static tai_hook_ref_t touch_peek_ref;
static tai_hook_ref_t touch_peek_region_ref;
static tai_hook_ref_t touch_read_ref;
static tai_hook_ref_t touch_read_region_ref;
static tai_hook_ref_t touch_peek_region_ext_ref;
static tai_hook_ref_t touch_read_region_ext_ref;

static void state_lock(void)
{
    if (state_mutex >= 0)
        ksceKernelLockMutex(state_mutex, 1, NULL);
}

static void state_unlock(void)
{
    if (state_mutex >= 0)
        ksceKernelUnlockMutex(state_mutex, 1);
}

static void clear_state(void)
{
    int port;
    int slot;

    button_state = 0;
    for (port = 0; port < 2; ++port)
    {
        analog_states[port].active = 0;
        analog_states[port].x = VITACOMPANION_ANALOG_CENTER;
        analog_states[port].y = VITACOMPANION_ANALOG_CENTER;
    }

    for (port = 0; port < 2; ++port)
    {
        for (slot = 0; slot < VITACOMPANION_TOUCH_SLOTS; ++slot)
        {
            touch_states[port][slot].active = 0;
            touch_states[port][slot].x = 0;
            touch_states[port][slot].y = 0;
        }
    }
}

static void reset_ctrl_emulation(void)
{
    ksceCtrlSetButtonEmulation(0, 0, 0, 0, CTRL_EMULATION_SAMPLES);
    ksceCtrlSetAnalogEmulation(0, 0,
        VITACOMPANION_ANALOG_CENTER, VITACOMPANION_ANALOG_CENTER,
        VITACOMPANION_ANALOG_CENTER, VITACOMPANION_ANALOG_CENTER,
        VITACOMPANION_ANALOG_CENTER, VITACOMPANION_ANALOG_CENTER,
        VITACOMPANION_ANALOG_CENTER, VITACOMPANION_ANALOG_CENTER, 0);
}

static int ctrl_thread(unsigned int args, void *argp)
{
    uint32_t last_buttons = 0;
    int analog_was_active = 0;

    (void)args;
    (void)argp;

    while (ctrl_thread_running)
    {
        uint32_t buttons;
        analog_state left;
        analog_state right;
        int analog_active;

        state_lock();
        buttons = button_state;
        left = analog_states[VITACOMPANION_STICK_LEFT];
        right = analog_states[VITACOMPANION_STICK_RIGHT];
        state_unlock();

        if (buttons != 0 || last_buttons != 0)
        {
            ksceCtrlSetButtonEmulation(
                0, 0, buttons & USER_BUTTON_MASK, buttons,
                CTRL_EMULATION_SAMPLES);
        }
        last_buttons = buttons;

        analog_active = left.active || right.active;
        if (analog_active)
        {
            uint8_t lx = left.active ? left.x : VITACOMPANION_ANALOG_CENTER;
            uint8_t ly = left.active ? left.y : VITACOMPANION_ANALOG_CENTER;
            uint8_t rx = right.active ? right.x : VITACOMPANION_ANALOG_CENTER;
            uint8_t ry = right.active ? right.y : VITACOMPANION_ANALOG_CENTER;

            ksceCtrlSetAnalogEmulation(
                0, 0, lx, ly, rx, ry, lx, ly, rx, ry,
                CTRL_EMULATION_SAMPLES);
        }
        else if (analog_was_active)
        {
            ksceCtrlSetAnalogEmulation(0, 0,
                VITACOMPANION_ANALOG_CENTER,
                VITACOMPANION_ANALOG_CENTER,
                VITACOMPANION_ANALOG_CENTER,
                VITACOMPANION_ANALOG_CENTER,
                VITACOMPANION_ANALOG_CENTER,
                VITACOMPANION_ANALOG_CENTER,
                VITACOMPANION_ANALOG_CENTER,
                VITACOMPANION_ANALOG_CENTER, 0);
        }
        analog_was_active = analog_active;

        ksceKernelDelayThread(CTRL_REFRESH_US);
    }

    reset_ctrl_emulation();
    ksceKernelExitDeleteThread(0);
    return 0;
}

static int snapshot_touch_points(unsigned int port,
    vitacompanion_touch_point points[VITACOMPANION_TOUCH_SLOTS])
{
    int active = 0;
    int slot;

    if (port > VITACOMPANION_TOUCH_REAR || !points)
        return 0;

    state_lock();
    for (slot = 0; slot < VITACOMPANION_TOUCH_SLOTS; ++slot)
    {
        points[slot] = touch_states[port][slot];
        active |= points[slot].active;
    }
    state_unlock();

    return active;
}

static unsigned int patch_touch_data(unsigned int port, SceTouchData *data,
    unsigned int count)
{
    vitacompanion_touch_point points[VITACOMPANION_TOUCH_SLOTS];
    unsigned int injected;

    if (!snapshot_touch_points(port, points))
        return 0;

    injected = vitacompanion_patch_touch_data(port, data, count, points);
    if (injected != 0)
        ksceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
    return injected;
}

static unsigned int patch_user_touch_data(unsigned int port,
    SceTouchData *user_data, unsigned int count)
{
    vitacompanion_touch_point points[VITACOMPANION_TOUCH_SLOTS];
    SceTouchData sample;
    unsigned int buffer_index;
    unsigned int injected = 0;

    if (!user_data || count == 0 || count > TOUCH_EXT_MAX_BUFFERS ||
        !snapshot_touch_points(port, points))
        return 0;

    for (buffer_index = 0; buffer_index < count; ++buffer_index)
    {
        SceTouchData *user_sample = &user_data[buffer_index];
        unsigned int sample_injected;

        if (ksceKernelCopyFromUser(
                &sample, user_sample, sizeof(sample)) < 0)
            break;

        sample_injected = vitacompanion_patch_touch_data(
            port, &sample, 1, points);
        if (sample_injected == 0)
            continue;

        if (ksceKernelCopyToUser(
                user_sample, &sample, sizeof(sample)) < 0)
            break;
        injected += sample_injected;
    }

    if (injected != 0)
        ksceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
    return injected;
}

static int touch_peek_hook(unsigned int port, SceTouchData *data,
    unsigned int count)
{
    int result = TAI_CONTINUE(int, touch_peek_ref, port, data, count);

    if (result > 0)
        patch_touch_data(port, data, (unsigned int)result);
    return result;
}

static int touch_peek_region_hook(unsigned int port, SceTouchData *data,
    unsigned int count, int region)
{
    int result = TAI_CONTINUE(int, touch_peek_region_ref,
        port, data, count, region);

    if (result > 0)
        patch_touch_data(port, data, (unsigned int)result);
    return result;
}

static int touch_read_hook(unsigned int port, SceTouchData *data,
    unsigned int count)
{
    int result = TAI_CONTINUE(int, touch_read_ref, port, data, count);

    if (result > 0)
        patch_touch_data(port, data, (unsigned int)result);
    return result;
}

static int touch_read_region_hook(unsigned int port, SceTouchData *data,
    unsigned int count, int region)
{
    int result = TAI_CONTINUE(int, touch_read_region_ref,
        port, data, count, region);

    if (result > 0)
        patch_touch_data(port, data, (unsigned int)result);
    return result;
}

static int touch_peek_region_ext_hook(unsigned int port, SceTouchData *data,
    unsigned int count, int region)
{
    int result = TAI_CONTINUE(int, touch_peek_region_ext_ref,
        port, data, count, region);

    if (result > 0)
        patch_user_touch_data(port, data, (unsigned int)result);
    return result;
}

static int touch_read_region_ext_hook(unsigned int port, SceTouchData *data,
    unsigned int count, int region)
{
    int result = TAI_CONTINUE(int, touch_read_region_ext_ref,
        port, data, count, region);

    if (result > 0)
        patch_user_touch_data(port, data, (unsigned int)result);
    return result;
}

static void install_touch_ext_hooks(const char *module_name)
{
    static const uint32_t nids[2] = {
        SCE_TOUCH_PEEK_REGION_EXT_NID,
        SCE_TOUCH_READ_REGION_EXT_NID
    };
    static const void *functions[2] = {
        touch_peek_region_ext_hook,
        touch_read_region_ext_hook
    };
    tai_hook_ref_t *refs[2] = {
        &touch_peek_region_ext_ref,
        &touch_read_region_ext_ref
    };
    int i;

    for (i = 0; i < 2; ++i)
    {
        touch_ext_hook_ids[i] = taiHookFunctionExportForKernel(
            KERNEL_PID, refs[i], module_name, SCE_TOUCH_USER_LIBRARY_NID,
            nids[i], functions[i]);
    }
}

static void release_touch_hooks(void)
{
    tai_hook_ref_t *driver_refs[] = {
        &touch_peek_ref,
        &touch_peek_region_ref,
        &touch_read_ref,
        &touch_read_region_ref
    };
    tai_hook_ref_t *ext_refs[] = {
        &touch_peek_region_ext_ref,
        &touch_read_region_ext_ref
    };
    int i;

    for (i = 1; i >= 0; --i)
    {
        if (touch_ext_hook_ids[i] >= 0)
            taiHookReleaseForKernel(touch_ext_hook_ids[i], *ext_refs[i]);
        touch_ext_hook_ids[i] = -1;
    }

    for (i = 3; i >= 0; --i)
    {
        if (touch_hook_ids[i] >= 0)
        {
            taiHookReleaseForKernel(touch_hook_ids[i], *driver_refs[i]);
            touch_hook_ids[i] = -1;
        }
    }
}

static int install_touch_hooks(void)
{
    static const uint32_t nids[4] = {
        0xBAD1960B,
        0x9B3F7207,
        0x70C8AACE,
        0x9A91F624
    };
    static const void *functions[4] = {
        touch_peek_hook,
        touch_peek_region_hook,
        touch_read_hook,
        touch_read_region_hook
    };
    tai_hook_ref_t *refs[4] = {
        &touch_peek_ref,
        &touch_peek_region_ref,
        &touch_read_ref,
        &touch_read_region_ref
    };
    tai_module_info_t info;
    const char *module_name = "SceTouch";
    int i;

    info.size = sizeof(info);
    if (taiGetModuleInfoForKernel(KERNEL_PID, module_name, &info) < 0)
    {
        module_name = "SceTouchDummy";
        info.size = sizeof(info);
        if (taiGetModuleInfoForKernel(KERNEL_PID, module_name, &info) < 0)
            return -1;
    }

    for (i = 0; i < 4; ++i)
    {
        touch_hook_ids[i] = taiHookFunctionExportForKernel(
            KERNEL_PID, refs[i], module_name, TAI_ANY_LIBRARY,
            nids[i], functions[i]);
        if (touch_hook_ids[i] < 0)
        {
            int result = touch_hook_ids[i];
            release_touch_hooks();
            return result;
        }
    }

    install_touch_ext_hooks(module_name);

    return 0;
}

int vitaCompanionKernelGetApiVersion(void)
{
    return VITACOMPANION_KERNEL_ABI_VERSION;
}

int vitaCompanionKernelSetButtons(uint32_t buttons, int pressed)
{
    state_lock();
    if (pressed)
        button_state |= buttons;
    else
        button_state &= ~buttons;
    state_unlock();
    return 0;
}

int vitaCompanionKernelSetAnalog(int stick, int x, int y, int active)
{
    if (stick < VITACOMPANION_STICK_LEFT ||
        stick > VITACOMPANION_STICK_RIGHT ||
        x < 0 || x > 255 || y < 0 || y > 255)
        return -1;

    state_lock();
    analog_states[stick].active = active != 0;
    analog_states[stick].x = active
        ? (uint8_t)x : VITACOMPANION_ANALOG_CENTER;
    analog_states[stick].y = active
        ? (uint8_t)y : VITACOMPANION_ANALOG_CENTER;
    state_unlock();
    return 0;
}

int vitaCompanionKernelSetTouch(int port, int slot_and_active, int x, int y)
{
    int slot = slot_and_active & 0xff;
    int active = (slot_and_active & VITACOMPANION_TOUCH_ACTIVE_FLAG) != 0;

    if (port < VITACOMPANION_TOUCH_FRONT ||
        port > VITACOMPANION_TOUCH_REAR ||
        (slot_and_active & ~0x103) != 0 ||
        slot < 0 ||
        slot >= VITACOMPANION_TOUCH_SLOTS ||
        x < 0 || x > VITACOMPANION_TOUCH_MAX_X ||
        y < 0 || y > VITACOMPANION_TOUCH_MAX_Y)
        return -1;

    state_lock();
    touch_states[port][slot].active = active != 0;
    touch_states[port][slot].x = (uint16_t)x;
    touch_states[port][slot].y = (uint16_t)y;
    state_unlock();
    return 0;
}

int vitaCompanionKernelReset(void)
{
    state_lock();
    clear_state();
    state_unlock();
    return 0;
}

int module_start(SceSize argc, const void *args)
{
    int result;

    (void)argc;
    (void)args;

    clear_state();
    state_mutex = ksceKernelCreateMutex(
        "vitacompanion_input_mutex", 0, 0, NULL);
    if (state_mutex < 0)
        return SCE_KERNEL_START_FAILED;

    result = install_touch_hooks();
    if (result < 0)
    {
        ksceKernelDeleteMutex(state_mutex);
        state_mutex = -1;
        return SCE_KERNEL_START_FAILED;
    }

    ctrl_thread_id = ksceKernelCreateThread(
        "vitacompanion_input_thread", ctrl_thread,
        0x40, 0x1000, 0, 0, NULL);
    if (ctrl_thread_id < 0)
    {
        release_touch_hooks();
        ksceKernelDeleteMutex(state_mutex);
        state_mutex = -1;
        return SCE_KERNEL_START_FAILED;
    }

    ctrl_thread_running = 1;
    result = ksceKernelStartThread(ctrl_thread_id, 0, NULL);
    if (result < 0)
    {
        ctrl_thread_running = 0;
        ksceKernelDeleteThread(ctrl_thread_id);
        ctrl_thread_id = -1;
        release_touch_hooks();
        ksceKernelDeleteMutex(state_mutex);
        state_mutex = -1;
        return SCE_KERNEL_START_FAILED;
    }

    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args)
{
    (void)argc;
    (void)args;

    state_lock();
    clear_state();
    state_unlock();

    ctrl_thread_running = 0;
    if (ctrl_thread_id >= 0)
    {
        ksceKernelWaitThreadEnd(ctrl_thread_id, NULL, NULL);
        ctrl_thread_id = -1;
    }

    release_touch_hooks();
    reset_ctrl_emulation();

    if (state_mutex >= 0)
    {
        ksceKernelDeleteMutex(state_mutex);
        state_mutex = -1;
    }

    return SCE_KERNEL_STOP_SUCCESS;
}

int _start(SceSize argc, const void *args)
    __attribute__((weak, alias("module_start")));
