"""数据库引擎、会话工厂、Declarative Base。

- SQLite + WAL 模式：内网小团队（<100 人）的读写并发足够，单文件零运维
- 全程 async engine；expire_on_commit=False 让 commit 后仍可直接读 ORM 属性
  （响应模型直接读属性的最省事用法，避免异步下的懒加载炸雷）
- 不用 Alembic：小项目建表靠 create_all（见 seed.py）；将来要改表结构再引入
"""

from sqlalchemy import event
from sqlalchemy.ext.asyncio import async_sessionmaker, create_async_engine
from sqlalchemy.orm import DeclarativeBase

from .config import DATABASE_URL

engine = create_async_engine(DATABASE_URL)

# commit 后对象属性不过期（配合上面注释的用法）
SessionLocal = async_sessionmaker(engine, expire_on_commit=False)


class Base(DeclarativeBase):
  """全部 ORM 模型的公共基类。"""


# 每个 SQLite 连接都执行 PRAGMA（SQLite 默认不开外键，必须手动开）
@event.listens_for(engine.sync_engine, "connect")
def _sqlite_pragmas(dbapi_conn, _):
  cursor = dbapi_conn.cursor()
  cursor.execute("PRAGMA journal_mode=WAL")  # WAL：读写不互相阻塞
  cursor.execute("PRAGMA foreign_keys=ON")  # 开外键约束
  cursor.close()
