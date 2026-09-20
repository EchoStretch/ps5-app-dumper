# Klog Viewer

The console's kernel log in a browser: a live view with a filter, a button that
puts a mark of your own between the kernel's lines, and a switch that also
writes everything to `logs/klog.txt` in the payload's data folder
(`<drive>/homebrew/klogview/` when a drive carries its config.ini, otherwise
`/data/homebrew/klogview/`).

It exists to prove that `webhb/` carries more than the dumper. What is written
here is `main.c` (about 200 lines: where the lines come from, one setting, one
route) and `web/index.html` (the markup and about 60 lines of script). The
server, the page kit, the settings file, the access code, the single running
copy, a place in Payload Manager, the cached page with its offline banner and
surviving rest mode come with the core.

```sh
make sim && make run      # this machine, made-up kernel lines, http://127.0.0.1:8199/
PS5_PAYLOAD_SDK=... make  # klogview.elf
cat klogview.elf | nc -w 6 <console> 9021    # then http://<console>:8181/
```

Good to know:

- `/dev/klog` hands each line to one reader only. While another klog server
  (klogsrv on port 3232) runs, the two share the lines between them.
- The page asks once a second and the core keeps the last 400 lines: a burst
  of more than that between two polls shows only its end. The file has it all.
- Reading is open to the network it runs in, like the dumper's live console;
  the code from the TV is only needed to change something.

## What a second app needs from the core - found by writing this one

- `webhb/webhb.mk`: page inlining, build stamp, icon, the two-stage build, and
  a host build (`make sim`) against `webhb/host/stubs.c`.
- A `/api/status` of the core's own (log feed and busy), for apps that have
  nothing more to tell.
- Telling a running copy by its file name (`/api/self`) rather than by what
  the dumper's status looks like - otherwise this payload would have asked
  the dumper to quit.
- A kit that leaves alone what a page does not have (no tile row here), and
  `whb.app.onLog` to see each line on its way into the console.
- `log_basename`, `lockedFrom`: two names that used to be the dumper's.
