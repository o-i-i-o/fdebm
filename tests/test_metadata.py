#!/usr/bin/env python3
"""打包元数据与接口约定的静态回归测试。

校验 polkit 策略、系统总线策略、D-Bus 激活/服务文件、systemd 单元、
桌面入口与接口 XML 之间的一致性；若存在 dbus-daemon，还会实际加载
系统总线策略文件，确认守护进程接受该配置。
"""
import configparser
import os
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PACKAGING = os.path.join(REPO, "packaging")

failures = 0


def check(condition, message):
    global failures
    if condition:
        print("PASS:", message, flush=True)
    else:
        print("FAIL:", message, flush=True)
        failures += 1


def read(path):
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read()


def load_ini(path):
    parser = configparser.ConfigParser(interpolation=None, strict=False)
    parser.optionxform = str
    parser.read(path, encoding="utf-8")
    return parser


def test_polkit_policy():
    path = os.path.join(PACKAGING, "org.fdebm.helper.policy")
    root = ET.parse(path).getroot()
    actions = {}
    for action in root.findall("action"):
        actions[action.get("id")] = action

    expected = {
        "org.fdebm.helper.install",
        "org.fdebm.helper.remove",
        "org.fdebm.helper.update-cache",
        "org.fdebm.helper.upgrade",
    }
    check(set(actions) == expected, "polkit 策略包含且仅包含四个 helper action")

    for action_id, action in actions.items():
        defaults = action.find("defaults")
        check(defaults is not None, "%s 包含 defaults" % action_id)
        if defaults is None:
            continue
        allow_any = defaults.findtext("allow_any")
        allow_inactive = defaults.findtext("allow_inactive")
        allow_active = defaults.findtext("allow_active")
        check(allow_any == "no", "%s allow_any=no" % action_id)
        check(allow_inactive == "auth_admin", "%s allow_inactive=auth_admin" % action_id)
        if action_id == "org.fdebm.helper.update-cache":
            check(allow_active == "yes", "%s allow_active=yes" % action_id)
        else:
            check(allow_active == "auth_admin_keep",
                  "%s allow_active=auth_admin_keep" % action_id)
        check(bool(action.findtext("description")), "%s 有 description" % action_id)
        check(bool(action.findtext("message")), "%s 有 message" % action_id)

    vendor_url = root.findtext("vendor_url") or ""
    check(vendor_url.startswith("https://") and "example.com" not in vendor_url,
          "polkit vendor_url 不是占位地址")


def test_bus_policy():
    path = os.path.join(PACKAGING, "org.fdebm.Helper.conf")
    root = ET.parse(path).getroot()
    owner_ok = False
    send_ok = False
    for policy in root.findall("policy"):
        if policy.get("user") == "root":
            owner_ok = any(item.get("own") == "org.fdebm.Helper"
                           for item in policy.findall("allow"))
        if policy.get("context") == "default":
            send_ok = any(item.get("send_destination") == "org.fdebm.Helper"
                          and item.get("send_interface") == "org.fdebm.Helper1"
                          for item in policy.findall("allow"))
    check(owner_ok, "总线策略允许 root 拥有 org.fdebm.Helper")
    check(send_ok, "总线策略允许向 Helper1 接口发送调用")

    if shutil.which("dbus-daemon") is None:
        print("SKIP: 没有 dbus-daemon，跳过策略加载", flush=True)
        return
    with tempfile.TemporaryDirectory() as tmp:
        wrapper = os.path.join(tmp, "bus.conf")
        with open(wrapper, "w", encoding="utf-8") as handle:
            handle.write("""<!DOCTYPE busconfig PUBLIC
 "-//freedesktop//DTD D-BUS Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>unix:tmpdir=%s</listen>
  <policy context="default">
    <allow own="*"/>
    <allow send_destination="*"/>
    <allow receive_sender="*"/>
    <allow send_requested_reply="true"/>
    <allow receive_requested_reply="true"/>
  </policy>
  <include>%s</include>
</busconfig>
""" % (tmp, path))
        result = subprocess.run(
            ["dbus-daemon", "--config-file", wrapper, "--fork",
             "--print-address=1", "--print-pid=1"],
            capture_output=True, text=True, timeout=15)
        check(result.returncode == 0,
              "dbus-daemon 接受系统总线策略文件" if result.returncode == 0
              else "dbus-daemon 接受系统总线策略文件（%s）" % result.stderr.strip())
        if result.returncode == 0:
            lines = result.stdout.strip().splitlines()
            try:
                os.kill(int(lines[-1]), 15)
            except (IndexError, ValueError, ProcessLookupError):
                pass


def test_service_files():
    activation = load_ini(os.path.join(PACKAGING, "org.fdebm.Helper.dbus-service"))
    check(activation.get("D-BUS Service", "Name", fallback="") == "org.fdebm.Helper",
          "D-Bus 激活文件的服务名正确")
    check(activation.get("D-BUS Service", "Exec", fallback="")
          == "/usr/lib/fdebm/fdebm-helperd",
          "D-Bus 激活文件指向 fdebm-helperd")
    check(activation.get("D-BUS Service", "User", fallback="") == "root",
          "D-Bus 激活文件以 root 启动")

    unit = load_ini(os.path.join(PACKAGING, "org.fdebm.Helper.service"))
    service = "Service"
    check(unit.get(service, "Type", fallback="") == "dbus", "systemd 单元 Type=dbus")
    check(unit.get(service, "BusName", fallback="") == "org.fdebm.Helper",
          "systemd 单元 BusName 正确")
    check(unit.get(service, "ExecStart", fallback="") == "/usr/lib/fdebm/fdebm-helperd",
          "systemd 单元 ExecStart 与激活文件一致")
    check(unit.get(service, "User", fallback="") == "root", "systemd 单元以 root 运行")
    for key in ("NoNewPrivileges", "PrivateTmp", "ProtectKernelTunables",
                "ProtectKernelModules", "ProtectControlGroups"):
        check(unit.get(service, key, fallback="") == "yes",
              "systemd 单元启用 %s" % key)


def test_desktop():
    parser = load_ini(os.path.join(PACKAGING, "fdebm.desktop"))
    check(parser.get("Desktop Entry", "Exec", fallback="") == "fdebm",
          "桌面入口 Exec=fdebm")
    check(parser.get("Desktop Entry", "Icon", fallback="") == "fdebm",
          "桌面入口 Icon=fdebm")
    check("System" in parser.get("Desktop Entry", "Categories", fallback=""),
          "桌面入口属于 System 分类")
    check(os.path.isfile(os.path.join(PACKAGING, "fdebm.svg")), "桌面图标文件存在")


def test_dbus_interface():
    path = os.path.join(REPO, "dbus", "org.fdebm.Helper1.xml")
    root = ET.parse(path).getroot()
    interfaces = root.findall("interface")
    check(len(interfaces) == 1, "接口 XML 只声明一个接口")
    if not interfaces:
        return
    iface = interfaces[0]
    check(iface.get("name") == "org.fdebm.Helper1", "接口名称为 org.fdebm.Helper1")

    methods = {}
    for method in iface.findall("method"):
        methods[method.get("name")] = [
            (arg.get("type"), arg.get("direction")) for arg in method.findall("arg")]
    expected_methods = {
        "InstallLocalPackages", "InstallPackages", "RemovePackages", "UpdateCache",
        "Upgrade", "AutoRemove", "Cancel",
    }
    check(set(methods) == expected_methods, "接口方法集合与实现一致")
    check(methods.get("Cancel") == [("s", "in"), ("b", "out")],
          "Cancel 的参数为 (s) -> b")

    signals = {}
    for signal in iface.findall("signal"):
        signals[signal.get("name")] = [
            (arg.get("type"), arg.get("direction")) for arg in signal.findall("arg")]
    check(set(signals) == {"Output", "Finished"}, "接口信号为 Output 与 Finished")
    check(signals.get("Output") == [("s", None), ("s", None)],
          "Output 信号为 (s, s)")
    check(signals.get("Finished") == [("s", None), ("i", None), ("s", None)],
          "Finished 信号为 (s, i, s)")


def test_no_legacy_pkexec():
    check(not os.path.exists(os.path.join(REPO, "org.fdebm.policy")),
          "不再保留根目录 pkexec 策略")
    policy_text = read(os.path.join(PACKAGING, "org.fdebm.helper.policy"))
    check("pkexec" not in policy_text, "polkit 策略不包含 pkexec action")


def main():
    for test in (test_polkit_policy, test_bus_policy, test_service_files,
                 test_desktop, test_dbus_interface, test_no_legacy_pkexec):
        test()
    print("----------------------------------------")
    if failures:
        print("元数据测试失败 %d 项" % failures)
        return 1
    print("元数据测试全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
