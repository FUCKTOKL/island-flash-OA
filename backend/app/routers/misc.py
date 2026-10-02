"""待办 + 通知（契约：待办 / 通知 两节）——两个互不相干的小 CRUD，合放一个文件。"""

import json

from fastapi import APIRouter, Depends, HTTPException
from sqlalchemy import select, update
from sqlalchemy.ext.asyncio import AsyncSession

from ..deps import get_current_user, get_db
from ..models import Notification, Todo, User
from ..schemas import MarkReadIn, TodoCreateIn, TodoUpdate

router = APIRouter(tags=["todo", "notification"])


# ---------- 待办 ----------


@router.get("/todos")
async def list_todos(
    me: User = Depends(get_current_user), db: AsyncSession = Depends(get_db)
):
    """我的待办：未完成在前，新的在前。"""
    rows = (
        (
            await db.execute(
                select(Todo)
                .where(Todo.user_id == me.id)
                .order_by(Todo.done, Todo.id.desc())
            )
        )
        .scalars()
        .all()
    )
    return [
        {
            "id": t.id,
            "content": t.content,
            "done": t.done,
            "due_at": t.due_at,
            "created_at": t.created_at,
        }
        for t in rows
    ]


@router.post("/todos")
async def create_todo(
    body: TodoCreateIn,
    me: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
):
    if not body.content.strip():
        raise HTTPException(400, "待办内容不能为空")
    todo = Todo(user_id=me.id, content=body.content.strip(), due_at=body.due_at)
    db.add(todo)
    await db.commit()
    return {"id": todo.id}


@router.put("/todos/{todo_id}")
async def update_todo(
    todo_id: int,
    body: TodoUpdate,
    me: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
):
    """改任意组合字段（done/content/due_at）；due_at 传 null 清空。"""
    todo = await db.get(Todo, todo_id)
    if todo is None or todo.user_id != me.id:
        raise HTTPException(404, "待办不存在")
    for k, v in body.model_dump(exclude_unset=True).items():
        setattr(todo, k, v)
    await db.commit()
    return {"ok": True}


@router.delete("/todos/{todo_id}")
async def delete_todo(
    todo_id: int,
    me: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
):
    todo = await db.get(Todo, todo_id)
    if todo is None or todo.user_id != me.id:
        raise HTTPException(404, "待办不存在")
    await db.delete(todo)
    await db.commit()
    return {"ok": True}


# ---------- 通知 ----------


@router.get("/notifications")
async def list_notifications(
    unread: int = 0,
    me: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
):
    """通知列表；?unread=1 只看未读（胶囊角标数据源）。payload 解析成对象返回。"""
    q = select(Notification).where(Notification.user_id == me.id)
    if unread:
        q = q.where(Notification.is_read == False)  # noqa: E712
    rows = (
        (await db.execute(q.order_by(Notification.id.desc()).limit(200)))
        .scalars()
        .all()
    )
    # ponytail: 只留最近 200 条在列表端点；更多历史不做（角标场景用不到）
    return [
        {
            "id": n.id,
            "type": n.type,
            "payload": json.loads(n.payload),
            "is_read": n.is_read,
            "created_at": n.created_at,
        }
        for n in rows
    ]


@router.put("/notifications/read")
async def mark_read(
    body: MarkReadIn,
    me: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
):
    """按 id 批量标记已读（只动自己的）。"""
    if body.ids:
        await db.execute(
            update(Notification)
            .where(Notification.user_id == me.id, Notification.id.in_(body.ids))
            .values(is_read=True)
        )
        await db.commit()
    return {"ok": True}
