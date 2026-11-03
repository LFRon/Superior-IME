# 架构

> **权威文档是仓库外的 [`架构设计.md`](../../架构设计.md)**（v3）。
> 本文只描述当前代码目录与运行拓扑；两者冲突时以 `架构设计.md` 为准。

第一原则：**组合属于能力，路由属于内核，宿主只做翻译与执行。**

---

## 1. 目录与职责

```
backend/
├─ api/                  契约：KeyPress / CompositionFact / Composer / 端口 / 场景 / 命令
├─ core/                 纯策略与纯状态：decideKey、decideGesture、Modes、CandidateModel、
│                        TextOutbox、ImeSession、SpeechState
├─ runtime/              内核：SessionRegistry、InputRouter、CapabilityRegistry、
│                        EffectQueue、设置、语音状态；消费 TextSessionPort /
│                        ScenePresenter / Scheduler / LifecyclePort
├─ pinyin/               拼音能力：RimeComposer（实现 api::Composer）、
│                        CandidatePipeline（engine + learned + cloud）、
│                        LearningStore、CloudService、LibrimePort
├─ cloudpinyin/          传输与解析：百度/搜狗/Google 的请求形状与报文校验
├─ speech-recognition/   语音识别：采集、识别子进程协议、会话服务
├─ config/               设置、学习词、状态栏状态（纯数据）
├─ paths/                Roots 纯数据（解析由宿主注入）
├─ protocol/             SUPI 帧编解码（传输在宿主里）
├─ skin/ theme/          皮肤模型与主题导出（纯数据）
├─ diagnostics/          结构化日志
└─ platforms/
   ├─ fcitx5/            Fcitx5 宿主：Fcitx5Addon（兼组装根）、TextSession、
   │                     OverlayController、SpeechProcess、SpeechController
   ├─ linux/             Linux 平台实现：音频采集
   └─ stub/              无平台依赖的桩：第二个 TextSessionPort 实现

frontend/
├─ desktop/              Qt 桌面壳：候选窗、状态栏、语言徽标、语音面板、X11/Wayland 分派
├─ settings/             设置窗口
├─ render/ widgets/ app/ 预留：共享渲染与装配
└─ mobile/               预留：移动壳

tools/                    scene-sim、skin-lint、superior-config
tests/                    单元、golden、conformance、真机脚本
skins/                    内置皮肤
```

---

## 2. 运行时拓扑

```
Fcitx5
  └─ libsuperiorpinyin.so  (Category=InputMethod, OnDemand)
       ├─ Fcitx5Addon      宿主 + 组装根
       │    ├─ 键盘/焦点/光标/能力位 → api::KeyPress / api::HostFacts
       │    ├─ TextSession           → api::TextSessionPort
       │    ├─ ScenePresenter        → SUPI 帧 → unix socket
       │    ├─ NativePanelSink       → fcitx::InputPanel（可选）
       │    └─ Scheduler             → fcitx 事件循环
       ├─ runtime::Runtime        每个输入框一个 Session
       │    ├─ core::ImeSession / core::Modes / core::TextOutbox
       │    ├─ 按键 → core::decideKey → 能力
       │    ├─ 手势 → core::decideGesture → 语言/语音
       │    └─ UI 命令 → 候选/翻页/模式/语音
       └─ pinyin::RimeComposer
            ├─ LibrimePort / RimeEngine
            ├─ CandidatePipeline（engine + learned + cloud）
            └─ CompositionFact

superior_ui（独立进程）
  ├─ SceneClient  ← SUPI 帧
  ├─ CandidateWindow / StatusWindow / SpeechRecognitionPanel
  └─ 命令 → SUPI Command → 宿主 → 内核
```

**组合只有一个生产者**：`pinyin::RimeComposer` 产出 `CompositionFact`。
浮层和 Fcitx5 原生候选列表都是它的派生；宿主不读回自己的面板。

---

## 3. 为什么只有一个 addon

v2 的两个 addon（观察者 + 引擎）之间没有类型化接口：引擎建
`fcitx::CandidateList`，观察者读 `InputPanel`、用 `toModifiable()` 插云端/学习词，
再用 `list` 指针和文本比对认自己的行。这带来三个问题：

1. 组合状态有两个 owner（`InputPanel` 与 `CandidateModel`）；
2. 云端/学习只能存在于 Fcitx5 适配器里，Windows/Android 必须重写；
3. commit 事件到达时 preedit 已清空，学习只能靠「组合可见时快照」的时序 hack。

v3 把宿主、内核、能力放进同一个 addon，用 `api::Composer` 切分职责：
宿主只翻译和执行，能力拥有组合，内核拥有路由。

---

## 4. 场景帧与命令

### 4.1 下行：`UiSceneFrame`

一帧是**完整期望画面**，不是增量：丢帧永远安全。包含：

- `panel`：可见性、锚点、皮肤代次、preedit、候选页、选中 id
- `status`：状态栏位置与语义项
- `speech`：语音面板阶段、模型、电平、partial、剩余时间
- `indicator`：工具栏隐藏时 Shift 切换中/英的「拼」/「A」徽标
- `transition`：动画数据（UI 按自己的 vsync 采样）

### 4.2 上行：`UiCommand`

语义命令，不含绝对几何、候选列表或引擎内部状态：

- `ActivateCandidate{session, id}`：点击候选（id 是代次 + 位置）
- `PageCandidates{session, delta}`
- `SelectIndex{session, index}`
- `SetInputMode{session, mode}`
- `ToggleStatusItem{id}`
- `SpeechStart/Stop/Cancel/Reload`
- `ShellReady/ShellLost`、`SetSkin`、`SetUiScale`

宿主把 socket 收到的帧解码后**只调用** `runtime.command()`；它不自己解释命令。

---

## 5. 平台后端

### 5.1 Fcitx5

- `Fcitx5Addon` 实现 `fcitx::InputMethodEngineV2` 与全部 `api` 端口；
- `TextSession` 是唯一写客户端文本的地方；
- `OverlayController` 管理 UI 进程与 socket；
- `NativePanelSink` 把 `api::NativePanel` 画进 `fcitx::InputPanel`；
- `StatusSink` 把 runtime 的状态项映射成 Fcitx5 tray action。

### 5.2 X11 / Wayland（UI 进程）

`frontend/desktop/src/platform/` 是唯一出现 XCB / libwayland 的地方：

- X11 统一实现（XWayland 算 X11）；
- Wayland 按桌面分派（KDE/GNOME/…）；
- 候选窗自行定位，不依赖桌面约定；`PanelChannel` 区分
  `Applied` / `Unsupported` / `Unreachable`。

### 5.3 Windows / Android（预留）

`backend/platforms/{windows,android}/README.md` 给出起点。验收标准：
新宿主写完后 `backend/api`、`backend/core`、`backend/runtime`、`backend/pinyin`
零改动；`tests/conformance` 全绿。

---

## 6. 当前实现状态

| 模块 | 状态 |
|---|---|
| `backend/api` | ✅ v3 契约：Key/Composition/Composer/Host/Ports |
| `backend/core` | ✅ 纯策略与状态；类型别名指向 `api` |
| `backend/runtime` | ✅ Session、InputRouter、模式、设置、语音路由、效果 |
| `backend/pinyin` | ✅ RimeComposer、CandidatePipeline、LearningStore、CloudService |
| `backend/platforms/fcitx5` | ✅ 单 addon 宿主 + 组装根；原生候选 + 浮层 |
| `backend/platforms/linux` | ✅ 音频采集 |
| `backend/platforms/stub` | ✅ 第二个 TextSessionPort，conformance |
| `frontend/desktop` | ✅ Qt 浮层、X11/Wayland 分派 |
| `frontend/settings` | ✅ 设置页 |
| `backend/platforms/{windows,android}` | ⏳ v3.5 |
| `api::ProcessPort` | ⏳ v3.3（语音子进程仍是 POSIX） |
