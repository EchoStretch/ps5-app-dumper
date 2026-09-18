# PS5 App Dumper

**PS5 App Dumper**

A small utility to dump PS5 application files from the console's `pfsmnt` to a connected USB storage device. This project builds into an ELF payload that runs on the PS5 and copies the app files to a mounted USB drive — **it does not support network transfers**.

Since v1.11 the payload also serves a **web interface**. Start it once, then drive the dump from your phone or PC: pick the running title, pick the drive, change the settings and watch the progress live. The game keeps running the whole time.

---

## Important note

This tool **only dumps to USB**. The web interface controls the dump over the network, but the files themselves are always written to a drive attached to the console. It does **not** stream or transfer dumps over the network.

---

## Web interface

![Web UI](docs/webui.png)

1. Plug in the USB drive and start the payload (see *Usage* below). A notification shows the address, for example `http://192.168.1.42:8081`.
2. Open that address in any browser on the same network — a phone or PC works well, or the console's own browser.
3. Pick the title under **Installed** and press **Start**, or launch the game on the console yourself.
4. Once it shows up under **Running**, choose the destination drive and press **Start dump**.

The page shows every mounted title with its name, icon and version, the size of the selected title against the free space on each drive, the live transfer rate with an ETA, and the full log output. Settings changed in the page are written straight to `config.ini`.

**Starting titles:** the **Installed** tab lists what is installed on the console, internal and external drives alike. Press **Start** and the payload launches the title, waits for it to mount and selects it for you. A PS5 runs one game at a time, so starting a title closes the running one — the button asks first.

Not every title starts this way. A title that the console itself refuses to launch from the home screen will not start from here either, and the page says so. If the console does not expose the launch service at all, the tab still lists the titles but without Start buttons.

**Port:** the first port tried is `8081`, because the homebrew launcher normally holds `8080`. If it is busy the payload walks up to nine ports further and announces the one it settled on. Set `web_port` in `config.ini` to pick another.

**Turning it off:** set `enable_webui = 0` to go back to the old behaviour (dump the running title immediately and exit), or `auto_start = 1` to keep the setting but dump right away.

> The interface has no password. Anyone on your network who can reach the console can start a dump while the payload runs — use it on a network you trust, and shut the payload down from the footer link when you are done.

---

## Requirements

* PS5 Payload SDK by John Tornblom (required). Download and install the SDK and make it available at build time (example: `/opt/ps5-payload-sdk`).

  * Repository: [https://github.com/ps5-payload-dev/pacbrew-repo](https://github.com/ps5-payload-dev/pacbrew-repo)

* A PS5 able to run payloads (Jailbroken 1.00 - 13.60).

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

Every method needs the USB drive **plugged into the console**. With the web interface the game can be started after the payload; in headless mode (`enable_webui = 0`) the game has to be **running already** when the payload launches.

### Method 1 — Homebrew launcher + webserv

1. **Copy `ps5-app-dumper` folder** to:

   * `/data/homebrew/`
   * `/mnt/usb#/homebrew/` *(replace `#` with your USB number, e.g., `usb0`, `usb1`, etc.)*
   * `/mnt/ext#/homebrew/` *(replace `#` with your EXT number, e.g., `ext0`, `ext1`, etc.)*

2. **Included Payload Files:**

   * `ps5-app-dumper.elf`
   * `homebrew.js`

3. Open the Homebrew Launcher with webserv loaded. You will see **PS5 App Dumper** listed. Select it — the payload starts in the background and shows the address of its web interface as a notification.

**Downloads:**

* Homebrew launcher: [https://github.com/ps5-payload-dev/websrv/releases/tag/v0.24](https://github.com/ps5-payload-dev/websrv/releases/tag/v0.24)
* webserv: [https://github.com/ps5-payload-dev/websrv/releases/tag/v0.26.1](https://github.com/ps5-payload-dev/websrv/releases/tag/v0.26.1)

### Method 2 — Send payload with Netcat GUI (Modded Warfare)

1. On your PC, run the Netcat GUI (Modded Warfare) to prepare to send the payload.

2. Ensure the USB drive is plugged into the console.

3. Use the Netcat GUI to send the `ps5-app-dumper.elf` payload to the PS5. The payload starts its web interface and reports the address as a notification; open it in a browser to run the dump.

**Downloads:**

* Netcat Gui 1.3: [https://www.sendspace.com/file/v765gd](https://www.sendspace.com/file/v765gd)

### Method 3 — Send payload with socat

1. Put `ps5-app-dumper.elf` in the folder where you'll run `socat` from.

2. With the USB drive plugged in, run the following command on your PC with cmd (replace `<ip>` with the PS5 IP address):

```bash
socat -t 99999999 - TCP:<ip>:9021 < ps5-app-dumper.elf
```

3. The payload will be sent to the PS5 and its log output appears in the socat status window. Open the announced address in a browser to run the dump.

**Download (socat for Windows)**: [https://github.com/tech128/socat-1.7.3.0-windows](https://github.com/tech128/socat-1.7.3.0-windows)

---

## Configuration

`config.ini` lives next to the dump, in `<drive>/homebrew/`. It is created on first run and rewritten whenever a setting is changed in the web interface.

| Key | Default | Meaning |
| --- | --- | --- |
| `enable_webui` | `1` | Serve the web interface instead of dumping right away |
| `auto_start` | `0` | Dump the running title immediately, even with the web UI enabled |
| `web_port` | `8081` | First TCP port tried for the web interface |
| `dump_subdir` | `homebrew` | Folder below the drive that receives the dump |
| `enable_decrypter` | `1` | Decrypt SELF/SPRX files while dumping |
| `enable_elf2fself` | `0` | Re-sign decrypted executables as FSELF |
| `enable_backport` | `0` | Patch SDK versions down — advanced, may break the dump |
| `ps4_backport_level` | `4` | PS4 SDK target, 1-6 |
| `ps5_backport_level` | `1` | PS5 SDK target, 1-10 |
| `enable_logging` | `1` | Write `log.txt` next to the dump |
| `split` | `3` | PS4 layout: 0 none, 1 app, 2 patch, 3 both |

---

## Contributing

Contributions are welcome. When opening issues or PRs, please include:

* Steps to reproduce the issue.
* Build logs and console output.
* The PS5 firmware version used during testing and any relevant SDK info.

---

## Credits / Links

* Project reference (This Project is base off pfsmnt): [https://github.com/logic-68/pfsmnt-dumper](https://github.com/logic-68/pfsmnt-dumper)
* Project repository: [https://github.com/EchoStretch/ps5-app-dumper](https://github.com/EchoStretch/ps5-app-dumper)
* PS5 Payload SDK (John Tornblom / pacbrew-repo): [https://github.com/ps5-payload-dev/pacbrew-repo](https://github.com/ps5-payload-dev/pacbrew-repo)
* PS5-SELF-Pager ( By Idlesauce): [https://github.com/idlesauce/ps5-self-pager](https://github.com/idlesauce/ps5-self-pager)
* PS4-App-Dumper ( By Alazif @ Scene-Collective): [https://github.com/Scene-Collective/ps4-app-dumper](https://github.com/Scene-Collective/ps4-app-dumper)
---
