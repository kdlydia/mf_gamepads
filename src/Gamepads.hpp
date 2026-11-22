#pragma once

#include "MayaFlux/Nodes/Input/HIDNode.hpp"

namespace MayaFlux::Nodes::Input {

/**
 * @struct GamepadLayout
 * @brief Where the standard controls of one family of gamepads sit.
 *
 * Names are positions, not labels: south is Cross on a PlayStation pad, A on an
 * Xbox pad and B on a Nintendo pad. Read through a HIDNode made with config(),
 * sticks give -1 to 1 with right and up positive, triggers give 0 to 1, and
 * buttons and d-pad directions give 0 or 1. A control a device lacks reads the
 * node's default.
 *
 * Some layouts read descriptor fields and some read the report bytes, as each
 * pad needs, so the same names work on either.
 *
 * @code
 * const auto& pad = Gamepads::dualsense;
 * auto node = vega.read_hid(pad.config(), bind_hid(id));
 * node->on_press(pad.south, [](HIDContext&) { jump(); });
 * double steer = node->value(pad.left_x);
 * @endcode
 */
struct GamepadLayout {
    std::string_view name;

    HIDControl south, east, west, north;
    HIDControl left_shoulder, right_shoulder;
    HIDControl left_trigger, right_trigger;
    HIDControl select, start, home;
    HIDControl left_stick_press, right_stick_press;
    HIDControl left_x, left_y, right_x, right_y;

    /** @brief The hat field, for HIDNode::hat() and on_hat(), where a layout reads one. */
    HIDControl dpad;

    /** @brief Builds the node config for this layout. */
    HIDConfig (*make)(const GamepadLayout& layout, double stick_deadzone) = nullptr;

    /** @brief D-pad directions, 1 while the d-pad points that way or diagonally beside it. */
    static constexpr HIDControl dpad_up = HIDControl::derived(0xFD00);
    static constexpr HIDControl dpad_right = HIDControl::derived(0xFD01);
    static constexpr HIDControl dpad_down = HIDControl::derived(0xFD02);
    static constexpr HIDControl dpad_left = HIDControl::derived(0xFD03);

    /**
     * @brief The device node config.
     * @param stick_deadzone Radial deadzone of both sticks, as a fraction of full travel.
     *
     * Derived ids 0xFD00 to 0xFDFF are taken. Combine with your own config through HIDConfig::with.
     */
    [[nodiscard]] HIDConfig config(double stick_deadzone = 0.1) const { return make(*this, stick_deadzone); }
};

namespace Gamepads {

    /**
     * @brief Sony DualShock 4 (054C:05C4, 054C:09CC, adapter 054C:0BA0), over USB or Bluetooth.
     *
     * Reads report 0x01 (USB, and the short Bluetooth report) and 0x11 (Bluetooth) by byte,
     * as the Linux driver lays them out. dpad is not a field here: use the four directions.
     */
    extern const GamepadLayout dualshock4;

    /** @brief Sony DualSense and DualSense Edge (054C:0CE6, 054C:0DF2), reports 0x01 (USB) and 0x31 (Bluetooth). */
    extern const GamepadLayout dualsense;

    /**
     * @brief Nintendo Switch Pro Controller (057E:2009), Joy-Cons (057E:2006, 057E:2007) and the
     *        charging grip (057E:200E), over USB or Bluetooth.
     *
     * Reads the standard input report (0x30, and 0x21 and 0x31, which share it) once the Linux
     * driver has put the pad in that mode. A Joy-Con on its own reads its half. Sticks use the
     * driver's default calibration, so allow a larger deadzone than for other pads.
     */
    extern const GamepadLayout switch_pro;

    /**
     * @brief The layout Android defines for HID gamepads, read from descriptor fields.
     *
     * Xbox Wireless controllers over Bluetooth use it (045E:0B13, 045E:0B22, 045E:02FD), as
     * does the Nexus Player gamepad (18D1:2C40). Select and home read either the button or
     * the Consumer page control a pad uses for them.
     */
    extern const GamepadLayout android;

    namespace DualShock4 {
        inline constexpr HIDControl touchpad = HIDControl::derived(0xFD21);
    }

    namespace DualSense {
        inline constexpr HIDControl touchpad = HIDControl::derived(0xFD21);
        inline constexpr HIDControl mute = HIDControl::derived(0xFD22);
    }

    namespace Switch {
        inline constexpr HIDControl capture = HIDControl::derived(0xFD41);
        inline constexpr HIDControl right_sl = HIDControl::derived(0xFD42); ///< SL on the right Joy-Con
        inline constexpr HIDControl right_sr = HIDControl::derived(0xFD43);
        inline constexpr HIDControl left_sl = HIDControl::derived(0xFD44); ///< SL on the left Joy-Con
        inline constexpr HIDControl left_sr = HIDControl::derived(0xFD45);
    }

    namespace Xbox {
        inline constexpr HIDControl share = { 0x0C, 0xB2, 0 }; ///< Series X|S and Elite 2
    }

    /** @brief The layout of a known pad, or null. */
    [[nodiscard]] const GamepadLayout* find(uint16_t vendor_id, uint16_t product_id);

} // namespace Gamepads

} // namespace MayaFlux::Nodes::Input
