# prospector-touch-zmk-module

Hybrid Prospector firmware ZMK module. One image, two runtime modes auto-selected by USB power:

- **USB present** → Dongle: ZMK split central + USB HID (acts as the keyboard's dongle).
- **Battery** → Scanner: passive BLE observer that decodes 26-byte status advertisements (no pairing, no connection slot).

Touch (CST816S), the ST7789V display, APDS9960 auto-brightness and NVS-persisted settings work in both modes. A USB-power transition triggers a cold reboot so the BLE stack initialises in the correct role.

See `HYBRID_PROSPECTOR_SPEC.md` in the `zmk-config-prospector-touch` repo for the full specification.

## Hardware

Seeeduino XIAO BLE (nRF52840) + ST7789V 1.69" 280×240 LCD (SPI3, MIPI-DBI) + CST816S touch (I2C0 0x15) + APDS9960 ALS (I2C0 0x39) + PWM backlight (P1.11).

The CST816S and ST7789V drivers come from upstream Zephyr; this module ships the mode manager, BLE status scanner, merged LVGL UI, brightness control and settings.

## Build

`config/west.yml`:

```yaml
manifest:
  remotes:
    - name: zmkfirmware
      url-base: https://github.com/zmkfirmware
    - name: dynapulse
      url-base: https://github.com/DynaPulse
  projects:
    - name: zmk
      remote: zmkfirmware
      revision: main
      import: app/west.yml
    - name: prospector-touch-zmk-module
      remote: dynapulse
      revision: main
  self:
    path: config
```

Build the `prospector_touch` shield on board `xiao_ble//zmk`.

## Key configuration

| Kconfig | Default | Purpose |
| --- | --- | --- |
| `CONFIG_PROSPECTOR_MODE_SCANNER` | y | Battery-mode passive BLE observer |
| `CONFIG_PROSPECTOR_TOUCH_ENABLED` | y | CST816S touch + swipe/tap gestures |
| `CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR` | y | APDS9960 auto-brightness |
| `CONFIG_PROSPECTOR_MAX_KEYBOARDS` | 3 | Scanner multi-keyboard slots (1–5) |
| `CONFIG_PROSPECTOR_SCANNER_TIMEOUT_MS` | 480000 | Drop a keyboard after this idle time |
| `CONFIG_PROSPECTOR_DISPLAY_IDLE_S` | 60 | Backlight off after inactivity (0 = never) |
| `CONFIG_PROSPECTOR_HYBRID_BATTERY_DIM` | 25 | Backlight % in scanner/battery mode |
| `CONFIG_PROSPECTOR_DEFAULT_LAYOUT` | 0 | Main layout style (0=Dashboard, 1=Focus, 2=Minimal) |

## Touch gestures

| Gesture | Main | Keyboards | Actions | Settings |
| --- | --- | --- | --- | --- |
| Swipe ←/→ | change page | change page | change page | change page |
| Swipe ↑/↓ | brightness | select keyboard | move cursor | brightness |
| Tap | cycle layout | cycle channel | run action | toggle auto-brightness |

Pages cycle: Main → Keyboards → Actions → Settings → Info.

## Status advertisement protocol

26-byte BLE manufacturer payload (manufacturer `0xFFFF`, service `0xABCD`) carrying battery, active layer + name, modifiers, WPM, profile, connection flags, peripheral batteries and channel. See `include/zmk/status_advertisement.h`.
