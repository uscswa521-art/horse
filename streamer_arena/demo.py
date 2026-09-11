"""
Demo 數據 / deterministic mock data.

冇 API key (或者全部平台都撲街) 嘅時候用, 令個視覺化即刻睇到效果。
數字係按真實直播生態嘅分佈 (Zipf: 頭幾個食晒大部分人) 砌出嚟, 但係**唔係真數據** —
所以 JSON 會標明 source = "demo", arena.html 見到就會喺畫面打「示範數據」水印。
"""
import hashlib
import math

from .sources import Result, Streamer

# 平台 : (直播主名, 分類) — 名係虛構嘅示範頻道, 唔對應任何真人
_ROSTER = {
    "twitch": [
        ("NeonKettle", "Just Chatting"), ("PixelDim", "League of Legends"),
        ("SaltyRamen", "VALORANT"), ("VoidCastle", "Grand Theft Auto V"),
        ("MochiRaid", "Just Chatting"), ("BitLantern", "Counter-Strike 2"),
        ("HyperTofu", "Minecraft"), ("EchoDelta", "Fortnite"),
        ("LimeOrbit", "Dota 2"), ("StaticPanda", "IRL"),
        ("RiftNoodle", "Apex Legends"), ("CobaltFig", "Chess"),
        ("DriftMuse", "Music"), ("PlasmaYuzu", "Just Chatting"),
        ("GlitchHarbor", "Souls-likes"),
    ],
    "youtube": [
        ("環球新聞直播台", "News"), ("Lo-Fi 深夜電台", "Music"),
        ("SpaceLaunch Live", "Science & Tech"), ("Cricket Central", "Sports"),
        ("料理長 24H", "Food"), ("Retro Arcade TV", "Gaming"),
        ("城市街頭 Cam", "Travel"), ("Study With Me 自習室", "Education"),
        ("Highlights 足球台", "Sports"), ("水族箱慢直播", "Pets"),
        ("Tech Keynote Live", "Science & Tech"), ("晚間音樂會", "Music"),
    ],
    "kick": [
        ("BoardroomBets", "Slots"), ("MidnightPoker", "Poker"),
        ("RallyStage", "Racing"), ("KnockoutKing", "Sports"),
        ("SunsetLounge", "Just Chatting"), ("PixelForge", "Gaming"),
        ("TurboOtter", "GTA RP"), ("StreetEats", "IRL"),
    ],
    "tiktok": [
        ("@dancefloor.hk", "Dance"), ("@makeup.mika", "Beauty"),
        ("@streetfood.tw", "Food"), ("@gym.bro.daily", "Fitness"),
        ("@karaoke.night", "Music"), ("@plushie.shop", "Shopping"),
        ("@late.night.talk", "Talk"), ("@fishing.uncle", "Outdoors"),
        ("@sketch.live", "Art"), ("@puppy.cam", "Pets"),
    ],
}

# 每個平台嘅「一哥」人數量級 — 反映真實生態嘅相對規模
_PEAK = {"twitch": 78000, "youtube": 145000, "kick": 26000, "tiktok": 41000}


def _jitter(seed_text: str, spread: float = 0.35) -> float:
    """由個名 hash 出一個穩定嘅 ±spread 抖動, 唔使 random, 每次跑都一樣。"""
    h = int(hashlib.sha1(seed_text.encode()).hexdigest()[:8], 16)
    return 1.0 + ((h % 2000) / 1000.0 - 1.0) * spread


def demo_results(platforms=None, top_n: int = 60, salt: str = "") -> list:
    """砌一批 Result, 格式同真實 fetcher 一模一樣。"""
    out = []
    for p in (platforms or list(_ROSTER)):
        roster = _ROSTER.get(p)
        if not roster:
            continue
        peak = _PEAK.get(p, 20000)
        streamers = []
        for i, (name, cat) in enumerate(roster[:top_n]):
            # Zipf: 第 n 名 ≈ 一哥 / n^0.85
            base = peak / math.pow(i + 1, 0.85)
            v = int(base * _jitter(f"{salt}{p}{name}"))
            streamers.append(Streamer(
                platform=p, name=name, viewers=max(v, 120),
                title=f"{cat} · 示範數據",
                category=cat,
                url="", language="", avatar="",
                channel_id=name.lstrip("@").lower(),
            ))
        streamers.sort(key=lambda s: s.viewers, reverse=True)
        out.append(Result(p, streamers=streamers))
    return out
