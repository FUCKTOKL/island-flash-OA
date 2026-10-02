"""文件：公共区/群区/个人空间 + 收藏（契约：文件 一节）。

权限速查（契约 v0.3）：
- public + group_id NULL  → 全员读，仅 admin 写
- public + group_id 非空  → 群成员读写（非成员不可见）
- personal → 仅 owner（列个人文件时 owner 自动=当前用户）
- chat     → 上传后拿 id 发消息；下载经消息反查会话校验成员身份
"""

import uuid
from pathlib import Path

from fastapi import APIRouter, Depends, File, Form, HTTPException, UploadFile
from fastapi.responses import FileResponse
from sqlalchemy import delete, func, select
from sqlalchemy.ext.asyncio import AsyncSession

from ..config import MAX_FILE_SIZE, UPLOAD_DIR
from ..deps import get_current_user, get_db
from ..models import ConversationMember, Favorite, Message, User, utcnow
from ..models import File as FileRow

router = APIRouter(tags=["files"])


# ---------- 内部工具 ----------


async def _is_member(db: AsyncSession, conv_id: int, uid: int) -> bool:
    """是否为指定会话（群）的成员。群区文件权限全靠它。"""
    return bool(
        (
            await db.execute(
                select(func.count())
                .select_from(ConversationMember)
                .where(
                    ConversationMember.conversation_id == conv_id,
                    ConversationMember.user_id == uid,
                )
            )
        ).scalar()
    )


def _file_dict(f: FileRow) -> dict:
    """对外文件信息。disk_path 是服务器内部路径，不外泄。"""
    return {
        "id": f.id,
        "scope": f.scope,
        "group_id": f.group_id,
        "owner_id": f.owner_id,
        "folder": f.folder,
        "name": f.name,
        "size": f.size,
        "mime": f.mime,
        "created_at": f.created_at,
    }


async def _can_access(db: AsyncSession, f: FileRow, me: User) -> bool:
    """下载权限判定（契约权限规则）。"""
    if me.role == "admin":
        return True
    if f.scope == "personal":
        return f.owner_id == me.id
    if f.scope == "public":
        return True if f.group_id is None else await _is_member(db, f.group_id, me.id)
    # chat：经消息反查会话（content 存的是文件 id 字符串），会话成员即可取
    conv_id = (
        await db.execute(
            select(Message.conversation_id)
            .where(Message.type.in_(("image", "file")), Message.content == str(f.id))
            .limit(1)
        )
    ).scalar_one_or_none()
    return conv_id is not None and await _is_member(db, conv_id, me.id)


# ---------- 端点 ----------


@router.get("/files")
async def list_files(
    scope: str = "personal",
    folder: str = "",
    group_id: int | None = None,
    me: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
):
    """文件列表。scope=personal 时 owner 自动取当前用户（客户端不用传）。"""
    q = select(FileRow).where(FileRow.deleted_at.is_(None))
    if scope == "personal":
        q = q.where(FileRow.scope == "personal", FileRow.owner_id == me.id)
    elif scope == "public":
        if group_id:  # 群区：非成员不可见
            await _is_member_or_403(db, group_id, me.id)
            q = q.where(FileRow.scope == "public", FileRow.group_id == group_id)
        else:  # 全员公共区：登录即可看
            q = q.where(FileRow.scope == "public", FileRow.group_id.is_(None))
    else:
        raise HTTPException(400, "scope 只能是 public 或 personal")
    rows = (
        (
            await db.execute(
                q.where(FileRow.folder == folder).order_by(FileRow.id.desc())
            )
        )
        .scalars()
        .all()
    )
    return [_file_dict(f) for f in rows]


async def _is_member_or_403(db: AsyncSession, conv_id: int, uid: int):
    if not await _is_member(db, conv_id, uid):
        raise HTTPException(403, "你不是该群成员，无权访问群文件")


@router.post("/files/upload")
async def upload_file(
    file: UploadFile = File(...),
    scope: str = Form(...),
    folder: str = Form(default=""),
    group_id: int | None = Form(default=None),
    me: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
):
    """上传。multipart 字段：file, scope, folder, group_id。

    磁盘文件名用 uuid（防重名/防路径穿越），用户看到的名字存 DB 的 name 列。
    """
    if scope == "public":
        if group_id:
            await _is_member_or_403(db, group_id, me.id)  # 群区：群成员可写
        elif me.role != "admin":
            raise HTTPException(403, "全员公共区仅管理员可上传")  # 全员区：admin 写
    elif scope not in ("personal", "chat"):
        raise HTTPException(400, "scope 只能是 public/personal/chat")

    data = await file.read()
    if len(data) > MAX_FILE_SIZE:
        raise HTTPException(400, "文件超过 200MB 上限")
    if not data:
        raise HTTPException(400, "空文件")

    ext = Path(file.filename or "").suffix  # 保留原扩展名；mime 用客户端报的类型
    disk_path = UPLOAD_DIR / f"{uuid.uuid4().hex}{ext}"
    disk_path.write_bytes(
        data
    )  # ponytail: 整读整写；UploadFile 本身已 spool 到临时盘，200MB 峰值可接受

    row = FileRow(
        scope=scope,
        group_id=group_id if scope == "public" else None,  # group_id 仅 public 群区有效
        owner_id=me.id,
        folder=folder,
        name=file.filename or disk_path.name,
        disk_path=str(disk_path),
        size=len(data),
        mime=file.content_type or "application/octet-stream",
    )
    db.add(row)
    await db.commit()
    return _file_dict(row)


@router.get("/files/{file_id}/download")
async def download_file(
    file_id: int,
    me: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
):
    """下载/预览。FileResponse 会带原文件名（name 列）。"""
    f = await db.get(FileRow, file_id)
    if f is None or f.deleted_at is not None:
        raise HTTPException(404, "文件不存在")
    if not await _can_access(db, f, me):
        raise HTTPException(403, "无权访问该文件")
    if not Path(f.disk_path).is_file():
        raise HTTPException(
            404, "文件已丢失"
        )  # 数据库有记录但磁盘没了（人工误删 data/）
    return FileResponse(f.disk_path, filename=f.name, media_type=f.mime)


@router.delete("/files/{file_id}")
async def delete_file(
    file_id: int,
    me: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
):
    """软删除（deleted_at 打时间戳），owner 或 admin 可删。回收站 P1 再做。"""
    f = await db.get(FileRow, file_id)
    if f is None or f.deleted_at is not None:
        raise HTTPException(404, "文件不存在")
    if f.owner_id != me.id and me.role != "admin":
        raise HTTPException(403, "只有上传者或管理员可删除")
    f.deleted_at = utcnow()
    await db.commit()
    return {"ok": True}


# ---------- 收藏 ----------


@router.post("/files/{file_id}/favorite")
async def add_favorite(
    file_id: int,
    me: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
):
    f = await db.get(FileRow, file_id)
    if f is None or f.deleted_at is not None:
        raise HTTPException(404, "文件不存在")
    db.add(Favorite(user_id=me.id, file_id=file_id))
    await db.commit()
    return {"ok": True}


@router.delete("/files/{file_id}/favorite")
async def remove_favorite(
    file_id: int,
    me: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
):
    await db.execute(
        delete(Favorite).where(Favorite.user_id == me.id, Favorite.file_id == file_id)
    )
    await db.commit()
    return {"ok": True}


@router.get("/favorites")
async def list_favorites(
    me: User = Depends(get_current_user), db: AsyncSession = Depends(get_db)
):
    """收藏列表 = 工作台"喜欢的照片"数据源，按 mime=image 过滤。"""
    rows = (
        (
            await db.execute(
                select(FileRow)
                .join(Favorite, Favorite.file_id == FileRow.id)
                .where(
                    Favorite.user_id == me.id,
                    FileRow.deleted_at.is_(None),
                    FileRow.mime.like("image/%"),
                )
                .order_by(FileRow.id.desc())
            )
        )
        .scalars()
        .all()
    )
    return [_file_dict(f) for f in rows]
