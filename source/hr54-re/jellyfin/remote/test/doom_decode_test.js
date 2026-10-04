/* Verifies the receiver's base64/HRDF decoder in app.js against a
   reference decode.  The DOOM frame decoder is pure string and array
   arithmetic, so it can be lifted straight out of app.js and run here
   with a stub canvas.

   This is the regression test for the decoder bugs found while wiring the
   canvas up, all of which failed silently by leaving the overlay blank:
     - the magic was compared against the base64 text instead of the
       decoded bytes, so every frame was rejected;
     - the palette was read as 768 RGB bytes when the header is 1044
       bytes, i.e. 256 RGBA entries;
     - a C-style integer division leaked into the group offset;
     - base64 '=' padding decoded as -1 and poisoned the final group.

   The pixel count is also varied so all three cases of w*h modulo three
   are exercised, since the trailing partial base64 group is padded. */
var fs = require("fs");
var path = require("path");

var src = fs.readFileSync(
  path.join(__dirname, "..", "static", "tv", "app.js"), "utf8");
var start = src.indexOf("function doomBase64Table");
var end = src.indexOf("/* Pull the newest frame.");
if (start < 0 || end < 0) {
  console.error("could not locate decoder in app.js");
  process.exit(1);
}

var fails = 0;
function check(name, ok, detail) {
  if (ok) { console.log("ok   - " + name); return; }
  fails++;
  console.log("FAIL - " + name + (detail ? ": " + detail : ""));
}

/* Fresh decoder state per render, so g.seq never masks a later frame. */
function newDecoder() {
  var g = {};
  var apply = new Function(
    "g", src.slice(start, end) + "\nreturn doomApply;")(g);
  var ctx = {
    createImageData: function (w, h) {
      return { width: w, height: h, data: new Array(w * h * 4) };
    },
    putImageData: function (img) { this.last = img; }
  };
  g.cv = { width: 0, height: 0, getContext: function () { return ctx; } };
  g.ctx = ctx;
  return { g: g, apply: apply, ctx: ctx };
}

/* Build a frame exactly as hr54_doom.c does: 20-byte header, then
   256 RGBA palette entries (1024 bytes), then w*h palette indices. */
function makeFrame(w, h, seq) {
  var buf = Buffer.alloc(1044 + w * h);
  buf.write("HRDF", 0, "ascii");
  buf.writeUInt32LE(1, 4);
  buf.writeUInt32LE(w, 8);
  buf.writeUInt32LE(h, 12);
  buf.writeUInt32LE(seq, 16);
  for (var i = 0; i < 256; i++) {
    buf[20 + i * 4 + 0] = i;
    buf[20 + i * 4 + 1] = (255 - i) & 255;
    buf[20 + i * 4 + 2] = (i * 7) & 255;
    buf[20 + i * 4 + 3] = 255;
  }
  for (var p = 0; p < w * h; p++) {
    buf[1044 + p] = (p * 3 + ((p / w) | 0)) & 255;
  }
  return buf;
}

function render(w, h, seq) {
  var d = newDecoder();
  d.apply(makeFrame(w, h, seq).toString("base64"));
  return d;
}

function verifyPixels(label, w, h, seq) {
  var d = render(w, h, seq);
  if (d.g.seq !== seq) {
    check(label + " frame accepted", false, "g.seq=" + d.g.seq);
    return;
  }
  if (!d.ctx.last) {
    check(label + " canvas drawn", false, "putImageData never called");
    return;
  }
  var ref = makeFrame(w, h, seq);
  var out = d.ctx.last.data;
  var bad = -1;
  for (var p = 0; p < w * h; p++) {
    var idx = ref[1044 + p], o = p * 4, e = 20 + idx * 4;
    if (out[o] !== ref[e] || out[o + 1] !== ref[e + 1] ||
        out[o + 2] !== ref[e + 2] || out[o + 3] !== ref[e + 3]) {
      bad = p; break;
    }
  }
  check(label + " all " + (w * h) + " pixels match reference", bad < 0,
        bad < 0 ? "" : "first bad pixel " + bad + " of " + (w * h));
}

/* 64000 is 1 mod 3, 64320 is 0 mod 3, 64640 is 2 mod 3. */
verifyPixels("320x200 (1 mod 3)", 320, 200, 7000);
verifyPixels("320x201 (0 mod 3)", 320, 201, 7001);
verifyPixels("320x202 (2 mod 3)", 320, 202, 7002);

/* A repeated sequence must be ignored so 304 pacing can settle. */
var d = render(320, 200, 8000);
d.ctx.last = null;
d.apply(makeFrame(320, 200, 8000).toString("base64"));
check("duplicate seq ignored", d.ctx.last === null, "redrew same frame");

/* A newer sequence must be drawn again. */
d.ctx.last = null;
d.apply(makeFrame(320, 200, 8001).toString("base64"));
check("advanced seq redrawn", d.ctx.last !== null && d.g.seq === 8001,
      "seq=" + d.g.seq);

/* Garbage must be rejected without throwing and without drawing. */
d.ctx.last = null;
var threw = false;
try {
  d.apply("");
  d.apply("not base64 at all!!");
  d.apply(Buffer.from("XXXX" + makeFrame(320, 200, 9).slice(4)).toString("base64"));
  d.apply(makeFrame(320, 200, 9).toString("base64").slice(0, 100));
  d.apply("A".repeat(90000));
} catch (e) {
  threw = true;
  console.log("  threw: " + e);
}
check("garbage rejected without throwing", !threw);

/* A truncated but well-formed prefix must not be drawn. */
var full = makeFrame(320, 200, 8500).toString("base64");
d.g.seq = -1;
d.ctx.last = null;
d.apply(full.slice(0, full.length - 40));
check("truncated frame not drawn", d.ctx.last === null, "drew a partial frame");

console.log(fails ? "FAILURES: " + fails : "all decoder checks passed");
process.exit(fails ? 1 : 0);
