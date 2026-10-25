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

## 组合文本由谁绘制

皮肤要统一，组合文本（拼音那一行）就必须由我们画，而不是客户端画。Fcitx5 的
约定是：引擎同时填两个字段，由 `CapabilityFlag::Preedit` 决定客户端会不会收到
`clientPreedit`。

```
PinyinEngine::preedit()  ← fcitx5-pinyin/im/pinyin/pinyin.cpp
  mode = ic->capabilityFlags().test(Preedit) ? config.preeditMode : No
  ComposingPinyin → 只填 clientPreedit   ← 客户端画，字体颜色不受我们控制
  No              → 只填 preedit        ← 我们画
```

所以同一个皮肤在 xterm（无 client preedit）和 VSCode（有 client preedit）里长得不一样：
VSCode 用它的字体把 `ni hao` 画在编辑器里，我们的窗口只剩候选行。

做法（`SuperiorAgent::takePreeditOwnership`）：

1. 接管面板时取走当前上下文的 `Preedit` capability → 引擎改走 `No` 分支，
   组合文本进 `InputPanel::preedit`；
2. `CandidateWindow` 顶行绘制：正文、整行下划线、光标（`PreeditLayout.cpp`
   按 UTF-8 边界切分）；
3. 切到其他输入法、浮层断开、上下文销毁时 `releasePreeditOwnership`，
   只把 `Preedit` 加回去；
4. 引擎若仍只写 `clientPreedit`，`choosePreedit` 会兜底使用它，绝不让组合文本消失。

归还时只加回 `Preedit` 而不是恢复整份快照：期间 `Password` / `Disable` 这类 flag
可能被别人改过，整体覆盖会把预编辑泄漏到密码框。

代价与边界：

- 我们不支持 inline 组合的应用（多数终端、VSCode 等）现在统一由浮层显示，
  这正是搜狗 Linux 版的行为；
- 输入法切走时立刻恢复，Rime 等继续使用它们自己的 client preedit 行为；
- 浮层没连上时（`server_.clientCount() == 0`）不接管 capability，用户仍能看到
  客户端自己的预编辑。

---

## 候选窗交互

候选窗是可点的，但每一次点击都会回到 agent 做校验：

| 交互 | UI 侧 | agent 侧 |
|---|---|---|
| 点击候选 | `ActivateCandidate{session, candidateId}` | `candidateIndexFor(id, size)` → `candidate(index).select(ic)` |
| 滚轮 | `PageCandidates{session, ±1}` | `toPageable()`；`hasNext`/`hasPrev` 后才 `next()`/`prev()` |
| 悬停 | 本地高亮，不发命令 | — |

两条硬规则：

1. **id 不是索引**。场景里的 `candidateId` 是 `index + 1`，agent 用实时列表重新
   换算；超出范围就丢弃并记日志。过期帧上的点击不会选中「现在正好在那个位置」的词。
2. **session 必须属于当前聚焦的输入上下文**。`contextFor()` 同时校验 epoch 与
   context id，窗口失焦后才到达的点击不会打进新窗口。

`CandidateList::size()` 仍然是**当前页**的数量，所以 `candidateIndexFor` 只在本页
内换算——翻页本身由 `PageCandidates` 完成，不靠 id 越界。

### 候选序号

渲染为 `1. 你好`。优先使用引擎给 `CandidateList::label(index)` 的值（所以 ①、
a.、自定义编号都保留），仅在此值为空时用「位置 + 1」；引擎只给 label 不给 text 时
不编号，避免出现 `1. 1`。格式化是纯函数，见
`frontend/desktop/src/CandidateLabel.cpp`。

---

## 工具栏：图形化与可点击

工具栏项来自 Fcitx5 的 `StatusArea`，但**不是照抄**：适配层把 action 名映射成语义
角色（`backend/platforms/fcitx5/src/StatusMapping.cpp`），UI 只认角色，不认识
Fcitx5 的 action 名。

| action | 角色 | 呈现 |
|---|---|---|
| `punctuation` | `Punctuation` | 月牙（半角）/ 实心圆（全角） |
| `pinyin-prediction` | 过滤 | 不显示 |
| 其它 | `Text` | 原样显示 `shortText` |

点击处理：UI 发 `ToggleStatusItem{id}` → agent 在 `StatusArea` 里按名查找 action →
调用 `action->activate(ic)` → 请求 `StatusArea` 重绘并刷新场景。引擎状态从不被 UI
直接改写，punctuation / fullwidth 的语义仍留在引擎里。

### 状态从哪里读

`punctuation` 的 `isChecked()` 恒为 false（它是 `ToggleAction`，但没有暴露 checked
状态），真正的状态在图标名：
`fcitx-punc-active`（全角）/ `fcitx-punc-inactive`（半角）。

`statusIconIsOn()` 因此按**后缀**判断，且先判 `-inactive`：「inactive」本身包含
「active」，子串搜索会把半角误判成全角，图标就永远不翻。

### 为什么画图形

「半角标点」4 个字和「全角标点」4 个字宽度不同（前者还更宽），每次切换整条工具栏
都会跳一下。图形尺寸固定，切换时只换形状。

月牙的绘制是「外圆减去偏移的内圆」，缺口用工具栏底色填，而不是
`CompositionMode_Clear`——抠成透明会在皮肤上留一个洞，露出后面的窗口。

缺口方向从正右（0°）逆时针转 45°（`kMoonRotationDegrees`），月牙开口朝右上。
只旋转「偏移方向」而不旋转画布：圆减圆本身旋转对称，转方向与转画布结果相同，
但少一层 save/rotate/restore。`moonBiteDirection()` 是纯函数，45° 与 90° 都有
单测。

---

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
