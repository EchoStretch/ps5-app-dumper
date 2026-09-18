#!/bin/sh
# Embeds a file into the payload as a C byte array.
#   usage: tools/bin2c.sh <input-file> <symbol-name> > output.c
set -e

in="$1"
sym="$2"

if [ -z "$in" ] || [ -z "$sym" ]; then
    echo "usage: $0 <input-file> <symbol-name>" >&2
    exit 1
fi

size=$(wc -c < "$in" | tr -d ' ')

echo "/* Generated from $in by tools/bin2c.sh - do not edit. */"
echo "#include <stddef.h>"
echo ""
echo "const unsigned char ${sym}[] = {"
od -An -v -tx1 "$in" | awk '
    { for (i = 1; i <= NF; i++) printf "0x%s,%s", $i, (++n % 16 == 0 ? "\n" : " ") }
    END { if (n % 16) printf "\n" }
'
echo "0x00};"
echo ""
echo "const size_t ${sym}_len = ${size};"
