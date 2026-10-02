"""认证与个人信息（契约：认证与个人信息 一节）。

- POST /auth/login        登录换 token
- GET  /users/me          当前用户
- PUT  /users/me          改显示名 / 头像（multipart）
- PUT  /users/me/password 改密码
- GET  /users/{id}/avatar 头像图（v0.4 补充端点）
"""

from pathlib import Path

from fastapi import APIRouter, Depends, File, Form, HTTPException, UploadFile
from fastapi.responses import FileResponse
from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from ..config import AVATAR_DIR, AVATAR_EXTS, MAX_AVATAR_SIZE
from ..deps import get_current_user, get_db
from ..models import User
from ..schemas import LoginIn, PasswordIn, UserOut
from ..security import create_token, hash_password, verify_password

router = APIRouter(tags=["auth"])


@router.post("/auth/login")
async def login(body: LoginIn, db: AsyncSession = Depends(get_db)):
    """账号密码 → {token, user}。

    用户不存在与密码错误报同一句话，不泄露"这个账号是否存在"。
    """
    user = (
        await db.execute(select(User).where(User.username == body.username))
    ).scalar_one_or_none()
    if user is None or not verify_password(body.password, user.password_hash):
        raise HTTPException(401, "用户名或密码错误")
    if user.status != "active":
        raise HTTPException(403, "账号已停用，请联系管理员")
    return {"token": create_token(user.id), "user": UserOut.model_validate(user)}


@router.get("/users/me")
async def read_me(me: User = Depends(get_current_user)):
    return UserOut.model_validate(me)


@router.put("/users/me")
async def update_me(
    display_name: str | None = Form(default=None),
    avatar: UploadFile | None = File(default=None),
    me: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
):
    """改显示名和/或头像（multipart，两个字段都可只传其一）。

    头像落盘 data/avatars/user_{id}.{ext}，同名直接覆盖；
    换了扩展名时清掉旧文件，避免同一用户多份头像残留。
    """
    if display_name is not None:
        if not display_name.strip():
            raise HTTPException(400, "显示名不能为空")
        me.display_name = display_name.strip()

    if avatar is not None:
        ext = Path(avatar.filename or "").suffix.lower()
        if ext not in AVATAR_EXTS:
            raise HTTPException(400, f"头像仅支持：{' '.join(sorted(AVATAR_EXTS))}")
        data = await avatar.read()
        if len(data) > MAX_AVATAR_SIZE:
            raise HTTPException(400, "头像不能超过 5MB")
        path = AVATAR_DIR / f"user_{me.id}{ext}"
        path.write_bytes(data)  # ponytail: 同步写盘，5MB 内阻塞事件循环可忽略
        for old in AVATAR_DIR.glob(f"user_{me.id}.*"):  # 清掉旧扩展名的残留
            if old != path:
                old.unlink(missing_ok=True)
        me.avatar_path = str(path)

    await db.commit()
    return UserOut.model_validate(me)


@router.put("/users/me/password")
async def change_password(
    body: PasswordIn,
    me: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
):
    if not verify_password(body.old_password, me.password_hash):
        raise HTTPException(400, "原密码不正确")
    me.password_hash = hash_password(body.new_password)
    await db.commit()
    # ponytail: 改密不吊销旧 token（无黑名单），7 天自然过期；客户端引导重登即可
    return {"ok": True}


@router.get("/users/{user_id}/avatar")
async def get_avatar(
    user_id: int,
    me: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
):
    """头像图片。登录即可访问（聊天/通讯录要显示任意人的头像）。"""
    user = await db.get(User, user_id)
    if user is None or not user.avatar_path or not Path(user.avatar_path).is_file():
        raise HTTPException(404, "无头像")
    return FileResponse(user.avatar_path)
