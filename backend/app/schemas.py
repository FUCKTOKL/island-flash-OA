"""请求/响应模型（Pydantic v2）。

只定义"需要校验的输入"和"结构稳定的简单输出"；
会话列表、审批详情这类多表拼装数据直接返回 dict，省一层模型。
"""

from pydantic import BaseModel, ConfigDict, Field

# ---------- 认证 / 个人信息 ----------


class LoginIn(BaseModel):
    username: str
    password: str


class UserOut(BaseModel):
    """对外暴露的用户信息（永不含密码哈希）。"""

    model_config = ConfigDict(from_attributes=True)  # 允许从 ORM 对象直接构造

    id: int
    username: str
    display_name: str
    department_id: int | None
    role: str
    avatar_path: str | None
    status: str
    created_at: str


class PasswordIn(BaseModel):
    old_password: str
    new_password: str = Field(min_length=6, max_length=64)  # 与建号同规则


# ---------- 组织架构（admin） ----------


class DeptIn(BaseModel):
    name: str
    parent_id: int | None = None
    leader_id: int | None = None


class DeptUpdate(BaseModel):
    name: str | None = None
    parent_id: int | None = None
    leader_id: int | None = None


class UserCreateIn(BaseModel):
    username: str = Field(min_length=2, max_length=32)
    password: str = Field(min_length=6, max_length=64)
    display_name: str
    department_id: int | None = None
    role: str = "member"  # 'admin' | 'member'


class UserAdminUpdate(BaseModel):
    """admin 改用户：全部可选，只改传了的字段（exclude_unset）。"""

    display_name: str | None = None
    department_id: int | None = None
    role: str | None = None
    status: str | None = None
    password: str | None = Field(default=None, min_length=6, max_length=64)


# ---------- 聊天 ----------


class GroupIn(BaseModel):
    name: str
    member_ids: list[int]


class MembersIn(BaseModel):
    user_ids: list[int]


class MessageIn(BaseModel):
    type: str = "text"  # 'text' | 'image' | 'file'
    content: str = Field(max_length=100_000)  # text=正文；image/file=文件 id 字符串


# ---------- 审批 ----------


class NodeIn(BaseModel):
    """模板审批链的一级。approver_type='fixed_user' 时 approver_id 必填。"""

    name: str
    approver_type: str  # 'fixed_user' | 'dept_leader'
    approver_id: int | None = None


class TemplateIn(BaseModel):
    name: str
    form_schema: list[dict]  # [{"key","label","type","required"},...]
    nodes: list[NodeIn]  # 顺序即 seq 1..n


class ApprovalCreateIn(BaseModel):
    template_id: int
    title: str
    form_data: dict


class ActionIn(BaseModel):
    action: str  # 'approve' | 'reject'
    comment: str | None = None


# ---------- 待办 / 通知 ----------


class TodoCreateIn(BaseModel):
    content: str
    due_at: str | None = None  # ISO8601，可空


class TodoUpdate(BaseModel):
    content: str | None = None
    done: bool | None = None
    due_at: str | None = None  # 传 null 表示清空截止时间


class MarkReadIn(BaseModel):
    ids: list[int]
