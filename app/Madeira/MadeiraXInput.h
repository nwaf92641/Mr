/*
 * Madeira: the app's virtual XInput controller.
 *
 * One controller, two sources: the on-screen pad and a paired Bluetooth/MFi
 * pad. Both already exist and both already produce a `GamepadInput` frame
 * (GamepadBridge merges them); this file takes that one merged frame and makes
 * it visible to the guest as an Xbox 360 pad in XInput player slot 0.
 *
 * It is not a HID device and this is deliberate. Presenting a HID gamepad
 * would mean shipping winebus.sys, hidclass.sys and winexinput.sys with a
 * working winedevice/plugplay host underneath them -- none of which is in this
 * tree, and the one time winebus was autostarted on iOS it wedged the driver
 * host behind the service startup lock (task #19). A unix call into our own
 * process needs none of that.
 *
 * The path, end to end:
 *
 *   GamepadBridge.tick()  (60fps, main thread, merged frame)
 *     -> madeira_xinput_publish()          this file, stores the frame
 *     -> xinput1_4.dll!XInputGetState      wine patch, see
 *                                          patches/wine-xinput-virtual-pad.patch
 *     -> WINE_UNIX_CALL(madeira_pad_get_state)
 *     -> madeira_pad_unix_get_state()      this file, copies the frame back
 *
 * ntdll finds the table through virtual_ios.c's load_builtin_unixlib(), which
 * matches the module name "xinput1_4.dll" (and its 1_1/1_2/1_3/9_1_0
 * siblings) and hands it the table this file exports.
 *
 * The struct below is the ABI across that call and is mirrored by
 * `struct madeira_pad_state` in the wine patch. Two definitions exist because
 * the PE side is compiled in the wine tree, long before this app links; they
 * must stay byte-for-byte identical. Keep the fields fixed-width for the same
 * reason -- arm64ec (the guest) and arm64 (here) agree on these and nothing
 * else can be assumed.
 */

#ifndef MADEIRA_XINPUT_H
#define MADEIRA_XINPUT_H

#include <stdint.h>

struct madeira_pad_state
{
    uint32_t packet;         /* bumps only when a field below changes */
    uint16_t buttons;        /* XINPUT_GAMEPAD_* bits (see GamepadMap.xinputState) */
    uint8_t left_trigger;    /* XINPUT_GAMEPAD.bLeftTrigger, 0...255 */
    uint8_t right_trigger;   /* XINPUT_GAMEPAD.bRightTrigger, 0...255 */
    int16_t thumb_lx, thumb_ly, thumb_rx, thumb_ry;
};

struct madeira_pad_rumble
{
    uint16_t left, right;    /* XINPUT_VIBRATION motors, 0...65535 */
};

/* Codes in `const void *xinput_unix_call_funcs[]`. The order is the ABI: the
 * PE side passes these numbers, so append only. */
enum
{
    MADEIRA_PAD_GET_STATE = 0,
    MADEIRA_PAD_SET_RUMBLE = 1,
    MADEIRA_PAD_FUNC_COUNT = 2,
};

/*
 * Publish the current frame. Called on the main thread at the bridge's tick
 * rate while a source is live; the packet number is assigned here and changes
 * only when the frame does, which is what XInput's dwPacketNumber promises.
 *
 * `buttons` is XINPUT_GAMEPAD_* bits, `thumb_*` are the guest's -32768...32767
 * with +y up. Callers should use GamepadMap.xinputState(for:), which is where
 * that mapping is written down and tested.
 */
void madeira_xinput_publish(uint16_t buttons,
                            uint8_t left_trigger, uint8_t right_trigger,
                            int16_t thumb_lx, int16_t thumb_ly,
                            int16_t thumb_rx, int16_t thumb_ry);

/*
 * No controller any more: the guest's slot 0 reports
 * ERROR_DEVICE_NOT_CONNECTED on its next XInputGetState.
 *
 * Called when neither source is live, so a game that supports both pads and
 * keyboard falls back to the keyboard bridge instead of reading a stuck axis.
 * A frame that is merely *held* is published, not cleared: a pad at rest is a
 * connected pad.
 */
void madeira_xinput_clear(void);

/*
 * Where XInputSetState lands. `handler` is called on the guest thread that
 * called into the DLL -- not the main thread -- and is handed the two motor
 * speeds; hop to the right queue before touching UIKit or a GCController.
 * Passing NULL uninstalls.
 */
void madeira_xinput_set_rumble_handler(void (*handler)(uint16_t left, uint16_t right));

/*
 * The table ntdll registers for xinput1_*.dll. Declared here so the one
 * consumer outside this target -- build/ntdll-unix/virtual_ios.c, which
 * compiles into libntdll_unix.a and is linked into the same binary -- can
 * reach it without a header of its own.
 */
const void *const *madeira_xinput_unix_call_table(void);

#endif /* MADEIRA_XINPUT_H */
