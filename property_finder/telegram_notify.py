"""
Telegram 通知 / Telegram notifier.

唔好喺 code 入面寫死 token —  token 由環境變數 TELEGRAM_BOT_TOKEN 提供。
"""
import html
import json
import urllib.parse
import urllib.request

API = "https://api.telegram.org/bot{token}/{method}"


class Telegram:
    def __init__(self, token: str, chat_id: str = ""):
        if not token:
            raise ValueError(
                "冇 TELEGRAM_BOT_TOKEN! 請喺 GitHub repo secrets 加返。"
            )
        self.token = token
        self.chat_id = str(chat_id) if chat_id else ""

    # ── 低層 API ────────────────────────────────────────────────────────
    def _call(self, method: str, params: dict) -> dict:
        url = API.format(token=self.token, method=method)
        data = urllib.parse.urlencode(params).encode()
        req = urllib.request.Request(url, data=data)
        with urllib.request.urlopen(req, timeout=30) as resp:
            return json.loads(resp.read().decode())

    # ── chat id 自動偵測 ────────────────────────────────────────────────
    def resolve_chat_id(self) -> str:
        """
        如果冇預設 chat_id, 用 getUpdates 攞返最近同 bot 傾過偈嘅人。
        你只要喺 Telegram 開 @Foundhorse_bot 㩒 Start / 講一句嘢就得。
        """
        if self.chat_id:
            return self.chat_id
        try:
            res = self._call("getUpdates", {"limit": 100})
        except Exception as e:  # noqa: BLE001
            print(f"[telegram] getUpdates 失敗: {e}")
            return ""
        chat_ids = []
        for upd in res.get("result", []):
            msg = upd.get("message") or upd.get("channel_post") or {}
            chat = msg.get("chat") or {}
            if chat.get("id") is not None:
                chat_ids.append(str(chat["id"]))
        if chat_ids:
            self.chat_id = chat_ids[-1]  # 最近一個
            print(f"[telegram] 自動搵到 chat_id = {self.chat_id}")
        else:
            print(
                "[telegram] 搵唔到 chat_id。請先喺 Telegram 同 @Foundhorse_bot "
                "㩒 Start 再講一句嘢。"
            )
        return self.chat_id

    # ── 發送 ────────────────────────────────────────────────────────────
    def send(self, text: str, disable_preview: bool = False) -> bool:
        if not self.chat_id:
            self.resolve_chat_id()
        if not self.chat_id:
            return False
        try:
            self._call(
                "sendMessage",
                {
                    "chat_id": self.chat_id,
                    "text": text,
                    "parse_mode": "HTML",
                    "disable_web_page_preview": "true" if disable_preview else "false",
                },
            )
            return True
        except Exception as e:  # noqa: BLE001
            print(f"[telegram] sendMessage 失敗: {e}")
            return False


def esc(s) -> str:
    """HTML escape, 安全放入 Telegram 訊息。"""
    return html.escape(str(s if s is not None else ""))
