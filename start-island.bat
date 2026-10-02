@echo off
rem ================= IF-OA 一键启动 =================
rem 用途：双击即启动「后端(FastAPI) + 灵动岛客户端(WinUI3)」
rem 依赖：backend\venv 已装依赖（见 backend\README.md）；客户端需先用
rem       VS2022 打开 client\Island.sln 生成一次（x64 Release/Debug 均可）
chcp 65001 >nul
title IF-OA 一键启动

rem ---------- 后端 ----------
if not exist "%~dp0backend\venv\Scripts\python.exe" (
    echo [!] 未找到 backend\venv，请先参照 backend\README.md 创建并安装依赖
    pause
    exit /b 1
)
start "IF-OA-Backend" cmd /k "cd /d %~dp0backend && venv\Scripts\python.exe -m uvicorn app.main:app --host 0.0.0.0 --port 8600"
echo [..] 后端启动中（独立窗口；关闭该窗口即停止服务）...
timeout /t 3 /nobreak >nul
curl -s http://localhost:8600/api/health >nul 2>&1
if %errorlevel%==0 (
    echo [√] 后端就绪：http://localhost:8600   接口文档：http://localhost:8600/docs
) else (
    echo [!] 后端健康检查未通过，请查看后端窗口日志
)

rem ---------- 灵动岛客户端 ----------
rem ponytail: 探测 Release/Debug 两个产物路径；客户端尚未编译则提示后跳过
set "EXE=%~dp0client\IslandApp\x64\Release\IslandApp.exe"
if not exist "%EXE%" set "EXE=%~dp0client\IslandApp\x64\Debug\IslandApp.exe"
if exist "%EXE%" (
    start "" "%EXE%"
    echo [√] 灵动岛已启动（顶部胶囊，点击展开）
) else (
    echo [i] 客户端尚未编译：用 VS2022 打开 client\Island.sln 生成一次（x64）
)

echo.
echo 完成。
timeout /t 8
