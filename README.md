# fdebm

基于 Qt5 的 DEB 包管理器：图形界面以普通用户身份运行，安装、卸载、更新等特权操作通过系统总线交给特权助手 `fdebm-helperd`，由 polkit 完成授权。程序不再调用 `pkexec`，也不需要开启开发者模式。

## 架构

```
fdebm（普通用户进程）
   │  系统总线 org.fdebm.Helper1
   ▼
fdebm-helperd（root 运行的特权助手，polkit 授权）
   │  固定环境变量与工作目录
   ▼
apt install / remove / purge / update / upgrade / autoremove
```

| 组件 | 位置 | 职责 |
| --- | --- | --- |
| 主窗口 | `src/mainwindow.*` | 包列表、搜索、操作入口、进度与取消交互 |
| 包管理 | `src/packagemanager.*` | 只读查询（`dpkg -l`、`apt search`）与助手操作的类型化封装 |
| D-Bus 客户端 | `src/helperclient.*` | 异步调用、事务状态机、信号过滤、超时提示 |
| 进度对话框 | `src/operationprogressdialog.*` | 输出展示、取消确认 |
| 特权助手 | `src/helper/*` | 参数校验、polkit 授权、本地 deb 暂存、执行 apt、广播事务信号 |

助手同一时刻只处理一个事务（新的请求返回 Busy）。调用成功受理后返回事务 ID，后续 `Output` / `Finished` 信号只单播给事务发起者，其他调用者收不到。

## D-Bus 接口

服务名 `org.fdebm.Helper`，对象路径 `/org/fdebm/Helper`，接口 `org.fdebm.Helper1`，完整定义见 [`dbus/org.fdebm.Helper1.xml`](dbus/org.fdebm.Helper1.xml)。

| 方法 | 参数 | 返回 | 说明 |
| --- | --- | --- | --- |
| `InstallLocalPackages` | `as paths` | `s` 事务 ID | 安装本地 `.deb` 文件 |
| `InstallPackages` | `as names` | `s` | 从软件源安装包 |
| `RemovePackages` | `as names, b purge` | `s` | 卸载 / 清除配置 |
| `UpdateCache` | 无 | `s` | `apt update` |
| `Upgrade` | `b distUpgrade` | `s` | `apt upgrade` / `apt dist-upgrade` |
| `AutoRemove` | 无 | `s` | `apt autoremove` |
| `Cancel` | `s 事务 ID` | `b` | 取消事务，仅限该事务的发起者 |

| 信号 | 参数 | 说明 |
| --- | --- | --- |
| `Output` | `s 事务 ID, s 文本` | apt 的实时输出 |
| `Finished` | `s 事务 ID, i 退出码, s 消息` | 事务终点（成功、失败或被取消） |

## 安全模型

- **最小授权**：GUI 与助手都不以 root 运行，特权仅存在于短小的助手进程；命令行参数按白名单组装，不经过 shell。
- **polkit 策略**（[`packaging/org.fdebm.helper.policy`](packaging/org.fdebm.helper.policy)）：

  | 操作 | 活动会话 | 非活动/远程 |
  | --- | --- | --- |
  | 安装、卸载、升级 | `auth_admin_keep` | `auth_admin` |
  | 更新缓存 | `yes` | `auth_admin` |

  任意用户的默认结果均为 `no`，授权失败时不创建事务、不产生暂存文件（fail-closed）。
- **本地 deb 防 TOCTOU**：授权前以 `O_NOFOLLOW` 打开并校验（必须是普通文件、非空、组/其他用户不可写、单包不超过 4 GiB、总量不超过 8 GiB），授权后按文件描述符重新校验标识、大小与权限，再复制到属主为 root、权限 0700 的暂存目录（默认 `/var/lib/fdebm/staging`），apt 只接触暂存副本；事务结束清理暂存文件。
- **不泄露文件元数据**：授权前的本地包校验失败时，对外只返回统一的 `本地安装包校验失败`，具体原因（不存在、类型、权限、大小等）仅写入助手日志，避免本地用户借差异错误探测 root 可见的文件；授权后的暂存校验失败仍返回具体原因。
- **调用者绑定**：`Cancel` 只接受事务发起者，其他调用者收到 `NotAuthorized`；`Finished` / `Output` 定向发送，避免输出泄露给无关进程。
- **固定执行环境**：`PATH`、`HOME`、`LANG=C`、`DEBIAN_FRONTEND=noninteractive` 等显式设定，工作目录 `/`，标准输入为空，避免环境变量注入与交互式挂起。`apt` 不通过调用方的 `PATH` 查找，而只在 `/usr/bin`、`/bin`、`/usr/sbin`、`/sbin` 中解析为绝对路径；GUI 的只读命令（`dpkg -l`、`apt search`）同样使用绝对路径并固定 C locale。
- **可取消但不自动终止**：取消会先发送 SIGTERM，5 秒后仍未退出则 SIGKILL；GUI 在 10 分钟无输出时只提示（`operationStalled`），不会自动杀掉 apt，以免 dpkg 停留在部分完成状态。
- **单事务**：助手在进行中拒绝新事务，客户端也会在本地拦截重复请求。

### 代理

助手不会继承登录会话的代理环境变量。需要代理时，创建 `/etc/fdebm/proxy.conf`（属主 root，组/其他用户不可写）：

```ini
http_proxy=http://127.0.0.1:3128
https_proxy=http://127.0.0.1:3128
no_proxy=localhost,127.0.0.1
```

只识别 `http_proxy`、`https_proxy`、`ftp_proxy`、`no_proxy`（大小写两种写法），其他变量与控制字符一律忽略；文件不存在或权限不安全时静默跳过，不会阻断操作。也可以直接使用 apt 自身的 `Acquire::*::Proxy` 配置。

## 依赖

构建：

```bash
sudo apt install cmake g++ qtbase5-dev
```

运行（deb 包已声明）：`apt`、`polkitd | policykit-1`、`libqt5core5a`、`libqt5widgets5`、`libqt5dbus5`。

测试：`dbus`、`python3-gi`、`libglib2.0-bin`（`gdbus`）；打包：`dpkg-dev`。

## 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j"$(nproc)"
```

产物为 `build/fdebm` 与 `build/fdebm-helperd`，构建目录可任意指定（建议放在源码树之外）。版本号唯一来源是 [`CMakeLists.txt`](CMakeLists.txt) 中的 `project(... VERSION ...)`，两个程序通过编译定义获得该版本。

### 交叉编译

工具链文件在 `cmake/toolchains/`：`linux-arm64.cmake`（amd64 宿主 → arm64）与 `linux-amd64.cmake`（arm64 宿主 → amd64），采用 Debian/Ubuntu multiarch 方案。

```bash
# 宿主 amd64 准备 arm64 交叉环境（一次性，需 root）
sudo dpkg --add-architecture arm64
sudo apt install crossbuild-essential-arm64 qtbase5-dev:arm64 qtbase5-dev-tools

cmake -S . -B build-arm64 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-arm64.cmake
cmake --build build-arm64 -j"$(nproc)"
```

- 目标 Qt5 的 CMake 配置从 `/usr/lib/<triplet>/cmake` 读取，`moc`/`uic`/`rcc` 始终使用宿主工具（`CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER`），因此不需要自建 host Qt。
- 交叉编译时 `BUILD_TESTING` 默认关闭：测试目标需要目标架构的 `Qt5::Test`，产物也无法在宿主直接执行。安装 `qemu-user-static` 后可用 `-DBUILD_TESTING=ON`，工具链会自动把 `CMAKE_CROSSCOMPILING_EMULATOR` 指向 `qemu-<arch> -L /usr/<triplet>`，让 CTest 可运行。
- 交叉编译必须使用全新的构建目录；切换工具链复用它会被 CMake 拒绝（打包脚本会提前报错）。

## 测试

```bash
ctest --test-dir build --output-on-failure
python3 tests/test_metadata.py
BUILD_DIR=$PWD/build bash tests/run_security_tests.sh
BUILD_DIR=$PWD/build bash tests/run_gui_smoke.sh
```

`BUILD_DIR` 必须显式指定，脚本不再默认使用仓库内 `build/`。所有测试脚本都直接执行构建产物，因此交叉编译产物需要先安装 `qemu-user-static` 并配置 binfmt，或在目标架构机器上运行。

- `packageargs`（CTest）：包名与本地 deb 路径校验的单元测试，覆盖多架构后缀、非法字符、路径规范化、权限与数量上限。
- `gui_interaction`（CTest，offscreen）：通过真实信号驱动主窗口与进度对话框，覆盖加载操作不创建进度框、按操作类型过滤进度与结束信号、取消确认/失败恢复、运行中关闭需确认、结束后按钮关闭等交互。
- `run_security_tests.sh`：启动私有 `dbus-daemon` 与模拟 polkit 服务，以 `--dry-run` 运行助手，共 37 项检查。除参数与暂存目录校验、授权期间替换/改权限（TOCTOU）、Busy、fail-closed、取消权限外，模拟 polkit 还会按 `/proc/<pid>/stat` 校验 subject 的 pid 与 start-time，并断言四类 action 均以正确 subject 到达。缺少 `python3-gi` 或 `dbus-daemon` 时以 77 退出表示跳过。
- `run_gui_smoke.sh`：在 offscreen 平台启动真实 GUI 与助手，确认进程稳定运行。
- `test_metadata.py`：校验 polkit 策略、系统总线策略、D-Bus 激活/服务文件、systemd 单元、桌面入口与接口 XML 的一致性（含 action 默认值、路径、信号/方法签名），并让 `dbus-daemon` 实际加载总线策略文件。

CI 在 Ubuntu 22.04 / 24.04 上执行原生构建、单元测试、元数据测试与安全回归，并校验版本一致性以及 deb 包内容；另有一个 ASan + UBSan 任务用同样的测试覆盖内存与未定义行为问题；`cross-build-arm64` 任务在 amd64 上交叉编译 arm64 并校验包架构与产物 ELF 架构一致。

## 打包与安装

```bash
bash packaging/package_deb.sh              # 默认构建目录 build-deb
bash packaging/package_deb.sh /tmp/build   # 也可显式指定
sudo apt install ./build-deb/fdebm_<版本>_<架构>.deb
```

交叉打包只需指定工具链文件，包架构由产物 ELF 头自动判定（`DEB_ARCH` 可显式覆盖，与产物不一致会报错；交叉构建下若无法识别产物架构会直接失败，不会静默回退成宿主架构）：

```bash
CMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-arm64.cmake \
  bash packaging/package_deb.sh build-arm64
# 得到 build-arm64/fdebm_<版本>_arm64.deb，可拷到 arm64 机器上安装
```

脚本可在任意工作目录执行：配置、编译、按 CMake 安装规则填充包目录并调用 `dpkg-deb`，版本取自 CMake。安装内容：

| 路径 | 内容 |
| --- | --- |
| `/usr/bin/fdebm` | 图形界面 |
| `/usr/lib/fdebm/fdebm-helperd` | 特权助手 |
| `/usr/share/dbus-1/system-services/org.fdebm.Helper.service` | D-Bus 激活 |
| `/usr/lib/systemd/system/org.fdebm.Helper.service` | systemd 单元（`Type=dbus`） |
| `/usr/share/dbus-1/system.d/org.fdebm.Helper.conf` | 系统总线策略 |
| `/usr/share/polkit-1/actions/org.fdebm.helper.policy` | polkit 授权策略 |
| `/usr/share/applications/fdebm.desktop`、`/usr/share/icons/hicolor/scalable/apps/fdebm.svg` | 桌面入口与图标 |

卸载用 `sudo apt remove fdebm`。开发时也可直接运行 `build/fdebm`，但特权操作需要系统总线上已安装并激活的助手，因此推荐用 deb 包部署。

## 平台说明

- 最初面向 UOS arm64 编写。amd64 已完成原生构建、测试与打包验证；arm64 通过 `cmake/toolchains/linux-arm64.cmake` 交叉构建并在 CI 中校验产物架构与包内容，但 arm64 的运行时测试（`ctest`、安全回归、GUI 冒烟）尚未在 CI 中覆盖，需在 arm64 机器或 qemu 下补跑。
- 代码与 Qt5 之外无架构相关依赖（不链接 `libapt-pkg`，`apt`/`dpkg` 通过绝对路径调用），两种架构同源。
- 不再依赖 `pkexec`、开发者模式或 root 启动 GUI；授权统一由 polkit 承担，需要系统启用 polkit 服务。
- GUI 只负责展示与发起请求，不直接执行任何特权命令。

## 许可

Apache License 2.0，见 [`LICENSE`](LICENSE)。项目部分代码在 AI 辅助下完成，按现状提供，不附带任何质量保证。
