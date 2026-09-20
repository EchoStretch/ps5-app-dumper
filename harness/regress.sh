#!/bin/sh
# Starts a fresh simulator, asks it everything - every route, nonsense
# included - and writes the normalised answers to a file. Two such files must
# diff clean across a change that is not meant to alter behaviour.
#
#   usage: regress.sh <sim binary> <output file> [port]
set -u
SIM="$1"; OUT="$2"; PORT="${3:-8099}"
HERE="$(cd "$(dirname "$0")" && pwd)"; RUN="$HERE/out/run"; B="http://127.0.0.1:$PORT"

pkill -f "$SIM" 2>/dev/null; sleep 0.5
# A simulator left over from `make run` goes by another command line and
# survives the line above. Ours would then move one port up, and the old one
# would answer every question here - with whatever state it has gathered.
if curl -s -m 2 -o /dev/null "$B/api/status"; then
    echo "something already answers on $B - stop it first (make run left open?)" >&2; exit 1
fi
# the pretend drive and the pretend console storage both start empty
rm -rf "$RUN/usb0" "$RUN/data"; mkdir -p "$RUN/usb0"
( cd "$RUN" && "$SIM" usb0 "$PORT" > "$RUN/sim.log" 2>&1 & )
for i in 1 2 3 4 5 6 7 8 9 10; do curl -s -o /dev/null "$B/api/status" && break; sleep 0.4; done

norm() { sed -E 's/"(started|finished|now|copyStarted|seq)":[0-9]+/"\1":N/g; s/[0-9]{4}-[0-9]{2}-[0-9]{2}_[0-9]{6}/STAMP/g'; }
q() {
    printf '\n### %s %s %s\n' "$1" "$2" "${3:-}"
    if [ "$1" = GET ]; then curl -s -m 5 -w '\n[%{http_code} %{content_type}]' "$B$2"
    else curl -s -m 5 -w '\n[%{http_code} %{content_type}]' -X POST -d "${3:-}" "$B$2"; fi | norm
}

{
  printf '### GET / (bytes, hash)\n'; curl -s "$B/" | wc -c | tr -d ' '; curl -s "$B/" | shasum | cut -c1-12
  printf '### GET /icon.png (bytes)\n'; curl -s "$B/icon.png" | wc -c | tr -d ' '
  curl -s -o /dev/null -w '[%{http_code} %{content_type}]' "$B/apple-touch-icon.png"
  q GET /cache.appcache; q GET /app.webmanifest; q GET /index.html-nope; q GET /api/nope
  q GET /api/status; q GET "/api/status?since=99999"; q GET /api/devices; q GET /api/config; q GET /api/library
  q GET /api/self; q GET /api/tile; q GET "/api/self/compare?path=/etc/passwd"
  q GET "/api/browse?mount=usb0&path="; q GET "/api/browse?mount=/etc&path="; q GET "/api/browse?mount=usb0&path=../.."
  q GET "/api/icon?app=nope"; q GET "/api/libicon?title=PPSA01234"; q GET "/api/size?app=nope"
  q POST /api/config "queueDelay=7&split=1&dumpSubdir=dumps"; q POST /api/config "dumpSubdir=../x"
  q POST /api/mkdir "mount=usb0&path=&name=made"; q POST /api/mkdir "mount=usb0&path=&name=../evil"
  q POST /api/dump ""; q POST /api/dump "app=nope&target=usb0"; q POST /api/abort ""
  q POST /api/queue/start "titles=&target=usb0"; q POST /api/queue/start "titles=NOPE00000&target=usb0"
  q POST /api/queue/start "titles=PPSA01234&target=usb0&o_PPSA01234=zz"; q POST /api/queue/skip ""; q POST /api/queue/clear ""
  q POST /api/dumps/delete "mount=usb0&folder=../x"; q POST /api/launch "title=bad"; q POST /api/launch "title=PPSA01234"
  sleep 6; q GET /api/devices; q POST /api/launch "title=CUSA07211"; q POST /api/tile ""; q GET /api/tile
  # access: loopback is trusted, so the lock itself is asked over the LAN address
  q GET /api/access | sed -E 's/"code":"[0-9]{6}"/"code":"CODE"/'
  LAN="$(ipconfig getifaddr en0 2>/dev/null || ipconfig getifaddr en1 2>/dev/null || hostname -I 2>/dev/null | cut -d' ' -f1)"
  if [ -n "$LAN" ]; then
    B="http://$LAN:$PORT"
    q GET /api/access; q GET "/api/access?token=nope"; q POST /api/config "queueDelay=9"
    q POST /api/unlock "code=abc"; q POST /api/access/show ""; q POST /api/quit ""
    B="http://127.0.0.1:$PORT"
  fi
  # the dump library: nothing there, then the refusals
  q GET /api/dumps; q POST /api/dumps/move "mount=usb0&dir=&folder=homebrew&toMount=usb0&toDir=x"
  q POST /api/dumps/move "mount=usb0&dir=&folder=PPSA01234-app0&toMount=usb0&toDir=../x"
  q POST /api/dumps/move "mount=usb0&dir=&folder=PPSA01234-app0&toMount=usb0&toDir=x"; q POST /api/dumps/move/cancel ""
  q POST /api/shadowmount/stop ""; q POST /api/shadowmount/stop ""; q GET /api/dumps
  q POST /api/dumps/remove "mount=usb0&dir=homebrew&folder=PPSA01234-app0"; q POST /api/dumps/remove "mount=usb0&dir=homebrew&folder=PPSA01234-app0&confirm=yes"
  q POST /api/dumps/remove "mount=usb0&dir=homebrew&folder=PPSA01234-app0&confirm=PPSA01234-app0"; q POST /api/dumps/remove "mount=usb0&dir=&folder=homebrew&confirm=homebrew"
  q POST /api/dumps/unlink "mount=usb0&dir=homebrew&folder=PPSA01234-app0"; q POST /api/dumps/unlink "mount=usb0&dir=..&folder=PPSA01234-app0"
  q POST /api/quit ""
} > "$OUT" 2>&1

sleep 1; pkill -f "$SIM" 2>/dev/null
echo "$(grep -c '^###' "$OUT") requests -> $OUT"
