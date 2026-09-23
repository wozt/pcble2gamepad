# Project instructions

- Implement the application in C11. Do not introduce Python application code.
- Keep all repository content, UI, comments, diagnostics and commit messages in English.
- Keep Joy-Con protocol code independent of Bluetooth transport and GUI code.
- GTK and external integrations control the daemon through the documented Unix socket API.
- Current controller catalog: Switch 1 Pro Controller and a paired Switch 1 Joy-Con (L/R) profile over Classic HID. The Pro profile is validated on a real Switch 2. A Joy-Con pair requires two distinct Bluetooth adapters and must not be claimed as console-validated until that hardware test is completed.
- Keep the catalog extensible for later Sony and Microsoft Bluetooth controller profiles. Do not present those profiles as implemented yet.
- Joy-Con 2 BLE work is paused. Mouse support alongside buttons/stick remains a future goal.
- Prefer BlueZ D-Bus, then management APIs, raw ATT/L2CAP and finally raw HCI only after documenting a demonstrated limitation.
- Never perform persistent Bluetooth adapter firmware/NVM modifications.
- Clearly separate community findings, public-capture observations, local adapter checks and actual Switch-console validation.
- Do not claim Switch compatibility without a real-console test. Do not integrate capture2cloud before the discovery milestone.
- Build with Meson, run relevant C/IPC tests, update documentation and create focused commits for meaningful changes.
