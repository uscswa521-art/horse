#!/usr/bin/env python3
"""Live Arena server 自我測試（純標準庫）。

    python3 selftest.py

會喺 subprocess 起幾個臨時 server（隨機 port、--grace 1），用 server.py 入面嘅 frame 函數做 WebSocket client，
逐項對 PROTOCOL.md。全部 PASS 先 exit 0。
"""

import asyncio
import base64
import hashlib
import http.client
import http.server
import json
import os
import socket
import struct
import subprocess
import sys
import threading
import time
import urllib.parse
from typing import Any, Dict, List, Optional

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import server as srv  # noqa: E402
from server import (OP_BINARY, OP_CLOSE, OP_CONT, OP_PING, OP_PONG, OP_TEXT,  # noqa: E402
                    WSError, encode_frame, read_frame, read_message)

RESULTS: List[bool] = []


def check(name: str, cond: Any, detail: Any = "") -> bool:
    ok = bool(cond)
    RESULTS.append(ok)
    print(("PASS  " if ok else "FAIL  ") + name + ("" if ok else "   -> %r" % (detail,)), flush=True)
    return ok


# --------------------------------------------------------------------------------------------
# 獨立實作嘅座位公式（唔用 server 嘅，用嚟交叉核對）
# --------------------------------------------------------------------------------------------

def ref_label(slot: int, rows: List[int]) -> str:
    r = 0
    for n in rows + [rows[-1]] * 3000:
        if slot < n:
            mid = n // 2
            pos = mid + slot // 2 if slot % 2 == 0 else mid - (slot + 1) // 2
            name = chr(65 + r) if r < 26 else chr(65 + r // 26 - 1) + chr(65 + r % 26)
            return "%s%d" % (name, pos + 1)
        slot -= n
        r += 1
    raise ValueError("slot too big")


# --------------------------------------------------------------------------------------------
# 臨時 server
# --------------------------------------------------------------------------------------------

def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class ServerProc:
    def __init__(self, *extra: str, env: Optional[Dict[str, str]] = None):
        self.port = free_port()
        cmd = [sys.executable, os.path.join(HERE, "server.py"), "--host", "127.0.0.1",
               "--port", str(self.port), "--grace", "1"] + list(extra)
        e = dict(os.environ)
        e.pop("YOUTUBE_API_KEY", None)
        e.update({"PYTHONUNBUFFERED": "1", "NO_PROXY": "127.0.0.1,localhost", "no_proxy": "127.0.0.1,localhost"})
        if env:
            e.update(env)
        self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=e)
        self.lines: List[str] = []
        threading.Thread(target=self._pump, daemon=True).start()
        deadline = time.time() + 10
        while time.time() < deadline:
            try:
                if http_get(self.port, "/healthz")[0] == 200:
                    return
            except OSError:
                time.sleep(0.05)
        raise RuntimeError("server 起唔到：\n" + "".join(self.lines))

    def _pump(self) -> None:
        assert self.proc.stdout is not None
        for raw in self.proc.stdout:
            self.lines.append(raw.decode("utf-8", "replace"))

    def alive(self) -> bool:
        return self.proc.poll() is None

    def log(self) -> str:
        return "".join(self.lines)

    def stop(self) -> None:
        self.proc.terminate()
        try:
            self.proc.wait(5)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def http_get(port: int, path: str, method: str = "GET", headers: Optional[Dict[str, str]] = None):
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    try:
        c.request(method, path, headers=headers or {})
        r = c.getresponse()
        return r.status, {k.lower(): v for k, v in r.getheaders()}, r.read()
    finally:
        c.close()


# --------------------------------------------------------------------------------------------
# WebSocket client
# --------------------------------------------------------------------------------------------

class WS:
    def __init__(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        self.reader = reader
        self.writer = writer
        self.msgs: "asyncio.Queue[Dict[str, Any]]" = asyncio.Queue()
        self.closed = asyncio.Event()
        self.close_code: Optional[int] = None
        self.pings: List[bytes] = []
        self.pongs: List[bytes] = []
        self.task = asyncio.ensure_future(self._read())

    @classmethod
    async def connect(cls, port: int, query: str) -> "WS":
        reader, writer = await asyncio.open_connection("127.0.0.1", port)
        key = base64.b64encode(os.urandom(16)).decode()
        writer.write(("GET /ws?%s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                      "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % (query, port, key)).encode())
        head = (await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), 5)).decode("latin-1")
        if not head.startswith("HTTP/1.1 101"):
            writer.close()
            raise RuntimeError("upgrade failed: " + head.split("\r\n")[0])
        expected = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
        ws = cls(reader, writer)
        ws.accept_ok = ("sec-websocket-accept: " + expected).lower() in head.lower()
        return ws

    async def _read(self) -> None:
        try:
            while True:
                op, data = await read_message(self.reader, require_mask=False, max_size=1 << 20,
                                              on_control=self._control)
                if op == OP_CLOSE:
                    self.close_code = struct.unpack("!H", data[:2])[0] if len(data) >= 2 else 1005
                    try:
                        self.writer.write(encode_frame(OP_CLOSE, data[:2], mask=True))
                    except Exception:  # noqa: BLE001
                        pass
                    break
                self.msgs.put_nowait(json.loads(data.decode("utf-8")))
        except (asyncio.IncompleteReadError, ConnectionError, OSError, WSError):
            pass
        finally:
            self.closed.set()

    def _control(self, op: int, payload: bytes) -> None:
        if op == OP_PING:
            self.pings.append(payload)
            self.writer.write(encode_frame(OP_PONG, payload, mask=True))
        else:
            self.pongs.append(payload)

    def send(self, obj: Any) -> None:
        self.writer.write(encode_frame(OP_TEXT, json.dumps(obj, ensure_ascii=False).encode("utf-8"), mask=True))

    def raw(self, data: bytes) -> None:
        self.writer.write(data)

    def drain(self) -> List[Dict[str, Any]]:
        out = []
        while not self.msgs.empty():
            out.append(self.msgs.get_nowait())
        return out

    async def recv(self, t: Optional[str] = None, timeout: float = 3.0) -> Dict[str, Any]:
        """等下一條（類型 t 嘅）訊息；其他類型丟咗。等唔到 raise TimeoutError。"""
        loop = asyncio.get_running_loop()
        deadline = loop.time() + timeout
        while True:
            remaining = deadline - loop.time()
            if remaining <= 0:
                raise TimeoutError("冇收到 %s" % t)
            try:
                m = await asyncio.wait_for(self.msgs.get(), remaining)
            except asyncio.TimeoutError:
                raise TimeoutError("冇收到 %s" % t) from None
            if t is None or m.get("t") == t:
                return m

    async def collect(self, t: str, wait: float) -> List[Dict[str, Any]]:
        """收 wait 秒，返回期間所有類型 t 嘅訊息。"""
        await asyncio.sleep(wait)
        return [m for m in self.drain() if m.get("t") == t]

    async def wait_closed(self, timeout: float = 3.0) -> bool:
        try:
            await asyncio.wait_for(self.closed.wait(), timeout)
            return True
        except asyncio.TimeoutError:
            return False

    async def close(self) -> None:
        try:
            self.writer.write(encode_frame(OP_CLOSE, struct.pack("!H", 1000), mask=True))
        except Exception:  # noqa: BLE001
            pass
        await self.wait_closed(2)
        self.writer.close()


async def viewer(port: int, name: str, vid: Optional[str] = None, room: str = "test") -> "tuple":
    ws = await WS.connect(port, "role=viewer&room=" + room)
    first = await ws.recv()
    ws.send({"t": "hello", "id": vid, "name": name})
    ticket = await ws.recv("ticket")
    return ws, ticket, first


async def safe(name: str, coro) -> Any:
    try:
        return await coro
    except Exception as exc:  # noqa: BLE001
        check(name, False, exc)
        return None


# --------------------------------------------------------------------------------------------
# 測試
# --------------------------------------------------------------------------------------------

def test_units() -> None:
    check("公式：row_name 0/25/26/27/51/52 = A/Z/AA/AB/AZ/BA",
          [srv.row_name(r) for r in (0, 25, 26, 27, 51, 52)] == ["A", "Z", "AA", "AB", "AZ", "BA"])
    check("公式：n=4 由中間向外 = 2,1,3,0", [srv.center_out(j, 4) for j in range(4)] == [2, 1, 3, 0])
    check("公式：n=5 由中間向外 = 2,1,3,0,4", [srv.center_out(j, 5) for j in range(5)] == [2, 1, 3, 0, 4])
    rows = srv.DEFAULT_ROWS
    check("預設 layout = 28 行，34..61", rows == list(range(34, 62)), rows)
    check("頭 5 個 = A18 A17 A19 A16 A20", [srv.slot_label(s, rows) for s in range(5)] == ["A18", "A17", "A19", "A16", "A20"])
    check("slot 34 = B18（第二行 n=35）", srv.slot_label(34, rows) == "B18", srv.slot_label(34, rows))
    mism = []
    for layout in (rows, [5, 5, 5], [1], [7, 8, 9, 10], [3] * 40):
        for s in range(0, 2000, 7):
            if srv.slot_label(s, layout) != ref_label(s, layout):
                mism.append((layout[:3], s))
    check("slot_label 同獨立實作一致（多個 layout、包括超出容量）", not mism, mism[:5])
    check("名：控制字元 / 零闊 / RTL 刪走", srv.sanitize_name("  \x07阿\u200b明\u202e  ") == "阿明",
          srv.sanitize_name("  \x07阿\u200b明\u202e  "))
    check("名：最多 20 字", srv.sanitize_name("字" * 50) == "字" * 20)
    check("名：空白 / 非字串 -> 觀眾", srv.sanitize_name("   ") == "觀眾" and srv.sanitize_name(123) == "觀眾")
    check("URL：javascript: 唔收", srv.clean_url("javascript:alert(1)") == "" and srv.clean_url("https://a.b/x") == "https://a.b/x")
    yid = "dQw4w9WgXcQ"
    urls = ["https://www.youtube.com/watch?v=" + yid, "https://youtu.be/" + yid, "https://www.youtube.com/live/" + yid + "?si=x",
            "https://m.youtube.com/watch?v=" + yid + "&t=3", yid]
    check("YouTube id 由網址拆", all(srv.youtube_id_from(u) == yid for u in urls), [srv.youtube_id_from(u) for u in urls])


async def test_frames_unit() -> None:
    big = b"x" * 70000
    r = asyncio.StreamReader()
    r.feed_data(encode_frame(OP_TEXT, big, mask=True))
    fin, op, payload = await read_frame(r, max_size=1 << 20)
    check("frame：127 長度（70000 bytes）encode/decode", fin and op == OP_TEXT and payload == big)
    r = asyncio.StreamReader()
    r.feed_data(encode_frame(OP_TEXT, b"y" * 300, mask=True))
    fin, op, payload = await read_frame(r)
    check("frame：126 長度 encode/decode", payload == b"y" * 300)
    r = asyncio.StreamReader()
    r.feed_data(encode_frame(OP_TEXT, big, mask=True))
    try:
        await read_frame(r)
        check("frame：超過 64KB raise 1009", False)
    except WSError as exc:
        check("frame：超過 64KB raise 1009", exc.code == 1009, exc.code)
    r = asyncio.StreamReader()
    r.feed_data(encode_frame(OP_TEXT, b"hi", mask=False))
    try:
        await read_frame(r, require_mask=True)
        check("frame：client 冇 mask raise 1002", False)
    except WSError as exc:
        check("frame：client 冇 mask raise 1002", exc.code == 1002)


async def test_http(port: int) -> None:
    st, _, body = http_get(port, "/healthz")
    check("HTTP /healthz = ok", st == 200 and body == b"ok", (st, body))
    st, h, body = http_get(port, "/")
    check("HTTP / 首頁", st == 200 and b"LIVE ARENA" in body and "text/html" in h.get("content-type", ""), st)
    st, h, body = http_get(port, "/r/test")
    check("HTTP /r/test 觀眾頁 + boot room", st == 200 and b'id="arena-boot"' in body and b'"room": "test"' in body, st)
    check("HTTP /r/test 有 CSP", "content-security-policy" in h)
    check("HTTP /r/test/ 尾斜線都得", http_get(port, "/r/test/")[0] == 200)
    check("HTTP /r/bad!x = 400", http_get(port, "/r/bad!x")[0] == 400)
    check("HTTP /r/<33 字> = 400", http_get(port, "/r/" + "a" * 33)[0] == 400)
    check("HTTP /nope = 404", http_get(port, "/nope")[0] == 404)
    check("HTTP POST / = 405", http_get(port, "/", method="POST")[0] == 405)
    st, _, body = http_get(port, "/healthz", method="HEAD")
    check("HTTP HEAD 冇 body", st == 200 and body == b"")
    check("HTTP /ws 冇 upgrade = 400", http_get(port, "/ws?role=viewer&room=test")[0] == 400)
    up = {"Upgrade": "websocket", "Connection": "Upgrade", "Sec-WebSocket-Key": base64.b64encode(os.urandom(16)).decode()}
    st, h, _ = http_get(port, "/ws?role=viewer&room=test", headers=dict(up, **{"Sec-WebSocket-Version": "8"}))
    check("WS 版本唔係 13 = 426", st == 426 and h.get("sec-websocket-version") == "13", st)
    st, _, _ = http_get(port, "/ws?role=viewer&room=bad$", headers=dict(up, **{"Sec-WebSocket-Version": "13"}))
    check("WS 房間名唔啱 = 400", st == 400, st)
    st, _, _ = http_get(port, "/ws?role=admin&room=test", headers=dict(up, **{"Sec-WebSocket-Version": "13"}))
    check("WS role 唔啱 = 400", st == 400, st)

    # 亂噏嘅 bytes 唔可以令 server 死
    for junk in (b"\x00\xff\xfe garbage\r\n\r\n", b"GET\r\n\r\n", b"GET / HTTP/1.1\r\n" + b"X: y\r\n" * 5000 + b"\r\n"):
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=3) as s:
                s.sendall(junk)
                s.settimeout(3)
                try:
                    s.recv(100)
                except OSError:
                    pass
        except OSError:
            pass
    check("亂 bytes 之後 server 仲生存", http_get(port, "/healthz")[0] == 200)


async def test_main(sp: ServerProc) -> None:
    port = sp.port

    # ---- host 認領 ----
    host = await WS.connect(port, "role=host&room=test&key=k1")
    check("WS handshake Sec-WebSocket-Accept 正確", host.accept_ok)
    snap = await host.recv()
    check("host 第一條 = snapshot，冇人，youtube=-1",
          snap.get("t") == "snapshot" and snap.get("viewers") == [] and snap.get("youtube") == -1, snap)

    bad = await WS.connect(port, "role=host&room=test&key=wrong")
    m = await safe("錯 key 收 error", bad.recv())
    check("錯 key -> error bad key", m == {"t": "error", "msg": "bad key"}, m)
    check("錯 key -> 斷線", await bad.wait_closed(), bad.close_code)
    empty = await WS.connect(port, "role=host&room=other&key=")
    m = await safe("空 key", empty.recv())
    check("空 key -> error bad key", m and m.get("msg") == "bad key", m)
    await empty.wait_closed()

    # ---- 觀眾入場 ----
    names = ["阿一", "阿二", "阿三", "阿四", "阿五"]
    vs = []
    for i, n in enumerate(names):
        ws, ticket, first = await viewer(port, n)
        if i == 0:
            check("觀眾連線後（hello 之前）已經收到 state", first.get("t") == "state" and "named" in first, first)
            st = await ws.recv()
            check("hello 之後 ticket 再跟 state", st.get("t") == "state" and st.get("named") == 1, st)
            check("ticket 欄位齊（id/name/slot/label/room/streamer/streamUrl/rows）",
                  all(k in ticket for k in ("id", "name", "slot", "label", "room", "streamer", "streamUrl", "rows"))
                  and ticket["room"] == "test" and ticket["rows"] == srv.DEFAULT_ROWS, ticket)
        vs.append((ws, ticket))
    labels = [t["label"] for _, t in vs]
    slots = [t["slot"] for _, t in vs]
    check("頭 5 人 slot = 0..4", slots == [0, 1, 2, 3, 4], slots)
    check("頭 5 人 label = A18 A17 A19 A16 A20（mid+1, mid, mid+2 …）", labels == ["A18", "A17", "A19", "A16", "A20"], labels)
    joins = [await safe("join %d" % i, host.recv("join")) for i in range(5)]
    check("host 收到 5 個 join，資料同 ticket 一樣",
          all(j and j["viewer"] == {k: t[k] for k in ("id", "name", "slot", "label")} for j, (_, t) in zip(joins, vs)), joins)

    ws, t, _ = await viewer(port, "  \x07阿\u200b明\u202e  ")
    check("hello 名 sanitize（經 WS）", t["name"] == "阿明", t["name"])
    vs.append((ws, t))  # slot 5
    await host.recv("join")

    # ---- 重連返原位 ----
    v1, t1 = vs[1]
    await v1.close()
    left = await host.collect("leave", 0.3)
    check("斷線即刻唔會 leave（grace 內）", not left, left)
    v1b, t1b, _ = await viewer(port, "改咗名", vid=t1["id"])
    check("用舊 id 重連返原位（id/slot/label 一樣、名唔變）",
          (t1b["id"], t1b["slot"], t1b["label"], t1b["name"]) == (t1["id"], t1["slot"], t1["label"], t1["name"]), t1b)
    j = await host.collect("join", 0.5)
    check("重連唔會再發 join", not j, j)
    vs[1] = (v1b, t1b)

    # ---- grace 過咗 -> leave，最細空 slot ----
    v4, t4 = vs[4]
    await v4.close()
    lv = await safe("grace leave", host.recv("leave", timeout=3))
    check("grace 過咗 host 收 leave", lv == {"t": "leave", "id": t4["id"]}, lv)
    v6, t6, _ = await viewer(port, "後來者")
    check("新觀眾攞最細空 slot（4 / A20）", (t6["slot"], t6["label"]) == (4, "A20"), t6)
    await host.recv("join")
    v4b, t4b, _ = await viewer(port, "阿五", vid=t4["id"])
    check("過咗 grace 用舊 id 返嚟 = 新 id、新位（slot 6 / A21）",
          t4b["id"] != t4["id"] and (t4b["slot"], t4b["label"]) == (6, "A21"), t4b)
    await host.recv("join")
    vs[4] = (v6, t6)
    vs.append((v4b, t4b))   # slot 6

    # ---- 拍手限速 ----
    v0, t0 = vs[0]
    host.drain()
    for _ in range(5):
        v0.send({"t": "clap"})
    claps = await host.collect("clap", 0.25)
    check("300ms 內拍 5 下 -> host 只收 1 下", claps == [{"t": "clap", "id": t0["id"]}], claps)
    await asyncio.sleep(0.15)
    v0.send({"t": "clap"})
    claps = await host.collect("clap", 0.3)
    check("過咗 300ms 再拍 -> 收到", len(claps) == 1, claps)
    pre = await WS.connect(port, "role=viewer&room=test")
    await pre.recv("state")
    pre.send({"t": "clap"})
    check("未 hello 拍手唔會轉發", not await host.collect("clap", 0.4))

    # ---- config：ticket 重發 ----
    for ws, _ in vs:
        ws.drain()
    host.send({"t": "config", "streamer": "阿明</script><b>", "videoId": "",
               "streamUrl": "https://www.youtube.com/watch?v=dQw4w9WgXcQ", "callUrl": "https://discord.gg/abc"})
    tk = [await safe("config ticket", ws.recv("ticket")) for ws, _ in vs]
    check("config 改咗 -> 每個觀眾收新 ticket（streamer/streamUrl）",
          all(x and x["streamer"] == "阿明</script><b>" and x["streamUrl"].endswith("dQw4w9WgXcQ") for x in tk), tk[:1])
    check("ticket 冇 callUrl", all(x and "callUrl" not in x for x in tk))
    st, _, body = http_get(port, "/r/test")
    check("觀眾頁 boot JSON escape 咗 </script>", b"\\u003c/script\\u003e" in body and b"</script><b>" not in body)
    host.send({"t": "config", "streamer": "阿明", "videoId": "", "streamUrl": "javascript:alert(1)", "callUrl": "https://discord.gg/abc"})
    x = await safe("config js url", vs[0][0].recv("ticket"))
    check("streamUrl 唔係 http(s) 當空", x and x["streamUrl"] == "", x)
    host.send({"t": "config", "streamer": "阿明", "videoId": "",
               "streamUrl": "https://www.youtube.com/watch?v=dQw4w9WgXcQ", "callUrl": "https://discord.gg/abc"})
    await vs[0][0].recv("ticket")

    # ---- mode ----
    for ws, _ in vs:
        ws.drain()
    host.send({"t": "mode", "mode": "interactive"})
    ms = [await safe("mode", ws.recv("mode")) for ws, _ in vs]
    check("mode 轉發畀所有觀眾", all(m == {"t": "mode", "mode": "interactive"} for m in ms), ms)
    st = await safe("state after mode", vs[0][0].recv("state", timeout=2.5))
    check("state 跟住更新 mode=interactive、hostOnline、youtube=-1",
          st and st["mode"] == "interactive" and st["hostOnline"] is True and st["youtube"] == -1, st)
    host.send({"t": "mode", "mode": "party"})
    e = await safe("bad mode", host.recv("error"))
    check("唔啱嘅 mode -> host error", e and e["msg"] == "bad mode", e)

    # ---- lottery ----
    v2, t2 = vs[2]
    for ws, _ in vs:
        ws.drain()
    host.send({"t": "lottery", "phase": "spin"})
    sp_ = [await safe("spin", ws.recv("lottery")) for ws, _ in vs]
    check("lottery spin 轉發", all(m == {"t": "lottery", "phase": "spin"} for m in sp_), sp_)
    host.send({"t": "lottery", "phase": "winner", "id": t2["id"]})
    wn = [await safe("winner", ws.recv("lottery")) for ws, _ in vs]
    want = {"t": "lottery", "phase": "winner", "id": t2["id"], "name": t2["name"], "label": t2["label"]}
    check("lottery winner 補埋 name/label 轉發", all(m == want for m in wn), wn[:1])
    host.send({"t": "lottery", "phase": "winner", "id": "nobody"})
    e = await safe("winner unknown", host.recv("error"))
    check("抽中唔存在嘅 id -> host error", e and e["msg"] == "unknown viewer", e)

    # ---- guest：callUrl 只畀嘉賓 ----
    for ws, _ in vs:
        ws.drain()
    host.send({"t": "guest", "id": t2["id"]})
    gs = [await safe("guest", ws.recv("guest")) for ws, _ in vs]
    mine = gs[2]
    others = [g for i, g in enumerate(gs) if i != 2]
    check("嘉賓本人收到 callUrl", mine and mine.get("callUrl") == "https://discord.gg/abc"
          and mine["id"] == t2["id"] and mine["label"] == t2["label"], mine)
    check("其他人收到 guest 但冇 callUrl", all(g and g["id"] == t2["id"] and "callUrl" not in g for g in others), others[:1])
    st = await safe("state guest", vs[0][0].recv("state", timeout=2.5))
    check("state.guest = 嘉賓（冇 callUrl）",
          st and st["guest"] == {"id": t2["id"], "name": t2["name"], "label": t2["label"]}, st)
    # 嘉賓重連都會再收到有 callUrl 嗰條
    await v2.close()
    v2b, t2b, _ = await viewer(port, "x", vid=t2["id"])
    g = await safe("guest after reconnect", v2b.recv("guest"))
    check("嘉賓重連之後再收有 callUrl 嘅 guest", g and g.get("callUrl") == "https://discord.gg/abc", g)
    vs[2] = (v2b, t2b)
    host.send({"t": "guest", "id": ""})
    gs = [await safe("guest off", ws.recv("guest")) for ws, _ in vs]
    check("嘉賓落台廣播 id=''", all(g == {"t": "guest", "id": ""} for g in gs), gs[:1])

    # ---- layout ----
    for ws, _ in vs:
        ws.drain()
    host.send({"t": "layout", "rows": [5, 5, 5]})
    tk = [await safe("layout ticket", ws.recv("ticket")) for ws, _ in vs]
    got = {x["slot"]: x["label"] for x in tk if x}
    exp = {s: ref_label(s, [5, 5, 5]) for s in got}
    check("layout 改咗 -> label 重算並重發 ticket", got == exp and got.get(0) == "A3" and got.get(5) == "B3", got)
    check("ticket.rows = 新 layout", all(x and x["rows"] == [5, 5, 5] for x in tk))
    host.send({"t": "layout", "rows": [5, 5, 5]})
    check("同一個 layout 再送唔會重發", not await vs[0][0].collect("ticket", 0.4))
    host.drain()
    host.send({"t": "layout", "rows": [5.0, 5, 5.0]})
    await asyncio.sleep(0.4)
    check("整數值寫成 5.0 嘅 layout 照收（同 [5,5,5] 一樣，唔會 error、唔會重發 ticket）",
          not [m for m in host.drain() if m.get("t") == "error"]
          and not [m for m in vs[0][0].drain() if m.get("t") == "ticket"])
    for bad_rows in ([0], [1.5], "x", [], [True]):
        host.send({"t": "layout", "rows": bad_rows})
        e = await safe("bad layout", host.recv("error"))
        if not (e and e["msg"] == "bad layout"):
            check("壞 layout -> error", False, (bad_rows, e))
            break
    else:
        check("壞 layout（0 / 小數 / 字串 / 空 / bool）-> error bad layout", True)

    # ---- host 送垃圾 ----
    host.raw(encode_frame(OP_TEXT, b"{not json", mask=True))
    e = await safe("bad json", host.recv("error"))
    check("host 送壞 JSON -> error，連線仲喺度", e and e["msg"] == "bad message" and not host.closed.is_set(), e)
    host.send({"t": "nope"})
    e = await safe("unknown type", host.recv("error"))
    check("host 送未知類型 -> error unknown type", e and e["msg"] == "unknown type", e)

    # ---- state 合併（最多每秒一次）----
    obs = vs[0][0]
    await asyncio.sleep(1.1)
    obs.drain()
    extra = []
    for i in range(6):
        ws, t, _ = await viewer(port, "湊熱鬧%d" % i)
        extra.append(ws)
    states = await obs.collect("state", 1.6)
    check("6 個人連續入場，觀察者 1.6 秒內收 state <= 2 條（合併）", 1 <= len(states) <= 2, len(states))
    check("合併後 named 係最新數", states and states[-1]["named"] == len(vs) + 6, states[-1:] if states else None)
    for ws in extra:
        await ws.close()

    # ---- 觀眾被另一分頁頂走 ----
    v3, t3 = vs[3]
    v3b, t3b, _ = await viewer(port, "x", vid=t3["id"])
    e = await safe("viewer replaced", v3.recv("error"))
    check("同一 id 第二個分頁入場 -> 舊嗰個收 error replaced", e and e["msg"] == "replaced", e)
    check("新分頁坐返同一個位", t3b["slot"] == t3["slot"])
    vs[3] = (v3b, t3b)

    # ---- frame 層 ----
    fws = await WS.connect(port, "role=viewer&room=test")
    await fws.recv("state")
    fws.raw(encode_frame(OP_TEXT, b'{"t":"hel', fin=False, mask=True))
    fws.raw(encode_frame(OP_CONT, 'lo","name":"分段"}'.encode("utf-8"), fin=True, mask=True))
    t = await safe("fragmented", fws.recv("ticket"))
    check("分段訊息（text + continuation）", t and t["name"] == "分段", t)
    fws.raw(encode_frame(OP_PING, b"abc", mask=True))
    await asyncio.sleep(0.3)
    check("client ping -> server pong（同 payload）", b"abc" in fws.pongs, fws.pongs)
    await asyncio.sleep(1.3)
    check("server 定時 ping（--ping-interval 1）", len(fws.pings) >= 1, fws.pings)
    await fws.close()

    lws = await WS.connect(port, "role=viewer&room=test")
    await lws.recv("state")
    lws.send({"t": "hello", "id": None, "name": "長" * 150})   # payload > 125 -> 126 長度
    t = await safe("126 hello", lws.recv("ticket"))
    check("126 長度 frame 收到，名截到 20 字", t and t["name"] == "長" * 20, t)
    await lws.close()

    for label, frame, code in (
        ("冇 mask 嘅 client frame -> close 1002", encode_frame(OP_TEXT, b'{"t":"clap"}', mask=False), 1002),
        ("超過 64KB -> close 1009", encode_frame(OP_TEXT, b"x" * 70000, mask=True), 1009),
        ("binary frame -> close 1003", encode_frame(OP_BINARY, b"\x01\x02", mask=True), 1003),
        ("唔係 UTF-8 -> close 1007", encode_frame(OP_TEXT, b"\xff\xfe", mask=True), 1007),
        ("未開始就 continuation -> close 1002", encode_frame(OP_CONT, b"x", mask=True), 1002),
    ):
        w = await WS.connect(port, "role=viewer&room=test")
        await w.recv("state")
        w.raw(frame)
        closed = await w.wait_closed(3)
        check(label, closed and w.close_code == code, w.close_code)
        w.writer.close()

    # ---- 新 host 頂走舊 host ----
    await asyncio.sleep(1.4)   # 等 frame 測試嗰兩位觀眾（分段 / 126）過 grace 釋放
    host2 = await WS.connect(port, "role=host&room=test&key=k1")
    snap2 = await safe("snapshot2", host2.recv("snapshot"))
    e = await safe("host replaced", host.recv("error"))
    check("新 host 頂走舊 host：舊嗰個收 error replaced", e and e["msg"] == "replaced", e)
    check("舊 host 被斷線", await host.wait_closed())
    named_ids = sorted(t["id"] for _, t in vs)
    check("新 host snapshot = 現有具名觀眾（按 slot 排）、youtube=-1",
          snap2 and sorted(v["id"] for v in snap2["viewers"]) == named_ids and snap2["youtube"] == -1
          and [v["slot"] for v in snap2["viewers"]] == sorted(v["slot"] for v in snap2["viewers"]),
          snap2 and len(snap2["viewers"]))

    # ---- host 離線 ----
    await asyncio.sleep(1.1)
    obs.drain()
    await host2.close()
    st = await safe("hostOnline false", obs.recv("state", timeout=2.5))
    check("host 走咗 -> state.hostOnline=false", st and st["hostOnline"] is False, st)
    vx, tx, _ = await viewer(port, "host 唔喺度都入到")
    check("host 離線時觀眾照樣入場", tx and tx["label"], tx)
    host3 = await WS.connect(port, "role=host&room=test&key=k1")
    snap3 = await safe("snapshot3", host3.recv("snapshot"))
    check("host 返嚟 snapshot 補返離線期間入場嘅人", snap3 and tx["id"] in [v["id"] for v in snap3["viewers"]])

    for ws, _ in vs:
        await ws.close()
    await vx.close()
    await host3.close()
    await pre.close()


async def test_full() -> None:
    sp = ServerProc("--max-viewers", "3")
    try:
        ws_list = []
        for i in range(3):
            ws, t, _ = await viewer(sp.port, "人%d" % i)
            ws_list.append(ws)
        w = await WS.connect(sp.port, "role=viewer&room=test")
        await w.recv("state")
        w.send({"t": "hello", "id": None, "name": "第四個"})
        e = await safe("full", w.recv("error"))
        check("滿座（--max-viewers 3）第 4 個收 error full 並斷線", e and e["msg"] == "full" and await w.wait_closed(), e)
        for ws in ws_list:
            await ws.close()
    finally:
        sp.stop()


async def test_layout_capacity() -> None:
    sp = ServerProc("--max-viewers", "10")
    try:
        host = await WS.connect(sp.port, "role=host&room=cap&key=k")
        await host.recv("snapshot")
        host.send({"t": "layout", "rows": [2, 1]})
        await asyncio.sleep(0.2)
        ws_list, tickets = [], []
        for i in range(3):
            ws, t, _ = await viewer(sp.port, "人%d" % i, room="cap")
            ws_list.append(ws)
            tickets.append(t)
        check("layout [2,1]：頭 3 人坐 A2 A1 B1", [t["label"] for t in tickets] == ["A2", "A1", "B1"],
              [t["label"] for t in tickets])
        w = await WS.connect(sp.port, "role=viewer&room=cap")
        await w.recv("state")
        w.send({"t": "hello", "id": None, "name": "第四個"})
        e = await safe("layout full", w.recv("error"))
        check("host 送咗 layout 之後具名觀眾唔可以多過座位數（3 < --max-viewers 10）-> full",
              e and e["msg"] == "full" and await w.wait_closed(), e)
        await ws_list[1].close()
        lv = await safe("cap leave", host.recv("leave", timeout=3))
        check("滿座之後有人走咗（grace 過咗）host 收 leave", lv and lv["id"] == tickets[1]["id"], lv)
        ws, t, _ = await viewer(sp.port, "補位", room="cap")
        check("空出嚟嘅座位可以再入場（slot / label 同走咗嗰位一樣）",
              (t["slot"], t["label"]) == (tickets[1]["slot"], tickets[1]["label"]), t)
        ws_list[1] = ws
        for x in ws_list:
            await x.close()
        await host.close()
    finally:
        sp.stop()


class FakeYouTube(http.server.BaseHTTPRequestHandler):
    count = "4321"
    fail = False
    keys: List[str] = []
    ids: List[str] = []

    def do_GET(self) -> None:  # noqa: N802
        q = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query)
        FakeYouTube.keys.append((q.get("key") or [""])[0])
        FakeYouTube.ids.append((q.get("id") or [""])[0])
        if FakeYouTube.fail:
            body = b'{"error":{"message":"quota exceeded"}}'
            self.send_response(403)
        else:
            vid = (q.get("id") or [""])[0]
            body = json.dumps({"items": [{"id": vid, "liveStreamingDetails": {"concurrentViewers": FakeYouTube.count}}]}).encode()
            self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args: Any) -> None:
        pass


async def test_youtube() -> None:
    fake = http.server.ThreadingHTTPServer(("127.0.0.1", 0), FakeYouTube)
    threading.Thread(target=fake.serve_forever, daemon=True).start()
    endpoint = "http://127.0.0.1:%d/youtube/v3/videos" % fake.server_address[1]
    sp = ServerProc("--yt-interval", "1", "--yt-endpoint", endpoint, env={"YOUTUBE_API_KEY": "SECRETKEY123"})
    try:
        host = await WS.connect(sp.port, "role=host&room=yt&key=k")
        await host.recv("snapshot")
        v, t, _ = await viewer(sp.port, "睇緊", room="yt")
        host.send({"t": "config", "streamer": "s", "videoId": "dQw4w9WgXcQ", "streamUrl": "", "callUrl": ""})
        c = await safe("yt count", host.recv("count", timeout=4))
        check("有 key + videoId -> host 收 count", c == {"t": "count", "youtube": 4321}, c)
        st = await safe("yt state", v.recv("state", timeout=3))
        while st and st.get("youtube") != 4321:
            st = await safe("yt state", v.recv("state", timeout=3))
        check("觀眾 state.youtube 更新", st and st["youtube"] == 4321, st)
        check("API key 有帶去 YouTube", "SECRETKEY123" in FakeYouTube.keys)
        FakeYouTube.fail = True
        await asyncio.sleep(2.5)
        check("YouTube API 出錯 server 唔死、保持舊值", sp.alive() and http_get(sp.port, "/healthz")[0] == 200)
        host.drain()
        FakeYouTube.fail = False
        FakeYouTube.count = "5000"
        c = await safe("yt recover", host.recv("count", timeout=4))
        check("YouTube 恢復之後繼續更新", c == {"t": "count", "youtube": 5000}, c)
        check("log 有寫失敗原因", "YouTube 人數攞唔到" in sp.log())
        check("log 唔會漏 API key", "SECRETKEY123" not in sp.log())
        # 冇填 videoId，由 YouTube 直播網址拆
        host.send({"t": "config", "streamer": "s", "videoId": "", "streamUrl": "https://youtu.be/abcdefghijk", "callUrl": ""})
        await asyncio.sleep(1.5)
        check("冇 videoId 時由 streamUrl 拆 YouTube id 去攞人數", "abcdefghijk" in FakeYouTube.ids, FakeYouTube.ids[-3:])
        await v.close()
        await host.close()
    finally:
        sp.stop()
        fake.shutdown()


async def amain() -> int:
    test_units()
    await test_frames_unit()
    sp = ServerProc("--ping-interval", "1")
    try:
        await test_http(sp.port)
        await test_main(sp)
        check("全部測試之後 server 仲生存", sp.alive() and http_get(sp.port, "/healthz")[0] == 200)
        errors = [ln for ln in sp.log().splitlines() if "Traceback" in ln or " ERROR " in ln]
        check("server log 冇 Traceback / ERROR", not errors, errors[:5])
    finally:
        sp.stop()
    await test_full()
    await test_layout_capacity()
    await test_youtube()

    # YouTube 冇 key：snapshot/state 都係 -1（test_main 已經驗過），再驗一次 config 有 videoId 都係 -1
    sp = ServerProc()
    try:
        host = await WS.connect(sp.port, "role=host&room=nokey&key=k")
        await host.recv("snapshot")
        host.send({"t": "config", "streamer": "s", "videoId": "dQw4w9WgXcQ", "streamUrl": "", "callUrl": ""})
        c = await host.collect("count", 1.5)
        v, t, first = await viewer(sp.port, "x", room="nokey")
        check("冇 YOUTUBE_API_KEY：唔會發 count、state.youtube=-1", not c and first["youtube"] == -1, (c, first))
        await v.close()
        await host.close()
    finally:
        sp.stop()

    passed = sum(RESULTS)
    print("\n%d / %d PASS" % (passed, len(RESULTS)), flush=True)
    return 0 if passed == len(RESULTS) else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(amain()))
