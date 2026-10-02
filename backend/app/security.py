"""密码哈希与 JWT。

- bcrypt 用官方库直接调（passlib 停更多年，不引入）
- JWT：PyJWT + HS256，payload 只放 uid 和 exp，够用且互操作简单
"""

from datetime import datetime, timedelta, timezone

import bcrypt  # pyright: ignore[reportMissingImports]  # 二进制包，辅助扫描器解析不到；运行时已验证（冒烟登录全走 checkpw）
import jwt

from .config import JWT_EXPIRE_DAYS, JWT_SECRET


def hash_password(plain: str) -> str:
    """明文 → bcrypt 哈希（自带随机盐）。"""
    return bcrypt.hashpw(plain.encode("utf-8"), bcrypt.gensalt()).decode("ascii")


def verify_password(plain: str, hashed: str) -> bool:
    """校验密码。任何异常（坏哈希/超长/空字节）一律返回 False，不抛错。"""
    try:
        return bcrypt.checkpw(plain.encode("utf-8"), hashed.encode("ascii"))
    except (ValueError, UnicodeEncodeError):
        return False


def create_token(user_id: int) -> str:
    """签发 7 天 JWT。"""
    payload = {
        "uid": user_id,
        "exp": datetime.now(timezone.utc) + timedelta(days=JWT_EXPIRE_DAYS),
    }
    return jwt.encode(payload, JWT_SECRET, algorithm="HS256")


def parse_token(token: str) -> int | None:
    """解 JWT → user_id；过期/伪造/格式错一律 None（REST 和 WS 共用）。"""
    try:
        return int(jwt.decode(token, JWT_SECRET, algorithms=["HS256"])["uid"])
    except (jwt.PyJWTError, KeyError, ValueError, TypeError):
        return None
