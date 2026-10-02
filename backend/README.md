# IF-OA 后端

FastAPI + SQLite 单进程服务，接口契约见 `../docs/01-数据库与接口契约.md`（v0.4）。

## 环境与依赖（用户手动执行，AI 不碰环境）

```bat
cd backend
venv\Scripts\pip install -r requirements.txt
```

## 启动

```bat
cd backend
venv\Scripts\uvicorn app.main:app --host 0.0.0.0 --port 8600
```

- 首次启动自动建表，并创建默认部门「总部」和管理员 `admin / admin123`（**请尽快改密码**）
- 数据（SQLite + 上传文件）都在 `backend/data/`，已 gitignore，删掉即重置
- 健康检查：`GET http://localhost:8600/api/health`
- 调试用交互文档：`http://localhost:8600/docs`
- 生产密钥：启动前设环境变量 `IFOA_JWT_SECRET`（默认值仅供开发）

## 目录

```text
backend/
├── app/
│   ├── main.py        # 入口：装配路由 + 首启初始化
│   ├── config.py      # 路径/常量/密钥
│   ├── database.py    # engine + session（WAL + 外键 PRAGMA）
│   ├── models.py      # 13 张表（契约一比一）
│   ├── schemas.py     # Pydantic 请求模型
│   ├── security.py    # bcrypt + JWT
│   ├── deps.py        # get_db / get_current_user / require_admin
│   ├── seed.py        # 首启建表 + admin
│   ├── ws.py          # WebSocket 连接管理 + /ws 端点
│   └── routers/       # auth org files chat approvals misc
├── data/              # 运行数据（gitignore）
├── requirements.txt
└── venv/              # 用户手建（gitignore）
```
