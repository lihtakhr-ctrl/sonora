@echo off
setlocal
cd /d "%~dp0"

where cmake >nul 2>nul
if errorlevel 1 (
    echo [ERROR] CMake が見つかりません。
    echo   Visual Studio Installer で
    echo   「C++ によるデスクトップ開発」→「C++ CMake ツール」にチェックを入れてください。
    pause & exit /b 1
)

echo === クライアントをビルドします ===
cmake -S client -B client\build -A x64
if errorlevel 1 ( echo 構成に失敗 & pause & exit /b 1 )

cmake --build client\build --config Release
if errorlevel 1 ( echo ビルドに失敗 & pause & exit /b 1 )

echo.
echo 完成: client\build\Release\ApexRing0Client.exe
pause