/* ══════════════════════════════════════════════════════════════════════════
   Streamer Arena · 3D 場館渲染器
   ──────────────────────────────────────────────────────────────────────────
   用原生 WebGL2 instancing 寫, 零外部依賴 (唔使 three.js), OBS Browser Source
   一樣行到。一個 instance = 一個觀眾, 幾萬人都係一個 draw call。

   ‧ 觀眾識郁: 企喺度郁/擰身/跳, 人浪由舞台一圈圈掃出去, 手上面有燈棒揈
   ‧ 直播主識郁: 喺台上行嚟行去 + 跳 + 追光燈跟住佢
   ‧ 你可以喺場館入面行: 拖 = 轉視角, 滾輪/兩指 = 由高空睇落去 ↔ 企入人堆中間,
     WASD / 方向鍵 = 行過去

   對外介面:
     Arena3D.supported()              → 有冇 WebGL2
     const r = Arena3D.create(canvas, {mode})
     r.setScene({pts, dots, R, R0, FAN, color, viewers, perDot, name, platform, venues})
     r.resize(w, h, dpr)
     r.render(tSec)
     r.dispose()
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
  mul(a, b){                       // a * b
    const o = new Float32Array(16);
    for (let c = 0; c < 4; c++) for (let r = 0; r < 4; r++) {
      let s = 0;
      for (let k = 0; k < 4; k++) s += a[k*4+r] * b[c*4+k];
      o[c*4+r] = s;
    }
    return o;
  },
  trs(tx,ty,tz, sx,sy,sz){
    return new Float32Array([sx,0,0,0, 0,sy,0,0, 0,0,sz,0, tx,ty,tz,1]);
  },
};

// ── GL 小工具 ─────────────────────────────────────────────────────────────
function sh(gl, type, src){
  const s = gl.createShader(type);
  gl.shaderSource(s, src.trim()); gl.compileShader(s);
  if (!gl.getShaderParameter(s, gl.COMPILE_STATUS))
    throw new Error("shader: " + gl.getShaderInfoLog(s) + "\n" + src);
  return s;
}
function prog(gl, vs, fs){
  const p = gl.createProgram();
  gl.attachShader(p, sh(gl, gl.VERTEX_SHADER, vs));
  gl.attachShader(p, sh(gl, gl.FRAGMENT_SHADER, fs));
  gl.linkProgram(p);
  if (!gl.getProgramParameter(p, gl.LINK_STATUS))
    throw new Error("link: " + gl.getProgramInfoLog(p));
  const u = {};
  const n = gl.getProgramParameter(p, gl.ACTIVE_UNIFORMS);
  for (let i = 0; i < n; i++) {
    const nm = gl.getActiveUniform(p, i).name.replace(/\[0\]$/, "");
    u[nm] = gl.getUniformLocation(p, nm);
  }
  p.u = u;
  return p;
}
const hex2rgb = h => {
  const m = /^#?([0-9a-f]{6})$/i.exec(h || "#9146ff");
  const v = m ? parseInt(m[1], 16) : 0x9146ff;
  return [(v>>16&255)/255, (v>>8&255)/255, (v&255)/255];
};

// ══ Shaders ═══════════════════════════════════════════════════════════════
const COMMON = `#version 300 es
precision highp float;`;

// 觀眾 / 直播主 (billboard, GPU 上面做動作)
const VS_CROWD = `${COMMON}
layout(location=0) in vec2 aQuad;      // -0.5..0.5 , 0..1
layout(location=1) in vec3 aPos;       // 腳踩喺邊 (world)
layout(location=2) in float aSeed;     // 0..1 每個人唔同
uniform mat4 uVP;
uniform vec3 uRight, uEye;
uniform float uTime, uWave, uEnergy, uSizeX, uSizeY, uMode, uFog;
out vec2 vUv; out float vSeed, vLit, vFog;
void main(){
  float ph = aSeed * 6.2831853;
  float r  = length(aPos.xz);
  float d  = r - uWave;
  float w  = exp(-d*d/110.0);                       // 人浪光帶
  // 企喺度郁: 上下彈 + 左右擰; 人浪掃到就跳起
  float jump = w*0.9*uEnergy + 0.055*(0.5+0.5*sin(uTime*2.4+ph))*uEnergy;
  float sway = 0.10*sin(uTime*1.15+ph)*uEnergy;
  vec3 base  = aPos + vec3(0.0, jump, 0.0) + uRight*sway;
  float sx = uSizeX, sy = uSizeY;
  if (uMode > 0.5) {                                // 燈棒 (只係部分人有)
    if (fract(aSeed*17.13) > 0.42) { gl_Position = vec4(2.0,2.0,2.0,1.0); return; }
    base += vec3(0.0, uSizeY*1.00, 0.0) + uRight*(0.42*sin(uTime*2.0+ph)) ;
    sx = uSizeX*0.30; sy = uSizeY*0.30;
  }
  vec3 wp = base + uRight*(aQuad.x*sx) + vec3(0.0, aQuad.y*sy, 0.0);
  gl_Position = uVP * vec4(wp, 1.0);
  vUv = aQuad + vec2(0.5, 0.0);
  vSeed = aSeed; vLit = w;
  vFog = clamp(1.0 - length(wp - uEye)/uFog, 0.0, 1.0);
}`;

const FS_CROWD = `${COMMON}
in vec2 vUv; in float vSeed, vLit, vFog;
uniform vec3 uColor;
uniform float uMode;
out vec4 o;
float h(float x){ return fract(sin(x*127.1)*43758.5453); }
void main(){
  vec2 uv = vUv;
  if (uMode > 0.5) {                                 // 燈棒: 一舊光
    float dd = length((uv - vec2(0.5,0.5)) * vec2(2.4,1.0));
    float a = smoothstep(1.0, 0.05, dd);
    vec3 c = mix(uColor, vec3(1.0), 0.72);
    o = vec4(c * a * 1.35 * vFog, a*vFog);
    return;
  }
  // 人形: 頭 + 身 + 兩條腿。用 smoothstep 做柔邊 —
  //  ⚠️ alpha 唔可以乘霧, 否則 alpha-to-coverage 會喺身上面拉出一條條橫紋。
  float dx   = abs(uv.x - 0.5);
  float bw   = 0.165 * (1.0 - 0.30*smoothstep(0.58,1.0,uv.y));
  float body = (1.0 - smoothstep(bw-0.015, bw+0.015, dx))
             * (1.0 - smoothstep(0.735, 0.775, uv.y));
  if (uv.y < 0.30) body *= smoothstep(0.020, 0.048, dx);            // 兩條腿
  float head = 1.0 - smoothstep(0.100, 0.128,
                     length((uv-vec2(0.5,0.88))*vec2(1.0,0.82)));
  float a = clamp(max(body, head), 0.0, 1.0);
  if (a < 0.02) discard;
  float t  = 0.62 + 0.85*h(vSeed*31.7);              // 每個人光暗唔同
  vec3 c   = uColor * t;
  c = mix(c, vec3(1.0), 0.10 + 0.55*vLit);           // 人浪掃過就着
  c *= mix(0.55, 1.15, smoothstep(0.0, 0.55, uv.y)); // 腳暗頭光, 出到層次
  c *= 0.72 + 0.28*(1.0 - dx*2.0);                   // 側面暗少少, 冇咁扁平
  c *= vFog;                                         // 遠嘅暗啲 (霧只食色, 唔食 alpha)
  o = vec4(c*a, a);
}`;

// 場館碗形地台 (連場館容量圈)
const VS_BOWL = `${COMMON}
layout(location=0) in vec3 aPos;
uniform mat4 uVP; uniform vec3 uEye; uniform float uFog;
out float vR, vFog; out vec3 vW;
void main(){
  vR = length(aPos.xz);
  vW = aPos;
  gl_Position = uVP * vec4(aPos,1.0);
  vFog = clamp(1.0 - length(aPos-uEye)/uFog, 0.0, 1.0);
}`;

const FS_BOWL = `${COMMON}
in float vR, vFog; in vec3 vW;
uniform vec3 uColor;
uniform float uRings[8];      // 場館容量圈半徑
uniform float uFilled[8];     // 坐滿咗未
uniform float uUnit;          // 主單位 (紅館) 嗰個圈半徑
uniform int   uN;
out vec4 o;
void main(){
  vec3 c = uColor*0.10 + vec3(0.035,0.04,0.065);
  float ring = 0.0, gold = 0.0;
  for (int i = 0; i < 8; i++) {
    if (i >= uN) break;
    float d = abs(vR - uRings[i]);
    float w = max(0.35, uRings[i]*0.006);
    float s = smoothstep(w, 0.0, d);
    if (abs(uRings[i]-uUnit) < 0.001) gold = max(gold, s);
    else ring = max(ring, s * (uFilled[i] > 0.5 ? 0.5 : 0.22));
  }
  // 主單位 (紅館) 範圍內鋪一層金
  if (vR < uUnit) c += vec3(0.16,0.13,0.06)*0.55;
  c += vec3(1.0)*ring*0.5 + vec3(1.0,0.83,0.47)*gold*0.9;
  o = vec4(c*vFog, vFog);
}`;

// 純色 / 發光幾何 (舞台、LED 幕、光柱)
const VS_SOLID = `${COMMON}
layout(location=0) in vec3 aPos;
layout(location=1) in vec2 aUv;
uniform mat4 uVP, uModel; uniform vec3 uEye; uniform float uFog;
out vec2 vUv; out float vFog;
void main(){
  vec4 wp = uModel * vec4(aPos,1.0);
  gl_Position = uVP * wp;
  vUv = aUv;
  vFog = clamp(1.0 - length(wp.xyz-uEye)/uFog, 0.0, 1.0);
}`;

const FS_SOLID = `${COMMON}
in vec2 vUv; in float vFog;
uniform vec3 uColor; uniform float uAlpha, uGlow, uTex;
uniform sampler2D uSampler;
out vec4 o;
void main(){
  vec3 c = uColor;
  float a = uAlpha;
  if (uTex > 0.5) { vec4 t = texture(uSampler, vUv); c = t.rgb; a *= t.a; }
  if (uGlow > 0.5) { a *= (1.0 - vUv.y);  c *= 1.4; }     // 光柱: 由上到下散開
  float A = a*vFog;
  o = vec4(c*A, A);
}`;

// ══ Renderer ══════════════════════════════════════════════════════════════
class Renderer {
  constructor(canvas, opts = {}){
    const gl = this.gl = canvas.getContext("webgl2", {
      alpha: true, antialias: false, premultipliedAlpha: true,
      powerPreference: "high-performance", depth: true,
    });
    if (!gl) throw new Error("冇 WebGL2");
    this.canvas = canvas;
    this.opts = opts;
    this.W = 1; this.H = 1;

    this.pCrowd = prog(gl, VS_CROWD, FS_CROWD);
    this.pBowl  = prog(gl, VS_BOWL,  FS_BOWL);
    this.pSolid = prog(gl, VS_SOLID, FS_SOLID);

    // 一塊 billboard quad (所有人共用)
    this.quad = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, this.quad);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([
      -0.5,0, 0.5,0, 0.5,1, -0.5,0, 0.5,1, -0.5,1]), gl.STATIC_DRAW);

    this.inst = gl.createBuffer();        // 觀眾 (x,y,z,seed)
    this.instN = 0;
    this.star = gl.createBuffer();        // 直播主 (1 個)
    gl.bindBuffer(gl.ARRAY_BUFFER, this.star);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(4), gl.DYNAMIC_DRAW);

    this.vaoCrowd = this._mkCrowdVao(this.inst);
    this.vaoStar  = this._mkCrowdVao(this.star);

    this.box    = this._mkBox();
    this.plane  = this._mkPlane();
    this.cone   = this._mkCone();
    this.screenTex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, this.screenTex);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);

    // ── 鏡頭 (你可以喺場館入面郁) ────────────────────────────────────────
    this.cam = { yaw: 0, pitch: 0.38, dist: 200, tx: 0, ty: 4, tz: 60 };
    this.auto = opts.cam !== "free";
    this.lastInput = -1e9;
    this.keys = new Set();
    this.scene = null;
    // 自適應畫質: 跑唔郁就少畫幾成人 (畫面會誠實講返 1 個公仔 = 幾多人)
    this.renderN = 0; this.frameMs = 16; this._adjT = 0; this._adjN = 0;
    this.onQuality = opts.onQuality || null;
    this._bindInput();
  }

  // ── 幾何 ──────────────────────────────────────────────────────────────
  _mkCrowdVao(instBuf){
    const gl = this.gl, v = gl.createVertexArray();
    gl.bindVertexArray(v);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.quad);
    gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
    gl.bindBuffer(gl.ARRAY_BUFFER, instBuf);
    gl.enableVertexAttribArray(1); gl.vertexAttribPointer(1, 3, gl.FLOAT, false, 16, 0);
    gl.vertexAttribDivisor(1, 1);
    gl.enableVertexAttribArray(2); gl.vertexAttribPointer(2, 1, gl.FLOAT, false, 16, 12);
    gl.vertexAttribDivisor(2, 1);
    gl.bindVertexArray(null);
    return v;
  }
  _mkMesh(verts, stride, attrs){
    const gl = this.gl, v = gl.createVertexArray(), b = gl.createBuffer();
    gl.bindVertexArray(v);
    gl.bindBuffer(gl.ARRAY_BUFFER, b);
    gl.bufferData(gl.ARRAY_BUFFER, verts, gl.STATIC_DRAW);
    for (const [loc, size, off] of attrs) {
      gl.enableVertexAttribArray(loc);
      gl.vertexAttribPointer(loc, size, gl.FLOAT, false, stride*4, off*4);
    }
    gl.bindVertexArray(null);
    return { vao: v, n: verts.length / stride };
  }
  _mkBox(){
    const q = [], push = (p, u) => q.push(...p, ...u);
    const F = [[[-.5,0,.5],[.5,0,.5],[.5,1,.5],[-.5,1,.5]],
               [[.5,0,-.5],[-.5,0,-.5],[-.5,1,-.5],[.5,1,-.5]],
               [[-.5,1,.5],[.5,1,.5],[.5,1,-.5],[-.5,1,-.5]],
               [[-.5,0,-.5],[-.5,0,.5],[-.5,1,.5],[-.5,1,-.5]],
               [[.5,0,.5],[.5,0,-.5],[.5,1,-.5],[.5,1,.5]]];
    for (const f of F) {
      const uv = [[0,0],[1,0],[1,1],[0,1]];
      for (const i of [0,1,2, 0,2,3]) push(f[i], uv[i]);
    }
    return this._mkMesh(new Float32Array(q), 5, [[0,3,0],[1,2,3]]);
  }
  _mkPlane(){
    const q = [];
    const p = [[-.5,0,0],[.5,0,0],[.5,1,0],[-.5,1,0]], uv = [[0,1],[1,1],[1,0],[0,0]];
    for (const i of [0,1,2, 0,2,3]) q.push(...p[i], ...uv[i]);
    return this._mkMesh(new Float32Array(q), 5, [[0,3,0],[1,2,3]]);
  }
  _mkRing(FAN){                    // 場館容量圈: 一堵企起身嘅環形光牆
    const q = [], N = 64;
    const a0 = Math.PI/2 - FAN/2, a1 = Math.PI/2 + FAN/2;
    for (let i = 0; i < N; i++) {
      const t0 = a0 + (a1-a0)*i/N, t1 = a0 + (a1-a0)*(i+1)/N;
      const p0 = [Math.cos(t0), 0, Math.sin(t0)], p1 = [Math.cos(t1), 0, Math.sin(t1)];
      const q0 = [p0[0], 1, p0[2]], q1 = [p1[0], 1, p1[2]];
      q.push(...p0,0,0, ...p1,1,0, ...q1,1,1, ...p0,0,0, ...q1,1,1, ...q0,0,1);
    }
    return this._mkMesh(new Float32Array(q), 5, [[0,3,0],[1,2,3]]);
  }
  _mkCone(){                       // 光柱: 頂點喺原點, 向下散開
    const q = [], N = 20;
    for (let i = 0; i < N; i++) {
      const a0 = i/N*6.2831853, a1 = (i+1)/N*6.2831853;
      q.push(0,0,0, 0.5,0);
      q.push(Math.cos(a0), -1, Math.sin(a0), 0.5, 1);
      q.push(Math.cos(a1), -1, Math.sin(a1), 0.5, 1);
    }
    return this._mkMesh(new Float32Array(q), 5, [[0,3,0],[1,2,3]]);
  }
  _mkBowl(R0, R, FAN, slope){
    const gl = this.gl;
    if (this.bowl) { gl.deleteVertexArray(this.bowl.vao); }
    const q = [], NA = 60, NR = 70;
    const a0 = Math.PI/2 - FAN/2 - 0.05, a1 = Math.PI/2 + FAN/2 + 0.05;
    const rr = i => {
      const t = i/NR;
      return R0*0.35 + (R*1.07 - R0*0.35) * t*t;    // 近舞台密啲
    };
    const P = (ai, ri) => {
      const a = a0 + (a1-a0)*ai/NA, r = rr(ri);
      return [Math.cos(a)*r, Math.max(0, r-R0)*slope, Math.sin(a)*r];
    };
    for (let i = 0; i < NA; i++) for (let j = 0; j < NR; j++) {
      const p00 = P(i,j), p10 = P(i+1,j), p11 = P(i+1,j+1), p01 = P(i,j+1);
      q.push(...p00, ...p10, ...p11, ...p00, ...p11, ...p01);
    }
    this.bowl = this._mkMesh(new Float32Array(q), 3, [[0,3,0]]);
  }

  // ── 換直播主 / 換人數 ─────────────────────────────────────────────────
  setScene(s){
    const gl = this.gl;
    this.scene = s;
    const n = s.dots, pts = s.pts;
    const slope = 0.50, R0 = s.R0, R = s.R;
    this.slope = slope;

    // 觀眾位置: 2D 個扇形直接搬上 3D, 再加碗形高度 (後排高啲先睇到台)
    // ⚠️ 打亂次序: 電腦跑唔郁嗰陣我哋會少畫幾成人 (adaptive), 洗過牌之後
    //    「畫頭 K 個」= 全場均勻抽樣, 個場唔會縮水, 只係人疏咗。
    const order = new Int32Array(n);
    for (let i = 0; i < n; i++) order[i] = i;
    let sd = 987654321;
    const rnd = () => (sd = (sd*1664525 + 1013904223) >>> 0) / 4294967296;
    for (let i = n - 1; i > 0; i--) {
      const j = (rnd() * (i+1)) | 0;
      const t = order[i]; order[i] = order[j]; order[j] = t;
    }
    const buf = new Float32Array(n * 4);
    for (let k = 0; k < n; k++) {
      const i = order[k];
      const x = pts[i*2], z = pts[i*2+1];
      const r = Math.hypot(x, z);
      buf[k*4] = x; buf[k*4+1] = Math.max(0, r - R0) * slope; buf[k*4+2] = z;
      buf[k*4+3] = ((Math.sin(i*12.9898)*43758.5453) % 1 + 1) % 1;
    }
    gl.bindBuffer(gl.ARRAY_BUFFER, this.inst);
    gl.bufferData(gl.ARRAY_BUFFER, buf, gl.STATIC_DRAW);
    this.instN = n;
    // 起手保守 (12000), 機夠力就自己加上去; 之前調低咗嘅就唔好推翻
    this.renderN = Math.min(n, this.renderN || Math.min(n, 12000));

    this._mkBowl(R0, R, s.FAN, slope);
    if (!this.ring) this.ring = this._mkRing(s.FAN);
    this._mkScreenTex(s);

    // 場館容量圈半徑
    this.rings = (s.venues || []).slice(0, 8);

    // 鏡頭: 頭一次入場擺喺後上方望落個台 (似啱啱入到場搵位)
    if (!this._camInit || this._lastR !== R) {
      this.cam.dist = R * 1.25;
      this.cam.tz = R * 0.22; this.cam.ty = R * slope * 0.12; this.cam.tx = 0;
      this.cam.yaw = 0; this.cam.pitch = 0.52;
      this._camInit = true; this._lastR = R;
    }
  }

  /** 台後面塊 LED 幕: 用 2D canvas 畫個名同人數, 上載做 texture。 */
  _mkScreenTex(s){
    const gl = this.gl;
    const c = this._texCv || (this._texCv = document.createElement("canvas"));
    c.width = 512; c.height = 256;
    const x = c.getContext("2d");
    const g = x.createLinearGradient(0,0,0,256);
    g.addColorStop(0, s.color); g.addColorStop(1, "#05060b");
    x.fillStyle = g; x.fillRect(0,0,512,256);
    x.fillStyle = "rgba(0,0,0,.42)"; x.fillRect(0,0,512,256);
    x.textAlign = "center";
    x.fillStyle = "#fff";
    x.font = "800 46px -apple-system,'PingFang HK','Microsoft JhengHei',sans-serif";
    const nm = (s.name || "").slice(0, 12);
    x.fillText(nm, 256, 108);
    x.font = "900 62px -apple-system,'PingFang HK',sans-serif";
    x.fillText((s.viewers|0).toLocaleString("en-US"), 256, 182);
    x.font = "700 22px -apple-system,'PingFang HK',sans-serif";
    x.fillStyle = "rgba(255,255,255,.72)";
    x.fillText("人 正在睇 · " + (s.platform || "").toUpperCase(), 256, 216);
    gl.bindTexture(gl.TEXTURE_2D, this.screenTex);
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, c);
  }

  // ── 輸入: 你可以喺場館度郁 ────────────────────────────────────────────
  _bindInput(){
    const cv = this.canvas, cam = this.cam;
    const touched = () => { this.lastInput = performance.now(); };
    let drag = null; const ptrs = new Map(); let pinch = 0;

    cv.style.touchAction = "none";
    cv.addEventListener("pointerdown", e => {
      ptrs.set(e.pointerId, [e.clientX, e.clientY]);
      if (ptrs.size === 1) drag = [e.clientX, e.clientY];
      cv.setPointerCapture(e.pointerId); touched();
    });
    cv.addEventListener("pointermove", e => {
      if (!ptrs.has(e.pointerId)) return;
      ptrs.set(e.pointerId, [e.clientX, e.clientY]);
      if (ptrs.size >= 2) {                       // 兩指: 縮放
        const [a, b] = [...ptrs.values()];
        const d = Math.hypot(a[0]-b[0], a[1]-b[1]);
        if (pinch) this._dolly(Math.pow(pinch/d, 1.6));
        pinch = d; drag = null; touched(); return;
      }
      if (!drag) return;
      cam.yaw   -= (e.clientX - drag[0]) * 0.0045;
      cam.pitch  = Math.max(-0.12, Math.min(1.35, cam.pitch + (e.clientY-drag[1])*0.0035));
      drag = [e.clientX, e.clientY]; touched();
    });
    const up = e => { ptrs.delete(e.pointerId); if (ptrs.size < 2) pinch = 0; if (!ptrs.size) drag = null; };
    cv.addEventListener("pointerup", up);
    cv.addEventListener("pointercancel", up);
    cv.addEventListener("wheel", e => {
      e.preventDefault(); this._dolly(Math.exp(e.deltaY * 0.0012)); touched();
    }, {passive:false});

    addEventListener("keydown", e => {
      const k = e.key.toLowerCase();
      if ("wasd".includes(k) || k.startsWith("arrow")) { this.keys.add(k); touched(); }
      if (k === "r") { this._camInit = false; if (this.scene) this.setScene(this.scene); }
    });
    addEventListener("keyup", e => this.keys.delete(e.key.toLowerCase()));
  }
  _dolly(f){
    const R = this.scene ? this.scene.R : 100;
    this.cam.dist = Math.max(R*0.035, Math.min(R*2.4, this.cam.dist * f));
  }

  /** 幀數唔夠就自動少畫幾成人, 夠快就加返 (有滯後, 唔會左右擺)。 */
  _autoQuality(t){
    const now = performance.now();
    if (this._last) {
      const dt = now - this._last;
      if (dt < 2500) this.frameMs += (dt - this.frameMs) * 0.25;  // EMA
    }
    this._last = now;
    if (t - this._adjT < 1.2 || this._adjN > 24) return;
    this._adjT = t;
    const before = this.renderN;
    if (this.frameMs > 34 && this.renderN > 1500) {
      this.renderN = Math.max(1200, Math.round(this.renderN * 0.5));
    } else if (this.frameMs < 13 && this.renderN < this.instN) {
      this.renderN = Math.min(this.instN, Math.round(this.renderN * 1.6));
    }
    if (this.renderN !== before) {
      this._adjN++;
      this.frameMs = 16;                       // 重新度過
      if (this.onQuality) this.onQuality(this.renderN, this.instN);
    }
  }

  resize(w, h, dpr){
    this.W = Math.max(1, Math.round(w*dpr)); this.H = Math.max(1, Math.round(h*dpr));
    this.canvas.width = this.W; this.canvas.height = this.H;
    this.gl.viewport(0, 0, this.W, this.H);
  }

  // ── 逐格畫 ────────────────────────────────────────────────────────────
  render(t){
    const gl = this.gl, s = this.scene;
    if (!s) return;
    this._autoQuality(t);
    const cam = this.cam, R = s.R, R0 = s.R0;

    // 行路 (WASD / 方向鍵) — 順住鏡頭方向行
    const spd = Math.max(0.35, cam.dist * 0.012);
    const fx = Math.sin(cam.yaw), fz = Math.cos(cam.yaw);
    let mx = 0, mz = 0;
    const k = this.keys;
    if (k.has("w") || k.has("arrowup"))    { mx -= fx; mz -= fz; }
    if (k.has("s") || k.has("arrowdown"))  { mx += fx; mz += fz; }
    if (k.has("a") || k.has("arrowleft"))  { mx -= fz; mz += fx; }
    if (k.has("d") || k.has("arrowright")) { mx += fz; mz -= fx; }
    if (mx || mz) {
      const l = Math.hypot(mx, mz);
      cam.tx = Math.max(-R, Math.min(R, cam.tx + mx/l*spd));
      cam.tz = Math.max(-R*0.4, Math.min(R*1.15, cam.tz + mz/l*spd));
    }
    // 望住嘅位就跟住個碗形升高, 唔會插入地下
    const tr = Math.hypot(cam.tx, cam.tz);
    cam.ty += (Math.max(0, tr-R0)*this.slope + R*0.02 - cam.ty) * 0.1;

    // 冇人郁就慢慢自己遊場 (OBS 掛住唔理都好睇)
    if (this.auto && performance.now() - this.lastInput > 7000) {
      cam.yaw = Math.sin(t*0.055) * 0.55;
      cam.pitch = 0.46 + Math.sin(t*0.031)*0.14;
      cam.dist = R * (0.80 + 0.55*(0.5+0.5*Math.sin(t*0.043)));
    }

    const cp = Math.cos(cam.pitch), sp = Math.sin(cam.pitch);
    const eye = [cam.tx + cam.dist*cp*Math.sin(cam.yaw),
                 cam.ty + cam.dist*sp + 1.2,
                 cam.tz + cam.dist*cp*Math.cos(cam.yaw)];
    // ⚠️ 個看台係碗形, 愈後排愈高。如果照計絕對高度, 鏡頭一退遠就會跌咗落
    //    看台下面 / 或者貼住地平望 —— 前排啲人就變成一幅牆遮晒後面。
    //    所以: 鏡頭高度 = 當地看台高度 + 「離地幾高」, 由 pitch × 距離決定。
    //    拉遠 = 由高空俯瞰成個場; 拉到最近 = 企喺人堆中間 (1.9 米, 即係你嘅視線)。
    const er = Math.hypot(eye[0], eye[2]);
    const rim = Math.max(0, R*1.1 - R0) * this.slope;
    const deck = Math.min(rim, Math.max(0, er - R0) * this.slope);
    eye[1] = deck + Math.max(1.9, cam.dist * sp + 1.2);
    const ctr = [cam.tx, cam.ty, cam.tz];
    const view = M4.lookAt(eye, ctr, [0,1,0]);
    const proj = M4.persp(1.02, this.W/this.H, 0.35, R*7);
    const VP = M4.mul(proj, view);
    const right = [view[0], 0, view[8]];
    const rl = Math.hypot(right[0], right[2]) || 1;
    right[0] /= rl; right[2] /= rl;
    const fog = R * 7.0;

    gl.viewport(0, 0, this.W, this.H);
    gl.clearColor(0,0,0,0);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    gl.enable(gl.DEPTH_TEST);
    gl.depthFunc(gl.LEQUAL);
    gl.disable(gl.CULL_FACE);

    const col = hex2rgb(s.color);
    const wave = ((t*0.30) % 1.7) * R;              // 人浪掃到邊
    const energy = 1.0;

    // ── 1. 碗形地台 + 場館容量圈 ──────────────────────────────────────
    gl.disable(gl.BLEND);
    gl.useProgram(this.pBowl);
    gl.uniformMatrix4fv(this.pBowl.u.uVP, false, VP);
    gl.uniform3fv(this.pBowl.u.uEye, eye);
    gl.uniform1f(this.pBowl.u.uFog, fog);
    gl.uniform3fv(this.pBowl.u.uColor, col);
    const rr = new Float32Array(8), rf = new Float32Array(8);
    let unitR = -1;
    this.rings.forEach((v, i) => { rr[i] = v.r; rf[i] = v.filled ? 1 : 0; if (v.unit) unitR = v.r; });
    gl.uniform1fv(this.pBowl.u.uRings, rr);
    gl.uniform1fv(this.pBowl.u.uFilled, rf);
    gl.uniform1f(this.pBowl.u.uUnit, unitR);
    gl.uniform1i(this.pBowl.u.uN, this.rings.length);
    gl.bindVertexArray(this.bowl.vao);
    gl.drawArrays(gl.TRIANGLES, 0, this.bowl.n);

    // ── 2. 舞台 + LED 幕 ──────────────────────────────────────────────
    const sw = R0*1.9, sd = R0*0.85, sh2 = Math.max(0.6, R0*0.10);
    gl.enable(gl.BLEND); gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
    gl.useProgram(this.pSolid);
    gl.uniformMatrix4fv(this.pSolid.u.uVP, false, VP);
    gl.uniform3fv(this.pSolid.u.uEye, eye);
    gl.uniform1f(this.pSolid.u.uFog, fog);
    gl.uniform1f(this.pSolid.u.uTex, 0);
    gl.uniform1f(this.pSolid.u.uGlow, 0);
    gl.uniform1f(this.pSolid.u.uAlpha, 1);
    gl.uniform3f(this.pSolid.u.uColor, 0.08, 0.09, 0.13);
    gl.uniformMatrix4fv(this.pSolid.u.uModel, false, M4.trs(0, 0, -R0*0.15, sw, sh2, sd));
    gl.bindVertexArray(this.box.vao);
    gl.drawArrays(gl.TRIANGLES, 0, this.box.n);

    // LED 幕 (企喺台後面)
    gl.uniform1f(this.pSolid.u.uTex, 1);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.screenTex);
    gl.uniform1i(this.pSolid.u.uSampler, 0);
    gl.uniform3f(this.pSolid.u.uColor, 1,1,1);
    gl.uniformMatrix4fv(this.pSolid.u.uModel, false,
      M4.trs(0, sh2, -R0*0.58, sw*0.92, sw*0.92*0.5, 1));
    gl.bindVertexArray(this.plane.vao);
    gl.drawArrays(gl.TRIANGLES, 0, this.plane.n);
    gl.uniform1f(this.pSolid.u.uTex, 0);

    // ── 3. 觀眾 (一個 draw call, 幾萬人) ──────────────────────────────
    const pc = this.pCrowd;
    gl.useProgram(pc);
    gl.uniformMatrix4fv(pc.u.uVP, false, VP);
    gl.uniform3fv(pc.u.uRight, right);
    gl.uniform3fv(pc.u.uEye, eye);
    gl.uniform1f(pc.u.uTime, t);
    gl.uniform1f(pc.u.uWave, wave);
    gl.uniform1f(pc.u.uEnergy, energy);
    gl.uniform1f(pc.u.uFog, fog);
    gl.uniform1f(pc.u.uSizeX, 0.52);
    gl.uniform1f(pc.u.uSizeY, 1.45);
    gl.uniform1f(pc.u.uMode, 0);
    gl.uniform3fv(pc.u.uColor, col);
    gl.enable(gl.BLEND); gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
    gl.depthMask(true);
    gl.bindVertexArray(this.vaoCrowd);
    gl.drawArraysInstanced(gl.TRIANGLES, 0, 6, this.renderN);

    // ── 4. 直播主: 喺台上行嚟行去 + 跳 ───────────────────────────────
    const px = Math.sin(t*0.42)*sw*0.30, pz = -R0*0.15 + Math.cos(t*0.27)*sd*0.16;
    const py = sh2 + Math.max(0, Math.sin(t*2.6))*R0*0.06;
    gl.bindBuffer(gl.ARRAY_BUFFER, this.star);
    gl.bufferSubData(gl.ARRAY_BUFFER, 0, new Float32Array([px, py, pz, 0.5]));
    gl.uniform1f(pc.u.uSizeX, Math.max(1.0, R0*0.30));
    gl.uniform1f(pc.u.uSizeY, Math.max(2.6, R0*0.80));
    gl.uniform1f(pc.u.uEnergy, 0.25);
    gl.uniform3f(pc.u.uColor, 1, 0.98, 0.92);
    gl.bindVertexArray(this.vaoStar);
    gl.drawArraysInstanced(gl.TRIANGLES, 0, 6, 1);

    // ── 5. 燈棒海 (加光混合) ─────────────────────────────────────────
    gl.enable(gl.BLEND); gl.blendFunc(gl.ONE, gl.ONE);   // premultiplied 加光
    gl.depthMask(false);
    gl.uniform1f(pc.u.uMode, 1);
    gl.uniform1f(pc.u.uSizeX, 0.52);
    gl.uniform1f(pc.u.uSizeY, 1.45);
    gl.uniform1f(pc.u.uEnergy, energy);
    gl.uniform3fv(pc.u.uColor, col);
    gl.bindVertexArray(this.vaoCrowd);
    gl.drawArraysInstanced(gl.TRIANGLES, 0, 6, this.renderN);
    gl.uniform1f(pc.u.uMode, 0);

    // ── 6. 舞台射燈 (由棚頂掃落人堆) ─────────────────────────────────
    gl.useProgram(this.pSolid);
    gl.uniformMatrix4fv(this.pSolid.u.uVP, false, VP);
    gl.uniform3fv(this.pSolid.u.uEye, eye);
    gl.uniform1f(this.pSolid.u.uFog, fog);
    gl.uniform1f(this.pSolid.u.uTex, 0);
    gl.uniform1f(this.pSolid.u.uGlow, 1);
    gl.uniform1f(this.pSolid.u.uAlpha, 0.13);
    gl.bindVertexArray(this.cone.vao);
    const topY = Math.max(6, R0*1.5);
    for (let i = 0; i < 4; i++) {
      const a = t*0.5 + i*1.57;
      const spread = R*0.16 * (1 + 0.3*Math.sin(t*0.8+i));
      gl.uniform3f(this.pSolid.u.uColor,
        i%2 ? col[0]*0.6+0.4 : 1, i%2 ? col[1]*0.6+0.4 : 0.95, i%2 ? col[2]*0.6+0.4 : 0.85);
      const m = M4.trs(Math.sin(a)*sw*0.5, topY, -R0*0.2 + Math.cos(a)*sd*0.4,
                       spread, topY*1.05, spread);
      gl.uniformMatrix4fv(this.pSolid.u.uModel, false, m);
      gl.drawArrays(gl.TRIANGLES, 0, this.cone.n);
    }
    // ── 7. 場館容量光牆: 一個紅館坐到邊, 直接喺人海入面圍一圈金光 ──────
    gl.uniform1f(this.pSolid.u.uGlow, 1);
    gl.bindVertexArray(this.ring.vao);
    const rh = Math.max(2.2, R*0.030);
    for (const v of this.rings) {
      if (v.r < R0*0.6) continue;
      if (v.unit)      gl.uniform3f(this.pSolid.u.uColor, 1.0, 0.83, 0.47);
      else if (v.filled) gl.uniform3f(this.pSolid.u.uColor, 0.85, 0.90, 1.0);
      else             gl.uniform3f(this.pSolid.u.uColor, 0.35, 0.40, 0.55);
      gl.uniform1f(this.pSolid.u.uAlpha, v.unit ? 0.85 : v.filled ? 0.30 : 0.16);
      gl.uniformMatrix4fv(this.pSolid.u.uModel, false,
        M4.trs(0, 0, 0, v.r, rh*(v.unit?1.6:1), v.r));
      gl.drawArrays(gl.TRIANGLES, 0, this.ring.n);
    }

    gl.depthMask(true);
    gl.disable(gl.BLEND);
    gl.bindVertexArray(null);
  }

  dispose(){ /* canvas 一齊拆走就得 */ }
}

window.Arena3D = {
  supported(){
    try { return !!document.createElement("canvas").getContext("webgl2"); }
    catch (e) { return false; }
  },
  create(canvas, opts){ return new Renderer(canvas, opts); },
};
})();
