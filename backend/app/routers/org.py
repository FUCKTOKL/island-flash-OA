"""组织架构（契约：组织架构 一节）。

- GET  /departments           部门树 + 每部门成员（选人、通讯录共用；登录即可）
- POST/PUT/DELETE /admin/departments  部门管理（admin）
- GET/POST/PUT/DELETE /admin/users    用户管理（admin）
"""

from fastapi import APIRouter, Depends, HTTPException
from sqlalchemy import func, select, update
from sqlalchemy.ext.asyncio import AsyncSession

from ..deps import get_current_user, get_db, require_admin
from ..models import (
    Approval,
    ApprovalNode,
    ApprovalRecord,
    ApprovalTemplate,
    Conversation,
    Department,
    Favorite,
    File,
    Message,
    Notification,
    Todo,
    User,
)
from ..schemas import DeptIn, DeptUpdate, UserAdminUpdate, UserCreateIn, UserOut
from ..security import hash_password

router = APIRouter(tags=["org"])


@router.get("/departments")
async def list_departments(
    me: User = Depends(get_current_user), db: AsyncSession = Depends(get_db)
):
    """部门树（森林）。无部门的用户不出现在树里（管理页走 /admin/users）。"""
    depts = (
        (await db.execute(select(Department).order_by(Department.id))).scalars().all()
    )
    users = (await db.execute(select(User).order_by(User.id))).scalars().all()

    nodes: dict[int, dict] = {}
    roots: list[dict] = []
    for d in depts:
        nodes[d.id] = {
            "id": d.id,
            "name": d.name,
            "parent_id": d.parent_id,
            "leader_id": d.leader_id,
            "members": [],
            "children": [],
        }
    for d in depts:  # 挂树：父不存在（或顶级）都进 roots，坏数据也不崩
        node = nodes[d.id]
        parent = nodes.get(d.parent_id) if d.parent_id is not None else None
        (parent["children"] if parent else roots).append(node)
    for u in users:
        if u.department_id in nodes:
            nodes[u.department_id]["members"].append(
                {
                    "id": u.id,
                    "username": u.username,
                    "display_name": u.display_name,
                    "role": u.role,
                    "status": u.status,
                    "avatar_path": u.avatar_path,
                }
            )
    return roots


# ---------- 部门管理（admin） ----------


@router.post("/admin/departments")
async def create_department(
    body: DeptIn, me: User = Depends(require_admin), db: AsyncSession = Depends(get_db)
):
    if body.parent_id is not None and await db.get(Department, body.parent_id) is None:
        raise HTTPException(400, "父部门不存在")
    dept = Department(
        name=body.name.strip(), parent_id=body.parent_id, leader_id=body.leader_id
    )
    db.add(dept)
    await db.commit()
    return {"id": dept.id}


@router.put("/admin/departments/{dept_id}")
async def update_department(
    dept_id: int,
    body: DeptUpdate,
    me: User = Depends(require_admin),
    db: AsyncSession = Depends(get_db),
):
    dept = await db.get(Department, dept_id)
    if dept is None:
        raise HTTPException(404, "部门不存在")
    data = body.model_dump(exclude_unset=True)  # 只改传了的字段

    if "name" in data:
        if not (data["name"] or "").strip():
            raise HTTPException(400, "部门名不能为空")
        dept.name = data["name"].strip()

    if "parent_id" in data:
        new_parent = data["parent_id"]  # 传 null = 改为顶级
        if new_parent is not None:
            if new_parent == dept.id:
                raise HTTPException(400, "父部门不能是自己")
            p = await db.get(Department, new_parent)
            if p is None:
                raise HTTPException(400, "父部门不存在")
            # 沿父链向上走，防止把自己挂到自己的子孙下面形成环
            seen = {dept.id}
            while p is not None:
                if p.id in seen:
                    raise HTTPException(400, "不能形成循环层级")
                seen.add(p.id)
                p = (
                    await db.get(Department, p.parent_id)
                    if p.parent_id is not None
                    else None
                )
        dept.parent_id = new_parent

    if "leader_id" in data:
        dept.leader_id = data["leader_id"]  # null = 取消领导

    await db.commit()
    return {"ok": True}


@router.delete("/admin/departments/{dept_id}")
async def delete_department(
    dept_id: int, me: User = Depends(require_admin), db: AsyncSession = Depends(get_db)
):
    dept = await db.get(Department, dept_id)
    if dept is None:
        raise HTTPException(404, "部门不存在")
    n_members = (
        await db.execute(
            select(func.count()).select_from(User).where(User.department_id == dept_id)
        )
    ).scalar()
    if n_members:
        raise HTTPException(400, f"该部门仍有 {n_members} 名成员，请先转移成员")
    n_children = (
        await db.execute(
            select(func.count())
            .select_from(Department)
            .where(Department.parent_id == dept_id)
        )
    ).scalar()
    if n_children:
        raise HTTPException(400, "该部门下有子部门，请先删除子部门")
    await db.delete(dept)
    await db.commit()
    return {"ok": True}


# ---------- 用户管理（admin） ----------


@router.get("/admin/users")
async def admin_list_users(
    me: User = Depends(require_admin), db: AsyncSession = Depends(get_db)
):
    """用户管理列表（含停用账号；v0.4 补充端点）。"""
    rows = (await db.execute(select(User).order_by(User.id))).scalars().all()
    return [UserOut.model_validate(u) for u in rows]


@router.post("/admin/users")
async def admin_create_user(
    body: UserCreateIn,
    me: User = Depends(require_admin),
    db: AsyncSession = Depends(get_db),
):
    if body.role not in ("admin", "member"):
        raise HTTPException(400, "role 只能是 admin 或 member")
    n = (
        await db.execute(
            select(func.count()).select_from(User).where(User.username == body.username)
        )
    ).scalar()
    if n:
        raise HTTPException(400, "用户名已存在")
    if (
        body.department_id is not None
        and await db.get(Department, body.department_id) is None
    ):
        raise HTTPException(400, "部门不存在")
    user = User(
        username=body.username,
        password_hash=hash_password(body.password),
        display_name=body.display_name,
        department_id=body.department_id,
        role=body.role,
    )
    db.add(user)
    await db.commit()
    return UserOut.model_validate(user)


@router.put("/admin/users/{user_id}")
async def admin_update_user(
    user_id: int,
    body: UserAdminUpdate,
    me: User = Depends(require_admin),
    db: AsyncSession = Depends(get_db),
):
    user = await db.get(User, user_id)
    if user is None:
        raise HTTPException(404, "用户不存在")
    data = body.model_dump(exclude_unset=True)

    # 防自锁：管理员改自己的角色/状态 → 可能把自己踢出管理或停用自己，拒绝
    if user.id == me.id and ("role" in data or "status" in data):
        raise HTTPException(400, "不能修改自己的角色或状态")
    if "role" in data and data["role"] not in ("admin", "member"):
        raise HTTPException(400, "role 只能是 admin 或 member")
    if "status" in data and data["status"] not in ("active", "disabled"):
        raise HTTPException(400, "status 只能是 active 或 disabled")
    if (
        data.get("department_id") is not None
        and await db.get(Department, data["department_id"]) is None
    ):
        raise HTTPException(400, "部门不存在")

    if data.get("password"):  # admin 重置密码
        user.password_hash = hash_password(data.pop("password"))
    for k in ("display_name", "department_id", "role", "status"):
        if k in data:
            setattr(user, k, data[k])
    await db.commit()
    return UserOut.model_validate(user)


@router.delete("/admin/users/{user_id}")
async def admin_delete_user(
    user_id: int, me: User = Depends(require_admin), db: AsyncSession = Depends(get_db)
):
    """只有"零业务数据"的用户才能物理删除，否则提示改用停用（保留历史可追溯）。"""
    user = await db.get(User, user_id)
    if user is None:
        raise HTTPException(404, "用户不存在")
    if user.id == me.id:
        raise HTTPException(400, "不能删除自己")

    refs = [
        (File, "owner_id"),
        (Message, "sender_id"),
        (Conversation, "owner_id"),
        (Favorite, "user_id"),
        (Approval, "applicant_id"),
        (ApprovalRecord, "approver_id"),
        (ApprovalNode, "approver_id"),
        (ApprovalTemplate, "created_by"),
        (Todo, "user_id"),
        (Notification, "user_id"),
    ]
    for model, col in refs:
        n = (
            await db.execute(
                select(func.count())
                .select_from(model)
                .where(getattr(model, col) == user.id)
            )
        ).scalar()
        if n:
            raise HTTPException(
                400,
                f"该用户有业务数据（{model.__tablename__} {n} 条），不能删除，建议停用",
            )

    # 当部门领导的，先摘掉 leader 引用再删人
    await db.execute(
        update(Department).where(Department.leader_id == user.id).values(leader_id=None)
    )
    await db.delete(user)
    await db.commit()
    return {"ok": True}
