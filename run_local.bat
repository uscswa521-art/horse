@echo off
REM 搵屋爬蟲 — 本機長駐版 (Windows)
REM 用法: 喺呢個資料夾 double-click 呢個檔案, 或者喺 CMD 打:  run_local.bat
cd /d "%~dp0"
chcp 65001 >nul

echo ================================
echo   搵屋爬蟲 (本機長駐版)
echo ================================

echo [1/3] 裝緊套件 (第一次會耐少少)...
python -m pip install --upgrade pip
python -m pip install "scrapling[fetchers]"
scrapling install || python -m camoufox fetch

if "%TELEGRAM_BOT_TOKEN%"=="" set /p TELEGRAM_BOT_TOKEN="[2/3] 貼上你個 Telegram bot token 再撳 Enter: "
set USE_STEALTH=1

echo [3/3] 開始! 每小時自動搵一次。唔好閂呢個視窗。 (Ctrl+C 停止)
echo.
python -m property_finder.finder --loop --interval 3600
pause
