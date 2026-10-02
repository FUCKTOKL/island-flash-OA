"""FastAPI 入口：装配全部路由 + 首启初始化。

启动（backend/ 目录下，依赖已由用户装进 venv）：
    venv\\Scripts\\uvicorn app.main:app --host 0.0.0.0 --port 8600
"""

from contextlib import asynccontextmanager

from fastapi import FastAPI

from . import ws
from .config import ensure_dirs
from .routers import approvals, auth, chat, files, misc, org
from .seed import init_db


@asynccontextmanager
async def lifespan(_app: FastAPI):
    ensure_dirs()  # data/ avatars/ uploads/
    await init_db()  # 建表 + 默认管理员（幂等）
    yield


app = FastAPI(title="IF-OA", lifespan=lifespan)

# REST 全部挂 /api 前缀（契约）
app.include_router(auth.router, prefix="/api")
app.include_router(org.router, prefix="/api")
app.include_router(files.router, prefix="/api")
app.include_router(chat.router, prefix="/api")
app.include_router(approvals.router, prefix="/api")
app.include_router(misc.router, prefix="/api")
app.include_router(ws.router)  # /ws 不带前缀（契约 §三）


@app.get("/api/health")
async def health():
    """连通性测试：客户端"服务端地址"设置页拨它。"""
    return {"status": "ok"}
