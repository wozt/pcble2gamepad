# Controller Studio

`pcble2gamepad` is the GTK4/libadwaita front end for the Nintendo Classic HID
controller backend. The interface has five pages:

- **Connection** selects Switch Pro Controller or a Joy-Con pair, assigns Bluetooth
  adapters by stable address, starts or stops the console session, chooses the input
  source and arms input.
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
listeners. The backend temporarily powers the selected adapter if BlueZ's restart
left it off, then restores the previous power state. It also records a private
`btmon` capture. The backend accepts commands
only from the desktop UID over the local socket documented in [the API](api.md).

The Pro Controller profile uses one adapter and one backend. A Joy-Con pair uses
two adapters and two backend instances, because left and right Joy-Con must have
independent Classic Bluetooth identities. Controller Studio sends each complete
input state to both; the left and right protocol profiles expose only their own
buttons and stick on the wire. The shared SDP profile and pairing agent cover both
selected adapters for the lifetime of the pair session.

```text
keyboard / SDL gamepad
          |
  GTK Controller Studio
          |
 private Unix socket, complete input frames
          |
privileged C Classic HID backend(s)
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
available. Joy-Con L/R identities, report masking and two-socket GTK routing are
covered by wire and simulated integration tests. A real paired Joy-Con session is
still unverified because only one Bluetooth adapter is currently present.

The controller catalog currently contains Nintendo Switch Pro Controller and
Nintendo Joy-Con Pair. Sony and Microsoft Bluetooth controller profiles are planned
for the same selection model once their individual HID transports are implemented.
