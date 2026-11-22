# mf_gamepads

Gamepad layouts for MayaFlux's `HIDNode`. Each layout says where the standard controls of a family of pads sit, so `south`, `left_x` or `dpad_up` mean the same control on every pad listed here.

Needs a MayaFlux nightly from 21 Nov 2026 or later (main `0f71ff55`), which reads HID controls by usage.

## Pads

| Layout | Pads | Connection |
|---|---|---|
| `Gamepads::dualshock4` | DualShock 4 (054C:05C4, 054C:09CC), its wireless adapter (054C:0BA0) | USB or Bluetooth |
| `Gamepads::dualsense` | DualSense (054C:0CE6), DualSense Edge (054C:0DF2) | USB or Bluetooth |
| `Gamepads::switch_pro` | Switch Pro Controller (057E:2009), Joy-Con L and R (057E:2006, 057E:2007), charging grip (057E:200E) | USB or Bluetooth |
| `Gamepads::android` | Xbox Series X\|S (045E:0B13), Xbox Elite Series 2 (045E:0B22), Xbox One S (045E:02FD), Nexus Player gamepad (18D1:2C40), and other pads that follow Android's HID layout | Bluetooth for the Xbox pads |

`Gamepads::find(vendor_id, product_id)` returns the layout for the ids above, or null.

## Use

```cpp
#include "Gamepads.hpp"

using namespace MayaFlux::Nodes::Input;

auto device = MayaFlux::choose_input_device(MayaFlux::Core::InputType::HID);
const GamepadLayout* pad = Gamepads::find(device->vendor_id, device->product_id);

auto node = vega.read_hid(pad->config(), device->to_binding());
node->on_press(pad->south, [](HIDContext&) { jump(); });
node->on_press(GamepadLayout::dpad_up, [](HIDContext&) { menu_up(); });

double steer = node->value(pad->left_x);
double brake = node->value(pad->left_trigger);
```

Names are positions, not labels: `south` is Cross on a PlayStation pad, A on an Xbox pad and B on a Nintendo pad.

- Sticks read -1 to 1, with right and up positive, and a radial deadzone (`config(deadzone)`, 0.1 by default).
- Triggers read 0 to 1. The Switch's ZL and ZR are buttons, so they read 0 or 1.
- Buttons and the four d-pad directions read 0 or 1. A diagonal sets both of its directions.
- A control a pad lacks reads the node's default. The Nexus Player gamepad has no `start`. A Joy-Con on its own reads only its half; the charging grip reads both.
- Extra controls are under the pad's name:
  - `Gamepads::DualShock4::touchpad`
  - `Gamepads::DualSense::touchpad` and `mute`
  - `Gamepads::Switch::capture`, plus SL and SR on each Joy-Con
  - `Gamepads::Xbox::share`
- `config()` uses derived ids 0xFD00 to 0xFDFF. Add your own with `HIDConfig::with`, using other ids.

## How each layout reads its pad

`android` reads the fields the pad's descriptor declares.

The Sony and Switch pads send reports their descriptors don't describe. Over Bluetooth the Linux drivers switch the Sony pads to an extended report, and the Switch pads' full report packs the sticks into 12-bit values. So `dualshock4`, `dualsense` and `switch_pro` read the report bytes, laid out as the Linux `hid-playstation` and `hid-nintendo` drivers read them. One layout covers USB and Bluetooth. This means for these three:

- Set the stick deadzone in `config()`. `set_stick_tuning` and `set_tuning` apply only to descriptor fields.
- `dpad` is not a hat field, so `hat()` and `on_hat()` read nothing. Use the four directions.
- The Switch sticks use the driver's default calibration (centre 2000, range 500 to 3500), not each pad's own, so allow a larger deadzone, such as 0.15.
- They rely on the Linux driver having put the pad in its full-report mode. It does this on its own when the pad connects.

## Not covered

- **Xbox pads over USB.** Linux binds them with `xpad`, which isn't HID, so MayaFlux's HID backend never sees them. Use Bluetooth. Reading them needs an evdev backend in MayaFlux.
- **Switch 2 pads** (057E:2066, 2067, 2069), which use a different protocol.
- **DualShock 3**, which needs a vendor request before it reports.
- **xpadneo.** This out-of-tree Xbox driver rewrites some descriptors. With it loaded, check with the probe below.
- **macOS and Windows** have no driver that switches modes. The DualShock 4's short Bluetooth report still reads, but the DualSense over Bluetooth and the Switch pads won't until something sends them the request the Linux drivers send. Untested there either way.
- The Elite 2's paddles and the DualSense Edge's extra buttons.

For any other pad, list what it declares, move it, and write a layout the same way:

```cpp
auto probe = vega.read_hid(HIDConfig::device());
probe->on_frame([done = false](HIDContext& ctx) mutable {
    if (std::exchange(done, true)) return;
    for (const auto& c : hid_controls(*ctx.snapshot->layout))
        MF_PRINT("{:#x} {:#x} {}..{}", c.control.usage_page, c.control.usage, c.logical_min, c.logical_max);
});
```

On Linux the pad's `/dev/hidraw*` node needs read access. MayaFlux's udev file covers tablets only. The `steam-devices` package covers most pads, and when MayaFlux can't open a device it logs the exact rule to add.

## Tests

```sh
cmake -G Ninja -B test/build -S test -DCMAKE_BUILD_TYPE=Release
cmake --build test/build --parallel
./test/build/test_mf_gamepads
```

No pad needs to be connected. Each test parses a descriptor captured from a real pad, builds reports, and feeds them to a `HIDNode` the way MayaFlux's HID backend does. It then checks that every button, stick, trigger and d-pad direction reads as the layout says, in every report format the pad sends.

- **Descriptors** (`test/Descriptors.hpp`):
  - DualShock 4 and DualSense over USB and Bluetooth, from hid-tools
  - the Pro Controller, from joycontrol's Bluetooth SDP record
  - the three Xbox pads, from xpadneo's dumps
  - the Nexus Player gamepad, from the Linux kernel's HID selftests
- **Reports:**
  - Sony and Switch reports follow the report structs and button masks in the Linux `hid-playstation` and `hid-nintendo` drivers.
  - One Switch report is the sample in dekuNukem's Switch reverse-engineering notes.
  - Three Xbox reports are real Bluetooth captures from a Series X|S pad.
- The DualShock 4 v2, its adapter, the DualSense Edge, the Joy-Cons and the charging grip are listed because the Linux drivers read them exactly as the first DualShock 4, the DualSense and the Pro Controller. Their own descriptors aren't in the tests.

None of this has been run with a real pad yet.

## Licence

GPLv3.
