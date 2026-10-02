"""一次性冒烟测试：覆盖登录/组织/审批链/聊天/文件/待办/通知。

纯 stdlib（urllib），由 backend/venv 的解释器在本机执行；跑完即删。
"""

import json
import time
import urllib.error
import urllib.request

BASE = "http://127.0.0.1:8600/api"
TOKENS: dict[str, str] = {}  # admin / zhangsan


def req(
    method: str,
    path: str,
    who: str | None = None,
    body=None,
    raw: bytes | None = None,
    ctype: str = "application/json",
):
    """发请求返回 (status, parsed_json_or_bytes)。who 取 TOKENS 里的 token。"""
    data = (
        raw
        if raw is not None
        else (json.dumps(body).encode() if body is not None else None)
    )
    r = urllib.request.Request(BASE + path, data=data, method=method)
    if who:
        r.add_header("Authorization", "Bearer " + TOKENS[who])
    if data is not None:
        r.add_header("Content-Type", ctype)
    with urllib.request.urlopen(r) as resp:
        payload = resp.read()
        try:
            return resp.status, json.loads(payload)
        except (ValueError, UnicodeDecodeError):
            return resp.status, payload


def multipart(fields: dict, file_field: str, filename: str, data: bytes, mime: str):
    """手工拼 multipart/form-data（免第三方库）。"""
    b = "----ifoa-smoke"
    out = ""
    for k, v in fields.items():
        out += f'--{b}\r\nContent-Disposition: form-data; name="{k}"\r\n\r\n{v}\r\n'
    out += (
        f'--{b}\r\nContent-Disposition: form-data; name="{file_field}"; '
        f'filename="{filename}"\r\nContent-Type: {mime}\r\n\r\n'
    )
    return (
        out.encode() + data + f"\r\n--{b}--\r\n".encode(),
        f"multipart/form-data; boundary={b}",
    )


def step(n: int, desc: str):
    print(f"[{n:02d}] {desc} ...", flush=True)


def main():
    # 等服务器就绪（最长 10s）
    for _ in range(20):
        try:
            s, d = req("GET", "/health")
            if s == 200:
                break
        except urllib.error.URLError:
            time.sleep(0.5)
    else:
        raise SystemExit("服务器未就绪")
    step(1, f"health -> {d}")

    s, d = req(
        "POST", "/auth/login", body={"username": "admin", "password": "admin123"}
    )
    assert s == 200 and d["token"] and d["user"]["role"] == "admin", d
    TOKENS["admin"] = d["token"]
    step(2, f"admin 登录 -> {d['user']['display_name']}")

    s, d = req("GET", "/departments", who="admin")
    assert s == 200 and d[0]["name"] == "总部" and d[0]["members"], d
    dept_id = d[0]["id"]
    step(3, f"部门树 -> {d[0]['name']} 成员 {[m['username'] for m in d[0]['members']]}")

    s, d = req(
        "POST",
        "/admin/users",
        who="admin",
        body={
            "username": "zhangsan",
            "password": "zs123456",
            "display_name": "张三",
            "department_id": dept_id,
        },
    )
    assert s == 200 and d["id"] > 1, d
    step(4, f"建用户 zhangsan -> id={d['id']}")

    s, d = req(
        "POST", "/auth/login", body={"username": "zhangsan", "password": "zs123456"}
    )
    assert s == 200, d
    TOKENS["zhangsan"] = d["token"]
    step(5, "zhangsan 登录")

    s, d = req(
        "PUT", f"/admin/departments/{dept_id}", who="admin", body={"leader_id": 1}
    )
    assert s == 200, d
    step(6, "设总部领导=admin（dept_leader 审批用）")

    s, d = req(
        "POST",
        "/admin/approval/templates",
        who="admin",
        body={
            "name": "请假",
            "form_schema": [
                {"key": "days", "label": "天数", "type": "number", "required": True},
                {"key": "reason", "label": "事由", "type": "text", "required": True},
            ],
            "nodes": [
                {"name": "部门领导审批", "approver_type": "dept_leader"},
                {"name": "总经理审批", "approver_type": "fixed_user", "approver_id": 1},
            ],
        },
    )
    assert s == 200 and d["id"], d
    tpl_id = d["id"]
    step(7, f"建审批模板「请假」(两级链) -> id={tpl_id}")

    s, d = req(
        "POST",
        "/approvals",
        who="zhangsan",
        body={
            "template_id": tpl_id,
            "title": "年假 1 天",
            "form_data": {"days": 1, "reason": "休息"},
        },
    )
    assert s == 200 and d["status"] == "pending", d
    ap_id = d["id"]
    step(8, f"张三发起审批 -> id={ap_id}")

    s, d = req("GET", "/approvals?box=todo", who="admin")
    assert s == 200 and any(a["id"] == ap_id for a in d), d
    step(9, f"admin 待我审批 -> {len(d)} 条（含本单，dept_leader 解析生效）")

    s, d = req(
        "POST",
        f"/approvals/{ap_id}/action",
        who="admin",
        body={"action": "approve", "comment": "同意"},
    )
    assert s == 200 and d["current_seq"] == 2, d
    step(10, f"一级通过 -> current_seq={d['current_seq']}")

    s, d = req(
        "POST", f"/approvals/{ap_id}/action", who="admin", body={"action": "approve"}
    )
    assert s == 200 and d["status"] == "approved", d
    step(11, f"二级通过 -> status={d['status']}")

    s, d = req("GET", "/notifications?unread=1", who="zhangsan")
    assert s == 200 and len(d) == 2, d  # 两次动作各一条
    step(12, f"张三未读通知 -> {len(d)} 条")

    s, d = req(
        "POST",
        "/conversations/single/1/messages",
        who="zhangsan",
        body={"type": "text", "content": "你好，管理员！"},
    )
    assert s == 200 and d["sender_name"] == "张三", d
    conv_id = d["conversation_id"]
    step(13, f"张三→admin 单聊自动建会话 -> conv={conv_id}")

    s, d = req("GET", "/conversations", who="admin")
    assert (
        s == 200 and d[0]["unread"] == 1 and d[0]["peer"]["display_name"] == "张三"
    ), d
    step(14, f"admin 会话列表 -> unread={d[0]['unread']}")

    s, d = req("PUT", f"/conversations/{conv_id}/read", who="admin")
    assert s == 200, d
    step(15, "admin 标记已读")

    payload, ctype = multipart(
        {"scope": "personal", "folder": ""},
        "file",
        "测试.txt",
        "IF-OA 冒烟测试内容".encode(),
        "text/plain",
    )
    s, d = req("POST", "/files/upload", who="zhangsan", raw=payload, ctype=ctype)
    assert s == 200 and d["name"] == "测试.txt", d
    file_id = d["id"]
    step(16, f"上传个人文件 -> id={file_id}")

    s, d = req("GET", "/files?scope=personal", who="zhangsan")
    assert s == 200 and len(d) == 1, d
    step(17, "个人文件列表 -> 1 个")

    s, d = req("POST", f"/files/{file_id}/favorite", who="zhangsan")
    assert s == 200, d
    s, d = req("GET", "/favorites", who="zhangsan")
    assert s == 200 and len(d) == 0, d  # 文本文件非 image/*，照片墙过滤生效
    step(18, "收藏成功；favorites 按图片过滤 -> 0（符合预期）")

    s, d = req("GET", f"/files/{file_id}/download", who="zhangsan")
    assert s == 200 and d == "IF-OA 冒烟测试内容".encode(), d
    step(19, "下载校验字节一致")

    s, d = req("POST", "/todos", who="zhangsan", body={"content": "写周报"})
    assert s == 200, d
    todo_id = d["id"]
    s, d = req("PUT", f"/todos/{todo_id}", who="zhangsan", body={"done": True})
    assert s == 200, d
    s, d = req("GET", "/todos", who="zhangsan")
    assert s == 200 and d[0]["done"], d
    s, d = req("DELETE", f"/todos/{todo_id}", who="zhangsan")
    assert s == 200, d
    step(20, "待办 增改查删")

    s, d = req("GET", "/approvals?box=sent", who="zhangsan")
    assert s == 200 and d[0]["status"] == "approved", d
    s, d = req("GET", f"/approvals/{ap_id}", who="zhangsan")
    assert s == 200 and len(d["records"]) == 2 and d["form_data"]["days"] == 1, d
    step(21, "审批详情：留痕 2 条 + 表单数据回显")

    s, d = req("GET", "/users/me", who="admin")
    assert s == 200 and d["username"] == "admin", d
    step(22, "越权抽查：zhangsan 访问 admin 端点应 403")
    r = urllib.request.Request(
        BASE + "/admin/users", headers={"Authorization": "Bearer " + TOKENS["zhangsan"]}
    )
    try:
        urllib.request.urlopen(r)
        raise AssertionError("member 竟然能访问 /admin/users")
    except urllib.error.HTTPError as e:
        assert e.code == 403, e.code

    print("\n[SMOKE] ALL PASS —— 22 项全过")


if __name__ == "__main__":
    main()
