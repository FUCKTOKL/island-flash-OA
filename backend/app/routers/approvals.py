"""审批：模板 / 发起 / 三个盒子 / 详情 / 动作 / 撤销 / 模板管理（契约：审批 一节）。

顺序链规则：节点 seq 1→n 逐级；approve 推进 current_seq，最后一级通过 → approved；
任意一级 reject → rejected。dept_leader 节点运行时解析申请人所在部门的 leader_id
（换领导后进行中的单子自动跟新领导走，契约取舍 #5）。
"""

import json

from fastapi import APIRouter, Depends, HTTPException
from sqlalchemy import and_, delete, func, or_, select
from sqlalchemy.ext.asyncio import AsyncSession

from ..deps import get_current_user, get_db, require_admin
from ..models import (
    Approval,
    ApprovalNode,
    ApprovalRecord,
    ApprovalTemplate,
    Department,
    Notification,
    User,
    utcnow,
)
from ..schemas import ActionIn, ApprovalCreateIn, NodeIn, TemplateIn
from ..ws import manager

router = APIRouter(tags=["approvals"])


def _jload(s: str, default):
    """解析本服务写入的 JSON 列（form_schema/form_data）。坏行回退默认值而非 500。"""
    try:
        return json.loads(s)
    except (ValueError, TypeError):
        return default


# ---------- 内部工具 ----------


async def _get_node(
    db: AsyncSession, template_id: int, seq: int
) -> ApprovalNode | None:
    """取模板指定顺序的节点。"""
    return (
        await db.execute(
            select(ApprovalNode).where(
                ApprovalNode.template_id == template_id, ApprovalNode.seq == seq
            )
        )
    ).scalar_one_or_none()


async def _current_approver_uids(db: AsyncSession, ap: Approval) -> list[int]:
    """当前节点（seq=current_seq）审批人 id。

    dept_leader 解析申请人部门的 leader_id；申请人无部门或部门无领导 → 空列表
    （流程卡住，等管理员设好领导后单子自动恢复流转，无需迁移数据）。
    """
    node = await _get_node(db, ap.template_id, ap.current_seq)
    if node is None:
        return []
    if node.approver_type == "fixed_user":
        # 建模板时已校验 fixed_user 必带 approver_id；类型上可空，防御一下
        return [node.approver_id] if node.approver_id else []
    leader_id = (
        await db.execute(
            select(Department.leader_id)
            .join(User, User.department_id == Department.id)
            .where(User.id == ap.applicant_id)
        )
    ).scalar_one_or_none()
    if leader_id:
        return [leader_id]
    return []  # 申请人无部门/部门无领导 → 流程卡住（等设好领导自动恢复）


async def _unread_total(db: AsyncSession, uid: int) -> int:
    return (
        await db.execute(
            select(func.count())
            .select_from(Notification)
            .where(Notification.user_id == uid, Notification.is_read == False)
        )
    ).scalar()  # noqa: E712


async def _notify(db: AsyncSession, uid: int, ntype: str, payload: dict):
    """落一条通知 + 在线即时推送（WS notification.new）。离线者登录后拉列表补看。"""
    db.add(
        Notification(
            user_id=uid, type=ntype, payload=json.dumps(payload, ensure_ascii=False)
        )
    )
    await db.commit()
    await manager.send_to_user(
        uid,
        {
            "type": "notification.new",
            "data": {
                "unread_total": await _unread_total(db, uid),
                "notification": {"type": ntype, "payload": payload},
            },
        },
    )


async def _users_map(db: AsyncSession, ids) -> dict[int, User]:
    ids = set(ids)
    if not ids:
        return {}
    rows = await db.execute(select(User).where(User.id.in_(ids)))
    return {u.id: u for u in rows.scalars()}


def _node_dict(n: ApprovalNode, users: dict[int, User]) -> dict:
    """审批链节点。dept_leader 的 approver_name 显示"部门领导"占位，客户端按申请人解析。"""
    u = users.get(n.approver_id) if n.approver_id else None
    return {
        "seq": n.seq,
        "name": n.name,
        "approver_type": n.approver_type,
        "approver_id": n.approver_id,
        "approver_name": u.display_name if u else "部门领导",
    }


def _mini_user(users: dict[int, User], uid: int | None) -> dict | None:
    u = users.get(uid) if uid else None
    return (
        {"id": u.id, "display_name": u.display_name, "avatar_path": u.avatar_path}
        if u
        else None
    )


# ---------- 模板（发起页用，登录即可） ----------


@router.get("/approval/templates")
async def list_templates(
    me: User = Depends(get_current_user), db: AsyncSession = Depends(get_db)
):
    """模板列表 + 审批链。"""
    tpls = (
        (await db.execute(select(ApprovalTemplate).order_by(ApprovalTemplate.id)))
        .scalars()
        .all()
    )
    nodes = (
        (
            await db.execute(
                select(ApprovalNode).order_by(
                    ApprovalNode.template_id, ApprovalNode.seq
                )
            )
        )
        .scalars()
        .all()
    )
    by_tpl: dict[int, list[ApprovalNode]] = {}
    for n in nodes:
        by_tpl.setdefault(n.template_id, []).append(n)
    users = await _users_map(db, {n.approver_id for n in nodes if n.approver_id})
    return [
        {
            "id": t.id,
            "name": t.name,
            "form_schema": _jload(t.form_schema, []),
            "nodes": [_node_dict(n, users) for n in by_tpl.get(t.id, [])],
        }
        for t in tpls
    ]


# ---------- 发起 / 查询 ----------


@router.post("/approvals")
async def create_approval(
    body: ApprovalCreateIn,
    me: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
):
    """发起审批：按模板校验必填项 → 落库（current_seq=1）→ 提醒当前审批人。"""
    tpl = await db.get(ApprovalTemplate, body.template_id)
    if tpl is None:
        raise HTTPException(404, "模板不存在")
    if not (body.title or "").strip():
        raise HTTPException(400, "标题不能为空")
    schema = _jload(tpl.form_schema, [])
    missing = [
        f["label"]
        for f in schema
        if f.get("required") and not body.form_data.get(f["key"])
    ]
    if missing:
        raise HTTPException(400, f"缺少必填项：{'、'.join(missing)}")

    ap = Approval(
        template_id=tpl.id,
        applicant_id=me.id,
        title=body.title.strip(),
        form_data=json.dumps(body.form_data, ensure_ascii=False),
        status="pending",
        current_seq=1,
    )
    db.add(ap)
    await db.commit()
    # 在线的当前审批人即时提醒；"待我审批 N 条"卡片数据本身走轮询
    await manager.send_to_users(
        await _current_approver_uids(db, ap),
        {
            "type": "approval.update",
            "data": {
                "approval_id": ap.id,
                "status": "pending",
                "actor": me.display_name,
            },
        },
    )
    return {"id": ap.id, "status": ap.status, "current_seq": ap.current_seq}


@router.get("/approvals")
async def list_approvals(
    box: str = "sent",
    me: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
):
    """box：todo=待我审批 / done=我已审批 / sent=我发起的。"""
    if box == "todo":
        # dept_leader 相关子查询：申请人所在部门的 leader_id（契约"待我审批"查询）
        leader_sub = (
            select(Department.leader_id)
            .join(User, User.department_id == Department.id)
            .where(User.id == Approval.applicant_id)
            .scalar_subquery()
        )
        q = (
            select(Approval)
            .join(
                ApprovalNode,
                and_(
                    ApprovalNode.template_id == Approval.template_id,
                    ApprovalNode.seq == Approval.current_seq,
                ),
            )
            .where(
                Approval.status == "pending",
                or_(
                    and_(
                        ApprovalNode.approver_type == "fixed_user",
                        ApprovalNode.approver_id == me.id,
                    ),
                    and_(
                        ApprovalNode.approver_type == "dept_leader", leader_sub == me.id
                    ),
                ),
            )
        )
    elif box == "done":
        q = (
            select(Approval)
            .join(ApprovalRecord, ApprovalRecord.approval_id == Approval.id)
            .where(ApprovalRecord.approver_id == me.id)
        )
    elif box == "sent":
        q = select(Approval).where(Approval.applicant_id == me.id)
    else:
        raise HTTPException(400, "box 只能是 todo/done/sent")

    rows = (await db.execute(q.order_by(Approval.id.desc()))).scalars().unique().all()

    # 附模板名 / 当前节点名 / 申请人名（列表直接可显示）
    tpl_ids = {ap.template_id for ap in rows}
    tpls = (
        {
            t.id: t
            for t in (
                await db.execute(
                    select(ApprovalTemplate).where(ApprovalTemplate.id.in_(tpl_ids))
                )
            ).scalars()
        }
        if tpl_ids
        else {}
    )
    users = await _users_map(db, {ap.applicant_id for ap in rows})
    out = []
    for ap in rows:
        node = await _get_node(db, ap.template_id, ap.current_seq)
        out.append(
            {
                "id": ap.id,
                "title": ap.title,
                "status": ap.status,
                "template_name": tpls[ap.template_id].name
                if ap.template_id in tpls
                else "",
                "current_node": node.name if node else "",
                "applicant_name": users[ap.applicant_id].display_name
                if ap.applicant_id in users
                else "",
                "created_at": ap.created_at,
                "finished_at": ap.finished_at,
            }
        )
    return out


@router.get("/approvals/{approval_id}")
async def approval_detail(
    approval_id: int,
    me: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
):
    """审批详情（v0.4 补充端点）：表单数据 + 审批链 + 留痕。

    可见性：申请人 / 曾参与审批的人 / 当前节点审批人。
    """
    ap = await db.get(Approval, approval_id)
    if ap is None:
        raise HTTPException(404, "审批不存在")
    records = (
        (
            await db.execute(
                select(ApprovalRecord)
                .where(ApprovalRecord.approval_id == ap.id)
                .order_by(ApprovalRecord.id)
            )
        )
        .scalars()
        .all()
    )
    if (
        me.id != ap.applicant_id
        and me.id not in {r.approver_id for r in records}
        and me.id not in await _current_approver_uids(db, ap)
    ):
        raise HTTPException(403, "无权查看该审批")

    tpl = await db.get(ApprovalTemplate, ap.template_id)
    if tpl is None:  # 模板被引用时禁止删除，正常不会发生；坏数据防御
        raise HTTPException(404, "模板不存在")
    nodes = (
        (
            await db.execute(
                select(ApprovalNode)
                .where(ApprovalNode.template_id == ap.template_id)
                .order_by(ApprovalNode.seq)
            )
        )
        .scalars()
        .all()
    )
    users = await _users_map(
        db,
        {ap.applicant_id}
        | {n.approver_id for n in nodes if n.approver_id}
        | {r.approver_id for r in records},
    )
    return {
        "id": ap.id,
        "title": ap.title,
        "status": ap.status,
        "current_seq": ap.current_seq,
        "form_data": _jload(ap.form_data, {}),
        "created_at": ap.created_at,
        "finished_at": ap.finished_at,
        "template": {
            "id": tpl.id,
            "name": tpl.name,
            "form_schema": _jload(tpl.form_schema, []),
        },
        "applicant": _mini_user(users, ap.applicant_id),
        "nodes": [_node_dict(n, users) for n in nodes],
        "records": [
            {
                "node_seq": r.node_seq,
                "approver": _mini_user(users, r.approver_id),
                "action": r.action,
                "comment": r.comment,
                "created_at": r.created_at,
            }
            for r in records
        ],
    }


# ---------- 动作 / 撤销 ----------


@router.post("/approvals/{approval_id}/action")
async def act(
    approval_id: int,
    body: ActionIn,
    me: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
):
    """审批动作：仅当前节点审批人。approve 推进一级（末级→approved），reject 直接 rejected。"""
    if body.action not in ("approve", "reject"):
        raise HTTPException(400, "action 只能是 approve 或 reject")
    ap = await db.get(Approval, approval_id)
    if ap is None:
        raise HTTPException(404, "审批不存在")
    if ap.status != "pending":
        raise HTTPException(400, "该审批已结束")
    if me.id not in await _current_approver_uids(db, ap):
        raise HTTPException(403, "你不是当前节点审批人")

    max_seq = (
        await db.execute(
            select(func.max(ApprovalNode.seq)).where(
                ApprovalNode.template_id == ap.template_id
            )
        )
    ).scalar() or 0
    db.add(
        ApprovalRecord(
            approval_id=ap.id,
            node_seq=ap.current_seq,
            approver_id=me.id,
            action="approved" if body.action == "approve" else "rejected",
            comment=body.comment,
        )
    )
    if body.action == "approve":
        if ap.current_seq >= max_seq:  # 审的就是最后一级 → 整单通过
            ap.status, ap.finished_at = "approved", utcnow()
        else:  # 还有下一级 → 只推进，不结束
            ap.current_seq += 1
    else:
        ap.status, ap.finished_at = "rejected", utcnow()
    await db.commit()

    # 申请人收到结果（通知落库 + WS）；通过且流转到下一级 → 提醒下一审批人
    await _notify(
        db,
        ap.applicant_id,
        "approval",
        {
            "approval_id": ap.id,
            "title": ap.title,
            "action": body.action,
            "actor": me.display_name,
            "comment": body.comment,
        },
    )
    next_uids = await _current_approver_uids(db, ap) if ap.status == "pending" else []
    await manager.send_to_users(
        next_uids + [ap.applicant_id],
        {
            "type": "approval.update",
            "data": {
                "approval_id": ap.id,
                "status": ap.status,
                "actor": me.display_name,
            },
        },
    )
    return {"status": ap.status, "current_seq": ap.current_seq}


@router.post("/approvals/{approval_id}/cancel")
async def cancel(
    approval_id: int,
    me: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
):
    """撤销：仅申请人，且 pending 状态。"""
    ap = await db.get(Approval, approval_id)
    if ap is None:
        raise HTTPException(404, "审批不存在")
    if ap.applicant_id != me.id:
        raise HTTPException(403, "只有申请人可撤销")
    if ap.status != "pending":
        raise HTTPException(400, "该审批已结束，不能撤销")
    ap.status, ap.finished_at = "cancelled", utcnow()
    await db.commit()
    await manager.send_to_user(
        ap.applicant_id,
        {
            "type": "approval.update",
            "data": {
                "approval_id": ap.id,
                "status": "cancelled",
                "actor": me.display_name,
            },
        },
    )
    return {"ok": True}


# ---------- 模板管理（admin） ----------


def _validate_nodes(nodes: list[NodeIn]):
    if not nodes:
        raise HTTPException(400, "至少需要一级审批节点")
    for n in nodes:
        if n.approver_type not in ("fixed_user", "dept_leader"):
            raise HTTPException(400, "approver_type 只能是 fixed_user 或 dept_leader")
        if n.approver_type == "fixed_user" and not n.approver_id:
            raise HTTPException(400, f"节点「{n.name}」为指定人审批，缺 approver_id")
        if not (n.name or "").strip():
            raise HTTPException(400, "节点名不能为空")


async def _insert_nodes(db: AsyncSession, tpl_id: int, nodes: list[NodeIn]):
    """按数组顺序写 seq=1..n。"""
    for i, n in enumerate(nodes, start=1):
        db.add(
            ApprovalNode(
                template_id=tpl_id,
                seq=i,
                name=n.name.strip(),
                approver_type=n.approver_type,
                approver_id=n.approver_id,
            )
        )


@router.post("/admin/approval/templates")
async def create_template(
    body: TemplateIn,
    me: User = Depends(require_admin),
    db: AsyncSession = Depends(get_db),
):
    _validate_nodes(body.nodes)
    if not body.form_schema:
        raise HTTPException(400, "表单 schema 不能为空")
    tpl = ApprovalTemplate(
        name=body.name.strip(),
        form_schema=json.dumps(body.form_schema, ensure_ascii=False),
        created_by=me.id,
    )
    db.add(tpl)
    await db.flush()  # 拿自增 id
    await _insert_nodes(db, tpl.id, body.nodes)
    await db.commit()
    return {"id": tpl.id}


@router.put("/admin/approval/templates/{tpl_id}")
async def update_template(
    tpl_id: int,
    body: TemplateIn,
    me: User = Depends(require_admin),
    db: AsyncSession = Depends(get_db),
):
    """整链替换（名字/schema/节点）。

    ponytail: 进行中的单子按新链继续走（改模板=改流程）；要快照式隔离再加"版本"列。
    """
    tpl = await db.get(ApprovalTemplate, tpl_id)
    if tpl is None:
        raise HTTPException(404, "模板不存在")
    _validate_nodes(body.nodes)
    tpl.name = body.name.strip()
    tpl.form_schema = json.dumps(body.form_schema, ensure_ascii=False)
    await db.execute(delete(ApprovalNode).where(ApprovalNode.template_id == tpl_id))
    await _insert_nodes(db, tpl_id, body.nodes)
    await db.commit()
    return {"ok": True}


@router.delete("/admin/approval/templates/{tpl_id}")
async def delete_template(
    tpl_id: int, me: User = Depends(require_admin), db: AsyncSession = Depends(get_db)
):
    """有审批引用的模板不可删（留痕可追溯）。"""
    tpl = await db.get(ApprovalTemplate, tpl_id)
    if tpl is None:
        raise HTTPException(404, "模板不存在")
    n = (
        await db.execute(
            select(func.count())
            .select_from(Approval)
            .where(Approval.template_id == tpl_id)
        )
    ).scalar()
    if n:
        raise HTTPException(400, f"已有 {n} 笔审批使用该模板，不能删除")
    await db.execute(delete(ApprovalNode).where(ApprovalNode.template_id == tpl_id))
    await db.delete(tpl)
    await db.commit()
    return {"ok": True}
