/* ══════════════════════════════════════════════════════════════════════════
   Tube3D · 行得入去嘅影片大堂 (WebGL2, 零外部依賴)
   ──────────────────────────────────────────────────────────────────────────
   唔再係一個個視窗 — 每條片變成一塊企喺大堂度嘅立體展板,
   你操控一個角色行過去, 望住邊塊就睇到邊條片嘅資料, 撳 E 就開嚟睇。

   對外介面:
     Tube3D.supported()
     const h = Tube3D.create(canvas, {onFocus, onOpen})
     h.setVideos([{id,title,channel,views,duration,thumb,url,live}, ...])
     h.resize(w,h,dpr)
     h.render(tSec)
     h.state()          → {x, z, yaw, focus, panels}   (畀小地圖用)
   ══════════════════════════════════════════════════════════════════════════ */
(() => {
"use strict";

// ── 迷你 mat4 ─────────────────────────────────────────────────────────────
const M4 = {
  persp(fovy, aspect, near, far){
    const f = 1/Math.tan(fovy/2), nf = 1/(near-far);
    return new Float32Array([f/aspect,0,0,0, 0,f,0,0, 0,0,(far+near)*nf,-1, 0,0,2*far*near*nf,0]);
  },
  lookAt(eye, ctr, up){
    let zx=eye[0]-ctr[0], zy=eye[1]-ctr[1], zz=eye[2]-ctr[2];
    let l = Math.hypot(zx,zy,zz) || 1; zx/=l; zy/=l; zz/=l;
    let xx = up[1]*zz - up[2]*zy, xy = up[2]*zx - up[0]*zz, xz = up[0]*zy - up[1]*zx;
    l = Math.hypot(xx,xy,xz) || 1; xx/=l; xy/=l; xz/=l;
    const yx = zy*xz - zz*xy, yy = zz*xx - zx*xz, yz = zx*xy - zy*xx;
    return new Float32Array([
      xx,yx,zx,0, xy,yy,zy,0, xz,yz,zz,0,
      -(xx*eye[0]+xy*eye[1]+xz*eye[2]),
      -(yx*eye[0]+yy*eye[1]+yz*eye[2]),
      -(zx*eye[0]+zy*eye[1]+zz*eye[2]), 1]);
  },
  mul(a, b){
    const o = new Float32Array(16);
    for (let c = 0; c < 4; c++) for (let r = 0; r < 4; r++) {
      let s = 0;
      for (let k = 0; k < 4; k++) s += a[k*4+r] * b[c*4+k];
      o[c*4+r] = s;
    }
    return o;
  },
  ident(){ return new Float32Array([1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]); },
  transl(x,y,z){ return new Float32Array([1,0,0,0, 0,1,0,0, 0,0,1,0, x,y,z,1]); },
  scale(x,y,z){ return new Float32Array([x,0,0,0, 0,y,0,0, 0,0,z,0, 0,0,0,1]); },
  rotY(a){ const c=Math.cos(a), s=Math.sin(a);
    return new Float32Array([c,0,-s,0, 0,1,0,0, s,0,c,0, 0,0,0,1]); },
  rotX(a){ const c=Math.cos(a), s=Math.sin(a);
    return new Float32Array([1,0,0,0, 0,c,s,0, 0,-s,c,0, 0,0,0,1]); },
  chain(...ms){ return ms.reduce((a,b) => M4.mul(a,b)); },
};

// ── GL 小工具 ─────────────────────────────────────────────────────────────
function sh(gl, type, src){
  const s = gl.createShader(type);
  gl.shaderSource(s, src.trim()); gl.compileShader(s);
  if (!gl.getShaderParameter(s, gl.COMPILE_STATUS))
    throw new Error("shader: " + gl.getShaderInfoLog(s));
  return s;
}
function prog(gl, vs, fs){
  const p = gl.createProgram();
  gl.attachShader(p, sh(gl, gl.VERTEX_SHADER, vs));
  gl.attachShader(p, sh(gl, gl.FRAGMENT_SHADER, fs));
  gl.linkProgram(p);
  if (!gl.getProgramParameter(p, gl.LINK_STATUS))
    throw new Error("link: " + gl.getProgramInfoLog(p));
  p.u = {};
  const n = gl.getProgramParameter(p, gl.ACTIVE_UNIFORMS);
  for (let i = 0; i < n; i++) {
    const nm = gl.getActiveUniform(p, i).name.replace(/\[0\]$/, "");
    p.u[nm] = gl.getUniformLocation(p, nm);
  }
  return p;
}

const H = `#version 300 es
precision highp float;`;

// 一般實心幾何 (地台 / 展板框 / 角色 / 柱)
const VS_SOLID = `${H}
layout(location=0) in vec3 aPos;
layout(location=1) in vec2 aUv;
layout(location=2) in vec3 aNrm;
uniform mat4 uVP, uModel;
out vec2 vUv; out vec3 vN, vW;
void main(){
  vec4 wp = uModel * vec4(aPos,1.0);
  gl_Position = uVP * wp;
  vUv = aUv; vW = wp.xyz;
  vN = mat3(uModel) * aNrm;
}`;

const FS_SOLID = `${H}
in vec2 vUv; in vec3 vN, vW;
uniform vec3 uColor, uEye;
uniform float uTex, uEmit, uGrid, uAlpha;
uniform sampler2D uSampler;
out vec4 o;
void main(){
  float vFog = clamp(1.0 - length(vW - uEye)/95.0, 0.0, 1.0);
  vec3 c = uColor;
  float a = uAlpha;
  if (uTex > 0.5) { vec4 t = texture(uSampler, vUv); c = t.rgb * uColor; a *= t.a; }
  if (uGrid > 0.5) {                        // 地台格網 + 中央走廊光
    vec2 g = abs(fract(vW.xz*0.25) - 0.5) / fwidth(vW.xz*0.25);
    float line = 1.0 - min(min(g.x, g.y), 1.0);
    c += vec3(0.16,0.24,0.46) * line;
    c += vec3(0.05,0.09,0.20) * exp(-abs(vW.x)*0.10);
  }
  if (uEmit < 0.5) {                        // 打燈 (簡單方向光 + 環境光)
    vec3 n = normalize(vN);
    float d = max(0.0, dot(n, normalize(vec3(0.35, 0.9, 0.25))));
    c *= 0.42 + 0.72*d;
  }
  o = vec4(c*vFog*a, a*vFog);
}`;

// 人 (instanced billboard) — 大堂入面行緊嘅其他訪客
const VS_PEOPLE = `${H}
layout(location=0) in vec2 aQuad;
layout(location=1) in vec4 aSeed;   // x: 行邊條走廊, y: 起步位, z: 速度, w: 色相
uniform mat4 uVP; uniform vec3 uRight, uEye;
uniform float uTime, uLen;
out vec2 vUv; out float vFog, vS;
void main(){
  float z = mod(aSeed.y + uTime*aSeed.z + uLen, uLen*2.0) - uLen;   // 行到尾就繞返轉頭
  float dir = aSeed.z > 0.0 ? 1.0 : -1.0;
  float bob = 0.055*abs(sin(uTime*4.2*abs(aSeed.z) + aSeed.y));
  vec3 base = vec3(aSeed.x, bob, z);
  vec3 wp = base + uRight*(aQuad.x*0.86) + vec3(0.0, aQuad.y*1.70, 0.0);
  gl_Position = uVP * vec4(wp,1.0);
  vUv = aQuad + vec2(0.5,0.0); vS = aSeed.w;
  vFog = clamp(1.0 - length(wp-uEye)/95.0, 0.0, 1.0);
  if (dir == 0.0) gl_Position = vec4(2.0,2.0,2.0,1.0);
}`;

const FS_PEOPLE = `${H}
in vec2 vUv; in float vFog, vS;
out vec4 o;
vec3 hue(float h){ return 0.55 + 0.45*cos(6.2831*(h + vec3(0.0,0.33,0.67))); }
void main(){
  float dx = abs(vUv.x-0.5);
  float bw = 0.255*(1.0-0.30*smoothstep(0.58,1.0,vUv.y));
  float body = (1.0-smoothstep(bw-0.015,bw+0.015,dx))*(1.0-smoothstep(0.735,0.775,vUv.y));
  if (vUv.y < 0.30) body *= smoothstep(0.030,0.070,dx);
  float head = 1.0-smoothstep(0.150,0.185, length((vUv-vec2(0.5,0.88))*vec2(1.0,0.82)));
  float a = clamp(max(body,head),0.0,1.0);
  if (a < 0.02) discard;
  vec3 c = hue(vS)*0.55;
  c *= mix(0.5,1.1, smoothstep(0.0,0.55,vUv.y));
  c *= vFog;
  o = vec4(c*a, a*vFog);
}`;

// ══ Hall ══════════════════════════════════════════════════════════════════
const PW = 4.3, PH = PW*9/16, PY = 1.25;      // 展板闊/高/離地
const COLS = 5, DX = 6.6, DZ = 10.0, Z0 = -14;

class Hall {
  constructor(canvas, opts = {}){
    const gl = this.gl = canvas.getContext("webgl2",
      {alpha:false, antialias:true, premultipliedAlpha:true, powerPreference:"high-performance"});
    if (!gl) throw new Error("冇 WebGL2");
    this.cv = canvas; this.opts = opts;
    this.pSolid  = prog(gl, VS_SOLID, FS_SOLID);
    this.pPeople = prog(gl, VS_PEOPLE, FS_PEOPLE);
    this.box   = this._box();
    this.quad  = this._quad();
    this.floor = this._quadXZ();
    this.bb    = this._bb();
    this.videos = []; this.panels = []; this.focus = -1;
    this.len = 120;                 // 大堂長度 (setVideos 之後會按片數再計)
    this.W = 1; this.H = 1;

    // 角色: 第三身。位置 / 面向 / 行路動畫
    this.me = {x: 0, z: 8, yaw: Math.PI, vy: 0, walk: 0, speed: 0};
    this.cam = {yaw: 0, pitch: 0.16, dist: 5.6, fp: false};
    this.keys = new Set();
    this.touch = {mv: null, look: null};
    this._bindInput();
    this._people(opts.people == null ? 320 : Math.max(0, opts.people|0));
  }

  // ── 幾何 ──────────────────────────────────────────────────────────────
  _mesh(v, stride, attrs){
    const gl = this.gl, vao = gl.createVertexArray(), b = gl.createBuffer();
    gl.bindVertexArray(vao);
    gl.bindBuffer(gl.ARRAY_BUFFER, b);
    gl.bufferData(gl.ARRAY_BUFFER, v, gl.STATIC_DRAW);
    for (const [loc, size, off] of attrs) {
      gl.enableVertexAttribArray(loc);
      gl.vertexAttribPointer(loc, size, gl.FLOAT, false, stride*4, off*4);
    }
    gl.bindVertexArray(null);
    return {vao, n: v.length/stride};
  }
  /** 頂部做原點嘅箱 (y ∈ [-1,0]) — 手腳可以繞住關節轉。 */
  _box(){
    const q = [], F = [
      [[-.5,-1,.5],[.5,-1,.5],[.5,0,.5],[-.5,0,.5],[0,0,1]],
      [[.5,-1,-.5],[-.5,-1,-.5],[-.5,0,-.5],[.5,0,-.5],[0,0,-1]],
      [[-.5,0,.5],[.5,0,.5],[.5,0,-.5],[-.5,0,-.5],[0,1,0]],
      [[-.5,-1,-.5],[-.5,-1,.5],[-.5,0,.5],[-.5,0,-.5],[-1,0,0]],
      [[.5,-1,.5],[.5,-1,-.5],[.5,0,-.5],[.5,0,.5],[1,0,0]],
      [[-.5,-1,-.5],[.5,-1,-.5],[.5,-1,.5],[-.5,-1,.5],[0,-1,0]],
    ];
    const uv = [[0,0],[1,0],[1,1],[0,1]];
    for (const f of F) for (const i of [0,1,2, 0,2,3]) q.push(...f[i], ...uv[i], ...f[4]);
    return this._mesh(new Float32Array(q), 8, [[0,3,0],[1,2,3],[2,3,5]]);
  }
  _quad(){                     // 企起身嘅面 (中心原點, 面向 +Z)
    const q = [], p = [[-.5,-.5,0],[.5,-.5,0],[.5,.5,0],[-.5,.5,0]],
          uv = [[0,1],[1,1],[1,0],[0,0]];
    for (const i of [0,1,2, 0,2,3]) q.push(...p[i], ...uv[i], 0,0,1);
    return this._mesh(new Float32Array(q), 8, [[0,3,0],[1,2,3],[2,3,5]]);
  }
  _quadXZ(){                   // 地台
    const q = [], p = [[-.5,0,.5],[.5,0,.5],[.5,0,-.5],[-.5,0,-.5]],
          uv = [[0,0],[1,0],[1,1],[0,1]];
    for (const i of [0,1,2, 0,2,3]) q.push(...p[i], ...uv[i], 0,1,0);
    return this._mesh(new Float32Array(q), 8, [[0,3,0],[1,2,3],[2,3,5]]);
  }
  _bb(){                       // people billboard
    const gl = this.gl, vao = gl.createVertexArray();
    const b = gl.createBuffer();
    gl.bindVertexArray(vao);
    gl.bindBuffer(gl.ARRAY_BUFFER, b);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([
      -.5,0, .5,0, .5,1, -.5,0, .5,1, -.5,1]), gl.STATIC_DRAW);
    gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0,2,gl.FLOAT,false,0,0);
    this.pbuf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, this.pbuf);
    gl.enableVertexAttribArray(1); gl.vertexAttribPointer(1,4,gl.FLOAT,false,0,0);
    gl.vertexAttribDivisor(1,1);
    gl.bindVertexArray(null);
    return {vao, n: 6};
  }
  _people(n){
    const gl = this.gl, a = new Float32Array(n*4);
    let sd = 12345;
    const rnd = () => (sd = (sd*1664525 + 1013904223) >>> 0) / 4294967296;
    for (let i = 0; i < n; i++) {
      // 企喺走廊 (兩排展板中間) 度行嚟行去
      const lane = (Math.floor(rnd()*(COLS+1)) - COLS/2) * DX;
      a[i*4]   = lane + (rnd()-0.5)*2.6;
      a[i*4+1] = rnd()*400 - 200;
      a[i*4+2] = (rnd() < 0.5 ? -1 : 1) * (0.7 + rnd()*1.5);
      a[i*4+3] = rnd();
    }
    gl.bindBuffer(gl.ARRAY_BUFFER, this.pbuf);
    gl.bufferData(gl.ARRAY_BUFFER, a, gl.STATIC_DRAW);
    this.peopleN = n;
  }

  // ── 每條片整一塊展板 (縮圖 + 標題整落一張 texture) ────────────────────
  setVideos(list){
    const gl = this.gl;
    for (const p of this.panels) if (p.tex) gl.deleteTexture(p.tex);
    this.videos = list;
    this.panels = list.map((v, i) => {
      const col = i % COLS, row = (i / COLS) | 0;
      // 排頭嗰啲擺近門口, 一行行向入面排
      const x = (col - (COLS-1)/2) * DX;
      const z = Z0 - row * DZ;
      // 人氣愈高塊板愈大 (最多大 45%)
      const mx = Math.max(1, ...list.map(a => a.views || 1));
      const sc = 1 + 0.45 * Math.pow((v.views || 1) / mx, 0.5);
      return {v, x, z, w: PW*sc, h: PH*sc, y: PY, tex: this._tex(v), i};
    });
    this.len = Math.max(60, (Math.ceil(list.length/COLS)) * DZ + 30);
    this.focus = -1;
  }

  /** 一塊板一張 texture: 上面縮圖, 下面標題/頻道/觀看數。 */
  _tex(v){
    const gl = this.gl;
    const cv = document.createElement("canvas");
    cv.width = 640; cv.height = 480;
    const x = cv.getContext("2d");
    const tex = gl.createTexture();
    const draw = (img) => {
      // 縮圖區 (16:9)
      x.fillStyle = "#0b0d14"; x.fillRect(0,0,640,480);
      if (img) {
        x.drawImage(img, 0, 0, 640, 360);
      } else {
        const g = x.createLinearGradient(0,0,640,360);
        const h = ((v.title || "").length * 37) % 360;
        g.addColorStop(0, `hsl(${h},58%,32%)`); g.addColorStop(1, `hsl(${(h+48)%360},52%,14%)`);
        x.fillStyle = g; x.fillRect(0,0,640,360);
        x.fillStyle = "rgba(255,255,255,.16)";
        x.beginPath(); x.moveTo(285,150); x.lineTo(285,210); x.lineTo(345,180); x.fill();
      }
      // 標題列
      x.fillStyle = "#0d1017"; x.fillRect(0,360,640,120);
      x.fillStyle = "#fff";
      x.font = "700 30px -apple-system,'PingFang HK','Microsoft JhengHei',sans-serif";
      const words = String(v.title || "").split("");
      let line = "", ln = 0;
      for (const ch of words) {
        if (x.measureText(line + ch).width > 600) {
          x.fillText(line, 16, 398 + ln*36); line = ch; ln++;
          if (ln > 1) { line = line + "…"; break; }
        } else line += ch;
      }
      x.fillText(line, 16, 398 + ln*36);
      x.font = "600 24px -apple-system,'PingFang HK',sans-serif";
      x.fillStyle = "#8b949e";
      const views = (v.views || 0) >= 1e4
        ? ((v.views/1e4).toFixed(1) + "萬次") : ((v.views||0) + "次");
      x.fillText(`${(v.channel||"").slice(0,14)} · ${views}`, 16, 468);
      if (v.live) { x.fillStyle = "#ff3355"; x.fillRect(596,368,32,14);
                    x.fillStyle="#fff"; x.font="700 11px sans-serif"; x.fillText("LIVE",600,379); }

      gl.bindTexture(gl.TEXTURE_2D, tex);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, cv);
      gl.generateMipmap(gl.TEXTURE_2D);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR_MIPMAP_LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    };
    draw(null);                              // 先出佔位卡, 有縮圖再換
    if (v.thumb) {
      const img = new Image();
      // ⚠️ 一定要 CORS: 冇 ACAO 嘅圖會載入失敗 (而唔係污染 canvas), 咁就用返佔位卡
      img.crossOrigin = "anonymous";
      img.onload = () => { try { draw(img); } catch (e) { /* 污染就照用佔位卡 */ } };
      img.src = v.thumb;
    }
    return tex;
  }

  // ── 輸入 ──────────────────────────────────────────────────────────────
  _bindInput(){
    const cv = this.cv;
    addEventListener("keydown", e => {
      const k = e.key.toLowerCase();
      this.keys.add(k);
      if (k === "e" || k === "enter") this._open();
      if (k === "v") this.cam.fp = !this.cam.fp;
      if (" wasd".includes(k) || k.startsWith("arrow")) e.preventDefault();
    });
    addEventListener("keyup", e => this.keys.delete(e.key.toLowerCase()));

    cv.addEventListener("click", () => {
      if (document.pointerLockElement === cv) this._open();
      else cv.requestPointerLock && cv.requestPointerLock();
    });
    addEventListener("mousemove", e => {
      if (document.pointerLockElement !== cv) return;
      this.cam.yaw   -= e.movementX * 0.0025;
      this.cam.pitch  = Math.max(-0.55, Math.min(0.85, this.cam.pitch - e.movementY*0.002));
    });
    cv.addEventListener("wheel", e => {
      e.preventDefault();
      this.cam.dist = Math.max(1.2, Math.min(12, this.cam.dist * Math.exp(e.deltaY*0.001)));
    }, {passive:false});

    // 觸控: 左邊 = 行, 右邊 = 望
    cv.style.touchAction = "none";
    const t = this.touch;
    cv.addEventListener("pointerdown", e => {
      if (e.pointerType === "mouse") return;
      const left = e.clientX < innerWidth/2;
      (left ? (t.mv = {id:e.pointerId, x0:e.clientX, y0:e.clientY, dx:0, dy:0})
            : (t.look = {id:e.pointerId, x:e.clientX, y:e.clientY}));
      cv.setPointerCapture(e.pointerId);
    });
    cv.addEventListener("pointermove", e => {
      if (t.mv && e.pointerId === t.mv.id) {
        t.mv.dx = Math.max(-1, Math.min(1, (e.clientX - t.mv.x0)/60));
        t.mv.dy = Math.max(-1, Math.min(1, (e.clientY - t.mv.y0)/60));
      } else if (t.look && e.pointerId === t.look.id) {
        this.cam.yaw   -= (e.clientX - t.look.x) * 0.006;
        this.cam.pitch  = Math.max(-0.55, Math.min(0.85, this.cam.pitch - (e.clientY-t.look.y)*0.004));
        t.look.x = e.clientX; t.look.y = e.clientY;
      }
    });
    const up = e => {
      if (t.mv && e.pointerId === t.mv.id) t.mv = null;
      if (t.look && e.pointerId === t.look.id) t.look = null;
    };
    cv.addEventListener("pointerup", up); cv.addEventListener("pointercancel", up);
  }
  _open(){
    if (this.focus >= 0 && this.opts.onOpen) this.opts.onOpen(this.panels[this.focus].v);
  }

  resize(w, h, dpr){
    this.W = Math.max(1, Math.round(w*dpr)); this.H = Math.max(1, Math.round(h*dpr));
    this.cv.width = this.W; this.cv.height = this.H;
  }
  state(){
    return {x: this.me.x, z: this.me.z, yaw: this.cam.yaw,
            focus: this.focus, panels: this.panels};
  }

  // ── 每格 ──────────────────────────────────────────────────────────────
  render(t){
    const gl = this.gl;
    const dt = Math.min(0.05, this._lt ? t - this._lt : 0.016);
    this._lt = t;
    this._move(dt);

    const me = this.me, cam = this.cam;
    const eyeH = 1.62;
    const cp = Math.cos(cam.pitch), sp = Math.sin(cam.pitch);
    const head = [me.x, eyeH, me.z];
    let eye;
    if (cam.fp) {
      eye = [me.x, eyeH, me.z];
    } else {
      const rx = Math.cos(cam.yaw), rz = -Math.sin(cam.yaw);   // 鏡頭右手邊
      eye = [me.x + cam.dist*cp*Math.sin(cam.yaw) + rx*0.95,
             eyeH + cam.dist*sp + 0.55,
             me.z + cam.dist*cp*Math.cos(cam.yaw) + rz*0.95];
      eye[1] = Math.max(0.45, eye[1]);
      head[0] += rx*0.95; head[2] += rz*0.95;                   // 望嘅目標一齊移
    }
    const dir = [-cp*Math.sin(cam.yaw), sp, -cp*Math.cos(cam.yaw)];
    const ctr = cam.fp ? [eye[0]+dir[0], eye[1]+dir[1], eye[2]+dir[2]] : head;
    const view = M4.lookAt(eye, ctr, [0,1,0]);
    const VP = M4.mul(M4.persp(1.15, this.W/this.H, 0.1, 400), view);
    const right = [view[0], 0, view[8]];
    const rl = Math.hypot(right[0], right[2]) || 1; right[0]/=rl; right[2]/=rl;

    this._pick(cam.fp ? eye : head, dir);

    gl.viewport(0,0,this.W,this.H);
    gl.clearColor(0.016, 0.022, 0.043, 1);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    gl.enable(gl.DEPTH_TEST); gl.depthFunc(gl.LEQUAL);
    gl.enable(gl.BLEND); gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
    gl.disable(gl.CULL_FACE);

    const P = this.pSolid;
    gl.useProgram(P);
    gl.uniformMatrix4fv(P.u.uVP, false, VP);
    gl.uniform3fv(P.u.uEye, eye);
    gl.uniform1f(P.u.uAlpha, 1);
    const set = (col, {tex=0, emit=0, grid=0} = {}) => {
      gl.uniform3fv(P.u.uColor, col);
      gl.uniform1f(P.u.uTex, tex); gl.uniform1f(P.u.uEmit, emit); gl.uniform1f(P.u.uGrid, grid);
    };

    // 地台
    set([0.085,0.10,0.155], {emit:1, grid:1});
    gl.uniformMatrix4fv(P.u.uModel, false, M4.chain(M4.transl(0,0,-this.len/2+20), M4.scale(360,1,this.len*2)));
    gl.bindVertexArray(this.floor.vao);
    gl.drawArrays(gl.TRIANGLES, 0, this.floor.n);

    // 展板
    gl.bindVertexArray(this.quad.vao);
    for (const p of this.panels) {
      const on = p.i === this.focus;
      // 外框 (focus 時着金光)
      set(on ? [1.0,0.80,0.35] : [0.16,0.19,0.28], {emit:1});
      gl.uniformMatrix4fv(P.u.uModel, false,
        M4.chain(M4.transl(p.x, p.y + p.h/2, p.z + 0.02), M4.scale(p.w*1.06, p.h*1.10, 1)));
      gl.drawArrays(gl.TRIANGLES, 0, this.quad.n);
      // 畫面
      gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D, p.tex);
      gl.uniform1i(P.u.uSampler, 0);
      set(on ? [1.25,1.25,1.25] : [0.95,0.95,0.95], {tex:1, emit:1});
      gl.uniformMatrix4fv(P.u.uModel, false,
        M4.chain(M4.transl(p.x, p.y + p.h/2, p.z + 0.04), M4.scale(p.w, p.h, 1)));
      gl.drawArrays(gl.TRIANGLES, 0, this.quad.n);
    }
    // 支架
    gl.bindVertexArray(this.box.vao);
    set([0.10,0.12,0.18]);
    for (const p of this.panels) {
      gl.uniformMatrix4fv(P.u.uModel, false,
        M4.chain(M4.transl(p.x, p.y, p.z), M4.scale(0.22, p.y, 0.22)));
      gl.drawArrays(gl.TRIANGLES, 0, this.box.n);
    }

    // 其他訪客
    const PP = this.pPeople;
    gl.useProgram(PP);
    gl.uniformMatrix4fv(PP.u.uVP, false, VP);
    gl.uniform3fv(PP.u.uRight, right);
    gl.uniform3fv(PP.u.uEye, eye);
    gl.uniform1f(PP.u.uTime, t);
    gl.uniform1f(PP.u.uLen, this.len);
    if (this.peopleN > 0) {
      gl.bindVertexArray(this.bb.vao);
      gl.drawArraysInstanced(gl.TRIANGLES, 0, 6, this.peopleN);
    }

    // 你自己 (第三身先畫)
    if (!cam.fp) this._drawMe(P, VP, eye, set);
    gl.bindVertexArray(null);
  }

  /** 你操控嘅角色: 幾個箱砌成, 手腳跟住行路擺。 */
  _drawMe(P, VP, eye, set){
    const gl = this.gl, me = this.me;
    gl.useProgram(P);
    gl.uniformMatrix4fv(P.u.uVP, false, VP);
    gl.uniform3fv(P.u.uEye, eye);
    gl.uniform1f(P.u.uAlpha, 1);
    gl.bindVertexArray(this.box.vao);
    const base = M4.chain(M4.transl(me.x, 0, me.z), M4.rotY(me.yaw));
    const sw = Math.sin(me.walk) * Math.min(1, me.speed/4) * 0.75;
    const part = (tx,ty,tz, rx, sx,sy,sz, col) => {
      set(col);
      gl.uniformMatrix4fv(P.u.uModel, false,
        M4.chain(base, M4.transl(tx,ty,tz), M4.rotX(rx), M4.scale(sx,sy,sz)));
      gl.drawArrays(gl.TRIANGLES, 0, this.box.n);
    };
    part(0, 1.60, 0,  0,      0.62, 0.80, 0.34, [0.95,0.30,0.28]);   // 身
    part(0, 2.02, 0,  0,      0.44, 0.42, 0.42, [0.98,0.82,0.66]);   // 頭
    part(-0.40, 1.56, 0,  sw,  0.19, 0.62, 0.19, [0.98,0.82,0.66]);  // 左手
    part( 0.40, 1.56, 0, -sw,  0.19, 0.62, 0.19, [0.98,0.82,0.66]);  // 右手
    part(-0.17, 0.80, 0, -sw,  0.23, 0.80, 0.24, [0.16,0.22,0.42]);  // 左腳
    part( 0.17, 0.80, 0,  sw,  0.23, 0.80, 0.24, [0.16,0.22,0.42]);  // 右腳
  }

  // ── 行路 + 撞板 ───────────────────────────────────────────────────────
  _move(dt){
    const me = this.me, k = this.keys, t = this.touch;
    let fx = 0, fz = 0;
    if (k.has("w") || k.has("arrowup"))    fz -= 1;
    if (k.has("s") || k.has("arrowdown"))  fz += 1;
    if (k.has("a") || k.has("arrowleft"))  fx -= 1;
    if (k.has("d") || k.has("arrowright")) fx += 1;
    if (t.mv) { fx += t.mv.dx; fz += t.mv.dy; }
    const run = k.has("shift") ? 2.0 : 1;
    const l = Math.hypot(fx, fz);
    if (l > 0.05) {
      fx /= l; fz /= l;
      // 相對鏡頭方向行: 鏡頭前 F = (-sin,0,-cos), 鏡頭右 R = (cos,0,-sin)
      const s = Math.sin(this.cam.yaw), c = Math.cos(this.cam.yaw);
      const fwd = -fz, rgt = fx;
      const wx = -s*fwd + c*rgt, wz = -c*fwd - s*rgt;
      const sp = 4.6 * run;
      const nx = me.x + wx*sp*dt, nz = me.z + wz*sp*dt;
      const [cx, cz] = this._collide(me.x, me.z, nx, nz);
      me.x = cx; me.z = cz;
      me.yaw = this._lerpAngle(me.yaw, Math.atan2(wx, wz), 1 - Math.pow(0.001, dt));
      me.speed = sp; me.walk += dt * sp * 2.4;
    } else {
      me.speed += (0 - me.speed) * 0.2;
      me.walk += dt * 0.6;
    }
    if (!isFinite(me.x) || !isFinite(me.z)) { me.x = 0; me.z = 8; }
    // 唔好行出大堂
    const halfW = COLS*DX/2 + 9;
    me.x = Math.max(-halfW, Math.min(halfW, me.x));
    me.z = Math.max(-this.len + 6, Math.min(14, me.z));
  }
  _collide(ox, oz, nx, nz){
    const R = 0.55;
    for (const p of this.panels) {
      const hw = p.w/2 + R, hd = 0.45 + R;
      if (Math.abs(nx - p.x) < hw && Math.abs(nz - p.z) < hd) {
        // 邊邊插得少啲就由邊邊推返出去
        const px = Math.abs(nx - p.x) - hw, pz = Math.abs(nz - p.z) - hd;
        if (px > pz) nx = ox; else nz = oz;
      }
    }
    return [nx, nz];
  }
  _lerpAngle(a, b, t){
    let d = ((b - a + Math.PI) % (2*Math.PI) + 2*Math.PI) % (2*Math.PI) - Math.PI;
    return a + d*t;
  }

  /** 望住邊塊板: 前方最近、角度最細嗰塊。 */
  _pick(from, dir){
    let best = -1, bestScore = 0.20;      // cos 門檻 0.62 ≈ 52°
    for (const p of this.panels) {
      const dx = p.x - from[0], dz = p.z - from[2];
      const dist = Math.hypot(dx, dz);
      if (dist > 22) continue;
      const dot = (dx*dir[0] + dz*dir[2]) / (dist || 1);
      const score = dot - dist*0.030;
      if (dot > 0.62 && score > bestScore) { bestScore = score; best = p.i; }
    }
    if (best !== this.focus) {
      this.focus = best;
      if (this.opts.onFocus) this.opts.onFocus(best >= 0 ? this.panels[best].v : null, best);
    }
  }
}

window.Tube3D = {
  supported(){
    try { return !!document.createElement("canvas").getContext("webgl2"); }
    catch (e) { return false; }
  },
  create(cv, opts){ return new Hall(cv, opts); },
};
})();
