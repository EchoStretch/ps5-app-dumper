// Walks the page through its views and dialogs in headless Chrome and writes
// down the computed style of every element in each of them. Two such files -
// one from before a change to the page's CSS or markup, one from after - must
// be equal when the change was not meant to be seen (compare with
// stylediff.js). A simulator must be serving the page.
//
//   usage: node --experimental-websocket stylecheck.js <url> <out.json>
"use strict";
const { spawn } = require("child_process");
const fs = require("fs"), os = require("os"), path = require("path");

const URL_ = process.argv[2] || "http://127.0.0.1:8099/";
const OUT = process.argv[3] || "styles.json";
const CHROME = process.env.CHROME || "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome";
const PORT = 9300 + Math.floor(Math.random() * 500);

const sleep = ms => new Promise(r => setTimeout(r, ms));

// what runs inside the page: every element, and its ::before / ::after when
// they exist, as one string of all computed properties
const SNAP = `(function(){
  var skip = ${JSON.stringify(process.env.STILL ? [] : ["opacity", "transform", "box-shadow", "rotate"])};
  function style(el, pseudo){
    var cs = getComputedStyle(el, pseudo || null), s = "";
    if(pseudo && (cs.content === "none" || cs.content === "normal")) return null;
    for(var i = 0; i < cs.length; i++){
      var p = cs[i];
      if(skip.indexOf(p) >= 0) continue;
      s += p + ":" + cs.getPropertyValue(p) + ";";
    }
    return s;
  }
  function where(el){
    var parts = [];
    for(; el && el.nodeType === 1; el = el.parentNode){
      var i = 0, sib = el;
      while((sib = sib.previousElementSibling)) i++;
      parts.unshift(el.tagName.toLowerCase() + (el.id ? "#" + el.id : "") +
                    (el.className && el.className.baseVal === undefined ? "." + String(el.className).trim().replace(/\\s+/g, ".") : "") + "[" + i + "]");
    }
    return parts.join(" > ");
  }
  var out = {}, all = document.querySelectorAll("*");
  for(var k = 0; k < all.length; k++){
    var el = all[k], tag = el.tagName.toLowerCase();
    if(tag === "script" || tag === "style" || tag === "head" || tag === "meta" || tag === "link" || tag === "title") continue;
    var w = where(el);
    out[w] = style(el);
    var b = style(el, "::before"), a = style(el, "::after");
    if(b) out[w + " ::before"] = b;
    if(a) out[w + " ::after"] = a;
  }
  return JSON.stringify(out);
})()`;

// each step: a name, what to do in the page, how long to let it settle
const STEPS = [
  ["main", ``, 300],
  ["queue-picked", `Array.prototype.slice.call(document.querySelectorAll("[data-queue]"), 0, 3).forEach(function(b){ b.click(); });`, 500],
  ["settings", `location.hash = "#settings";`, 600],
  ["picker", `document.getElementById("c-browse").click();`, 700],
  ["picker-closed-unlock", `document.getElementById("pk-close").click(); document.getElementById("unlock").hidden = false; document.getElementById("ul-msg").textContent = "Six digits.";`, 300],
  ["dumps-empty", `document.getElementById("unlock").hidden = true; location.hash = "#dumps";`, 700],
  ["offline", `location.hash = ""; ["offline","offline-start","offline-spin"].forEach(function(i){ document.getElementById(i).hidden = false; }); document.getElementById("conn").className = "live off";`, 500],
  ["launched", `["offline","offline-start","offline-spin"].forEach(function(i){ document.getElementById(i).hidden = true; }); document.getElementById("conn").className = "live on";
                var q = document.getElementById("q-empty"); if(q) q.click();
                fetch("/api/launch", {method: "POST", headers: {"Content-Type": "application/x-www-form-urlencoded"}, body: "title=PPSA01234"});`, 9000],
  ["dumping", `var t = document.querySelector("#apps [data-dir]"); if(t) t.click();
               setTimeout(function(){ document.getElementById("btn-go").click(); }, 400);`, 1800],
  ["dumped", ``, 9000],
  ["dumps", `location.hash = "#dumps";`, 900],
  ["delete", `var d = document.querySelector("[data-del], .mini.del"); if(d) d.click();`, 500],
  ["everything", `Array.prototype.forEach.call(document.querySelectorAll("[hidden]"), function(e){ e.hidden = false; });`, 500],
];

async function main(){
  const profile = fs.mkdtempSync(path.join(os.tmpdir(), "stylecheck-"));
  const chrome = spawn(CHROME, ["--headless=new", "--disable-gpu", "--no-first-run", "--hide-scrollbars",
    "--remote-debugging-port=" + PORT, "--user-data-dir=" + profile, "about:blank"], { stdio: "ignore" });

  let target = null;
  for(let i = 0; i < 40 && !target; i++){
    await sleep(250);
    try {
      const list = await (await fetch("http://127.0.0.1:" + PORT + "/json")).json();
      target = list.find(t => t.type === "page");
    } catch(e){}
  }
  if(!target) throw new Error("Chrome did not come up");

  const ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((ok, bad) => { ws.onopen = ok; ws.onerror = bad; });
  let seq = 0; const waiting = {};
  ws.onmessage = m => {
    const d = JSON.parse(m.data);
    if(d.id && waiting[d.id]){ waiting[d.id](d); delete waiting[d.id]; }
    // what the page itself trips over is worth knowing too
    if(d.method === "Runtime.exceptionThrown")
      process.stderr.write("PAGE ERROR: " + JSON.stringify(d.params.exceptionDetails.exception || d.params.exceptionDetails).slice(0, 300) + "\n");
  };
  const send = (method, params) => new Promise((ok, bad) => {
    const id = ++seq; waiting[id] = ok;
    setTimeout(() => { if(waiting[id]){ delete waiting[id]; bad(new Error(method + " got no answer")); } }, 60000);
    ws.send(JSON.stringify({ id, method, params }));
  });
  const run = async js => {
    const r = await send("Runtime.evaluate", { expression: js, returnByValue: true });
    if(r.result && r.result.exceptionDetails) throw new Error(JSON.stringify(r.result.exceptionDetails).slice(0, 400));
    return r.result.result.value;
  };

  await send("Runtime.enable", {});
  const result = {}, strings = [], index = new Map();
  const pack = snap => {
    const o = JSON.parse(snap), packed = {};
    for(const k in o){
      if(!index.has(o[k])){ index.set(o[k], strings.length); strings.push(o[k]); }
      packed[k] = index.get(o[k]);
    }
    return packed;
  };

  for(const flexgap of [true, false]){
    for(const [w, h] of [[1280, 900], [390, 800]]){
      await send("Emulation.setDeviceMetricsOverride", { width: w, height: h, deviceScaleFactor: 1, mobile: false });
      await send("Page.navigate", { url: URL_ + "#" });
      await sleep(2500);
      await run(`try { localStorage.clear(); } catch(e){}`);
      await send("Page.navigate", { url: "about:blank" }); await sleep(200);
      await send("Page.navigate", { url: URL_ }); await sleep(3000);
      if(!flexgap) await run(`document.documentElement.className += " no-flexgap";`);

      for(const [name, js, settle] of STEPS){
        if(js) await run(js);
        await sleep(settle);
        const key = (flexgap ? "gap" : "nogap") + "/" + w + "/" + name;
        result[key] = pack(await run(SNAP));
        process.stderr.write(key + ": " + Object.keys(result[key]).length + " elements\n");
      }
      // let the pretend dump be forgotten before the next round
      await run(`var r = document.getElementById("btn-reload"); if(r) r.click();`);
    }
  }

  fs.writeFileSync(OUT, JSON.stringify({ strings, result }));
  ws.close(); chrome.kill("SIGKILL");
  await sleep(500);
  fs.rmSync(profile, { recursive: true, force: true });
}

main().then(() => process.exit(0), e => { console.error(e); process.exit(1); });
