# Superior-IME 实现计划

> 版本：v0.1（2026-10-23）
> 范围：GNU/Linux 桌面优先；Windows/macOS/Android 只预留接口，不进入本期实现。
> 状态：M0/M1 开始实现。

---

## 1. 目标与非目标

### 1.1 目标

1. **Linux 桌面可日常使用**：作为 Fcitx5 的输入法后端接入，输入与候选交给 Fcitx5 生态，Superior-IME 提供 UI、皮肤、语音与跨平台架构。
2. **UI 与逻辑彻底解耦**：UI 进程只依赖 `superior-api` / `superior-proto` / `superior-skin`，不允许绕过抽象层直接拿核心状态。
3. **皮肤自建、稳定、可校验**：定义并实现 `superior.skin/1`，声明式、版本化、可诊断、可确定性渲染。
4. **为移动端留出正确的抽象**：`UiProfile` / `UiCapabilities` 从第一天存在，但移动实现后置。
5. **可验证**：边界、确定性、协议、降级都有自动化测试。

### 1.2 非目标（本期）

- 不逆向、不分发搜狗 `.ssf` 资源；`.ssf` 导入器是 P2 的可选工具，不进入运行时。
- 不复用搜狗云端语音接口；语音使用本地模型（sherpa-onnx 等），P3 再落地。
- 不实现 Windows TSF / macOS IMKit / Android IME。
- 不在 fcitx5 进程内嵌 Qt 事件循环。

---

## 2. 关键结论（来自前期验证）

| 结论 | 影响 |
|---|---|
| `libwaylandim.so` 不导出 `getInputPopupSurface` | 第三方 UI addon 拿不到 Wayland input-popup 表面；P0 的 Wayland 候选窗走 classicui 主题，P1 再评估公开接口或私有 stub |
| X11 下 `InputContext::cursorRect()` 是全局坐标 | 外部 Qt 进程可直接精确定位候选窗，**不需要成为 UI addon** |
| Fcitx5 `UserInterfaceManager` 只有一个 `ui_` | 我们的 agent 使用 `Category=Module` 观察事件，与 classicui 共存 |
| Treeland 是 wlroots 系，支持 `wlr-layer-shell` | 右下角状态栏用 layer-shell 锚定，不抢焦点 |
| 本机具备 Fcitx5 5.1.12 dev、Qt 6.8（含 private dev）、wayland-scanner | 工具链可直接使用 |

---

## 3. 架构

### 3.1 前后端分离的目录与依赖规则

参照 `fl-scx` 的 `lib/backend`（纯后端）与 `lib/pages` + `lib/widgets`（纯前端）划分，
C++ 侧同样把**前端、后端、进程入口**分开放：

```
backend/                       纯后端：无 Qt、无窗口、可在 headless 下测试
├─ api/                        契约（INTERFACE）：事件、场景帧、命令、端口
├─ core/                       纯逻辑：候选、选中、翻页、可见性、语音状态
├─ runtime/                    串行泵、票据、代次隔离、sampleAt
├─ skin/                       皮肤模型 + 校验 + specHash（不解码像素）
├─ protocol/                   IPC 帧编解码与能力协商
└─ platforms/
   ├─ fcitx5/                  Fcitx5 适配（agent addon、classicui 主题导出）
   ├─ windows/                 （预留）
   └─ android/                 （预留）

frontend/                      纯前端：只依赖 backend/api、backend/protocol、backend/skin
├─ app/                        Qt 应用装配与 main()
├─ render/                     皮肤驱动布局、九宫格、字体度量、命中区、动画采样
├─ desktop/                    Qt Widgets 桌面壳（候选窗、状态栏、托盘）
├─ mobile/                     移动壳（预留）
└─ widgets/                    前后端无关的共享绘制控件

tools/                         开发工具（当前：skin-lint；计划：skin-init、skin-preview）
skins/                         内置皮肤
tests/                         单元、golden、边界测试
```

依赖方向（箭头 = 允许依赖）：

```
frontend/*  ──►  backend/api
     │       └►  backend/protocol
     │       └►  backend/skin        (只读模型)
     │
     ✗ 不得依赖 backend/core / backend/runtime / backend/platforms / fcitx*

backend/platforms/fcitx5  ──►  backend/runtime  ──►  backend/core  ──►  backend/api
                                                          └──────────►  backend/skin
```

硬规则（由 `scripts/check-boundaries.sh` + `nm` 检查强制）：

1. `frontend/` 不得 include 或链接 `superior/core`、任何 `fcitx*`。
2. `backend/core` 不得 include Qt / Fcitx5 / 平台头 / 系统时钟。
3. `backend/skin` 只做模型与校验，不解码像素、不依赖 Qt。
4. `backend/api` 是纯头文件契约，无实现依赖。
5. `backend/runtime` 私有链接 `backend/core`，公共头文件不出现 core 类型。
6. 前后端之间只能通过 `backend/api` 的类型与 `backend/protocol` 的帧通信。

### 3.2 运行时拓扑

```
Fcitx5
  ├─ libsuperiorpinyin  (Category=InputMethod, 内置 librime, 唯一输入法条目)
  └─ superior-agent  (Category=Module, 常驻, 无 Qt)
       ├─ 观察 InputContextUpdateUI / FocusIn / FocusOut / IM 切换
       ├─ 读取 InputPanel: preedit / candidates / cursorRect / capabilities
       ├─ 维护候选、选中、翻页、可见性（语义状态）
       └─ Unix socket (SOCK_SEQPACKET) ⇄ superior-ui
                                            ├─ X11: 候选窗 + 状态栏 (override-redirect)
                                            ├─ Wayland: 状态栏 (wlr-layer-shell)
                                            └─ 语音面板 / 设置
       ◄── UiCommand { ActivateCandidate{candidateId}, SetSkin{id}, VoiceStart, ... }
```

- **语义状态只在 agent**：候选内容、选中项、可见性、皮肤代次。
- **UI 拥有纯瞬时视觉状态**：hover、按下、tooltip 淡出、动画进度。
- **动画不走 IPC**：agent 只发 `Transition{from,to,spec}`，UI 本地采样。
- **IPC 鉴权**：`SO_PEERCRED` + 版本/能力协商；单实例。
- **降级**：UI 崩溃不影响输入；agent 继续提交文本，UI 自动重连并重建快照。

### 3.3 UI 交互契约

UI → agent 只能发**语义命令**：

```cpp
ActivateCandidate { uint64_t sessionEpoch; uint32_t candidateId; }
PageCandidates    { uint64_t sessionEpoch; int delta; }
SetSkin           { std::string skinId; }
SetMode           { Mode mode; }        // 中/英、全/半角、简/繁
VoiceStart        { VoiceOptions options; }
VoiceStop         { bool commit; }
MovePanel         { int dx; int dy; }   // 仅用户拖动，不是任意绝对定位
```

**不允许** `SetPanelRect{x,y,w,h}` 这类状态改写；窗口位置由平台后端根据 `cursorRect` + 用户偏移决定。

落选/翻页使用 `candidateId` 而非 index，避免异步竞态。

---

## 4. 皮肤：`superior.skin/1`

### 4.1 设计原则

- 纯声明式，无脚本、无表达式求值。
- 版本化：`schema` 固定；未知键 → warning（strict 下 error）；缺省键 → 内置默认。
- 确定性：动画只允许 `tween` / `spring` 参数或显式帧序列；不依赖墙钟。
- 安全：资源路径必须在包内，禁止绝对路径与 `..`。
- 可诊断：错误带 JSON Pointer + 稳定错误码。
- 可寻址：`specHash` 覆盖规范化 JSON + 资源内容哈希。

### 4.2 包结构

```
my-skin/
├─ skin.json
├─ preview.png
└─ assets/
   ├─ panel-h1.png
   ├─ bar.png
   └─ ...
```

P0 只支持目录；`.skin`（zip）在 P2 追加，`skin.json` 固定在根。

### 4.3 结构概要

```jsonc
{
  "schema": "superior.skin/1",
  "id": "com.example.default",
  "name": "默认",
  "version": "0.1.0",
  "modes": ["light", "dark"],
  "defaultMode": "light",

  "tokens": {
    "color.text": "#FFFFFF",
    "font.candidate": { "family": "Noto Sans CJK SC", "size": 16, "weight": 500 }
  },

  "panel": {
    "variants": [
      { "when": { "orientation": "horizontal", "layout": "single-row" }, "use": "h1" }
    ],
    "styles": {
      "h1": {
        "background": { "image": "assets/panel-h1.png",
                        "slice": { "left": 24, "top": 20, "right": 24, "bottom": 20 },
                        "fit": "stretch" },
        "padding": [8, 10, 8, 10],
        "preedit": { "font": "$font.candidate", "color": "$color.text" },
        "candidates": {
          "flow": "horizontal", "maxVisible": 5,
          "cell": { "padding": [2, 6, 2, 6] },
          "text": { "font": "$font.candidate", "color": "$color.text",
                    "firstColor": "$color.text.first" },
          "selected": { "background": { "color": "$color.selection", "radius": 6 } }
        }
      }
    }
  },

  "status": {
    "background": { "image": "assets/bar.png" },
    "items": [
      { "id": "ime", "kind": "toggle", "size": [22, 22],
        "icons": { "cn": "assets/cn.png", "en": "assets/en.png" } }
    ]
  },

  "voice": { "background": { "color": "$color.surface" } },

  "motion": {
    "panel.show": { "type": "spring", "stiffness": 420, "damping": 34, "mass": 1 }
  }
}
```

完整规范见 `docs/skin-format.md`。

### 4.4 职责切分

| 层 | 负责 | 不负责 |
|---|---|---|
| `superior-skin` | 解析、校验、token 解析、`SkinSpec`、`specHash`、资源清单 | 像素解码、Qt、窗口 |
| `superior-core` | 组合、候选、选中、翻页、可见性、皮肤 id、语音状态 | 字形度量、像素坐标 |
| `ui/render` | 皮肤驱动布局、字体度量、九宫格绘制、命中区、动画采样 | 候选内容、翻页决策 |

布局放 UI：跨进程拿不到可靠字形度量；点击仍回传 `candidateId`，由 core 校验代次。

---

## 5. 目录结构

```
Superior-IME/
├─ CMakeLists.txt
├─ CMakePresets.json
├─ PLAN.md
├─ cmake/
│  ├─ SuperiorOptions.cmake
│  └─ SuperiorWarnings.cmake
├─ scripts/
│  ├─ bootstrap.sh
│  ├─ configure.sh
│  ├─ build.sh
│  ├─ test.sh
│  ├─ dev-install.sh
│  └─ check-boundaries.sh
├─ backend/
│  ├─ api/
│  │  ├─ CMakeLists.txt
│  │  └─ include/superior/api/{Ids,Content,Scene,Commands,Ports,Capabilities}.hpp
│  ├─ skin/
│  │  ├─ CMakeLists.txt
│  │  ├─ include/superior/skin/{Types,Diagnostic,Skin}.hpp
│  │  └─ src/{Diagnostic,Skin}.cpp
│  ├─ core/                    语义状态机 ✅
│  ├─ runtime/                 串行泵 + 票据 ✅
│  ├─ protocol/                （M2.5）IPC 帧
│  └─ platforms/
│     ├─ fcitx5/               （M3）agent addon
│     ├─ windows/              （预留）
│     └─ android/              （预留）
├─ frontend/
│  ├─ app/                     （M3）Qt 应用装配 + main()
│  ├─ render/                  （M3）离屏渲染
│  ├─ desktop/                 （M3）Qt Widgets 壳
│  ├─ mobile/                  （预留）
│  └─ widgets/                 （M3）共享控件
├─ tools/
│  └─ skin-lint/
├─ skins/
│  ├─ default/
│  └─ high-contrast/           （M1 后续）
├─ tests/
└─ docs/
```

---

## 6. 里程碑

| 里程碑 | 内容 | 验收标准 |
|---|---|---|
| **M0 骨架** | CMake、presets、脚本、`superior-api`、边界检查 | `configure/build/test` 全绿；边界脚本能对故意注入的违规失败 |
| **M1 皮肤** | `superior.skin/1` 规范、loader/validator、`specHash`、`skin-lint`、`skins/default` | 默认皮肤零 error；坏样本给出精确 pointer；strict 模式 warning 即失败；hash 稳定 |
| **M2 核心与运行时** ✅ | `superior-core` 候选/选中/翻页/会话状态、`superior-runtime` 串行泵/票据/帧合并 | 单元测试覆盖取消、过期结果、确定性采样 |
| **M2.5 协议** ✅ | `backend/protocol`：帧、`Hello`/`Scene`/`Command`、Unix socket、单测 | 往返、坏帧、截断、socket 广播/命令 |
| **M3.5 可观测性** ✅ | `backend/diagnostics`、Qt 日志桥、埋点 | 日志单测；不记录用户输入 |
| **M3 X11 端到端** ✅ | `fcitx5/agent`、IPC、Qt 候选窗/状态栏、内联英文 | agent 无 Qt、只读；X11 浮层 override-redirect 已实测；候选窗/状态栏自绘已实测 |
| **M4 Wayland** 🚧 | XWayland 平台自动选择、设置热重载、layer-shell 状态栏、classicui 主题导出 | 自动选择、热重载、可点击状态栏已完成（17 个 ctest 全绿）；layer-shell 待做 |
| **M5 收尾** | `dev-install`、文档、打包骨架 | 新用户按文档 5 分钟跑通；`fcitx5-diagnose` 可见 addon |
| **P2 可选** | `tools/ssf-import` | `.ssf` → `superior.skin/1`，不进入运行时依赖 |
| **P3 语音** | sherpa-onnx 本地流式 ASR | 流式部分结果、最终提交、取消、断网可用 |

---

## 7. 本期实现范围（M0 + M1）

> 状态：已完成（2026-10-23）。`scripts/configure.sh && scripts/build.sh && scripts/test.sh` 全绿，
> `linux-werror` preset 也通过。

### 7.1 交付

1. 构建系统：CMake ≥ 3.25、Ninja、C++20、presets（debug/release/asan）。
2. 脚本：`bootstrap` / `configure` / `build` / `test` / `check-boundaries`。
3. `libs/superior-api`：会话键、场景帧、命中区、命令、端口契约（头文件）。
4. `libs/superior-skin`：
   - `Diagnostic` / `Severity`
   - 类型：`Color`、`Font`、`Insets`、`ImageBrush`、`PanelStyle`、`StatusStyle`、`VoiceStyle`、`MotionSpec`
   - `loadSkin(path, options) -> LoadResult`
   - token 解析与循环检测
   - 资源路径校验与清单
   - `specHash`（规范化 JSON + 资源内容哈希）
   - `scanSkins(root)` 轻量发现
5. `tools/skin-lint`：人读与 `--json` 输出、`--strict`、退出码约定。
6. `skins/default/skin.json` + 最小资源（SVG，自绘）。
7. 测试：valid / unknown key / bad color / token cycle / traversal / hash stability / strict 模式。
8. `docs/skin-format.md`。

### 7.2 明确不做

- 不接 Fcitx5、不写 Qt 窗口、不解码像素。
- 不实现 `.ssf` 导入。
- 不实现 zip 皮肤包。

---

## 8. 风险与对策

| 风险 | 对策 |
|---|---|
| Wayland 候选窗受私有接口限制 | P0 用 classicui 主题；私有 stub 可选编译、运行时版本校验 |
| Qt 授权与仓库 GPL-3.0 一致性 | 动态链接 Qt LGPL/GPL 组件；未来闭源需商业授权 |
| 皮肤格式演进破坏兼容 | `schema` 版本 + 未知键忽略 + 缺省值 + golden 测试 |
| 跨进程延迟 | 控制帧低频；动画本地采样；热路径零额外 IPC |
| 字形度量差异 | UI 侧测量并缓存；不把像素坐标写进 core |
| 发行版 Qt 私有头差异 | 不在 P0 依赖 Qt 私有头；layer-shell 自实现 |

---

## 9. 决策记录

| 编号 | 决策 | 理由 |
|---|---|---|
| D-01 | 自建 `superior.skin/1`，不把 `.ssf` 放进运行时 | 稳定、可校验、无合规风险 |
| D-02 | agent 用 `Category=Module`，不抢 UI addon | Fcitx5 只能有一个 UI addon；保证与 classicui 共存 |
| D-03 | 语义状态在 agent，视觉瞬时状态在 UI | 单一真相 + 低延迟交互 |
| D-04 | 点击用 `candidateId`，不用 index | 避免异步竞态 |
| D-05 | 动画传 `Transition`，UI 本地采样 | 避免高频 IPC |
| D-06 | `superior-skin` 不解码像素 | 可在无 Qt 环境测试；渲染归 UI |
| D-07 | 布局归 UI，语义归 core | 跨进程无法可靠共享字形度量 |
| D-08 | P0 不做 zip 皮肤包 | 降低依赖；目录形式已够用 |

---

## 10. 2026-10-29 复盘：Telegram + 云拼音卡顿

### 10.1 现象

Telegram（Flatpak，X11/XIM）里快速打字时，字母像排队一样逐个出现，候选框也跟着一格一格刷新；`fcitx5-rime` / `fcitx5-pinyin` 不卡，VSCode 等走 GTK/IBus 路径的客户端也不明显。关闭云拼音，或把 `cloudPinyinMinLength` 调高到不会发请求时，输入立刻流畅。

### 10.2 两个根因

1. **每个键都重建 librime session**。Telegram 的 XIM 客户端会在每个键上触发 `XResetIC`，旧 `RimeInputMethod::reset()` 走 `forget()` → `destroy_session()` → `create_session()`，一次约 30 ms；日志表现为每个字母前都出现一对 `rime engine stopped` / `rime engine ready`。
2. **云拼音的 40 ms tick 退化成了事件循环忙循环**。Fcitx5 的 `addTimeEvent(clock, usec, accuracy, callback)` 是**单次**时间源，第三个参数是精度而不是周期；旧代码把它当周期定时器，并在 `updateCloudTimer()` 里反复 `setEnabled(true)`。deadline 留在过去时，sd-event 会不停回调 `pumpCloud()`。单次 `pumpCloud` 很快，所以慢日志抓不到，但 XCB/XIM 的按键分发被饿死：X 事件时间相差几十到几百毫秒，到达 agent 却稳定约 200 ms 一个，偶尔积压数秒后一次性 burst。

**关键教训：云拼音“异步”不等于“不会卡输入”。** 网络请求确实没有同步等待，但错误的定时器调度会占满 Fcitx5 主循环，效果和阻塞一样；而且它伤害的是 XIM 的同步转发队列，所以只有 Telegram 这类客户端最明显。

### 10.3 修复

- `RimePort::clearComposition()` / `RimeEngine::reset()`：用 librime `clear_composition` 原地清空组合，不再重建 session；`activate` / `deactivate` / `reset` 保留面板 custom callback。
- `Fetcher`：HTTP 传输移到独立工作线程，请求/结果队列 + generation 取消；析构停止并 `join`，保证 addon 被 `dlclose` 时线程不会活过库。
- `updateCloudTimer()`：回调内 `source->setNextInterval(kCloudTickUs)` 显式续期；空闲时 `setEnabled(false)`；`cloudPumping_` 防止在回调里重复启用；`OverlayController` 的 socket 重试定时器同样改为显式续期。
- 云结果到达只刷新我们的浮层 scene，不再对 XIM 客户端做异步 `updateUserInterface`；后台 context 的结果留在其面板，不抢当前浮层。
- 回归测试：`testResetClearsCompositionInPlace()` 固定“reset 不重建 session”的约定；全量 `ctest` 40/40 通过。

### 10.4 当前状态（2026-10-29）

- GNU/Linux X11 路线可日常使用：Fcitx5 内加载 `libsuperioragent.so` + `libsuperiorpinyin.so`，Qt 浮层负责候选窗/状态栏，`superior_speech_recognition` 负责语音子进程。
- 拼音使用内置 librime + 万象方案；`superior-config` 可做 `rime status/deploy/lookup` 和 `cloud probe`。
- 云拼音默认关闭；`googlecn` 实测可用，`baidu` 端点已失效，`sogou` 仍是占位。当前实现不再插占位符，结果到达后才插入真实候选。
- 用户目录安装（`~/.local`）可用：`scripts/install-user.sh` 写入 `~/.local/lib/fcitx5`、`~/.local/share/fcitx5`、`~/.local/share/superior-ime`，并通过 `~/.config/environment.d/90-superior-ime.conf` 设置 `FCITX_ADDON_DIRS`。
- 已实测：Telegram（XIM）连续输入/云候选正常；VSCode 等非 XIM 客户端正常；40/40 ctest 通过。
- 未完成/边界：Wayland layer-shell 候选窗仍是 P1；Windows/macOS/Android 只有接口预留；语音为 P3 本地识别路线；`docs/` 里仍有一些早期架构描述与实现漂移，后续按模块收敛。
