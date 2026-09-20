#!/bin/sh
# Puts the web-homebrew client kit into an app's page. A line that says
#
#   /* @WHB_CSS@ */          becomes whb.css
#   /* @WHB_JS@ */           becomes whb.js
#   <!-- @WHB:name@ -->      becomes name.html - the markup every app shares:
#                            head, chips, offline, row-tile, row-store, access,
#                            foot, unlock, toast
#
# The page stays a single file - it is served, and cached, as one document.
#
#   usage: inline.sh <page.html> <kit dir> > <out.html>
set -e
awk -v kit="$2" '
    function paste(file,   line, n) {
        n = 0
        while ((getline line < file) > 0) { print line; n++ }
        close(file)
        if (!n) { print "inline.sh: " file " is missing or empty" > "/dev/stderr"; exit 1 }
    }
    $0 == "/* @WHB_CSS@ */" { paste(kit "/whb.css"); next }
    $0 == "/* @WHB_JS@ */"  { paste(kit "/whb.js");  next }
    /^[ \t]*<!-- @WHB:[a-z-]+@ -->[ \t]*$/ {
        name = $0; sub(/^[ \t]*<!-- @WHB:/, "", name); sub(/@ -->[ \t]*$/, "", name)
        paste(kit "/" name ".html"); next
    }
    { print }
' "$1"
