#!/usr/bin/env python3
"""A stand-in for Payload Manager (pldmgr) on port 8084.

It answers the three things the dumper's page and payload ask of it: the
payload list, an upload, and a launch. Uploads are really written to disk, so
the payload's "is this the same build?" check has a file to read. A launch
starts the simulator, which is what makes the cached page's start button
testable end to end.

    usage: fake_pldmgr.py <run dir> <sim binary> [port]
"""
import glob, http.server, json, os, subprocess, sys, urllib.parse

RUN, SIM = sys.argv[1], sys.argv[2]
PORT = int(sys.argv[3]) if len(sys.argv) > 3 else 8084
STORE = os.path.join(RUN, "pstore", "ps5-app-dumper")
os.makedirs(STORE, exist_ok=True)


class Handler(http.server.BaseHTTPRequestHandler):
    def reply(self, code, body, ctype="text/plain"):
        self.send_response(code)
        self.send_header("Access-Control-Allow-Origin", "*")   # as pldmgr does
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/list_payloads":
            body = json.dumps({"payloads": sorted(glob.glob(STORE + "/*.elf")), "meta": {}})
            self.reply(200, body.encode(), "application/json")
        elif self.path.startswith("/loadpayload:"):
            os.makedirs(os.path.join(RUN, "usb0"), exist_ok=True)
            subprocess.Popen([SIM, "usb0", "8099"], cwd=RUN,
                             stdout=open(os.path.join(RUN, "sim.log"), "w"), stderr=subprocess.STDOUT)
            self.reply(200, b"OK")
        else:
            self.reply(404, b"no such route\n")

    def do_POST(self):
        url = urllib.parse.urlparse(self.path)
        name = urllib.parse.parse_qs(url.query).get("filename", [""])[0]
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        if url.path != "/manage:upload" or not name or "/" in name or ".." in name:
            return self.reply(400, b"Invalid filename\n")
        with open(os.path.join(STORE, name), "wb") as f:
            f.write(body)
        self.reply(200, b"OK")

    def log_message(self, *args):
        pass


print("fake pldmgr on http://127.0.0.1:%d, storing in %s" % (PORT, STORE))
http.server.HTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
