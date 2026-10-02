"""聊天：会话列表 / 建群 / 加人 / 消息收发 / 已读（契约：聊天 一节）。

单聊无需先建会话：POST /conversations/single/{peer_id}/messages 首条消息自动
find-or-create 一个恰好 2 成员的 type='single' 会话。
所有写操作走 REST；新消息经 WS message.new 推给会话全体成员。
"""

from fastapi import APIRouter, Depends, HTTPException
from sqlalchemy import and_, func, select, update
from sqlalchemy.ext.asyncio import AsyncSession

from ..deps import get_current_user, get_db
from ..models import Conversation, ConversationMember, Message, User
from ..models import File as FileRow
from ..schemas import GroupIn, MembersIn, MessageIn
from ..ws import manager

router = APIRouter(tags=["chat"])


# ---------- 内部工具 ----------


async def _member_ids(db: AsyncSession, conv_id: int) -> list[int]:
    rows = await db.execute(
        select(ConversationMember.user_id).where(
            ConversationMember.conversation_id == conv_id
        )
    )
    return [r[0] for r in rows]


async def _require_member(db: AsyncSession, conv_id: int, uid: int):
    n = (
        await db.execute(
            select(func.count())
            .select_from(ConversationMember)
            .where(
                ConversationMember.conversation_id == conv_id,
                ConversationMember.user_id == uid,
            )
        )
    ).scalar()
    if not n:
        raise HTTPException(403, "你不是该会话成员")


async def _users_map(db: AsyncSession, ids) -> dict[int, User]:
    """id 集合 → {id: User}，供拼 sender 信息。"""
    ids = set(ids)
    if not ids:
        return {}
    rows = await db.execute(select(User).where(User.id.in_(ids)))
    return {u.id: u for u in rows.scalars()}


def _msg_dict(m: Message, users: dict[int, User]) -> dict:
    """消息对外结构：附发送者显示名/头像，客户端免二次查人。"""
    u = users.get(m.sender_id)
    return {
        "id": m.id,
        "conversation_id": m.conversation_id,
        "sender_id": m.sender_id,
        "type": m.type,
        "content": m.content,
        "created_at": m.created_at,
        "sender_name": u.display_name if u else "（已注销）",
        "sender_avatar": u.avatar_path if u else None,
    }


async def _send_message(
    db: AsyncSession, conv: Conversation, sender: User, body: MessageIn
) -> dict:
    """插消息 + WS 推送。供"群/单聊发消息"两个端点共用。"""
    if body.type == "text":
        if not body.content.strip():
            raise HTTPException(400, "消息内容不能为空")
    else:  # image / file：content 必须是本人上传的 chat 文件 id（先 POST /files/upload 拿 id）
        if not body.content.isdigit():
            raise HTTPException(400, "图片/文件消息的 content 必须是文件 id")
        f = await db.get(FileRow, int(body.content))
        if f is None or f.scope != "chat" or f.owner_id != sender.id:
            raise HTTPException(400, "文件不存在或不属于你")

    msg = Message(
        conversation_id=conv.id,
        sender_id=sender.id,
        type=body.type,
        content=body.content,
    )
    db.add(msg)
    await db.commit()

    mids = await _member_ids(db, conv.id)
    msg_dict = _msg_dict(msg, await _users_map(db, mids))
    # WS 推送格式见契约 §三：只推给会话成员
    await manager.send_to_users(
        mids,
        {
            "type": "message.new",
            "data": {"conversation_id": conv.id, "message": msg_dict},
        },
    )
    return msg_dict


# ---------- 会话 ----------


@router.get("/conversations")
async def list_conversations(
    me: User = Depends(get_current_user), db: AsyncSession = Depends(get_db)
):
    """会话列表：含最后一条消息、未读数、单聊对方信息。按最后消息倒序。"""
    my_convs = (
        (
            await db.execute(
                select(Conversation)
                .join(
                    ConversationMember,
                    ConversationMember.conversation_id == Conversation.id,
                )
                .where(ConversationMember.user_id == me.id)
            )
        )
        .scalars()
        .all()
    )
    if not my_convs:
        return []
    conv_ids = [c.id for c in my_convs]

    # 全体成员行（拿对方/群成员信息 + 发送者名字）
    member_rows = (
        await db.execute(
            select(ConversationMember, User)
            .join(User, User.id == ConversationMember.user_id)
            .where(ConversationMember.conversation_id.in_(conv_ids))
        )
    ).all()
    users = {u.id: u for _, u in member_rows}

    # 未读数：会话里 id > 我的 last_read 且不是我发的消息条数
    unread = dict(
        (
            await db.execute(
                select(Message.conversation_id, func.count())
                .join(
                    ConversationMember,
                    and_(
                        ConversationMember.conversation_id == Message.conversation_id,
                        ConversationMember.user_id == me.id,
                    ),
                )
                .where(
                    Message.conversation_id.in_(conv_ids),
                    Message.id > ConversationMember.last_read_message_id,
                    Message.sender_id != me.id,
                )
                .group_by(Message.conversation_id)
            )
        ).all()
    )

    items = []
    for c in my_convs:
        # ponytail: 每会话单独查最后一条（N+1）；会话数 < 百无感，量大再改窗口函数
        last = (
            await db.execute(
                select(Message)
                .where(Message.conversation_id == c.id)
                .order_by(Message.id.desc())
                .limit(1)
            )
        ).scalar_one_or_none()
        peer = None
        if c.type == "single":
            peer_row = next(
                (
                    u
                    for cm, u in member_rows
                    if cm.conversation_id == c.id and u.id != me.id
                ),
                None,
            )
            peer = (
                {
                    "id": peer_row.id,
                    "display_name": peer_row.display_name,
                    "avatar_path": peer_row.avatar_path,
                }
                if peer_row
                else None
            )
        items.append(
            {
                "id": c.id,
                "type": c.type,
                "name": c.name,
                "owner_id": c.owner_id,
                "peer": peer,
                "last_message": _msg_dict(last, users) if last else None,
                "unread": unread.get(c.id, 0),
            }
        )
    items.sort(
        key=lambda x: x["last_message"]["id"] if x["last_message"] else 0, reverse=True
    )
    return items


@router.post("/conversations")
async def create_group(
    body: GroupIn,
    me: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
):
    """建群：name + 成员 id 列表（创建者自动是成员和群主）。"""
    if not body.name.strip():
        raise HTTPException(400, "群名不能为空")
    ids = set(body.member_ids) | {me.id}
    users = (
        (
            await db.execute(
                select(User).where(User.id.in_(ids), User.status == "active")
            )
        )
        .scalars()
        .all()
    )
    if len(users) != len(ids):
        raise HTTPException(400, "部分成员不存在或已停用")
    conv = Conversation(type="group", name=body.name.strip(), owner_id=me.id)
    db.add(conv)
    await db.flush()  # 拿自增 id
    db.add_all(
        [ConversationMember(conversation_id=conv.id, user_id=u.id) for u in users]
    )
    await db.commit()
    return {
        "id": conv.id,
        "type": conv.type,
        "name": conv.name,
        "owner_id": conv.owner_id,
    }


@router.post("/conversations/{conv_id}/members")
async def add_members(
    conv_id: int,
    body: MembersIn,
    me: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
):
    """加人（仅群主）。已有的跳过。"""
    conv = await db.get(Conversation, conv_id)
    if conv is None:
        raise HTTPException(404, "会话不存在")
    if conv.type != "group":
        raise HTTPException(400, "单聊不能加人")
    if conv.owner_id != me.id:
        raise HTTPException(403, "仅群主可加人")
    existing = set(await _member_ids(db, conv_id))
    new_ids = {uid for uid in body.user_ids if uid not in existing}
    if new_ids:
        users = (
            (
                await db.execute(
                    select(User).where(User.id.in_(new_ids), User.status == "active")
                )
            )
            .scalars()
            .all()
        )
        db.add_all(
            [ConversationMember(conversation_id=conv_id, user_id=u.id) for u in users]
        )
        await db.commit()
        return {"added": [u.id for u in users]}
    return {"added": []}


# ---------- 消息 ----------


@router.get("/conversations/{conv_id}/messages")
async def list_messages(
    conv_id: int,
    before_id: int | None = None,
    limit: int = 50,
    me: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
):
    """向上翻页：传当前最旧一条的 id 作 before_id。返回正序（旧→新）。"""
    await _require_member(db, conv_id, me.id)
    q = select(Message).where(Message.conversation_id == conv_id)
    if before_id:
        q = q.where(Message.id < before_id)
    rows = (
        (await db.execute(q.order_by(Message.id.desc()).limit(max(1, min(limit, 100)))))
        .scalars()
        .all()
    )
    rows.reverse()  # 查询取的是最新 N 条倒序，翻转为时间正序
    users = await _users_map(db, {m.sender_id for m in rows})
    return [_msg_dict(m, users) for m in rows]


@router.post("/conversations/{conv_id}/messages")
async def send_message(
    conv_id: int,
    body: MessageIn,
    me: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
):
    """群聊/已有单聊会话发消息。"""
    conv = await db.get(Conversation, conv_id)
    if conv is None:
        raise HTTPException(404, "会话不存在")
    await _require_member(db, conv_id, me.id)
    return await _send_message(db, conv, me, body)


@router.post("/conversations/single/{peer_id}/messages")
async def send_single(
    peer_id: int,
    body: MessageIn,
    me: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
):
    """单聊发消息（会话不存在则自动建）。客户端永远走这个入口，不用先建会话。"""
    peer = await db.get(User, peer_id)
    if peer is None or peer.status != "active":
        raise HTTPException(404, "对方不存在或已停用")
    if peer.id == me.id:
        raise HTTPException(400, "不能给自己发单聊")

    # find：双方共同且恰好 2 成员的 single 会话
    conv_id = (
        await db.execute(
            select(Conversation.id)
            .join(
                ConversationMember,
                ConversationMember.conversation_id == Conversation.id,
            )
            .where(
                Conversation.type == "single",
                ConversationMember.user_id.in_([me.id, peer.id]),
            )
            .group_by(Conversation.id)
            .having(func.count(func.distinct(ConversationMember.user_id)) == 2)
            .limit(1)
        )
    ).scalar_one_or_none()

    if conv_id is None:  # or create
        conv = Conversation(type="single")
        db.add(conv)
        await db.flush()
        db.add_all(
            [
                ConversationMember(conversation_id=conv.id, user_id=me.id),
                ConversationMember(conversation_id=conv.id, user_id=peer.id),
            ]
        )
        await db.commit()
    else:
        conv = await db.get(Conversation, conv_id)
    return await _send_message(db, conv, me, body)


@router.put("/conversations/{conv_id}/read")
async def mark_read(
    conv_id: int,
    me: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
):
    """标记已读：直接取会话最新消息 id（body 为空，契约约定）。"""
    await _require_member(db, conv_id, me.id)
    last = (
        await db.execute(
            select(func.max(Message.id)).where(Message.conversation_id == conv_id)
        )
    ).scalar() or 0
    await db.execute(
        update(ConversationMember)
        .where(
            ConversationMember.conversation_id == conv_id,
            ConversationMember.user_id == me.id,
        )
        .values(last_read_message_id=last)
    )
    await db.commit()
    return {"last_read_message_id": last}
