"""WebSocket：只推送，不收业务（契约 §三）。

- 连接：/ws?token=<jwt>；非法 token 关闭（code=4401）
- 客户端每 30s 发 {"type":"ping"}；服务端 60s 收不到任何消息即断开
- 推送：message.new / approval.update / notification.new，格式见契约
"""

import asyncio
import json

from fastapi import APIRouter, WebSocket, WebSocketDisconnect

from .security import parse_token

router = APIRouter()


class Manager:
    """在线连接表：user_id → 该用户的全部连接（同账号多端登录都能收到推送）。"""

    def __init__(self):
        self.conns: dict[int, set[WebSocket]] = {}

    def connect(self, uid: int, ws: WebSocket):
        self.conns.setdefault(uid, set()).add(ws)

    def disconnect(self, uid: int, ws: WebSocket):
        self.conns.get(uid, set()).discard(ws)
        if uid in self.conns and not self.conns[uid]:
            del self.conns[uid]

    async def send_to_user(self, uid: int, payload: dict):
        """推给单个用户的所有端。发送失败的连接静默移除。"""
        conns = self.conns.get(uid)
        if not conns:
            return
        for ws in list(conns):
            try:
                await ws.send_text(json.dumps(payload, ensure_ascii=False))
            except Exception:
                self.disconnect(uid, ws)

    async def send_to_users(self, uids, payload: dict):
        for uid in set(uids):
            await self.send_to_user(uid, payload)


# 全局唯一实例：routers 里 from ..ws import manager 直接用
manager = Manager()


@router.websocket("/ws")
async def ws_endpoint(ws: WebSocket, token: str = ""):
    uid = parse_token(token)
    await ws.accept()
    if uid is None:
        await ws.close(code=4401)  # 自定义：未认证
        return
    manager.connect(uid, ws)
    try:
        while True:
            # 只收 ping（内容不校验）；60s 无消息判掉线（契约）
            await asyncio.wait_for(ws.receive_text(), timeout=60)
    except (asyncio.TimeoutError, WebSocketDisconnect, RuntimeError):
        pass  # 超时/客户端断开，都走清理
    finally:
        manager.disconnect(uid, ws)
