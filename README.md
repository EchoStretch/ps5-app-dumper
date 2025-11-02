# PS5 App Dumper

**PS5 App Dumper**

A small utility to dump PS5 application files from the console's `pfsmnt` to a connected USB storage device. This project builds into an ELF payload that runs on the PS5 and copies the app files to a mounted USB drive — **it does not support network transfers**.

---

## Important note

This tool **only dumps to USB**. It does **not** stream or transfer dumps over the network. If you need network transfer capabilities you should use a different tool or implement a custom transfer solution.

---

## Requirements

* PS5 Payload SDK by John Tornblom (required). Download and install the SDK and make it available at build time (example: `/opt/ps5-payload-sdk`).

  * Repository: [https://github.com/ps5-payload-dev/pacbrew-repo](https://github.com/ps5-payload-dev/pacbrew-repo)

* A PS5 able to run payloads (Jailbroken 1.00 - 10.01).

* A USB drive formatted and mounted on the PS5 (the dumper writes files to the USB).

* **elfldr.elf** — all delivery methods require `elfldr.elf` running on the PS5 to accept and execute incoming payloads. Download: [https://github.com/ps5-payload-dev/elfldr/releases/tag/v0.21](https://github.com/ps5-payload-dev/elfldr/releases/tag/v0.21)

---

## Building

1. Download and install the required PS5 Payload SDK. Note its path (we'll use `/opt/ps5-payload-sdk` as an example).

2. Point the project at the SDK by exporting the environment variable or editing the `Makefile`:

```bash
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
```

3. Build from the repository root:

```bash
make
```

You should get an ELF binary such as `ps5-app-dumper.elf`.

---

## Usage

Both methods require the target PS5 application (the game) to be **already running** and the USB drive to be **plugged into the console** before sending or launching the payload.

### Method 1 — Homebrew launcher + webserv

1. **Copy `ps5-app-dumper` folder** to:

   * `/data/homebrew/`
   * `/mnt/usb#/homebrew/` *(replace `#` with your USB number, e.g., `usb0`, `usb1`, etc.)*
   * `/mnt/ext#/homebrew/` *(replace `#` with your EXT number, e.g., `ext0`, `ext1`, etc.)*

2. **Included Payload Files:**

   * `ps5-app-dumper.elf`
   * `homebrew.js`

3. Open the Homebrew Launcher with webserv loaded. You will see **PS5 App Dumper** listed. While the app is open, select **PS5 App Dumper** and it will start dumping to USB.

**Downloads:**

* Homebrew launcher: [https://github.com/ps5-payload-dev/websrv/releases/tag/v0.24](https://github.com/ps5-payload-dev/websrv/releases/tag/v0.24)
* webserv: [https://github.com/ps5-payload-dev/websrv/releases/tag/v0.26.1](https://github.com/ps5-payload-dev/websrv/releases/tag/v0.26.1)

### Method 2 — Send payload with Netcat GUI (Modded Warfare)

1. On your PC, run the Netcat GUI (Modded Warfare) to prepare to send the payload.

2. Ensure the target game is already open on the PS5 and the USB drive is plugged into the console.

3. Use the Netcat GUI to send the `ps5-app-dumper.elf` payload to the PS5. The dumper will execute and write the dumped files to the USB drive.

**Downloads:**

* Netcat Gui 1.3: [https://www.sendspace.com/file/v765gd](https://www.sendspace.com/file/v765gd)

### Method 3 — Send payload with socat

1. Put `ps5-app-dumper.elf` in the folder where you'll run `socat` from.

2. With the app already open on the PS5 and the USB drive plugged in, run the following command on your PC with cmd (replace `<ip>` with the PS5 IP address):

```bash
socat -t 99999999 - TCP:<ip>:9021 < ps5-app-dumper.elf
```

3. The payload will be sent to the PS5. The dumper's log output will appear on the PS5 console screen and in the socat status window.

**Download (socat for Windows)**: [https://github.com/tech128/socat-1.7.3.0-windows](https://github.com/tech128/socat-1.7.3.0-windows)

---

## Contributing

Contributions are welcome. When opening issues or PRs, please include:

* Steps to reproduce the issue.
* Build logs and console output.
* The PS5 firmware version used during testing and any relevant SDK info.

---

## Credits / Links

* Project repository: [https://github.com/EchoStretch/ps5-app-dumper](https://github.com/EchoStretch/ps5-app-dumper)
* PS5 Payload SDK (John Tornblom / pacbrew-repo): [https://github.com/ps5-payload-dev/pacbrew-repo](https://github.com/ps5-payload-dev/pacbrew-repo)
* PS5-SELF-Decrypter ( By Specter / Updated By Idlesauce): [https://github.com/idlesauce/PS5-SELF-Decrypter](https://github.com/idlesauce/PS5-SELF-Decrypter)
---
