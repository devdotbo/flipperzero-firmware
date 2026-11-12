<picture>
    <source media="(prefers-color-scheme: dark)" srcset="/.github/assets/dark_theme_banner.png">
    <source media="(prefers-color-scheme: light)" srcset="/.github/assets/light_theme_banner.png">
    <img
        alt="A pixel art of a Dophin with text: Flipper Zero Official Repo"
        src="/.github/assets/light_theme_banner.png">
</picture>

# Flipper Zero Firmware

- [Flipper Zero Official Website](https://flipper.net) - A simple way to explain to your friends what Flipper Zero can do.
- [Flipper Zero Firmware Update](https://flipper.net/pages/downloads) - Improvements for your dolphin: latest firmware releases, upgrade tools for PC and mobile devices.
- [User Documentation](https://docs.flipper.net/zero) - Learn more about your dolphin: specs, usage guides, and anything you want to ask.
- [Developer Documentation](https://developer.flipper.net/flipperzero/doxygen) - Dive into the Flipper Zero Firmware source code: build system, firmware structure, and more.

## About this fork

This is an unofficial fork of official Flipper Zero firmware release 1.5.1. It adds a Bluetooth
Low Energy central HAL (scan, one connection, characteristic discovery, write without response)
and selects the BLE Full radio stack, which the central role needs. It also contains an external
app that controls Govee H6006 bulbs with it.

- [BLE central HAL](documentation/BleCentralHal.md): API for app authors.
- [Govee H6006 app](applications_user/govee_h6006/README.md): requirements, flashing, use.
- [Host tools of the app](applications_user/govee_h6006/host/README.md): tests and remote control.

### Getting this fork

The changes are on the branch `feat/ble-central-1.5.1` of
<https://github.com/devdotbo/flipperzero-firmware>. The default branch of that repository does not
contain them. Clone the branch with its submodules:

```shell
git clone --recursive --branch feat/ble-central-1.5.1 https://github.com/devdotbo/flipperzero-firmware.git
```

### Building and flashing this fork

The sections "Cloning source code", "Building" and the two "Flashing firmware" sections further
down are the official project's text. They clone the official repository and use the default
build options, and plain `./fbt flash_usb` does not install the Full radio stack. For this fork
use the steps in the [Govee H6006 app README](applications_user/govee_h6006/README.md#build-and-install):
build with `COMPACT=1 DEBUG=0`, and on a device that still runs the Light radio stack install
once with `./fbt COMPACT=1 DEBUG=0 flash_usb_full`, which also writes the Full stack.

### Safety and going back

Installing the Full stack rewrites the radio coprocessor. That is the risky step. During
development of this fork, one full update that changed the radio stack left a device unable to
start until its main firmware was written again with an SWD programmer. The update package is
copied over USB, and the Flipper then installs it from the SD card by itself, restarting several
times while it removes the old radio stack and writes the new one. Charge the battery first and
keep the USB cable connected until the update has finished. The key combinations for reset and
recovery are in [Key Combos](documentation/KeyCombo.md).

The built-in apps of official 1.5.1 are unchanged, and external apps built for official 1.5.1 run
on this fork, because its API table contains every entry of the official one. The firmware
accepts both the Light and the Full radio stack when it starts the radio.

To go back, install an official firmware release with qFlipper or the Flipper mobile app. The
official update package puts the official main firmware and the Light stack back, so it writes
the radio coprocessor again.

The "Issues", "Discussions" and "Pull Request" links in the sections below lead to the official
project, not to this fork. This fork has no issue tracker.

# Contributing

Our main goal is to build a healthy and sustainable community around Flipper, so we're open to any new ideas and contributions. We also have some rules and taboos here, so please read this page and our [Code of Conduct](/CODE_OF_CONDUCT.md) carefully.

## I need help

The best place to search for answers is our [User Documentation](https://docs.flipper.net/zero). If you can't find the answer there, check our [Discord Server](https://flipp.dev/discord). If you want to contribute to the firmware development or modify it for your own needs, you can also check our [Developer Documentation](https://developer.flipper.net/flipperzero/doxygen).

## I want to report an issue

If you've found an issue and want to report it, please check our [Issues](https://github.com/flipperdevices/flipperzero-firmware/issues) page. Make sure the description contains information about the firmware version you're using, your platform, and a clear explanation of the steps to reproduce the issue.

## I want to propose a new feature

If you have a feature request or want to vote on an existing one, please use [Discussions](https://github.com/flipperdevices/flipperzero-firmware/discussions) and follow our [Discussion Guidelines](https://github.com/flipperdevices/flipperzero-firmware/discussions/4395).

## I want to contribute code

Before opening a PR, please confirm that your changes must be contained in the firmware. Many ideas can easily be implemented as external applications and published in the [Flipper Application Catalog](https://github.com/flipperdevices/flipper-application-catalog). If you are unsure, reach out to us on the [Discord Server](https://flipp.dev/discord) or the [Issues](https://github.com/flipperdevices/flipperzero-firmware/issues) page, and we'll help you find the right place for your code.

Also, please read our [Contribution Guide](/CONTRIBUTING.md) and our [Coding Style](/CODING_STYLE.md), and make sure your code is compatible with our [Project License](/LICENSE).

Finally, open a [Pull Request](https://github.com/flipperdevices/flipperzero-firmware/pulls) and make sure that CI/CD statuses are all green.

# Development

Flipper Zero Firmware is written in C, with some bits and pieces written in C++ and armv7m assembly languages. An intermediate level of C knowledge is recommended for comfortable programming. C, C++, and armv7m assembly languages are supported for Flipper applications.

## Requirements

Supported development platforms:

- Windows 10+ with PowerShell and Git (x86_64)
- macOS 12+ with Command Line tools (x86_64, arm64)
- Ubuntu 20.04+ with build-essential and Git (x86_64)

Supported in-circuit debuggers (optional but highly recommended):

- [Flipper Zero Wi-Fi Development Board](https://flipper.net/products/wifi-devboard)
- CMSIS-DAP compatible: Raspberry Pi Debug Probe and etc...
- ST-Link (v2, v3, v3mods)
- J-Link

Flipper Build System will take care of all the other dependencies.

## Cloning source code

Make sure you have enough space and clone the source code:

```shell
git clone --recursive https://github.com/flipperdevices/flipperzero-firmware.git
```

## Building

Build firmware using Flipper Build Tool:

```shell
./fbt
```

## Flashing firmware using an in-circuit debugger

Connect your in-circuit debugger to your Flipper and flash firmware using Flipper Build Tool:

```shell
./fbt flash
```

## Flashing firmware using USB

Make sure your Flipper is on, and your firmware is functioning. Connect your Flipper with a USB cable and flash firmware using Flipper Build Tool:

```shell
./fbt flash_usb
```

## Documentation

- [Flipper Build Tool](/documentation/fbt.md) - building, flashing, and debugging Flipper software
- [Applications](/documentation/AppsOnSDCard.md), [Application Manifest](/documentation/AppManifests.md) - developing, building, deploying, and debugging Flipper applications
- [Hardware combos and Un-bricking](/documentation/KeyCombo.md) - recovering your Flipper from the most nasty situations
- [Flipper File Formats](/documentation/file_formats) - everything about how Flipper stores your data and how you can work with it
- [Universal Remotes](/documentation/UniversalRemotes.md) - contributing your infrared remote to the universal remote database
- And much more in the [Developer Documentation](https://developer.flipper.net/flipperzero/doxygen)

# Project structure

- `applications`        - Applications and services used in firmware
- `applications_users`  - Place for your additional applications and services
- `assets`              - Assets used by applications and services
- `documentation`       - Documentation generation system configs and input files
- `furi`                - Furi Core: OS-level primitives and helpers
- `lib`                 - Our and 3rd party libraries, drivers, tools and etc...
- `site_scons`          - Build system configuration and modules
- `scripts`             - Supplementary scripts and various python libraries
- `targets`             - Firmware targets: platform specific code

Also, see `ReadMe.md` files inside those directories for further details.

# Links

- Discord: [flipp.dev/discord](https://flipp.dev/discord)
- Reddit: [reddit.com/r/flipperzero](https://www.reddit.com/r/flipperzero/)
- Website: [flipper.net](https://flipper.net)
- Kickstarter: [kickstarter.com](https://www.kickstarter.com/projects/flipper-devices/flipper-zero-tamagochi-for-hackers)

## SAST Tools

- [PVS-Studio](https://pvs-studio.com/pvs-studio/?utm_source=website&utm_medium=github&utm_campaign=open_source) - static analyzer for C, C++, C#, and Java code.
