/* ══════════════════════════════════════════════════════════════════════════
   TheatreTex · 一號影廳質感工房 (Canvas 2D, 零外部依賴)
   ──────────────────────────────────────────────────────────────────────────
   牆肋 / 地毯 / 石地 / 絲絨幕 / 幕楣 / 座位銅牌 / 海報 / 招牌 / 預留卡 / 戲飛
   全部即場用 Canvas 2D 畫出嚟, 唔使 load 任何圖檔。

   規矩:
   · 每個 function 一次 call 畫一次, 同步 (loadThumb / ready 除外)。
   · 永遠唔會 throw: web font 未到 (每個 stack 都有本機 fallback), 或者
     俾啲古怪 input (空 title, undefined 欄位, 唔識嘅座位 code) 都照樣出圖;
     最差情況退返一張底色 canvas。
   · 顏色只用 spec 個 palette, 或者 palette 之間嘅混色。無 emoji。
   · 文字一定 set textBaseline, 一定量度 + 縮細至啱位; CJK 逐字斷行,
     拉丁字按 word 斷行; 拉丁 small caps + tracking 係人手逐字排 (唔靠
     瀏覽器新 API), 所以邊度都一樣。

   對外介面 (window.TheatreTex):
     ribs()      → 1024×1024  牆肋 (橫向可 tile)
     carpet()    → 512×512    地毯噪點 (可 tile)
     stone()     → 1024×1024  方格石地 = 4.0 m × 4.0 m (可 tile)
     curtain()   → 1024×2048  絲絨幕 (有 alpha; 右邊 = 前緣鋸齒, 底邊鋸齒)
     pelmet()    → 2048×256   幕楣 (5 個扇形 + 銅穗線)
     plateAtlas(codes) → {canvas 2048×1024, uv(code) → [u0,v0,u1,v1]}
                  v=0 係 canvas 頂 (caller 自己 flip); 唔識嘅 code → 空牌
     marquee(text)     → 2048×192  招牌 (Latin small caps + CJK 分 run)
     poster(video, img) → 700×1000 海報 (有圖 / 無圖 title card)
     titleCard(video)  → 700×1000 (= poster 無圖)
     reservedCard()    → 480×320  「RESERVED · 預留」象牙卡
     ticketStub(fields) → 680×400 戲飛 {venue, film, time, seat, no}
     loadThumb(url, timeoutMs=4000) → Promise<Image|null> (永遠唔 reject)
     ready(timeoutMs=3000) → Promise<boolean>  web font 到齊未 (永遠唔 reject)
     starField(n, seed) → {positions: Float32Array(n*3), twinkle: Uint8Array(n)}
   ══════════════════════════════════════════════════════════════════════════ */
(() => {
"use strict";

// ── 調色板 (全部嚟自 spec) ─────────────────────────────────────────────────
const P = {
  bg: "#050506", wall: "#2B2B34", floor: "#0C0D11",
  ribHi: "#3B3B47", ribLo: "#17171C",
  stoneDark: "#0B0A0C", stoneLight: "#E0D6C4",
  velvetHi: "#5B1A22", velvetLo: "#3A0F14",
  brass: "#B08D57", brassDim: "#6E5636", walnut: "#3A2A1E",
  marqueeBg: "#0E0E11", ivory: "#F3EBDD", ink: "#14151A",
  textDim: "#9A928A", inkDim: "#6E665E",
  washTop: "#14151A", washBot: "#3A3126", burgundy: "#6A1E2A",
};

// ── 字體 stack (每個都有本機 fallback, web font 未到照樣有字) ──────────────
const FONT = {
  display: '"Cormorant Garamond","Times New Roman","Songti TC","PingFang HK",serif',
  cjk: '"Noto Sans HK",-apple-system,"PingFang HK","Microsoft JhengHei","Noto Sans CJK TC",sans-serif',
  mono: '"IBM Plex Mono",ui-monospace,"SF Mono",Menlo,monospace',
  plate: '"Cormorant Garamond",Georgia,serif',
};
const font = (w, px, fam) => w + " " + px + "px " + fam;

// ── 細工具 ─────────────────────────────────────────────────────────────────
const clamp = (v, a, b) => v < a ? a : v > b ? b : v;
function hexRgb(h){ const v = parseInt(h.slice(1), 16); return [v >> 16 & 255, v >> 8 & 255, v & 255]; }
function rgba(h, a){ const c = hexRgb(h); return "rgba(" + c[0] + "," + c[1] + "," + c[2] + "," + a + ")"; }
// 兩個 palette 色之間混色 → "rgb(r,g,b)"
function mix(a, b, t){
  const A = hexRgb(a), B = hexRgb(b);
  return "rgb(" + Math.round(A[0] + (B[0] - A[0]) * t) + "," + Math.round(A[1] + (B[1] - A[1]) * t) + "," + Math.round(A[2] + (B[2] - A[2]) * t) + ")";
}
// 有種子嘅 RNG (同 spec 一樣用 mulberry32), 質感每次生成都一樣
function mulberry32(seed){
  let a = (seed >>> 0) || 1;
  return function(){
    a = (a + 0x6D2B79F5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}
function mk(w, h){
  const c = document.createElement("canvas");
  c.width = w; c.height = h;
  const g = c.getContext("2d");
  if (!g) throw new Error("no 2d context");
  return [c, g];
}
// 最差情況: 一張純色 canvas (連 canvas 都整唔到就 null)
function fallback(w, h, fill){
  try { const [c, g] = mk(w, h); g.fillStyle = fill; g.fillRect(0, 0, w, h); return c; }
  catch (e) { return null; }
}
function warn(name, e){ try { console.warn("[TheatreTex] " + name + " fallback:", e); } catch (_) {} }
// 包住每個 public function: 任何 error 都食咗佢, 退返底色
function guard(name, fn, w, h, fill){
  return function(){
    try { return fn.apply(null, arguments); }
    catch (e) { warn(name, e); return fallback(w, h, fill); }
  };
}

// ── 文字引擎 ───────────────────────────────────────────────────────────────
const CJK_RE = /[⺀-⿟　-〿぀-ヿ㄀-ㄯ㐀-䶿一-鿿豈-﫿︰-﹏＀-￯]/;
const str = s => (s == null ? "" : String(s));
const chars = s => Array.from(str(s));
const isCJK = ch => CJK_RE.test(ch);
// 拆做 Latin / CJK run, 每個 run 各自用自己嘅字體
function splitRuns(s){
  const out = [];
  for (const ch of chars(s)) {
    const c = isCJK(ch), last = out[out.length - 1];
    if (last && last.cjk === c) last.text += ch; else out.push({ cjk: c, text: ch });
  }
  return out;
}
// 逐隻字排版: letter-spacing (em) + 人手 small caps (細楷 → 大寫 0.74 倍)
function layoutRun(g, text, px, weight, fam, trackEm, smallCaps){
  const glyphs = []; const tr = trackEm * px; let w = 0;
  for (const ch of chars(text)) {
    let draw = ch, f = font(weight, px, fam);
    if (smallCaps && ch !== ch.toUpperCase() && ch === ch.toLowerCase()) {
      draw = ch.toUpperCase(); f = font(weight, Math.round(px * 0.74), fam);
    }
    g.font = f;
    const adv = g.measureText(draw).width;
    glyphs.push({ ch: draw, font: f, adv });
    w += adv + tr;
  }
  if (glyphs.length) w -= tr;
  return { glyphs, width: Math.max(0, w), track: tr };
}
function drawRun(g, run, x, y){
  for (const gl of run.glyphs) { g.font = gl.font; g.fillText(gl.ch, x, y); x += gl.adv + run.track; }
  return x;
}
// 混排一行: Latin run 用 latin 設定, CJK run 用 cjk 設定
//   spec = {px, latin:{weight,fam,track,smallCaps,scale}, cjk:{weight,fam,track,scale}}
function layoutMixed(g, text, spec){
  const runs = splitRuns(text); let w = 0;
  const L = runs.map(r => {
    const s = r.cjk ? spec.cjk : spec.latin;
    const px = Math.max(6, Math.round(spec.px * (s.scale || 1)));
    const l = layoutRun(g, r.text, px, s.weight, s.fam, s.track || 0, !!s.smallCaps);
    l.cjk = r.cjk; l.px = px; w += l.width; return l;
  });
  return { runs: L, width: w };
}
function drawMixed(g, lay, x, y, align){
  if (align === "center") x -= lay.width / 2; else if (align === "right") x -= lay.width;
  for (const l of lay.runs) x = drawRun(g, l, x, y);
  return x;
}
// 縮細到啱位: 由 px 開始, 直到 width ≤ maxW (最細 minPx)
function fitMixed(g, text, spec, maxW, minPx){
  let lay = layoutMixed(g, text, spec);
  if (lay.width > maxW && lay.width > 0) {
    spec.px = Math.max(minPx, Math.floor(spec.px * maxW / lay.width));
    lay = layoutMixed(g, text, spec);
    while (lay.width > maxW && spec.px > minPx) { spec.px -= 1; lay = layoutMixed(g, text, spec); }
  }
  return lay;
}
// 實際墨跡 (actualBoundingBox) 嘅上下, 用嚟真正對中; 舊瀏覽器冇就估
function inkMetrics(g, f, sample, px){
  g.font = f;
  const m = g.measureText(sample);
  const a = m.actualBoundingBoxAscent, d = m.actualBoundingBoxDescent;
  return (a >= 0 && d >= 0 && isFinite(a + d) && a + d > 0) ? [a, d] : [px * 0.7, px * 0.1];
}
// 混排行嘅 baseline: 令 Latin 大楷 + CJK 墨跡嘅聯集喺 cy 對中
function mixedBaseline(g, lay, spec, cy){
  let asc = 0, desc = 0;
  for (const l of lay.runs) {
    const s = l.cjk ? spec.cjk : spec.latin;
    const [a, d] = inkMetrics(g, font(s.weight, l.px, s.fam), l.cjk ? "國" : "H", l.px);
    asc = Math.max(asc, a); desc = Math.max(desc, d);
  }
  return cy + (asc - desc) / 2;
}
// 標題斷行: CJK 逐字可斷, 拉丁字按 word 斷; 超過 maxLines 尾行加 …
function wrapText(g, text, f, maxW, maxLines){
  g.font = f;
  const W = s => g.measureText(s).width;
  const toks = []; let cur = "";
  for (const ch of chars(text)) {
    if (isCJK(ch)) { if (cur) { toks.push(cur); cur = ""; } toks.push(ch); }
    else if (ch === " " || ch === "\n") { if (cur) { toks.push(cur); cur = ""; } toks.push(ch); }
    else cur += ch;
  }
  if (cur) toks.push(cur);
  const lines = []; let line = "";
  const push = () => { lines.push(line.replace(/\s+$/, "")); line = ""; };
  for (let t of toks) {
    if (t === "\n") { push(); continue; }
    if (t === " " && !line) continue;
    if (W(line + t) <= maxW) { line += t; continue; }
    if (line) push();
    if (t === " ") continue;
    // 單個 token 都過闊 (超長英文字): 逐字斬
    while (W(t) > maxW) {
      const k = chars(t); let n = k.length;
      while (n > 1 && W(k.slice(0, n).join("")) > maxW) n--;
      if (n >= k.length) break;
      lines.push(k.slice(0, n).join("")); t = k.slice(n).join("");
    }
    line = t;
  }
  if (line) push();
  if (lines.length > maxLines) {
    lines.length = maxLines;
    const k = chars(lines[maxLines - 1]);
    while (k.length && W(k.join("") + "…") > maxW) k.pop();
    lines[maxLines - 1] = k.join("") + "…";
  }
  return lines;
}
// 觀看次數 → 萬 / 億 (12.4萬, 1.2億, 8,532)
function fmtViews(v){
  const n = Number(v);
  if (!isFinite(n) || n < 0) return null;
  const one = x => { const s = x.toFixed(1); return s.slice(-2) === ".0" ? s.slice(0, -2) : s; };
  if (n >= 1e8) return one(n / 1e8) + "億";
  if (n >= 1e4) return one(n / 1e4) + "萬";
  return String(Math.round(n)).replace(/\B(?=(\d{3})+(?!\d))/g, ",");
}
// 片長: 數字 (秒) → m:ss / h:mm:ss; 字串照用
function fmtDuration(d){
  if (d == null || d === "") return "";
  if (typeof d === "number" && isFinite(d)) {
    const s = Math.max(0, Math.round(d)), h = Math.floor(s / 3600), m = Math.floor(s % 3600 / 60), ss = s % 60;
    const two = x => (x < 10 ? "0" : "") + x;
    return h ? h + ":" + two(m) + ":" + two(ss) : m + ":" + two(ss);
  }
  return str(d);
}

// ── 共用嘅面料 ─────────────────────────────────────────────────────────────
// 紙紋: 128² 暖色噪點 (核桃色 / 米白交錯), alpha 平均 ≈ 2%
let grainCanvas = null;
function grainPattern(g){
  if (!grainCanvas) {
    const [c, x] = mk(128, 128);
    const id = x.createImageData(128, 128), d = id.data, R = mulberry32(7);
    for (let i = 0; i < d.length; i += 4) {
      const warm = R() < 0.5;
      d[i] = warm ? 58 : 243; d[i + 1] = warm ? 42 : 235; d[i + 2] = warm ? 30 : 221;
      d[i + 3] = Math.floor(R() * 11);
    }
    x.putImageData(id, 0, 0);
    grainCanvas = c;
  }
  return g.createPattern(grainCanvas, "repeat");
}
function grain(g, w, h, alpha){
  g.save(); g.globalAlpha = alpha == null ? 1 : alpha;
  g.fillStyle = grainPattern(g); g.fillRect(0, 0, w, h); g.restore();
}
// 暗角: 中間透明, 邊位 aMid, 角位 aCorner
function vignette(g, w, h, aMid, aCorner){
  const r = g.createRadialGradient(w / 2, h / 2, Math.min(w, h) * 0.3, w / 2, h / 2, Math.hypot(w, h) / 2);
  r.addColorStop(0, rgba(P.bg, 0)); r.addColorStop(0.62, rgba(P.bg, aMid)); r.addColorStop(1, rgba(P.bg, aCorner));
  g.fillStyle = r; g.fillRect(0, 0, w, h);
}
// 紙面: 象牙底 + 紙紋 + 邊位暖暗 2%
function paper(g, w, h){
  g.fillStyle = P.ivory; g.fillRect(0, 0, w, h);
  grain(g, w, h, 1);
  const r = g.createRadialGradient(w / 2, h / 2, Math.min(w, h) * 0.35, w / 2, h / 2, Math.hypot(w, h) / 2);
  r.addColorStop(0, rgba(P.walnut, 0)); r.addColorStop(0.7, rgba(P.walnut, 0.02)); r.addColorStop(1, rgba(P.walnut, 0.06));
  g.fillStyle = r; g.fillRect(0, 0, w, h);
}
// 銅雙線框: 1 px + gap + 1 px, 離邊 inset
function doubleHairline(g, w, h, inset, gap, alpha){
  g.strokeStyle = rgba(P.brass, alpha == null ? 0.9 : alpha); g.lineWidth = 1;
  g.strokeRect(inset + 0.5, inset + 0.5, w - 2 * inset - 1, h - 2 * inset - 1);
  const i2 = inset + 1 + gap;
  g.strokeRect(i2 + 0.5, i2 + 0.5, w - 2 * i2 - 1, h - 2 * i2 - 1);
}
// 拉絲: 橫向 1 px 幼線, alpha ≤ 20% (畫喺 g 現時嘅 composite mode 之下)
function brushLines(g, w, h, seed, step){
  const R = mulberry32(seed);
  for (let y = 0; y < h; y += step) {
    const seg = 1 + Math.floor(R() * 3);
    for (let s = 0; s < seg; s++) {
      const x0 = R() * w, len = w * (0.15 + R() * 0.6);
      g.fillStyle = R() < 0.5 ? rgba(P.ivory, R() * 0.2) : rgba(P.brassDim, R() * 0.2);
      g.fillRect(x0, y, len, 1);
      if (x0 + len > w) g.fillRect(0, y, x0 + len - w, 1);   // 橫向接得埋
    }
  }
}
// 可以 tile 嘅 value noise (格點取模), octaves 個 octave, 回傳 size² Float32 (0..1)
function valueNoise(size, seed, octaves){
  const R = mulberry32(seed), out = new Float32Array(size * size);
  const sm = t => t * t * (3 - 2 * t);
  let amp = 1, total = 0, cells = 8;
  for (let o = 0; o < octaves; o++) {
    const lat = new Float32Array(cells * cells);
    for (let i = 0; i < lat.length; i++) lat[i] = R();
    const step = size / cells;
    for (let y = 0; y < size; y++) {
      const fy = y / step, y0 = Math.floor(fy) % cells, ty = sm(fy - Math.floor(fy)), y1 = (y0 + 1) % cells;
      for (let x = 0; x < size; x++) {
        const fx = x / step, x0 = Math.floor(fx) % cells, tx = sm(fx - Math.floor(fx)), x1 = (x0 + 1) % cells;
        const a = lat[y0 * cells + x0], b = lat[y0 * cells + x1], c = lat[y1 * cells + x0], d = lat[y1 * cells + x1];
        const top = a + (b - a) * tx, bot = c + (d - c) * tx;
        out[y * size + x] += amp * (top + (bot - top) * ty);
      }
    }
    total += amp; amp *= 0.5; cells *= 2;
  }
  for (let i = 0; i < out.length; i++) out[i] /= total;
  return out;
}

// ── 牆肋 1024×1024 ─────────────────────────────────────────────────────────
// 64 條直肋, 6 cm pitch (3.84 m 闊 → 16 px 一條); 橫向可以 tile
function ribs(){
  const W = 1024, H = 1024, N = 64, pitch = W / N;
  const [c, g] = mk(W, H);
  g.fillStyle = P.wall; g.fillRect(0, 0, W, H);
  for (let i = 0; i < N; i++) {
    const x = i * pitch;
    // 肋身: 坑底陰影 → 底色 → 向光邊慢慢升 (圓肋, 唔係平間條)
    const gr = g.createLinearGradient(x, 0, x + pitch - 3, 0);
    gr.addColorStop(0, mix(P.wall, P.ribLo, 0.45));
    gr.addColorStop(0.3, P.wall);
    gr.addColorStop(1, mix(P.wall, P.ribHi, 0.6));
    g.fillStyle = gr; g.fillRect(x, 0, pitch - 3, H);
    g.fillStyle = P.ribHi; g.fillRect(x + pitch - 3, 0, 1, H);   // 1 px 高光邊
    g.fillStyle = P.ribLo; g.fillRect(x + pitch - 2, 0, 2, H);   // 2 px 坑
  }
  // 灰泥質感: 好淡嘅直紋 (alpha ≤ 6%), 上下接得埋
  const R = mulberry32(11);
  for (let k = 0; k < 320; k++) {
    const x = Math.floor(R() * W), y = Math.floor(R() * H), len = 80 + R() * 520;
    g.fillStyle = R() < 0.5 ? rgba(P.ribHi, 0.05) : rgba(P.ribLo, 0.06);
    g.fillRect(x, y, 1, len);
    if (y + len > H) g.fillRect(x, 0, 1, y + len - H);
  }
  return c;
}

// ── 地毯 512×512 ───────────────────────────────────────────────────────────
// #0C0D11 ± 6% 灰階噪點: 一半逐像素, 一半 2×2 粗粒 (絨頭顆粒感); 白噪本身可 tile
function carpet(){
  const S = 512, [c, g] = mk(S, S);
  const id = g.createImageData(S, S), d = id.data, R = mulberry32(3);
  const base = hexRgb(P.floor), half = S >> 1;
  const blk = new Float32Array(half * half);
  for (let i = 0; i < blk.length; i++) blk[i] = R() * 2 - 1;
  for (let y = 0; y < S; y++) {
    const row = (y >> 1) * half;
    for (let x = 0; x < S; x++) {
      const n = 0.5 * (R() * 2 - 1) + 0.5 * blk[row + (x >> 1)];
      const k = 1 + n * 0.06, i = (y * S + x) * 4;
      d[i] = Math.round(base[0] * k); d[i + 1] = Math.round(base[1] * k); d[i + 2] = Math.round(base[2] * k); d[i + 3] = 255;
    }
  }
  g.putImageData(id, 0, 0);
  return c;
}

// ── 方格石地 1024×1024 = 4.0 m × 4.0 m ─────────────────────────────────────
// 6 × 6 格 (0.667 m 一格; 0.6 m 除唔盡 4.0 m, 要 tileable 就取 6),
// #0B0A0C / #E0D6C4 相間, 3-octave 紋理 8%, 每塊有 ±2% 明暗差, 1 px 石縫
function stone(){
  const S = 1024, N = 6, T = S / N, [c, g] = mk(S, S);
  const R = mulberry32(5);
  for (let j = 0; j < N; j++) for (let i = 0; i < N; i++) {
    const dark = (i + j) & 1;
    g.fillStyle = dark ? mix(P.stoneDark, P.stoneLight, 0.004 + R() * 0.03) : mix(P.stoneLight, P.stoneDark, 0.005 + R() * 0.04);
    g.fillRect(Math.round(i * T), Math.round(j * T), Math.ceil(T), Math.ceil(T));
  }
  // 大理石紋: 256² 低解像度 → 放大 4× (自動平滑), 用對面格嘅顏色以 8% 畫出
  const L = 256, n3 = valueNoise(L, 9, 3), n1 = valueNoise(L, 13, 1);
  const [vc, vg] = mk(L, L), id = vg.createImageData(L, L), d = id.data;
  const dk = hexRgb(P.stoneDark), lt = hexRgb(P.stoneLight), TL = T / (S / L);
  for (let y = 0; y < L; y++) for (let x = 0; x < L; x++) {
    const k = y * L + x, i = k * 4;
    const tile = ((Math.floor(x / TL) + Math.floor(y / TL)) & 1) === 1;   // true = 淺色格
    const ridge = Math.pow(1 - Math.abs(2 * n3[k] - 1), 5);              // 幼細紋路
    const cloud = n1[k] * 0.5;                                             // 大片雲狀
    const col = tile ? dk : lt;
    d[i] = col[0]; d[i + 1] = col[1]; d[i + 2] = col[2];
    d[i + 3] = Math.round(255 * (0.08 * ridge + 0.03 * cloud));
  }
  vg.putImageData(id, 0, 0);
  g.imageSmoothingEnabled = true; g.drawImage(vc, 0, 0, S, S);
  // 石縫: 1 px, 淺色格先睇得出
  g.fillStyle = rgba(P.stoneDark, 0.35);
  for (let i = 0; i < N; i++) { const p = Math.round(i * T); g.fillRect(p, 0, 1, S); g.fillRect(0, p, S, 1); }
  return c;
}

// ── 絲絨 (幕 + 幕楣共用) ───────────────────────────────────────────────────
// 16 px 一摺: 摺面係 cos 斜坡 (峰位收窄, 峰後一絲陰影), 做成 pattern 鋪滿
function velvetPattern(g){
  const PIT = 16, [c, x] = mk(PIT, 1), id = x.createImageData(PIT, 1), d = id.data;
  const hi = hexRgb(P.velvetHi), lo = hexRgb(P.velvetLo);
  for (let i = 0; i < PIT; i++) {
    const u = (i + 0.5) / PIT;
    const s = Math.pow(0.5 - 0.5 * Math.cos(2 * Math.PI * u), 1.35);
    const shade = 1 - 0.12 * Math.exp(-Math.pow((u - 0.8) / 0.1, 2));
    for (let k = 0; k < 3; k++) d[i * 4 + k] = Math.round((lo[k] + (hi[k] - lo[k]) * s) * shade);
    d[i * 4 + 3] = 255;
  }
  x.putImageData(id, 0, 0);
  return g.createPattern(c, "repeat");
}
function velvetBase(g, W, H, seed){
  g.fillStyle = velvetPattern(g); g.fillRect(0, 0, W, H);
  const R = mulberry32(seed);
  // 絨毛: 直向細紋
  const n = Math.round(W * H / 4000);
  for (let k = 0; k < n; k++) {
    const x = Math.floor(R() * W), y = Math.floor(R() * H), len = 40 + R() * Math.min(600, H * 0.5);
    g.fillStyle = R() < 0.5 ? rgba(P.velvetHi, 0.06) : rgba(P.velvetLo, 0.07);
    g.fillRect(x, y, 1, len);
  }
  // 壓痕: 幾條闊而淡嘅橫帶 (絲絨反光唔均勻)
  for (let k = 0; k < 6; k++) {
    const y = R() * H, h = 40 + R() * Math.min(240, H * 0.5), col = R() < 0.5 ? P.velvetHi : P.velvetLo;
    const gr = g.createLinearGradient(0, y, 0, y + h);
    gr.addColorStop(0, rgba(col, 0)); gr.addColorStop(0.5, rgba(col, 0.09)); gr.addColorStop(1, rgba(col, 0));
    g.fillStyle = gr; g.fillRect(0, y, W, h);
  }
}
// 鋸齒邊: 先畫一條暗色縫線, 再用 destination-out 剪走三角 (透明缺口)
function hemZigzag(g, W, H, pitch, depth, right, bottom){
  const seam = () => { g.strokeStyle = rgba(P.velvetLo, 0.8); g.lineWidth = 5; g.lineJoin = "round"; g.stroke(); };
  if (right) {
    g.beginPath(); g.moveTo(W, 0);
    for (let y = 0; y < H; y += pitch) { g.lineTo(W - depth, y + pitch / 2); g.lineTo(W, y + pitch); }
    seam();
    g.lineTo(W + 2, H + 2); g.lineTo(W + 2, -2); g.closePath();
    g.globalCompositeOperation = "destination-out"; g.fill(); g.globalCompositeOperation = "source-over";
  }
  if (bottom) {
    g.beginPath(); g.moveTo(0, H);
    for (let x = 0; x < W; x += pitch) { g.lineTo(x + pitch / 2, H - depth); g.lineTo(x + pitch, H); }
    seam();
    g.lineTo(W + 2, H + 2); g.lineTo(-2, H + 2); g.closePath();
    g.globalCompositeOperation = "destination-out"; g.fill(); g.globalCompositeOperation = "source-over";
  }
}

// ── 絲絨幕 1024×2048 (有 alpha) ────────────────────────────────────────────
// 64 摺; 下面 24% 漸漸光 +18% (腳燈); 右邊 = 前緣鋸齒 (caller 另一半鏡像), 底邊鋸齒
function curtain(){
  const W = 1024, H = 2048, [c, g] = mk(W, H);
  velvetBase(g, W, H, 21);
  g.globalCompositeOperation = "lighter";    // 加法: 保持絲絨色相, 淨係光啲
  const gr = g.createLinearGradient(0, H * 0.76, 0, H);
  gr.addColorStop(0, rgba(P.velvetHi, 0)); gr.addColorStop(1, rgba(P.velvetHi, 0.18));
  g.fillStyle = gr; g.fillRect(0, H * 0.76, W, H * 0.24);
  g.globalCompositeOperation = "source-over";
  hemZigzag(g, W, H, 32, 18, true, true);
  return c;
}

// ── 幕楣 2048×256 (有 alpha) ───────────────────────────────────────────────
// 同一款絲絨, 底邊剪 5 個扇形, 沿弧邊一條 2 cm 銅穗線 + 垂落嘅穗
function pelmet(){
  const W = 2048, H = 256, N = 5, SW = W / N, DEPTH = 72, [c, g] = mk(W, H);
  velvetBase(g, W, H, 33);
  const arcs = () => {
    g.beginPath();
    for (let k = 0; k < N; k++) { g.moveTo(k * SW, H); g.ellipse((k + 0.5) * SW, H, SW / 2, DEPTH, 0, Math.PI, 2 * Math.PI); }
  };
  // 扇形陰影 (弧邊之上 12 px 漸暗, 令布邊有厚度)
  arcs(); g.strokeStyle = rgba(P.velvetLo, 0.6); g.lineWidth = 26; g.stroke();
  // 剪走扇形
  arcs(); g.closePath(); g.globalCompositeOperation = "destination-out"; g.fill();
  // 銅穗線: 只畫喺仲有布嘅地方 (source-atop), 線嘅一半喺布內 ≈ 7 px
  g.globalCompositeOperation = "source-atop";
  arcs(); g.strokeStyle = P.brass; g.lineWidth = 14; g.stroke();
  arcs(); g.strokeStyle = mix(P.brass, P.ivory, 0.35); g.lineWidth = 3; g.stroke();
  g.globalCompositeOperation = "source-over";
  // 垂穗: 沿弧邊每 5 px 一條幼銅線向下 14 px (落喺透明區)
  g.strokeStyle = rgba(P.brass, 0.75); g.lineWidth = 1; g.beginPath();
  for (let k = 0; k < N; k++) {
    const cx = (k + 0.5) * SW, rx = SW / 2;
    for (let x = Math.ceil(k * SW) + 2; x < (k + 1) * SW - 2; x += 5) {
      const dx = (x - cx) / rx, y = H - DEPTH * Math.sqrt(Math.max(0, 1 - dx * dx));
      g.moveTo(x + 0.5, y - 1); g.lineTo(x + 0.5, Math.min(H, y + 14));
    }
  }
  g.stroke();
  return c;
}

// ── 座位銅牌 atlas 2048×1024 ───────────────────────────────────────────────
// 8 欄 × 8 行, 每格 256×128 (= 6×3 cm 牌); uv(code) → [u0,v0,u1,v1],
// v=0 喺 canvas 頂 (WebGL 一般要 flip, caller 自己搞); 唔識嘅 code → 空牌
function plateAtlas(codes){
  const COLS = 8, ROWS = 8, CW = 256, CH = 128, W = COLS * CW, H = ROWS * CH;
  const list = Array.isArray(codes) ? codes.slice(0, COLS * ROWS).map(s => str(s).trim()) : [];
  const [c, g] = mk(W, H);
  // 1. 每格: 橫向拉絲漸變 (光 / 暗帶)
  for (let k = 0; k < COLS * ROWS; k++) {
    const x = (k % COLS) * CW, y = Math.floor(k / COLS) * CH;
    const gr = g.createLinearGradient(x, 0, x + CW, 0);
    gr.addColorStop(0, mix(P.brass, P.brassDim, 0.25));
    gr.addColorStop(0.28, mix(P.brass, P.ivory, 0.2));
    gr.addColorStop(0.5, P.brass);
    gr.addColorStop(0.72, mix(P.brass, P.brassDim, 0.22));
    gr.addColorStop(1, mix(P.brass, P.ivory, 0.1));
    g.fillStyle = gr; g.fillRect(x, y, CW, CH);
  }
  // 2. 拉絲: 全張畫橫線 (≤ 20% alpha)
  brushLines(g, W, H, 77, 1);
  // 3. 每格: 上光下暗 + 2 px 斜邊 + 刻字
  const map = Object.create(null);
  g.textAlign = "center"; g.textBaseline = "alphabetic";
  for (let k = 0; k < COLS * ROWS; k++) {
    const x = (k % COLS) * CW, y = Math.floor(k / COLS) * CH;
    const vg = g.createLinearGradient(0, y, 0, y + CH);
    vg.addColorStop(0, rgba(P.ivory, 0.07)); vg.addColorStop(0.45, rgba(P.ivory, 0)); vg.addColorStop(1, rgba(P.brassDim, 0.25));
    g.fillStyle = vg; g.fillRect(x, y, CW, CH);
    g.fillStyle = rgba(P.brassDim, 0.9);
    g.fillRect(x, y, CW, 2); g.fillRect(x, y + CH - 2, CW, 2); g.fillRect(x, y, 2, CH); g.fillRect(x + CW - 2, y, 2, CH);
    g.fillStyle = mix(P.brass, P.ivory, 0.5); g.fillRect(x + 2, y + 2, CW - 4, 1); g.fillRect(x + 2, y + 2, 1, CH - 4);
    g.fillStyle = mix(P.brass, P.brassDim, 0.55); g.fillRect(x + 2, y + CH - 3, CW - 4, 1); g.fillRect(x + CW - 3, y + 2, 1, CH - 4);
    const code = list[k];
    if (!code) continue;
    if (!(code in map)) map[code] = k;
    let px = 84;
    for (;;) { g.font = font(700, px, FONT.plate); if (g.measureText(code).width <= CW - 60 || px <= 24) break; px -= 4; }
    const [a, d] = inkMetrics(g, font(700, px, FONT.plate), code, px);
    const cx = x + CW / 2, by = y + CH / 2 + (a - d) / 2;
    g.fillStyle = mix(P.brass, P.ivory, 0.55); g.fillText(code, cx, by + 1);   // 刻痕下沿嘅反光
    g.fillStyle = P.walnut; g.fillText(code, cx, by);
  }
  const blank = Math.min(list.length, COLS * ROWS - 1);
  function uv(code){
    const key = str(code).trim();
    const k = (key in map) ? map[key] : blank;
    const u0 = (k % COLS) / COLS, v0 = Math.floor(k / COLS) / ROWS;
    return [u0, v0, u0 + 1 / COLS, v0 + 1 / ROWS];
  }
  return { canvas: c, uv, cols: COLS, rows: ROWS, cell: [CW, CH], blank };
}

// ── 招牌 2048×192 ──────────────────────────────────────────────────────────
// #0E0E11 底, 銅字: Latin = Cormorant 600 small caps 0.2em; CJK = Noto Sans HK 700 (≤0.06em)
// 置中, 自動縮細, 超過 22 字 → 22 字 + …; 上下各離邊 12 px 一條 1 px 銅線
function marquee(text, opts){
  const W = 2048, H = 192, [c, g] = mk(W, H);
  const max = (opts && opts.max > 0) ? opts.max : 22;
  g.fillStyle = P.marqueeBg; g.fillRect(0, 0, W, H);
  g.fillStyle = rgba(P.brass, 0.85); g.fillRect(0, 12, W, 1); g.fillRect(0, H - 13, W, 1);
  let cs = chars(text).filter(ch => ch !== "\n" && ch !== "\r");
  if (cs.length > max) cs = cs.slice(0, max).concat(["…"]);
  const s = cs.join("").trim();
  if (!s) return c;
  const spec = {
    px: 96,
    latin: { weight: 600, fam: FONT.display, track: 0.2, smallCaps: true },
    cjk: { weight: 700, fam: FONT.cjk, track: 0.04, scale: 0.85 },
  };
  g.textAlign = "left"; g.textBaseline = "alphabetic";
  const lay = fitMixed(g, s, spec, W - 2 * 96, 20);
  const cy = H / 2, by = mixedBaseline(g, lay, spec, cy);
  // 字畫喺獨立一層: 上光下暗嘅銅漸變 + 拉絲 (source-atop 只落喺字上)
  const [lc, lg] = mk(W, H);
  lg.textAlign = "left"; lg.textBaseline = "alphabetic";
  const gr = lg.createLinearGradient(0, by - spec.px * 0.7, 0, by + spec.px * 0.25);
  gr.addColorStop(0, mix(P.brass, P.ivory, 0.35)); gr.addColorStop(0.55, P.brass); gr.addColorStop(1, mix(P.brass, P.brassDim, 0.35));
  lg.fillStyle = gr;
  drawMixed(lg, lay, W / 2, by, "center");
  lg.globalCompositeOperation = "source-atop";
  brushLines(lg, W, H, 5, 2);
  g.drawImage(lc, 0, 0);
  return c;
}

// ── 海報 700×1000 ──────────────────────────────────────────────────────────
// video = {title, channel, views, duration, live}; img = 已 load 嘅 HTMLImageElement (可無)
function poster(video, img){
  const W = 700, H = 1000, [c, g] = mk(W, H);
  const v = video && typeof video === "object" ? video : {};
  const title = str(v.title).trim() || "一號影廳";
  const channel = str(v.channel).trim();
  const views = fmtViews(v.views);
  const dur = fmtDuration(v.duration);
  const live = !!v.live;
  const hasImg = !!(img && typeof img === "object" && (img.naturalWidth > 0 || img.width > 0) && (img.naturalHeight > 0 || img.height > 0));
  g.textAlign = "left"; g.textBaseline = "alphabetic";
  // 底: 雙色調 wash (色相永遠一樣)
  const wash = g.createLinearGradient(0, 0, 0, H);
  wash.addColorStop(0, P.washTop); wash.addColorStop(1, P.washBot);
  g.fillStyle = wash; g.fillRect(0, 0, W, H);
  const IMG_H = Math.round(H * 0.62);
  if (hasImg) {
    // 縮圖 cover-fit 落上面 62%
    const iw = img.naturalWidth || img.width, ih = img.naturalHeight || img.height;
    const sc = Math.max(W / iw, IMG_H / ih), dw = iw * sc, dh = ih * sc;
    g.save(); g.beginPath(); g.rect(0, 0, W, IMG_H); g.clip();
    try { g.drawImage(img, (W - dw) / 2, (IMG_H - dh) / 2, dw, dh); } catch (e) { warn("poster.drawImage", e); }
    g.restore();
    // 炭灰漸變: 由圖底 35% 開始溶入下面嘅 wash
    const gr = g.createLinearGradient(0, IMG_H * 0.65, 0, IMG_H);
    gr.addColorStop(0, rgba(P.washTop, 0)); gr.addColorStop(0.7, rgba(P.washTop, 0.7)); gr.addColorStop(1, rgba(P.washTop, 1));
    g.fillStyle = gr; g.fillRect(0, IMG_H * 0.65, W, IMG_H * 0.35 + 1);
    const panel = g.createLinearGradient(0, IMG_H, 0, H);
    panel.addColorStop(0, P.washTop); panel.addColorStop(1, P.washBot);
    g.fillStyle = panel; g.fillRect(0, IMG_H, W, H - IMG_H);
  }
  grain(g, W, H, hasImg ? 0.45 : 0.7);
  // 內框: 1 px 銅線 (唯一裝飾)
  g.strokeStyle = rgba(P.brass, hasImg ? 0.35 : 0.5); g.lineWidth = 1;
  g.strokeRect(28.5, 28.5, W - 57, H - 57);
  // 文字區
  const X = 64, TW = W - 2 * X;
  const tPx = hasImg ? 40 : 48, tLH = hasImg ? 50 : 60, maxLines = hasImg ? 3 : 4;
  const tFont = font(700, tPx, FONT.cjk);
  const lines = wrapText(g, title, tFont, TW, maxLines);
  const chPx = 26, vwPx = 22;
  const blockH = lines.length * tLH + 24 + 2 + (channel ? 30 + chPx : 0) + (views ? 34 + vwPx : 0);
  let y = hasImg ? IMG_H + 44 : Math.round((H - blockH) / 2) - 20;
  y = Math.max(hasImg ? IMG_H + 44 : 120, y);
  g.font = tFont; g.fillStyle = P.ivory;
  for (const ln of lines) { g.fillText(ln, X, y + tPx * 0.86); y += tLH; }
  y += 24;
  g.fillStyle = P.brass; g.fillRect(X, y, 72, 2); y += 2;
  if (channel) {
    y += 30;
    const cf = font(500, chPx, FONT.cjk);
    const cl = wrapText(g, channel, cf, TW, 1)[0] || "";
    g.font = cf; g.fillStyle = rgba(P.ivory, 0.8); g.fillText(cl, X, y + chPx * 0.86);
    y += chPx;
  }
  if (views) {
    y += 34;
    // 數字用 Plex Mono, 「萬 / 億 / 次觀看」用 Noto Sans HK (mono 冇 CJK)
    const spec = { px: vwPx, latin: { weight: 500, fam: FONT.mono, track: 0.04 }, cjk: { weight: 500, fam: FONT.cjk, track: 0.02, scale: 0.95 } };
    const lay = layoutMixed(g, views + " 次觀看", spec);
    g.fillStyle = P.brass; drawMixed(g, lay, X, y + vwPx * 0.8, "left");
    y += vwPx;
  }
  // 底行: 左「VELOUR · 一號影廳」(Cormorant small caps + Noto), 右片長 (mono)
  const fy = H - 60;
  {
    const spec = { px: 20, latin: { weight: 600, fam: FONT.display, track: 0.25, smallCaps: true }, cjk: { weight: 500, fam: FONT.cjk, track: 0.06, scale: 0.9 } };
    const lay = layoutMixed(g, "VELOUR · 一號影廳", spec);
    g.fillStyle = rgba(P.brass, 0.85); drawMixed(g, lay, X, fy, "left");
    if (dur) {
      const dl = layoutRun(g, dur, 20, 500, FONT.mono, 0.04, false);
      g.fillStyle = P.textDim; drawRun(g, dl, W - X - dl.width, fy);
    }
  }
  // 直播中: 酒紅牌, 左上
  if (live) {
    const lf = font(500, 22, FONT.cjk); g.font = lf;
    const tw = g.measureText("直播中").width, bw = Math.round(tw + 56), bh = 44, bx = 48, byy = 48;
    g.fillStyle = P.burgundy; g.fillRect(bx, byy, bw, bh);
    g.fillStyle = P.ivory; g.beginPath(); g.arc(bx + 20, byy + bh / 2, 4.5, 0, Math.PI * 2); g.fill();
    g.fillText("直播中", bx + 36, byy + bh / 2 + 8);
  }
  vignette(g, W, H, 0.03, 0.1);
  return c;
}
function titleCard(video){ return poster(video, null); }

// ── 預留卡 480×320 ─────────────────────────────────────────────────────────
// 象牙紙, "RESERVED" Cormorant 600 small caps 0.22em 42 px, 下面「預留」Noto 500 44 px,
// 銅雙線框 (1 + 3 + 1) 離邊 14 px, 邊位暖暗 2%
function reservedCard(){
  const W = 480, H = 320, [c, g] = mk(W, H);
  paper(g, W, H);
  doubleHairline(g, W, H, 14, 3, 0.95);
  g.textAlign = "left"; g.textBaseline = "alphabetic";
  const spec = { px: 42, latin: { weight: 600, fam: FONT.display, track: 0.22, smallCaps: true }, cjk: { weight: 500, fam: FONT.cjk } };
  const lay = fitMixed(g, "RESERVED", spec, W - 100, 20);
  g.fillStyle = P.ink; drawMixed(g, lay, W / 2 + spec.px * 0.11, 128, "center");   // 補返尾字嘅 tracking, 視覺先真正置中
  g.fillStyle = P.brass; g.fillRect(W / 2 - 24, 154, 48, 1);
  const zf = font(500, 44, FONT.cjk), [a, d] = inkMetrics(g, zf, "預留", 44);
  g.font = zf; g.textAlign = "center"; g.fillStyle = P.ink;
  g.fillText("預留", W / 2, 214 + (a - d) / 2);
  return c;
}

// ── 戲飛 680×400 ───────────────────────────────────────────────────────────
// fields = {venue, film, time, seat, no}; 左邊打孔 (透明), 右邊 24 條 barcode
function ticketStub(fields){
  const W = 680, H = 400, [c, g] = mk(W, H);
  const f = fields && typeof fields === "object" ? fields : {};
  const venue = str(f.venue).trim() || "VELOUR · 一號影廳";
  const film = str(f.film).trim() || "—";
  const time = str(f.time).trim() || "—", seat = str(f.seat).trim() || "—", no = str(f.no).trim() || "—";
  paper(g, W, H);
  doubleHairline(g, W, H, 20, 3, 0.9);
  g.textAlign = "left"; g.textBaseline = "alphabetic";
  const X = 76, TW = 500;
  // 行 1: 場館 (Cormorant small caps + Noto), 下面一條銅線
  {
    const spec = { px: 26, latin: { weight: 600, fam: FONT.display, track: 0.22, smallCaps: true }, cjk: { weight: 500, fam: FONT.cjk, track: 0.06, scale: 0.92 } };
    const lay = fitMixed(g, venue, spec, TW, 14);
    g.fillStyle = P.ink; drawMixed(g, lay, X, 84, "left");
    g.fillStyle = rgba(P.brass, 0.9); g.fillRect(X, 102, TW, 1);
  }
  // 行 2: 標籤「今場放映」(mono 灰) + 片名 (Noto 500, 最多 2 行)
  g.font = font(500, 20, FONT.cjk); g.fillStyle = P.textDim; g.fillText("今場放映", X, 150);
  {
    const tf = font(500, 32, FONT.cjk), lines = wrapText(g, film, tf, TW, 2);
    g.font = tf; g.fillStyle = P.ink;
    lines.forEach((ln, i) => g.fillText(ln, X, 196 + i * 42));
  }
  // 行 3: 場次 / 座位 / 票號 (label 灰 + 數值 mono)
  {
    const y = 330, cols = [[X, "場次", time], [X + 190, "座位", seat], [X + 340, "票號", no]];
    for (const [x, label, val] of cols) {
      g.font = font(500, 18, FONT.cjk); g.fillStyle = P.textDim; g.fillText(label, x, y - 30);
      const l = layoutRun(g, val, 24, 500, FONT.mono, 0.04, false);
      g.fillStyle = P.ink; drawRun(g, l, x, y);
    }
  }
  // Barcode: 右邊 24 條橫 bar, 粗幼由票號決定
  {
    let seed = 0; for (const ch of chars(no)) seed = (seed * 31 + ch.charCodeAt(0)) >>> 0;
    const R = mulberry32(seed || 483921), x0 = W - 78, bw = 40, y0 = 52, span = H - 104, slot = span / 24;
    g.fillStyle = P.ink;
    for (let i = 0; i < 24; i++) { const t = 2 + Math.floor(R() * 6); g.fillRect(x0, Math.round(y0 + i * slot), bw, t); }
    g.fillStyle = rgba(P.brass, 0.9); g.fillRect(x0 - 22, 52, 1, span);
  }
  // 左邊打孔: 先落一圈陰影, 再打穿 (透明)
  {
    const x = 38;
    for (let y = 40; y <= H - 40; y += 22) {
      g.fillStyle = rgba(P.walnut, 0.18); g.beginPath(); g.arc(x, y + 0.6, 5.2, 0, Math.PI * 2); g.fill();
    }
    g.globalCompositeOperation = "destination-out"; g.fillStyle = "#000";
    for (let y = 40; y <= H - 40; y += 22) { g.beginPath(); g.arc(x, y, 4.2, 0, Math.PI * 2); g.fill(); }
    g.globalCompositeOperation = "source-over";
  }
  return c;
}

// ── 縮圖 loader: 永遠 resolve (成功 → Image, 失敗 / 超時 / 空 url → null) ───
function loadThumb(url, timeoutMs){
  return new Promise(res => {
    try {
      if (!url || typeof url !== "string") return res(null);
      const img = new Image(); let done = false;
      const fin = v => { if (done) return; done = true; clearTimeout(t); res(v); };
      const t = setTimeout(() => fin(null), timeoutMs > 0 ? timeoutMs : 4000);
      img.crossOrigin = "anonymous"; img.decoding = "async";
      img.onload = () => fin(img.naturalWidth > 0 ? img : null);
      img.onerror = () => fin(null);
      img.src = url;
    } catch (e) { res(null); }
  });
}

// ── Web font 到齊未 (畫完文字質感之後可以再畫一次); 永遠 resolve boolean ────
function ready(timeoutMs){
  return new Promise(res => {
    try {
      const fs = document.fonts;
      if (!fs || !fs.load) return res(false);
      const t = setTimeout(() => res(false), timeoutMs > 0 ? timeoutMs : 3000);
      Promise.all([
        fs.load('600 32px "Cormorant Garamond"', "VELOUR"), fs.load('700 32px "Cormorant Garamond"', "D6"),
        fs.load('700 32px "Noto Sans HK"', "一號影廳"), fs.load('500 32px "Noto Sans HK"', "預留"),
        fs.load('500 32px "IBM Plex Mono"', "0123"),
      ]).then(() => { clearTimeout(t); res(true); }, () => { clearTimeout(t); res(false); });
    } catch (e) { res(false); }
  });
}

// ── 星空天花: n 粒 (x −6.5..6.5, y 8.19, z 2..17), 12 粒閃 (seeded, 每次一樣) ──
function starField(n, seed){
  n = clamp(Math.floor(Number(n) || 0), 0, 4096);
  const R = mulberry32(Number(seed) || 1);
  const positions = new Float32Array(n * 3), twinkle = new Uint8Array(n);
  for (let i = 0; i < n; i++) { positions[i * 3] = -6.5 + 13 * R(); positions[i * 3 + 1] = 8.19; positions[i * 3 + 2] = 2 + 15 * R(); }
  const idx = new Uint16Array(n); for (let i = 0; i < n; i++) idx[i] = i;
  for (let k = 0; k < Math.min(12, n); k++) {   // partial Fisher–Yates: 揀 12 粒唔重複
    const j = k + Math.floor(R() * (n - k)); const tmp = idx[k]; idx[k] = idx[j]; idx[j] = tmp; twinkle[idx[k]] = 1;
  }
  return { positions, twinkle, count: n };
}

// ── 對外 ───────────────────────────────────────────────────────────────────
const TheatreTex = {
  ribs: guard("ribs", ribs, 1024, 1024, P.wall),
  carpet: guard("carpet", carpet, 512, 512, P.floor),
  stone: guard("stone", stone, 1024, 1024, P.stoneDark),
  curtain: guard("curtain", curtain, 1024, 2048, P.velvetHi),
  pelmet: guard("pelmet", pelmet, 2048, 256, P.velvetHi),
  plateAtlas(codes){
    try { return plateAtlas(codes); }
    catch (e) { warn("plateAtlas", e); return { canvas: fallback(2048, 1024, P.brass), uv: () => [0, 0, 1 / 8, 1 / 8], cols: 8, rows: 8, cell: [256, 128], blank: 0 }; }
  },
  marquee: guard("marquee", marquee, 2048, 192, P.marqueeBg),
  poster: guard("poster", poster, 700, 1000, P.washTop),
  titleCard: guard("titleCard", titleCard, 700, 1000, P.washTop),
  reservedCard: guard("reservedCard", reservedCard, 480, 320, P.ivory),
  ticketStub: guard("ticketStub", ticketStub, 680, 400, P.ivory),
  loadThumb,
  ready,
  starField(n, seed){
    try { return starField(n, seed); }
    catch (e) { warn("starField", e); return { positions: new Float32Array(0), twinkle: new Uint8Array(0), count: 0 }; }
  },
  fonts: FONT,
  palette: P,
  fmtViews, fmtDuration,
};
if (typeof window !== "undefined") window.TheatreTex = TheatreTex;
if (typeof module !== "undefined" && module.exports) module.exports = TheatreTex;
})();
