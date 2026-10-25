# Superior-IME

先！进！的跨平台输入法,目标是完全取代老旧的搜狗输入法
目标支持Android,GNU/Linux,Windows三平台 (苹果系没设备暂缓)

- 输入引擎交给 Fcitx5（拼音 / Rime），本项目不重写引擎。
- 前端与后端分离：UI 只通过契约与后端通信。
- 皮肤使用自有的 `superior.skin/1`：声明式、版本化、可校验、可确定性渲染。
- 语音、Windows、macOS、Android 是后续里程碑，接口已预留。

详细设计：[PLAN.md](PLAN.md)、[docs/architecture.md](docs/architecture.md)、
[docs/skin-format.md](docs/skin-format.md)、[docs/logging.md](docs/logging.md)、
[docs/dictionary.md](docs/dictionary.md)、[docs/fcitx5-integration.md](docs/fcitx5-integration.md)。

---

## 当前状态

| 里程碑 | 内容 | 状态 |
|---|---|---|
| M0 | 构建系统、脚本、`backend/api`、边界检查 | ✅ |
| M1 | `superior.skin/1`、loader/validator、`specHash`、`skin-lint`、默认皮肤 | ✅ |
| M2 | `backend/core`、`backend/runtime` | ✅ |
| M2.5 | `backend/protocol`：帧、`Hello` / `Scene` / `Command`、Unix socket | ✅ |
| M3 | Fcitx5 观察者 agent、Qt 桌面壳、X11 浮层 | ✅ 自绘候选窗与状态栏已实测 |
| M3.5 | 统一日志、词库布局对齐、搜狗细胞词库导入 | ✅ |
| M4 | XWayland 自动选择、classicui 主题导出 | 🚧 自动选择已完成；layer-shell 状态栏待做 |
| P2 | `.ssf` 导入器（可选工具，不进入运行时） | 未计划 |
| P3 | 本地流式语音（sherpa-onnx） | 未计划 |

---

## 构建

要求：CMake ≥ 3.25、Ninja、C++20 编译器、`nlohmann-json3-dev`。

```bash
./scripts/bootstrap.sh          # 只检查依赖，不安装任何东西
./scripts/configure.sh          # 默认 linux-debug
./scripts/build.sh
./scripts/test.sh
```

其他 preset：

```bash
./scripts/configure.sh linux-release
./scripts/build.sh linux-release

./scripts/configure.sh linux-asan    # ASan + UBSan
./scripts/test.sh linux-asan

./scripts/configure.sh linux-ui      # Fcitx5 agent + Qt 浮层
./scripts/build.sh linux-ui
```

---

## 运行桌面浮层（XWayland）

状态栏与候选窗都是 override-redirect 窗口：永久置顶、不抢焦点、
不出现在任务栏。它们只是另一个进程，**不参与输入路径**。

`superior_ui` 启动时自己选择平台插件：Wayland 会话且存在 `DISPLAY` 时强制
`QT_QPA_PLATFORM=xcb`（走 XWayland），这样窗口才保持 override-redirect；
`SUPERIOR_UI_PLATFORM` 可覆盖这个选择。判定逻辑与用例见
`frontend/desktop/src/PlatformChoice.{hpp,cpp}` 与 `tests/qt_platform_tests.cpp`。

```bash
# 1. 构建
./scripts/configure.sh linux-ui
./scripts/build.sh linux-ui

# 2. 没有安装 agent 时，用模拟器看效果
./build/linux-ui/tools/scene-sim/superior_scene_sim &
./scripts/run-ui.sh
# 默认同时绘制候选窗与状态栏；候选窗只接管 super-ime，
# 其他输入法仍由 Fcitx5 classicui 绘制：
#   ./scripts/run-ui.sh --no-candidates   # 只显示状态栏

# 3. 用户目录安装（推荐，可干净卸载，随会话自启动）
./scripts/install-user.sh
# agent 加载后会自动拉起 superior_ui；不需要时设 SUPERIOR_UI_AUTOSTART=0

# 4. 每次重新编译后，完整重启（不用注销、不用重启电脑；脚本在仓库外）
../restart-ime.sh

# 5. 添加输入法：fcitx5-configtool 里选「先！进！输入法」
#    或直接切换
fcitx5-remote -s super-ime
```

### 改了代码之后怎么让它生效

**不用注销，也不用重启电脑**，但必须让两个进程都退出：

| 进程 | 为什么必须重启 |
|---|---|
| `fcitx5` | agent 是 `libsuperioragent.so`，以共享库形式加载进 Fcitx5，无法原地重载 |
| `superior_ui` | 只杀掉 Fcitx5 时旧浮层不会退出，它会重连到新 agent 并继续跑**旧版本**的 UI |

```bash
../restart-ime.sh          # 仓库外的本机辅助脚本，见下文等价命令
```

为什么重启必须写对：`FCITX_ADDON_DIRS` 若没导出，用户目录的 addon 会被静默丢掉
（`systemctl --user set-environment` 只对 systemd 拉起的进程生效，终端里起的
`fcitx5` 不继承它），症状是「输入法不见了」。等价命令：

```bash
kill $(pgrep -x superior_ui) 2>/dev/null; fcitx5-remote -e
export FCITX_ADDON_DIRS="$HOME/.local/lib/fcitx5:/usr/lib/$(gcc -print-multiarch)/fcitx5"
setsid fcitx5 -d
```

安装后得到两个不同的东西：

| 产物 | 类型 |
|---|---|
| `libsuperioragent.so` + `superioragent.conf` | `Category=Module` 观察者 addon（不是输入法） |
| `super-ime.conf` | 输入法条目，uniqueName = `super-ime`，绑定 `Addon=pinyin` |

新增条目必须**重启** Fcitx5 才会出现在“添加输入法”里；`fcitx5-remote -r` 不够，
`Controller1.ResetIMList` 会重建分组并丢掉用户条目，不要用。
详见 [docs/fcitx5-integration.md](docs/fcitx5-integration.md)。

验证窗口语义：

```bash
DISPLAY=:0 xwininfo -name "Superior-IME Status" | grep "Override Redirect"
DISPLAY=:0 xprop -name "Superior-IME Status" WM_HINTS _NET_WM_STATE
```

预期：`Override Redirect State: yes`、`Client accepts input or input focus: False`、
`_NET_WM_STATE_ABOVE`。细节见 [docs/xwayland-ui.md](docs/xwayland-ui.md)。

Fcitx5 agent 是 `Category=Module` 的被动观察者：不注册输入法、不抢 UI addon、
不修改预编辑。因此 Wayland 客户端继续使用 Fcitx5 的 wayland 前端，
X11 客户端继续使用 XCB/XIM 前端，应用输入框内的预编辑行为不变。

唯一例外是**内联英文模式**：轻按 Shift 后，字母由 agent 直接 `commitString`
上屏（这是搜狗式的中英文切换，不切换输入法、不加载英文键盘布局）；
`Ctrl` / `Alt` / `Super` 组合与功能键一律放行。规则见
[docs/input-panel.md](docs/input-panel.md)。

---

## 日志

全项目统一入口，无 Qt 依赖，Qt 消息也会汇入同一格式：

```bash
SUPERIOR_LOG_LEVEL=debug ./superior_ui
SUPERIOR_LOG="superior.protocol=trace,superior.ui=debug" ./superior_ui
SUPERIOR_LOG_FILE=~/.cache/superior-ime/log.txt ./superior_ui
SUPERIOR_LOG_VIA_QT=1 QT_LOGGING_RULES="superior.*=true" ./superior_ui
```

规则与安全约束见 [docs/logging.md](docs/logging.md)。核心原则：**不记录用户输入内容**，
预编辑/候选一律用 `redacted()` 只留长度。

---

## 词库

拼音引擎仍是 `fcitx5-pinyin` + `libime`，Superior 不复制词库格式，只按同样的
XDG 布局读取、检测、报告：

```bash
./build/linux-ui/tools/dict-tool/superior_dict diagnose
```

搜狗细胞词库导入使用与 fcitx5-pinyin 相同的工具链
（`scel2org5` → `libime_pinyindict`），产物落在
`~/.local/share/fcitx5/pinyin/dictionaries/`：

```bash
./build/linux-ui/tools/dict-tool/superior_dict import 网络流行.scel
fcitx5-remote -e && fcitx5 -d
```

细节见 [docs/dictionary.md](docs/dictionary.md)。

---

## Shift 中英文切换

轻按 Shift 在输入法内部切换中英文，与搜狗一致：

- 英文模式下字母、数字、符号直接上屏，不经过拼音；
- 长按 Shift 打大写字母、`Ctrl`/`Alt`/`Super` 组合键都不会触发切换；
- `Esc` 立刻回到中文；模式是**全局**的，切窗口不会改变；
- 关闭设置页的「Shift 切换中英文」后，Shift 完全归还给应用。

判定逻辑（`ShiftTapDetector`）在 `backend/platforms/fcitx5/src/ShiftTap.cpp`，
不依赖 Fcitx5 或 Qt，单测见 `tests/shift_tap_tests.cpp`。

---

## 设置

```bash
~/.local/bin/superior_settings      # 或从应用菜单打开「先！进！输入法设置」
# 真正的可执行文件在 ~/.local/share/superior-ime/bin/，~/.local/bin 里只是 200 字节的包装脚本
```

设置写在 `~/.config/superior-ime/settings.json`，**两个进程都轮询这个文件**，
所以改完 500 ms 内生效，不需要重启 Fcitx5，也不需要重启浮层：

| 键 | 作用 |
|---|---|
| `skinId` | 浮层重新加载皮肤 |
| `uiScale` | 候选窗与状态栏的字号、内边距、命中区一起缩放（0.75–3.0） |
| `showStatusBar` | 状态栏显隐（是设置项，不会被下一帧场景覆盖） |
| `statusPlacement` | `bottomRight` / `bottomLeft` / `topRight` / `topLeft` |
| `shiftToggle` | 内联中英文切换总开关 |
| `cloudPinyinEnabled` / `cloudPinyinBackend` | 云拼音开关与服务商，见 [docs/cloud-pinyin.md](docs/cloud-pinyin.md) |

状态栏是可点击的。点击只发语义命令 `ToggleStatusItem{id}`，agent 找到 Fcitx5
里同名的 action 并调用它——引擎状态仍由引擎决定，浮层永远不持输入状态，也不抢焦点。

| 元素 | 含义 | 点击 |
|---|---|---|
| 圆点 | 拖动手柄，见下节 | 拖动 |
| 中 / 英 | 内联中英文 | 切换中英文 |
| 🌙 月牙 | 半角标点 | 切到全角 |
| ● 实心圆 | 全角标点 | 切到半角 |

标点项画成图形而不是「半角标点」四个字，原因有两个：字宽会随着切换变化，
整条工具栏会跟着跳；而且这个状态本就适合用图形表达。形状由
`StatusGlyph.cpp` 决定，`active`（全角）画实心圆，否则画月牙；月牙是「圆减去
偏移的圆」，缺口用工具栏底色绘制而不是抠透明，否则会露出后面的窗口。缺口方向
从正右（0°）逆时针旋转 45°（`kMoonRotationDegrees`），所以月牙开口朝右上。

状态来源要注意：`punctuation` 这个 action 的 `isChecked()` 永远是 false，真正
的状态在它的图标名 `fcitx-punc-active` / `fcitx-punc-inactive` 里。所以
`statusIconIsOn()` 先匹配 `-inactive` 再匹配 `-active`——「inactive」本身就包含
「active」，只做子串搜索会把半角当成全角。

引擎的 `pinyin-prediction`（预测开关）被过滤掉，不出现在工具栏：它是配置项，
不是打字时要按的按钮。

### 组合文本（输入框）也是自己画的

搜狗 Linux 版的输入框是它自己绘制的，所以拼音、下划线、光标都长在皮肤里。
先！进！在 `super-ime` 上下文里做同样的事：

| 环节 | 做法 |
|---|---|
| 让引擎把组合文本交给我们 | 取走 `CapabilityFlag::Preedit`，引擎于是写 `InputPanel::preedit` 而不是 `clientPreedit` |
| 绘制 | 候选窗顶行：拼音 + 下划线 + 光标（`PreeditLayout.cpp`） |
| 归还 | 切到别的输入法 / 浮层断开时把 capability 加回去，客户端恢复内联预编辑 |
| 兜底 | 引擎仍只写 `clientPreedit` 时也照样显示（`PreeditSource.cpp`） |

这就是「在 xterm 里皮肤正常、在 VSCode 里皮肤不一样」的根因：VSCode 自己渲染
client preedit（用它的字体和颜色），我们的皮肤只能覆盖候选行。取走 capability 后
两处完全一致。

只移除 `Preedit` 一个 flag，归还时也只加回这一个 —— 不会覆盖别人在此期间改过的
`Password` / `Disable`。

### 候选窗

每个候选带序号（`1. 你好`）。序号优先用引擎自己的 label（因此 ①②③ 之类的
编号会保留），label 为空时用「位置 + 1」兜底；Fcitx5 只给了 label 没给文本时不会
渲染成 `1. 1`。格式化逻辑是纯函数：`frontend/desktop/src/CandidateLabel.cpp`，
单测 `tests/candidate_label_tests.cpp`。

- **点击候选**上屏：UI 发 `ActivateCandidate{session, candidateId}`，agent 用
  `candidateIndexFor` 把 id 换算成索引并**重新校验**（超出当前页就丢弃，不猜）。
  过期的帧点了也不会选中别的词。
- **滚轮翻页**：一格一页（`PageCandidates{delta}`），`hasNext`/`hasPrev` 为假时
  不动，不循环。触摸板的小 delta 被忽略，否则一次手势会翻过好几页。
- 悬停高亮来自点击热区的实测矩形，换了新场景就重置，不会高亮到别的词上。

命令只在 `session` 属于**当前聚焦**的输入上下文时才执行，所以窗口失焦后到达的
点击不会打到新窗口里。

### 拖动与位置记忆

状态栏只有**左侧的圆点**（拖动手柄）能拖：从「中」「全角标点」上按下永远只是点击，
不会误拖动。手柄在悬停时会高亮，光标变成手形。

松开鼠标时位置写入 `~/.config/superior-ime/statusbar.json`，下次启动直接还原：

```json
{ "schema": "superior.statusbar/1", "x": 600, "y": 395, "placement": "bottomRight" }
```

- 位置会被夹在屏幕可用区域内，多屏时按落点所在的屏幕计算；
- 在设置页重新选「位置」会**丢弃**拖动坐标（文件同时删除），否则那次选择看起来没有效果；
- 单独一个文件而不是塞进 `settings.json`：设置页写设置、浮层写坐标，互不覆盖。

窗口位置与缩放的纯计算在 `frontend/desktop/src/LayoutRules.{hpp,cpp}`，
单测见 `tests/layout_rules_tests.cpp`。

---

## 云拼音

默认关闭。开启后输入两个字母以上、停顿 200 ms，就会把**纯拼音**发给服务商，
用返回的词补一个候选；密码框与英文模式永不发送。

```bash
# 先确认当前网络能不能用（不改任何配置）
./build/linux-ui/tools/superior-config/superior_config_cli cloud probe --backend googlecn nihao
```

| 服务商 | 状态 |
|---|---|
| `googlecn`（默认） | 可用，直连约 0.2 s |
| `baidu` | 接口失效（固定返回 `errno 19`） |
| `sogou` | 无公开端点，暂不可用 |

实现与取舍（为什么不用线程、为什么状态以拼音为身份、为什么去重）见
[docs/cloud-pinyin.md](docs/cloud-pinyin.md)。

---

## 可选：把皮肤导出成 Fcitx5 主题

自绘候选窗不依赖 Fcitx5 主题。只有当你想让 **Fcitx5 自己的面板**（Rime 等）
也用同一套配色时，才需要导出：

```bash
./build/linux-debug/tools/skin-lint/skin_lint skins/default --strict \
    --export-theme ~/.local/share/fcitx5/themes/org.superior.default
```

然后在 classicui 配置里选 `org.superior.default`。这是全局设置，会影响所有
输入法；默认不写。详见 [docs/fcitx5-theme.md](docs/fcitx5-theme.md)。

---

## 皮肤工具

```bash
# 校验（warning 也视为失败）
./build/linux-debug/tools/skin-lint/skin_lint skins/default --strict

# 机器可读
./build/linux-debug/tools/skin-lint/skin_lint skins/default --json

# 打印规范化后的 spec（用于 diff / golden）
./build/linux-debug/tools/skin-lint/skin_lint skins/default --print-spec
```

退出码：`0` 干净，`1` 有诊断，`2` 用法或 I/O 错误。

---

## 目录

```
backend/
├─ api/        契约（事件、场景、命令、端口）
├─ skin/       皮肤模型 + 校验 + 哈希
├─ core/       纯逻辑
├─ runtime/    串行泵 + 票据
├─ protocol/   帧编解码 + Unix socket
└─ platforms/  fcitx5（观察者 agent）

frontend/
├─ app/        （后续）应用装配
├─ render/     （后续）离屏渲染
├─ desktop/    Qt 桌面壳：状态栏 + X11 候选窗
├─ mobile/     （预留）
└─ widgets/    （后续）共享控件

tools/skin-lint
skins/default
tests/
docs/
```

边界规则由 `scripts/check-boundaries.sh` 强制执行，并作为 ctest 用例运行：

- `frontend/` 不得依赖 `backend/core`、`backend/runtime`、`fcitx*`；
- `backend/core` 不得依赖 Qt / Fcitx5 / 墙钟；
- `backend/skin` 不得依赖 Qt 或做像素解码；
- `backend/api` 不得依赖任何实现。

---

## 许可

GPL-3.0-or-later，见 [LICENSE](LICENSE)。

内置皮肤为本项目自绘，不含任何第三方输入法的素材、字体或图标。
