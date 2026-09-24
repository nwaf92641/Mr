/*
 * Madeira: the app's virtual XInput controller -- storage and the unix-call
 * side. See MadeiraXInput.h for the path through the guest and why this is a
 * unix call rather than a HID device.
 *
 * Threading: one frame, written by the main thread at the bridge's tick rate
 * and read by whichever guest thread calls XInputGetState. The guest may read
 * from any thread (a game is free to poll XInput off its input thread), so the
 * copy in and the copy out are both under `lock`. The whole frame is 24 bytes,
 * which is smaller than the bookkeeping for anything cleverer, and a torn
 * frame -- new sticks, old buttons -- would be a game responding to input the
 * user never made.
 */

#include "MadeiraXInput.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

/* NTSTATUS, spelled as int32_t because this file is compiled outside the wine
 * tree and cannot include ntstatus.h. The values are the ABI: the PE side
 * compares against the same ones. */
#define MADEIRA_STATUS_SUCCESS              0x00000000
#define MADEIRA_STATUS_INVALID_PARAMETER    ((int32_t)0xC000000D)
#define MADEIRA_STATUS_DEVICE_NOT_CONNECTED ((int32_t)0xC000009C)

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct madeira_pad_state frame;
static int present;
static uint32_t generation;

static void (*rumble_handler)(uint16_t left, uint16_t right);

/* Only the fields the guest can see. `packet` is ours, and comparing it would
 * make every call look like a change. */
static int same_frame(const struct madeira_pad_state *a,
                      const struct madeira_pad_state *b)
{
    return a->buttons == b->buttons
        && a->left_trigger == b->left_trigger
        && a->right_trigger == b->right_trigger
        && a->thumb_lx == b->thumb_lx && a->thumb_ly == b->thumb_ly
        && a->thumb_rx == b->thumb_rx && a->thumb_ry == b->thumb_ry;
}

void madeira_xinput_publish(uint16_t buttons,
                            uint8_t left_trigger, uint8_t right_trigger,
                            int16_t thumb_lx, int16_t thumb_ly,
                            int16_t thumb_rx, int16_t thumb_ry)
{
    struct madeira_pad_state next = {
        .packet = 0,
        .buttons = buttons,
        .left_trigger = left_trigger,
        .right_trigger = right_trigger,
        .thumb_lx = thumb_lx, .thumb_ly = thumb_ly,
        .thumb_rx = thumb_rx, .thumb_ry = thumb_ry,
    };

    pthread_mutex_lock(&lock);
    if (!present || !same_frame(&frame, &next)) generation++;
    next.packet = generation;
    frame = next;
    present = 1;
    pthread_mutex_unlock(&lock);
}

void madeira_xinput_clear(void)
{
    pthread_mutex_lock(&lock);
    present = 0;
    memset(&frame, 0, sizeof(frame));
    pthread_mutex_unlock(&lock);
}

void madeira_xinput_set_rumble_handler(void (*handler)(uint16_t left, uint16_t right))
{
    /* Read without the lock on the call side: the handler is installed once, at
     * startup, and a stale read would at worst send one frame to the previous
     * handler. */
    rumble_handler = handler;
}

static int32_t madeira_pad_unix_get_state(void *arg)
{
    struct madeira_pad_state *out = arg;
    int32_t status = MADEIRA_STATUS_SUCCESS;

    if (!out) return MADEIRA_STATUS_INVALID_PARAMETER;

    pthread_mutex_lock(&lock);
    if (present) *out = frame;
    else status = MADEIRA_STATUS_DEVICE_NOT_CONNECTED;
    pthread_mutex_unlock(&lock);

    return status;
}

static int32_t madeira_pad_unix_set_rumble(void *arg)
{
    const struct madeira_pad_rumble *rumble = arg;
    void (*handler)(uint16_t, uint16_t) = rumble_handler;

    if (!rumble) return MADEIRA_STATUS_INVALID_PARAMETER;

    /* No handler, or nobody listening: still SUCCESS. A game that gets an error
     * back from XInputSetState is entitled to think it is talking to a pad that
     * cannot rumble and may stop asking; the motors are simply not ours to
     * drive yet. */
    if (handler) handler(rumble->left, rumble->right);
    return MADEIRA_STATUS_SUCCESS;
}

static const void *const xinput_unix_call_funcs[MADEIRA_PAD_FUNC_COUNT] =
{
    (const void *)madeira_pad_unix_get_state,
    (const void *)madeira_pad_unix_set_rumble,
};

const void *const *madeira_xinput_unix_call_table(void)
{
    return xinput_unix_call_funcs;
}
