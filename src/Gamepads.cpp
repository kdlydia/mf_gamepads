#include "Gamepads.hpp"

namespace MayaFlux::Nodes::Input {

namespace {

    constexpr HIDControl k_no_hat = HIDControl::derived(0xFDFF);
    constexpr HIDControl k_android_select = HIDControl::derived(0xFD04);
    constexpr HIDControl k_android_home = HIDControl::derived(0xFD05);

    constexpr HIDControl k_consumer_back = { 0x0C, 0x224, 0 };
    constexpr HIDControl k_consumer_home = { 0x0C, 0x223, 0 };
    constexpr HIDControl k_brake = { 0x02, 0xC5, 0 };
    constexpr HIDControl k_accelerator = { 0x02, 0xC4, 0 };

    /** Where the input block of a report starts, or nothing for a report the pad's layout doesn't cover. */
    using Locate = std::optional<size_t> (*)(std::span<const uint8_t> report);

    /** True when a hat direction (0 up, clockwise to 7) points toward another or beside it. */
    bool points_toward(int32_t direction, int32_t toward)
    {
        const int32_t offset = (direction - toward + 8) % 8;
        return offset == 0 || offset == 1 || offset == 7;
    }

    /** Radial deadzone over a stick pair, as HIDNode applies it to field sticks. */
    std::pair<double, double> radial(double x, double y, double deadzone)
    {
        const double magnitude = std::hypot(x, y);
        if (magnitude <= deadzone || deadzone >= 1.0) {
            return { 0.0, 0.0 };
        }
        const double scale = (std::min(magnitude, 1.0) - deadzone) / (1.0 - deadzone) / magnitude;
        return { x * scale, y * scale };
    }

    /** The byte at an offset into a report's input block, if the report has one and is long enough. */
    std::optional<uint8_t> byte_at(const HIDSnapshot& snapshot, Locate locate, size_t offset)
    {
        const auto block = locate(snapshot.report);
        if (!block || *block + offset >= snapshot.report.size()) {
            return std::nullopt;
        }
        return snapshot.report[*block + offset];
    }

    // -------------------------------------------------------------------------
    // Sony. The input block is struct dualshock4_input_report_common or struct
    // dualsense_input_report in the Linux hid-playstation driver: four stick
    // bytes, then buttons and triggers at offsets that differ by pad. The
    // button bits are its DS_BUTTONS0/1/2 masks.
    // -------------------------------------------------------------------------

    std::optional<size_t> dualshock4_block(std::span<const uint8_t> report)
    {
        if (report.size() >= 10 && report[0] == 0x01) {
            return 1; // USB, and the 10-byte Bluetooth report
        }
        if (report.size() >= 12 && report[0] == 0x11) {
            return 3; // Bluetooth
        }
        return std::nullopt;
    }

    std::optional<size_t> dualsense_block(std::span<const uint8_t> report)
    {
        if (report.size() >= 12 && report[0] == 0x01) {
            return 1; // USB
        }
        if (report.size() >= 13 && report[0] == 0x31) {
            return 2; // Bluetooth
        }
        return std::nullopt;
    }

    struct SonyOffsets {
        size_t buttons;
        size_t left_trigger;
        size_t right_trigger;
        bool has_mute;
    };

    HIDDerivation sony_bit(Locate locate, size_t offset, uint8_t mask)
    {
        return [=](const HIDSnapshot& snapshot) {
            const auto value = byte_at(snapshot, locate, offset);
            return value && (*value & mask) ? 1.0 : 0.0;
        };
    }

    HIDDerivation sony_trigger(Locate locate, size_t offset)
    {
        return [=](const HIDSnapshot& snapshot) {
            return byte_at(snapshot, locate, offset).value_or(0) / 255.0;
        };
    }

    HIDDerivation sony_stick(Locate locate, size_t offset, bool want_y, double deadzone)
    {
        return [=](const HIDSnapshot& snapshot) {
            const auto raw_x = byte_at(snapshot, locate, offset);
            const auto raw_y = byte_at(snapshot, locate, offset + 1);
            if (!raw_x || !raw_y) {
                return 0.0;
            }
            const auto [x, y] = radial(*raw_x / 255.0 * 2.0 - 1.0, 1.0 - *raw_y / 255.0 * 2.0, deadzone);
            return want_y ? y : x;
        };
    }

    HIDDerivation sony_dpad(Locate locate, size_t offset, int32_t toward)
    {
        return [=](const HIDSnapshot& snapshot) {
            const auto value = byte_at(snapshot, locate, offset);
            const int32_t direction = value ? *value & 0x0F : 8;
            return direction < 8 && points_toward(direction, toward) ? 1.0 : 0.0;
        };
    }

    HIDConfig sony_config(const GamepadLayout& pad, double deadzone, Locate locate, SonyOffsets at)
    {
        const size_t b = at.buttons;
        auto config = HIDConfig::device()
                          .derive(pad.west.usage, sony_bit(locate, b, 0x10))
                          .derive(pad.south.usage, sony_bit(locate, b, 0x20))
                          .derive(pad.east.usage, sony_bit(locate, b, 0x40))
                          .derive(pad.north.usage, sony_bit(locate, b, 0x80))
                          .derive(pad.left_shoulder.usage, sony_bit(locate, b + 1, 0x01))
                          .derive(pad.right_shoulder.usage, sony_bit(locate, b + 1, 0x02))
                          .derive(pad.select.usage, sony_bit(locate, b + 1, 0x10))
                          .derive(pad.start.usage, sony_bit(locate, b + 1, 0x20))
                          .derive(pad.left_stick_press.usage, sony_bit(locate, b + 1, 0x40))
                          .derive(pad.right_stick_press.usage, sony_bit(locate, b + 1, 0x80))
                          .derive(pad.home.usage, sony_bit(locate, b + 2, 0x01))
                          .derive(Gamepads::DualSense::touchpad.usage, sony_bit(locate, b + 2, 0x02))
                          .derive(pad.left_trigger.usage, sony_trigger(locate, at.left_trigger))
                          .derive(pad.right_trigger.usage, sony_trigger(locate, at.right_trigger))
                          .derive(pad.left_x.usage, sony_stick(locate, 0, false, deadzone))
                          .derive(pad.left_y.usage, sony_stick(locate, 0, true, deadzone))
                          .derive(pad.right_x.usage, sony_stick(locate, 2, false, deadzone))
                          .derive(pad.right_y.usage, sony_stick(locate, 2, true, deadzone))
                          .derive(GamepadLayout::dpad_up.usage, sony_dpad(locate, b, 0))
                          .derive(GamepadLayout::dpad_right.usage, sony_dpad(locate, b, 2))
                          .derive(GamepadLayout::dpad_down.usage, sony_dpad(locate, b, 4))
                          .derive(GamepadLayout::dpad_left.usage, sony_dpad(locate, b, 6));
        // On the DualShock 4 this bit belongs to a frame counter.
        return at.has_mute ? config.derive(Gamepads::DualSense::mute.usage, sony_bit(locate, b + 2, 0x04)) : config;
    }

    HIDConfig dualshock4_config(const GamepadLayout& pad, double deadzone)
    {
        return sony_config(pad, deadzone, &dualshock4_block, { .buttons = 4, .left_trigger = 7, .right_trigger = 8, .has_mute = false });
    }

    HIDConfig dualsense_config(const GamepadLayout& pad, double deadzone)
    {
        return sony_config(pad, deadzone, &dualsense_block, { .buttons = 7, .left_trigger = 4, .right_trigger = 5, .has_mute = true });
    }

    // -------------------------------------------------------------------------
    // Nintendo. The standard input report is struct joycon_input_report in the
    // Linux hid-nintendo driver: 24 button bits from byte 3 (its JC_BTN_*
    // masks), then each stick as two 12-bit values from bytes 6 and 9. Without
    // the pad's own calibration, the driver's default is used.
    // -------------------------------------------------------------------------

    constexpr int32_t k_switch_centre = 2000;
    constexpr int32_t k_switch_min = 500;
    constexpr int32_t k_switch_max = 3500;

    std::optional<size_t> switch_block(std::span<const uint8_t> report)
    {
        if (report.size() >= 12 && (report[0] == 0x21 || report[0] == 0x30 || report[0] == 0x31)) {
            return 0;
        }
        return std::nullopt;
    }

    HIDDerivation switch_bit(uint32_t mask)
    {
        return [=](const HIDSnapshot& snapshot) {
            if (!switch_block(snapshot.report)) {
                return 0.0;
            }
            const auto& r = snapshot.report;
            const uint32_t buttons = r[3] | (r[4] << 8U) | (r[5] << 16U);
            return buttons & mask ? 1.0 : 0.0;
        };
    }

    double switch_axis(int32_t raw)
    {
        const double value = raw > k_switch_centre
            ? static_cast<double>(raw - k_switch_centre) / (k_switch_max - k_switch_centre)
            : -static_cast<double>(k_switch_centre - raw) / (k_switch_centre - k_switch_min);
        return std::clamp(value, -1.0, 1.0);
    }

    HIDDerivation switch_stick(size_t offset, bool want_y, double deadzone)
    {
        return [=](const HIDSnapshot& snapshot) {
            if (!switch_block(snapshot.report)) {
                return 0.0;
            }
            const auto& r = snapshot.report;
            const int32_t raw_x = r[offset] | ((r[offset + 1] & 0x0F) << 8);
            const int32_t raw_y = (r[offset + 1] >> 4) | (r[offset + 2] << 4);
            const auto [x, y] = radial(switch_axis(raw_x), switch_axis(raw_y), deadzone);
            return want_y ? y : x;
        };
    }

    HIDConfig switch_config(const GamepadLayout& pad, double deadzone)
    {
        constexpr auto bit = [](unsigned n) { return 1U << n; };
        return HIDConfig::device()
            .derive(pad.west.usage, switch_bit(bit(0))) // Y
            .derive(pad.north.usage, switch_bit(bit(1))) // X
            .derive(pad.south.usage, switch_bit(bit(2))) // B
            .derive(pad.east.usage, switch_bit(bit(3))) // A
            .derive(Gamepads::Switch::right_sr.usage, switch_bit(bit(4)))
            .derive(Gamepads::Switch::right_sl.usage, switch_bit(bit(5)))
            .derive(pad.right_shoulder.usage, switch_bit(bit(6))) // R
            .derive(pad.right_trigger.usage, switch_bit(bit(7))) // ZR
            .derive(pad.select.usage, switch_bit(bit(8))) // minus
            .derive(pad.start.usage, switch_bit(bit(9))) // plus
            .derive(pad.right_stick_press.usage, switch_bit(bit(10)))
            .derive(pad.left_stick_press.usage, switch_bit(bit(11)))
            .derive(pad.home.usage, switch_bit(bit(12)))
            .derive(Gamepads::Switch::capture.usage, switch_bit(bit(13)))
            .derive(GamepadLayout::dpad_down.usage, switch_bit(bit(16)))
            .derive(GamepadLayout::dpad_up.usage, switch_bit(bit(17)))
            .derive(GamepadLayout::dpad_right.usage, switch_bit(bit(18)))
            .derive(GamepadLayout::dpad_left.usage, switch_bit(bit(19)))
            .derive(Gamepads::Switch::left_sr.usage, switch_bit(bit(20)))
            .derive(Gamepads::Switch::left_sl.usage, switch_bit(bit(21)))
            .derive(pad.left_shoulder.usage, switch_bit(bit(22))) // L
            .derive(pad.left_trigger.usage, switch_bit(bit(23))) // ZL
            .derive(pad.left_x.usage, switch_stick(6, false, deadzone))
            .derive(pad.left_y.usage, switch_stick(6, true, deadzone))
            .derive(pad.right_x.usage, switch_stick(9, false, deadzone))
            .derive(pad.right_y.usage, switch_stick(9, true, deadzone));
    }

    // -------------------------------------------------------------------------
    // Android layout, read from descriptor fields.
    // -------------------------------------------------------------------------

    HIDDerivation hat_field(HIDControl hat, int32_t toward)
    {
        return [hat, toward](const HIDSnapshot& snapshot) {
            const auto index = snapshot.find(hat);
            if (!index || *index >= snapshot.raw.size()) {
                return 0.0;
            }
            const auto& field = snapshot.layout->fields[*index];
            const int32_t raw = snapshot.raw[*index];
            if (raw < field.logical_min || raw > field.logical_max) {
                return 0.0;
            }
            return points_toward(raw - field.logical_min, toward) ? 1.0 : 0.0;
        };
    }

    HIDDerivation either(HIDControl a, HIDControl b)
    {
        return [a, b](const HIDSnapshot& snapshot) {
            return snapshot.value(a).value_or(0.0) > 0.5 || snapshot.value(b).value_or(0.0) > 0.5 ? 1.0 : 0.0;
        };
    }

    HIDConfig android_config(const GamepadLayout& pad, double deadzone)
    {
        return HIDConfig::device()
            .with_spec(pad.left_x, HIDControlSpec::centered())
            .with_spec(pad.left_y, HIDControlSpec::centered().inverted())
            .with_spec(pad.right_x, HIDControlSpec::centered())
            .with_spec(pad.right_y, HIDControlSpec::centered().inverted())
            .with_stick(pad.left_x, pad.left_y, deadzone)
            .with_stick(pad.right_x, pad.right_y, deadzone)
            .derive(GamepadLayout::dpad_up.usage, hat_field(pad.dpad, 0))
            .derive(GamepadLayout::dpad_right.usage, hat_field(pad.dpad, 2))
            .derive(GamepadLayout::dpad_down.usage, hat_field(pad.dpad, 4))
            .derive(GamepadLayout::dpad_left.usage, hat_field(pad.dpad, 6))
            .derive(k_android_select.usage, either(HIDControl::button(11), k_consumer_back))
            .derive(k_android_home.usage, either(HIDControl::button(13), k_consumer_home));
    }

    /** A layout whose controls are all derived from report bytes, numbered from a base id. */
    constexpr GamepadLayout by_bytes(std::string_view name, uint16_t base,
        HIDConfig (*make)(const GamepadLayout&, double))
    {
        const auto at = [base](uint16_t offset) { return HIDControl::derived(static_cast<uint16_t>(base + offset)); };
        return {
            .name = name,
            .south = at(0),
            .east = at(1),
            .west = at(2),
            .north = at(3),
            .left_shoulder = at(4),
            .right_shoulder = at(5),
            .left_trigger = at(6),
            .right_trigger = at(7),
            .select = at(8),
            .start = at(9),
            .home = at(10),
            .left_stick_press = at(11),
            .right_stick_press = at(12),
            .left_x = at(13),
            .left_y = at(14),
            .right_x = at(15),
            .right_y = at(16),
            .dpad = k_no_hat,
            .make = make,
        };
    }

} // namespace

namespace Gamepads {

    const GamepadLayout dualshock4 = by_bytes("DualShock 4", 0xFD10, &dualshock4_config);

    const GamepadLayout dualsense = by_bytes("DualSense", 0xFD10, &dualsense_config);

    const GamepadLayout switch_pro = by_bytes("Switch Pro", 0xFD30, &switch_config);

    const GamepadLayout android = {
        .name = "Android",
        .south = HIDControl::button(1),
        .east = HIDControl::button(2),
        .west = HIDControl::button(4),
        .north = HIDControl::button(5),
        .left_shoulder = HIDControl::button(7),
        .right_shoulder = HIDControl::button(8),
        .left_trigger = k_brake,
        .right_trigger = k_accelerator,
        .select = k_android_select,
        .start = HIDControl::button(12),
        .home = k_android_home,
        .left_stick_press = HIDControl::button(14),
        .right_stick_press = HIDControl::button(15),
        .left_x = HIDControl::x(),
        .left_y = HIDControl::y(),
        .right_x = HIDControl::z(),
        .right_y = HIDControl::rz(),
        .dpad = HIDControl::hat(),
        .make = &android_config,
    };

    const GamepadLayout* find(uint16_t vendor_id, uint16_t product_id)
    {
        struct Known {
            uint16_t vendor;
            uint16_t product;
            const GamepadLayout* layout;
        };

        static const std::array<Known, 13> known { {
            { 0x054C, 0x05C4, &dualshock4 },
            { 0x054C, 0x09CC, &dualshock4 },
            { 0x054C, 0x0BA0, &dualshock4 },
            { 0x054C, 0x0CE6, &dualsense },
            { 0x054C, 0x0DF2, &dualsense },
            { 0x057E, 0x2006, &switch_pro },
            { 0x057E, 0x2007, &switch_pro },
            { 0x057E, 0x2009, &switch_pro },
            { 0x057E, 0x200E, &switch_pro },
            { 0x045E, 0x0B13, &android },
            { 0x045E, 0x0B22, &android },
            { 0x045E, 0x02FD, &android },
            { 0x18D1, 0x2C40, &android },
        } };

        const auto it = std::ranges::find_if(known, [&](const Known& entry) {
            return entry.vendor == vendor_id && entry.product == product_id;
        });
        return it != known.end() ? it->layout : nullptr;
    }

} // namespace Gamepads

} // namespace MayaFlux::Nodes::Input
