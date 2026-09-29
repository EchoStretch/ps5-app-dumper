// Compares two files written by stylecheck.js and names what differs.
//   usage: node stylediff.js <before.json> <after.json>
"use strict";
const fs = require("fs");
const A = JSON.parse(fs.readFileSync(process.argv[2])), B = JSON.parse(fs.readFileSync(process.argv[3]));
const props = s => { const o = {}; (s || "").split(";").forEach(kv => { const i = kv.indexOf(":"); if(i > 0) o[kv.slice(0, i)] = kv.slice(i + 1); }); return o; };
let differing = 0, shown = 0;
for(const step of Object.keys(A.result)){
  const a = A.result[step], b = B.result[step] || {};
  const keys = new Set(Object.keys(a).concat(Object.keys(b)));
  let here = 0;
  for(const k of keys){
    const sa = a[k] === undefined ? null : A.strings[a[k]], sb = b[k] === undefined ? null : B.strings[b[k]];
    if(sa === sb) continue;
    here++; differing++;
    if(shown >= 40) continue;
    shown++;
    if(sa === null || sb === null){ console.log(step + "  " + k + "  " + (sa === null ? "only after" : "only before")); continue; }
    const pa = props(sa), pb = props(sb), d = [];
    for(const p of new Set(Object.keys(pa).concat(Object.keys(pb)))) if(pa[p] !== pb[p]) d.push(p + ": " + pa[p] + " -> " + pb[p]);
    console.log(step + "  " + k + "\n    " + d.slice(0, 6).join("\n    "));
  }
  console.log(step + ": " + keys.size + " elements, " + here + " differ");
}
console.log(differing ? differing + " DIFFERENCES" : "IDENTICAL");
process.exit(differing ? 1 : 0);
