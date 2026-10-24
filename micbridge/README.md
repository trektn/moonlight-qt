# MicBridge for Moonlight / Sunshine

Native Linux microphone forwarding integrated into this Moonlight fork, with a
native Windows receiver. Neither side needs Python. Sunshine itself needs no patch.

## Linux

In Moonlight **Settings → Audio Settings**, enable **Forward microphone to host
(MicBridge)** and select a microphone, or leave **Default microphone** selected.
Leave the receiver address blank to use the connected Moonlight host. Start a new
stream after changing settings. Recording starts only when the stream is ready,
and stops when it ends. Capture continues while unfocused or the mouse is released.
The performance overlay (**Ctrl+Alt+Shift+S**) shows capture state and capture errors,
peak and RMS levels, clipping percentage, send rate and payload bitrate, send errors,
and receiver feedback. Level and rate measurements cover approximately one second.
Receiver 1.1 reports its state, packet counts, missing packets at playout, late packets,
underruns, backlog trims, queued audio, and actual WASAPI output padding/buffer size.
Counters reset for each microphone stream. Feedback becomes stale after two seconds.
The feedback RTT includes sender-to-receiver transit, receiver scheduling/acknowledgement
delay, return transit, and the next Linux capture callback. It is not one-way audio
latency and does not include playback or microphone application processing.
If microphone capture fails, an error appears over the stream; video can continue.

## Windows host

Use the existing **Steam Streaming Microphone** virtual audio driver. The receiver
plays into **Speakers (Steam Streaming Microphone)**; games, Discord and other voice
apps should record **Microphone (Steam Streaming Microphone)**. It never falls back
to default physical speakers and does not change Windows default audio devices.

From an elevated PowerShell, beside the extracted Windows bundle:

```powershell
powershell -ExecutionPolicy Bypass -File .\install-startup.ps1 -ClientAddress 10.0.10.156
```

This copies the receiver to `%LOCALAPPDATA%\MicBridge`, adds a firewall rule limited
to that executable and Linux client IPv4 address, registers **MicBridge Receiver**
to run silently at sign-in, and starts it. Logs and configuration live in that same
directory. It runs in the interactive user session alongside Sunshine, not as a
Session 0 audio service. The task restarts after failures. It does not depend on a
Sunshine launch hook or leave microphone recording running on Linux between streams.

Other virtual audio cables can be selected with `-OutputDevice 'CABLE Input'`.
Choose the cable's paired recording endpoint in voice apps. Run
`micbridge-receiver.exe --list` to see active playback endpoints. No driver is bundled.

To stop and disable automatic startup:

```powershell
Stop-ScheduledTask -TaskName 'MicBridge Receiver'
Disable-ScheduledTask -TaskName 'MicBridge Receiver'
```

## Transport and buffering

- LAN IPv4 UDP port **39842**, restricted to the configured client by the firewall
  and receiver. Audio is raw and **unencrypted**; use only a trusted LAN or VPN.
- 48,000 samples/second, mono signed 16-bit little-endian PCM. Each packet contains
  240 samples (5 ms), roughly 0.8 Mbit/s including packet headers.
- 16-byte header: ASCII `MCB1`, big-endian uint32 stream ID, uint32 sequence,
  uint16 flags, uint16 sample count. Flags 0/count 240 means audio (496-byte UDP
  payload). Flags 1/count 0 means stream end (16-byte payload).
- New streams get a random ID. A 15 ms initial reorder buffer absorbs short packet
  jitter. Missing packets become silence, late packets are discarded, and excess
  backlog is trimmed at 60 ms. No retransmission or microphone audio is saved.
- WASAPI shared mode converts the mono PCM to the virtual endpoint format.
  Its actual output buffer size is logged. These are buffering settings, not a
  measured end-to-end latency guarantee. The local overlay measures sends, not
  confirmed delivery. Receiver logs include missing packets and underruns.
- Stream end or 300 ms inactivity clears queued input; the Windows endpoint
  continues outputting silence. Already submitted WASAPI audio drains normally.
- Receiver 1.1 sends a 72-byte `MCS1` feedback datagram to the active sender every
  500 ms. Big-endian fields: stream ID at 4, acknowledged sequence at 8, queued
  frames at 12, WASAPI padding at 16, buffer frames at 20, uint64 received/missing/
  late/underrun/trim counters at 24/32/40/48/56, reserved at 64, playing flag at 68.
  Missing counts represent absent packets at their playout deadline, not an exact
  network loss percentage. Underruns count output fills that ran out of queued audio.
  The audio packet format is unchanged; older receivers work without feedback stats.

The Steam endpoint pairing is also used in
[Apollo's microphone implementation](https://github.com/logabell/apollo-microphone/blob/master/docs/remote_microphone.md).
WASAPI shared-mode initialization follows
[Microsoft's IAudioClient documentation](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-initialize).

## Build the Windows receiver on Linux

With LLVM-MinGW extracted, use its C++ compiler:

```sh
cmake -S micbridge/windows -B build/micbridge-windows \
  -DCMAKE_SYSTEM_NAME=Windows \
  -DCMAKE_CXX_COMPILER=/path/to/llvm-mingw/bin/x86_64-w64-mingw32-clang++ \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/micbridge-windows
```

The executable is linked with static language runtimes and only Windows system
DLLs. The Moonlight sender uses SDL audio capture and the existing Qt/SDL build
dependencies.
