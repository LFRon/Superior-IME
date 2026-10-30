# 浮层缩放：一个比例，两个平台来源

候选窗、状态栏、语音面板都是 **override-redirect** 的独立 Qt 进程窗口。
它们的大小只有一个输入：`setScale(ratio)`。这个比例从哪里来、怎么跟随系统
缩放，是本文的全部内容。

---

## 1. 为什么不能直接用 Qt 的 `devicePixelRatio`

Qt 在**进程启动时**把每个窗口的 DPR 定死，而这个值不一定等于桌面真正的缩放：
X11 下它来自启动那一刻的 `Xft.dpi`，可能已经被写坏或过期（实测会话里 DDE 的
`scale-factor` 是 1.0，`Xft.dpi` 却是 120）。

浮层是常驻进程，一旦它拿到的是旧值，就会一整天都比别的应用小或大。所以缩放值由
我们自己向桌面要，不依赖 Qt 的缓存；**只在启动时读一次**——X11 下改缩放本来就
需要注销重登，浮层会随会话一起重启，没有运行时刷新的必要。

---

## 2. 分层

```
frontend/desktop/src/main.cpp              组合根：算出唯一比例，交给 setScale()
        │
        ├─ platform/ScreenScale.{hpp,cpp}      纯算术 + 运行时选择（不含任何 X11/Wayland 头）
        │      ├─ scaleForWindows()            比例补偿（见 §3）
        │      └─ parseXftDpi()                X 资源解析（纯函数，可单测）
        │
        ├─ platform/ScreenScaleX11.cpp         X11：读 X 资源的 `Xft.dpi`
        └─ platform/ScreenScaleWayland.cpp     Wayland：读 `wl_output.scale`
```

策略层（`StatusWindow` / `CandidateWindow` / `SpeechRecognitionPanel`）**只知道比例**，
不认识 `Xft.dpi`、`wl_output` 或 `QScreen`。这与 `架构设计.md` 的 P-6 一致：
平台细节留在最外层。

X11 与 Wayland 的实现由 CMake 按库是否存在决定是否编入：

- `xcb` → `SUPERIOR_HAVE_XCB_SCALE`
- `wayland-client` → `SUPERIOR_HAVE_WAYLAND_SCALE`

两个都没有时，浮层照常工作，只是比例恒为 1（等于“跟随不了”），不会编译失败。

---

## 3. 比例怎么算

```
applied = uiScale × systemScale / toolkitDpr
```

| 项 | 来源 |
|---|---|
| `uiScale` | `settings.json`，用户滑杆（0.75–3.0） |
| `systemScale` | X11：`Xft.dpi / 96`；Wayland：`wl_output.scale` |
| `toolkitDpr` | `QGuiApplication::primaryScreen()->devicePixelRatio()`，Qt 已经自动乘到每个逻辑像素上 |

**为什么要除 `toolkitDpr`**：Qt 已经把启动时的 DPR 应用到了窗口上。如果再乘一次
`systemScale`，在 125% 桌面上就会得到 156% 的窗口。相除之后，最终屏幕尺寸恒等于
`systemScale × uiScale`，无论 Qt 缓存的是 1.0、1.25 还是后来跟上了新值：

| 桌面 | Qt DPR | uiScale | applied | 最终 |
|---|---|---|---|---|
| 1.25 | 1.25 | 1.0 | 1.0 | 1.25 |
| 1.50 | 1.25（过期） | 1.0 | 1.2 | 1.50 |
| 1.50 | 1.50（已更新） | 1.0 | 1.0 | 1.50 |
| 1.25 | 1.25 | 1.5 | 1.5 | 1.875 |

---

## 4. 各平台怎么拿

### X11（含 Wayland 会话下的 XWayland）

`Xft.dpi` 是 X 桌面上事实标准的缩放声明：Xft、GTK、Qt 都读它，DDE/GNOME/KDE 的
“缩放”最终都会写进 X 资源数据库。实现：

1. 建一条 XCB 连接读一次（进程生命周期只读一次，不常驻、不轮询）；
2. 取根窗口的 `RESOURCE_MANAGER` 属性（`xrdb` 写的就是它），必要时回退到
   `SCREEN_RESOURCES`；
3. `parseXftDpi()` 解析 `Xft.dpi:`，范围限制在 48–960 dpi。

读不到就返回 1.0 —— X11 的默认 DPI 就是 96。

### Wayland

`wl_output.scale` 是协议规定的输出缩放。浮层本身跑在 XWayland 上（override-redirect
只能在 X11 语义下实现），但会话的缩放是合成器说了算，所以只要
`WAYLAND_DISPLAY` 存在就优先读它：

1. `wl_display_connect` + `wl_registry` → 绑定每个 `wl_output`；
2. 读 `scale` 事件，取所有输出里的最大值（多屏混合缩放时取大的，保证可读）；
3. `wl_display_roundtrip` 带 400 ms 超时（`poll` + `dispatch`），合成器卡住时不会
   把浮层一起冻住；
4. 结果在进程内只读一次。

---

## 5. 什么时候读

`main.cpp` 在建立三个窗口之前调用一次 `readSystemScale()`，把结果算成一个比例交给
`setScale()`，并打一条 INFO：

```text
overlay scale resolved reason=startup source=x11-xft-dpi raw=120
    system=1.250 toolkitDpr=1.250 userScale=1.010 applied=1.010
```

之后只在 `settings.json` 变化时重算（用户拖滑杆）。桌面缩放变化**不轮询**：
X11/XWayland 下改缩放本来就要注销重登，浮层随 Fcitx5 一起重启，启动时自然会读到
新值；轮询只会给主线程加一份没人需要的开销。

---

## 6. 验证

```bash
# 当前解析结果
grep 'overlay scale resolved' ~/.local/state/superior-ime/ime.log | tail -1

# 实际窗口尺寸（设备像素 = 逻辑尺寸 × 桌面缩放）
xwininfo -name "Superior-IME Status" | grep -E 'Width|Height'

# 桌面缩放变了：注销重登，或至少重启 Fcitx5（浮层会随 agent 一起重启）
../restart-ime.sh
```

纯算术部分有单测：`tests/ui_scale_tests.cpp`（`ctest -R ui-scale`），覆盖
`Xft.dpi` 解析、非法值、补偿公式与来源名称。

---

## 7. 已知边界

- 用户滑杆 `uiScale` 与系统缩放是**相乘**关系：125% 桌面 + 150% 滑杆 = 187.5%。
  `clampScale` 把最终比例限制在 0.75–3.0。
- 桌面缩放只在启动时读取：改完缩放要注销重登（X11 的常规做法），或者至少执行
  `../restart-ime.sh` 重启 agent 与浮层。
- 多屏不同缩放时取最大值，浮层固定在主屏右下角/用户拖到的位置，不做逐屏切换。
- 系统缩放只影响本项目的浮层；Fcitx5 classicui 主题、其他输入法的外观不受影响。
