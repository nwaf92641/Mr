import Foundation
import GameController
import CoreHaptics

/// The guest's Xbox pad: one frame in, XInput out.
///
/// `GamepadBridge` already merges the on-screen pad and a paired controller into
/// a single `GamepadInput`; this hands that frame to the app's virtual pad
/// (`MadeiraXInput.h`), and from there to `xinput1_4.dll` in the guest, where a
/// game reads it with `XInputGetState` as an Xbox 360 pad in player slot 0 —
/// buttons, both sticks, both triggers, and rumble back the other way. The wine
/// half of that path is `patches/wine-xinput-virtual-pad.patch`.
///
/// Why it is not a HID device: this tree ships no winebus.sys, no hidclass.sys
/// and no winedevice host to enumerate one, and autostarting winebus on iOS
/// wedged the driver host behind the service startup lock (task #19). A unix
/// call into our own process needs none of those pieces.
///
/// There is no switch here on purpose. The pad exists exactly while something
/// feeds it — a paired controller, or the on-screen pad — so a user who turns
/// both off (ENABLED = 0 in `madeira-gamepad.txt`, and the pad's own switch in
/// Settings) gets a guest with no pad at all, which is the opt-out for a game
/// that reacts badly to one being present.
final class XInputBridge {
    static let shared = XInputBridge()

    private var rumble = HapticRumble()
    private var rumbleOn = false
    private var rumbleReports = 0

    private init() {
        // Installed before the guest can call XInputSetState. The guest's DLL
        // binds to the app's table while it loads, which is while a game is
        // starting, and frames are already in flight by then.
        madeira_xinput_set_rumble_handler { left, right in
            XInputBridge.receivedRumble(left: left, right: right)
        }
    }

    /// Hand the current frame to the guest. Main thread; the bridge's tick calls
    /// this at its own rate while a source is live.
    ///
    /// The packet number XInput promises — it changes only when the state does —
    /// is assigned on the C side from the frame itself, so a held stick does not
    /// look like movement and a game that compares packets can skip work.
    func publish(_ frame: GamepadInput) {
        dispatchPrecondition(condition: .onQueue(.main))
        let state = GamepadMap.xinputState(for: frame)
        madeira_xinput_publish(state.buttons,
                               state.leftTrigger, state.rightTrigger,
                               state.leftX, state.leftY,
                               state.rightX, state.rightY)
    }

    /// No source is live: the guest's slot 0 reports DEVICE_NOT_CONNECTED.
    ///
    /// Called when both the controller and the on-screen pad have gone away, so
    /// a game falls back to the keyboard bridge instead of reading a pad that is
    /// still holding whatever was down when the last thumb lifted.
    func clear() {
        dispatchPrecondition(condition: .onQueue(.main))
        madeira_xinput_clear()
        rumble.stop()
        rumbleOn = false
    }

    // MARK: - rumble

    /// XInputSetState, arriving from the guest.
    ///
    /// On the guest thread that called into the DLL, not the main thread, and
    /// possibly before the app has seen a controller at all. Hop first, then
    /// touch anything: `rumble` holds a `GCController` and a `CHHapticEngine`.
    private static func receivedRumble(left: UInt16, right: UInt16) {
        DispatchQueue.main.async {
            XInputBridge.shared.deliverRumble(left: left, right: right)
        }
    }

    private func deliverRumble(left: UInt16, right: UInt16) {
        if left == 0 && right == 0 {
            if rumbleOn {
                rumbleOn = false
                rumble.stop()
                fputs("[xinput] rumble off\n", stderr)
            }
            return
        }

        rumbleOn = true
        rumble.set(level: max(left, right))

        // A game holding a motor on calls this every frame; report the first few
        // so a stuck or mis-scaled motor is visible in the log, then stop.
        guard rumbleReports < 4 else { return }
        rumbleReports += 1
        fputs("[xinput] rumble left=\(left) right=\(right)\n", stderr)
    }
}

/// Turns XInput's motor speed into something the paired pad can feel.
///
/// XInput rumble is a *level*, held until the game sends another value, while a
/// Core Haptics player plays a waveform. A continuous event of infinite duration
/// is the honest translation: it runs until the game sends zero. The level is
/// quantised before the player is restarted, because a game that ramps its
/// motors would otherwise build and start a player on every frame.
///
/// One actuator, driven by the stronger of the two motors. XInput separates them
/// and a DualSense can too, through `GCHapticsLocality.leftHandle`/`.rightHandle`
/// with one engine each — worth doing once this has been felt on a device, and
/// deliberately not guessed at now.
///
/// Everything here fails quietly: a pad whose haptics this app cannot reach (an
/// Xbox pad's rumble lives behind HID output reports nothing here sends) must
/// not cost the game its input.
private final class HapticRumble {
    /// Level resolution. 32 steps is under a percent of travel — inaudible as a
    /// step — and turns a full-speed ramp into at most 31 player restarts.
    private static let stepCount = 32

    private weak var controller: GCController?
    private var engine: CHHapticEngine?
    /// `CHHapticEngine` has no `isRunning`, so the state is tracked here. Only
    /// this class starts and stops the engine, so the flag is accurate; it is
    /// cleared whenever a call throws or `stop()` runs, so the next frame tries
    /// a start again rather than playing into a stopped engine forever.
    private var engineStarted = false
    private var player: CHHapticPatternPlayer?
    /// The level the running player was started with, in steps.
    private var steps = -1
    private var reportedFailure = false

    func set(level: UInt16) {
        let wanted = Int(level) * Self.stepCount / 65535
        guard wanted != steps else { return }

        guard wanted > 0 else {
            stop()
            // Not -1: the level is still zero, and a game that keeps sending
            // zero must not rebuild a player it just had torn down.
            steps = 0
            return
        }
        guard let engine = engineForCurrentPad() else { return }

        do {
            try player?.stop(atTime: CHHapticTimeImmediate)
            if !engineStarted {
                try engine.start()
                engineStarted = true
            }
            let event = CHHapticEvent(
                eventType: .hapticContinuous,
                parameters: [
                    CHHapticEventParameter(parameterID: .hapticIntensity,
                                           value: Float(wanted) / Float(Self.stepCount)),
                    CHHapticEventParameter(parameterID: .hapticSharpness, value: 0.5),
                ],
                relativeTime: 0,
                duration: TimeInterval(GCHapticDurationInfinite))
            let pattern = try CHHapticPattern(events: [event], parameters: [])
            player = try engine.makePlayer(with: pattern)
            try player?.start(atTime: CHHapticTimeImmediate)
            steps = wanted
        } catch {
            reportOnce("rumble failed: \(error.localizedDescription)")
            engineStarted = false
            steps = 0
        }
    }

    func stop() {
        try? player?.stop(atTime: CHHapticTimeImmediate)
        engine?.stop(completionHandler: nil)
        engineStarted = false
        player = nil
        engine = nil
        controller = nil
        steps = -1
    }

    /// One engine per pad, because a `CHHapticEngine` belongs to the controller
    /// it came from and must not outlive it.
    private func engineForCurrentPad() -> CHHapticEngine? {
        guard let pad = GCController.controllers().first(where: { $0.haptics != nil }) else {
            reportOnce("no paired controller with haptics")
            return nil
        }
        if pad !== controller || engine == nil {
            stop()
            controller = pad
            guard let haptics = pad.haptics else { return nil }
            let locality = haptics.supportedLocalities.contains(.default)
                ? GCHapticsLocality.default
                : haptics.supportedLocalities.first
            guard let locality else {
                reportOnce("controller reports no haptics locality")
                return nil
            }
            engine = haptics.createEngine(withLocality: locality)
            if engine == nil { reportOnce("createEngine returned nil") }
        }
        return engine
    }

    /// Once, not per frame: this is reachable sixty times a second.
    private func reportOnce(_ reason: String) {
        guard !reportedFailure else { return }
        reportedFailure = true
        fputs("[xinput] \(reason) — rumble reported only\n", stderr)
    }
}
