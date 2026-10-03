# IF-OA 灵动岛轻量化 OA

常驻 Windows 屏幕顶部的"灵动岛"办公系统：胶囊态（216×36 时间/音乐/角标）↔ 展开态（1280×480 四页面：工作台/文件/对话/个人中心），与 FastAPI 后端通信。小团队（<100 人）内网部署，轻量优先。

```text
WinUI3 + C++/WinRT 客户端（XAML + Composition 动画 + AppWindow 窗口控制）
        │ REST(Windows.Web.Http) + WebSocket(MessageWebSocket)
FastAPI + SQLAlchemy 2.0(async) + SQLite(WAL)（认证/组织/文件/聊天/审批/待办/通知）
```

## 目录结构

```text
IF-OA/
├── start-island.bat      # 一键启动（后端 + 客户端）
├── docs/                 # 契约与设计文档（唯一事实来源）
│   ├── 01-数据库与接口契约.md
│   └── 02-灵动岛设计.md
├── backend/              # FastAPI 后端（详见 backend/README.md）
│   ├── app/
│   ├── requirements.txt
│   └── smoke_test.py     # 22 项冒烟自检
└── client/               # WinUI3 C++/WinRT 客户端（VS 解决方案）
    ├── Island.sln
    └── IslandApp/
```

## 环境要求

- Windows 10 19041+ / Windows 11
- Python 3.11+（后端）
- Visual Studio 2022 或更高（含 VS18/2026），需安装：
  - 工作负载：**使用 C++ 的桌面开发**
  - 单个组件：**适用于 C++ 的通用 Windows 平台工具**（提供原生 XAML 编译规则，必装）
  - 可选：Windows App SDK C++ 模板（新建项目用模板更方便）
- Windows App SDK 1.8.260921001 与 CppWinRT 2.0.250303.1 通过 NuGet 还原自动下载，无需手动安装

## 安装

### 后端（依赖由用户手动安装）

```bat
cd E:\IF-OA\backend
python -m venv venv
venv\Scripts\pip install -r requirements.txt
```

### 客户端（NuGet 还原即可）

用 VS 打开 `client\Island.sln` 首次生成时会自动还原 NuGet 包，无需其他步骤。

## 构建客户端

**方式一（VS）**：打开 `client\Island.sln` → 配置 x64 → 生成（Debug/Release 均可）。

**方式二（命令行）**：

```bat
"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe"
:: 用上面输出的 MSBuild.exe 路径执行：
MSBuild.exe E:\IF-OA\client\IslandApp\IslandApp.vcxproj /t:Build /p:Configuration=Debug /p:Platform=x64
```

产物：`client\IslandApp\x64\Debug\IslandApp.exe`（解包 + 自包含部署，拷目录即用，无需安装器）。

## 运行

**一键启动**：双击 `start-island.bat`（后端独立窗口 + 自动探测并启动客户端 exe）。

**手动运行**：

```bat
:: 后端（0.0.0.0 供内网其他机器访问）
cd E:\IF-OA\backend
venv\Scripts\python.exe -m uvicorn app.main:app --host 0.0.0.0 --port 8600

:: 客户端（灵动岛出现在屏幕顶部居中）
E:\IF-OA\client\IslandApp\x64\Debug\IslandApp.exe
```

- 首次启动自动建库并创建管理员：**admin / admin123**
- 接口文档：<http://localhost:8600/docs>
- 客户端交互：点击胶囊展开 / 拖动换位（位置持久化）/ Esc 或点窗外收起

## 常见问题

| 现象 | 处理 |
| --- | --- |
| 编译报 MSB8020（找不到 v143 工具集） | vcxproj 已用 `$(DefaultPlatformToolset)` 自适应，检查是否装了 C++ 桌面开发工作负载 |
| XAML 编译相关报错 | 确认已装"适用于 C++ 的通用 Windows 平台工具"组件 |
| 客户端点了没反应 | 任务管理器结束残留的 IslandApp 进程后重试（崩溃僵尸进程会攥住单实例互斥锁；新版本已自动绕过） |
| 端口冲突 | 修改 uvicorn `--port` 与客户端设置中的服务端地址 |

## 已知限制（P1 待办）

- 胶囊 18px 全圆角为 DWM 系统圆角近似（WASDK 暂无真透明窗口）
- 多显示器拖动跨屏为 P1；当前按主屏工作区钳制
- 登录卡/服务接入（ApiClient/WS/SMTC）为下一阶段
