#include "MayaFlux/MayaFlux.hpp"

#include "MayaFlux/Journal/Archivist.hpp"

#include "Descriptors.hpp"
#include "Gamepads.hpp"

namespace {

using MayaFlux::Core::HIDReportLayout;
using MayaFlux::Core::InputValue;
using namespace MayaFlux::Nodes::Input;

int g_failures = 0;
int g_checks = 0;

void check(bool ok, std::string_view what)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
    }
    MF_PRINT("{}  {}", ok ? "PASS" : "FAIL", what);
}

void check_near(double got, double want, std::string_view what)
{
    ++g_checks;
    const bool ok = std::abs(got - want) <= 0.01;
    if (!ok) {
        ++g_failures;
    }
    MF_PRINT("{}  {}  (got {:.4f}, want {:.4f})", ok ? "PASS" : "FAIL", what, got, want);
}

/** A device as HIDBackend holds one: its parsed layout and the raw value of every field, kept across reports. */
struct Device {
    std::shared_ptr<const HIDReportLayout> layout;
    std::vector<int32_t> raw;
    std::shared_ptr<HIDNode> node;

    Device(std::span<const uint8_t> descriptor, const GamepadLayout& pad)
        : layout(std::make_shared<HIDReportLayout>(MayaFlux::Core::parse_hid_descriptor(descriptor)))
        , raw(layout->fields.size(), 0)
        , node(std::make_shared<HIDNode>(pad.config()))
    {
    }

    /** What HIDBackend::decode_hid_report does with one report, then the node's input path. */
    void send(std::vector<uint8_t> report)
    {
        uint8_t id = 0;
        std::span<const uint8_t> body = report;
        if (layout->uses_report_ids) {
            id = report[0];
            body = body.subspan(1);
        }
        for (size_t i = 0; i < layout->fields.size(); ++i) {
            if (layout->fields[i].report_id == id) {
                raw[i] = MayaFlux::Core::read_hid_field(body, layout->fields[i]);
            }
        }
        node->process_input(InputValue::make_hid(layout, raw, std::move(report), 1));
    }

    [[nodiscard]] double value(HIDControl control) const { return node->value(control); }

    [[nodiscard]] bool has(HIDControl control) const
    {
        if (control.usage_page == 0) {
            return true;
        }
        const auto controls = hid_controls(*layout);
        return std::ranges::any_of(controls, [&](const HIDControlInfo& info) { return info.control == control; });
    }
};

struct Named {
    std::string_view name;
    HIDControl control;
};

std::vector<Named> buttons(const GamepadLayout& pad)
{
    return {
        { "south", pad.south }, { "east", pad.east }, { "west", pad.west }, { "north", pad.north },
        { "left_shoulder", pad.left_shoulder }, { "right_shoulder", pad.right_shoulder },
        { "select", pad.select }, { "start", pad.start }, { "home", pad.home },
        { "left_stick_press", pad.left_stick_press }, { "right_stick_press", pad.right_stick_press },
        { "dpad_up", GamepadLayout::dpad_up }, { "dpad_right", GamepadLayout::dpad_right },
        { "dpad_down", GamepadLayout::dpad_down }, { "dpad_left", GamepadLayout::dpad_left },
    };
}

/** Every field control of a layout exists on the device, except the ones a pad is known to lack. */
void check_controls(const Device& device, const GamepadLayout& pad, std::string_view label,
    std::initializer_list<std::string_view> absent = {})
{
    auto all = buttons(pad);
    all.insert(all.end(), {
                              { "left_trigger", pad.left_trigger }, { "right_trigger", pad.right_trigger },
                              { "left_x", pad.left_x }, { "left_y", pad.left_y },
                              { "right_x", pad.right_x }, { "right_y", pad.right_y }, { "dpad", pad.dpad },
                          });
    for (const auto& [name, control] : all) {
        const bool expect = std::ranges::find(absent, name) == absent.end();
        check(device.has(control) == expect,
            std::format("{}: {} {}", label, name, expect ? "is on the device" : "is absent, as expected"));
    }
}

/** Exactly the named buttons read 1 and every other button of the layout reads 0. */
void check_pressed(const Device& device, const GamepadLayout& pad, std::string_view label,
    std::initializer_list<std::string_view> pressed)
{
    std::string wrong;
    for (const auto& [name, control] : buttons(pad)) {
        const double want = std::ranges::find(pressed, name) != pressed.end() ? 1.0 : 0.0;
        if (device.value(control) != want) {
            wrong += std::format(" {}={}", name, device.value(control));
        }
    }
    check(wrong.empty(), std::format("{}{}", label, wrong.empty() ? "" : " | wrong:" + wrong));
}

// ---------------------------------------------------------------------------
// Sony. Byte layouts from Linux drivers/hid/hid-playstation.c: struct
// dualshock4_input_report_usb and _bt, the 10-byte Bluetooth report, struct
// dualsense_input_report at data[1] (USB) or data[2] (Bluetooth), DS_BUTTONS*.
// ---------------------------------------------------------------------------

struct SonyState {
    uint8_t lx { 0x80 }, ly { 0x80 }, rx { 0x80 }, ry { 0x80 };
    uint8_t l2 {}, r2 {};
    uint8_t hat { 8 }; ///< 0 up, clockwise; 8 none
    uint8_t buttons0 {}; ///< SQUARE 0x10, CROSS 0x20, CIRCLE 0x40, TRIANGLE 0x80
    uint8_t buttons1 {}; ///< L1 0x01, R1 0x02, L2 0x04, R2 0x08, SHARE/CREATE 0x10, OPTIONS 0x20, L3 0x40, R3 0x80
    uint8_t buttons2 {}; ///< PS 0x01, TOUCHPAD 0x02, MIC_MUTE 0x04 (DualSense)
};

struct SonyFormat {
    std::string_view name;
    uint8_t id;
    size_t size;
    size_t block;
    bool dualsense;
};

std::vector<uint8_t> sony_report(const SonyFormat& f, const SonyState& s)
{
    std::vector<uint8_t> r(f.size, 0);
    const size_t b = f.block;
    r[0] = f.id;
    r[b + 0] = s.lx;
    r[b + 1] = s.ly;
    r[b + 2] = s.rx;
    r[b + 3] = s.ry;
    const size_t buttons = f.dualsense ? b + 7 : b + 4;
    r[buttons] = static_cast<uint8_t>((s.hat & 0x0F) | s.buttons0);
    r[buttons + 1] = s.buttons1;
    if (f.dualsense) {
        r[b + 4] = s.l2;
        r[b + 5] = s.r2;
        r[b + 6] = 0x5C; // sequence number
        r[buttons + 2] = s.buttons2;
    } else {
        r[buttons + 2] = static_cast<uint8_t>(s.buttons2 | (0x2A << 2)); // upper six bits count frames
        r[b + 7] = s.l2;
        r[b + 8] = s.r2;
    }
    return r;
}

void sony_tests(std::span<const uint8_t> descriptor, const GamepadLayout& pad, const SonyFormat& format)
{
    const auto label = std::format("{} {}", pad.name, format.name);
    const auto report = [&format](const SonyState& state) { return sony_report(format, state); };
    MF_PRINT("\n--- {} ---", label);
    Device device(descriptor, pad);

    device.send(report({}));
    check_pressed(device, pad, std::format("{} at rest: nothing pressed", label), {});
    check_near(device.value(pad.left_x), 0.0, std::format("{} at rest: left_x", label));
    check_near(device.value(pad.right_y), 0.0, std::format("{} at rest: right_y", label));
    check_near(device.value(pad.left_trigger), 0.0, std::format("{} at rest: left_trigger", label));

    const std::array<std::pair<SonyState, std::string_view>, 12> presses { {
        { { .buttons0 = 0x20 }, "south" }, // cross
        { { .buttons0 = 0x40 }, "east" }, // circle
        { { .buttons0 = 0x10 }, "west" }, // square
        { { .buttons0 = 0x80 }, "north" }, // triangle
        { { .buttons1 = 0x01 }, "left_shoulder" },
        { { .buttons1 = 0x02 }, "right_shoulder" },
        { { .buttons1 = 0x10 }, "select" }, // share / create
        { { .buttons1 = 0x20 }, "start" }, // options
        { { .buttons1 = 0x40 }, "left_stick_press" },
        { { .buttons1 = 0x80 }, "right_stick_press" },
        { { .buttons2 = 0x01 }, "home" }, // PS
        { { .hat = 0 }, "dpad_up" },
    } };
    for (const auto& [state, name] : presses) {
        device.send(report(state));
        check_pressed(device, pad, std::format("{}: {} alone", label, name), { name });
    }

    device.send(report({ .hat = 1 }));
    check_pressed(device, pad, std::format("{}: hat up-right gives up and right", label), { "dpad_up", "dpad_right" });
    device.send(report({ .hat = 4 }));
    check_pressed(device, pad, std::format("{}: hat down", label), { "dpad_down" });
    device.send(report({ .hat = 6 }));
    check_pressed(device, pad, std::format("{}: hat left", label), { "dpad_left" });

    device.send(report({ .buttons2 = 0x02 }));
    check(device.value(Gamepads::DualShock4::touchpad) == 1.0, std::format("{}: touchpad click", label));
    if (format.dualsense) {
        device.send(report({ .buttons2 = 0x04 }));
        check(device.value(Gamepads::DualSense::mute) == 1.0, std::format("{}: mute", label));
    } else {
        check(device.value(Gamepads::DualSense::mute) == 0.0, std::format("{}: no mute, and the frame counter doesn't read as one", label));
    }

    device.send(report({ .lx = 0x00, .ly = 0x00 }));
    check_near(device.value(pad.left_x), -0.7071, std::format("{}: left stick up-left, x", label));
    check_near(device.value(pad.left_y), 0.7071, std::format("{}: left stick up-left, y is positive up", label));
    device.send(report({ .rx = 0xFF }));
    check_near(device.value(pad.right_x), 1.0, std::format("{}: right stick full right", label));
    device.send(report({ .ry = 0xFF }));
    check_near(device.value(pad.right_y), -1.0, std::format("{}: right stick full down", label));
    device.send(report({ .lx = 0x88 }));
    check_near(device.value(pad.left_x), 0.0, std::format("{}: small left stick move stays in the deadzone", label));

    device.send(report({ .l2 = 0xFF, .r2 = 0x80, .buttons1 = 0x0C }));
    check_near(device.value(pad.left_trigger), 1.0, std::format("{}: left trigger full", label));
    check_near(device.value(pad.right_trigger), 128.0 / 255.0, std::format("{}: right trigger half", label));
    check_pressed(device, pad, std::format("{}: the digital L2 and R2 bits press no layout button", label), {});

    std::vector<uint8_t> other(10, 0xFF);
    other[0] = 0x05;
    device.send(other);
    check_pressed(device, pad, std::format("{}: a report of another id reads as rest", label), {});
}

// ---------------------------------------------------------------------------
// Nintendo. Byte layout from Linux drivers/hid/hid-nintendo.c, struct
// joycon_input_report and JC_BTN_*; 12-bit sticks as hid_field_extract reads
// them. One sample report is from dekuNukem's Nintendo_Switch_Reverse_Engineering
// bluetooth_hid_notes.md.
// ---------------------------------------------------------------------------

struct SwitchState {
    uint32_t buttons {}; ///< JC_BTN_* bits
    uint16_t lx { 2000 }, ly { 2000 }, rx { 2000 }, ry { 2000 };
};

std::vector<uint8_t> switch_report(const SwitchState& s, uint8_t id = 0x30)
{
    std::vector<uint8_t> r(49, 0);
    const auto stick = [&r](size_t at, uint16_t x, uint16_t y) {
        r[at] = static_cast<uint8_t>(x & 0xFF);
        r[at + 1] = static_cast<uint8_t>(((x >> 8) & 0x0F) | ((y & 0x0F) << 4));
        r[at + 2] = static_cast<uint8_t>(y >> 4);
    };
    r[0] = id;
    r[1] = 0x5A; // timer
    r[2] = 0x8E; // battery and connection
    r[3] = static_cast<uint8_t>(s.buttons & 0xFF);
    r[4] = static_cast<uint8_t>((s.buttons >> 8) & 0xFF);
    r[5] = static_cast<uint8_t>((s.buttons >> 16) & 0xFF);
    stick(6, s.lx, s.ly);
    stick(9, s.rx, s.ry);
    r[12] = 0x70; // vibrator report
    return r;
}

void switch_tests()
{
    const auto& pad = Gamepads::switch_pro;
    constexpr std::string_view label = "Switch Pro";
    MF_PRINT("\n--- {} ---", label);
    Device device(Fixtures::switch_pro_bt, pad);

    device.send(switch_report({}));
    check_pressed(device, pad, "Switch at rest: nothing pressed", {});
    check_near(device.value(pad.left_x), 0.0, "Switch at rest: left_x at the default centre");

    const std::array<std::pair<unsigned, std::string_view>, 15> presses { {
        { 0, "west" }, // Y
        { 1, "north" }, // X
        { 2, "south" }, // B
        { 3, "east" }, // A
        { 6, "right_shoulder" }, // R
        { 8, "select" }, // minus
        { 9, "start" }, // plus
        { 10, "right_stick_press" },
        { 11, "left_stick_press" },
        { 12, "home" },
        { 16, "dpad_down" },
        { 17, "dpad_up" },
        { 18, "dpad_right" },
        { 19, "dpad_left" },
        { 22, "left_shoulder" }, // L
    } };
    for (const auto& [bit, name] : presses) {
        device.send(switch_report({ .buttons = 1U << bit }));
        check_pressed(device, pad, std::format("{}: JC_BTN bit {} is {}", label, bit, name), { name });
    }

    device.send(switch_report({ .buttons = (1U << 7) | (1U << 23) }));
    check_near(device.value(pad.right_trigger), 1.0, "Switch: ZR is the right trigger");
    check_near(device.value(pad.left_trigger), 1.0, "Switch: ZL is the left trigger");
    check_pressed(device, pad, "Switch: ZL and ZR press no layout button", {});

    const std::array<std::pair<unsigned, HIDControl>, 5> extras { {
        { 13, Gamepads::Switch::capture },
        { 4, Gamepads::Switch::right_sr },
        { 5, Gamepads::Switch::right_sl },
        { 20, Gamepads::Switch::left_sr },
        { 21, Gamepads::Switch::left_sl },
    } };
    for (const auto& [bit, control] : extras) {
        device.send(switch_report({ .buttons = 1U << bit }));
        check(device.value(control) == 1.0, std::format("Switch: JC_BTN bit {} reads on its own control", bit));
    }

    device.send(switch_report({ .lx = 3500 }));
    check_near(device.value(pad.left_x), 1.0, "Switch: left stick full right");
    device.send(switch_report({ .ly = 3500 }));
    check_near(device.value(pad.left_y), 1.0, "Switch: left stick full up is positive");
    device.send(switch_report({ .rx = 500, .ry = 500 }));
    check_near(device.value(pad.right_x), -0.7071, "Switch: right stick down-left, x");
    check_near(device.value(pad.right_y), -0.7071, "Switch: right stick down-left, y");
    device.send(switch_report({ .lx = 2100 }));
    check_near(device.value(pad.left_x), 0.0, "Switch: small move stays in the deadzone");
    device.send(switch_report({ .lx = 4095 }));
    check_near(device.value(pad.left_x), 1.0, "Switch: beyond the default range clamps to 1");

    auto sample = switch_report({}, 0x21);
    sample[3] = 0x41;
    sample[4] = 0x00;
    sample[5] = 0x82;
    device.send(sample);
    check_pressed(device, pad, "Switch: the notes' 0x21 sample (41 00 82) is Y, R and up", { "west", "right_shoulder", "dpad_up" });
    check_near(device.value(pad.left_trigger), 1.0, "Switch: and ZL");

    std::vector<uint8_t> usb_reply { 0x81, 0x01, 0x00, 0x03, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    device.send(usb_reply);
    check_pressed(device, pad, "Switch: a USB 0x81 reply reads as rest", {});
}

// ---------------------------------------------------------------------------
// Android layout. Report 1 of the Xbox pads: X, Y, Z, Rz as 16 bits each,
// brake and accelerator as 10 of 16 bits, the hat in four bits (1 up,
// clockwise, 0 none), 15 buttons, then one Consumer bit, as their
// descriptors declare. The three captures are btmon notifications from a real
// Series X|S pad, from xpadneo docs/descriptors/xbxs.md.
// ---------------------------------------------------------------------------

struct XboxState {
    uint16_t lx { 0x8000 }, ly { 0x8000 }, rx { 0x8000 }, ry { 0x8000 };
    uint16_t brake {}, accelerator {};
    uint8_t hat {};
    uint16_t buttons {}; ///< bit n is button n + 1
    uint8_t consumer {}; ///< bit 0: Share on Series and Elite 2, AC Back on One S
};

std::vector<uint8_t> xbox_report(const XboxState& s)
{
    std::vector<uint8_t> r(64, 0);
    const auto put16 = [&r](size_t at, uint16_t v) {
        r[at] = static_cast<uint8_t>(v & 0xFF);
        r[at + 1] = static_cast<uint8_t>(v >> 8);
    };
    r[0] = 0x01;
    put16(1, s.lx);
    put16(3, s.ly);
    put16(5, s.rx);
    put16(7, s.ry);
    put16(9, s.brake);
    put16(11, s.accelerator);
    r[13] = s.hat;
    put16(14, s.buttons);
    r[16] = s.consumer;
    return r;
}

std::vector<uint8_t> btmon_capture(std::string_view hex)
{
    std::vector<uint8_t> r { 0x01 };
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        r.push_back(static_cast<uint8_t>(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16)));
    }
    r.resize(64, 0);
    return r;
}

uint16_t button_bit(int button) { return static_cast<uint16_t>(1U << (button - 1)); }

void xbox_tests(std::string_view label, std::span<const uint8_t> descriptor, bool consumer_is_share)
{
    const auto& pad = Gamepads::android;
    MF_PRINT("\n--- {} ---", label);
    Device device(descriptor, pad);
    check_controls(device, pad, label);

    device.send(xbox_report({}));
    check_pressed(device, pad, std::format("{} at rest: nothing pressed", label), {});
    check(!device.node->hat(pad.dpad).has_value(), std::format("{} at rest: hat reports no direction", label));

    const std::array<std::pair<int, std::string_view>, 10> presses { {
        { 1, "south" }, { 2, "east" }, { 4, "west" }, { 5, "north" },
        { 7, "left_shoulder" }, { 8, "right_shoulder" }, { 12, "start" },
        { 13, "home" }, { 14, "left_stick_press" }, { 15, "right_stick_press" },
    } };
    for (const auto& [button, name] : presses) {
        device.send(xbox_report({ .buttons = button_bit(button) }));
        check_pressed(device, pad, std::format("{}: button {} is {}", label, button, name), { name });
    }
    device.send(xbox_report({ .buttons = button_bit(11) }));
    check_pressed(device, pad, std::format("{}: button 11 (View) is select", label), { "select" });

    device.send(xbox_report({ .consumer = 0x01 }));
    if (consumer_is_share) {
        check_pressed(device, pad, std::format("{}: Share presses no layout button", label), {});
        check(device.value(Gamepads::Xbox::share) == 1.0, std::format("{}: Xbox::share reads the Consumer bit", label));
    } else {
        check_pressed(device, pad, std::format("{}: AC Back is select", label), { "select" });
    }

    device.send(xbox_report({ .hat = 1 }));
    check_pressed(device, pad, std::format("{}: hat 1 is up", label), { "dpad_up" });
    check(device.node->hat(pad.dpad) == 0, std::format("{}: hat() reads 0 for up", label));
    device.send(xbox_report({ .hat = 4 }));
    check_pressed(device, pad, std::format("{}: hat 4 is down-right", label), { "dpad_down", "dpad_right" });

    device.send(xbox_report({ .lx = 0xFFFF, .ly = 0x0000 }));
    check_near(device.value(pad.left_x), 0.7071, std::format("{}: left stick up-right, x", label));
    check_near(device.value(pad.left_y), 0.7071, std::format("{}: left stick up-right, y is positive up", label));
    device.send(xbox_report({ .ry = 0xFFFF }));
    check_near(device.value(pad.right_y), -1.0, std::format("{}: right stick full down", label));
    device.send(xbox_report({ .brake = 1023, .accelerator = 256 }));
    check_near(device.value(pad.left_trigger), 1.0, std::format("{}: brake is the left trigger, full", label));
    check_near(device.value(pad.right_trigger), 256.0 / 1023.0, std::format("{}: accelerator is the right trigger", label));
}

void xbox_capture_tests()
{
    const auto& pad = Gamepads::android;
    MF_PRINT("\n--- Xbox Series X|S, real captures ---");
    Device device(Fixtures::xbox_series_ble, pad);

    device.send(btmon_capture("eb7e5f80eb7f85800000000000000400"));
    check_pressed(device, pad, "capture 'back': select", { "select" });
    check_near(device.value(pad.left_x), 0.0, "capture 'back': resting left_x is in the deadzone");

    device.send(btmon_capture("eb7e5f80eb7f85800000000000000800"));
    check_pressed(device, pad, "capture 'select' (the Menu button, 12): start", { "start" });

    device.send(btmon_capture("eb7e5f80eb7f85800000000000000001"));
    check_pressed(device, pad, "capture 'share': no layout button", {});
    check(device.value(Gamepads::Xbox::share) == 1.0, "capture 'share': Xbox::share");
}

void one_s_home_test()
{
    const auto& pad = Gamepads::android;
    MF_PRINT("\n--- Xbox One S, Xbox button on report 2 (AC Home) ---");
    Device device(Fixtures::xbox_one_s_bt, pad);
    device.send(xbox_report({}));
    device.send({ 0x02, 0x01 });
    check_pressed(device, pad, "One S: AC Home is home", { "home" });
}

// ---------------------------------------------------------------------------
// Nexus Player gamepad (Linux HID selftests, AsusGamepad). Report 1: buttons
// 1, 2, 4, 5, 7, 8, 14, 15, 13, AC Back, AC Home, one pad bit, the hat (0 up),
// then X, Y, Z, Rz, brake, accelerator as bytes, in descriptor order.
// ---------------------------------------------------------------------------

std::vector<uint8_t> nexus_report(uint16_t bits, uint8_t hat, std::array<uint8_t, 6> axes)
{
    std::vector<uint8_t> r(9, 0);
    r[0] = 0x01;
    const auto first = static_cast<uint16_t>(bits | (hat << 12));
    r[1] = static_cast<uint8_t>(first & 0xFF);
    r[2] = static_cast<uint8_t>(first >> 8);
    std::ranges::copy(axes, r.begin() + 3);
    return r;
}

void nexus_tests()
{
    const auto& pad = Gamepads::android;
    constexpr std::string_view label = "Nexus Player gamepad";
    constexpr std::array<uint8_t, 6> centre { 0x80, 0x80, 0x80, 0x80, 0x00, 0x00 };
    MF_PRINT("\n--- {} ---", label);
    Device device(Fixtures::nexus_player, pad);
    check_controls(device, pad, label, { "start" });

    device.send(nexus_report(0, 8, centre));
    check_pressed(device, pad, std::format("{} at rest: nothing pressed", label), {});

    const std::array<std::pair<int, std::string_view>, 11> bits { {
        { 0, "south" }, { 1, "east" }, { 2, "west" }, { 3, "north" },
        { 4, "left_shoulder" }, { 5, "right_shoulder" },
        { 6, "left_stick_press" }, { 7, "right_stick_press" },
        { 8, "home" }, { 9, "select" }, { 10, "home" },
    } };
    for (const auto& [bit, name] : bits) {
        device.send(nexus_report(static_cast<uint16_t>(1U << bit), 8, centre));
        check_pressed(device, pad, std::format("{}: report bit {} is {}", label, bit, name), { name });
    }

    device.send(nexus_report(0, 2, { 0x00, 0xFF, 0x80, 0x80, 0xFF, 0x00 }));
    check_pressed(device, pad, std::format("{}: hat right", label), { "dpad_right" });
    check_near(device.value(pad.left_x), -0.7071, std::format("{}: left stick down-left, x", label));
    check_near(device.value(pad.left_y), -0.7071, std::format("{}: left stick down-left, y", label));
    check_near(device.value(pad.left_trigger), 1.0, std::format("{}: brake is the left trigger", label));
}

void find_tests()
{
    MF_PRINT("\n--- find ---");
    check(Gamepads::find(0x054C, 0x09CC) == &Gamepads::dualshock4, "054C:09CC is a DualShock 4");
    check(Gamepads::find(0x054C, 0x0DF2) == &Gamepads::dualsense, "054C:0DF2 is a DualSense");
    check(Gamepads::find(0x057E, 0x2009) == &Gamepads::switch_pro, "057E:2009 is a Switch Pro Controller");
    check(Gamepads::find(0x057E, 0x2006) == &Gamepads::switch_pro, "057E:2006 (Joy-Con L) reads through the Switch layout");
    check(Gamepads::find(0x045E, 0x0B13) == &Gamepads::android, "045E:0B13 uses the Android layout");
    check(Gamepads::find(0x045E, 0x028E) == nullptr, "045E:028E (Xbox 360, not HID) is not listed");
}

} // namespace

int main()
{
    constexpr SonyFormat ds4_usb { "USB (0x01)", 0x01, 64, 1, false };
    constexpr SonyFormat ds4_bt { "Bluetooth (0x11)", 0x11, 78, 3, false };
    constexpr SonyFormat ds4_bt_short { "Bluetooth, short report (0x01)", 0x01, 10, 1, false };
    constexpr SonyFormat dualsense_usb { "USB (0x01)", 0x01, 64, 1, true };
    constexpr SonyFormat dualsense_bt { "Bluetooth (0x31)", 0x31, 78, 2, true };

    sony_tests(Fixtures::ds4_usb, Gamepads::dualshock4, ds4_usb);
    sony_tests(Fixtures::ds4_bt, Gamepads::dualshock4, ds4_bt);
    sony_tests(Fixtures::ds4_bt, Gamepads::dualshock4, ds4_bt_short);
    sony_tests(Fixtures::dualsense_usb, Gamepads::dualsense, dualsense_usb);
    sony_tests(Fixtures::dualsense_bt, Gamepads::dualsense, dualsense_bt);
    switch_tests();
    xbox_tests("Xbox Series X|S", Fixtures::xbox_series_ble, true);
    xbox_tests("Xbox Elite Series 2", Fixtures::xbox_elite2_ble, true);
    xbox_tests("Xbox One S", Fixtures::xbox_one_s_bt, false);
    xbox_capture_tests();
    one_s_home_test();
    nexus_tests();
    find_tests();

    MF_PRINT("\n{} of {} checks passed: {}", g_checks - g_failures, g_checks, g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
