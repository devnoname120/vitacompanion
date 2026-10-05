#include <vitacompanion_input.h>
#include <vitacompanion_kernel.h>

#include <psp2/touch.h>
#include <psp2kern/ctrl.h>
#include <psp2kern/display.h>
#include <psp2kern/kernel/sysmem/data_transfers.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/threadmgr.h>
#include <stdint.h>
#include <string.h>
#include <taihen.h>

#define CTRL_REFRESH_US (8 * 1000)
#define CTRL_EMULATION_SAMPLES 32
#define FRONT_TOUCH_REPORTS 6
#define REAR_TOUCH_REPORTS 4
#define SYNTHETIC_TOUCH_ID_BASE 0x70
#define USER_BUTTON_MASK 0x0000FFFF
#define SCREEN_MAX_WIDTH 1920
#define SCREEN_MAX_HEIGHT 1088
#define SCREEN_FRAME_WAIT_US (100 * 1000)

typedef struct {
    int active;
    uint8_t x;
    uint8_t y;
} analog_state;

typedef struct {
    int active;
    uint16_t x;
    uint16_t y;
} touch_state;

static SceUID state_mutex = -1;
static SceUID ctrl_thread_id = -1;
static volatile int ctrl_thread_running;
static uint32_t button_state;
static analog_state analog_states[2];
static touch_state touch_states[2][VITACOMPANION_TOUCH_SLOTS];

static SceUID screen_mutex = -1;
static uint8_t screen_row[SCREEN_MAX_WIDTH * 4];

static SceUID display_hook_id = -1;
static tai_hook_ref_t display_hook_ref;

/*
 * Screenshot handshake with the display hook. The hook copies the next
 * submitted frame before passing it on: while the app's display callback is
 * blocked there, neither that buffer nor the one on screen can go back to the
 * GPU, so the copy cannot tear however long it takes.
 */
enum {
    CAPTURE_IDLE,
    CAPTURE_REQUESTED,
    CAPTURE_COPYING,
    CAPTURE_DONE
};
static volatile int capture_state;
static int capture_index;
static SceUID capture_pid;
static uint8_t *capture_dst;
static SceSize capture_size;
static SceDisplayFrameBuf capture_frame;
static int capture_result;

static SceUID touch_hook_ids[4] = {-1, -1, -1, -1};
static tai_hook_ref_t touch_peek_ref;
static tai_hook_ref_t touch_peek_region_ref;
static tai_hook_ref_t touch_read_ref;
static tai_hook_ref_t touch_read_region_ref;

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

static void patch_touch_data(unsigned int port, SceTouchData *data,
    unsigned int count)
{
    touch_state points[VITACOMPANION_TOUCH_SLOTS];
    unsigned int maximum_reports;
    unsigned int buffer_index;
    int slot;

    if (port > VITACOMPANION_TOUCH_REAR || !data)
        return;

    state_lock();
    for (slot = 0; slot < VITACOMPANION_TOUCH_SLOTS; ++slot)
        points[slot] = touch_states[port][slot];
    state_unlock();

    maximum_reports = port == VITACOMPANION_TOUCH_FRONT
        ? FRONT_TOUCH_REPORTS : REAR_TOUCH_REPORTS;

    for (buffer_index = 0; buffer_index < count; ++buffer_index)
    {
        SceTouchData *current = &data[buffer_index];

        for (slot = 0; slot < VITACOMPANION_TOUCH_SLOTS; ++slot)
        {
            SceTouchReport *report;

            if (!points[slot].active ||
                current->reportNum >= maximum_reports)
                continue;

            report = &current->report[current->reportNum++];
            report->id = (uint8_t)(SYNTHETIC_TOUCH_ID_BASE + slot);
            report->force = 0x80;
            report->x = (int16_t)points[slot].x;
            report->y = (int16_t)points[slot].y;
            report->reserved[0] = 0;
            report->reserved[1] = 0;
            report->reserved[2] = 0;
            report->reserved[3] = 0;
            report->reserved[4] = 0;
            report->reserved[5] = 0;
            report->reserved[6] = 0;
            report->reserved[7] = 0;
            report->info = 0;
        }
    }
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

static void release_touch_hooks(void)
{
    tai_hook_ref_t *refs[] = {
        &touch_peek_ref,
        &touch_peek_region_ref,
        &touch_read_ref,
        &touch_read_region_ref
    };
    int i;

    for (i = 3; i >= 0; --i)
    {
        if (touch_hook_ids[i] >= 0)
        {
            taiHookReleaseForKernel(touch_hook_ids[i], *refs[i]);
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

static int get_displayed_frame(SceDisplayFrameBufInfo *frame, int *index)
{
    int head = ksceDisplayGetPrimaryHead();
    int result;

    /* Same lookup as xerpi's vita-udcd-uvc: index 0, else index 1. */
    for (*index = 0; *index < 2; ++*index)
    {
        memset(frame, 0, sizeof(*frame));
        frame->size = sizeof(*frame);
        result = ksceDisplayGetProcFrameBufInternal(-1, head, *index, frame);
        if (result >= 0 && frame->paddr != 0 && frame->framebuf.base)
            return 0;
    }

    return result < 0 ? result : -1;
}

/* Runs in the hook, in the process that submitted the frame. */
static int copy_submitted_frame(const SceDisplayFrameBuf *param)
{
    SceSize row_size = param->width * 4;
    unsigned int row;
    int result = 0;

    if (param->pitch < param->width ||
        row_size * param->height != capture_size)
        return -1;

    for (row = 0; row < param->height && result >= 0; ++row)
    {
        result = ksceKernelCopyFromUser(screen_row,
            (const uint8_t *)param->base + (SceSize)row * param->pitch * 4,
            row_size);
        if (result >= 0)
            result = ksceKernelCopyToUserProc(capture_pid,
                capture_dst + row * row_size, screen_row, row_size);
    }

    capture_frame = *param;
    return result;
}

/* Has the hook copy the next frame submitted to index; < 0 if none came. */
static int capture_next_frame(int index)
{
    int waited;

    if (display_hook_id < 0)
        return -1;

    capture_index = index;
    __sync_synchronize();
    capture_state = CAPTURE_REQUESTED;

    for (waited = 0; waited < SCREEN_FRAME_WAIT_US &&
        capture_state != CAPTURE_DONE; waited += 1000)
        ksceKernelDelayThread(1000);

    /* Withdraw the request, unless the hook has taken it already. */
    if (__sync_bool_compare_and_swap(&capture_state, CAPTURE_REQUESTED,
        CAPTURE_IDLE))
        return -1;

    while (capture_state != CAPTURE_DONE)
        ksceKernelDelayThread(1000);
    capture_state = CAPTURE_IDLE;
    return capture_result;
}

/* Copies the frame as it is, when the hook did not copy one. */
static int copy_current_frame(const SceDisplayFrameBufInfo *frame)
{
    SceSize row_size = frame->framebuf.width * 4;
    unsigned int row;
    int result = 0;

    for (row = 0; row < frame->framebuf.height && result >= 0; ++row)
    {
        result = ksceKernelCopyFromUserProc(frame->pid, screen_row,
            (const uint8_t *)frame->framebuf.base +
                (SceSize)row * frame->framebuf.pitch * 4,
            row_size);
        if (result >= 0)
            result = ksceKernelCopyToUser(capture_dst + row * row_size,
                screen_row, row_size);
    }

    return result;
}

int vitaCompanionKernelScreenCapture(vitacompanion_screen_info *info,
    void *dst, uint32_t dst_size)
{
    SceDisplayFrameBufInfo frame;
    vitacompanion_screen_info frame_info;
    SceSize frame_size;
    int index;
    int result;

    ksceKernelLockMutex(screen_mutex, 1, NULL);

    result = get_displayed_frame(&frame, &index);
    if (result >= 0 &&
        (frame.framebuf.width == 0 ||
         frame.framebuf.width > SCREEN_MAX_WIDTH ||
         frame.framebuf.height == 0 ||
         frame.framebuf.height > SCREEN_MAX_HEIGHT ||
         frame.framebuf.pitch < frame.framebuf.width))
        result = -1;
    if (result < 0)
        goto out;

    frame_size = frame.framebuf.width * 4 * frame.framebuf.height;

    if (dst && dst_size >= frame_size)
    {
        capture_pid = ksceKernelGetProcessId();
        capture_dst = dst;
        capture_size = frame_size;
        if (capture_next_frame(index) >= 0)
            frame.framebuf = capture_frame;
        else
            result = copy_current_frame(&frame);
        if (result < 0)
            goto out;
    }

    frame_info.width = frame.framebuf.width;
    frame_info.height = frame.framebuf.height;
    frame_info.pixelformat = frame.framebuf.pixelformat;
    result = ksceKernelCopyToUser(info, &frame_info, sizeof(frame_info));
    if (result >= 0)
        result = (int)frame_size;

out:
    ksceKernelUnlockMutex(screen_mutex, 1);
    return result;
}

/*
 * Sees every frame submitted to a display, the same hook point PSVshell
 * uses. Index 0 is the foreground application's framebuffer, 1 the shell's.
 */
static int set_frame_buf_hook(int head, int index,
    const SceDisplayFrameBuf *param, int sync)
{
    if (index == capture_index && param && param->base &&
        head == ksceDisplayGetPrimaryHead() &&
        __sync_bool_compare_and_swap(&capture_state, CAPTURE_REQUESTED,
            CAPTURE_COPYING))
    {
        capture_result = copy_submitted_frame(param);
        __sync_synchronize();
        capture_state = CAPTURE_DONE;
    }

    return TAI_CONTINUE(int, display_hook_ref, head, index, param, sync);
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

    screen_mutex = ksceKernelCreateMutex(
        "vitacompanion_screen_mutex", 0, 0, NULL);
    if (screen_mutex < 0)
    {
        ksceKernelDeleteMutex(state_mutex);
        state_mutex = -1;
        return SCE_KERNEL_START_FAILED;
    }

    result = install_touch_hooks();
    if (result < 0)
    {
        ksceKernelDeleteMutex(screen_mutex);
        screen_mutex = -1;
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
        ksceKernelDeleteMutex(screen_mutex);
        screen_mutex = -1;
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
        ksceKernelDeleteMutex(screen_mutex);
        screen_mutex = -1;
        ksceKernelDeleteMutex(state_mutex);
        state_mutex = -1;
        return SCE_KERNEL_START_FAILED;
    }

    /* Optional: without it, screenshots copy the frame as it is. */
    display_hook_id = taiHookFunctionExportForKernel(KERNEL_PID,
        &display_hook_ref, "SceDisplay", 0x9FED47AC, 0x16466675,
        set_frame_buf_hook);

    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args)
{
    (void)argc;
    (void)args;

    if (display_hook_id >= 0)
    {
        taiHookReleaseForKernel(display_hook_id, display_hook_ref);
        display_hook_id = -1;
    }

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

    if (screen_mutex >= 0)
    {
        ksceKernelDeleteMutex(screen_mutex);
        screen_mutex = -1;
    }

    if (state_mutex >= 0)
    {
        ksceKernelDeleteMutex(state_mutex);
        state_mutex = -1;
    }

    return SCE_KERNEL_STOP_SUCCESS;
}

int _start(SceSize argc, const void *args)
    __attribute__((weak, alias("module_start")));
