/* ------------------------------------------------------------------ *
 *  whb.js - what the page of every web homebrew does, whatever the app
 *
 *  Not a module: the build puts this text where the app's page has its
 *  @WHB_JS@ line (tools/inline.sh), inside the page's own function, so
 *  the page stays one cached document. The app fills in whb.app and
 *  calls whbBoot() once its own functions exist:
 *
 *    elf           "ps5-app-dumper" - how the payload's file name starts, to
 *                  find it in a payload manager while the page runs from
 *                  the cache
 *    shortName     "App Dumper" - what the home-screen tile is called
 *    storage       prefix of this page's localStorage keys
 *    busyText      "A dump is running." - why the payload cannot be shut down
 *    lockedFrom    "start, stop or delete anything" - what a locked device may
 *                  not do; "change anything" when left out
 *    afterBusy     "after the dump" - when a new page shows, if not at once
 *    views         { "#settings": "view-settings", ... } - the pages behind
 *                  the menu, by location.hash
 *    settingsView  the one of them that holds the access rows
 *    onView(id)    a page was opened (null: the main one)
 *    onOnline()    the payload answers again: load everything anew
 *    onStatus(d, wasBusy)  the rest of /api/status, once a second
 *    onConfig(c)   the settings changed behind the app's back
 *    onLog(el, line)  a line is about to go into the live console
 *
 *  Elements every page must have, by id: toast, conn, conntext, offline,
 *  offline-msg, offline-spin, offline-start, lockchip, unlock, ul-code,
 *  ul-msg, ul-go, ul-show, ul-close - and a <main>. The rest is served when
 *  it is there: logbox + clearlog (live console), menubtn + menupop +
 *  [data-back] (views), c-require + require-hint + row-access + access-code
 *  (access settings), c-cfgcopy, c-tile + tile-hint, c-store + store-hint,
 *  quit, sub, ver, build, cachestate, .actions.
 * ------------------------------------------------------------------ */
var whb = {
  app: { elf: "payload", shortName: "Homebrew", storage: "whb", busyText: "The payload is busy.",
         afterBusy: "when the work is done", views: {}, settingsView: null },
  online: true, failures: 0,
  logSeq: 0,          /* how far the live console has read the log */
  busy: false         /* what /api/status said last */
};

var $ = function(id){ return document.getElementById(id); };
/* A page need not have every row the kit can serve - no tile, no place in a
   payload manager, no menu. What is not there is left alone. */
function on(id, type, fn){ var e = $(id); if(e) e.addEventListener(type, fn); }
function put(id, text){ var e = $(id); if(e) e.textContent = text; }

/* ---------------- helpers ---------------- */
function bytes(n){
  if(!n && n !== 0) return "—";
  var u = ["B","KB","MB","GB","TB"], i = 0;
  while(n >= 1024 && i < u.length - 1){ n /= 1024; i++; }
  return (i >= 3 ? n.toFixed(2) : i >= 2 ? n.toFixed(1) : Math.round(n)) + " " + u[i];
}
function clock(sec){
  if(!isFinite(sec) || sec < 0) return "—";
  sec = Math.round(sec);
  var h = Math.floor(sec/3600), m = Math.floor((sec%3600)/60), s = sec%60;
  var p = function(v){ return v < 10 ? "0"+v : ""+v; };
  return (h ? h+":" : "") + p(m) + ":" + p(s);
}
function esc(s){
  return String(s).replace(/[&<>"]/g, function(c){
    return {"&":"&amp;","<":"&lt;",">":"&gt;","\"":"&quot;"}[c];
  });
}
var toastTimer;
function toast(msg, kind){
  var t = $("toast");
  t.textContent = msg;
  t.className = "toast show" + (kind ? " " + kind : "");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(function(){ t.className = "toast"; }, 3600);
}
/* Other devices than the console have to show once that they can see the TV:
   the code from there buys a token, and every POST carries it from then on. */
var TOKEN_KEY = "whb.token", refusedTap = false;
function accessToken(){
  try { return localStorage.getItem(TOKEN_KEY) || ""; } catch(e){ return ""; }
}
function api(path, opts){
  return fetch(path, opts).then(function(r){
    return r.json().catch(function(){ return {}; }).then(function(body){
      if(r.status === 401 && path !== "/api/unlock"){ refusedTap = true; openUnlock(); }
      if(!r.ok){
        var err = new Error(body.error || ("HTTP " + r.status));
        err.body = body;
        throw err;
      }
      return body;
    });
  });
}
function post(path, data){
  var body = "", token = accessToken();
  if(data) for(var k in data) if(data.hasOwnProperty(k))
    body += (body ? "&" : "") + encodeURIComponent(k) + "=" + encodeURIComponent(data[k]);
  if(token) body += (body ? "&" : "") + "token=" + encodeURIComponent(token);
  return api(path, {
    method: "POST",
    headers: {"Content-Type": "application/x-www-form-urlencoded"},
    body: body
  });
}

/* ---------------- console ---------------- */
function appendLog(lines){
  if(!lines || !lines.length) return;
  var box = $("logbox");
  if(!box) return;
  var empty = box.querySelector(".empty");
  if(empty) box.removeChild(empty);

  var stick = box.scrollTop + box.clientHeight >= box.scrollHeight - 24;

  lines.forEach(function(l){
    var d = document.createElement("div");
    var low = l.toLowerCase();
    if(low.indexOf("error") >= 0 || low.indexOf("failed") >= 0) d.className = "e";
    else if(low.indexOf("complete") >= 0 || low.indexOf("success") >= 0) d.className = "g";
    else if(low.indexOf("warn") >= 0 || low.indexOf("not found") >= 0) d.className = "a";
    d.textContent = l;
    if(whb.app.onLog) whb.app.onLog(d, l);
    box.appendChild(d);
  });

  while(box.childNodes.length > 400) box.removeChild(box.firstChild);
  if(stick) box.scrollTop = box.scrollHeight;
}

/* ---------------- is the payload there? ---------------- */
function setOnline(ok){
  if(whb.online === ok) return;
  whb.online = ok;
  var el = $("conn");
  el.className = "live " + (ok ? "on" : "off");
  $("conntext").textContent = ok ? "Connected" : "Disconnected";

  $("offline").hidden = ok;
  if(ok){
    /* back from the dead: whatever is on screen predates the payload */
    $("offline-spin").hidden = true;
    $("offline-msg").textContent = OFFLINE_HINT;
    if(whb.app.onOnline) whb.app.onOnline();
  } else {
    findLauncher();
  }
}

/* ---------------- starting the payload from the cached page ----------------
   A browser cannot hand an ELF to the loader on port 9021 - that speaks raw
   TCP. What it can do is ask something that speaks HTTP and starts payloads.
   This is a convenience on top, never a requirement: with no launcher
   around the banner just says how things stand. Further ones go in here. */
var OFFLINE_HINT = "This page comes from the browser's cache. Start the payload the way you usually do - it connects by itself.";
/* Payload managers keep the ELF under a name of their choosing, version
   suffix and all, so it is looked up in their list rather than assumed. */
function payloadPattern(){
  return new RegExp("(^|/)" + whb.app.elf.replace(/[.*+?^${}()|[\]\\]/g, "\\$&") + "[^/]*\\.(elf|bin)$", "i");
}
var LAUNCHERS = [
  { name: "Payload Manager", port: 8084, list: "/list_payloads",
    pick: function(d){ return (d && d.payloads) || []; },
    start: function(path){ return "/loadpayload:" + path; } }
];
var launcher = null;

/* "…_v1.12.elf" -> [1, 12]; a name without a version counts as the oldest. */
function payloadVersion(path){
  var m = /[_-]v?(\d+(?:\.\d+)*)[^\/]*$/i.exec(path);
  return m ? m[1].split(".").map(Number) : [];
}
function newestPayload(paths){
  return paths.slice().sort(function(a, b){
    var x = payloadVersion(a), y = payloadVersion(b);
    for(var i = 0; i < Math.max(x.length, y.length); i++)
      if((x[i] || 0) !== (y[i] || 0)) return (y[i] || 0) - (x[i] || 0);
    return y.length - x.length;
  })[0];
}

function launcherUrl(l, path){ return "http://" + location.hostname + ":" + l.port + path; }

function findLauncher(){
  launcher = null;
  $("offline-start").hidden = true;

  LAUNCHERS.forEach(function(l){
    fetch(launcherUrl(l, l.list)).then(function(r){ return r.ok ? r.json() : null; }).then(function(d){
      if(!d || launcher || whb.online) return;

      var paths = l.pick(d).filter(function(p){ return typeof p === "string" && payloadPattern().test(p); });
      if(!paths.length){
        $("offline-msg").textContent = OFFLINE_HINT + " (" + l.name + " is running, but has no " + whb.app.elf + " ELF in its list - " +
                                       "upload it there once and this page can start it.)";
        return;
      }
      var best = newestPayload(paths);
      launcher = { name: l.name, url: launcherUrl(l, l.start(best)), file: best.split("/").pop() };
      $("offline-start").textContent = "Start it with " + l.name;
      $("offline-start").title = launcher.file;
      $("offline-start").hidden = false;
    }).catch(function(){ /* not there - that is fine */ });
  });
}

function startPayload(){
  if(!launcher) return;
  var b = $("offline-start"), l = launcher;

  b.disabled = true;
  $("offline-spin").hidden = false;
  $("offline-msg").textContent = "Asking " + l.name + " to start " + l.file + "…";

  fetch(l.url).then(function(r){
    b.disabled = false;
    if(r.ok){
      /* the status poll notices it on its own and takes the banner away */
      $("offline-msg").textContent = "Started " + l.file + " - waiting for it to come up…";
      return;
    }
    $("offline-spin").hidden = true;
    $("offline-msg").textContent = l.name + " could not start it (HTTP " + r.status + ").";
  }).catch(function(e){
    b.disabled = false;
    $("offline-spin").hidden = true;
    $("offline-msg").textContent = l.name + " did not answer: " + e.message;
  });
}

/* Asks how things stand, once a second: that is what tells whether the
   payload is there at all, feeds the live console and knows about busy. What
   else the answer holds is the app's (whb.app.onStatus). */
function whbPoll(){
  return api("/api/status?since=" + whb.logSeq).then(function(d){
    whb.failures = 0;
    setOnline(true);

    if(d.log){
      appendLog(d.log.lines);
      if(d.log.seq) whb.logSeq = d.log.seq;
    }

    var wasBusy = whb.busy;
    whb.busy = !!d.busy;
    if(whb.app.onStatus) whb.app.onStatus(d, wasBusy);
  }).catch(function(){
    if(++whb.failures >= 2) setOnline(false);
  });
}

/* ---------------- application cache, with a paper trail ----------------
   The cache works out of sight, and on the console there are no developer
   tools to look into it. So every step is written into the live console,
   and kept across the reload that an update triggers - otherwise the most
   interesting lines would vanish with the page that logged them. */
var BUILD = "@BUILD@".charAt(0) === "@" ? "dev" : "@BUILD@";
function cacheLogKey(){ return whb.app.storage + ".cachelog"; }
var CACHE_STATES = ["uncached", "idle", "checking", "downloading", "update ready", "obsolete"];

function cacheLog(msg){
  var now = new Date(), two = function(v){ return v < 10 ? "0" + v : "" + v; };
  var line = "Cache [" + two(now.getHours()) + ":" + two(now.getMinutes()) + ":" + two(now.getSeconds()) +
             ", build " + BUILD + "] " + msg;
  appendLog([line]);
  try {
    var kept = JSON.parse(localStorage.getItem(cacheLogKey()) || "[]");
    kept.push(line);
    localStorage.setItem(cacheLogKey(), JSON.stringify(kept.slice(-25)));
  } catch(e){}
}

function showCacheState(){
  var ac = window.applicationCache;
  if(!$("cachestate")) return;
  $("cachestate").textContent = ac ? (CACHE_STATES[ac.status] || ac.status) : "not supported";
}

function cacheStart(){
  /* what earlier loads of this page went through */
  try {
    var kept = JSON.parse(localStorage.getItem(cacheLogKey()) || "[]");
    if(kept.length) appendLog(kept.map(function(l){ return "(earlier) " + l; }));
  } catch(e){}

  var ac = window.applicationCache;
  if(!ac){
    cacheLog("this browser has no application cache - the page only opens while the payload runs");
    showCacheState();
    return;
  }

  /* "checking" usually fires before this script runs, hence the status */
  cacheLog("page loaded, cache status: " + (CACHE_STATES[ac.status] || ac.status));

  var said = {
    checking:    "asking the payload for the manifest",
    noupdate:    "manifest unchanged - the cached page is current",
    downloading: "manifest is new - downloading the page",
    cached:      "page stored for the first time - it now opens without the payload",
    obsolete:    "the payload no longer offers a manifest - cache dropped",
    error:       "could not reach the manifest (payload down?) - staying with the cached page"
  };
  Object.keys(said).forEach(function(name){
    ac.addEventListener(name, function(){ cacheLog(said[name]); showCacheState(); });
  });

  /* A changed page is fetched in the background while the cached one is
     already on screen; switch over as soon as it is there. */
  ac.addEventListener("updateready", function(){
    showCacheState();
    try { ac.swapCache(); } catch(e){}
    if(whb.busy){
      cacheLog("a new page is ready - it shows " + whb.app.afterBusy + ", on the next reload");
      return;
    }
    cacheLog("a new page is ready - reloading to show it");
    location.reload();
  });

  showCacheState();
}


/* ---------------- views ---------------- */
/* What is not the app's main business lives on pages of its own (whb.app.views).
   They are further views of this one file - the page has to stay a single
   cached document - and the hash makes the back button work. */
function showView(){
  var VIEWS = whb.app.views;
  var page = VIEWS[location.hash] || null;
  for(var h in VIEWS) if(VIEWS.hasOwnProperty(h)) $(VIEWS[h]).hidden = VIEWS[h] !== page;
  document.querySelector("main").hidden = !!page;
  if(document.querySelector(".actions")) document.querySelector(".actions").hidden = !!page;
  closeMenu();
  if(page === whb.app.settingsView) showAccess();
  if(whb.app.onView) whb.app.onView(page);
  if(page) window.scrollTo(0, 0);
}
function closeMenu(){
  if(!$("menupop")) return;
  $("menupop").hidden = true;
  $("menubtn").setAttribute("aria-expanded", "false");
}

/* ---------------- access ---------------- */
/* A locked device is told so right away, not only when a tap is refused. */
var unlockOffered = false;
function showAccess(){
  api("/api/access?token=" + encodeURIComponent(accessToken())).then(function(a){
    var locked = !!(a.required && !a.unlocked);
    $("lockchip").hidden = !locked;
    if(locked && !unlockOffered){ unlockOffered = true; openUnlock(); }
    /* whether others need a code is for the console's own browser to decide */
    if(!$("c-require")) return;
    $("c-require").checked = !!a.required;
    $("c-require").disabled = !a.local;
    $("require-hint").textContent = a.local
      ? "Phones and PCs may look, but have to enter the code from the TV before they can " + (whb.app.lockedFrom || "change anything") + "."
      : "Can only be changed in the console's own browser. " + (a.required
          ? "Devices like this one enter the code from the TV once."
          : "Right now every device on the network may change things.");
    var row = $("row-access");
    row.hidden = !(a.required && a.unlocked && a.code);
    $("access-code").textContent = a.code || "";
    /* a token from before the code was changed is of no use any more */
    if(a.required && !a.unlocked && accessToken())
      try { localStorage.removeItem(TOKEN_KEY); } catch(e){}
  }).catch(function(){});
}
function openUnlock(){
  if(!$("unlock").hidden) return;
  $("ul-msg").textContent = "";
  $("ul-code").value = "";
  $("unlock").hidden = false;
  try { $("ul-code").focus(); } catch(e){}
}
function closeUnlock(){ $("unlock").hidden = true; }
function tryUnlock(){
  var code = $("ul-code").value.replace(/\D/g, "");
  if(code.length !== 6){ $("ul-msg").textContent = "Six digits."; return; }
  $("ul-go").disabled = true;
  post("/api/unlock", {code: code}).then(function(r){
    try { localStorage.setItem(TOKEN_KEY, r.token); } catch(e){}
    closeUnlock();
    showAccess();
    toast(refusedTap ? "Unlocked - press it again, please." : "Unlocked.", "ok");
    refusedTap = false;
  }).catch(function(e){
    $("ul-msg").textContent = e.message;
  }).then(function(){ $("ul-go").disabled = false; });
}

/* The tile only opens this page in the console browser; the payload still
   has to be running. Asked for once, never polled. */
function showTileState(installed, current){
  if(!$("c-tile")) return;
  var outdated = installed && !current;
  $("c-tile").textContent = outdated ? "Update" : installed ? "Reinstall" : "Install";
  $("c-tile").classList.toggle("warn", outdated);
  $("tile-hint").textContent = outdated
    ? "The tile on the home screen is from an older version, or points at another port. Update it."
    : installed
      ? "The \"" + whb.app.shortName + "\" tile is on the home screen. It opens this page, and can start the payload if it is not running."
      : "Adds an \"" + whb.app.shortName + "\" tile that opens this page";
}
/* ---------------- this copy: version, and a place in Payload Manager ---------------- */
var self = null;

/* stored: "none" | "same" | "other" | "unknown"
   "same" is this very build, "other" a different build under the same
   version - during development the number stays while the code moves. */
function showStoreState(stored){
  if(!$("c-store")) return;
  var can = !!(self && self.canStore), b = $("c-store");
  b.disabled = !can || stored === "same";
  b.classList.toggle("warn", can && stored === "other");
  b.textContent = stored === "same" ? "Saved" : stored === "other" ? "Update" : stored === "unknown" ? "Save again" : "Save";
  $("store-hint").textContent = !self ? "…"
    : !can ? "This copy was started from a stored file, so it is in a payload manager already."
    : stored === "same"  ? "Payload Manager holds exactly this build as " + self.file + "." + ($("c-tile") ? " The home-screen shortcut can start it when the payload is not running." : "")
    : stored === "other" ? "Payload Manager holds a different build of v" + self.version + ". Update it to keep what is running now."
    : stored === "unknown" ? self.file + " is in Payload Manager; whether it is this build could not be told."
    : "Stores this payload as " + self.file + " in pldmgr, so it can be started from there" + ($("c-tile") ? " - and from the home-screen shortcut." : ".");
}

/* pldmgr says whether it has a file of this name; whether that file is this
   very build the payload finds out itself, reading it off the console's disk */
function checkStored(){
  if(!self || !$("c-store")) return;
  fetch("http://" + location.hostname + ":8084/list_payloads").then(function(r){ return r.ok ? r.json() : null; }).then(function(d){
    var found = ((d && d.payloads) || []).filter(function(p){ return typeof p === "string" && p.split("/").pop() === self.file; })[0];
    if(!found){ showStoreState("none"); return; }

    api("/api/self/compare?path=" + encodeURIComponent(found)).then(function(r){
      showStoreState(r.match === "same" ? "same" : r.match === "other" ? "other" : "unknown");
    }).catch(function(){ showStoreState("unknown"); });
  }).catch(function(){ showStoreState("none"); });
}

/* ---------------- boot ---------------- */
/* Wires up what the kit owns and asks the first questions. Call once, after
   whb.app is filled in and the app's own functions exist. */
function whbBoot(){
  on("offline-start", "click", startPayload);

  put("build", BUILD);
  cacheStart();

  on("clearlog", "click", function(){
    $("logbox").innerHTML = '<div class="empty">console cleared</div>';
  });

  on("menubtn", "click", function(e){
    e.stopPropagation();
    var open = $("menupop").hidden;
    $("menupop").hidden = !open;
    $("menubtn").setAttribute("aria-expanded", String(open));
    Array.prototype.forEach.call($("menupop").querySelectorAll("button"), function(b){
      b.classList.toggle("on", (b.getAttribute("data-go") || "") === (whb.app.views[location.hash] ? location.hash : ""));
    });
  });
  document.addEventListener("click", closeMenu);
  if($("menupop")) Array.prototype.forEach.call($("menupop").querySelectorAll("button"), function(b){
    b.addEventListener("click", function(){ location.hash = b.getAttribute("data-go"); closeMenu(); });
  });
  Array.prototype.forEach.call(document.querySelectorAll("[data-back]"), function(b){
    b.addEventListener("click", function(){ location.hash = ""; });
  });
  window.addEventListener("hashchange", showView);

  on("c-require", "change", function(){
    var want = $("c-require").checked ? 1 : 0;
    post("/api/config", {requireCode: want}).then(function(r){
      if(whb.app.onConfig) whb.app.onConfig(r.config);
      toast(want ? "Other devices need the code again." : "Other devices may change things without a code.", want ? "ok" : "bad");
    }).catch(function(e){ toast(e.message, "bad"); }).then(showAccess);
  });
  on("c-cfgcopy", "click", function(){
    var b = $("c-cfgcopy");
    b.disabled = true;
    post("/api/config/console").then(function(r){
      toast("Settings copied to " + r.path, "ok");
    }).catch(function(e){ toast(e.message, "bad"); }).then(function(){ b.disabled = false; });
  });
  on("lockchip", "click", openUnlock);
  on("ul-go", "click", tryUnlock);
  on("ul-code", "keydown", function(e){ if(e.key === "Enter" || e.keyCode === 13) tryUnlock(); });
  on("ul-close", "click", closeUnlock);
  on("ul-show", "click", function(){
    post("/api/access/show").then(function(){
      $("ul-msg").textContent = "Look at the TV.";
    }).catch(function(e){ $("ul-msg").textContent = e.message; });
  });
  showAccess();
  showView();

  if($("c-tile")) api("/api/tile").then(function(r){
    showTileState(!!r.installed, !!r.current);
    if(r.installed && !r.current)
      toast("The home-screen tile is out of date - update it under Menu > Settings.", "bad");
  }).catch(function(){});

  on("c-tile", "click", function(){
    var b = $("c-tile");
    b.disabled = true;
    b.textContent = "Installing…";
    post("/api/tile").then(function(){
      showTileState(true, true);
      toast("Shortcut installed - look for \"" + whb.app.shortName + "\" on the home screen.", "ok");
    }).catch(function(e){
      api("/api/tile").then(function(r){ showTileState(!!r.installed, !!r.current); }).catch(function(){});
      toast("Shortcut not installed: " + e.message, "bad");
    }).then(function(){ b.disabled = false; });
  });
  var quitArm;
  on("quit", "click", function(){
    var b = $("quit");
    if(!b.classList.contains("arm")){
      if(whb.busy){ toast(whb.app.busyText, "bad"); return; }
      b.classList.add("arm");
      b.textContent = "confirm shut down";
      clearTimeout(quitArm);
      quitArm = setTimeout(function(){ b.classList.remove("arm"); b.textContent = "shut down payload"; }, 4000);
      return;
    }
    clearTimeout(quitArm);
    post("/api/quit").then(function(){
      setOnline(false);
      b.textContent = "payload stopped";
      b.disabled = true;
      toast("The payload has been stopped. Relaunch it from the homebrew menu.");
    }).catch(function(e){
      b.classList.remove("arm");
      b.textContent = "shut down payload";
      toast(e.message, "bad");
    });
  });

  put("sub", "Web console · " + location.host);

  api("/api/self").then(function(r){
    self = r;
    put("sub", "Web console · v" + r.version + " · " + location.host);
    put("ver", "v" + r.version + " · ");
    showStoreState("none");
    checkStored();
  }).catch(function(){});

  on("c-store", "click", function(){
    var b = $("c-store");
    b.disabled = true;
    b.textContent = "Saving…";
    post("/api/self/store").then(function(r){
      toast(r.file + " stored in Payload Manager", "ok");
      checkStored();
    }).catch(function(e){
      toast("Not stored: " + e.message, "bad");
      checkStored();
    });
  });
}
