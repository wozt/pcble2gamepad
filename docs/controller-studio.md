# Controller Studio

`pcble2gamepad` is the GTK4/libadwaita front end for the validated Switch 1 Pro
Controller backend. The interface has five pages:

- **Connection** selects a Bluetooth adapter by stable address, starts or stops the
  console session, chooses the input source and arms input.
- **Keyboard bindings** remaps all buttons, D-pad directions, stick clicks and both
  stick directions. WASD and ZQSD presets are included. Escape cancels learning or
  pauses keyboard input; Backspace clears a binding.
- **PC controller** discovers SDL-compatible USB and Bluetooth gamepads and remaps
  their buttons and triggers. Hot-unplug releases controller state.
- **Input settings** controls radial dead zone, sensitivity, per-axis inversion,
  stick swapping and optional background gamepad input.
- **Diagnostics** shows association, L2CAP and HID initialization milestones and
  can copy them to the clipboard.

Profiles are stored as readable INI files in
`~/.config/pcble2gamepad/profiles`. The default profile is created on first run.
The live controller drawing previews the composed input frame before it is sent.

## Process boundary

The GTK process never opens Bluetooth sockets and does not run as root. Starting a
session authenticates the installed `run-classic.sh` through `pkexec`. The runner
starts a C backend that owns the temporary Agent1, SDP record and L2CAP PSM 17/19
listeners. It also records a private `btmon` capture. The backend accepts commands
only from the desktop UID over the local socket documented in [the API](api.md).

```text
keyboard / SDL gamepad
          |
  GTK Controller Studio
          |
 private Unix socket, complete input frames
          |
 privileged C Classic HID backend
          |
      Switch 2
```

Input frames are composed at approximately 60 Hz while armed. Keyboard input is
accepted only while the window is focused. Gamepad background input is opt-in.
The backend independently neutralizes state after 500 ms without a frame, so a UI
crash, stalled client or unplugged gamepad cannot leave a held button indefinitely.
If the entire UI disappears, a separate five-second backend lease ends the session
and lets the launcher restore normal BlueZ.

## Validation boundary

On a real Switch 2, the C backend has appeared as a Pro Controller; A, X and Plus
were confirmed on the button-test screen, and left-stick right/left/up movement and
centering were confirmed on the calibration screen. Right-stick reports have wire
tests but were not separately observed in the console UI. GTK keyboard routing has
been checked end to end with the backend simulation. SDL direct forwarding has a
virtual-gamepad test; a physical PC gamepad test is deferred until hardware is
available.
