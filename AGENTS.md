# Project instructions

- Implement the application in C11. Do not introduce Python application code.
- Keep all repository content, UI, comments, diagnostics and commit messages in English.
- Keep Joy-Con protocol code independent of Bluetooth transport and GUI code.
- GTK and external integrations control the daemon through the documented Unix socket API.
- Immediate objective: validate a standalone C Switch 1 Pro Controller POC over Classic HID on a real Switch 2, without any real controller in the emulation chain. Do not introduce generic backend abstractions or capture2cloud integration before this POC is validated.
- Joy-Con 2 BLE work is paused. Mouse support alongside buttons/stick remains a future goal.
- Prefer BlueZ D-Bus, then management APIs, raw ATT/L2CAP and finally raw HCI only after documenting a demonstrated limitation.
- Never perform persistent Bluetooth adapter firmware/NVM modifications.
- Clearly separate community findings, public-capture observations, local adapter checks and actual Switch-console validation.
- Do not claim Switch compatibility without a real-console test. Do not integrate capture2cloud before the discovery milestone.
- Build with Meson, run relevant C/IPC tests, update documentation and create focused commits for meaningful changes.
