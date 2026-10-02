"""全局配置：路径、常量、密钥。

约定：所有可调参数集中在此，其余模块只 import，不散落魔法数字。
"""

import os
from pathlib import Path

# backend/ 根目录；运行数据统一放 backend/data/（已 gitignore）
BASE_DIR = Path(__file__).resolve().parent.parent
DATA_DIR = BASE_DIR / "data"
DB_PATH = DATA_DIR / "ifoa.db"  # SQLite 数据库文件
AVATAR_DIR = DATA_DIR / "avatars"  # 用户头像（user_{id}.{ext}）
UPLOAD_DIR = DATA_DIR / "uploads"  # 业务文件（uuid 命名，原名存 DB）

DATABASE_URL = f"sqlite+aiosqlite:///{DB_PATH}"

# JWT 配置
JWT_EXPIRE_DAYS = 7  # 契约：7 天过期，无 refresh token（v1 刻意取舍）
# ponytail: 默认密钥仅供开发；生产必须设环境变量 IFOA_JWT_SECRET，密钥泄露=任意伪造 token
JWT_SECRET = os.environ.get("IFOA_JWT_SECRET", "dev-secret-CHANGE-ME")

# 上传限制
MAX_FILE_SIZE = 200 * 1024 * 1024  # 契约：单文件上限 200MB，不分片
MAX_AVATAR_SIZE = 5 * 1024 * 1024  # 头像 5MB
AVATAR_EXTS = {".jpg", ".jpeg", ".png", ".webp", ".gif"}  # 头像允许的扩展名


def ensure_dirs() -> None:
    """启动时创建数据目录（存在则跳过）。"""
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    AVATAR_DIR.mkdir(parents=True, exist_ok=True)
    UPLOAD_DIR.mkdir(parents=True, exist_ok=True)
