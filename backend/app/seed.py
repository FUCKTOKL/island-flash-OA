"""首启初始化：建表 + 默认部门/管理员。

幂等：每次启动都跑，已有数据不碰。
无 Alembic —— 改表结构 = 手写 SQL 或删库重来（内网工具，可接受）。
"""

from sqlalchemy import func, select

from .database import Base, SessionLocal, engine
from .models import Department, User
from .security import hash_password


async def init_db():
    # 1. 建表（存在则跳过，SQLite CREATE TABLE IF NOT EXISTS 语义）
    async with engine.begin() as conn:
        await conn.run_sync(Base.metadata.create_all)

    # 2. 空库才播种：部门「总部」+ 管理员 admin/admin123
    async with SessionLocal() as db:
        n = (await db.execute(select(func.count()).select_from(User))).scalar()
        if n == 0:
            dept = Department(name="总部")
            db.add(dept)
            await db.flush()  # 拿自增 id
            db.add(
                User(
                    username="admin",
                    password_hash=hash_password("admin123"),
                    display_name="管理员",
                    department_id=dept.id,
                    role="admin",
                )
            )
            await db.commit()
            print(
                "[IFOA] 首次启动：已创建部门「总部」与管理员 admin / admin123，请尽快修改密码"
            )
