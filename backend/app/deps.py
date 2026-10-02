"""FastAPI 公共依赖：数据库会话、当前用户、管理员校验。

所有需要登录的端点写 `me: User = Depends(get_current_user)`；
需要管理员的写 `me: User = Depends(require_admin)`。
"""

from collections.abc import AsyncIterator

from fastapi import Depends, HTTPException, Request
from sqlalchemy.ext.asyncio import AsyncSession

from .database import SessionLocal
from .models import User
from .security import parse_token


async def get_db() -> AsyncIterator[AsyncSession]:
    """每请求一会话，请求结束自动归还。"""
    async with SessionLocal() as session:
        yield session


async def get_current_user(
    request: Request, db: AsyncSession = Depends(get_db)
) -> User:
    """解析 Authorization: Bearer <jwt> → 加载用户。

    401 = 没登录/过期（客户端应跳登录页）；403 = 账号被停用（提示找管理员）。
    """
    token = request.headers.get("Authorization", "").removeprefix("Bearer ").strip()
    uid = parse_token(token)
    if uid is None:
        raise HTTPException(401, "未登录或登录已过期")
    user = await db.get(User, uid)
    if user is None:
        raise HTTPException(401, "账号不存在")
    if user.status != "active":
        raise HTTPException(403, "账号已停用，请联系管理员")
    return user


async def require_admin(user: User = Depends(get_current_user)) -> User:
    """管理员门槛。管理端点统一挂这个。"""
    if user.role != "admin":
        raise HTTPException(403, "需要管理员权限")
    return user
