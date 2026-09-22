# Prospector ZMK Module

This is a [ZMK module](https://zmk.dev/docs/features/modules) that provides custom status screen support for the [Prospector](https://github.com/carrefinho/prospector) display dongle.

![Four status screen layouts for Prospector](docs/images/status-screen-update-hero.png)

> [!IMPORTANT]
> This branch is a work-in-progress and is only compatible with the Zephyr 4.1 version of ZMK (current main).
>
> The `feat/nv3007-wide-port` branch targets the 2.79-inch **NV3007** panel
> (142×428, used as a 428×142 landscape display). All four original
> layouts are adapted to it: Classic's layer roller becomes a horizontal
> carousel, and Radii puts its layer wheel beside the name and prints each
> battery's charge inside its ring. A fifth layout, Gossip, is new on this
> branch: every key you type flies out of the screen toward you.
>
> The display driver comes from the
> [zmk-nv3007-display](https://github.com/Aleblazer/zmk-nv3007-display)
> module, which must be in your `west.yml` alongside this one (see
> Installation). Its README covers wiring, devicetree properties, tuning and
> hardware test status.
>
> Verified on a XIAO BLE at up to 32 MHz. The Pro Micro wiring is untested.

The wide-screen branch supports both the original Xiao controller and
nice!nano v2-compatible Pro Micro footprint nRF52840 controllers. The NV3007
panel uses the same SPI/DC/reset/backlight pins as the ST7789 panel. The Pro Micro
wiring is documented in
`boards/shields/prospector_adapter/boards/nice_nano_zmk.overlay`.

## Table of Contents

- [Features](#features)
- [Installation](#installation)
- [Status Screens](#status-screens)
- [Usage](#usage)
- [Configuration](#configuration)
- [Troubleshooting](#troubleshooting)
- [Known Issues](#known-issues)
- [To-Do](#to-do)

## Features

- Four status screen layouts to choose from
- Active layer display
- Peripheral battery status
- BLE profile and output indicator
- Active modifier display
- Caps word indicator

## Installation

Your ZMK keyboard should be set up with a dongle as central.

Add this module to your `config/west.yml` with these new entries under `remotes` and `projects`:

```yaml
manifest:
  remotes:
    - name: zmkfirmware
      url-base: https://github.com/zmkfirmware
    - name: aleblazer                             # <--- add this
      url-base: https://github.com/Aleblazer      # <--- and this
  projects:
    - name: zmk
      remote: zmkfirmware
      revision: main
      import: app/west.yml
    - name: prospector-zmk-module                 # <--- and these
      remote: aleblazer                           # <---
      revision: feat/nv3007-wide-port             # <---
    - name: zmk-nv3007-display                    # <--- NV3007 display driver
      remote: aleblazer                           # <---
      revision: main                              # <---
  self:
    path: config
```

Then add the `prospector_adapter` shield to the dongle in your `build.yaml`:

```yaml
---
include:
  - board: xiao_ble//zmk
    shield: [YOUR KEYBOARD SHIELD]_dongle prospector_adapter
```

For more information on ZMK Modules and building locally, see [the ZMK docs page on modules.](https://zmk.dev/docs/features/modules)

## Status Screens

Classic is used by default. To choose a different screen, add one of the following to your `.conf` file:

```ini
CONFIG_PROSPECTOR_STATUS_SCREEN_RADII=y
CONFIG_PROSPECTOR_STATUS_SCREEN_FIELD=y
CONFIG_PROSPECTOR_STATUS_SCREEN_OPERATOR=y
CONFIG_PROSPECTOR_STATUS_SCREEN_GOSSIP=y
```

Gossip shows almost nothing until you type. Each letter, number or symbol
you press appears at a random spot and flies toward you along a random
curved path, growing and fading as it passes. The only fixed elements are
the layer name in the bottom-left corner and one small battery bar per
peripheral in the bottom-right, amber below 20%. Its fonts are generated
from [DINish](https://github.com/playbeing/dinish) (OFL-1.1).

## Usage

For split keyboards, the peripheral battery widget arranges sub-widgets in pairing order. After flashing the dongle, pair the left side first, then the right side. For more than two peripherals, pair them left to right.

The layer display shows the `display-name` property when available, falling back to the layer index otherwise. To add a `display-name` to a keymap layer:

```dts
keymap {
  compatible = "zmk,keymap";
  base {
    display-name = "Base";           # <--- add this
    bindings = <
      ...
    >;
  }
}
```

## Configuration

To customize, add config options to your `.conf` file:
```ini
CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR=n
CONFIG_PROSPECTOR_FIXED_BRIGHTNESS=80
```

### Bring-up

| Name | Description | Default |
| ---- | ----------- | ------- |
| `CONFIG_PROSPECTOR_DEMO_WPM` | Animate the layout with nothing paired, for bring-up and photos: a synthetic typing rhythm for Operator and Field, a layer change every 2.5 s for Classic and Radii (plus sample battery levels on Radii), and a sample sentence typed on a loop for Gossip. Real WPM and layer events are ignored while it is on | n |
| `CONFIG_PROSPECTOR_DEMO_WPM_PERIOD_MS` | One full rise and fall | 6000 |

### General
| Name | Description | Default |
| ---- | ----------- | ------- |
| `CONFIG_PROSPECTOR_ROTATE_DISPLAY_180` | Rotate the display 180 degrees | n |
| `CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR` | Use ambient light sensor for auto brightness | y |
| `CONFIG_PROSPECTOR_FIXED_BRIGHTNESS` | Fixed display brightness when not using ambient light sensor | 50 (1-100) |
| `CONFIG_PROSPECTOR_LAYER_NAME_UPPERCASE` | Convert layer names to uppercase (Operator and Radii only) | y |

### Modifiers
| Name | Description | Default |
| ---- | ----------- | ------- |
| `CONFIG_PROSPECTOR_SHOW_MODIFIERS` | Display modifier key indicators | y |
| `CONFIG_PROSPECTOR_SHOW_INACTIVE_MODIFIERS` | Show inactive modifiers dimmed (Classic and Field only) | y |
| `CONFIG_PROSPECTOR_MODIFIER_ORDER` | Order of modifiers: G=GUI, A=Alt, C=Ctrl, S=Shift | "GACS" |

### Field-specific
| Name | Description | Default |
| ---- | ----------- | ------- |
| `CONFIG_PROSPECTOR_ANIMATION_WPM_REFERENCE` | WPM value at which animation reaches max speed | 70 |
| `CONFIG_PROSPECTOR_ANIMATION_INTENSITY_DECAY_SEC` | Seconds for lines to fade out after typing stops | 30 |
| `CONFIG_PROSPECTOR_ANIMATION_FLOW_DECAY_SEC` | Seconds for line directions and length to settle | 300 |

## Troubleshooting

### Build fails on `newvision,nv3007`

A devicetree error about an unknown `newvision,nv3007` compatible, or
`DT_HAS_NEWVISION_NV3007_ENABLED` never being set, means the
[zmk-nv3007-display](https://github.com/Aleblazer/zmk-nv3007-display) module
is missing from your `config/west.yml`. Add it as shown under Installation.

### RAM overflow error

If you encounter a `region 'RAM' overflowed` error when building, add the following to your `.conf` file to reduce the display buffer size:

```ini
CONFIG_LV_Z_VDB_SIZE=25
```

## Known Issues

- One peripheral may fail to register key presses after connecting to the dongle; reset the affected peripheral to fix. https://github.com/zmkfirmware/zmk/issues/3156
- Operator, Radii: battery display only supports up to three peripherals

## To-Do

- Operator: per-profile BLE status
- Radii: document and improve color theme customization
- OS-specific modifier styles
- Caps lock indication
