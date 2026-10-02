#!/usr/bin/env python3
"""Live Arena server: 觀眾入場網頁 + WebSocket 中轉 + YouTube 即時觀看人數。

純標準庫（Python 3.9+），唔使 pip。協議見 ../PROTOCOL.md。

    python3 server.py --host 0.0.0.0 --port 8787 --grace 120

    觀眾入場：  http://<呢部機 IP>:8787/r/<房間名>
    UE host：   ws://<呢部機 IP>:8787/ws   （UE 自己加 ?role=host&room=<房間名>&key=<HostKey>）

想攞 YouTube 即時人數：開 server 之前設定環境變數 YOUTUBE_API_KEY，UE 嘅 config 填 videoId。
"""

import argparse
import asyncio
import base64
import hashlib
import heapq
import hmac
import json
import logging
import os
import re
import secrets
import socket
import struct
import sys
import unicodedata
import urllib.error
import urllib.parse
import urllib.request
from http import HTTPStatus
from typing import Any, Callable, Dict, List, Optional, Set, Tuple

log = logging.getLogger("live_arena")

# --------------------------------------------------------------------------------------------
# 常數
# --------------------------------------------------------------------------------------------

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
MAX_MESSAGE = 64 * 1024           # 單條 WebSocket 訊息上限（bytes）
MAX_OUT_QUEUE = {"viewer": 512, "host": 50000}   # 未送出嘅 frame 上限，爆咗當慢 client 踢走
CLOSE_TIMEOUT = 2.0               # 送 close frame 之後等對方回覆嘅時間
SEND_TIMEOUT = 30.0               # 一次 drain 最多等幾耐

OP_CONT, OP_TEXT, OP_BINARY = 0x0, 0x1, 0x2
OP_CLOSE, OP_PING, OP_PONG = 0x8, 0x9, 0xA

ROOM_RE = re.compile(r"[A-Za-z0-9_-]{1,32}")
ID_RE = re.compile(r"[A-Za-z0-9_-]{1,64}")
YT_ID_RE = re.compile(r"[A-Za-z0-9_-]{11}")
VIDEO_ID_RE = re.compile(r"[A-Za-z0-9_-]{6,20}")

DEFAULT_ROWS = [34 + r for r in range(28)]   # 34, 35, ... 61
MAX_LAYOUT_ROWS = 200
MAX_ROW_SEATS = 2000
CLAP_INTERVAL = 0.3
STATE_INTERVAL = 1.0
DEFAULT_NAME = "觀眾"
NAME_MAX = 20
MAX_ROOMS = 2000
EXTRA_PENDING_CONNS = 500         # 未 hello 嘅觀眾連線（睇緊入場券）額外上限
YT_ENDPOINT = "https://www.googleapis.com/youtube/v3/videos"


def dumps(obj: Any) -> bytes:
    return json.dumps(obj, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


# --------------------------------------------------------------------------------------------
# 座位公式（一定要同 LiveArena/Source/LiveArena/ArenaTypes.h 嘅 ArenaSlots 一模一樣）
# --------------------------------------------------------------------------------------------

def center_out(j: int, n: int) -> int:
    """行內第 j 個入座嘅人坐第幾個位（0 = 觀眾望住舞台最左）。由中間向兩邊填。"""
    mid = n // 2
    return mid + j // 2 if j % 2 == 0 else mid - (j + 1) // 2


def row_name(r: int) -> str:
    """0..25 = A..Z，26 = AA，27 = AB ……"""
    if r < 26:
        return chr(ord("A") + r)
    return chr(ord("A") + r // 26 - 1) + chr(ord("A") + r % 26)


def slot_position(slot: int, rows: List[int]) -> Tuple[int, int]:
    """slot -> (行, 行內位置)。slot 超出 layout 總位數嘅話，用最後一行嘅位數繼續加虛擬行。"""
    r = 0
    for n in rows:
        if slot < n:
            return r, center_out(slot, n)
        slot -= n
        r += 1
    n = rows[-1]
    return r + slot // n, center_out(slot % n, n)


def slot_label(slot: int, rows: List[int]) -> str:
    r, pos = slot_position(slot, rows)
    return row_name(r) + str(pos + 1)


# --------------------------------------------------------------------------------------------
# 輸入清理
# --------------------------------------------------------------------------------------------

def _strip_controls(s: str) -> str:
    # 刪走所有 Unicode "C*" 類字元：控制字元、格式字元（零闊、RTL override）、私用區等
    return "".join(ch for ch in s if not unicodedata.category(ch).startswith("C"))


def sanitize_name(raw: Any) -> str:
    """去控制字元、strip、空白壓成一個、最多 20 字；空就叫「觀眾」。"""
    s = raw if isinstance(raw, str) else ""
    s = " ".join(_strip_controls(s[:1000]).split())
    s = s[:NAME_MAX].strip()
    return s or DEFAULT_NAME


def clean_text(raw: Any, max_len: int) -> str:
    if not isinstance(raw, str):
        return ""
    return " ".join(_strip_controls(raw[: max_len * 4]).split())[:max_len].strip()


def clean_url(raw: Any, max_len: int = 500) -> str:
    """只收 http(s) 絕對網址；其他（javascript: 之類）一律當空。"""
    if not isinstance(raw, str):
        return ""
    s = raw.strip()
    if not s or len(s) > max_len:
        return ""
    if any(ch.isspace() or unicodedata.category(ch).startswith("C") for ch in s):
        return ""
    try:
        parts = urllib.parse.urlsplit(s)
    except ValueError:
        return ""
    if parts.scheme.lower() not in ("http", "https") or not parts.netloc:
        return ""
    return s


def youtube_id_from(value: str) -> str:
    """由 YouTube 網址（watch?v= / youtu.be / live/ / embed/ / shorts/）或者直接 id 攞 11 位片 id。"""
    s = (value or "").strip()
    if YT_ID_RE.fullmatch(s):
        return s
    try:
        parts = urllib.parse.urlsplit(s)
    except ValueError:
        return ""
    host = (parts.hostname or "").lower()
    if host.startswith("www.") or host.startswith("m."):
        host = host.split(".", 1)[1]
    cand = ""
    if host == "youtu.be":
        cand = parts.path.strip("/").split("/")[0]
    elif host in ("youtube.com", "music.youtube.com", "youtube-nocookie.com"):
        v = urllib.parse.parse_qs(parts.query).get("v")
        if v:
            cand = v[0]
        else:
            segs = [p for p in parts.path.split("/") if p]
            if len(segs) >= 2 and segs[0] in ("live", "embed", "shorts", "v"):
                cand = segs[1]
    return cand if YT_ID_RE.fullmatch(cand) else ""


# --------------------------------------------------------------------------------------------
# WebSocket frame（RFC 6455）—— selftest.py 都會 import 呢幾個嚟做 client
# --------------------------------------------------------------------------------------------

class WSError(Exception):
    """協議錯誤；code 係要回畀對方嘅 close code。"""

    def __init__(self, code: int, reason: str = ""):
        super().__init__("%d %s" % (code, reason))
        self.code = code
        self.reason = reason


def apply_mask(data: bytes, key: bytes) -> bytes:
    if not data:
        return b""
    n = len(data)
    k = (key * (n // 4 + 1))[:n]
    return (int.from_bytes(data, "big") ^ int.from_bytes(k, "big")).to_bytes(n, "big")


def encode_frame(opcode: int, payload: bytes = b"", fin: bool = True, mask: bool = False) -> bytes:
    """砌一個 frame。server 送出嘅唔 mask；client（selftest）送出嘅要 mask=True。"""
    b0 = (0x80 if fin else 0) | (opcode & 0x0F)
    mbit = 0x80 if mask else 0
    n = len(payload)
    if n < 126:
        head = struct.pack("!BB", b0, mbit | n)
    elif n < 65536:
        head = struct.pack("!BBH", b0, mbit | 126, n)
    else:
        head = struct.pack("!BBQ", b0, mbit | 127, n)
    if mask:
        key = os.urandom(4)
        return head + key + apply_mask(payload, key)
    return head + payload


async def read_frame(reader: asyncio.StreamReader, max_size: int = MAX_MESSAGE,
                     require_mask: bool = True) -> Tuple[bool, int, bytes]:
    """讀一個 frame，返回 (fin, opcode, payload)。協議錯誤 raise WSError。"""
    b0, b1 = await reader.readexactly(2)
    fin = bool(b0 & 0x80)
    if b0 & 0x70:
        raise WSError(1002, "reserved bits set")
    opcode = b0 & 0x0F
    masked = bool(b1 & 0x80)
    n = b1 & 0x7F
    if opcode in (OP_CLOSE, OP_PING, OP_PONG):
        if not fin:
            raise WSError(1002, "fragmented control frame")
        if n > 125:
            raise WSError(1002, "control frame too long")
    elif opcode not in (OP_CONT, OP_TEXT, OP_BINARY):
        raise WSError(1002, "unknown opcode")
    if n == 126:
        (n,) = struct.unpack("!H", await reader.readexactly(2))
    elif n == 127:
        (n,) = struct.unpack("!Q", await reader.readexactly(8))
        if n >> 63:
            raise WSError(1002, "bad length")
    if require_mask and not masked:
        raise WSError(1002, "client frames must be masked")
    if n > max_size:
        raise WSError(1009, "message too big")
    key = await reader.readexactly(4) if masked else b""
    payload = await reader.readexactly(n) if n else b""
    if masked:
        payload = apply_mask(payload, key)
    return fin, opcode, payload


async def read_message(reader: asyncio.StreamReader, require_mask: bool = True,
                       max_size: int = MAX_MESSAGE,
                       on_control: Optional[Callable[[int, bytes], None]] = None) -> Tuple[int, bytes]:
    """讀一條完整訊息（處理 continuation）。ping/pong 交畀 on_control。

    返回 (OP_TEXT|OP_BINARY, data) 或者 (OP_CLOSE, close payload)。
    """
    parts: List[bytes] = []
    total = 0
    first_op: Optional[int] = None
    while True:
        fin, op, payload = await read_frame(reader, max_size, require_mask)
        if op in (OP_PING, OP_PONG):
            if on_control is not None:
                on_control(op, payload)
            continue
        if op == OP_CLOSE:
            if len(payload) == 1:
                raise WSError(1002, "bad close payload")
            return OP_CLOSE, payload
        if op == OP_CONT:
            if first_op is None:
                raise WSError(1002, "unexpected continuation")
        else:
            if first_op is not None:
                raise WSError(1002, "expected continuation")
            first_op = op
        total += len(payload)
        if total > max_size:
            raise WSError(1009, "message too big")
        parts.append(payload)
        if fin:
            return first_op, b"".join(parts)


def ws_accept(key: str) -> str:
    return base64.b64encode(hashlib.sha1((key + WS_GUID).encode("ascii")).digest()).decode("ascii")


# --------------------------------------------------------------------------------------------
# 連線
# --------------------------------------------------------------------------------------------

class Conn:
    """一條 WebSocket 連線。所有送出都經 queue，所以廣播唔會畀一個慢 client 拖住。"""

    def __init__(self, server: "ArenaServer", reader: asyncio.StreamReader, writer: asyncio.StreamWriter,
                 role: str, peer: str):
        self.server = server
        self.reader = reader
        self.writer = writer
        self.role = role
        self.peer = peer
        self.room: Optional["Room"] = None
        self.viewer: Optional["Viewer"] = None
        self.out: "asyncio.Queue[Optional[bytes]]" = asyncio.Queue()
        self.closing = False
        self.last_seen = server.loop.time()
        self.writer_task: Optional["asyncio.Future[None]"] = None

    def send_frame(self, frame: bytes) -> None:
        if self.closing:
            return
        if self.out.qsize() >= MAX_OUT_QUEUE[self.role]:
            log.warning("%s 收得太慢，踢走 (%s)", self.peer, self.role)
            self.abort()
            return
        self.out.put_nowait(frame)

    def send_json(self, obj: Any) -> None:
        self.send_frame(encode_frame(OP_TEXT, dumps(obj)))

    def close(self, code: int = 1000, reason: str = "") -> None:
        """送 close frame；對方 CLOSE_TIMEOUT 秒內唔回就自己收線。"""
        if self.closing:
            return
        self.closing = True
        payload = struct.pack("!H", code) + reason.encode("utf-8")[:120]
        self.out.put_nowait(encode_frame(OP_CLOSE, payload))
        self.out.put_nowait(None)
        self.server.loop.call_later(CLOSE_TIMEOUT, self.shutdown)

    def shutdown(self) -> None:
        """好好哋收線（送晒 buffer 先關）；10 秒都送唔晒就硬斷。"""
        self.closing = True
        try:
            self.writer.close()
        except Exception:  # noqa: BLE001 - transport 可能已經冇咗
            pass
        self.server.loop.call_later(10.0, self.abort)

    def abort(self) -> None:
        """即刻硬斷（慢 client / 冇反應）。"""
        self.closing = True
        try:
            self.writer.transport.abort()
        except Exception:  # noqa: BLE001
            pass

    async def writer_loop(self) -> None:
        try:
            while True:
                data = await self.out.get()
                if data is None or self.writer.is_closing():
                    break
                self.writer.write(data)
                await asyncio.wait_for(self.writer.drain(), SEND_TIMEOUT)
        except asyncio.CancelledError:
            raise
        except Exception as exc:  # noqa: BLE001
            log.debug("送唔到畀 %s: %r", self.peer, exc)
            self.abort()

    async def ping_loop(self, interval: float) -> None:
        loop = self.server.loop
        while not self.closing:
            await asyncio.sleep(interval)
            if loop.time() - self.last_seen > interval * 3:
                log.info("%s 冇反應，斷線 (%s)", self.peer, self.role)
                self.abort()
                return
            self.send_frame(encode_frame(OP_PING, b"arena"))


# --------------------------------------------------------------------------------------------
# 房間
# --------------------------------------------------------------------------------------------

class Viewer:
    __slots__ = ("id", "name", "slot", "label", "conn", "last_clap", "release_handle")

    def __init__(self, vid: str, name: str, slot: int, label: str):
        self.id = vid
        self.name = name
        self.slot = slot
        self.label = label
        self.conn: Optional[Conn] = None
        self.last_clap = -1e9
        self.release_handle: Optional[asyncio.TimerHandle] = None

    def public(self) -> Dict[str, Any]:
        return {"id": self.id, "name": self.name, "slot": self.slot, "label": self.label}


class Room:
    def __init__(self, name: str):
        self.name = name
        self.host_key: Optional[str] = None
        self.host: Optional[Conn] = None
        self.viewers: Dict[str, Viewer] = {}
        self.conns: Set[Conn] = set()          # 所有觀眾連線（包括未 hello 嘅）
        self.free_slots: List[int] = []        # heap：釋放咗嘅 slot
        self.next_slot = 0
        self.rows: List[int] = list(DEFAULT_ROWS)
        self.layout_set = False                # host 送咗 layout 未：送咗之後具名觀眾唔可以多過場館座位
        self.mode = "watch"
        self.guest_id = ""
        self.streamer = ""
        self.stream_url = ""
        self.call_url = ""
        self.video_id = ""
        self.youtube = -1
        self.state_handle: Optional[asyncio.TimerHandle] = None
        self.last_state = -1e9
        self.yt_task: Optional["asyncio.Task[None]"] = None

    # ---- 座位 ----
    def alloc_slot(self) -> int:
        """最細嘅空 slot：heap 入面嘅全部細過 next_slot，所以 heap 頂（如有）就係答案。"""
        if self.free_slots:
            return heapq.heappop(self.free_slots)
        slot = self.next_slot
        self.next_slot += 1
        return slot

    def free_slot(self, slot: int) -> None:
        heapq.heappush(self.free_slots, slot)

    # ---- 訊息 ----
    def ticket(self, v: Viewer) -> Dict[str, Any]:
        return {"t": "ticket", "id": v.id, "name": v.name, "slot": v.slot, "label": v.label,
                "room": self.name, "streamer": self.streamer, "streamUrl": self.stream_url,
                "rows": self.rows}

    def guest_info(self) -> Optional[Dict[str, Any]]:
        v = self.viewers.get(self.guest_id) if self.guest_id else None
        return {"id": v.id, "name": v.name, "label": v.label} if v else None

    def state_msg(self) -> Dict[str, Any]:
        return {"t": "state", "mode": self.mode, "hostOnline": self.host is not None,
                "named": len(self.viewers), "youtube": self.youtube, "guest": self.guest_info()}

    def snapshot(self) -> Dict[str, Any]:
        viewers = sorted(self.viewers.values(), key=lambda v: v.slot)
        return {"t": "snapshot", "viewers": [v.public() for v in viewers], "youtube": self.youtube}

    def broadcast(self, obj: Any, exclude: Optional[Conn] = None) -> None:
        frame = encode_frame(OP_TEXT, dumps(obj))
        for c in list(self.conns):
            if c is not exclude:
                c.send_frame(frame)

    def to_host(self, obj: Any) -> None:
        if self.host is not None:
            self.host.send_json(obj)

    def is_idle(self) -> bool:
        return self.host is None and not self.viewers and not self.conns


# --------------------------------------------------------------------------------------------
# YouTube
# --------------------------------------------------------------------------------------------

def fetch_concurrent_viewers(endpoint: str, video_id: str, api_key: str, timeout: float = 10.0) -> int:
    """（喺 thread 行）攞 liveStreamingDetails.concurrentViewers。失敗 raise，訊息唔會包含 key。"""
    query = urllib.parse.urlencode({"part": "liveStreamingDetails", "id": video_id, "key": api_key})
    req = urllib.request.Request(endpoint + "?" + query, headers={"User-Agent": "LiveArena/1.0"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            data = json.loads(resp.read(1_000_000).decode("utf-8"))
    except urllib.error.HTTPError as exc:
        detail = ""
        try:
            body = json.loads(exc.read(20_000).decode("utf-8", "replace"))
            detail = " " + str(body.get("error", {}).get("message", ""))[:160]
        except Exception:  # noqa: BLE001
            pass
        raise RuntimeError("HTTP %d%s" % (exc.code, detail)) from None
    except urllib.error.URLError as exc:
        raise RuntimeError("網絡錯誤: %s" % (exc.reason,)) from None
    items = data.get("items") if isinstance(data, dict) else None
    if not items:
        raise RuntimeError("搵唔到片 %s" % video_id)
    details = items[0].get("liveStreamingDetails") or {}
    cv = details.get("concurrentViewers")
    if cv is None:
        raise RuntimeError("片 %s 冇 concurrentViewers（未開播或者已經完咗）" % video_id)
    return max(0, int(cv))


# --------------------------------------------------------------------------------------------
# Server
# --------------------------------------------------------------------------------------------

INDEX_HTML = """<!doctype html>
<html lang="zh-HK"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="theme-color" content="#050506"><title>Live Arena</title>
<style>
:root{--bg:#050506;--wall:#14151A;--copper:#B08D57;--copper-dk:#6E5636;--ivory:#F3EBDD;--grey:#9A928A;--seat:#4A1C1F;--seat-hi:#6A2A2E}
*{box-sizing:border-box}
body{margin:0;min-height:100vh;display:grid;place-items:center;background:var(--bg);color:var(--ivory);
font-family:"Noto Sans HK","PingFang HK","Microsoft JhengHei",system-ui,sans-serif;
background-image:radial-gradient(ellipse at 50% -20%,rgba(255,184,107,.12),transparent 60%)}
form{width:min(360px,calc(100% - 32px));padding:32px 24px;background:var(--wall);border:1px solid rgba(176,141,87,.35)}
h1{margin:0 0 4px;font:600 30px/1.1 "Cormorant Garamond",Georgia,serif;letter-spacing:.24em;text-align:center}
p{margin:0 0 24px;color:var(--grey);font-size:13px;text-align:center}
label{display:block;font-size:12px;color:var(--grey);letter-spacing:.1em;margin-bottom:6px}
input{width:100%;padding:10px 2px;background:transparent;border:0;border-bottom:1px solid var(--copper-dk);
color:var(--ivory);font:500 18px "IBM Plex Mono",ui-monospace,monospace;outline:none}
input:focus{border-color:var(--copper)}
button{margin-top:24px;width:100%;height:50px;background:linear-gradient(180deg,var(--seat-hi),var(--seat));
border:1px solid var(--copper);color:var(--ivory);font-size:16px;letter-spacing:.4em;cursor:pointer}
.err{color:#d9a07a;font-size:12px;min-height:16px;margin-top:8px}
</style></head><body>
<form id="f"><h1>LIVE ARENA</h1><p>輸入直播主畀你嘅房間名</p>
<label for="r">房間名</label><input id="r" autocomplete="off" autocapitalize="off" spellcheck="false" maxlength="32">
<div class="err" id="e"></div><button>前往</button></form>
<script>
document.getElementById('f').addEventListener('submit',function(ev){ev.preventDefault();
var v=document.getElementById('r').value.trim();
if(!/^[A-Za-z0-9_-]{1,32}$/.test(v)){document.getElementById('e').textContent='房間名只可以用英文字母、數字、_ 同 -';return;}
location.href='/r/'+v;});
</script></body></html>
"""

BOOT_TAG = '<script id="arena-boot" type="application/json">null</script>'

SECURITY_HEADERS = [
    ("X-Content-Type-Options", "nosniff"),
    # YouTube embed 要有 referrer，唔好用 no-referrer
    ("Referrer-Policy", "strict-origin-when-cross-origin"),
    ("Content-Security-Policy",
     "default-src 'self'; script-src 'self' 'unsafe-inline'; "
     "style-src 'self' 'unsafe-inline' https://fonts.googleapis.com; font-src 'self' https://fonts.gstatic.com; "
     "img-src 'self' data:; connect-src 'self' ws: wss:; "
     "frame-src https://www.youtube-nocookie.com https://www.youtube.com https://player.twitch.tv https://player.kick.com; "
     "base-uri 'none'; form-action 'self'; frame-ancestors 'self'"),
]


class ArenaServer:
    def __init__(self, grace: float, ping_interval: float, max_viewers: int,
                 yt_key: str, yt_endpoint: str, yt_interval: float):
        self.grace = max(0.0, grace)
        self.ping_interval = max(0.2, ping_interval)
        self.max_viewers = max(1, max_viewers)
        self.yt_key = yt_key
        self.yt_endpoint = yt_endpoint
        self.yt_interval = max(1.0, yt_interval)
        self.rooms: Dict[str, Room] = {}
        self.loop = asyncio.get_running_loop()
        self.page_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "audience.html")

    # ------------------------------------------------------------------ rooms
    def get_room(self, name: str) -> Optional[Room]:
        room = self.rooms.get(name)
        if room is None:
            if len(self.rooms) >= MAX_ROOMS:
                return None
            room = Room(name)
            self.rooms[name] = room
        return room

    def maybe_gc(self, room: Room) -> None:
        """未有 host 認領、又冇人嘅房先刪；認領咗嘅房保留（記住 key）。"""
        if room.host_key is None and room.is_idle() and self.rooms.get(room.name) is room:
            if room.state_handle is not None:
                room.state_handle.cancel()
            if room.yt_task is not None:
                room.yt_task.cancel()
            del self.rooms[room.name]

    def mark_state(self, room: Room) -> None:
        """state 最多每秒廣播一次；期間嘅改動合併成一條。"""
        if room.state_handle is not None:
            return
        delay = max(0.0, room.last_state + STATE_INTERVAL - self.loop.time())
        room.state_handle = self.loop.call_later(delay, self._flush_state, room)

    def _flush_state(self, room: Room) -> None:
        room.state_handle = None
        room.last_state = self.loop.time()
        try:
            room.broadcast(room.state_msg())
        except Exception:  # noqa: BLE001
            log.exception("廣播 state 出錯")

    # ------------------------------------------------------------------ host
    def attach_host(self, conn: Conn, room_name: str, key: str) -> bool:
        if not key or len(key) > 128:
            conn.send_json({"t": "error", "msg": "bad key"})
            conn.close(1008, "bad key")
            return False
        room = self.get_room(room_name)
        if room is None:
            conn.send_json({"t": "error", "msg": "server busy"})
            conn.close(1013, "server busy")
            return False
        if room.host_key is None:
            room.host_key = key
            log.info("[%s] host 認領咗房間", room.name)
        elif not hmac.compare_digest(room.host_key.encode("utf-8"), key.encode("utf-8")):
            log.warning("[%s] host key 錯 (%s)", room.name, conn.peer)
            conn.send_json({"t": "error", "msg": "bad key"})
            conn.close(1008, "bad key")
            return False
        old = room.host
        if old is not None and old is not conn:
            log.info("[%s] 新 host 頂走舊 host", room.name)
            old.send_json({"t": "error", "msg": "replaced"})
            old.close(4000, "replaced")
        conn.room = room
        room.host = conn
        conn.send_json(room.snapshot())
        self.mark_state(room)
        log.info("[%s] host 上線 (%s)，已入場 %d 人", room.name, conn.peer, len(room.viewers))
        return True

    def on_host_message(self, conn: Conn, msg: Dict[str, Any]) -> None:
        room = conn.room
        if room is None or room.host is not conn:
            return
        t = msg.get("t")
        if t == "layout":
            self._host_layout(conn, room, msg)
        elif t == "config":
            self._host_config(room, msg)
        elif t == "mode":
            mode = msg.get("mode")
            if mode not in ("watch", "interactive"):
                conn.send_json({"t": "error", "msg": "bad mode"})
                return
            room.mode = mode
            room.broadcast({"t": "mode", "mode": mode})
            self.mark_state(room)
            log.info("[%s] 模式 -> %s", room.name, mode)
        elif t == "lottery":
            self._host_lottery(conn, room, msg)
        elif t == "guest":
            self._host_guest(conn, room, msg)
        else:
            conn.send_json({"t": "error", "msg": "unknown type"})

    def _host_layout(self, conn: Conn, room: Room, msg: Dict[str, Any]) -> None:
        rows = msg.get("rows")
        if isinstance(rows, list):
            # JSON 冇分 int / float：有啲 client 會將 34 寫成 34.0，整數值就照收
            rows = [int(n) if isinstance(n, float) and n.is_integer() else n for n in rows]
        ok = (isinstance(rows, list) and 1 <= len(rows) <= MAX_LAYOUT_ROWS
              and all(isinstance(n, int) and not isinstance(n, bool) and 1 <= n <= MAX_ROW_SEATS for n in rows))
        if not ok:
            conn.send_json({"t": "error", "msg": "bad layout"})
            return
        room.layout_set = True
        if rows == room.rows:
            return
        room.rows = list(rows)
        for v in room.viewers.values():
            v.label = slot_label(v.slot, room.rows)
            if v.conn is not None:
                v.conn.send_json(room.ticket(v))
        if room.guest_id:
            self.mark_state(room)
        log.info("[%s] layout：%d 行，共 %d 個位", room.name, len(rows), sum(rows))

    def _host_config(self, room: Room, msg: Dict[str, Any]) -> None:
        # 冇出現嘅欄位保持原值；出現咗（包括空字串）就照設
        streamer = clean_text(msg["streamer"], 40) if "streamer" in msg else room.streamer
        stream_url = clean_url(msg["streamUrl"]) if "streamUrl" in msg else room.stream_url
        call_url = clean_url(msg["callUrl"]) if "callUrl" in msg else room.call_url
        if "videoId" in msg:
            raw = msg.get("videoId") if isinstance(msg.get("videoId"), str) else ""
            raw = raw.strip()
            video_id = raw if VIDEO_ID_RE.fullmatch(raw) else youtube_id_from(raw)
        else:
            video_id = room.video_id
        if not video_id:
            video_id = youtube_id_from(stream_url)   # 冇填 videoId 就試下由 YouTube 直播網址拆

        ticket_changed = (streamer, stream_url) != (room.streamer, room.stream_url)
        call_changed = call_url != room.call_url
        room.streamer, room.stream_url, room.call_url = streamer, stream_url, call_url

        if ticket_changed:
            for v in room.viewers.values():
                if v.conn is not None:
                    v.conn.send_json(room.ticket(v))
        if call_changed and room.guest_id:
            g = room.viewers.get(room.guest_id)
            if g is not None and g.conn is not None:
                g.conn.send_json(self._guest_msg(room, g, private=True))
        if video_id != room.video_id or (video_id and room.yt_task is None and self.yt_key):
            room.video_id = video_id
            self._restart_youtube(room)
        log.info("[%s] config：直播主=%s 直播=%s videoId=%s 通話=%s", room.name, streamer or "-",
                 stream_url or "-", video_id or "-", "有" if call_url else "冇")

    def _host_lottery(self, conn: Conn, room: Room, msg: Dict[str, Any]) -> None:
        phase = msg.get("phase")
        if phase == "spin":
            room.broadcast({"t": "lottery", "phase": "spin"})
            log.info("[%s] 抽獎開始", room.name)
        elif phase == "winner":
            v = room.viewers.get(msg.get("id")) if isinstance(msg.get("id"), str) else None
            if v is None:
                conn.send_json({"t": "error", "msg": "unknown viewer"})
                return
            room.broadcast({"t": "lottery", "phase": "winner", "id": v.id, "name": v.name, "label": v.label})
            log.info("[%s] 抽中 %s (%s)", room.name, v.name, v.label)
        else:
            conn.send_json({"t": "error", "msg": "bad phase"})

    @staticmethod
    def _guest_msg(room: Room, v: Viewer, private: bool) -> Dict[str, Any]:
        m = {"t": "guest", "id": v.id, "name": v.name, "label": v.label}
        if private:
            m["callUrl"] = room.call_url
        return m

    def _host_guest(self, conn: Conn, room: Room, msg: Dict[str, Any]) -> None:
        gid = msg.get("id")
        if gid in ("", None):
            room.guest_id = ""
            room.broadcast({"t": "guest", "id": ""})
            self.mark_state(room)
            log.info("[%s] 嘉賓落台", room.name)
            return
        v = room.viewers.get(gid) if isinstance(gid, str) else None
        if v is None:
            conn.send_json({"t": "error", "msg": "unknown viewer"})
            return
        room.guest_id = v.id
        # 公開版本（冇 callUrl）畀其他人；有 callUrl 嘅只畀嘉賓本人
        room.broadcast(self._guest_msg(room, v, private=False), exclude=v.conn)
        if v.conn is not None:
            v.conn.send_json(self._guest_msg(room, v, private=True))
        self.mark_state(room)
        log.info("[%s] 請 %s (%s) 上台", room.name, v.name, v.label)

    # ------------------------------------------------------------------ viewers
    def attach_viewer(self, conn: Conn, room_name: str) -> bool:
        room = self.get_room(room_name)
        if room is None:
            conn.send_json({"t": "error", "msg": "server busy"})
            conn.close(1013, "server busy")
            return False
        if len(room.conns) >= self.max_viewers + EXTRA_PENDING_CONNS:
            conn.send_json({"t": "error", "msg": "full"})
            conn.close(1013, "full")
            self.maybe_gc(room)
            return False
        conn.room = room
        room.conns.add(conn)
        conn.send_json(room.state_msg())   # 未入場都睇到人數
        return True

    def on_viewer_message(self, conn: Conn, msg: Dict[str, Any]) -> None:
        room = conn.room
        if room is None:
            return
        t = msg.get("t")
        if t == "hello":
            if conn.viewer is None:
                self._viewer_hello(conn, room, msg)
        elif t == "clap":
            v = conn.viewer
            if v is None or v.conn is not conn:
                return
            now = self.loop.time()
            if now - v.last_clap < CLAP_INTERVAL:
                return
            v.last_clap = now
            room.to_host({"t": "clap", "id": v.id})

    def _viewer_hello(self, conn: Conn, room: Room, msg: Dict[str, Any]) -> None:
        old_id = msg.get("id")
        v = room.viewers.get(old_id) if isinstance(old_id, str) and ID_RE.fullmatch(old_id) else None
        if v is not None:
            # 重連：坐返原位，唔再發 join
            if v.release_handle is not None:
                v.release_handle.cancel()
                v.release_handle = None
            if v.conn is not None and v.conn is not conn:
                prev = v.conn
                prev.viewer = None
                prev.send_json({"t": "error", "msg": "replaced"})
                prev.close(4000, "replaced")
            v.conn = conn
            conn.viewer = v
            conn.send_json(room.ticket(v))
            conn.send_json(room.state_msg())
            if room.guest_id == v.id:
                conn.send_json(self._guest_msg(room, v, private=True))
            log.info("[%s] %s (%s) 返嚟", room.name, v.name, v.label)
            return

        # 上限 = --max-viewers；host 送咗 layout 之後再唔可以多過場館總座位（超出嘅 slot UE 冇位畫、抽唔中）。
        # 新人永遠攞最細嘅空 slot，所以人數 < 座位數嘅時候派出去嘅 slot 一定喺場館入面。
        cap = min(self.max_viewers, sum(room.rows)) if room.layout_set else self.max_viewers
        if len(room.viewers) >= cap:
            conn.send_json({"t": "error", "msg": "full"})
            conn.close(1013, "full")
            return
        vid = secrets.token_urlsafe(12)
        while vid in room.viewers:
            vid = secrets.token_urlsafe(12)
        slot = room.alloc_slot()
        v = Viewer(vid, sanitize_name(msg.get("name")), slot, slot_label(slot, room.rows))
        v.conn = conn
        conn.viewer = v
        room.viewers[vid] = v
        conn.send_json(room.ticket(v))
        conn.send_json(room.state_msg())
        room.to_host({"t": "join", "viewer": v.public()})
        self.mark_state(room)
        log.info("[%s] %s 入場 -> %s（共 %d 人）", room.name, v.name, v.label, len(room.viewers))

    def release_viewer(self, room: Room, vid: str) -> None:
        v = room.viewers.get(vid)
        if v is None or v.conn is not None:
            return
        v.release_handle = None
        del room.viewers[vid]
        room.free_slot(v.slot)
        room.to_host({"t": "leave", "id": vid})
        if room.guest_id == vid:
            room.guest_id = ""
            room.broadcast({"t": "guest", "id": ""})
        self.mark_state(room)
        log.info("[%s] %s (%s) 座位釋放", room.name, v.name, v.label)
        self.maybe_gc(room)

    # ------------------------------------------------------------------ detach
    def detach(self, conn: Conn) -> None:
        room = conn.room
        if room is None:
            return
        if conn.role == "host":
            if room.host is conn:
                room.host = None
                self.mark_state(room)
                log.info("[%s] host 離線（房間同座位保留）", room.name)
        else:
            room.conns.discard(conn)
            v = conn.viewer
            if v is not None and v.conn is conn:
                v.conn = None
                v.release_handle = self.loop.call_later(self.grace, self._safe_release, room, v.id)
        self.maybe_gc(room)

    def _safe_release(self, room: Room, vid: str) -> None:
        try:
            self.release_viewer(room, vid)
        except Exception:  # noqa: BLE001
            log.exception("釋放座位出錯")

    # ------------------------------------------------------------------ YouTube
    def _restart_youtube(self, room: Room) -> None:
        if room.yt_task is not None:
            room.yt_task.cancel()
            room.yt_task = None
        if not room.video_id:
            if room.youtube != -1:
                room.youtube = -1
                room.to_host({"t": "count", "youtube": -1})
                self.mark_state(room)
            return
        if not self.yt_key:
            return
        room.yt_task = asyncio.ensure_future(self._youtube_loop(room, room.video_id))

    async def _youtube_loop(self, room: Room, video_id: str) -> None:
        while True:
            if room.host is not None or room.conns:     # 冇人睇就慳 quota
                try:
                    n = await asyncio.to_thread(fetch_concurrent_viewers, self.yt_endpoint, video_id, self.yt_key)
                    if room.video_id != video_id:
                        return
                    room.youtube = n
                    room.to_host({"t": "count", "youtube": n})
                    self.mark_state(room)
                except asyncio.CancelledError:
                    raise
                except Exception as exc:  # noqa: BLE001 - 失敗保持舊值
                    log.warning("[%s] YouTube 人數攞唔到：%s", room.name, exc)
            await asyncio.sleep(self.yt_interval)

    # ------------------------------------------------------------------ HTTP / WS 入口
    async def handle_client(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        peer_info = writer.get_extra_info("peername")
        peer = "%s:%s" % (peer_info[0], peer_info[1]) if isinstance(peer_info, tuple) else str(peer_info)
        try:
            try:
                head = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), 15)
            except (asyncio.IncompleteReadError, asyncio.LimitOverrunError, asyncio.TimeoutError,
                    ConnectionError, OSError):
                return
            req = parse_request(head)
            if req is None:
                await http_respond(writer, 400, b"bad request")
                return
            method, target, headers = req
            try:
                url = urllib.parse.urlsplit(target)
            except ValueError:
                await http_respond(writer, 400, b"bad request")
                return
            path = url.path
            head_only = method == "HEAD"
            if method not in ("GET", "HEAD"):
                await http_respond(writer, 405, b"method not allowed", extra=[("Allow", "GET, HEAD")])
            elif path == "/healthz":
                await http_respond(writer, 200, b"ok", head_only=head_only)
            elif path == "/":
                await http_respond(writer, 200, INDEX_HTML.encode("utf-8"), "text/html; charset=utf-8",
                                   extra=SECURITY_HEADERS, head_only=head_only)
            elif path.startswith("/r/"):
                await self.serve_audience(writer, path[3:].rstrip("/"), head_only)
            elif path == "/ws":
                await self.serve_ws(reader, writer, url.query, headers, peer)
            elif path == "/favicon.ico":
                await http_respond(writer, 204, b"")
            else:
                await http_respond(writer, 404, b"not found", head_only=head_only)
        except Exception:  # noqa: BLE001 - 任何錯都唔可以令 server 死
            log.exception("處理 %s 出錯", peer)
        finally:
            try:
                writer.close()
            except Exception:  # noqa: BLE001
                pass

    async def serve_audience(self, writer: asyncio.StreamWriter, room_name: str, head_only: bool) -> None:
        if not ROOM_RE.fullmatch(room_name):
            await http_respond(writer, 400, "房間名只可以用 A-Z a-z 0-9 _ -（最多 32 字）".encode("utf-8"))
            return
        try:
            with open(self.page_path, "r", encoding="utf-8") as f:
                page = f.read()
        except OSError as exc:
            log.error("讀唔到 audience.html：%s", exc)
            await http_respond(writer, 500, b"audience.html missing")
            return
        room = self.rooms.get(room_name)
        boot = {"room": room_name, "streamer": room.streamer if room else ""}
        boot_json = (json.dumps(boot, ensure_ascii=False).replace("<", "\\u003c").replace(">", "\\u003e")
                     .replace("&", "\\u0026").replace("\u2028", "\\u2028").replace("\u2029", "\\u2029"))
        page = page.replace(BOOT_TAG, BOOT_TAG.replace(">null<", ">" + boot_json + "<"), 1)
        await http_respond(writer, 200, page.encode("utf-8"), "text/html; charset=utf-8",
                           extra=SECURITY_HEADERS, head_only=head_only)

    async def serve_ws(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter, query: str,
                       headers: Dict[str, str], peer: str) -> None:
        conn_tokens = [t.strip().lower() for t in headers.get("connection", "").split(",")]
        if headers.get("upgrade", "").lower() != "websocket" or "upgrade" not in conn_tokens:
            await http_respond(writer, 400, b"expected websocket upgrade")
            return
        if headers.get("sec-websocket-version", "") != "13":
            await http_respond(writer, 426, b"websocket version 13 only", extra=[("Sec-WebSocket-Version", "13")])
            return
        ws_key = headers.get("sec-websocket-key", "")
        try:
            if len(base64.b64decode(ws_key, validate=True)) != 16:
                raise ValueError
        except ValueError:
            await http_respond(writer, 400, b"bad Sec-WebSocket-Key")
            return
        qs = urllib.parse.parse_qs(query, keep_blank_values=True)
        role = (qs.get("role") or [""])[0]
        room_name = (qs.get("room") or [""])[0]
        key = (qs.get("key") or [""])[0]
        if role not in ("host", "viewer") or not ROOM_RE.fullmatch(room_name):
            await http_respond(writer, 400, b"need role=host|viewer and room=[A-Za-z0-9_-]{1,32}")
            return

        writer.write(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                      "Sec-WebSocket-Accept: %s\r\n\r\n" % ws_accept(ws_key)).encode("ascii"))
        await writer.drain()

        conn = Conn(self, reader, writer, role, peer)
        conn.writer_task = asyncio.ensure_future(conn.writer_loop())
        ping_task = asyncio.ensure_future(conn.ping_loop(self.ping_interval))
        try:
            ok = self.attach_host(conn, room_name, key) if role == "host" else self.attach_viewer(conn, room_name)
            if ok:
                await self.read_loop(conn)
        finally:
            ping_task.cancel()
            try:
                self.detach(conn)
            except Exception:  # noqa: BLE001
                log.exception("detach 出錯")
            conn.close(1001, "bye")
            try:
                await asyncio.wait_for(conn.writer_task, CLOSE_TIMEOUT)
            except (asyncio.TimeoutError, asyncio.CancelledError, Exception):  # noqa: BLE001
                pass
            conn.shutdown()

    async def read_loop(self, conn: Conn) -> None:
        loop = self.loop

        def on_control(op: int, payload: bytes) -> None:
            conn.last_seen = loop.time()
            if op == OP_PING:
                conn.send_frame(encode_frame(OP_PONG, payload))

        while True:
            try:
                op, data = await read_message(conn.reader, require_mask=True, on_control=on_control)
            except WSError as exc:
                log.info("%s 協議錯誤：%s", conn.peer, exc)
                conn.close(exc.code, exc.reason)
                return
            except (asyncio.IncompleteReadError, ConnectionError, OSError):
                return
            conn.last_seen = loop.time()
            if conn.closing and op != OP_CLOSE:
                continue   # 已經叫佢收線（例如被頂走）：唔再處理新訊息
            if op == OP_CLOSE:
                conn.close(1000, "")
                return
            if op == OP_BINARY:
                conn.close(1003, "text only")
                return
            try:
                text = data.decode("utf-8")
            except UnicodeDecodeError:
                conn.close(1007, "invalid utf-8")
                return
            try:
                msg = json.loads(text)
            except Exception:  # noqa: BLE001 - ValueError / RecursionError
                msg = None
            if not isinstance(msg, dict) or not isinstance(msg.get("t"), str):
                if conn.role == "host":
                    conn.send_json({"t": "error", "msg": "bad message"})
                continue
            try:
                if conn.role == "host":
                    self.on_host_message(conn, msg)
                else:
                    self.on_viewer_message(conn, msg)
            except Exception:  # noqa: BLE001
                log.exception("處理訊息出錯 (%s)", conn.role)
            if conn.closing:
                return


def parse_request(head: bytes) -> Optional[Tuple[str, str, Dict[str, str]]]:
    try:
        lines = head.decode("latin-1").split("\r\n")
        method, target, version = lines[0].split(" ")
    except ValueError:
        return None
    if not version.startswith("HTTP/1.") or not target.startswith("/"):
        return None
    headers: Dict[str, str] = {}
    for line in lines[1:]:
        if not line:
            continue
        k, sep, v = line.partition(":")
        if not sep:
            return None
        k = k.strip().lower()
        headers[k] = (headers[k] + ", " + v.strip()) if k in headers else v.strip()
    return method, target, headers


async def http_respond(writer: asyncio.StreamWriter, status: int, body: bytes,
                       ctype: str = "text/plain; charset=utf-8",
                       extra: Optional[List[Tuple[str, str]]] = None, head_only: bool = False) -> None:
    try:
        reason = HTTPStatus(status).phrase
    except ValueError:
        reason = ""
    lines = ["HTTP/1.1 %d %s" % (status, reason), "Content-Type: " + ctype,
             "Content-Length: %d" % len(body), "Cache-Control: no-cache", "Connection: close"]
    for k, v in extra or []:
        lines.append("%s: %s" % (k, v))
    writer.write(("\r\n".join(lines) + "\r\n\r\n").encode("latin-1") + (b"" if head_only else body))
    try:
        await asyncio.wait_for(writer.drain(), 10)
    except Exception:  # noqa: BLE001
        pass


# --------------------------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------------------------

def lan_ip() -> str:
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("10.255.255.255", 1))   # 唔會真係送嘢，只係攞本機對外 IP
            return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"


def parse_args(argv: Optional[List[str]] = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Live Arena server（觀眾網頁 + WebSocket + YouTube 人數）")
    p.add_argument("--host", default="0.0.0.0", help="綁定位址（預設 0.0.0.0）")
    p.add_argument("--port", type=int, default=8787, help="port（預設 8787）")
    p.add_argument("--grace", type=float, default=120.0, help="觀眾斷線後保留座位幾多秒（預設 120）")
    p.add_argument("--ping-interval", type=float, default=20.0, help="server ping 間隔秒數（預設 20）")
    p.add_argument("--max-viewers", type=int, default=5000, help="每房具名觀眾上限（預設 5000）")
    p.add_argument("--verbose", action="store_true", help="印 debug log")
    # 測試用：
    p.add_argument("--yt-interval", type=float, default=30.0, help=argparse.SUPPRESS)
    p.add_argument("--yt-endpoint", default=YT_ENDPOINT, help=argparse.SUPPRESS)
    return p.parse_args(argv)


def print_banner(args: argparse.Namespace, port: int, yt_key: str) -> None:
    shown = lan_ip() if args.host in ("0.0.0.0", "::", "") else args.host
    base = "http://%s:%d" % (shown, port)
    lines = [
        "",
        "  LIVE ARENA server 已經開咗  (Ctrl+C 停)",
        "  ------------------------------------------------------------",
        "  觀眾入場 link   %s/r/<房間名>      例如 %s/r/amy" % (base, base),
        "  UE host 連線    ws://%s:%d/ws" % (shown, port),
        "                  （UE Project Settings > Live Arena > ServerUrl；RoomId / HostKey 自己揀）",
        "  健康檢查        %s/healthz" % base,
        "  YouTube 人數    %s" % ("開（YOUTUBE_API_KEY 已設定，每 %d 秒）" % args.yt_interval if yt_key
                                 else "關（冇 YOUTUBE_API_KEY，人數 = -1）"),
        "  座位保留        %g 秒" % args.grace,
        "  ------------------------------------------------------------",
        "  觀眾唔喺同一個網絡？用 cloudflared / ngrok 開條 https tunnel 去 port %d，" % port,
        "  入場 link 就變成 https://<tunnel 網址>/r/<房間名>，UE 用 wss://<tunnel 網址>/ws。",
        "",
    ]
    print("\n".join(lines), flush=True)


async def amain(args: argparse.Namespace) -> None:
    loop = asyncio.get_running_loop()

    def on_loop_error(_loop: asyncio.AbstractEventLoop, ctx: Dict[str, Any]) -> None:
        exc = ctx.get("exception")
        if isinstance(exc, (ConnectionError, asyncio.CancelledError)):
            return
        log.error("event loop: %s %r", ctx.get("message"), exc)

    loop.set_exception_handler(on_loop_error)
    yt_key = os.environ.get("YOUTUBE_API_KEY", "").strip()
    server = ArenaServer(args.grace, args.ping_interval, args.max_viewers, yt_key,
                         args.yt_endpoint, args.yt_interval)
    try:
        srv = await asyncio.start_server(server.handle_client, args.host, args.port)
    except OSError as exc:
        print("開唔到 %s:%d：%s（port 俾人用緊？試下 --port 8788）" % (args.host, args.port, exc),
              file=sys.stderr, flush=True)
        raise SystemExit(1)
    port = srv.sockets[0].getsockname()[1] if srv.sockets else args.port
    print_banner(args, port, yt_key)
    log.info("listening on %s:%d", args.host, port)
    async with srv:
        await srv.serve_forever()


def main(argv: Optional[List[str]] = None) -> None:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")   # Windows console 唔會因中文死
        except (AttributeError, ValueError):
            pass
    args = parse_args(argv)
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)-5s %(message)s", datefmt="%H:%M:%S")
    try:
        asyncio.run(amain(args))
    except KeyboardInterrupt:
        print("\nbye", flush=True)


if __name__ == "__main__":
    main()
