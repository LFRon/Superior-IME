# 自绘输入窗：不改 Fcitx5 全局皮肤

## 结论

先！进！输入法**不再修改 Fcitx5 的经典界面主题**。候选窗由我们自己的
Qt 进程绘制，皮肤来自 `superior.skin/1`。其他输入法继续使用它们原来的外观。

这与搜狗输入法 Linux 版的做法一致：它以普通 `Category=InputMethod` 插件接入，
候选窗由自有前端绘制，而不是替换 classicui 的全局主题。

## Fcitx5 提供的接管点

`InputPanel::setCustomInputPanelCallback()`（Fcitx5 5.0.24+）：

> When this is set, `Instance::updateUserInterface(InputPanel)` will trigger a
> call to the callback function instead. The capability flag
> `ClientSideInputPanel` will not be respected, but the clientPreedit will still
> be sent.

`UserInterfaceManager` 的分发顺序是：

```cpp
if (ic->inputPanel().customInputPanelCallback()) {
    callback(ic);            // 我们
} else if (ui_) {
    ui_->update(InputPanel); // classicui
}
```

所以只要在**当前输入法**上设置回调，classicui 就不会再画这个上下文的候选窗；
其他输入法（Rime、五笔、键盘）不受影响。

## 我们的实现

`backend/platforms/fcitx5/src/SuperiorAgent.cpp`：

```cpp
if (instance_->inputMethod(ic) == "super-ime") {
    panel.setCustomInputPanelCallback([this](fcitx::InputContext *ctx) { refresh(ctx); });
} else {
    panel.setCustomInputPanelCallback({});
}
```

- 在 `InputContextInputMethodActivated` / `SwitchInputMethod` / `FocusIn` 时安装，
  保证**第一帧**就路由到我们；
- 在 `InputMethodDeactivated` / `ContextDestroyed` 时清除，把界面还给 Fcitx5；
- 回调里只做一件事：把最新状态通过 socket 发给 UI 进程。

> 早期版本在 `InputContextFlushUI` 里才安装回调。Fcitx5 先 `postEvent(FlushUI)`
> 再 `updateDispatch()`，事件是延迟派发的，所以第一帧仍由 classicui 画出，
> 而它此后不再收到更新，窗口就永远留在屏幕上。必须在激活阶段安装。

## 为什么不用全局主题

| 方案 | 影响面 | 结论 |
|---|---|---|
| 导出 classicui 主题并写 `classicui.conf` | **所有**输入法、菜单、托盘 | 已放弃（用户反馈：没必要） |
| 自定义输入窗（本方案） | 仅 `super-ime` 上下文 | 采用 |
| 替换 UI addon | 全局唯一 UI，classicui 无法共存 | 不采用 |

`theme` 模块仍保留：`superior-config theme install` 可以把皮肤导出成 classicui
主题，供只想用 Fcitx5 原生候选窗的用户选择，但**不再是默认路径**，设置页也不再
自动写入 `classicui.conf`。

## 皮肤如何生效

`frontend/desktop`：

1. 读取 `~/.config/superior-ime/settings.json` 的 `skinId`；
2. 在用户目录与系统目录中查找该皮肤并 `loadSkin`；
3. 每帧根据 `panel.styleId`（h1/v1）取出 `PanelStyle`；
4. `SkinTheme::from()` 提取颜色、圆角、内边距、选中色；
5. `CandidateWindow` 按光标位置绘制，`StatusWindow` 用同一套配色。

皮肤缺失时使用内置的深色回退值，不会崩溃。

## 位置

`CandidateWindow` 使用 `Anchor::cursor`（Fcitx5 报告的插入点）放在光标下方，
并夹在屏幕可用区域内。X11 下这是全局坐标；Wayland 原生客户端可能拿不到坐标，
此时回退到右下角。状态栏始终固定在屏幕角落。

## 内联英文模式（Shift 切换中英文）

搜狗的 Shift 是**输入法内部**的中英文切换，不是切换输入法、也不是加载英文键盘
布局。这里照做，但只影响 `super-ime` 上下文：

| 环节 | 实现 |
|---|---|
| 判定"轻按" | `ShiftTapDetector`（`backend/platforms/fcitx5/src/ShiftTap.cpp`） |
| 拦截点 | `InputContextKeyEvent`，`PreInputMethod` 阶段 |
| 上屏 | 英文模式下 `commitString(text)`，并 `event.filterAndAccept()` |
| 中文模式 | 立刻 `return false`，按键原样交给 pinyin 引擎 |
| 模式归属 | **全局**（`englishMode_`），切窗口不改变，与搜狗一致 |
| 退出 | `Esc`，或再次轻按 Shift |
| 关闭 | 设置页「Shift 切换中英文」→ `shiftToggle`，500 ms 内热生效 |

`ShiftTapDetector` 的规则（全部有单测，`tests/shift_tap_tests.cpp`）：

- 按下到松开 < 400 ms 才算"轻按"，长按不切换；
- Shift 按住期间按了任何其它键（例如打大写 `A`）→ 视为修饰键，不切换；
- `Shift_L` / `Shift_R` 分开记账：两个一起按、只松一个不会误切换；
- X11 的 Shift 自动重复不会重置计时窗口；
- 焦点移出时 `reset()`，避免带着"按住"状态进下一个窗口。

英文模式下**只**接管纯文本输入：

- `Ctrl` / `Alt` / `Super` 组合一律放行，快捷键不受影响；
- `Return` / `Tab` / 方向键 / `Home` / `End` 放行；
- `BackSpace` / `Delete` **完全不拦截**。

最后一条是踩过坑的结论：英文模式的字符是我们用 `commitString` 直接交给客户端的，
字符已经属于应用，删除就该由应用自己做。早期版本在英文模式下拦截 Backspace 并调用
`deleteSurroundingText(-1, 1)`，结果在**不支持 surrounding text 的客户端**（绝大多数）
上按键被吞掉、什么都没删——也就是“英文模式按 Backspace 没反应”。
现在 Backspace 原样下传，撤销栈、选区、原生控件的行为都保持正确。

切到英文时先 `resetCompose()` + `inputPanel().reset()`，丢掉未完成的拼音，
否则旧候选会留在面板上，而字母已经直接上屏了。

---

## 验证

```bash
# 只应看到我们的窗口；classicui 的窗口是 1x1 且 IsUnMapped
DISPLAY=:0 xwininfo -root -tree | grep -E 'Superior-IME Candidates|Fcitx5 Input'
DISPLAY=:0 xwininfo -id <fcitx-window-id> | grep 'Map State'
```

实测（Deepin 25 / X11 会话）：

```
0x560001c "Superior-IME Candidates": 332x44+116+151
0x8200003 "Fcitx5 Input Window": 1x1+0+0
Map State: IsUnMapped
```

## 限制

- 我们只接管 `super-ime` 这一个条目的候选窗；切到 Rime 等仍由 classicui 绘制。
- Wayland 原生应用的光标坐标受 compositor 限制，XWayland 浮层可能无法精确跟随；
  这是既有结论，不是本方案的回归。
- 候选分页由 Fcitx5 决定：`CandidateList::size()` 是**当前页**数量，
  页数来自 `PageableCandidateList::totalPages()`。两者都原样透传。
