# qutaq-dash

## Overview

`qutaq-dash` is Dash menu for Qutaq Desktop.

## Build

To build the program, be sure to install the libraries

**Arch Linux**

```sh
sudo pacman -S --needed base-devel git cmake qt6-base qt6-wayland qt6-tools qt6-svg layer-shell-qt wayland
```

**Debian / Ubuntu**

```sh
sudo apt install build-essential git cmake pkg-config libwayland-dev \
  qt6-base-dev qt6-base-private-dev qt6-wayland-dev qt6-wayland-private-dev \
  qt6-tools-dev qt6-l10n-tools liblayershellqtinterface-dev qt6-svg-plugins
```

**Fedora**

```sh
sudo dnf install gcc-c++ git cmake wayland-devel \
  qt6-qtbase-devel qt6-qtbase-private-devel qt6-qtwayland-devel qt6-qttools-devel qt6-linguist \
  layer-shell-qt-devel qt6-qtsvg
```

Building:

```sh
git clone https://github.com/Qutaq-Linux/qutaq-dash.git
cd qutaq-dash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## Run

The top button of [qutaq-launcher]([https://github.com/solidi/coldice-remastered](https://github.com/Qutaq-Linux/qutaq-launcher)) opens the Dash when it runs.

The Super key, in `~/.config/labwc/rc.xml` inside `<keyboard>`:

```xml
<keybind key="Super_L" onRelease="yes">
  <action name="Execute" command="qutaq-dash --toggle" />
</keybind>
```

The Head Up Display aka HUD on a quick tap of the left Alt:

```xml
<keybind key="Alt_L" onRelease="yes">
  <action name="Execute" command="qutaq-dash --hud" />
</keybind>
```
