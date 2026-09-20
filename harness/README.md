# Host harness

The payload's web server, dump job, queue and dump store, compiled for this
machine and run against a pretend console. It is how the web UI is developed
and how a change is checked before it goes anywhere near a PS5. No SDK needed.

```
make -C harness sim         # build harness/out/sim
make -C harness run         # serve http://127.0.0.1:8099/  (Ctrl-C to stop)
make -C harness pldmgr      # a stand-in for Payload Manager on :8084
make -C harness snapshot    # every route's answer -> out/snapshot.txt
make -C harness render      # what headless Chrome makes of the page (needs `run`)
make -C harness styles      # every element's computed style, view by view -> out/styles.json (needs `run`)
```

What is real and what is not: `mock_console.c` fakes the mounted games, the
library, the launch services, the two dumpers, notifications and the tile.
Everything else - `webhb/` (bar `tile.c` and `instance.c`), the routes,
`dump_job`, `dump_queue`, `dump_store`, `utils` - is the payload's own code. The pretend
console is awkward on purpose; its header comment lists the five titles and
what each one is there to provoke.

## Checking a change that must not change behaviour

```
make -C harness snapshot && cp harness/out/snapshot.txt /tmp/before.txt
# ... make the change ...
make -C harness snapshot && diff /tmp/before.txt harness/out/snapshot.txt
```

`regress.sh` fires about forty requests - every route, with nonsense such as
`../..` and empty parameters among them - and normalises timestamps, so the two
files are identical unless an answer really changed. The hash of the served
page is part of it. This is how the split of `http_server.c` was verified.
It refuses to run while something else answers on the port: a simulator left
over from `make run` would otherwise answer in place of the fresh one.

Requests from 127.0.0.1 are trusted, so the access check never shows there.
The script asks the last few questions over the machine's LAN address to see
the lock; to try the unlock dialog in a browser, open the page that way too.
The simulator has no data folder, so it makes up a new code with every start -
it is in its output, in the line the console would show as a toast.

## Checking the page

`node --check` finds a syntax error; it does not notice that a function the
page calls is gone. `render` loads the page in headless Chrome, lets its script
run and reports what ended up on screen: titles, drives, connection state, log
lines. Each count includes one hit from the page's own source, so compare the
numbers before and after rather than reading them as absolutes.

`styles` is for changes to the stylesheet or the markup that are not meant to
be seen - moving rules around, splitting files. `stylecheck.js` walks the page
through its views and dialogs (queue, settings, folder picker, unlock, a dump
from start to end, the dumps page, delete, and everything un-hidden at once),
at two widths, with and without the flex-gap fallback, and writes down the
computed style of every element. Do it before and after, each time against a
freshly started simulator, and compare:

```sh
node harness/stylediff.js before.json after.json
```

Two runs of the same page differ in a handful of elements that depend on
timing (the progress bar in mid-dump, a button that shows a moment later) -
run the old page twice to know them. Hover, focus and disabled states are not
reached this way; rules for those want a look at what changed places. It needs
Node 20 or newer and Chrome.

## What this cannot show

The dumpers are fakes, so nothing about a real dump is tested here: not the
copy loops, not whether a stop reaches them, not PS4 versus PS5 paths. The
same goes for the launch services, the tile install and the console's browser,
an older WebKit with CSS limits of its own. Those need the console.
