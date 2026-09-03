/* ══════════════════════════════════════════════════════════════════════════
   一號影廳 · VELOUR (WebGL2, 零外部依賴)
   ──────────────────────────────────────────────────────────────────────────
   入場 → 揀位 → 坐低 → 燈暗 → 幕開 → 大銀幕亮起。
   60 張皮革躺椅嘅精品影院: 炭灰肋牆、酒紅皮革、黃銅細節、絲絨幕。

   技術重點:
   · 真正嘅 YouTube 播放器 (iframe) 用 CSS matrix3d 貼落 3D 銀幕上,
     同 WebGL 鏡頭用同一組矩陣 — 望正個幕嗰陣仲會「貼平」做 2D, 字唔會朦。
   · 座位 / 銅牌 / 觀眾剪影 / 星空 / 塵埃 全部 GPU instancing。
   · 8 盞燈嘅光照迴圈 + 絲絨 sheen + 影院式 tone-map, 全部一條 fragment 路。

   對外介面 (theatre.html 用):
     Theatre.supported()
     const T = Theatre.create(glCanvas, cssLayer, screenEl, {onPhase, onHover, onSeat, onCurtain})
     T.setTextures(TheatreTex)  T.setProgramme({posters, marquee})  T.setMarquee(text)
     T.enter({returning})  T.skip()  T.pickSeat(i)  T.bestSeat()  T.stand()  T.hop(dx,dz)
     T.house(target, tau)  T.curtain(open, seconds)  T.screen(on, seconds)
     T.render(t)  T.resize(w,h,dpr)  T.state()  T.seatCode(i)  T.projectedScreenRect()
   ══════════════════════════════════════════════════════════════════════════ */
(() => {
"use strict";

// ── 迷你 mat4 (column-major, 同 CSS matrix3d 一樣次序) ─────────────────────
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
      let s = 0; for (let k = 0; k < 4; k++) s += a[k*4+r] * b[c*4+k];
      o[c*4+r] = s;
    }
    return o;
  },
  transl(x,y,z){ return new Float32Array([1,0,0,0, 0,1,0,0, 0,0,1,0, x,y,z,1]); },
  scale(x,y,z){ return new Float32Array([x,0,0,0, 0,y,0,0, 0,0,z,0, 0,0,0,1]); },
  rotY(a){ const c=Math.cos(a), s=Math.sin(a); return new Float32Array([c,0,-s,0, 0,1,0,0, s,0,c,0, 0,0,0,1]); },
  rotX(a){ const c=Math.cos(a), s=Math.sin(a); return new Float32Array([1,0,0,0, 0,c,s,0, 0,-s,c,0, 0,0,0,1]); },
  chain(...ms){ return ms.reduce((a,b) => M4.mul(a,b)); },
};
const clamp = (v,a,b) => v < a ? a : v > b ? b : v;
const lerp = (a,b,t) => a + (b-a)*t;
const DEG = Math.PI/180;
// 規格指定嘅曲線: 鏡頭 cubic-bezier(0.65,0,0.35,1) ≈ in-out; UI cubic-bezier(0.22,1,0.36,1) ≈ out-expo-ish
const easeInOut = t => t < 0.5 ? 4*t*t*t : 1 - Math.pow(-2*t+2, 3)/2;
const easeInOutQuart = t => t < 0.5 ? 8*t*t*t*t : 1 - Math.pow(-2*t+2, 4)/2;
const easeOut = t => 1 - Math.pow(1-t, 3);
const hex = h => { const v = parseInt(String(h).replace("#",""), 16); return [(v>>16&255)/255, (v>>8&255)/255, (v&255)/255]; };
const mulberry = seed => () => { seed |= 0; seed = seed + 0x6D2B79F5 | 0; let t = Math.imul(seed ^ seed >>> 15, 1 | seed);
  t = t + Math.imul(t ^ t >>> 7, 61 | t) ^ t; return ((t ^ t >>> 14) >>> 0) / 4294967296; };

// ── GL 小工具 ─────────────────────────────────────────────────────────────
function sh(gl, type, src){
  const s = gl.createShader(type); gl.shaderSource(s, src.trim()); gl.compileShader(s);
  if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error("shader: " + gl.getShaderInfoLog(s));
  return s;
}
function prog(gl, vs, fs){
  const p = gl.createProgram();
  gl.attachShader(p, sh(gl, gl.VERTEX_SHADER, vs)); gl.attachShader(p, sh(gl, gl.FRAGMENT_SHADER, fs));
  gl.linkProgram(p);
  if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error("link: " + gl.getProgramInfoLog(p));
  p.u = {};
  const n = gl.getProgramParameter(p, gl.ACTIVE_UNIFORMS);
  for (let i = 0; i < n; i++) { const nm = gl.getActiveUniform(p, i).name.replace(/\[0\]$/, ""); p.u[nm] = gl.getUniformLocation(p, nm); }
  return p;
}
const H = `#version 300 es
precision highp float;`;

// ── 共用光照: 環境 + 頂光 + 8 盞點光 + 絲絨 sheen + 霧 + tone-map ─────────
const LIGHT_FN = `
uniform float uHouse;           // 場燈 0..1
uniform vec3  uWarm, uEye, uFogC;
uniform float uFogD;
uniform int   uNL;
uniform vec3  uLP[8]; uniform vec3 uLC[8]; uniform float uLR[8];
vec3 tonemap(vec3 c){
  c = (c*(2.51*c+0.03))/(c*(2.43*c+0.59)+0.14);
  return c*vec3(1.03,1.00,0.96) + vec3(0.012,0.010,0.008);
}
vec3 fogged(vec3 c, vec3 wp){
  float f = clamp(length(wp-uEye)/uFogD, 0.0, 1.0);
  f = 1.0 - exp(-f*f*1.2);
  return mix(c, uFogC, f);
}
vec3 lit(vec3 alb, vec3 n, vec3 wp, vec3 sheen, float specPow){
  n = normalize(n); vec3 v = normalize(uEye - wp);
  vec3 amb = vec3(0.16,0.155,0.17) * mix(0.28, 1.0, uHouse);
  float top = max(0.0, dot(n, normalize(vec3(0.15,1.0,0.25))));
  vec3 c = alb * (amb + uWarm*0.45*top*uHouse);
  for (int i = 0; i < 8; i++) {
    if (i >= uNL) break;
    vec3 L = uLP[i] - wp; float d = length(L); L /= max(d, 0.001);
    float att = clamp(1.0 - d/uLR[i], 0.0, 1.0); att *= att;
    if (att <= 0.0) continue;
    float nl = max(0.0, dot(n, L));
    float sp = specPow > 0.5 ? pow(max(0.0, dot(n, normalize(L+v))), specPow) * 0.22 : 0.0;
    c += uLC[i] * att * (alb*nl + sp);
  }
  c += sheen * pow(1.0 - abs(dot(n, v)), 3.0) * 0.35;
  return tonemap(fogged(c, wp));
}`;

// 一般幾何 (箱 / 面): 牆、地台、幕、門、海報、跑馬燈
const VS_SOLID = `${H}
layout(location=0) in vec3 aPos; layout(location=1) in vec2 aUv; layout(location=2) in vec3 aNrm;
uniform mat4 uVP, uModel; uniform vec2 uUvScale;
out vec2 vUv; out vec3 vN, vW;
void main(){ vec4 wp = uModel*vec4(aPos,1.0); gl_Position = uVP*wp; vUv = aUv*uUvScale; vW = wp.xyz; vN = mat3(uModel)*aNrm; }`;
const FS_SOLID = `${H}
in vec2 vUv; in vec3 vN, vW;
uniform vec3 uColor, uSheen; uniform float uSpec, uEmit, uTex, uAlpha, uMode;
uniform sampler2D uSampler;
${LIGHT_FN}
out vec4 o;
void main(){
  vec3 alb = uColor; float a = uAlpha;
  if (uTex > 0.5) { vec4 t = texture(uSampler, vUv); alb = t.rgb * (uMode > 1.5 ? uColor : vec3(1.0)); a *= t.a; }
  if (a < 0.02) discard;
  vec3 c;
  if (uMode > 0.5 && uMode < 1.5) c = tonemap(alb * uEmit);          // 純發光 (燈、星、銅線)
  else c = lit(alb, vN, vW, uSheen, uSpec) + (uEmit > 0.0 ? tonemap(alb*uEmit) : vec3(0.0));
  o = vec4(c*a, a);
}`;

// Instanced 座位部件 (60 張 × 每部件一個 draw)
const VS_INST = `${H}
layout(location=0) in vec3 aPos; layout(location=1) in vec2 aUv; layout(location=2) in vec3 aNrm;
layout(location=3) in vec4 aT;      // x,y,z, yaw
layout(location=4) in vec4 aS;      // state (0 free,1 hover,2 mine,3 reco), unused, unused, unused
uniform mat4 uVP; uniform vec3 uOff, uScale; uniform float uTilt;
out vec3 vN, vW; out float vSt;
void main(){
  float c = cos(aT.w), s = sin(aT.w), ct = cos(uTilt), st = sin(uTilt);
  vec3 p = aPos * uScale;
  p = vec3(p.x, ct*p.y - st*p.z, st*p.y + ct*p.z) + uOff;         // 靠背向後傾
  vec3 n = vec3(aNrm.x, ct*aNrm.y - st*aNrm.z, st*aNrm.y + ct*aNrm.z);
  vec3 r = vec3(c*p.x + s*p.z, p.y, -s*p.x + c*p.z);
  vec3 wp = aT.xyz + r;
  gl_Position = uVP * vec4(wp, 1.0);
  vN = vec3(c*n.x + s*n.z, n.y, -s*n.x + c*n.z); vW = wp; vSt = aS.x;
}`;
const FS_INST = `${H}
in vec3 vN, vW; in float vSt;
uniform vec3 uColor, uSheen; uniform float uSpec, uHoverBoost;
${LIGHT_FN}
out vec4 o;
void main(){
  vec3 alb = uColor * (vSt > 0.5 && vSt < 1.5 ? 1.0 + uHoverBoost : 1.0);
  o = vec4(lit(alb, vN, vW, uSheen, uSpec), 1.0);
}`;

// 座位銅牌 (instanced quad, atlas 揀格)
const VS_PLATE = `${H}
layout(location=0) in vec2 aQuad;
layout(location=1) in vec4 aT;      // x,y,z,yaw (牌中心, 面向 +z 即係向後排)
layout(location=2) in vec4 aUV;     // u0,v0,u1,v1
layout(location=3) in float aGlow;
uniform mat4 uVP; uniform vec2 uSize;
out vec2 vUv; out float vGlow; out vec3 vW;
void main(){
  float c = cos(aT.w), s = sin(aT.w);
  vec3 p = vec3(aQuad.x*uSize.x, (aQuad.y-0.5)*uSize.y, 0.0);
  vec3 r = vec3(c*p.x + s*p.z, p.y, -s*p.x + c*p.z);
  vec3 wp = aT.xyz + r;
  gl_Position = uVP * vec4(wp, 1.0);
  vUv = vec2(mix(aUV.x, aUV.z, aQuad.x+0.5), mix(aUV.w, aUV.y, aQuad.y)); vGlow = aGlow; vW = wp;
}`;
const FS_PLATE = `${H}
in vec2 vUv; in float vGlow; in vec3 vW;
uniform sampler2D uSampler; uniform float uHasTex;
${LIGHT_FN}
out vec4 o;
void main(){
  vec3 brass = vec3(0.69,0.55,0.34);
  vec3 alb = uHasTex > 0.5 ? texture(uSampler, vUv).rgb : brass;
  vec3 c = lit(alb, vec3(0.0,0.0,1.0), vW, vec3(0.5,0.47,0.4), 64.0) + tonemap(alb*vGlow);
  o = vec4(c, 1.0);
}`;

// 觀眾剪影 (坐低, 由後面睇, 黑色、被銀幕光鑲邊)
const VS_SIL = `${H}
layout(location=0) in vec2 aQuad;
layout(location=1) in vec4 aP;      // x,y,z, seed
uniform mat4 uVP; uniform vec3 uRight; uniform float uTime, uPre;
out vec2 vUv; out float vS; out vec3 vW;
void main(){
  float ph = aP.w*6.2831, amp = 1.0 + uPre;                      // 開場前郁多啲
  float sway = 0.01*sin(uTime*0.35 + ph)*amp;
  float lean = 0.04*smoothstep(0.86, 0.9, fract(uTime*0.05 + aP.w))*amp;
  float bob  = 0.02*abs(sin(uTime*25.0))*step(0.94, fract(aP.w*3.1))*step(0.9, fract(uTime*0.05 + aP.w*0.7));
  vec3 wp = aP.xyz + uRight*(aQuad.x*0.60 + sway + lean) + vec3(0.0, aQuad.y*0.75 + bob, 0.0);
  gl_Position = uVP * vec4(wp, 1.0);
  vUv = aQuad + vec2(0.5, 0.0); vS = aP.w; vW = wp;
}`;
const FS_SIL = `${H}
in vec2 vUv; in float vS; in vec3 vW;
uniform vec3 uScreenLum; uniform float uTime;
${LIGHT_FN}
out vec4 o;
float rr(vec2 p, vec2 b, float r){ vec2 q = abs(p)-b+r; return length(max(q,0.0)) + min(max(q.x,q.y),0.0) - r; }
void main(){
  vec2 p = (vUv - vec2(0.5, 0.0)) * vec2(0.60, 0.75);             // 米
  float hair = floor(fract(vS*13.7)*4.0);
  float d = rr(p - vec2(0.0, 0.20), vec2(0.29, 0.20), 0.14);      // 膊頭
  d = min(d, length(p - vec2(0.0, 0.60)) - 0.115);                 // 頭
  d = min(d, rr(p - vec2(0.0, 0.46), vec2(0.05, 0.04), 0.01));     // 頸
  if (hair > 0.5 && hair < 1.5) d = min(d, rr(p - vec2(0.0, 0.48), vec2(0.14, 0.14), 0.08));   // 長髮
  if (hair > 1.5 && hair < 2.5) d = min(d, length(p - vec2(0.0, 0.72)) - 0.05);                // 髻
  if (hair > 2.5) d = min(d, rr(p - vec2(0.0, 0.66), vec2(0.15, 0.03), 0.01));                 // 帽
  float a = 1.0 - smoothstep(0.0, 0.012, d);
  if (a < 0.02) discard;
  float rim = pow(clamp(1.0 + d/0.012, 0.0, 1.0), 3.0);           // 邊緣被銀幕光鑲邊 (幼)
  vec3 c = vec3(0.039,0.039,0.047)*0.5 + 0.035*uWarm*uHouse + rim*uScreenLum*0.28;
  o = vec4(tonemap(fogged(c, vW))*a, a);
}`;

// 加光 sprites: 壁燈光暈 / 星 / 塵 (instanced quad, billboard)
const VS_SPR = `${H}
layout(location=0) in vec2 aQuad;
layout(location=1) in vec4 aP;      // x,y,z, size
layout(location=2) in vec4 aC;      // r,g,b, seed
uniform mat4 uVP; uniform vec3 uRight, uUp; uniform float uTime, uGain, uMode;
out vec2 vUv; out vec3 vC; out float vA;
void main(){
  vec3 base = aP.xyz; float sz = aP.w; float a = uGain;
  if (uMode > 0.5) {                          // 塵: 喺光束入面漂 (aP = t,u,v, seed; 位置由 uniform 光束計)
    a *= 0.35 + 0.65*fract(aC.w*9.1);
  }
  if (uMode > 1.5) {                          // 星: 部分閃
    float tw = step(0.87, fract(aC.w*7.7));
    a *= mix(1.0, 0.55 + 0.45*sin(uTime*(0.2+0.3*fract(aC.w*3.3))*6.28 + aC.w*20.0), tw);
  }
  vec3 wp = base + uRight*(aQuad.x*sz) + uUp*((aQuad.y-0.5)*sz);
  gl_Position = uVP * vec4(wp, 1.0);
  vUv = aQuad + vec2(0.5, 0.0); vC = aC.rgb; vA = a;
}`;
const FS_SPR = `${H}
in vec2 vUv; in vec3 vC; in float vA; out vec4 o;
void main(){ float d = length(vUv - 0.5)*2.0; float a = (1.0 - smoothstep(0.0, 1.0, d)); a *= a; o = vec4(vC*a*vA, a*vA); }`;
// 塵埃專用 (位置由光束參數計)
const VS_DUST = `${H}
layout(location=0) in vec2 aQuad;
layout(location=1) in vec4 aP;      // t, u, v, seed
uniform mat4 uVP; uniform vec3 uRight, uUp, uApex, uAxis, uHalfW, uHalfH; uniform float uTime, uGain;
out float vA;
void main(){
  float t = fract(aP.x + uTime*0.006*(0.5+aP.w));
  float u = aP.y + 0.05*sin(uTime*0.33 + aP.w*20.0), v = aP.z + 0.04*cos(uTime*0.27 + aP.w*13.0);
  vec3 p = uApex + uAxis*t + (uHalfW*u + uHalfH*v)*t;
  float s = 0.025;
  vec3 wp = p + uRight*(aQuad.x*s) + uUp*((aQuad.y-0.5)*s);
  gl_Position = uVP*vec4(wp,1.0);
  vA = uGain*(0.4+0.6*fract(aP.w*9.1))*smoothstep(0.0,0.08,t)*(1.0-0.5*t)*(0.7+0.3*sin(uTime*3.0+aP.w*40.0));
}`;
const FS_DUST = `${H}
in float vA; out vec4 o; uniform vec3 uColor;
void main(){ o = vec4(uColor*vA, vA); }`;
// 光束本體 (frustum 側面, 加光, 帶啲 noise)
const FS_BEAM = `${H}
in vec2 vUv; in vec3 vN, vW; uniform float uGain, uTime; uniform vec3 uColor; out vec4 o;
float n2(vec2 p){ vec2 i=floor(p), f=fract(p); f=f*f*(3.0-2.0*f);
  float a=fract(sin(dot(i,vec2(127.1,311.7)))*43758.5); float b=fract(sin(dot(i+vec2(1,0),vec2(127.1,311.7)))*43758.5);
  float c=fract(sin(dot(i+vec2(0,1),vec2(127.1,311.7)))*43758.5); float d=fract(sin(dot(i+vec2(1,1),vec2(127.1,311.7)))*43758.5);
  return mix(mix(a,b,f.x),mix(c,d,f.x),f.y); }
void main(){
  float haze = 0.6 + 0.4*(0.6*n2(vUv*vec2(6.0,40.0)+vec2(0.0,uTime*0.25)) + 0.4*n2(vUv*vec2(12.0,80.0)-vec2(0.0,uTime*0.4)));
  float a = uGain * haze * (1.0 - vUv.y*0.6);
  o = vec4(uColor*a, a);
}`;

// ══ Theatre ═══════════════════════════════════════════════════════════════
class Theatre {
  constructor(cv, cssLayer, screenEl, opts = {}){
    const gl = this.gl = cv.getContext("webgl2", {alpha:false, antialias:true, premultipliedAlpha:true, powerPreference:"high-performance"});
    if (!gl) throw new Error("冇 WebGL2");
    this.cv = cv; this.css = cssLayer; this.screenEl = screenEl; this.opts = opts;
    this.pSolid = prog(gl, VS_SOLID, FS_SOLID);
    this.pInst  = prog(gl, VS_INST, FS_INST);
    this.pPlate = prog(gl, VS_PLATE, FS_PLATE);
    this.pSil   = prog(gl, VS_SIL, FS_SIL);
    this.pSpr   = prog(gl, VS_SPR, FS_SPR);
    this.pDust  = prog(gl, VS_DUST, FS_DUST);
    this.pBeam  = prog(gl, VS_SOLID, FS_BEAM);
    this.box = this._box(); this.quad = this._quad(); this.beamMesh = null;
    this.W = 1; this.H = 1; this.dpr = 1;
    this.tex = {}; this.TT = null;

    // ── 調色 (規格) ───────────────────────────────────────────────────────
    this.P = { bg:"#050506", wall:"#1E1E25", floor:"#0E0F13", ceiling:"#0A0B0E", seatBase:"#2A2A2E",
      leather:"#4A1C1F", leatherHi:"#6A2A2E", headrest:"#56232A", walnut:"#3A2A1E", brass:"#B08D57",
      warm:"#FFB86B", velvet:"#5B1A22", velvetSheen:"#8A2A34", leatherSheen:"#6A2A2E", stage:"#101013",
      masking:"#050506", coffer:"#0E0F13", vestWall:"#1A1714", door:"#1A1512", strip:"#E8C27A", star:"#D9D2C0" };
    // ── 場館尺寸 (米, 規格) ───────────────────────────────────────────────
    this.G = { hallW:14, hallD:17.5, hallH:8.2, screenW:10, screenH:5.625, screenY:1.4,
      rows:[8,8,10,10,12,12], rowZ:r => 6.5 + 1.6*r, treadY:r => 0.30*r, seatPitch:0.90,
      crossY:1.5, vestZ0:17.5, vestZ1:21.5, vestW:4.0, vestCeil:4.7 };
    this.screenC = [0, this.G.screenY + this.G.screenH/2, 0];
    // 燈光狀態
    this.houseV = 1.0; this.houseT = 1.0; this.houseTau = 0.7;
    this.curtainV = 0; this.curtainTw = null;         // 0 閂 → 1 開
    this.screenV = 0; this.screenTw = null;           // 銀幕光/光束 0..1
    this.footV = 1.0; this.footT = 1.0;
    this.flicker = 1.0; this.flickT = 0;
    this.screenTint = [0.788,0.839,0.941];            // #C9D6F0
    this.doorA = 0;                                   // 門開角 0..1
    this.phase = "load"; this.anim = null; this.timers = [];
    this.cam = { x:0, y:3.15, z:21.0, yaw:0, pitch:0, fov:62*DEG, roll:0 };
    this.look = { yaw:0, pitch:0 }; this.lookIdle = 0; this.snap = false;
    this.hover = -1; this.seat = -1; this.kbSel = -1; this.reservedSeat = -1;
    this.keys = new Set(); this.mouse = {x:-1, y:-1}; this.drag = null; this.isTouch = false;
    this.lo = false; this.probe = null;
    this.marqueeText = ""; this.marqueeTex = null; this.marqueeOld = null; this.marqueeMix = 1;
    this.posters = [];
    this.layout();
    this._bindInput();
  }

  // ── 幾何 ──────────────────────────────────────────────────────────────
  _mesh(v, stride, attrs){
    const gl = this.gl, vao = gl.createVertexArray(), b = gl.createBuffer();
    gl.bindVertexArray(vao); gl.bindBuffer(gl.ARRAY_BUFFER, b); gl.bufferData(gl.ARRAY_BUFFER, v, gl.STATIC_DRAW);
    for (const [loc,size,off] of attrs) { gl.enableVertexAttribArray(loc); gl.vertexAttribPointer(loc,size,gl.FLOAT,false,stride*4,off*4); }
    gl.bindVertexArray(null); return {vao, n: v.length/stride};
  }
  _boxVerts(){
    const q = [], F = [
      [[-.5,-.5,.5],[.5,-.5,.5],[.5,.5,.5],[-.5,.5,.5],[0,0,1]], [[.5,-.5,-.5],[-.5,-.5,-.5],[-.5,.5,-.5],[.5,.5,-.5],[0,0,-1]],
      [[-.5,.5,.5],[.5,.5,.5],[.5,.5,-.5],[-.5,.5,-.5],[0,1,0]],  [[-.5,-.5,-.5],[-.5,-.5,.5],[-.5,.5,.5],[-.5,.5,-.5],[-1,0,0]],
      [[.5,-.5,.5],[.5,-.5,-.5],[.5,.5,-.5],[.5,.5,.5],[1,0,0]],  [[-.5,-.5,-.5],[.5,-.5,-.5],[.5,-.5,.5],[-.5,-.5,.5],[0,-1,0]] ];
    const uv = [[0,0],[1,0],[1,1],[0,1]];
    for (const f of F) for (const i of [0,1,2, 0,2,3]) q.push(...f[i], ...uv[i], ...f[4]);
    return new Float32Array(q);
  }
  _box(){ return this._mesh(this._boxVerts(), 8, [[0,3,0],[1,2,3],[2,3,5]]); }
  _quad(){ const q=[], p=[[-.5,-.5,0],[.5,-.5,0],[.5,.5,0],[-.5,.5,0]], uv=[[0,1],[1,1],[1,0],[0,0]];
    for (const i of [0,1,2,0,2,3]) q.push(...p[i], ...uv[i], 0,0,1); return this._mesh(new Float32Array(q), 8, [[0,3,0],[1,2,3],[2,3,5]]); }
  _instBox(n){
    const gl = this.gl, vao = gl.createVertexArray(); gl.bindVertexArray(vao);
    const vb = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, vb); gl.bufferData(gl.ARRAY_BUFFER, this._boxVerts(), gl.STATIC_DRAW);
    for (const [loc,size,off] of [[0,3,0],[1,2,3],[2,3,5]]) { gl.enableVertexAttribArray(loc); gl.vertexAttribPointer(loc,size,gl.FLOAT,false,32,off*4); }
    const ib = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, ib); gl.bufferData(gl.ARRAY_BUFFER, n*32, gl.DYNAMIC_DRAW);
    gl.enableVertexAttribArray(3); gl.vertexAttribPointer(3,4,gl.FLOAT,false,32,0);  gl.vertexAttribDivisor(3,1);
    gl.enableVertexAttribArray(4); gl.vertexAttribPointer(4,4,gl.FLOAT,false,32,16); gl.vertexAttribDivisor(4,1);
    gl.bindVertexArray(null); return {vao, ib, count:n};
  }
  /** instanced billboard quad; layout = [[loc,size,offsetFloats],...], stride in floats */
  _instQuad(n, layout, stride){
    const gl = this.gl, vao = gl.createVertexArray(); gl.bindVertexArray(vao);
    const vb = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, vb);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-.5,0, .5,0, .5,1, -.5,0, .5,1, -.5,1]), gl.STATIC_DRAW);
    gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0,2,gl.FLOAT,false,0,0);
    const ib = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, ib); gl.bufferData(gl.ARRAY_BUFFER, Math.max(1,n)*stride*4, gl.DYNAMIC_DRAW);
    for (const [loc,size,off] of layout) { gl.enableVertexAttribArray(loc); gl.vertexAttribPointer(loc,size,gl.FLOAT,false,stride*4,off*4); gl.vertexAttribDivisor(loc,1); }
    gl.bindVertexArray(null); return {vao, ib, count:n, stride};
  }
  _upload(inst, arr){ const gl = this.gl; gl.bindBuffer(gl.ARRAY_BUFFER, inst.ib); gl.bufferData(gl.ARRAY_BUFFER, arr, gl.DYNAMIC_DRAW); inst.count = arr.length/inst.stride; }
  _texture(canvas, {repeat=false} = {}){
    const gl = this.gl, t = gl.createTexture(); gl.bindTexture(gl.TEXTURE_2D, t);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, canvas);
    gl.generateMipmap(gl.TEXTURE_2D);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR_MIPMAP_LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, repeat ? gl.REPEAT : gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, repeat ? gl.REPEAT : gl.CLAMP_TO_EDGE);
    return t;
  }
  _beam(){   // 由投影窗到銀幕四角嘅 frustum 側面 (4 面), uv.y 0 頂 → 1 幕
    const G = this.G, a = [0, 6.9, 17.4];
    const c = [[-5, G.screenY, 0],[5, G.screenY, 0],[5, G.screenY+G.screenH, 0],[-5, G.screenY+G.screenH, 0]];
    const q = [];
    for (let i = 0; i < 4; i++) { const p = c[i], n = c[(i+1)%4];
      q.push(...a,0,0, 0,1,0, ...p,0,1, 0,1,0, ...n,1,1, 0,1,0); }
    return this._mesh(new Float32Array(q), 8, [[0,3,0],[1,2,3],[2,3,5]]);
  }

  /** 排位 + 所有 instanced 物件。 */
  layout(){
    const G = this.G, gl = this.gl;
    const seats = [], rowNames = "ABCDEF";
    G.rows.forEach((n, r) => {
      for (let i = 0; i < n; i++) {
        const x = (i - (n-1)/2) * G.seatPitch, z = G.rowZ(r), y = G.treadY(r);
        seats.push({ i: seats.length, r, c: i, code: rowNames[r] + (i+1), x, y, z,
                     yaw: Math.atan2(x, z), occupied: false, reco: false });
      }
    });
    this.seats = seats;
    // 推薦位 C5 C6 D5 D6 D7; 最佳 D6
    for (const s of seats) if ((s.code === "C5"||s.code==="C6"||s.code==="D5"||s.code==="D6"||s.code==="D7")) s.reco = true;
    this.bestIdx = seats.findIndex(s => s.code === "D6"); this.reservedSeat = this.bestIdx;

    this.seatInst = this.seatInst || this._instBox(seats.length);
    this.seatData = new Float32Array(seats.length*8);
    this.plateInst = this.plateInst || this._instQuad(seats.length, [[1,4,0],[2,4,4],[3,1,8]], 9);
    this.plateData = new Float32Array(seats.length*9);
    this._writeSeats();

    // 觀眾 (預設 26 個; setAudience 會按片嘅觀看數再計)
    this.silInst = this.silInst || this._instQuad(seats.length, [[1,4,0]], 4);
    this.setAudience(26, 1);

    // 星 (90), 壁燈光暈 sprites, 塵埃
    const rnd = mulberry(7);
    const stars = [];
    for (let i = 0; i < 90; i++) stars.push(-6.5+13*rnd(), 8.19, 2+15*rnd(), 0.05+0.03*rnd(), 0.85,0.82,0.75, rnd());
    this.starInst = this.starInst || this._instQuad(90, [[1,4,0],[2,4,4]], 8);
    this._upload(this.starInst, new Float32Array(stars));
    this.lights = [];                                   // 壁燈 (hall 6/side + vestibule 2/side)
    for (let k = 0; k < 6; k++) for (const s of [-1,1]) { const z = 2.5 + 2.5*k; this.lights.push({x:s*(G.hallW/2-0.08), y:2.4+0.1*z, z, vest:false, side:s}); }
    for (const z of [19.0, 20.2]) for (const s of [-1,1]) this.lights.push({x:s*(G.vestW/2-0.08), y:3.9, z, vest:true, side:s});
    this.foots = [-4.5,-3,-1.5,0,1.5,3,4.5].map(x => ({x, y:0.47, z:1.6}));
    const halo = [];
    for (const l of this.lights) halo.push(l.x - l.side*0.12, l.y, l.z, 0.6, 1.0,0.72,0.42, 0);
    for (const f of this.foots) halo.push(f.x, f.y+0.05, f.z+0.05, 0.35, 1.0,0.72,0.42, 0);
    this.haloInst = this.haloInst || this._instQuad(halo.length/8, [[1,4,0],[2,4,4]], 8);
    this._upload(this.haloInst, new Float32Array(halo));
    const DN = 160, dd = new Float32Array(DN*4);
    for (let i = 0; i < DN; i++) { dd[i*4]=rnd(); dd[i*4+1]=rnd()-0.5; dd[i*4+2]=rnd()-0.5; dd[i*4+3]=rnd(); }
    this.dustInst = this.dustInst || this._instQuad(DN, [[1,4,0]], 4); this._upload(this.dustInst, dd);
    this.beamMesh = this.beamMesh || this._beam();
  }
  _writeSeats(){
    const d = this.seatData, p = this.plateData, uv = this.tex.plateUV;
    this.seats.forEach((s, k) => {
      const st = k === this.hover ? 1 : k === this.seat ? 2 : s.reco ? 3 : 0;
      d[k*8]=s.x; d[k*8+1]=s.y; d[k*8+2]=s.z; d[k*8+3]=s.yaw; d[k*8+4]=st; d[k*8+5]=0; d[k*8+6]=0; d[k*8+7]=0;
      // 銅牌: 靠背頂後面, 面向後排 (即係 +z 方向, 座位 local 嘅 +z)
      const c = Math.cos(s.yaw), sn = Math.sin(s.yaw), lz = 0.36, ly = 1.08;
      p[k*9]=s.x + sn*lz; p[k*9+1]=s.y+ly; p[k*9+2]=s.z + c*lz; p[k*9+3]=s.yaw;
      const u = uv ? uv(s.code) : [0,0,1,1]; p[k*9+4]=u[0]; p[k*9+5]=u[1]; p[k*9+6]=u[2]; p[k*9+7]=u[3];
      p[k*9+8] = 0;   // glow 逐格更新
    });
    const gl = this.gl;
    gl.bindBuffer(gl.ARRAY_BUFFER, this.seatInst.ib); gl.bufferSubData(gl.ARRAY_BUFFER, 0, d);
    this._upload(this.plateInst, p);
  }
  /** 觀眾人數 ↔ 觀看數: 18 + 2·log10(views), 保護中間推薦區。 */
  setAudience(count, seed){
    const rnd = mulberry(seed|0), n = clamp(count|0, 0, 40), pool = this.seats.filter(s => !s.reco && !(s.r >= 2 && s.r <= 3 && Math.abs(s.x) <= 1.4));
    for (const s of this.seats) s.occupied = false;
    // 後排 B–E 同中間優先
    const weighted = pool.map(s => ({s, w: (s.r>=1&&s.r<=4 ? 1.0 : 0.6) * (Math.abs(s.x) <= 3.15 ? 1.0 : 0.7) * (0.5+rnd())}))
      .sort((a,b) => b.w - a.w).slice(0, n);
    for (const {s} of weighted) s.occupied = true;
    if (this.seat >= 0) this.seats[this.seat].occupied = false;
    const sd = [];
    for (const s of this.seats) if (s.occupied) sd.push(s.x, s.y + 0.56, s.z + 0.05, rnd());
    this._upload(this.silInst, new Float32Array(sd));
  }
  setTextures(TT){
    if (!TT) return; this.TT = TT;
    const t = this.tex;
    try {
      t.ribs = this._texture(TT.ribs(), {repeat:true}); t.carpet = this._texture(TT.carpet(), {repeat:true});
      t.stone = this._texture(TT.stone(), {repeat:true}); t.curtain = this._texture(TT.curtain()); t.pelmet = this._texture(TT.pelmet());
      const atlas = TT.plateAtlas(this.seats.map(s => s.code)); t.plate = this._texture(atlas.canvas); t.plateUV = atlas.uv;
      t.reserved = this._texture(TT.reservedCard());
      this._writeSeats();
    } catch (e) { console.warn("theatre-tex 部分失敗, 用純色頂住:", e); }
  }
  setProgramme({posters = [], marquee} = {}){
    const TT = this.TT; if (!TT) return;
    this.posters = posters.slice(0, 6).map((v, i) => {
      const side = i % 2 ? 1 : -1, z = 18.4 + 1.2*Math.floor(i/2);
      const o = { v, x: side*(this.G.vestW/2 - 0.02), y: 3.1, z, side, tex: this._texture(TT.poster(v, null)) };
      if (v.thumb && TT.loadThumb) TT.loadThumb(v.thumb).then(img => { if (img) { this.gl.deleteTexture(o.tex); o.tex = this._texture(TT.poster(v, img)); } });
      return o;
    });
    if (marquee) this.setMarquee(marquee);
  }
  setMarquee(text){
    if (!this.TT || text === this.marqueeText) return;
    this.marqueeText = text;
    if (this.marqueeTex) { if (this.marqueeOld) this.gl.deleteTexture(this.marqueeOld); this.marqueeOld = this.marqueeTex; this.marqueeMix = 0; }
    this.marqueeTex = this._texture(this.TT.marquee(text));
  }

  // ── 流程 ──────────────────────────────────────────────────────────────
  _emit(ph, a){ if (this.opts.onPhase) this.opts.onPhase(ph, a); }
  _after(sec, fn){ this.timers.push({at: this.now + sec, fn}); }
  /** 入場: 門開 → 沿走廊行入 → 抬頭一掃 → 定喺後方觀眾席後面。 */
  enter({returning = false} = {}){
    if (this.phase !== "load") return;
    this.phase = "walk"; this._emit("walk");
    const t0 = this.now;
    this.walk = { t0, returning };
    this.cam.x = 0; this.cam.y = 3.15; this.cam.z = returning ? 18.6 : 21.0; this.cam.yaw = 0; this.cam.pitch = 0;
  }
  skip(){
    if (this.phase !== "walk") return;
    this.walk = null; this.doorA = 1; this.phase = "choose";
    Object.assign(this.cam, {x:0, y:3.15, z:16.6, yaw:0, pitch:-8*DEG, roll:0, fov:62*DEG});
    this._emit("choose");
  }
  _walkStep(t){
    const w = this.walk; if (!w) return;
    const e = t - w.t0, ret = w.returning;
    this.doorA = clamp((e - 0.3)/1.4, 0, 1);
    const dEnd = ret ? 2.0 : 4.0;
    if (e >= 0.6 && e <= dEnd) {
      const k = clamp((e - 0.6)/(dEnd - 0.6), 0, 1), kk = k < 0.15 ? easeOut(k/0.15)*0.15 : k > 0.75 ? 0.75 + (1-Math.pow(1-(k-0.75)/0.25, 2))*0.25 : k;
      const z0 = ret ? 18.6 : 21.0;
      this.cam.z = lerp(z0, 16.6, kk);
      const env = Math.sin(Math.PI*k);
      this.cam.y = 3.15 + 0.035*Math.sin(2*Math.PI*1.9*e)*env;
      this.cam.x = 0.015*Math.sin(2*Math.PI*0.95*e)*env;
      this.cam.roll = 0.4*DEG*Math.sin(2*Math.PI*0.95*e)*env;
      this._foot(e);
    }
    if (ret) { if (e >= dEnd) { this.walk = null; this.phase = "choose"; Object.assign(this.cam,{x:0,y:3.15,z:16.6,yaw:0,pitch:-8*DEG,roll:0}); this._emit("choose"); } return; }
    if (e > 4.0 && e <= 6.6) {                                   // 抬頭一掃 (the reveal)
      this.cam.x = 0; this.cam.y = 3.15; this.cam.z = 16.6; this.cam.roll = 0;
      const k = (e - 4.0)/2.6;
      const p = e < 5.2 ? lerp(0, 18*DEG, easeOut((e-4.0)/1.2)) : lerp(18*DEG, -8*DEG, easeInOut((e-5.2)/1.4));
      this.cam.pitch = p; this.cam.yaw = lerp(-6*DEG, 6*DEG, easeInOut(k)) * 0.5;
      if (!w.bell && e >= 4.3) { w.bell = true; this._emit("bell"); }
    }
    if (e > 6.6) { this.walk = null; this.phase = "choose"; Object.assign(this.cam,{x:0,y:3.15,z:16.6,yaw:0,pitch:-8*DEG,roll:0}); this._emit("choose"); }
  }
  _foot(e){ const n = Math.floor(e/0.53); if (n !== this._lastFoot) { this._lastFoot = n; this._emit("footstep", {surface: this.cam.z > 17.5 ? "stone" : "carpet", pan: (n%2?0.15:-0.15)}); } }

  seatCode(i){ const s = this.seats[i]; return s ? s.code : ""; }
  bestSeat(){
    let best = -1, bs = -1e9;
    for (const s of this.seats) { if (s.occupied) continue; const sc = -Math.abs(s.x - 0.02) - 0.6*Math.abs(s.z - 11.3); /* 平手偏右 → D6 */ if (sc > bs) { bs = sc; best = s.i; } }
    return best;
  }
  /** 揀位: 一條直線滑去嗰行嘅腳位, 再坐低 (總共 ≤ 3.1 s)。 */
  pickSeat(i){
    const s = this.seats[i]; if (!s || s.occupied) return false;
    if (!(this.phase === "choose" || this.phase === "seated")) return false;
    const wasSeated = this.phase === "seated";
    this.seat = i; this.hover = -1; this.kbSel = -1; if (this.reservedSeat >= 0) { this.reservedSeat = -1; }
    this._writeSeats();
    this.phase = "glide"; this._emit("glide", s);
    const from = this._pose(), corridor = { x:s.x, y:s.y+1.65, z:s.z-0.55 };
    const dist = Math.hypot(from.x-corridor.x, from.y-corridor.y, from.z-corridor.z);
    const seatYaw = Math.atan2(s.x, s.z), seatPitch = Math.atan2(this.screenC[1] - (s.y+1.15), Math.hypot(s.x, s.z));
    const sit = { x:s.x, y:s.y+1.15, z:s.z+0.05, yaw:seatYaw, pitch:seatPitch, fov:55*DEG };
    const steps = [];
    if (dist > 0.6 && !wasSeated) steps.push({ dur: clamp(dist/3.0, 0.9, 2.2), to: {...corridor, yaw: lerp(from.yaw, seatYaw, 0.6), pitch: -2*DEG, fov: 62*DEG}, ease: easeInOut });
    steps.push({ dur: wasSeated ? 0.6 : 0.9, to: sit, ease: easeInOut, settle: true });
    this.look.yaw = this.look.pitch = 0;
    this._runSteps(steps, () => { this.phase = "seated"; this.lookIdle = this.now; this._emit("seated", s); });
    return true;
  }
  /** 坐緊嗰陣 ←/→ 跳去同排隔籬嘅空位; ↑/↓ 前後排最近嘅空位。 */
  hop(dx, dz){
    if (this.phase !== "seated" || this.seat < 0) return false;
    const me = this.seats[this.seat]; let cand = null;
    if (dx) { const row = this.seats.filter(s => s.r === me.r && !s.occupied && Math.sign(s.c - me.c) === Math.sign(dx)).sort((a,b) => Math.abs(a.c-me.c) - Math.abs(b.c-me.c)); cand = row[0]; }
    if (dz) { const r = me.r + (dz > 0 ? 1 : -1); const row = this.seats.filter(s => s.r === r && !s.occupied).sort((a,b) => Math.abs(a.x-me.x) - Math.abs(b.x-me.x)); cand = row[0]; }
    if (!cand) return false;
    return this.pickSeat(cand.i);
  }
  stand(){
    if (this.phase !== "seated") return;
    const s = this.seats[this.seat]; this.seat = -1; this._writeSeats();
    this.phase = "glide"; this.look.yaw = this.look.pitch = 0;
    this._runSteps([{ dur: 0.7, to: { x:s.x, y:s.y+1.65, z:s.z-0.55, yaw: this.cam.yaw, pitch: -4*DEG, fov: 62*DEG }, ease: easeInOut }],
      () => { this.phase = "choose"; this._emit("choose", {stood:true}); });
  }
  _pose(){ return { x:this.cam.x, y:this.cam.y, z:this.cam.z, yaw:this.cam.yaw+this.look.yaw, pitch:this.cam.pitch+this.look.pitch, fov:this.cam.fov }; }
  _runSteps(steps, done){
    const start = this._pose(); this.look.yaw = this.look.pitch = 0; Object.assign(this.cam, start);
    this.anim = { steps, i:0, t0:this.now, from:{...start}, done };
  }
  _animStep(t){
    const a = this.anim; if (!a) return;
    const st = a.steps[a.i], k = clamp((t - a.t0)/st.dur, 0, 1), e = st.ease(k);
    for (const key of ["x","y","z","yaw","pitch","fov"]) this.cam[key] = lerp(a.from[key], st.to[key] ?? a.from[key], e);
    if (st.settle && k > 0.7) this.cam.y -= 0.02*Math.sin((k-0.7)/0.3*Math.PI);
    if (k >= 1) { a.i++; if (a.i >= a.steps.length) { this.anim = null; a.done && a.done(); } else { a.t0 = t; a.from = this._pose(); } }
  }
  // 燈光
  house(target, tau = 0.7){ this.houseT = clamp(target, 0, 1); this.houseTau = tau; }
  curtain(open, seconds = 2.8){ this.curtainTw = { from: this.curtainV, to: open ? 1 : 0, t0: this.now, dur: seconds }; }
  screen(on, seconds = 1.5){ this.screenTw = { from: this.screenV, to: on ? 1 : 0, t0: this.now, dur: seconds }; }
  foot(on){ this.footT = on ? 1 : 0; }
  setScreenTint(rgb){ if (rgb) this.screenTint = rgb; }
  setQuality(lo){ this.lo = !!lo; }

  // ── 輸入 ──────────────────────────────────────────────────────────────
  _bindInput(){
    const cv = this.cv;
    const down = (x, y, touch) => { this.drag = {x, y, moved:0}; this.isTouch = !!touch; if (this.phase === "walk") this.skip(); };
    const move = (x, y) => {
      this.mouse.x = x; this.mouse.y = y;
      if (this.drag) { const dx = x - this.drag.x, dy = y - this.drag.y; this.drag.x = x; this.drag.y = y; this.drag.moved += Math.abs(dx)+Math.abs(dy);
        if (this.drag.moved >= 6) this._turn(-dx*(this.isTouch?0.004:0.0032), -dy*(this.isTouch?0.003:0.0024)); }
    };
    const up = (x, y) => { if (this.drag && this.drag.moved < 6) this._click(x, y); this.drag = null; };
    cv.addEventListener("mousedown", e => { if (e.button === 0) down(e.clientX, e.clientY, false); });
    addEventListener("mousemove", e => move(e.clientX, e.clientY));
    addEventListener("mouseup", e => { if (this.drag) up(e.clientX, e.clientY); });
    cv.addEventListener("mouseleave", () => { this.mouse.x = -1; });
    cv.style.touchAction = "none";
    cv.addEventListener("touchstart", e => { const t = e.touches[0]; down(t.clientX, t.clientY, true); }, {passive:true});
    cv.addEventListener("touchmove", e => { const t = e.touches[0]; move(t.clientX, t.clientY); }, {passive:true});
    cv.addEventListener("touchend", e => { const t = e.changedTouches[0]; up(t.clientX, t.clientY); this.mouse.x = -1; });
    addEventListener("keydown", e => {
      const k = e.key.toLowerCase(); this.keys.add(k);
      if (this.phase === "walk" && !e.metaKey && !e.ctrlKey) { this.skip(); return; }
      if (this.phase === "choose") {
        if (k.startsWith("arrow")) { e.preventDefault(); this._kbMove(k); }
        else if (k === "enter") { e.preventDefault(); this.pickSeat(this.kbSel >= 0 ? this.kbSel : this.bestSeat()); }
        else if (k === "b") this.pickSeat(this.bestSeat());
      } else if (this.phase === "seated") {
        if (k === "arrowleft") { e.preventDefault(); this.hop(-1, 0); }
        else if (k === "arrowright") { e.preventDefault(); this.hop(1, 0); }
        else if (k === "arrowup") { e.preventDefault(); this.hop(0, -1); }
        else if (k === "arrowdown") { e.preventDefault(); this.hop(0, 1); }
        else if (k === "e") this.stand();
      }
    });
    addEventListener("keyup", e => this.keys.delete(e.key.toLowerCase()));
    addEventListener("blur", () => this.keys.clear());
  }
  _kbMove(k){
    const cur = this.kbSel >= 0 ? this.seats[this.kbSel] : this.seats[this.bestIdx];
    let cand = null;
    if (k === "arrowleft" || k === "arrowright") { const d = k === "arrowleft" ? -1 : 1;
      cand = this.seats.filter(s => s.r === cur.r && !s.occupied && Math.sign(s.c-cur.c) === d).sort((a,b) => Math.abs(a.c-cur.c)-Math.abs(b.c-cur.c))[0]; }
    else { const r = cur.r + (k === "arrowup" ? -1 : 1);
      cand = this.seats.filter(s => s.r === r && !s.occupied).sort((a,b) => Math.abs(a.x-cur.x)-Math.abs(b.x-cur.x))[0]; }
    if (this.kbSel < 0 && !cand) cand = cur;
    if (cand) { this.kbSel = cand.i; this.hover = cand.i; this._writeSeats(); if (this.opts.onHover) this.opts.onHover(cand, {kb:true}); }
  }
  _turn(dy, dp){
    if (this.anim || this.walk) return;
    if (this.phase === "seated") {
      const ly = clamp(this.look.yaw + dy*(Math.abs(this.look.yaw) > 50*DEG ? 0.4 : 1), -60*DEG, 60*DEG);
      const lp = clamp(this.look.pitch + dp*(this.look.pitch > 25*DEG || this.look.pitch < -15*DEG ? 0.4 : 1), -25*DEG, 35*DEG);
      this.look.yaw = ly; this.look.pitch = lp; this.lookIdle = this.now; this.snap = false;
    } else if (this.phase === "choose") {
      this.cam.yaw += dy; this.cam.pitch = clamp(this.cam.pitch + dp, -40*DEG, 45*DEG);
    }
  }
  _click(x, y){
    if (this.phase === "choose" || this.phase === "seated") {
      const i = this._seatAt(x, y);
      if (i >= 0 && !this.seats[i].occupied) { this.pickSeat(i); return; }
      if (this.phase === "seated" && this.opts.onScreenClick && this._inScreenRect(x, y)) this.opts.onScreenClick();
      if (this.phase === "choose" && this.opts.onCurtainClick && this._inScreenRect(x, y) && this.curtainV < 0.5) this.opts.onCurtainClick();
    }
  }
  _inScreenRect(x, y){ const r = this.projectedScreenRect(); return r && x >= r.l && x <= r.r && y >= r.t && y <= r.b; }
  _seatAt(px, py){
    if (!this.VP) return -1;
    let best = -1, bd = 1e9;
    const F = this._F || 700, R = this.isTouch ? 0.75 : 0.55;
    for (const s of this.seats) {
      const p = this._proj(s.x, s.y + 0.49 + 0.25, s.z); if (!p) continue;
      const rpx = Math.max(10, R * F / p[2]);
      const d = Math.hypot(p[0]-px, p[1]-py);
      if (d < rpx && p[2] < bd) { bd = p[2]; best = s.i; }     // 最近鏡頭嗰張 (前面遮後面)
    }
    return best;
  }
  _proj(x, y, z){
    const m = this.VP;
    const cx = m[0]*x + m[4]*y + m[8]*z + m[12], cy = m[1]*x + m[5]*y + m[9]*z + m[13], cw = m[3]*x + m[7]*y + m[11]*z + m[15];
    if (cw <= 0.05) return null;
    return [(cx/cw*0.5+0.5)*this.W, (1-(cy/cw*0.5+0.5))*this.H, cw];
  }
  projectedScreenRect(){
    const G = this.G, y0 = G.screenY, y1 = G.screenY+G.screenH;
    const c = [this._proj(-5,y0,0), this._proj(5,y0,0), this._proj(5,y1,0), this._proj(-5,y1,0)];
    if (c.some(p => !p)) return null;
    return { l: Math.min(...c.map(p=>p[0])), r: Math.max(...c.map(p=>p[0])), t: Math.min(...c.map(p=>p[1])), b: Math.max(...c.map(p=>p[1])), c };
  }
  resize(w, h, dpr){ this.W = w; this.H = h; this.dpr = dpr; this.cv.width = Math.round(w*dpr); this.cv.height = Math.round(h*dpr); }
  state(){ return { phase:this.phase, seat: this.seat >= 0 ? this.seats[this.seat] : null, hover:this.hover, cam:{...this.cam}, look:{...this.look},
                    snap:this.snap, house:this.houseV, curtain:this.curtainV, screen:this.screenV, lo:this.lo, seats:this.seats }; }

  // ── 每格 ──────────────────────────────────────────────────────────────
  render(t){
    const gl = this.gl, G = this.G, P = this.P;
    const dt = Math.min(0.05, this._lt ? t - this._lt : 0.016); this._lt = t; this.now = t;
    // timers
    for (const tm of this.timers.splice(0)) { if (t >= tm.at) tm.fn(); else this.timers.push(tm); }
    if (this.walk) this._walkStep(t); else if (this.anim) this._animStep(t);
    if (this.phase === "load") { this.cam.yaw = 3*DEG*Math.sin(t*0.25*DEG*2*Math.PI/0.25*0.25); }     // 好慢嘅 idle 漂移 ±3°
    // choose 模式 WASD (冇提示, 隱藏功能)
    if (this.phase === "choose" && !this.anim) {
      const k = this.keys; let fx=0, fz=0;
      if (k.has("w")) fz -= 1; if (k.has("s")) fz += 1; if (k.has("a")) fx -= 1; if (k.has("d")) fx += 1;
      if (fx||fz) { const s=Math.sin(this.cam.yaw), c=Math.cos(this.cam.yaw), sp=1.4*dt, fwd=-fz, rgt=fx;
        this.cam.x = clamp(this.cam.x + (-s*fwd + c*rgt)*sp, -6.6, 6.6); this.cam.z = clamp(this.cam.z + (-c*fwd - s*rgt)*sp, 2.2, 21.2);
        const y = this._floorAt(this.cam.x, this.cam.z) + 1.65; this.cam.y += (y - this.cam.y)*(1-Math.pow(0.001, dt));
        if (Math.abs(this.cam.z) > 0) this._foot(t); }
    }
    // 坐低: 6 秒冇郁就慢慢望返正; 好接近正中就貼平 (2D snap)
    if (this.phase === "seated" && !this.anim) {
      if (t - this.lookIdle > 6 && (this.look.yaw || this.look.pitch)) { const f = 1 - Math.pow(0.02, dt); this.look.yaw *= 1-f; this.look.pitch *= 1-f;
        if (Math.abs(this.look.yaw) < 0.002 && Math.abs(this.look.pitch) < 0.002) this.look.yaw = this.look.pitch = 0; }
      this.snap = Math.abs(this.look.yaw) < 0.5*DEG && Math.abs(this.look.pitch) < 0.5*DEG && !this.drag;
      if (this.snap) { this.look.yaw = this.look.pitch = 0; }
    } else this.snap = false;
    // 燈光緩緩過渡 (τ)
    this.houseV += (this.houseT - this.houseV) * (1 - Math.exp(-dt/this.houseTau));
    this.footV  += (this.footT  - this.footV)  * (1 - Math.exp(-dt/0.7));
    if (this.curtainTw) { const c = this.curtainTw, k = clamp((t-c.t0)/c.dur, 0, 1); this.curtainV = lerp(c.from, c.to, easeInOutQuart(k)); if (k >= 1) { this.curtainTw = null; if (this.opts.onCurtain) this.opts.onCurtain(this.curtainV); } }
    if (this.screenTw) { const c = this.screenTw, k = clamp((t-c.t0)/c.dur, 0, 1); this.screenV = lerp(c.from, c.to, easeInOut(k)); if (k >= 1) this.screenTw = null; }
    // 銀幕光閃爍: random walk + 偶然「剪接」跳一跳
    if (t > this.flickT) { this.flickT = t + 0.14 + 0.19*Math.random(); this.flickTarget = clamp((this.flickTarget||1) + (Math.random()-0.5)*0.08, 0.92, 1.08); if (Math.random() < 0.05) this.flickTarget = clamp(this.flickTarget + (Math.random()<0.5?-0.15:0.15), 0.8, 1.2); }
    this.flicker += ((this.flickTarget||1) - this.flicker) * (1 - Math.pow(0.001, dt));
    const flick = this.flicker + 0.06*Math.sin(1.7*t) + 0.03*Math.sin(4.3*t);
    // hover (揀位)
    if (this.phase === "choose" && this.mouse.x >= 0 && !this.drag && !this.anim) {
      const h = this._seatAt(this.mouse.x, this.mouse.y);
      if (h !== this.hover) { this.hover = h; this.kbSel = -1; this._writeSeats(); if (this.opts.onHover) this.opts.onHover(h >= 0 ? this.seats[h] : null, {}); }
    } else if (this.hover >= 0 && this.phase !== "choose") { this.hover = -1; this._writeSeats(); if (this.opts.onHover) this.opts.onHover(null, {}); }
    // 銅牌發光: 推薦 0.45 / 預留脈動 / 我嘅 0.6 / hover 0.9
    { const p = this.plateData; let changed = false;
      this.seats.forEach((s, k) => { let g = 0.3;
        if (this.phase === "choose" && s.reco) g = 0.45;
        if (k === this.reservedSeat && this.phase === "choose") g = 0.55 + 0.25*Math.sin(t*0.8*2*Math.PI);
        if (k === this.seat) g = 0.6; if (k === this.hover) g = 0.9;
        if (this.phase === "seated" || this.phase === "glide") g *= 0.5 + 0.5*this.houseV;
        if (Math.abs(p[k*9+8] - g) > 0.01) { p[k*9+8] = g; changed = true; } });
      if (changed) this._upload(this.plateInst, p); }

    // ── 鏡頭 ────────────────────────────────────────────────────────────
    const c = this.cam, yaw = c.yaw + this.look.yaw, pitch = clamp(c.pitch + this.look.pitch, -1.2, 1.2);
    let sway = 0; if (this.phase === "seated" && !this.snap) sway = 0.15*DEG*Math.sin(t*0.12*2*Math.PI);
    const eye = [c.x, c.y, c.z];
    const dir = [-Math.cos(pitch)*Math.sin(yaw+sway), Math.sin(pitch), -Math.cos(pitch)*Math.cos(yaw+sway)];
    const up = [Math.sin(c.roll||0), Math.cos(c.roll||0), 0];
    const view = M4.lookAt(eye, [eye[0]+dir[0], eye[1]+dir[1], eye[2]+dir[2]], up);
    const proj = M4.persp(c.fov, this.W/this.H, 0.08, 80);
    const VP = this.VP = M4.mul(proj, view); this._F = proj[5]*this.H/2;
    const right = [view[0], view[4], view[8]], upV = [view[1], view[5], view[9]];
    const flatRight = [view[0], 0, view[8]]; { const l = Math.hypot(flatRight[0], flatRight[2])||1; flatRight[0]/=l; flatRight[2]/=l; }
    this._syncCss(view, proj);

    // ── 燈 ──────────────────────────────────────────────────────────────
    const screenLum = this.screenTint.map(v => v * this.screenV * this.curtainV * flick);
    const L = [];
    const inHall = c.z < 17.5;
    const near = this.lights.filter(l => inHall ? !l.vest : true).map(l => ({l, d: Math.hypot(l.x-c.x, l.y-c.y, l.z-c.z)})).sort((a,b) => a.d-b.d).slice(0, this.lo ? 3 : 4);
    for (const {l} of near) L.push([l.x - l.side*0.15, l.y, l.z], hex(P.warm).map(v => v*this.houseV*1.7), 6.5);
    L.push([0, 4.2, 0.6], screenLum, 20);
    if (!this.lo && this.footV > 0.02) for (const x of [-2, 2]) L.push([x, 0.6, 1.7], hex(P.warm).map(v => v*0.6*this.footV*this.houseV), 3);
    if (!this.lo && this.screenV > 0.02) L.push([0, 6.9, 17.4], [1.0,0.96,0.88].map(v => v*this.screenV*0.8), 3);
    const NL = Math.min(8, L.length/3), LP = new Float32Array(24), LC = new Float32Array(24), LR = new Float32Array(8);
    for (let i = 0; i < NL; i++) { LP.set(L[i*3], i*3); LC.set(L[i*3+1], i*3); LR[i] = L[i*3+2]; }
    const fogC = [0.027,0.031,0.039].map((v,i) => lerp(v, screenLum[i]*0.08 + v, this.screenV));
    const setLight = (p) => {
      gl.uniform1f(p.u.uHouse, this.houseV); gl.uniform3fv(p.u.uWarm, hex(P.warm)); gl.uniform3fv(p.u.uEye, eye);
      gl.uniform3fv(p.u.uFogC, fogC); gl.uniform1f(p.u.uFogD, 30);
      gl.uniform1i(p.u.uNL, NL); gl.uniform3fv(p.u.uLP, LP); gl.uniform3fv(p.u.uLC, LC); gl.uniform1fv(p.u.uLR, LR);
    };

    // ── 畫 ──────────────────────────────────────────────────────────────
    gl.viewport(0,0,this.cv.width,this.cv.height);
    gl.clearColor(0.02,0.02,0.024,1); gl.clear(gl.COLOR_BUFFER_BIT|gl.DEPTH_BUFFER_BIT);
    gl.enable(gl.DEPTH_TEST); gl.depthFunc(gl.LEQUAL); gl.enable(gl.BLEND); gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA); gl.disable(gl.CULL_FACE);

    const S = this.pSolid; gl.useProgram(S);
    gl.uniformMatrix4fv(S.u.uVP, false, VP); setLight(S);
    const draw = (mesh, model, col, {emit=0, alpha=1, tex=null, uvs=[1,1], mode=0, sheen="#000000", spec=0} = {}) => {
      gl.uniform3fv(S.u.uColor, hex(col)); gl.uniform3fv(S.u.uSheen, hex(sheen)); gl.uniform1f(S.u.uSpec, spec);
      gl.uniform1f(S.u.uEmit, emit); gl.uniform1f(S.u.uAlpha, alpha); gl.uniform1f(S.u.uMode, mode); gl.uniform2fv(S.u.uUvScale, uvs);
      gl.uniform1f(S.u.uTex, tex ? 1 : 0); if (tex) { gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D, tex); gl.uniform1i(S.u.uSampler, 0); }
      gl.uniformMatrix4fv(S.u.uModel, false, model); gl.bindVertexArray(mesh.vao); gl.drawArrays(gl.TRIANGLES, 0, mesh.n);
    };
    const T = this.tex, HW = G.hallW/2, D = G.hallD, HH = G.hallH;
    // 地台: 樂池地 + 每排踏級 + 踢腳
    draw(this.box, M4.chain(M4.transl(0,-0.25,3.75), M4.scale(G.hallW,0.5,3.9)), P.floor, {tex:T.carpet, uvs:[G.hallW/1.5, 3.9/1.5]});
    for (let r = 0; r < 6; r++) { const z = G.rowZ(r), y = G.treadY(r);
      draw(this.box, M4.chain(M4.transl(0, y-0.25, z-0.25), M4.scale(G.hallW, 0.5, 1.6)), P.floor, {tex:T.carpet, uvs:[G.hallW/1.5, 1.6/1.5]});
      if (r > 0) draw(this.box, M4.chain(M4.transl(0, y-0.15, z-1.05), M4.scale(G.hallW-2.4, 0.30, 0.05)), P.walnut, {spec:12});
    }
    draw(this.box, M4.chain(M4.transl(0, G.crossY-0.25, 16.4), M4.scale(G.hallW, 0.5, 2.2)), P.floor, {tex:T.carpet, uvs:[G.hallW/1.5, 2.2/1.5]});
    // 走廊銅條 (step lights, 永遠唔低過 0.15)
    const stripE = Math.max(this.houseV, 0.15) * 0.3;
    for (let r = 0; r < 6; r++) for (const x of [-6.7,-5.5,5.5,6.7]) draw(this.box, M4.chain(M4.transl(x, G.treadY(r)+0.005, G.rowZ(r)-0.25), M4.scale(0.02, 0.01, 1.6)), P.strip, {mode:1, emit:stripE*4});
    // 牆 / 天花 / 後牆 (門口留空)
    draw(this.box, M4.chain(M4.transl(-HW-0.25, HH/2, D/2), M4.scale(0.5, HH, D)), P.wall, {tex:T.ribs, uvs:[D/3.84, 1]});
    draw(this.box, M4.chain(M4.transl( HW+0.25, HH/2, D/2), M4.scale(0.5, HH, D)), P.wall, {tex:T.ribs, uvs:[D/3.84, 1]});
    draw(this.box, M4.chain(M4.transl(0, HH+0.25, D/2), M4.scale(G.hallW+1, 0.5, D)), P.ceiling);
    draw(this.box, M4.chain(M4.transl(-(HW+1.1)/2-0.55, HH/2, D+0.25), M4.scale(HW-1.1, HH, 0.5)), P.wall, {tex:T.ribs, uvs:[(HW-1.1)/3.84,1]});
    draw(this.box, M4.chain(M4.transl( (HW+1.1)/2+0.55, HH/2, D+0.25), M4.scale(HW-1.1, HH, 0.5)), P.wall, {tex:T.ribs, uvs:[(HW-1.1)/3.84,1]});
    draw(this.box, M4.chain(M4.transl(0, (HH+4.1)/2, D+0.25), M4.scale(2.2, HH-4.1, 0.5)), P.wall, {tex:T.ribs, uvs:[2.2/3.84,1]});
    for (const s of [-1,1]) draw(this.box, M4.chain(M4.transl(s*HW, 0.06, D/2), M4.scale(0.08, 0.12, D)), P.walnut, {spec:12});
    // 舞台 + 幕框 + 遮光帶 + 銅線 + 柱 + 過樑
    draw(this.box, M4.chain(M4.transl(0, 0.225, 0.9), M4.scale(G.hallW, 0.45, 1.8)), P.stage, {spec:40});
    draw(this.box, M4.chain(M4.transl(0, 0.225, 1.8), M4.scale(G.hallW, 0.45, 0.06)), P.walnut, {spec:12});
    const sy = this.screenC[1];
    draw(this.quad, M4.chain(M4.transl(0, sy, 0.02), M4.scale(G.screenW+1.2, G.screenH+1.2, 1)), P.masking);
    draw(this.quad, M4.chain(M4.transl(0, sy, 0.0), M4.scale(G.screenW, G.screenH, 1)), "#050506", {mode:1, emit:0.25});
    const hl = 0.4; for (const [w,h,x,y] of [[G.screenW+2*hl, 0.015, 0, sy+G.screenH/2+hl],[G.screenW+2*hl, 0.015, 0, sy-G.screenH/2-hl],[0.015, G.screenH+2*hl, -5-hl, sy],[0.015, G.screenH+2*hl, 5+hl, sy]])
      draw(this.quad, M4.chain(M4.transl(x, y, 0.03), M4.scale(w, h, 1)), P.brass, {mode:1, emit:0.35*3});
    for (const s of [-1,1]) { draw(this.box, M4.chain(M4.transl(s*5.9, (0.45+7.6)/2, 0.25), M4.scale(0.6, 7.15, 0.5)), P.wall); draw(this.box, M4.chain(M4.transl(s*5.9, 7.65, 0.25), M4.scale(0.64, 0.1, 0.54)), P.walnut, {spec:12}); }
    draw(this.box, M4.chain(M4.transl(0, 7.9, 0.6), M4.scale(G.hallW, 0.6, 1.2)), P.coffer);
    // 幕 (兩幅, 由外邊向內縮; 開幕 = scale 1 → 0.10)
    const sc = 1 - 0.9*this.curtainV;
    for (const s of [-1,1]) { const w = 5.9*sc, x = s*(5.6 - w/2);
      draw(this.quad, M4.chain(M4.transl(x, (7.45+1.2)/2, 0.15), M4.scale(w*s, 6.25, 1)), P.velvet, {tex:T.curtain, sheen:P.velvetSheen, spec:6, uvs:[1,1]}); }
    draw(this.box, M4.chain(M4.transl(0, 7.275, 0.175), M4.scale(12.4, 0.45, 0.25)), P.velvet, {tex:null, sheen:P.velvetSheen, spec:6});
    if (T.pelmet) draw(this.quad, M4.chain(M4.transl(0, 7.05-0.2, 0.31), M4.scale(12.4, 0.5, 1)), P.velvet, {tex:T.pelmet, sheen:P.velvetSheen, spec:6});
    draw(this.box, M4.chain(M4.transl(0, 7.05, 0.31), M4.scale(12.4, 0.02, 0.02)), P.brass, {mode:1, emit:0.9});
    // 腳燈 + 壁燈 (發光箱 + 銅托)
    for (const f of this.foots) draw(this.box, M4.chain(M4.transl(f.x, f.y, f.z), M4.scale(0.10, 0.04, 0.06)), P.warm, {mode:1, emit: 0.2 + 2.2*this.footV*this.houseV});
    for (const l of this.lights) { if (!inHall && !l.vest && l.z < 14) continue;
      draw(this.box, M4.chain(M4.transl(l.x - l.side*0.11, l.y, l.z), M4.scale(0.12, 0.30, 0.06)), P.warm, {mode:1, emit: 0.15 + 2.0*this.houseV});
      draw(this.box, M4.chain(M4.transl(l.x - l.side*0.04, l.y-0.1, l.z), M4.scale(0.04, 0.06, 0.10)), P.brass, {spec:64, sheen:"#7a6238"}); }
    // 投影窗
    draw(this.quad, M4.chain(M4.transl(0, 6.9, D-0.03), M4.scale(0.5, 0.3, 1)), "#FFF4E0", {mode:1, emit: 0.3 + 2.5*this.screenV});
    // 舞台反光 cheat
    if (this.screenV > 0.02 && this.curtainV > 0.02) draw(this.quad, M4.chain(M4.transl(0, 0.46, 0.9), M4.rotX(-Math.PI/2), M4.scale(10, 1.8, 1)), "#C9D6F0", {mode:1, emit: 0.9*this.screenV*this.curtainV*flick, alpha: 0.25});
    // 前廳: 石地、牆、天花、門框、海報、跑馬燈
    const VZ0 = G.vestZ0, VZ1 = G.vestZ1, VW = G.vestW/2;
    draw(this.box, M4.chain(M4.transl(0, G.crossY-0.25, (VZ0+VZ1)/2), M4.scale(G.vestW, 0.5, VZ1-VZ0)), "#0B0A0C", {tex:T.stone, uvs:[1, 1], spec:40});
    for (const s of [-1,1]) draw(this.box, M4.chain(M4.transl(s*(VW+0.25), (G.crossY+G.vestCeil)/2, (VZ0+VZ1)/2), M4.scale(0.5, G.vestCeil-G.crossY+0.2, VZ1-VZ0)), P.vestWall, {tex:T.ribs, uvs:[(VZ1-VZ0)/3.84,1]});
    draw(this.box, M4.chain(M4.transl(0, G.vestCeil+0.25, (VZ0+VZ1)/2), M4.scale(G.vestW+1, 0.5, VZ1-VZ0)), P.ceiling);
    draw(this.box, M4.chain(M4.transl(0, (G.crossY+G.vestCeil)/2, VZ1+0.25), M4.scale(G.vestW+1, G.vestCeil-G.crossY+0.2, 0.5)), P.vestWall, {tex:T.ribs, uvs:[G.vestW/3.84,1]});
    for (const s of [-1,1]) draw(this.box, M4.chain(M4.transl(s*1.16, (G.crossY+4.1)/2, VZ0), M4.scale(0.12, 4.1-G.crossY, 0.14)), P.brass, {spec:64, sheen:"#7a6238"});
    draw(this.box, M4.chain(M4.transl(0, 4.16, VZ0), M4.scale(2.44, 0.12, 0.14)), P.brass, {spec:64, sheen:"#7a6238"});
    // 門: 兩塊, 喺外邊門框轉軸, 向場內 (−z) 開 ±100°
    const da = easeInOutQuart(this.doorA) * 100*DEG;
    for (const s of [-1,1]) { const m = M4.chain(M4.transl(s*1.10, G.crossY, VZ0), M4.rotY(-s*da), M4.transl(-s*0.55, 1.3, 0));
      draw(this.box, M4.chain(m, M4.scale(1.10, 2.60, 0.06)), P.door, {spec:12, sheen:"#2a1a10"});
      draw(this.box, M4.chain(m, M4.transl(-s*0.15, -0.25, -0.05), M4.scale(0.60, 0.04, 0.05)), P.brass, {spec:64, sheen:"#7a6238"}); }
    for (const po of this.posters) {
      draw(this.quad, M4.chain(M4.transl(po.x, po.y, po.z), M4.rotY(-po.side*Math.PI/2), M4.scale(0.76, 1.06, 1)), P.brass, {spec:64, sheen:"#7a6238"});
      draw(this.quad, M4.chain(M4.transl(po.x - po.side*0.02, po.y, po.z), M4.rotY(-po.side*Math.PI/2), M4.scale(0.70, 1.00, 1)), "#ffffff", {tex:po.tex, mode:1, emit:0.75}); }
    if (this.marqueeTex) {
      const mq = (tex, alpha) => { draw(this.quad, M4.chain(M4.transl(0, 4.35, VZ0+0.05), M4.rotY(Math.PI), M4.scale(3.6, 0.35, 1)), "#fff", {tex, mode:1, emit:0.8, alpha});
                                   draw(this.quad, M4.chain(M4.transl(0, 7.9, 1.21), M4.scale(3.6*2, 0.35*2, 1)), "#fff", {tex, mode:1, emit:0.7, alpha}); };
      if (this.marqueeOld && this.marqueeMix < 1) { this.marqueeMix = Math.min(1, this.marqueeMix + dt/0.4); mq(this.marqueeOld, 1-this.marqueeMix); mq(this.marqueeTex, this.marqueeMix); }
      else mq(this.marqueeTex, 1);
    }
    // 預留卡 (D6 坐墊上)
    if (this.reservedSeat >= 0 && this.phase !== "load" && T.reserved) { const s = this.seats[this.reservedSeat];
      draw(this.quad, M4.chain(M4.transl(s.x, s.y+0.57, s.z-0.05), M4.rotY(s.yaw), M4.rotX(-Math.PI/2+0.25), M4.scale(0.12, 0.08, 1)), "#fff", {tex:T.reserved, mode:1, emit:0.9}); }

    // ── 座位 (instanced 部件) ────────────────────────────────────────────
    const I = this.pInst; gl.useProgram(I);
    gl.uniformMatrix4fv(I.u.uVP, false, VP); setLight(I);
    gl.bindVertexArray(this.seatInst.vao);
    const part = (col, sheen, spec, ox,oy,oz, sx,sy2,sz, tilt=0, hoverBoost=0) => {
      gl.uniform3fv(I.u.uColor, hex(col)); gl.uniform3fv(I.u.uSheen, hex(sheen)); gl.uniform1f(I.u.uSpec, spec); gl.uniform1f(I.u.uHoverBoost, hoverBoost);
      gl.uniform3f(I.u.uOff, ox,oy,oz); gl.uniform3f(I.u.uScale, sx,sy2,sz); gl.uniform1f(I.u.uTilt, tilt);
      gl.drawArraysInstanced(gl.TRIANGLES, 0, 36, this.seats.length); };
    part(P.seatBase, "#000000", 0,       0, 0.21, 0,     0.72, 0.42, 0.55);                  // 底座
    part(P.leather, P.leatherSheen, 24,  0, 0.49, 0,     0.70, 0.14, 0.55, 0, 0.35);          // 坐墊
    part(P.leather, P.leatherSheen, 24,  0, 0.56, 0.20,  0.72, 0.62, 0.16, -12*DEG);          // 靠背 (向後傾; box 中心要升半個高度: 用 tilt 之前嘅 y 偏移)
    part(P.headrest, P.leatherSheen, 24, 0, 1.06, 0.31,  0.50, 0.20, 0.12);                   // 頭枕
    for (const s of [-1,1]) { part(P.leather, P.leatherSheen, 24, s*0.36, 0.73, 0.0, 0.10, 0.62, 0.55); part(P.walnut, "#000000", 12, s*0.36, 1.05, 0.0, 0.10, 0.02, 0.55); }
    // 銅牌
    const PL = this.pPlate; gl.useProgram(PL);
    gl.uniformMatrix4fv(PL.u.uVP, false, VP); setLight(PL); gl.uniform2f(PL.u.uSize, 0.14, 0.07);
    gl.uniform1f(PL.u.uHasTex, T.plate ? 1 : 0); if (T.plate) { gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D, T.plate); gl.uniform1i(PL.u.uSampler, 0); }
    gl.bindVertexArray(this.plateInst.vao); gl.drawArraysInstanced(gl.TRIANGLES, 0, 6, this.plateInst.count);
    // ── 觀眾剪影 ────────────────────────────────────────────────────────
    const SI = this.pSil; gl.useProgram(SI);
    gl.uniformMatrix4fv(SI.u.uVP, false, VP); setLight(SI);
    gl.uniform3fv(SI.u.uRight, flatRight); gl.uniform1f(SI.u.uTime, t); gl.uniform1f(SI.u.uPre, this.houseV > 0.45 && this.curtainV < 0.5 ? 1 : 0);
    gl.uniform3fv(SI.u.uScreenLum, screenLum);
    gl.bindVertexArray(this.silInst.vao); gl.drawArraysInstanced(gl.TRIANGLES, 0, 6, this.silInst.count);

    // ── 加光層: 光暈 / 星 / 光束 / 塵 ───────────────────────────────────
    gl.blendFunc(gl.ONE, gl.ONE); gl.depthMask(false);
    const SP = this.pSpr; gl.useProgram(SP);
    gl.uniformMatrix4fv(SP.u.uVP, false, VP); gl.uniform3fv(SP.u.uRight, right); gl.uniform3fv(SP.u.uUp, upV); gl.uniform1f(SP.u.uTime, t);
    if (!this.lo) { gl.uniform1f(SP.u.uMode, 0); gl.uniform1f(SP.u.uGain, 0.35*this.houseV); gl.bindVertexArray(this.haloInst.vao); gl.drawArraysInstanced(gl.TRIANGLES, 0, 6, this.haloInst.count); }
    gl.uniform1f(SP.u.uMode, 2); gl.uniform1f(SP.u.uGain, 0.4); gl.bindVertexArray(this.starInst.vao); gl.drawArraysInstanced(gl.TRIANGLES, 0, 6, this.lo ? 40 : 90);
    const beam = this.screenV * this.curtainV * flick;
    if (beam > 0.01) {
      gl.useProgram(this.pBeam); gl.uniformMatrix4fv(this.pBeam.u.uVP, false, VP); gl.uniformMatrix4fv(this.pBeam.u.uModel, false, M4.transl(0,0,0)); gl.uniform2f(this.pBeam.u.uUvScale, 1, 1);
      gl.uniform1f(this.pBeam.u.uGain, 0.05*beam); gl.uniform1f(this.pBeam.u.uTime, t); gl.uniform3f(this.pBeam.u.uColor, 0.85, 0.82, 0.78);
      gl.bindVertexArray(this.beamMesh.vao); gl.drawArrays(gl.TRIANGLES, 0, this.beamMesh.n);
      if (!this.lo) { const DU = this.pDust; gl.useProgram(DU); gl.uniformMatrix4fv(DU.u.uVP, false, VP);
        gl.uniform3f(DU.u.uApex, 0, 6.9, 17.4); gl.uniform3f(DU.u.uAxis, 0, sy-6.9, -17.4); gl.uniform3f(DU.u.uHalfW, 5, 0, 0); gl.uniform3f(DU.u.uHalfH, 0, G.screenH/2, 0);
        gl.uniform3fv(DU.u.uRight, right); gl.uniform3fv(DU.u.uUp, upV); gl.uniform1f(DU.u.uTime, t); gl.uniform1f(DU.u.uGain, 0.5*beam); gl.uniform3f(DU.u.uColor, 0.9, 0.86, 0.8);
        gl.bindVertexArray(this.dustInst.vao); gl.drawArraysInstanced(gl.TRIANGLES, 0, 6, this.dustInst.count); }
      // 銀幕光暈帶
      gl.useProgram(S); gl.uniformMatrix4fv(S.u.uVP, false, VP);
      const halo = 0.10*beam + (this.haloPeak||0);
      draw(this.quad, M4.chain(M4.transl(0, sy, 0.01), M4.scale(G.screenW+1.0, G.screenH+1.0, 1)), "#C9D6F0", {mode:1, emit:halo, alpha:0.5});
    }
    gl.depthMask(true); gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA); gl.bindVertexArray(null);
    if (this.haloPeak) this.haloPeak = Math.max(0, this.haloPeak - dt*0.4);
    // 畫質探測 (choose 開始之後 90 格)
    if (this.probe) { this.probe.push(dt*1000); if (this.probe.length >= 90) { const m = this.probe.slice().sort((a,b)=>a-b)[45]; this.probe = null; if (m > 19) { this.lo = true; this._emit("lo"); } } }
  }
  _floorAt(x, z){
    const G = this.G; if (z >= 15.3) return G.crossY; if (z < 5.45) return 0;
    for (let r = 5; r >= 0; r--) if (z >= G.rowZ(r) - 1.05) return G.treadY(r);
    return 0;
  }
  startProbe(){ this.probe = []; }
  thump(){ this.haloPeak = 0.45; }

  // ── CSS3D: iframe 貼落個幕 (同 WebGL 同一組矩陣); 望正就貼平做 2D ────────
  _syncCss(view, proj){
    const el = this.screenEl, layer = this.css; if (!el || !layer) return;
    const G = this.G, W = this.W, Hh = this.H, F = proj[5]*Hh/2, camEl = layer.firstElementChild;
    const f = v => (Math.abs(v) < 1e-6 ? 0 : v).toFixed(6);
    if (!this._cssInit) { this._cssInit = true; camEl.style.transformOrigin = "0 0"; el.style.transformOrigin = "0 0"; el.style.width = "1600px"; el.style.height = "900px"; }
    if (this.snap) {
      const r = this.projectedScreenRect();
      if (r) { const l = Math.round(r.l), tp = Math.round(r.t), w = Math.round(r.r - r.l), h = Math.round(r.b - r.t);
        const o = `translate(${l}px,${tp}px) scale(${(w/1600).toFixed(5)},${(h/900).toFixed(5)})`;
        if (this._camCss !== "none") { camEl.style.transform = "none"; layer.style.perspective = "none"; this._camCss = "none"; }
        if (this._objCss !== o) { el.style.transform = o; this._objCss = o; } }
      return;
    }
    if (this._F !== F || this._camCss === "none") { layer.style.perspective = F + "px"; this._F = F; }
    const e = view;
    const cam = `translate3d(${W/2}px,${Hh/2}px,${f(F)}px) matrix3d(${[e[0],-e[1],e[2],e[3], e[4],-e[5],e[6],e[7], e[8],-e[9],e[10],e[11], e[12],-e[13],e[14],e[15]].map(f).join(",")})`;
    if (this._camCss !== cam) { camEl.style.transform = cam; this._camCss = cam; }
    const s = G.screenW/1600, m = M4.chain(M4.transl(0, G.screenY + G.screenH/2, 0.0), M4.scale(s, s, s));
    const o = `matrix3d(${[m[0],m[1],m[2],m[3], -m[4],-m[5],-m[6],-m[7], m[8],m[9],m[10],m[11], m[12],m[13],m[14],m[15]].map(f).join(",")}) translate(-50%,-50%)`;
    if (this._objCss !== o) { el.style.transform = o; this._objCss = o; }
  }
  /** 幕布前緣 → iframe clip-path (幕布永遠遮住片邊 10 cm)。 */
  curtainClip(){
    const sc = 1 - 0.9*this.curtainV, xl = -5.6 + 5.9*sc, xr = 5.6 - 5.9*sc;
    const L = clamp((xl + 0.10 + 5.0)/10, 0, 1), R = clamp((5.0 - (xr - 0.10))/10, 0, 1);
    return { L, R, hidden: L + R >= 0.999 };
  }
}

window.Theatre = {
  supported(){ try { return !!document.createElement("canvas").getContext("webgl2"); } catch (e) { return false; } },
  create(cv, css, screenEl, opts){ return new Theatre(cv, css, screenEl, opts); },
};
})();
