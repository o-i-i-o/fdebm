#!/usr/bin/env python3
"""fdebm-helperd 安全用例客户端。

用法: security_client.py <bus-address> <command> [args...]

命令：
  install <deb>...
  reject-install <错误关键字> <deb>...
  install-names <错误关键字> <name>...  从源安装，预期被拒绝
  install-names-ok <name>...            从源安装，预期受理并成功
  update-cache                          更新缓存，预期受理并成功
  upgrade                               系统升级，预期受理并成功
  reject-update-cache <错误关键字>      更新缓存，预期被拒绝
  reject-upgrade <错误关键字>           系统升级，预期被拒绝
  busy <deb>                        安装事务进行中再次请求，预期 Busy
  remove <name>                     请求卸载包，预期受理
  reject-remove <错误关键字> <name>  请求卸载包，预期被拒绝
  chmod-during-auth <八进制权限> <deb>  授权等待期间放宽文件权限
  swap-during-auth <deb> <目标>       授权等待期间把文件替换为符号链接
  replace-during-auth <deb>          授权等待期间把文件替换为另一个普通文件
  cancel                             发起事务后由发起方取消，预期成功
  cancel-foreign                     另一连接取消他人事务，预期被拒绝
  cancel-missing                     取消不存在的事务，预期被拒绝
"""
import os
import sys

from gi.repository import Gio, GLib

BUS_NAME = "org.fdebm.Helper"
BUS_PATH = "/org/fdebm/Helper"
IFACE = "org.fdebm.Helper1"


def fail(message):
    print("FAIL", message, flush=True)
    return 1


def ok(message):
    print("RESULT OK", message, flush=True)
    return 0


class Client:
    def __init__(self, address):
        self.conn = Gio.DBusConnection.new_for_address_sync(
            address,
            Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT
            | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION,
            None, None)
        self.loop = GLib.MainLoop()
        self.finished = {}
        self.check = None
        self.conn.signal_subscribe(None, IFACE, "Finished", BUS_PATH, None,
                                   Gio.DBusSignalFlags.NONE, self._on_signal, None)
        self.conn.signal_subscribe(None, IFACE, "Output", BUS_PATH, None,
                                   Gio.DBusSignalFlags.NONE, self._on_output, None)

    def _on_signal(self, _conn, _sender, _path, _iface, _name, params, _data):
        tx, exit_code, message = params.unpack()
        print("SIGNAL Finished", tx, exit_code, message, flush=True)
        self.finished[tx] = (exit_code, message)
        if self.check:
            self.check()

    def _on_output(self, _conn, _sender, _path, _iface, _name, params, _data):
        tx, text = params.unpack()
        print("OUTPUT", tx, text.replace("\n", " ").strip(), flush=True)

    def call(self, method, params, on_reply):
        self.conn.call(BUS_NAME, BUS_PATH, IFACE, method, params, None,
                       Gio.DBusCallFlags.NONE, 60000, None, on_reply, None)

    def run(self, timeout_ms=30000):
        state = {"timed_out": False}

        def on_timeout():
            state["timed_out"] = True
            self.loop.quit()
            return False

        GLib.timeout_add(timeout_ms, on_timeout)
        self.loop.run()
        return state["timed_out"]


def install_expect_reject(client, paths, substring, setup=None):
    state = {"error": None}

    def on_reply(source, result, _data):
        try:
            reply = source.call_finish(result).unpack()
            print("UNEXPECTED_REPLY", reply, flush=True)
        except GLib.Error as exc:
            state["error"] = exc.message
            print("ERROR", exc.message, flush=True)
        client.loop.quit()

    if setup:
        setup()
    client.call("InstallLocalPackages", GLib.Variant("(as)", (paths,)), on_reply)
    if client.run():
        return fail("等待错误回复超时")
    if not state["error"]:
        return fail("预期拒绝，但调用成功")
    if substring not in state["error"]:
        return fail("错误信息未包含 %r" % substring)
    return ok(substring)


def cmd_accept(client, method, params):
    state = {"tx": None, "error": None}

    def complete():
        if state["tx"] in client.finished:
            client.loop.quit()

    def on_reply(source, result, _data):
        try:
            state["tx"] = source.call_finish(result).unpack()[0]
            print("REPLY", state["tx"], flush=True)
        except GLib.Error as exc:
            state["error"] = exc.message
            print("ERROR", exc.message, flush=True)
            client.loop.quit()
            return
        complete()

    client.check = complete
    client.call(method, params, on_reply)
    if client.run():
        return fail("等待事务结束超时")
    if state["error"]:
        return fail("调用失败: %s" % state["error"])
    exit_code, message = client.finished.get(state["tx"], (None, None))
    if exit_code != 0:
        return fail("事务未成功结束: %r %r" % (exit_code, message))
    return ok("事务完成")


def cmd_install(client, paths):
    return cmd_accept(client, "InstallLocalPackages",
                      GLib.Variant("(as)", (paths,)))


def cmd_install_names_ok(client, names):
    return cmd_accept(client, "InstallPackages", GLib.Variant("(as)", (names,)))


def cmd_update_cache(client):
    return cmd_accept(client, "UpdateCache", None)


def cmd_upgrade(client):
    return cmd_accept(client, "Upgrade", GLib.Variant("(b)", (False,)))


def expect_error(client, method, params, substring):
    state = {"error": None}

    def on_reply(source, result, _data):
        try:
            reply = source.call_finish(result).unpack()
            print("UNEXPECTED_REPLY", reply, flush=True)
        except GLib.Error as exc:
            state["error"] = exc.message
            print("ERROR", exc.message, flush=True)
        client.loop.quit()

    client.call(method, params, on_reply)
    if client.run():
        return fail("等待错误回复超时")
    if not state["error"]:
        return fail("预期拒绝，但调用成功")
    if substring not in state["error"]:
        return fail("错误信息未包含 %r" % substring)
    return ok(substring)


def cmd_reject_install_names(client, names, substring):
    return expect_error(client, "InstallPackages", GLib.Variant("(as)", (names,)),
                        substring)


def cmd_reject_update_cache(client, substring):
    return expect_error(client, "UpdateCache", None, substring)


def cmd_reject_upgrade(client, substring):
    return expect_error(client, "Upgrade", GLib.Variant("(b)", (False,)), substring)


def cmd_busy(client, path):
    state = {"tx": None, "busy": None, "error": None}

    def maybe_finish():
        if state["tx"] in client.finished and (state["busy"] or state["error"]):
            client.loop.quit()

    def on_second(source, result, _data):
        try:
            reply = source.call_finish(result).unpack()
            state["error"] = "并发调用意外成功: %r" % (reply,)
        except GLib.Error as exc:
            state["busy"] = exc.message
            print("SECOND_ERROR", exc.message, flush=True)
        maybe_finish()

    def on_reply(source, result, _data):
        try:
            state["tx"] = source.call_finish(result).unpack()[0]
            print("REPLY", state["tx"], flush=True)
        except GLib.Error as exc:
            state["error"] = exc.message
            client.loop.quit()
            return
        check()

    def check():
        if state["tx"] is not None and state["busy"] is None and state["error"] is None \
                and not state.get("second_sent"):
            state["second_sent"] = True
            client.call("UpdateCache", None, on_second)
        maybe_finish()

    client.check = check
    client.call("InstallLocalPackages", GLib.Variant("(as)", ([path],)), on_reply)
    if client.run():
        return fail("等待 Busy 用例超时")
    if state["error"]:
        return fail(state["error"])
    if "已有操作正在进行" not in (state["busy"] or ""):
        return fail("并发请求未被拒绝: %r" % (state["busy"],))
    if client.finished.get(state["tx"], (None,))[0] != 0:
        return fail("首个事务未正常完成")
    return ok("并发请求被 Busy 拒绝")


def cmd_remove(client, name):
    state = {"tx": None, "error": None}

    def complete():
        if state["tx"] in client.finished:
            client.loop.quit()

    def on_reply(source, result, _data):
        try:
            state["tx"] = source.call_finish(result).unpack()[0]
            print("REPLY", state["tx"], flush=True)
        except GLib.Error as exc:
            state["error"] = exc.message
            print("ERROR", exc.message, flush=True)
            client.loop.quit()
            return
        complete()

    client.check = complete
    client.call("RemovePackages", GLib.Variant("(asb)", ([name], False)), on_reply)
    if client.run():
        return fail("等待卸载结果超时")
    if state["error"]:
        return fail("卸载失败: %s" % state["error"])
    exit_code, message = client.finished.get(state["tx"], (None, None))
    if exit_code != 0:
        return fail("事务未成功结束: %r %r" % (exit_code, message))
    return ok("卸载请求已受理")


def cmd_reject_remove(client, name, substring):
    state = {"error": None}

    def on_reply(source, result, _data):
        try:
            reply = source.call_finish(result).unpack()
            print("UNEXPECTED_REPLY", reply, flush=True)
        except GLib.Error as exc:
            state["error"] = exc.message
            print("ERROR", exc.message, flush=True)
        client.loop.quit()

    client.call("RemovePackages", GLib.Variant("(asb)", ([name], False)), on_reply)
    if client.run():
        return fail("等待错误回复超时")
    if not state["error"]:
        return fail("预期拒绝，但调用成功")
    if substring not in state["error"]:
        return fail("错误信息未包含 %r" % substring)
    return ok(substring)


def cmd_chmod_during_auth(client, mode_text, path):
    def setup():
        def chmod():
            os.chmod(path, int(mode_text, 8))
            print("CHMOD", path, mode_text, flush=True)
            return False
        GLib.timeout_add(150, chmod)

    return install_expect_reject(client, [path], "允许其他用户写入", setup)


def cmd_swap_during_auth(client, path, target):
    def setup():
        def swap():
            os.rename(path, path + ".orig")
            os.symlink(target, path)
            print("SWAP", path, "->", target, flush=True)
            return False
        GLib.timeout_add(150, swap)

    return install_expect_reject(client, [path], "在授权期间被替换", setup)


def cmd_replace_during_auth(client, path):
    def setup():
        def replace():
            os.rename(path, path + ".orig")
            with open(path, "wb") as handle:
                handle.write(b"replacement deb payload\n")
            os.chmod(path, 0o644)
            print("REPLACE", path, flush=True)
            return False
        GLib.timeout_add(150, replace)

    return install_expect_reject(client, [path], "在授权期间被替换", setup)


def new_connection(address):
    return Gio.DBusConnection.new_for_address_sync(
        address,
        Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT
        | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION,
        None, None)


def cmd_cancel(client, address, foreign):
    state = {"tx": None, "cancelled": None, "error": None, "foreign_denied": False}

    def call_own_cancel():
        def on_cancel(source, result, _data):
            try:
                state["cancelled"] = source.call_finish(result).unpack()[0]
            except GLib.Error as exc:
                state["error"] = "取消失败: %s" % exc.message
                client.loop.quit()
                return
            print("CANCELLED", state["cancelled"], flush=True)
            if state["tx"] in client.finished:
                client.loop.quit()

        client.call("Cancel", GLib.Variant("(s)", (state["tx"],)), on_cancel)

    def call_foreign_cancel():
        other = new_connection(address)

        def on_foreign(source, result, _data):
            try:
                reply = source.call_finish(result).unpack()
                state["error"] = "第三方取消意外成功: %r" % (reply,)
                client.loop.quit()
                return
            except GLib.Error as exc:
                print("FOREIGN_ERROR", exc.message, flush=True)
                if "无权取消该事务" not in exc.message:
                    state["error"] = "第三方取消错误信息不符: %s" % exc.message
                    client.loop.quit()
                    return
            state["foreign_denied"] = True
            call_own_cancel()

        other.call(BUS_NAME, BUS_PATH, IFACE, "Cancel",
                   GLib.Variant("(s)", (state["tx"],)), None,
                   Gio.DBusCallFlags.NONE, 5000, None, on_foreign, None)

    def on_install(source, result, _data):
        try:
            state["tx"] = source.call_finish(result).unpack()[0]
            print("REPLY", state["tx"], flush=True)
        except GLib.Error as exc:
            state["error"] = "安装请求失败: %s" % exc.message
            client.loop.quit()
            return
        if foreign:
            call_foreign_cancel()
        else:
            call_own_cancel()

    def complete():
        if state["tx"] in client.finished and state["cancelled"] is not None:
            client.loop.quit()

    client.check = complete
    client.call("InstallPackages", GLib.Variant("(as)", (["bash"],)), on_install)
    if client.run(15000):
        return fail("等待取消用例超时")
    if state["error"]:
        return fail(state["error"])
    if foreign and not state["foreign_denied"]:
        return fail("第三方取消未被拒绝")
    if state["cancelled"] is not True:
        return fail("Cancel 未返回 true: %r" % (state["cancelled"],))
    exit_code, message = client.finished.get(state["tx"], (None, None))
    if exit_code is None or exit_code == 0 or message != "操作已取消":
        return fail("取消后 Finished 不符合预期: %r %r" % (exit_code, message))
    return ok("第三方取消被拒绝" if foreign else "发起方取消成功")


def cmd_cancel_missing(address):
    conn = new_connection(address)
    try:
        conn.call_sync(BUS_NAME, BUS_PATH, IFACE, "Cancel",
                       GLib.Variant("(s)", ("tx-does-not-exist",)), None,
                       Gio.DBusCallFlags.NONE, 5000, None)
    except GLib.Error as exc:
        print("ERROR", exc.message, flush=True)
        if "事务不存在或已结束" in exc.message:
            return ok("取消不存在的事务")
        return fail("错误信息未包含事务不存在: %s" % exc.message)
    return fail("取消不存在的事务意外成功")


def main(argv):
    if len(argv) < 3:
        return fail("用法: security_client.py <bus-address> <command> ...")

    client = Client(argv[1])
    command = argv[2]

    if command == "install" and len(argv) >= 4:
        return cmd_install(client, argv[3:])
    if command == "reject-install" and len(argv) >= 4:
        return install_expect_reject(client, argv[4:], argv[3])
    if command == "install-names" and len(argv) >= 4:
        return cmd_reject_install_names(client, argv[4:], argv[3])
    if command == "install-names-ok" and len(argv) >= 4:
        return cmd_install_names_ok(client, argv[3:])
    if command == "update-cache" and len(argv) == 3:
        return cmd_update_cache(client)
    if command == "upgrade" and len(argv) == 3:
        return cmd_upgrade(client)
    if command == "reject-update-cache" and len(argv) == 4:
        return cmd_reject_update_cache(client, argv[3])
    if command == "reject-upgrade" and len(argv) == 4:
        return cmd_reject_upgrade(client, argv[3])
    if command == "busy" and len(argv) == 4:
        return cmd_busy(client, argv[3])
    if command == "remove" and len(argv) == 4:
        return cmd_remove(client, argv[3])
    if command == "reject-remove" and len(argv) == 5:
        return cmd_reject_remove(client, argv[4], argv[3])
    if command == "chmod-during-auth" and len(argv) == 5:
        return cmd_chmod_during_auth(client, argv[3], argv[4])
    if command == "swap-during-auth" and len(argv) == 5:
        return cmd_swap_during_auth(client, argv[3], argv[4])
    if command == "replace-during-auth" and len(argv) == 4:
        return cmd_replace_during_auth(client, argv[3])
    if command == "cancel" and len(argv) == 3:
        return cmd_cancel(client, argv[1], False)
    if command == "cancel-foreign" and len(argv) == 3:
        return cmd_cancel(client, argv[1], True)
    if command == "cancel-missing" and len(argv) == 3:
        return cmd_cancel_missing(argv[1])
    return fail("未知命令: %s" % " ".join(argv[2:]))


if __name__ == "__main__":
    sys.exit(main(sys.argv))
