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

`backend/platforms/fcitx5/src/Fcitx5Addon.cpp`：

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

候选窗的位置有三个来源，按准确度取用；完整规则与实测数据见下面的
「候选窗位置」一节。状态栏始终固定在屏幕角落（或用户拖到的位置）。

## 组合文本由谁绘制

皮肤要统一，组合文本（拼音那一行）就必须由我们画，而不是客户端画。Fcitx5 的
约定是：引擎填 `preedit`，客户端有 `CapabilityFlag::Preedit` 时还会收到
`clientPreedit` 并用自己的字体画在输入框里。

所以只要客户端声明了这个能力，同一个皮肤在 xterm（无 client preedit）和 VSCode
（有 client preedit）里就会长得不一样：VSCode 把 `ni hao` 画进编辑器，我们的窗口
只剩候选行。

做法（`Fcitx5Addon::takePreeditOwnership`）：

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
| `punctuation` | `Punctuation` | 月牙（半角）/ 实心圆（全角）；内置 Rime 引擎注册，切换 `ascii_punct` |
| `pinyin-prediction` | 过滤 | 不显示（其它引擎的预测开关；当前引擎没有） |
| `superior-toolbar` | 过滤 | 只出现在 Fcitx5 托盘菜单（见下节） |
| `superior-speech` | 过滤 | 只出现在 Fcitx5 托盘菜单（见下节） |
| 其它 | `Text` | 原样显示 `shortText` |

点击处理：UI 发 `ToggleStatusItem{id}` → agent 在 `StatusArea` 里按名查找 action →
调用 `action->activate(ic)` → 请求 `StatusArea` 重绘并刷新场景。引擎状态从不被 UI
直接改写，punctuation / fullwidth 的语义仍留在引擎里。内置 Rime 引擎的
`punctuation` action 会把 `ascii_punct` 应用到所有会话，再由 Rime 的 punctuator
决定输出 `，` 还是 `,`。

### 状态从哪里读

内置 Rime 引擎的 `punctuation` action 是 `SimpleAction`，`isChecked()` 表示全角，
并同步设置 `fcitx-punc-active`（全角）/ `fcitx-punc-inactive`（半角）图标；旧的
Fcitx 标点模块只翻图标、不暴露 checked，所以两种来源都要认。

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

## 右键菜单：用 Fcitx5 托盘图标的那一个

三个开关住在 **Fcitx5 托盘图标的右键菜单**里，我们**不画自己的菜单**。

### 为什么只能这样

`notificationitem` 的菜单结构是硬编码的（`dbusmenu.cpp`），没有任何第三方扩展点：

```
(0,0) ├ 输入法组（>1 组才有）
      ├ 输入法列表
      ├ ──分隔线──
      ├ ← 当前输入上下文 statusArea 里每个 id()!=0 的 action
      ├ ──分隔线──（有 action 才加）
      ├ 配置 / 重启 / 退出
```

所以「出现在托盘菜单里」= 「注册一个 action 并挂进我们上下文的 statusArea」，
除此之外没有第二条路。外框、字体、配色都是桌面环境的，我们只贡献条目。

### 我们贡献的三项

| 菜单项 | action | 勾选状态来自 | 点击后 |
|---|---|---|---|
| 全角标点 | `punctuation` | 引擎的 `isChecked()` | 引擎切 `ascii_punct` |
| 显示工具栏 | `superior-toolbar` | `settings.json: showStatusBar` | 写回该键 |
| 语音输入 | `superior-speech` | `core::SpeechState::visible()` | 开关语音面板 |

`Fcitx5Addon::registerActions()` 在启动时注册这两个 checkable action，
`attachTrayActions()` 在 `refresh()` 里把它们挂到**属于我们的**上下文上——别的
输入法的托盘菜单不该出现我们的开关。

勾选状态问的是 owner 自己，不是界面的缓存：`speech_` 现在是
`SpeechController`（P5.4 从 agent 拆出），它持有 `core::SpeechState` 与识别子进程，
`visible()` / `enabled()` 就是那两个开关的真相。拆出去的好处是可以拿假能力、
假时钟测：`tests/speech_controller_tests` 覆盖「面板没装好不该起进程」「密码框拒绝
录音」「取消后迟到的结果不许上屏」「子进程死掉/阶段卡死要复位面板」「没焦点时
先暂存再补上」这些规则，不需要麦克风，也不需要 Fcitx5。

### 两个必须记住的细节

1. **菜单只在状态区重绘时重新读勾选**。`notificationitem` 只在
   `InputContextFlushUI` 且 `component == StatusArea` 时 `updateMenu()`，所以
   `refreshTrayActions()` 一旦发现值变了就 `updateUserInterface(StatusArea)`。
2. **托盘点击是「裸」的 `Action::activate`**，不经过我们的 UI。引擎侧因此必须
   在 `FlushUI(StatusArea)` 里也刷新一次场景，否则工具栏的月牙会停在旧状态，
   直到下一次按键。这就是 `InputContextFlushUI` 同时处理 `InputPanel` 与
   `StatusArea` 的原因。

### 工具栏显隐的唯一 owner

仍然是 `settings.json` 的 `showStatusBar`。agent 从托盘菜单改它，UI 侧不轮询，而是
用 `QFileSystemWatcher` 盯着 `~/.config/superior-ime/` 这个**目录**，写进来就
`setEnabled()`。不另开 IPC，也不允许两边各写一次。

盯目录而不是盯文件：`config::saveSettings()` 是「写 `.tmp` 再 `rename`」，每次保存
都换掉 `settings.json` 的 inode，盯文件的 watch 第一次保存就废了。目录 watch 对
`rename` 进入目录的事件照样触发。只有 inotify 不可用、或配置目录还不存在时才退回
500 ms 轮询，而且每个 tick 都会重试挂 watch，挂上就自动停表。

为什么不能只靠 Qt 信号槽：托盘点击发生在 **fcitx5 进程**里的 `Action::activate`，
UI 在 `superior_ui` 进程里，信号槽跨不过进程边界。inotify 是「同一个事件语义」在
跨进程下的等价物——写入那一刻唤醒事件循环，而不是等下一个 tick。

实测（`CLOCK_MONOTONIC` 打点，两进程共用一个时钟）：点击 → agent 收到 `Activated`
约 149 ms，`Activated` → 写盘 → UI `hide()` → X `UnmapNotify` 只要 **0.5 ms**。
这 149 ms 全在 fcitx5 自己的 `notificationitem` 里（`dbusmenu.cpp` 故意
`addTimeEvent(+30 ms)` 等输入上下文拿到焦点，剩下是它事件循环的派发开销）——
换成「键盘 - 英语（美国）」这类纯 Fcitx5 菜单项也是 149 ms。所以**不要**再去
优化这一段，我们这条管线已经没有可压缩的部分了。

### 「全角标点」为什么标签固定

`shortText` 就是菜单里那一行的文字，所以它必须描述**开关**而不是**当前状态**：
勾=全角、不勾=半角。早先它写成「半角/全角」随状态翻转，菜单里就出现「半角」配
一个没勾的圆点，看不出点下去会变成什么。工具栏不受影响——那一格画的是图形不是
文字。

---

## 候选窗位置：三条来源，按准确度排序

| 优先级 | 情况 | 位置来自 |
|---|---|---|
| 1 | Treeland + 浮层跑在 XWayland（默认选择） | **合成器**（`_DEEPIN_IM_CANDIDATE_PANEL`），它用应用自己的 `set_cursor_rectangle` 定位 |
| 2 | X11 / XIM 客户端（含 XWayland） | Fcitx5 的 `cursorRect()`，我们自己贴着光标放 |
| 3 | 原生 Wayland 客户端 / 其它 Wayland 桌面 | 右下角固定位置（协议边界，见下） |

三条来源由平台层统一选择，窗口自己不做判断：`platform/Platform.cpp` 的
`compositorPlacesCandidatePanel()` 是唯一的表（见下一节末尾）。

### Treeland：把面板交给合成器

Treeland（`IMCandidatePanelManager`）提供了一条**输入法候选面板**通道：

- Wayland 客户端：`xdg_toplevel.set_tag("org.deepin.treeland.im-candidate-panel")`
  （当前用不了：Qt 不暴露它自己建的 `xdg_toplevel*`，见上一节的 `Unreachable`）
- XWayland 客户端：X11 属性 `_DEEPIN_IM_CANDIDATE_PANEL`（`CARDINAL`，值 1）

被采纳的窗口会被移进 popup 容器、去掉装饰、移出任务栏、不接受键盘焦点，并且
**由合成器按输入框光标矩形定位**——这正是外部进程在 Wayland 上拿不到的信息。

两个实现约束（都踩过）：

1. **必须是受管窗口**。override-redirect 窗口不被 WM 管理，Treeland 在自己的容器里
   查不到它，属性写了也没人处理（`xprop` 能读到属性，但 `WM_STATE: not found` 暴露了
   真相）。因此这种模式下候选窗改用普通无边框窗口，置顶/不抢焦点由采纳方提供。
2. **不要在组合之间 unmap**。采纳是按“已映射的 surface”记的，隐藏会让它失效；空闲时
   窗口缩成 1×1 且不着色，既不可见又保持被采纳。

### 已知的合成器侧行为（不是我们能修的）

Treeland 对一次几何变化的回应**不是唯一答案**。实测同一组合内：

```text
11:15:07.428 apply (pos 1471,878)        ← 上一轮组合的位置
11:15:07.429 compositor → 1691,899
11:15:07.430 compositor → 1468,878       ← 1ms 后给了另一个答案
11:16:18.866 compositor → 1531,899
11:16:19.611 compositor → 1441,878       ← 落定 700ms 后又改
```

两个 y（899 / 878）相差一行，来自不同的 text-input 焦点面：VSCode 这类应用有多个
可输入 surface，`WInputMethodHelper::textInputCursorRect()` 会在它们之间取值。
该通道没有 serial/时间戳，客户端无法分辨「中间答案」和「最终答案」，所以首帧偶尔会
落在上一轮的位置上——**这是合成器侧的排序/归属问题**，客户端只能延迟绘制，无法修正。

### 平台层怎么选：先分窗口系统，再按桌面分派

Linux 后端的目录结构就是这条分派规则，`frontend/desktop` 只认识门面
（`platform/Platform.hpp`），不认识 XCB、libwayland 或任何桌面私有约定：

```text
platform/Session.{hpp,cpp}      窗口系统 + 桌面，纯函数 classifySession()，可单测
platform/Scale.hpp              比例算术与来源枚举（无平台头文件）
platform/Platform.{hpp,cpp}     门面：detectSession / readSystemScale / 面板通道
platform/x11/                   一个实现，XWayland 也算 X11
        X11Scale.cpp            Xft.dpi（X 资源数据库）
        X11Panel.cpp            _DEEPIN_IM_CANDIDATE_PANEL
platform/wayland/               按桌面分派，因为“贴光标”没有共同答案
        WaylandScale.cpp        wl_output.scale
```

**窗口系统优先于会话类型。** `QT_QPA_PLATFORM=wayland;xcb` 在 Wayland 会话里加载的
是 `xcb`，浮层就有 X11 窗口、X11 像素、X11 坐标，缩放必须读 `Xft.dpi`；反过来把
Wayland 的值套到 XWayland 窗口上会算出 1.6 倍。判据只有 `classifySession()` 一处。

| 窗口系统 | 桌面 | 候选面板通道 |
|---|---|---|
| X11 | Treeland | `Applied` — 写 `_DEEPIN_IM_CANDIDATE_PANEL` |
| X11 | 其它 | `Unsupported` — 没有这个约定，用 `cursorRect()` |
| Wayland | Treeland | `Unreachable` — 约定存在，但 Qt 不暴露自己建的 `xdg_toplevel*`，拿不到 tag |
| Wayland | 其它 | `Unsupported` — 协议上就没有 |

`Unreachable` 与 `Unsupported` 分开，是因为两者的下一步不同：前者只要换回 XWayland
就能用，后者换通道也没用。日志里会打一行 `linux platform backend windowSystem=…
desktop=… panelChannel=…`。

### 光标锚点：什么算“客户端报了光标”

`InputContext::cursorRect()` 给的是**矩形**，但光标是**一条线**：宽度天然为 0。所以
判定标准不是“是不是一个正常矩形”，而是“客户端到底报没报”：

| 前端 | 实测 `cursorRect` | 结论 |
|---|---|---|
| GTK（`dbus`） | `x, y, 0, 34` | 有光标 |
| XIM（xterm） | `x, y, 0, 0` | 有光标（只有位置） |
| Wayland（`waylandim`） | `0, 0, 0, 0` | **没有**，全零就是“从没设置过” |

只有全零才算没报。曾经要求 `width > 0 && height > 0`，结果所有 X11 客户端都被判成
“没有光标”，候选窗一律退到屏幕角落 —— 位置永远错。`SUPERIOR_LOG=superior.fcitx5=trace`
会打出 `cursorKnown/cursorX/cursorY/cursorW/cursorH`，这是排查“位置不对”的第一手数据。

### 锚点怎么换算成窗口位置

1. **按屏幕换算**，不是按桌面：Fcitx5 给的是设备像素，Qt 移动窗口用逻辑像素，而两个
   缩放比不同的屏幕不共享同一个比例。`caretToLogical()` 先把每块屏幕按
   `geometry × dpr` 还原成设备矩形，用光标的设备坐标选出所在屏幕，再从那块屏幕自己的
   原点换算（`logical = 屏幕原点 + (设备 − 设备原点) / dpr`）。全局除以
   `primaryScreen()` 的比例，在第二块屏上会偏，选屏也会选错。
2. **优先放在光标下方**，下方放不下就翻到光标上方：最后一行终端、聊天框底部正是最不
   该被候选窗盖住的位置。
3. 结果最后夹到该屏幕的 `availableGeometry()` 里，避开任务栏/面板。

### 其它 Wayland 桌面：没有任何来源

没有 Treeland 的通道时，Wayland 原生客户端既不给 Fcitx5 光标矩形
（`src/frontend/waylandim` 在 5.1.12 里没有一处 `setCursorRect`），客户端自己也不知道
桌面坐标。外部进程要贴光标只有一条路：`zwp_input_method_v2` 的
`get_input_popup_surface`，而那个对象只在 fcitx5 进程内。此时候选窗退到右下角固定位置，
拼音本身仍显示在候选窗里；X11/XWayland 客户端不受影响。

---

## 内联英文模式（Shift 切换中英文）

搜狗的 Shift 是**输入法内部**的中英文切换，不是切换输入法、也不是加载英文键盘
布局。这里照做，但只影响 `super-ime` 上下文：

| 环节 | 实现 |
|---|---|
| 判定"轻按" | `ShiftTapDetector`（`backend/platforms/fcitx5/src/ShiftTap.cpp`） |
| 拦截点 | `InputContextKeyEvent`，`PreInputMethod` 阶段 |
| 上屏 | 英文模式下 `commitString(text)`，并 `event.filterAndAccept()` |
| 中文模式 | 立刻 `return false`，按键原样交给 Rime 引擎 |
| 模式归属 | **全局**（`englishMode_`），切窗口不改变，与搜狗一致 |
| 退出 | `Esc`，或再次轻按 Shift |
| 关闭 | 设置页「Shift 切换中英文」→ `shiftToggle`，500 ms 内热生效 |
| 组合中轻按 | 上屏已输入的字母**并切到英文**（`shiftCommitPinyin`，默认开）；关掉后仍按上一行只切换中英文，字母随之丢弃、不上屏 |

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
