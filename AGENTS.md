# IF-OA — Windows 灵动岛轻量化 OA 系统

## 项目定位

一个常驻 Windows 屏幕顶部的“灵动岛”桌面应用（胶囊态 ↔ 展开态），展开后可切换 4 个页面，后端为公司内网的轻量 OA 服务。另有一个完整桌面 OA 客户端（全功能+管理后台，技术栈默认同 WinUI3），与灵动岛共用同一套后端与接口契约。目标用户：小团队（<100 人）。核心约束：**轻量**（内存占用低、安装包小、部署简单）。

## 技术栈（已定案，不要更换）

| 层 | 技术 | 说明 |
| --- | --- | --- |
| 客户端 UI | WinUI 3 + XAML | 无边框透明置顶窗口、Composition 弹性动画 |
| 客户端逻辑 | C++/WinRT | Windows App SDK 1.5+，VS2022 |
| 系统能力 | windows-rs 等价 WinRT API | SMTC 读正在播放音乐、`AppWindow` 窗口控制、`Launcher` 启动外部程序、`MessageWebSocket`/`Windows.Web.Http` 通信（系统自带，不引第三方网络库） |
| 后端 | Python + FastAPI + uvicorn | REST + WebSocket，单进程部署内网 |
| ORM/DB | SQLAlchemy 2.0 (async) + aiosqlite + SQLite (WAL) | 不用 Postgres/Redis/消息队列 |
| 认证 | PyJWT（7 天过期）+ passlib[bcrypt] | v1 不做 refresh token |
| 天气 | 客户端直连 Open-Meteo API | 免费无 key，不过后端 |
| 文件存储 | 服务器磁盘 + DB 元数据 | 不用对象存储，单文件上限 200MB |

**明确不用**：Electron、WebView2 混合 UI（留作动画做不出效果时的后路）、MVVM 框架（C++ 侧 code-behind + x:Bind 即可）。

## 架构

```text
WinUI3 + C++/WinRT 客户端（XAML UI + Composition 动画 + 系统能力）
        │ REST(Windows.Web.Http) + WebSocket(MessageWebSocket)
FastAPI + SQLAlchemy + SQLite（认证/组织/文件/聊天/审批/待办/通知）
        │
磁盘存文件，Open-Meteo 出天气
```

- 胶囊态：常驻顶部居中，显示时间/音乐/未读角标；鼠标不在其上时开启点击穿透
- 展开态：1280×480（横向宽板，以胶囊 x 为中心向两侧展开），内部切换 4 个页面，展开动画 = 逐帧 `AppWindow.Resize` + XAML Composition 弹性动画；详细设计见 `docs/02-灵动岛设计.md`
- 独占全屏应用会盖住灵动岛——接受，不抢 z-order
- 通信约定：**所有写操作走 REST，WS 只做服务端推送**（message.new / approval.update / notification.new），客户端 30s ping 心跳

## 功能范围

### 4 个页面（展开态切换）

1. **工作台（个人仪表盘）**：小组件自由增删/排序（布局存本机 LocalSettings JSON，2 列网格 small/wide 两档）：天气、待办、喜欢的照片（= 收藏的个人图片文件）、音乐卡片（SMTC 读系统正在播放，不做播放器）、客户端启动图标（最多 4 个）、审批入口卡片
2. **文件页（左右分栏）**：左 = 公共文件（顶部群选择栏：全员公共区 + 我所在的群，群区群成员读写），右 = 我的文件；上传/下载/删除/收藏；P0 平铺列表
3. **对话页（左列表右会话）**：单聊 + 群聊统一会话模型；列表项含头像/摘要/未读数；单聊首次发消息自动建会话；文字/图片/文件消息；WS 实时追加
4. **个人中心**：资料/头像/密码 + 设置中心（4 个软件、天气城市、服务端地址、开机自启）

### 审批流（已拍板要做）

- **顺序链审批**：节点 1→2→…→n 逐级，不做会签/并行/条件分支
- 审批人两种类型：指定人（fixed_user）/ 部门领导（dept_leader，运行时解析 departments.leader_id）
- 表单由模板 JSON schema 定义（请假/报销等），管理员可维护模板
- UI 位置（已拍板）：**不做第 5 个页签**，入口放个人使用页的卡片（"待我审批 N 条"），点开浮出列表/详情层；胶囊态有待审批时显示角标

### 明确不做（P2 再议）

考勤打卡、会议提醒、公告栏、文件分片上传、消息撤回/已读回执、refresh token、回收站（files 表已留 deleted_at 软删列）

## 数据库与接口契约

**唯一契约：`docs/01-数据库与接口契约.md`** —— 9 张表（departments/users/files/favorites/conversations/conversation_members/messages/approval_templates/approval_nodes/approvals/approval_records/todos/notifications）+ 约 25 个 REST 端点 + WS 推送格式。

⚠️ 改契约必须双方（客户端/服务端）同步确认后再动代码。

## 目录结构（规划）

```text
IF-OA/
├── docs/            # 契约与设计文档
├── backend/         # FastAPI 后端（venv 由用户手动创建维护）
│   ├── app/main.py  # 入口
│   ├── app/models.py、routers/…
│   ├── data/        # SQLite + 上传文件（gitignore）
│   └── venv/        # 虚拟环境（用户手建，gitignore）
└── client/          # WinUI3 C++ 客户端（VS2022 解决方案）
```

## 开发环境

- 客户端：VS 2022 + "Windows 应用程序开发"工作负载 + Windows App SDK 1.5+，目标 Win10 19041+ / Win11
- 后端：Python 3.11+，依赖 `pip install fastapi "uvicorn[standard]" "sqlalchemy[asyncio]" aiosqlite pyjwt bcrypt python-multipart`
- 部署：内网一台机器，uvicorn 单进程 + NSSM/systemd 守护；客户端无安装器诉求（打包即用）

## 工作约定

- 遵循 ponytail（懒人优先）：最少代码、stdlib/平台能力优先、不过度设计；刻意取舍处留 `// ponytail:` 注释说明天花板与升级路径
- **环境由用户手动建造**：AI 不创建/删除虚拟环境、不执行 pip install；需要依赖时只写/更新 requirements.txt 清单，由用户自行安装；AI 需要删除批量内容（如整个 venv 目录）时必须停下请用户手动处理
- 契约文档是双方唯一事实来源，接口/表结构变更先改文档
- 时间存储统一 TEXT ISO8601 UTC；id 统一 INTEGER 自增
- 客户端 UI 文案中文；代码注释中文
- 服务端不引入 fastapi 依赖清单之外的库，除非先在本文档登记理由
