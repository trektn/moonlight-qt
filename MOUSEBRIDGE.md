# Moonlight 6.1.0 + MouseBridge

This fork adds a native Linux physical-mouse sender to Moonlight Qt **6.1.0**.
Python and a separate Linux sender are unnecessary. The Windows PC still runs
the MouseBridge 1.1 receiver and a compatible VMulti driver.

## Use it

1. Install the Arch package with `sudo pacman -U moonlight-qt-mousebridge-*.pkg.tar.zst`.
   It replaces the standard Moonlight package and keeps your existing Moonlight
   hosts, pairing, and settings.
2. In **Settings → Input Settings**, turn on **Use MouseBridge / VMulti for mouse input**.
3. Select your physical mouse. Stable `/dev/input/by-id` names are preferred
   automatically. Use **Refresh** after reconnecting devices.
4. Leave the receiver field blank. MouseBridge uses the selected Moonlight
   host's IPv4 address on UDP **39841**. Set an override only when the receiver
   is on a different machine or an IPv4 override is needed for an IPv6 host.
5. Start streaming normally. **Ctrl+Alt+Shift+C** releases the mouse to Linux
   without stopping the video stream. Press it again to resume forwarding.
   Either side's Ctrl/Alt/Shift keys works. The shortcut is monitored globally
   while the stream is running, including while the mouse is released.

The old Ctrl+Alt+Shift+C cursor-visibility action remains when MouseBridge is
disabled. Moonlight's usual Ctrl+Alt+Shift+Q stops the stream.

MouseBridge uses relative input, so remote-desktop absolute mouse mode is
disabled while it is enabled. Other SDL physical mouse events are suppressed
to prevent duplicate input through Moonlight's regular mouse channel.
Keyboard and gamepad streaming continue through Moonlight.

## Requirements and behavior

- The selected mouse and a keyboard providing the shortcut must be readable
  under `/dev/input`. The application reports a startup error if they are not.
  Use your distribution's normal input-device permissions; do not run Moonlight
  as root. No blanket permission rule is installed by this package.
- The Windows receiver needs incoming UDP 39841 allowed from the Linux PC.
  Packets are intended for a trusted LAN and are not authenticated/encrypted.
- Do not run the standalone Linux MouseBridge sender at the same time.
- The input worker is separate from decoding/rendering. Relative motion is
  batched on a best-effort 1 ms interval; button transitions send immediately
  so quick clicks are preserved. Five mouse buttons and the vertical wheel are
  supported. This is not a hard real-time or measured 1000 Hz guarantee.
- Release, focus loss, stream shutdown, and device disconnection clear remote
  button state and release the exclusive evdev grab. The Windows receiver's
  500 ms silence timeout is an additional fallback. Paused motion is discarded.
- Keyboard input is read solely to recognize the shortcut; it is not logged
  or transmitted by MouseBridge. Normal streamed keys use Moonlight's existing
  keyboard path.
- Resuming requests focus for the Moonlight window. A Wayland compositor may
  require you to focus Moonlight yourself before streamed keyboard input resumes.

## Build the Arch package

From `packaging/arch`, run `makepkg -s`. The recipe checks out the pinned fork
tag and its original submodules, builds with Qt 6 and hardware-decoder support,
and runs the synthetic input/UDP tests before packaging.

Compatibility changes allow the 6.1.0 codebase to build with current FFmpeg
pixel-format and Vulkan queue APIs. The fork retains VA-API, VDPAU, NVIDIA
CUDA/NVDEC, Vulkan/libplacebo, DRM, and Wayland backends when their build
dependencies are present.

For a direct local build with the submodules initialized:

```sh
mkdir -p build/local
cd build/local
qmake6 ../../moonlight-qt.pro CONFIG+=release PREFIX=/usr
make -j"$(nproc)" release
./app/moonlight
```

## Tests

```sh
mkdir -p build/mousebridge-test
cd build/mousebridge-test
qmake6 ../../tests/mousebridge/mousebridge-test.pro
make
./mousebridge-test
```

Tests use FIFO devices, mocked input ioctls, and real loopback UDP. They verify
device filtering, global left/right shortcut handling, pause/resume, dropping
paused movement, preserving quick clicks, button swapping, disconnect cleanup,
and failed regrab recovery without opening or grabbing physical input devices.
