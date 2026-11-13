#!/usr/bin/env python3
"""最小化的 polkit Authority 替身，只实现 CheckAuthorization。

环境变量：
  FAKE_POLKIT_ALLOW       逗号分隔的允许 action，设为空表示一律拒绝
  FAKE_POLKIT_DELAY_MS    回复前的延迟，用于复现授权窗口内的并发行为

每次调用都会校验 subject 必须是 unix-process，且 subject 中的 pid 与
start-time 必须和 /proc/<pid>/stat 一致；不一致一律拒绝，并在日志中
记录 SUBJECT_MISMATCH，避免替身服务掩盖助手的 subject 构造错误。
"""
import os
import sys
import time

from gi.repository import Gio, GLib

DEFAULT_ACTIONS = ",".join([
    "org.fdebm.helper.install",
    "org.fdebm.helper.remove",
    "org.fdebm.helper.update-cache",
    "org.fdebm.helper.upgrade",
])

xml = """
<node>
  <interface name="org.freedesktop.PolicyKit1.Authority">
    <method name="CheckAuthorization">
      <arg type="(sa{sv})" name="subject" direction="in"/>
      <arg type="s" name="action_id" direction="in"/>
      <arg type="a{ss}" name="details" direction="in"/>
      <arg type="u" name="flags" direction="in"/>
      <arg type="s" name="cancellation_id" direction="in"/>
      <arg type="(bba{ss})" name="result" direction="out"/>
    </method>
  </interface>
</node>
"""


def process_start_time(pid):
    """读取 /proc/<pid>/stat 的第 22 个字段（starttime，单位 clock tick）。"""
    try:
        with open("/proc/%d/stat" % pid, "r", encoding="utf-8", errors="replace") as handle:
            data = handle.read()
    except OSError:
        return None
    # comm 字段可能含空格或右括号，从最后一个 ')' 之后开始切分
    fields = data[data.rfind(")") + 2:].split()
    try:
        return int(fields[19])  # 第 3 个字段是 state，starttime 是第 22 个
    except (IndexError, ValueError):
        return None


def check_subject(subject):
    """返回 ok / invalid / missing / mismatch。"""
    try:
        kind, details = subject
        pid = details.get("pid")
        start_time = details.get("start-time")
    except (TypeError, ValueError):
        return "invalid"
    if kind != "unix-process" or not isinstance(pid, int) or not isinstance(start_time, int):
        return "invalid"
    if pid <= 0 or start_time <= 0:
        return "invalid"
    actual = process_start_time(pid)
    if actual is None:
        return "missing"
    return "ok" if actual == start_time else "mismatch"


def main():
    if len(sys.argv) != 2:
        raise SystemExit("用法: fake_polkit.py <bus-address>")

    allow_env = os.environ.get("FAKE_POLKIT_ALLOW")
    if allow_env is None:
        allow_env = DEFAULT_ACTIONS
    allowed = {item for item in allow_env.split(",") if item}
    delay_ms = int(os.environ.get("FAKE_POLKIT_DELAY_MS", "0"))

    connection = Gio.DBusConnection.new_for_address_sync(
        sys.argv[1],
        Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT
        | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION,
        None, None)
    node = Gio.DBusNodeInfo.new_for_xml(xml)

    def on_call(_conn, sender, _path, _iface, _method, params, invocation):
        subject, action, _details, flags, _cid = params.unpack()
        subject_state = check_subject(subject)
        # subject 不可信时即使 action 在白名单也拒绝，模拟真实 polkit 的行为
        allowed_now = subject_state == "ok" and action in allowed
        print("POLKIT action=%s pid=%s subject=%s flags=%s allowed=%s sender=%s"
              % (action, subject[1].get("pid") if subject_state != "invalid" else "-",
                 subject_state, flags, allowed_now, sender), flush=True)
        if subject_state != "ok":
            print("SUBJECT_%s action=%s" % (subject_state.upper(), action), flush=True)
        if delay_ms > 0:
            # 故意阻塞：模拟用户正在授权对话框中等待
            time.sleep(delay_ms / 1000.0)
        invocation.return_value(GLib.Variant("((bba{ss}))", ((allowed_now, False, {}),)))

    connection.register_object("/org/freedesktop/PolicyKit1/Authority",
                               node.interfaces[0], on_call, None, None)
    Gio.bus_own_name_on_connection(connection, "org.freedesktop.PolicyKit1",
                                   Gio.BusNameOwnerFlags.NONE, None, None)
    print("FAKE POLKIT READY", flush=True)
    GLib.MainLoop().run()


if __name__ == "__main__":
    main()
