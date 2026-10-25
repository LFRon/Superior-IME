# XWayland 状态栏与“不干扰输入”约束

本文记录桌面浮层的窗口策略、验证方法和边界条件。它回答两个问题：

1. 右下角状态栏如何做到**永久置顶且不抢焦点**；
2. 为什么这套 UI **不会影响应用输入框里的预编辑/候选**。

---

## 1. 窗口策略

`frontend/desktop` 的浮层统一使用：

```cpp
setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint |
               Qt::WindowStaysOnTopHint | Qt::BypassWindowManagerHint |
               Qt::WindowDoesNotAcceptFocus);
setAttribute(Qt::WA_ShowWithoutActivating);
setAttribute(Qt::WA_TranslucentBackground);
setFocusPolicy(Qt::NoFocus);
```

在 Qt 的 XCB 平台插件中，`Qt::BypassWindowManagerHint` 会设置
`XCB_CW_OVERRIDE_REDIRECT`，而 `Qt::ToolTip` 还会自动补上
`WindowStaysOnTopHint | FramelessWindowHint | X11BypassWindowManagerHint`。

因此窗口是 **override-redirect**：

- compositor 不会把它当成普通顶层窗口重新堆叠；
- 点击不会触发窗口管理器激活，不会把焦点从文本客户端拿走；
- 不出现在任务栏/窗口列表；
- 位置完全由应用自己决定。

运行脚本或直接启动都会走 XWayland：

```bash
QT_QPA_PLATFORM=xcb DISPLAY=:1 ./scripts/run-ui.sh
```

`superior_ui` 已经内置这个选择（`frontend/desktop/src/PlatformChoice.cpp`）——
agent 自动拉起 overlay 时不会经过任何脚本，所以必须由进程自己决定：

| 情况 | 选择 | `reason` |
|---|---|---|
| `SUPERIOR_UI_PLATFORM` 非空 | 原样使用 | `override` |
| Wayland 会话 + `DISPLAY`，且 `QT_QPA_PLATFORM` 已是 X11 插件（含 `xcb`，如 Deepin 的 `dxcb;xcb`） | 保留该值 | `session-x11-plugin` |
| Wayland 会话 + `DISPLAY` | `xcb` | `xwayland` |
| Wayland 会话、无 `DISPLAY`，`QT_QPA_PLATFORM` 非空 | 保留该值 | `session-default` |
| Wayland 会话、无 `DISPLAY` | `wayland` | `wayland-only` |
| X11 会话，`QT_QPA_PLATFORM` 非空 | 保留该值 | `session-default` |
| X11 会话，仅有 `DISPLAY` | `xcb` | `x11` |
| 都没有（无头环境） | 交给 Qt | `default` |

要点：**Wayland 会话下 `QT_QPA_PLATFORM` 即使等于 `wayland` 也会被覆盖成 `xcb`**。
全局导出 `QT_QPA_PLATFORM=wayland` 不是针对这个浮层做的选择，照做会静默丢掉
“永不置顶失效”这个核心保证。真要原生 Wayland，用 `SUPERIOR_UI_PLATFORM=wayland`
或 Qt 自己的 `-platform wayland` 参数。

判定在 `QApplication` 构造**之前**执行，并通过 `qputenv` 写入，日志里会打印
`platform choice=... reason=...`。用例见 `tests/qt_platform_tests.cpp`。

Wayland 原生会话下同样的 flags 只能退化成普通无边框窗口；真正的 Wayland
状态栏需要 `wlr-layer-shell`，属于后续后端（见 §5）。

候选窗由我们自己绘制（`setCustomInputPanelCallback`，见
[docs/input-panel.md](input-panel.md)），classicui 不再为 `super-ime` 画候选。

---

## 2. 实测验证

环境：Deepin 25 / Treeland / XWayland `:1` / Qt 6.8 `xcb`。

```bash
$ DISPLAY=:1 xwininfo -id 0x2600007
  Absolute upper-left X:  1699
  Absolute upper-left Y:  987
  Width:  208
  Height: 34
  Map State: IsViewable
  Override Redirect State: yes

$ DISPLAY=:1 xprop -id 0x2600007 WM_HINTS _NET_WM_WINDOW_TYPE _NET_WM_STATE
WM_HINTS(WM_HINTS):
        Client accepts input or input focus: False
_NET_WM_WINDOW_TYPE(ATOM) = _NET_WM_WINDOW_TYPE_TOOLTIP, _KDE_NET_WM_WINDOW_TYPE_OVERRIDE, _NET_WM_WINDOW_TYPE_NORMAL
_NET_WM_STATE(ATOM) = _NET_WM_STATE_ABOVE
```

`xwininfo -root -tree` 中浮层位于 X 根窗口子树的顶部；候选窗与状态栏都不抢焦点。

> Treeland 下 override-redirect 窗口的**像素合成/定位**偶有异常
> （例如抓图得到黑块、窗口被原生层遮挡）。这属于 compositor 的实现细节，
> 不影响上面的语义结论：窗口确实是 override-redirect、`_NET_WM_STATE_ABOVE`、
> 且 `WM_HINTS` 明确声明不接受输入焦点。

---

## 3. 为什么不会影响输入

Superior 的后端是一个 **被动观察者**：

| 约束 | 实现 |
|---|---|
| 不注册输入法引擎 | addon 的 `Category=Module`，没有 `InputMethodEngine` |
| 不抢占 UI addon | 不做 `UserInterface`，与 classicui 共存 |
| 不消费按键（中文模式） | 只监听 `InputContextFlushUI` / focus / cursorRect 事件 |
| 不改预编辑 | 只读 `InputPanel::preedit()` 与 `candidateList()` |
| 不改客户端预编辑 | 从不调用 `setClientPreedit` / `setPreedit` |
| 英文模式例外 | 仅在用户轻按 Shift 后 `commitString` 纯文本；`Ctrl`/`Alt`/`Super` 与导航键放行 |

因此：

- Wayland 原生应用：预编辑仍由 **Fcitx5 的 wayland 前端**通过 text-input 协议处理；
- X11 应用：预编辑仍由 **Fcitx5 的 XCB / XIM 前端**处理；
- Superior UI 只是另一个进程，通过 Unix socket 接收“要画什么”。

UI 进程崩溃、重启、卡住，都不会阻塞按键或提交；最坏情况是状态栏不更新。

---

## 4. 进程与数据流

```text
Fcitx5 进程
  ├─ 拼音/Rime 引擎（原样）
  ├─ waylandim / xcb / xim 前端（原样，负责客户端预编辑）
  └─ superioragent (Category=Module, 无 Qt)
        └─ Unix socket: $XDG_RUNTIME_DIR/superior-ime.sock
              └─ superior-ui (Qt, 自动选择 xcb)
                    ├─ 右下角状态栏（override-redirect）
                    └─ 候选窗（override-redirect，随光标定位）
```

socket 权限为 `0600`，`SO_PEERCRED` 校验同 UID，单实例最多 8 个连接。

---

## 5. 后续：Wayland 原生浮层

若要摆脱 XWayland：

| 方案 | 适用 | 状态 |
|---|---|---|
| `wlr-layer-shell` | Treeland / wlroots 系 | M4 |
| `zwp_input_method_v2` popup | 候选窗，需 Fcitx5 私有接口 | 评估中 |
| classicui 主题 | Wayland 候选窗的 P0 方案 | 已有 |

`frontend/desktop/src/Overlay.cpp` 是唯一的窗口策略入口；增加 layer-shell
后端时不应改动 `StatusWindow` / `CandidateWindow` 的状态逻辑。

---

## 6. 边界情况

- **不要**把状态栏做成 `Qt::Window` 或普通 `QWidget` 顶层窗口：会进入窗口管理器栈，可能抢焦点。
- **不要**在 X11 候选窗里调用 `activateWindow()` / `raise()`：会打断文本客户端。
- **不要**让 UI 直接改 Fcitx5 的 `InputPanel`：那会破坏“双前端”语义。
- 多屏、分数缩放、旋转需要 compositor 配合；当前按主屏 `availableGeometry()` 定位。
