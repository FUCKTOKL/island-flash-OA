"""全部 13 张表 —— 契约 docs/01 §一 的 SQLAlchemy 一比一翻译。

约定：
- 不建 ORM relationship：异步下的懒加载是坑，需要关联一律显式 join / 二次查询
- 时间列一律 TEXT（ISO8601 UTC），统一用 utcnow() 生成（契约约定）
- id 一律 INTEGER 自增主键（SQLite rowid 别名，无需显式 autoincrement）
- done / is_read 契约里是 INTEGER，这里用 bool 映射（SQLite 存 0/1，语义更清楚）
"""

from datetime import datetime, timezone

from sqlalchemy import ForeignKey, Index, Text
from sqlalchemy.orm import Mapped, mapped_column

from .database import Base


def utcnow() -> str:
    """统一时间源：ISO8601 UTC 文本。全部时间列的默认值都用它。"""
    return datetime.now(timezone.utc).isoformat()


# ---------- 1. 组织与用户 ----------


class Department(Base):
    """部门。层级靠 parent_id 自引用；leader_id 供审批流 dept_leader 节点运行时解析。"""

    __tablename__ = "departments"

    id: Mapped[int] = mapped_column(primary_key=True)
    name: Mapped[str]
    parent_id: Mapped[int | None] = mapped_column(
        ForeignKey("departments.id")
    )  # NULL=顶级
    leader_id: Mapped[int | None]  # 部门领导 user_id（不设外键，避免删人/换人纠结）


class User(Base):
    """用户。username 全局唯一；status='disabled' 不可登录但数据保留。"""

    __tablename__ = "users"

    id: Mapped[int] = mapped_column(primary_key=True)
    username: Mapped[str] = mapped_column(unique=True)
    password_hash: Mapped[str]  # bcrypt 哈希（永不明文）
    display_name: Mapped[str]  # 显示名（通讯录/聊天里用这个）
    department_id: Mapped[int | None] = mapped_column(ForeignKey("departments.id"))
    role: Mapped[str] = mapped_column(default="member")  # 'admin' | 'member'
    avatar_path: Mapped[str | None]  # 头像磁盘路径；NULL=客户端画默认头像
    status: Mapped[str] = mapped_column(default="active")  # 'active' | 'disabled'
    created_at: Mapped[str] = mapped_column(default=utcnow)


# ---------- 2. 文件（公共/群/个人/聊天图片，一张表） ----------


class File(Base):
    """文件元数据。disk_path 才是磁盘真身；name 是用户看到的名字。

    scope 权限速查（契约 v0.3）：
    - public + group_id NULL  → 全员读，admin 写
    - public + group_id 非空  → 群成员读写（非成员不可见）
    - personal → 仅 owner
    - chat     → 仅会话成员（下载时经消息反查会话校验）
    """

    __tablename__ = "files"

    id: Mapped[int] = mapped_column(primary_key=True)
    scope: Mapped[str]  # 'public' | 'personal' | 'chat'
    group_id: Mapped[int | None] = mapped_column(
        ForeignKey("conversations.id")
    )  # 仅 public 群区用
    owner_id: Mapped[int | None] = mapped_column(ForeignKey("users.id"))  # 上传者/属主
    folder: Mapped[str] = mapped_column(default="")  # 虚拟目录，'' 为根目录
    name: Mapped[str]  # 原文件名
    disk_path: Mapped[str]  # 服务器磁盘路径
    size: Mapped[int]  # 字节
    mime: Mapped[str]
    created_at: Mapped[str] = mapped_column(default=utcnow)
    deleted_at: Mapped[str | None]  # 软删除时间；NULL=未删（回收站 P1 再做）


Index(
    "idx_files_scope",
    File.__table__.c.scope,
    File.__table__.c.owner_id,
    File.__table__.c.folder,
)


class Favorite(Base):
    """收藏：工作台"喜欢的照片"= 对 personal 图片文件的收藏。复合主键防重复。"""

    __tablename__ = "favorites"

    user_id: Mapped[int] = mapped_column(ForeignKey("users.id"), primary_key=True)
    file_id: Mapped[int] = mapped_column(ForeignKey("files.id"), primary_key=True)


# ---------- 3. 聊天（单聊/群聊统一会话模型） ----------


class Conversation(Base):
    """会话。type='single' 恰好 2 成员（首发消息自动建）；group 有 name 和群主。"""

    __tablename__ = "conversations"

    id: Mapped[int] = mapped_column(primary_key=True)
    type: Mapped[str]  # 'single' | 'group'
    name: Mapped[str | None]  # 仅群聊
    owner_id: Mapped[int | None] = mapped_column(ForeignKey("users.id"))  # 群主
    created_at: Mapped[str] = mapped_column(default=utcnow)


class ConversationMember(Base):
    """会话成员。last_read_message_id 实现未读数：未读 = 会话中 id 大于它的消息条数。"""

    __tablename__ = "conversation_members"

    conversation_id: Mapped[int] = mapped_column(
        ForeignKey("conversations.id"), primary_key=True
    )
    user_id: Mapped[int] = mapped_column(ForeignKey("users.id"), primary_key=True)
    last_read_message_id: Mapped[int] = mapped_column(default=0)


class Message(Base):
    """消息。content：text=正文；image/file=files.id 的字符串。"""

    __tablename__ = "messages"

    id: Mapped[int] = mapped_column(primary_key=True)
    conversation_id: Mapped[int] = mapped_column(ForeignKey("conversations.id"))
    sender_id: Mapped[int] = mapped_column(ForeignKey("users.id"))
    type: Mapped[str]  # 'text' | 'image' | 'file'
    content: Mapped[str] = mapped_column(Text)
    created_at: Mapped[str] = mapped_column(default=utcnow)


Index("idx_messages_conv", Message.__table__.c.conversation_id, Message.__table__.c.id)


# ---------- 4. 审批流（顺序链：节点 1→2→…→n 逐级） ----------


class ApprovalTemplate(Base):
    """审批模板。form_schema 是 JSON 字符串（字段定义数组，见契约示例）。"""

    __tablename__ = "approval_templates"

    id: Mapped[int] = mapped_column(primary_key=True)
    name: Mapped[str]  # 请假 / 报销 / …
    form_schema: Mapped[str]  # JSON 数组文本
    created_by: Mapped[int | None] = mapped_column(ForeignKey("users.id"))
    created_at: Mapped[str] = mapped_column(default=utcnow)


class ApprovalNode(Base):
    """模板的审批链节点。seq 从 1 起；dept_leader 运行时解析申请人部门的 leader_id。"""

    __tablename__ = "approval_nodes"

    id: Mapped[int] = mapped_column(primary_key=True)
    template_id: Mapped[int] = mapped_column(ForeignKey("approval_templates.id"))
    seq: Mapped[int]  # 顺序号，1 起
    name: Mapped[str]  # "组长审批"
    approver_type: Mapped[str]  # 'fixed_user' | 'dept_leader'
    approver_id: Mapped[int | None] = mapped_column(
        ForeignKey("users.id")
    )  # fixed_user 必填


class Approval(Base):
    """一次审批申请。current_seq 指当前卡在哪个节点。"""

    __tablename__ = "approvals"

    id: Mapped[int] = mapped_column(primary_key=True)
    template_id: Mapped[int] = mapped_column(ForeignKey("approval_templates.id"))
    applicant_id: Mapped[int] = mapped_column(ForeignKey("users.id"))
    title: Mapped[str]
    form_data: Mapped[str]  # JSON：按 form_schema 填写的值
    status: Mapped[str] = mapped_column(
        default="pending"
    )  # pending|approved|rejected|cancelled
    current_seq: Mapped[int] = mapped_column(default=1)
    created_at: Mapped[str] = mapped_column(default=utcnow)
    finished_at: Mapped[str | None]  # 结束（通过/驳回/撤销）时间


class ApprovalRecord(Base):
    """审批动作留痕：谁在哪一级通过/驳回了什么。"""

    __tablename__ = "approval_records"

    id: Mapped[int] = mapped_column(primary_key=True)
    approval_id: Mapped[int] = mapped_column(ForeignKey("approvals.id"))
    node_seq: Mapped[int]
    approver_id: Mapped[int] = mapped_column(ForeignKey("users.id"))
    action: Mapped[str]  # 'approved' | 'rejected'
    comment: Mapped[str | None]
    created_at: Mapped[str] = mapped_column(default=utcnow)


# ---------- 5. 待办与通知 ----------


class Todo(Base):
    """个人待办。done 用 bool（SQLite 存 0/1）。"""

    __tablename__ = "todos"

    id: Mapped[int] = mapped_column(primary_key=True)
    user_id: Mapped[int] = mapped_column(ForeignKey("users.id"))
    content: Mapped[str]
    done: Mapped[bool] = mapped_column(default=False)
    due_at: Mapped[str | None]  # 截止时间，可空
    created_at: Mapped[str] = mapped_column(default=utcnow)


class Notification(Base):
    """通知。payload 是 JSON 字符串，结构随 type 而定；胶囊角标数据源。"""

    __tablename__ = "notifications"

    id: Mapped[int] = mapped_column(primary_key=True)
    user_id: Mapped[int] = mapped_column(ForeignKey("users.id"))
    type: Mapped[str]  # 'approval' | 'chat' | 'system'
    payload: Mapped[str]  # JSON 文本
    is_read: Mapped[bool] = mapped_column(default=False)
    created_at: Mapped[str] = mapped_column(default=utcnow)
