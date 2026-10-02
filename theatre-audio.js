/* ══════════════════════════════════════════════════════════════════════════
   The Grand · 一號影廳 音效 (WebAudio, 零外部依賴, 冇音檔)
   ──────────────────────────────────────────────────────────────────────────
   所有聲音都係即場用 WebAudio node 合成: 一段 2 s 白噪音 buffer + 濾波器 +
   振盪器 + 包絡。冇 UI 撳掣聲, 冇任何嘢似遊戲 — 呢度係高級戲院, 一切都要淡。
   入場之前係完全靜嘅; 冇 AudioContext 嘅機就成個模組變 no-op, 唔會掉 error。

   聲音清單 (電平 = 相對 master 嘅 dBFS, master 本身 0.6):
   · 房間底噪 room tone   噪音 → lowpass 180 Hz → −46 dB, 0.08 Hz LFO ±2 dB; 放映時再 −8 dB
   · 冷氣 HVAC            sine 60 Hz −52 dB + sine 120.3 Hz −56 dB; 放映時再 −8 dB
   · 觀眾細語 murmur      噪音 → bandpass 550 Hz → 慢噪音 AM 60 %; 前廳 −56 / 影廳 −50 / 放映 −62 dB
   · 雜聲咳嗽 rustle      每 6–18 s 一下, 90–180 ms, bandpass 0.8–2.4 kHz, −40 dB, 跟座位方向 pan
   · 入場鐘聲 chime       sine E5 → B5 (隔 220 ms), 各 1.2 s 衰減, −18 dB, lowpass 4 kHz
   · 門聲 door            噪音 → lowpass 400 Hz, 0 → −38 dB 0.6 s, 停 0.2 s, 放 0.8 s (遲到者 −50 dB)
   · 腳步 footstep        40 ms 噪音, 石地 bandpass 1.8 kHz / 地氈 lowpass 500 Hz, −34 dB, 左右 ±0.15 交替
   · 帶位鐘 bell          sine 1568 + 2349 Hz, 2 s 衰減, −18 dB (閒置 −30 dB), pan +0.4
   · 幕布摩打 motor       噪音 → lowpass 260 Hz + sine 48 Hz, −44 dB, 開 2.8 s / 閂 2.0 s, pan 跟領先嗰半幅
   · 皮革吱聲 creak       60 ms 噪音 → bandpass Q 4, 420 → 240 Hz 掃頻, −36 dB
   · 銀幕醒嚟 thump       sine 48 Hz, 350 ms 衰減, −24 dB
   · 影廳 room            三條 feedback delay 37/53/71 ms, feedback 0.25, wet −14 dB;
                          只有 bell / door / motor / creak 經過, 底噪 / 冷氣 / 細語唔經。
   最後有個 DynamicsCompressor (threshold −6 dB, ratio 4) 做安全網, 保證唔會爆。

   對外介面:
     const A = TheatreAudio.create();    // 呢一步未開 AudioContext
     A.available                         // false = 呢部機開唔到 AudioContext; 之後所有方法都係靜靜地 no-op
     A.start()                           // 一定要喺 click handler 入面叫: 開/resume context, 起 graph,
                                         // 播 chime, 起底噪 + 冷氣 + 細語, 開始自動雜聲。叫兩次冇問題。
     A.resume()                          // 之後嘅 pointerdown 叫一下 (iOS 有時會 suspended)
     A.event(name, opts)                 // 'chime' | 'door' {db, pan} | 'footstep' {surface, pan} | 'bell' {db, pan}
                                         // | 'motor' {open, seconds} | 'creak' | 'thump' | 'rustle' {pan, dist}
                                         // | 'latecomer' (門聲 −50 dB) | 'idleBell' (帶位鐘 −30 dB)
     A.setState({preShow, playing, inHall, house})   // 連續狀態, 全部用 ramp, 永遠唔會跳
     A.setListener({x, z, yaw})          // 鏡頭位置 (同 theatre.js 一樣: yaw 0 望 −z, +x 係右手邊)
     A.rustleFrom({x, z})                // 由某個世界位置發一下雜聲: pan = clamp(dx/8, −1, 1), gain × 1/(1 + dist·0.15)
     A.setOccupied([{x, z}, ...])        // 有人坐嘅座位, 自動雜聲會由呢啲位發出
     A.mute(bool) / A.toggleMute() → 而家係咪靜音   A.muted
     A.debug()                           // 畀 debug 面板睇嘅數字
     A.dispose()                         // 清晒 timer, 停晒 source, 閂 context
   ══════════════════════════════════════════════════════════════════════════ */
(() => {
"use strict";

const AC = (typeof window !== "undefined" && (window.AudioContext || window.webkitAudioContext)) || null;
const clamp = (v,a,b) => v < a ? a : v > b ? b : v;
const dB = x => Math.pow(10, x/20);                 // dBFS → 線性 gain
const rnd = (a,b) => a + Math.random()*(b-a);
const T60 = secs => secs / 6.91;                     // 「n 秒指數衰減」= n 秒跌 60 dB → setTargetAtTime 嘅時間常數

// ── 電平表 (dB, 相對 master) ─────────────────────────────────────────────
//  全部照 spec 抄, 改數就喺呢度改, 唔好散落喺下面。
const LV = {
  master: 0.6,                 // 線性, 唔係 dB
  roomTone: -46, roomToneLfo: 2,
  hvac60: -52, hvac120: -56,
  murmurVestibule: -56, murmurHall: -50, murmurShow: -62,
  rustle: -40,
  chime: -18,
  door: -38, doorLate: -50,
  step: -34,
  bell: -18, bellIdle: -30,
  motor: -44,
  creak: -36,
  thump: -24,
  duck: -8,                    // 放映時 底噪 + 冷氣 + 雜聲 再壓落去
  roomWet: -14,
};

class TheatreAudio {
  constructor(){
    this.available = !!AC;
    this.ctx = null;
    this._n = null;              // graph 入面啲 node
    this._noise = null;          // 2 s 白噪音 buffer, 整一次, 個個 source 共用
    this._started = false;
    this._disposed = false;
    this._muted = false;
    this._timers = new Set();    // 所有 setTimeout 都記低, dispose 時一齊清
    this._st = { preShow: true, playing: false, inHall: false, house: 1 };
    this._lis = { x: 0, z: 21, yaw: 0 };   // 鏡頭出生點 (前廳)
    this._occ = null;            // 有人坐嘅座位 [{x,z}]
    this._stepSide = 1;          // 腳步左右交替
  }

  get muted(){ return this._muted; }

  // 「而家可唔可以出聲」: 有 context、未 dispose、context 未閂
  _ok(){ return this.available && !!this.ctx && !this._disposed && this.ctx.state !== "closed"; }

  // ── 生命周期 ────────────────────────────────────────────────────────────
  start(){
    if (!this.available || this._disposed) return;
    if (!this.ctx) {
      // 開 context。舊 Safari 嘅 webkitAudioContext 可能唔食 options, 所以退一步再試。
      try { this.ctx = new AC({ latencyHint: "interactive" }); }
      catch (e) { try { this.ctx = new AC(); } catch (e2) { this.ctx = null; } }
      if (!this.ctx) { this.available = false; return; }
      try { this._build(); }
      catch (e) {
        // graph 起唔到 (奇怪嘅 WebView 之類): 當冇音效, 唔好搞死個 page
        this.available = false;
        try { this.ctx.close(); } catch (e2) {}
        this.ctx = null; this._n = null;
        return;
      }
    }
    // 喺 user gesture 入面叫嘅話呢度會真係 resume; 唔係嘅話就靜靜地留喺 suspended
    this.resume();
    if (this._started) return;
    this._started = true;
    const t = this.ctx.currentTime + 0.02;
    this._startBeds(t);
    this._chime(t);
    this._scheduleRustle();
  }

  resume(){
    if (!this._ok()) return;
    if (this.ctx.state === "suspended") {
      try { const p = this.ctx.resume(); if (p && p.catch) p.catch(() => {}); } catch (e) {}
    }
  }

  dispose(){
    if (this._disposed) return;
    this._disposed = true;
    for (const id of this._timers) clearTimeout(id);
    this._timers.clear();
    const ctx = this.ctx, n = this._n;
    this.ctx = null; this._n = null; this._noise = null; this._started = false;
    if (!ctx) return;
    try {
      if (n) {
        n.master.gain.cancelScheduledValues(0);
        n.master.gain.value = 0;                          // 先收聲, 再拆
        for (const s of n.beds) { try { s.stop(); } catch (e) {} }
      }
      if (ctx.state !== "closed") { const p = ctx.close(); if (p && p.catch) p.catch(() => {}); }
    } catch (e) {}
  }

  // ── 細 node 工廠 ───────────────────────────────────────────────────────
  _g(v){ const g = this.ctx.createGain(); g.gain.value = v; return g; }
  _osc(type, f){ const o = this.ctx.createOscillator(); o.type = type; o.frequency.value = f; return o; }
  // 留意: BiquadFilter 嘅 lowpass/highpass Q 係用 dB 計嘅, 所以呢度將傳統 (線性) Q 轉做 dB,
  // 咁 spec 寫「Q 0.7」先真係 Butterworth 附近, 而唔係暗中加咗個 resonance。
  _lp(f, q = 0.7071){ const b = this.ctx.createBiquadFilter(); b.type = "lowpass"; b.frequency.value = f; b.Q.value = 20*Math.log10(q); return b; }
  _bp(f, q){ const b = this.ctx.createBiquadFilter(); b.type = "bandpass"; b.frequency.value = f; b.Q.value = q; return b; }
  _src(loop){ const s = this.ctx.createBufferSource(); s.buffer = this._noise; s.loop = !!loop; return s; }
  _pan(p){
    const ctx = this.ctx;
    if (typeof ctx.createStereoPanner === "function") { const n = ctx.createStereoPanner(); n.pan.value = clamp(p, -1, 1); return n; }
    // 舊 Safari 冇 StereoPanner: 用普通 gain 頂住, pan 變 no-op (單聲道都好過冇聲)
    const g = ctx.createGain();
    g.pan = { value: p, setValueAtTime(){}, linearRampToValueAtTime(){} };
    return g;
  }
  // 慢噪音 (白噪音 → lowpass 2 Hz → 放大 → tanh 夾住 ±1): 用嚟做細語嘅 AM。
  // playbackRate 較低 → 2 s 嘅 buffer loop 變長, 兩聲道又用唔同 rate, 就聽唔出重複。
  _slowNoise(t, rate){
    const ctx = this.ctx;
    const s = this._src(true); s.playbackRate.value = rate;
    const lp = this._lp(2, 0.7071);
    // 2 Hz lowpass 之後嘅 RMS 好細 (≈ 0.577·√(2.22 Hz / Nyquist)), 先估返個放大倍數令 RMS ≈ 0.8
    const rms = 0.577 * Math.sqrt(2.22 / (rate * ctx.sampleRate / 2));
    const boost = this._g(0.8 / rms);
    const sh = ctx.createWaveShaper();
    const N = 1024, curve = new Float32Array(N);
    for (let i = 0; i < N; i++) curve[i] = Math.tanh(1.6 * ((i/(N-1))*2 - 1));
    sh.curve = curve;
    s.connect(lp); lp.connect(boost); boost.connect(sh);
    s.start(t, rnd(0, 1.8));
    return { out: sh, src: s };
  }
  // AudioParam 平滑去某個值: 兩邊都 > 0 就用指數 (dB 線性, 聽落順), 否則用線性。永遠唔跳。
  _ramp(p, v, secs){
    const t = this.ctx.currentTime, cur = Math.max(p.value, 0);
    p.cancelScheduledValues(t);
    p.setValueAtTime(cur, t);
    if (cur > 1e-6 && v > 1e-6) p.exponentialRampToValueAtTime(v, t + secs);
    else p.linearRampToValueAtTime(v, t + secs);
  }
  // one-shot 嘅出口: 可選 pan、可選經影廳 (room)、可選經 duck bus
  _route(node, { pan = null, room = false, duck = false } = {}){
    const n = this._n; let last = node;
    if (pan !== null) { const p = this._pan(pan); last.connect(p); last = p; }
    last.connect(duck ? n.duck : n.master);
    if (room) last.connect(n.roomIn);
    return last;   // motor 要拎住個 panner 嚟 ramp
  }
  _later(fn, ms){
    const id = setTimeout(() => { this._timers.delete(id); fn(); }, ms);
    this._timers.add(id);
    return id;
  }

  // ── 起 graph ────────────────────────────────────────────────────────────
  _build(){
    const ctx = this.ctx, n = this._n = { beds: [] };

    // master (0.6) → 安全壓縮器 → destination
    n.comp = ctx.createDynamicsCompressor();
    n.comp.threshold.value = -6; n.comp.ratio.value = 4; n.comp.knee.value = 6;
    n.comp.attack.value = 0.003; n.comp.release.value = 0.25;
    n.comp.connect(ctx.destination);
    n.master = this._g(LV.master); n.master.connect(n.comp);

    // duck bus: 底噪 + 冷氣 + 雜聲 放映時一齊 −8 dB
    n.duck = this._g(this._st.playing ? dB(LV.duck) : 1); n.duck.connect(n.master);

    // 影廳: 三條 feedback delay 並排, 各自 feedback 0.25, 合埋 wet −14 dB。
    // 返回路加個 3.8 kHz lowpass 做阻尼 — 絨布同地氈唔會反射高頻, 冇佢會有金屬味。
    n.roomIn = this._g(1);
    n.roomWet = this._g(dB(LV.roomWet)); n.roomWet.connect(n.master);
    n.roomDamp = this._lp(3800); n.roomDamp.connect(n.roomWet);
    for (const d of [0.037, 0.053, 0.071]) {
      const dl = ctx.createDelay(0.5); dl.delayTime.value = d;
      const fb = this._g(0.25); dl.connect(fb); fb.connect(dl);
      const tap = this._g(1/3);                    // 三條加埋先係 wet 嘅標稱電平
      n.roomIn.connect(dl); dl.connect(tap); tap.connect(n.roomDamp);
    }

    // 白噪音 buffer: 2 s, 整一次, 之後每個 source 都用佢 (隨機 offset 開始就唔會一齊)
    const len = Math.floor(ctx.sampleRate * 2);
    const buf = ctx.createBuffer(1, len, ctx.sampleRate), d = buf.getChannelData(0);
    for (let i = 0; i < len; i++) d[i] = Math.random()*2 - 1;
    this._noise = buf;
  }

  // 長開嘅底層: 房間底噪、冷氣、觀眾細語 (全部唔經影廳 delay)
  _startBeds(t){
    const n = this._n, st = this._st;

    // ROOM TONE: 噪音 → lowpass 180 Hz Q 0.7 → −46 dB (0.08 Hz LFO ±2 dB) → duck
    const rt = this._src(true), rtF = this._lp(180, 0.7), rtG = this._g(dB(LV.roomTone));
    rt.connect(rtF); rtF.connect(rtG); rtG.connect(n.duck);
    // ±2 dB 即係 ×1.26 … ×0.79, 擺幅大約係基準嘅 0.23 倍; LFO 加落 gain param 度
    const lfo = this._osc("sine", 0.08), lfoG = this._g(dB(LV.roomTone) * 0.23);
    lfo.connect(lfoG); lfoG.connect(rtG.gain);
    rt.start(t, rnd(0, 1.8)); lfo.start(t);
    n.roomTone = rtG; n.beds.push(rt, lfo);

    // HVAC: sine 60 Hz −52 dB + sine 120.3 Hz −56 dB (同 60 嘅二次諧波差 0.3 Hz, 有少少拍頻)
    for (const [f, lv] of [[60, LV.hvac60], [120.3, LV.hvac120]]) {
      const o = this._osc("sine", f), g = this._g(dB(lv));
      o.connect(g); g.connect(n.duck); o.start(t); n.beds.push(o);
    }

    // MURMUR: 兩個獨立聲道 pan ±0.6, 各自 噪音 → bandpass 550 Hz Q 0.9 → AM (深度 60 %) → murmur bus
    const showing = st.playing && !st.preShow;
    n.murmur = this._g(dB(showing ? LV.murmurShow : (st.inHall ? LV.murmurHall : LV.murmurVestibule)));
    n.murmur.connect(n.master);
    [-0.6, 0.6].forEach((pan, i) => {
      const s = this._src(true); s.playbackRate.value = i ? 1.03 : 0.97;   // 兩邊 loop 長度唔同, 唔會鎖埋一齊
      const bp = this._bp(550, 0.9);
      const am = this._g(0.7);                       // 基準 0.7, 慢噪音 ±0.3 → 0.4 … 1.0 = 60 % 深度
      const mod = this._slowNoise(t, i ? 0.43 : 0.31);
      const depth = this._g(0.3); mod.out.connect(depth); depth.connect(am.gain);
      const ch = this._g(Math.SQRT1_2);              // 兩聲道各 −3 dB, 合埋先係標稱電平
      const p = this._pan(pan);
      s.connect(bp); bp.connect(am); am.connect(ch); ch.connect(p); p.connect(n.murmur);
      s.start(t, rnd(0, 1.8));
      n.beds.push(s, mod.src);
    });
  }

  // ── 連續狀態 ───────────────────────────────────────────────────────────
  //  preShow  幕閂住嘅時候 (入場、揀位、換片、散場) → 細語返嚟
  //  playing  iframe 話緊 playing (或者幕開咗 4 s) → 底噪/冷氣/雜聲 −8 dB, 細語 −62 dB
  //  inHall   鏡頭過咗 z=17.5 → 細語由 −56 升去 −50
  //  house    燈光 0..1, 記低畀 debug 睇, 而家冇聲音跟佢
  setState(s){
    if (!s || typeof s !== "object") return;
    for (const k of ["preShow", "playing", "inHall", "house"]) if (s[k] !== undefined) this._st[k] = s[k];
    if (!this._ok() || !this._started) return;     // 未 start 就淨係記低, start 時會用
    const st = this._st, n = this._n;
    try {
      const showing = st.playing && !st.preShow;
      const lvl = showing ? LV.murmurShow : (st.inHall ? LV.murmurHall : LV.murmurVestibule);
      this._ramp(n.murmur.gain, dB(lvl), showing ? 2.0 : 0.5);    // 開場 2 s 慢慢沉落去; 其他 0.5 s
      this._ramp(n.duck.gain, st.playing ? dB(LV.duck) : 1, 1.0);
    } catch (e) {}
  }

  setListener(l){
    if (!l) return;
    if (typeof l.x === "number") this._lis.x = l.x;
    if (typeof l.z === "number") this._lis.z = l.z;
    if (typeof l.yaw === "number") this._lis.yaw = l.yaw;
  }
  setOccupied(list){
    this._occ = (Array.isArray(list) && list.length)
      ? list.map(s => ({ x: +s.x || 0, z: +s.z || 0 })) : null;
  }
  // 由世界位置 (x,z) 發一下雜聲, 相對聽者: 右手邊 = (cos yaw, −sin yaw) (同 theatre.js 嘅 yaw 一樣)
  rustleFrom(p){
    if (!p) return;
    const L = this._lis, dx = (+p.x || 0) - L.x, dz = (+p.z || 0) - L.z;
    const right = dx*Math.cos(L.yaw) - dz*Math.sin(L.yaw);
    this.event("rustle", { pan: clamp(right/8, -1, 1), dist: Math.hypot(dx, dz) });
  }

  mute(on){
    this._muted = !!on;
    if (!this._ok()) return this._muted;
    try { this._ramp(this._n.master.gain, this._muted ? 0 : LV.master, 0.2); } catch (e) {}
    return this._muted;
  }
  toggleMute(){ return this.mute(!this._muted); }

  debug(){
    const n = this._n;
    return {
      available: this.available, started: this._started, muted: this._muted,
      ctxState: this.ctx ? this.ctx.state : null,
      sampleRate: this.ctx ? this.ctx.sampleRate : 0,
      state: Object.assign({}, this._st), listener: Object.assign({}, this._lis),
      timers: this._timers.size,
      levels: n ? { master: n.master.gain.value, duck: n.duck.gain.value, murmur: n.murmur ? n.murmur.gain.value : 0,
                    roomTone: n.roomTone ? n.roomTone.gain.value : 0, compReduction: n.comp.reduction } : null,
    };
  }

  // ── 自動雜聲: 每 6–18 s 由一個有人嘅座位 (冇就隨機 x ±5, z 6..15) 咳一聲 / 郁一下 ──
  _scheduleRustle(){
    this._later(() => {
      if (!this._ok() || !this._started) return;
      if (this.ctx.state === "running") {            // suspended 時唔排, 唔然 resume 一刻會一齊爆出嚟
        const p = (this._occ && this._occ.length)
          ? this._occ[Math.floor(Math.random() * this._occ.length)]
          : { x: rnd(-5, 5), z: rnd(6, 15) };
        this.rustleFrom(p);
      }
      this._scheduleRustle();
    }, rnd(6000, 18000));
  }

  // ── one-shot ───────────────────────────────────────────────────────────
  event(name, opts){
    if (!this._ok() || !this._started) return;      // 入場前永遠靜
    const o = opts || {}, t = this.ctx.currentTime + 0.005;
    try {
      switch (name) {
        case "chime":     this._chime(t); break;
        case "door":      this._door(t, typeof o.db === "number" ? o.db : LV.door, +o.pan || 0); break;
        case "latecomer": this._door(t, LV.doorLate, +o.pan || 0); break;
        case "footstep":  this._footstep(t, o.surface === "carpet" ? "carpet" : "stone", typeof o.pan === "number" ? o.pan : null); break;
        case "bell":      this._bell(t, typeof o.db === "number" ? o.db : LV.bell, typeof o.pan === "number" ? o.pan : 0.4); break;
        case "idleBell":  this._bell(t, LV.bellIdle, typeof o.pan === "number" ? o.pan : 0.4); break;
        case "motor":     this._motor(t, o.open !== false, +o.seconds || 0); break;
        case "creak":     this._creak(t); break;
        case "thump":     this._thump(t); break;
        case "rustle":    this._rustle(t, typeof o.pan === "number" ? o.pan : 0, Math.max(0, +o.dist || 0)); break;
        default: break;                               // 唔識嘅名: 唔出聲, 唔嘈
      }
    } catch (e) { /* 音效永遠唔可以整死個 page */ }
  }

  // 入場鐘聲: E5 659.3 → B5 987.8, 隔 220 ms, 各 1.2 s 指數衰減, −18 dB, 經 lowpass 4 kHz (唔經影廳)
  _chime(t){
    const lp = this._lp(4000); this._route(lp);
    for (const [f, dt] of [[659.3, 0], [987.8, 0.22]]) {
      const o = this._osc("sine", f), g = this._g(0), t0 = t + dt, pk = dB(LV.chime);
      o.connect(g); g.connect(lp);
      g.gain.setValueAtTime(0, t0);
      g.gain.linearRampToValueAtTime(pk, t0 + 0.006);   // 6 ms 起音, 唔會有 click
      g.gain.setTargetAtTime(0, t0 + 0.006, T60(1.2));
      o.start(t0); o.stop(t0 + 1.5);
    }
  }

  // 門聲: 噪音 → lowpass 400 Hz → 0 → 標稱 0.6 s, 停 0.2 s, 放 0.8 s; 經影廳
  _door(t, db, pan){
    const s = this._src(true), lp = this._lp(400), g = this._g(0), pk = dB(db);
    s.connect(lp); lp.connect(g); this._route(g, { pan, room: true });
    g.gain.setValueAtTime(0, t);
    g.gain.linearRampToValueAtTime(pk, t + 0.6);
    g.gain.setValueAtTime(pk, t + 0.8);
    g.gain.linearRampToValueAtTime(0, t + 1.6);
    s.start(t, rnd(0, 1.5)); s.stop(t + 1.7);
  }

  // 腳步: 40 ms 噪音, 石地 bandpass 1.8 kHz Q 1.2 / 地氈 lowpass 500 Hz, 120 ms 衰減, −34 dB, pan ±0.15 交替
  _footstep(t, surface, pan){
    if (pan === null) { this._stepSide = -this._stepSide; pan = 0.15 * this._stepSide; }
    const s = this._src(true), f = surface === "carpet" ? this._lp(500) : this._bp(1800, 1.2), g = this._g(0), pk = dB(LV.step);
    s.connect(f); f.connect(g); this._route(g, { pan });
    g.gain.setValueAtTime(0, t);
    g.gain.linearRampToValueAtTime(pk, t + 0.003);
    g.gain.setValueAtTime(pk, t + 0.04);
    g.gain.setTargetAtTime(0, t + 0.04, T60(0.12));
    s.start(t, rnd(0, 1.8)); s.stop(t + 0.3);
  }

  // 帶位鐘: sine 1568 + 2349 Hz (0.65 : 0.35), 2 s 指數衰減, 標稱 −18 dB, pan +0.4 (觀眾席右邊); 經影廳
  _bell(t, db, pan){
    const g = this._g(0), pk = dB(db);
    this._route(g, { pan, room: true });
    for (const [f, w] of [[1568, 0.65], [2349, 0.35]]) {
      const o = this._osc("sine", f), og = this._g(w);
      o.connect(og); og.connect(g); o.start(t); o.stop(t + 2.3);
    }
    g.gain.setValueAtTime(0, t);
    g.gain.linearRampToValueAtTime(pk, t + 0.004);
    g.gain.setTargetAtTime(0, t + 0.004, T60(2.0));
  }

  // 幕布摩打: 噪音 → lowpass 260 Hz, 加 sine 48 Hz, 各 −44 dB; 起 0.4 s, 停, 放 0.6 s (開 2.8 s / 閂 2.0 s);
  // pan 跟住領先嗰半幅幕: 開幕左邊先 (−0.2 → +0.2), 閂幕右邊先 (+0.2 → −0.2); 經影廳
  _motor(t, open, secs){
    secs = Math.max(secs || (open ? 2.8 : 2.0), 1.2);
    const s = this._src(true), lp = this._lp(260), nG = this._g(dB(LV.motor));
    const o = this._osc("sine", 48), oG = this._g(dB(LV.motor));
    const g = this._g(0);
    s.connect(lp); lp.connect(nG); nG.connect(g);
    o.connect(oG); oG.connect(g);
    const p = this._route(g, { pan: open ? -0.2 : 0.2, room: true });
    g.gain.setValueAtTime(0, t);
    g.gain.linearRampToValueAtTime(1, t + 0.4);
    g.gain.setValueAtTime(1, t + secs - 0.6);
    g.gain.linearRampToValueAtTime(0, t + secs);
    p.pan.setValueAtTime(open ? -0.2 : 0.2, t);
    p.pan.linearRampToValueAtTime(open ? 0.2 : -0.2, t + secs);
    s.start(t, rnd(0, 1.5)); s.stop(t + secs + 0.1);
    o.start(t); o.stop(t + secs + 0.1);
  }

  // 皮革吱聲: 60 ms 噪音 → bandpass Q 4, 420 → 240 Hz 掃落去, −36 dB; 經影廳
  _creak(t){
    const s = this._src(true), bp = this._bp(420, 4), g = this._g(0), pk = dB(LV.creak);
    bp.frequency.setValueAtTime(420, t);
    bp.frequency.exponentialRampToValueAtTime(240, t + 0.06);
    s.connect(bp); bp.connect(g); this._route(g, { pan: 0, room: true });
    g.gain.setValueAtTime(0, t);
    g.gain.linearRampToValueAtTime(pk, t + 0.004);
    g.gain.setValueAtTime(pk, t + 0.05);
    g.gain.linearRampToValueAtTime(0, t + 0.075);
    s.start(t, rnd(0, 1.8)); s.stop(t + 0.15);
  }

  // 銀幕醒嚟: sine 48 Hz, 350 ms 指數衰減, −24 dB (唔經影廳, 低音入 delay 會糊)
  _thump(t){
    const o = this._osc("sine", 48), g = this._g(0), pk = dB(LV.thump);
    o.connect(g); this._route(g);
    g.gain.setValueAtTime(0, t);
    g.gain.linearRampToValueAtTime(pk, t + 0.008);
    g.gain.setTargetAtTime(0, t + 0.008, T60(0.35));
    o.start(t); o.stop(t + 0.6);
  }

  // 雜聲 / 咳嗽: 90–180 ms 噪音 → bandpass 800–2400 Hz (隨機中心) → −40 dB × 1/(1 + dist·0.15) → pan → duck
  _rustle(t, pan, dist){
    const s = this._src(true), bp = this._bp(rnd(800, 2400), 1.4), g = this._g(0);
    const dur = rnd(0.09, 0.18), pk = dB(LV.rustle) / (1 + dist*0.15);
    s.connect(bp); bp.connect(g); this._route(g, { pan, duck: true });
    g.gain.setValueAtTime(0, t);
    g.gain.linearRampToValueAtTime(pk, t + dur*0.25);
    g.gain.linearRampToValueAtTime(0, t + dur);
    s.start(t, rnd(0, 1.8)); s.stop(t + dur + 0.05);
  }
}

window.TheatreAudio = {
  create(){ return new TheatreAudio(); },
};
})();
