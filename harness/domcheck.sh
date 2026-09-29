#!/bin/sh
# Loads the page in headless Chrome, lets its script run, and says what ended
# up on screen. A syntax check cannot tell that a function went missing; this
# can. The page polls for ever, so Chrome never exits by itself: it is ended
# once the DOM has been written.
#
#   usage: domcheck.sh [url]      (a simulator must be serving it)
URL="${1:-http://127.0.0.1:8099/}"
HERE="$(cd "$(dirname "$0")" && pwd)"; OUT="$HERE/out"; mkdir -p "$OUT"; rm -f "$OUT/dom.html"
CHROME="${CHROME:-/Applications/Google Chrome.app/Contents/MacOS/Google Chrome}"

"$CHROME" --headless=new --disable-gpu --no-first-run --user-data-dir="$OUT/chrome-profile" \
    --virtual-time-budget=4000 --dump-dom "$URL" 2>/dev/null > "$OUT/dom.html" &
for i in 1 2 3 4 5 6 7 8 9 10 11 12; do sleep 1; [ -s "$OUT/dom.html" ] && break; done
sleep 1; pkill -f "$OUT/chrome-profile" 2>/dev/null

python3 - "$OUT/dom.html" <<'PY'
import re, sys
d = open(sys.argv[1]).read()
n = lambda p: len(re.findall(p, d))
g = lambda p: (re.search(p, d) or [None, None])[1]
# each count includes one hit from the page's own script source
print("titles=%d drives=%d header=%r connection=%r serverlog=%d cachelog=%d bytes=%d" % (
    n(r'class="tile lib"'), n(r'class="drive"'), g(r'id="sub">([^<]*)'), g(r'id="conntext">([^<]*)'),
    n(r'Web UI listening'), n(r'Cache \['), len(d)))
PY
