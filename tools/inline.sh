#!/bin/sh
# Puts the web-homebrew client kit into an app's page: the line that says
# /* @WHB_CSS@ */ becomes whb.css, the one that says /* @WHB_JS@ */ whb.js.
# The page stays a single file - it is served, and cached, as one document.
#
#   usage: inline.sh <page.html> <kit dir> > <out.html>
set -e
awk -v css="$2/whb.css" -v js="$2/whb.js" '
    function paste(file,   line) { while ((getline line < file) > 0) print line; close(file) }
    $0 == "/* @WHB_CSS@ */" { paste(css); next }
    $0 == "/* @WHB_JS@ */"  { paste(js);  next }
    { print }
' "$1"
