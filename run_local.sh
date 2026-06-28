#!/usr/bin/env bash
# 搵屋爬蟲 — 本機長駐版 (macOS / Linux)
# 用法: 喺呢個資料夾開 Terminal, 打:  bash run_local.sh
set -e
cd "$(dirname "$0")"

echo "================================"
echo "  搵屋爬蟲 (本機長駐版)"
echo "================================"

# 1) 裝依賴 (隱身瀏覽器爬蟲)
echo "[1/3] 裝緊套件 (第一次會耐少少)..."
python3 -m pip install --upgrade pip >/dev/null
python3 -m pip install "scrapling[fetchers]"
scrapling install || python3 -m camoufox fetch || true

# 2) 攞 Telegram token
if [ -z "${TELEGRAM_BOT_TOKEN:-}" ]; then
  echo
  read -r -p "[2/3] 貼上你個 Telegram bot token 再撳 Enter: " TELEGRAM_BOT_TOKEN
fi
export TELEGRAM_BOT_TOKEN
export USE_STEALTH=1

# 3) 每小時跑一次, 唔好閂個 Terminal (閂咗就停)
echo "[3/3] 開始! 每小時自動搵一次。唔好閂呢個視窗。 (Ctrl+C 停止)"
echo
python3 -m property_finder.finder --loop --interval 3600
