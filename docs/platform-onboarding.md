# 平台对接手册（新增一个宿主）

> 本文回答一个问题：**要在 Windows / Android / 任何新宿主上跑起这个输入法，我要实现什么、
> 按什么顺序、怎么算做完？**
>
> 分层判据以仓库外的 [`架构设计.md`](../../架构设计.md) 为准（§6 端口、§6.9 宿主职责、
> §11 P5、附录 D）；本文只做一件事：把那些判断翻译成**签名级的施工说明**。
> 两者冲突时以 `架构设计.md` 为准，并修正本文。
>
> 每一条断言都以当前代码为依据，标注了文件名。**发现本文与代码不一致时，先信代码。**
> 判据是机械的：`scripts/check-boundaries.sh` 与 `tests/` 会跑红。

**这份手册的诚实前提**：端口不是一次长齐的。今天真正能实现、能被验收的只有
`TextSessionPort` 与 `AudioSource`；另外三个（`Scheduler` / `ScenePresenter` /
`LifecyclePort`）**已声明但零消费者**，`StoragePort` **还不存在**。§2 逐个说清楚该做什么、
不该做什么 —— 给空接口写实现是 §9.3 明令禁止的事（「抽象只由第二个真实消费者驱动」）。

---

## 1. 一小时骨架

新宿主的日子好过，因为有一个**只依赖 `Superior::Api` 的完整实现**可以抄：

```
backend/platforms/stub/
├─ include/superior/stub/StubPorts.hpp   RecordingTextSession / SilentAudioSource
├─ src/StubPorts.cpp                     无平台的真实实现
└─ src/ApiProbe.cpp                      api/ 全头文件编译探针
```

它同时也是契约探针：`Superior::PlatformStub` 只链 `Superior::Api`，所以**哪天
`api/` 长出平台词汇（fd / HWND / keysym / QObject），这个模块第一个编不过**。

### 1.1 上手顺序（照抄 `架构设计.md` 附录 D.1，这里给出可执行细节）

| 步 | 做什么 | 完成信号 |
|---|---|---|
| 1 | 目录：`backend/platforms/<os>/`，`CMakeLists.txt` 链 `Superior::Api` + 平台 SDK | `cmake --build` 出一个空 `.so`/`.dll` |
| 2 | `TextSessionPort::apply`（§3.1） | `tests/conformance` 全绿；记事本里打出 `Hello`，焦点切换后无残留 |
| 3 | 键盘回调 → `core::decideKey` / `core::decideGesture`（§4.1） | `Ctrl+C` 透传；`Backspace` 不被吃；Shift 轻按切英文 |
| 4 | `SessionKey` 铸造 + `HostFacts` 采集（§4.2、§4.3） | 候选窗跟着光标走，密码框不弹语音 |
| 5 | 场景下发（照抄 `protocol::SceneServer`，§4.4） | UI 进程能画出第一帧 |
| 6 | 能力装配：拼音 → 语音 | 打 `nihao` 出「你好」；连说两段语音两段都到位 |
| 7 | `check-boundaries.sh` + `tests/conformance` | 全绿即完成 |

第 5 步刻意排在能力之后：先接 UI 会让人误以为「链路通了」，而**上屏是唯一必须成功的
通道**（`架构设计.md` §3.5）。先把字打出来，再谈窗口。

### 1.2 骨架代码（Windows 为例，形状示意）

```cpp
// backend/platforms/windows/include/superior/windows/WinTextSession.hpp
#pragma once
#include <windows.h>
#include <msctf.h>

#include "superior/api/Ports.hpp"

namespace superior::windows {

/// api::TextSessionPort 的 TSF 实现。整个仓库里只有它往客户端写文本。
class WinTextSession final : public api::TextSessionPort {
public:
    /// 宿主注入两个查询，端口自己不认识焦点：
    /// `resolve` 回答「这个 key 现在指向哪个 ITfContext」，`destination` 回答
    /// 「当前焦点会话的 key」。返回 nullptr 是正常情况，不是错误（§6.7）。
    using Resolver = std::function<ITfContext *(const api::SessionKey &)>;
    using Destination = std::function<api::SessionKey()>;

    WinTextSession(Resolver resolve, Destination destination);

    void apply(const api::SessionKey &session, std::span<const api::TextEdit> edits) override;

private:
    void applyOne(ITfContext &context, const api::TextEdit &edit);
    Resolver resolve_;
    Destination destination_;
};

} // namespace superior::windows
```

```cpp
// apply 的骨架：三件事，顺序不能换
void WinTextSession::apply(const api::SessionKey &session, std::span<const api::TextEdit> edits) {
    if (edits.empty()) {
        return;
    }
    // 1. key 只是提示：无效 key = 「光标现在在哪就写到哪」（语音需要这个语义）
    ITfContext *context = session.valid() ? resolve_(session) : resolve_(destination_());
    if (context == nullptr) {
        return;                       // 陈旧点击就这么丢掉，不报错、不重定向
    }
    // 2. 一整批 edits 一次同步完成：塞进**同一个** ITfEditSession，
    //    不要每个 edit 开一个 —— 那会在中间让出主循环，顺序就不再保证。
    // 3. 返回时客户端已经拿到文本（TSF 的异步由实现内部吸收，不外泄给决策层）。
    for (const api::TextEdit &edit : edits) {
        applyOne(*context, edit);     // 三种 Kind 见 §3.1
    }
}
```

`applyOne` 的三种 `Kind` 各有**唯一正确的处理**，见下一节。这是最容易写错的地方，
也是两个历史 bug 的来源。

---

## 2. 端口清单：现在到底有什么

| 端口 | 状态 | 你要做什么 |
|---|---|---|
| `api::TextSessionPort` | ✅ 接口 + 两个实现（fcitx5 / stub） | **必须实现**（§3.1） |
| `api::AudioSource` | ✅ 接口 + 两个实现（linux / stub） | 有麦克风就实现（§3.2）；没有就 `SilentAudioSource` |
| `api::Scheduler` | ⚠️ 已声明，**0 消费者** | **不要实现**。宿主直接用自己的事件循环（§4.5） |
| `api::ScenePresenter` | ⚠️ 已声明，**0 消费者** | **不要实现**。照抄 `protocol::SceneServer`（§4.4） |
| `api::LifecyclePort` | ⚠️ 已声明，**0 消费者** | **不要实现**。降级目前写日志（§4.6） |
| `api::StoragePort` | ❌ 不存在 | 用 `paths::Roots` + `config/*`（§4.7） |

「不要实现」不是偷懒，是 §9.3 的判据：**没有第二个消费者的抽象是负债**。三个空端口
现在实现出来，只会得到三份彼此不一致的空实现，等真正的注入点出现时全部推翻重写。
`架构设计.md` §9.2 已经把「`Scheduler`/`ScenePresenter`/`LifecyclePort` 生产调用者为 0」
记成待偿债务，由 P5.4 的剩余部分处理。

### 2.1 宿主必须提供的四件事（不是端口）

| 职责 | 靠什么 | 绝不包含 |
|---|---|---|
| 送键 | 框架的按键回调 | **判断这个键归谁**（那是 `core::decideKey`） |
| 收文本 | `TextSessionPort` | 决定什么时候上屏什么 |
| 报事实 | `core::HostFacts` / `core::EngineFacts`（数据结构） | 推测焦点、猜屏幕 |
| 连 UI | `protocol::SceneServer` + `protocol` 帧 | 决定该画什么 |

**为什么键盘不做成端口**：三个框架都是「它把键推给我」，不是「我去拉键」。做成端口要
反向控制权，唯一收益是测试方便 —— 而 `core::decideKey` 是纯函数，测试已经不需要宿主了。

---

## 3. 两个必须实现的端口

### 3.1 `TextSessionPort` —— 全仓唯一的文本出口

```cpp
// backend/api/include/superior/api/Ports.hpp
class TextSessionPort {
public:
    virtual ~TextSessionPort() = default;
    virtual void apply(const SessionKey &session, std::span<const TextEdit> edits) = 0;
};
```

```cpp
// backend/api/include/superior/api/TextEdit.hpp
struct TextEdit {
    enum class Kind { Commit, Preedit, Clear };
    Kind kind = Kind::Commit;
    std::string utf8;                       // UTF-8；Clear 时忽略
    static TextEdit commit(std::string text);
    static TextEdit preedit(std::string text);
    static TextEdit clear();
};
```

**三条规则，按「违反后的代价」排序**（出处：`Ports.hpp` 的注释）：

1. **同步且有序。** `apply` 返回时客户端已经拿到这批编辑。实现内部可以异步
   （Android 的 `InputConnection` 本来就是、TSF 必须排进 edit session），但**不许外泄**。
   一旦把异步泄漏到决策层，就会重新长出「挂起 / 冲刷」那一对，而两个回归都在那里。
2. **key 只是提示。** 不是当前焦点的会话，**静默丢弃**，不报错、不重定向。
   陈旧点击是正常现象（`架构设计.md` §6.7）。
3. **没有平台词汇。** 签名里不许出现 fd / 句柄 / `QObject` / keysym。

**三种 `Kind` 的确切语义与三平台映射**：

| `Kind` | 语义 | 引擎占用的组合怎么办 | TSF | Android | Fcitx5（参考实现） |
|---|---|---|---|---|---|
| `Commit` | 结束组合并产出文本 | **先丢组合再提交** | `EndComposition` → `ITfRange::SetText` | `finishComposingText()` → `commitText()` | `dropComposition()` → `commitString()` |
| `Preedit`（非空） | 只显示草稿，不提交 | **不许动**别人的组合 | `ITfComposition` + 显示属性 | `setComposingText()` | `setClientPreedit()`（客户端不支持就不画） |
| `Preedit`（空） | 只清草稿 | **不许动**引擎组合 | 结束 / 清空草稿 range | `setComposingText("")` | 只清两个 preedit，**不** `reset()` |
| `Clear` | 草稿**和**引擎组合一起丢，不产出 | **要丢** | `EndComposition` + 清 range | `finishComposingText()` | `InputContext::reset()` + `InputPanel::reset()` |

三条最容易写错的地方，每条都有真实 bug 做依据：

- **`Commit` 之前必须丢组合。** 边留组合边 `commitString` 的老代码产出了「你好」后面跟着
  一串没人要的 `nihao`。注意 Fcitx5 里 `InputContext::reset()` 才会走到引擎的 `reset()`，
  `Instance::resetCompose` 只重置 XKB —— 你那边同样要找「真正通知引擎」的那个调用。
- **`Preedit("")` 不等于 `Clear`。** 取消一段语音只该清草稿，不能顺手把用户正在打的
  拼音组合干掉。反过来，切英文 / 开始语音时**必须** `Clear`，否则候选窗留着一个
  用户已经不再输入的东西。
- **密码框 / 敏感字段：一律拒绝写。** 判断放在实现里，一处。参考实现记下
  `Refusal::SensitiveField` 供调用方记日志。注意密码框**不决定**「要不要上屏」——
  那是决策层的事（P-4）：决策层决定「不学习、不上云」，但用户自己打的密码照上屏。

参考实现（117 行，含全部注释）：
[`backend/platforms/fcitx5/src/TextSession.cpp`](../backend/platforms/fcitx5/src/TextSession.cpp)。
它的 `apply` 值得逐行读，尤其是「`session.valid()` 为假时回落到当前焦点」那一段 ——
那是语音能「先说话、后点输入框」的原因。

**验收**：`tests/conformance/port_conformance_tests.cpp` 里五组断言，逐条对上就是对的：

```bash
ctest --test-dir build/linux-werror -R port-conformance --output-on-failure
```

### 3.2 `AudioSource` —— 麦克风

```cpp
struct AudioRequest {
    int rate = 16000;
    int channels = 1;
    std::string device;          // 空 = 跟随宿主当前默认设备
};

class AudioSink {
public:
    virtual void onAudio(std::span<const std::int16_t> samples) = 0;  // 单声道 s16le
    virtual void onAudioEnded() = 0;                                  // 非主动结束
};

class AudioSource {
public:
    [[nodiscard]] virtual bool available() const { return false; }
    virtual bool start(const AudioRequest &, AudioSink &, std::string &error) { return false; }
    virtual void stop() {}
    [[nodiscard]] virtual std::string defaultDevice() const { return {}; }
    virtual bool restoreDefault(const std::string &device) { return false; }
};
```

**这些方法全部有默认实现，这是设计而不是疏忽**（§6.8）：没有麦克风的宿主把
`available()` 留成 `false`，语音能力会在面板里自己解释缺什么，不必造一条错误路径。
`platforms/stub` 的 `SilentAudioSource` 就是这份默认值的具名版本。

五条实现要求：

- **`onAudio` 在实现自己的线程上调用，一块一块、按顺序。** span 只在本次调用内有效 ——
  要不要拷是 sink 的决定（流式识别不保留音频，一小时能省 115 MB）。
- **`stop()` 返回时必须保证 sink 已经安静**，且可重复调用。参考实现用
  `poll(fd, 50ms)` 而不是阻塞 `read`：关闭另一个线程正在 `read` 的 fd 是未定义行为，
  实践上也不会唤醒它，`stop()` 会挂到设备停止说话为止。
- **设备自己结束 → `onAudioEnded()`。** 不报的话，服务会一直「录音」到超时。
  用户主动 `stop()` 不算结束，别误报。
- **`defaultDevice()` 在开始采集之前读。** 桌面会在**任何**流关闭约 2 秒后重新选举默认设备，
  只有调用方知道用户原本选的是哪个；`restoreDefault()` 负责把用户的选择放回去，
  规则是「只在旧设备仍然存在、且当前确实被换掉时才动」——绝不凭空造设备名，
  也绝不覆盖用户的主动切换。
- **设备名不拼 shell 字符串。** 参考实现里那三个 `pactl` 调用是 Linux 专属的坏味道，
  但它们全在 `platforms/linux` 内部，能力侧看不到（边界脚本强制）。

**验收**：

```bash
ctest --test-dir build/linux-werror -R audio-capture --output-on-failure
```

它用 `SUPERIOR_CAPTURE_CMD` 指向一个脚本，**不需要麦克风**就覆盖三条路径：工具自己退出、
`stop()` 不阻塞且此后不再回调、`available()` 为假时安静地拒绝。

---

## 4. 宿主必须做对的四件事

### 4.1 送键：两次翻译，一次决策

```
框架的原始事件
  ├─ 翻译 1：原生键值 → core::KeyCode          ← 适配器自己的表
  ├─ 翻译 2：轻按 / 长按 → core::Gesture        ← 适配器自己的表
  └─ 决策：core::decideKey / decideGesture      ← 纯函数，全平台相同
```

```cpp
// backend/core/include/superior/core/KeyPolicy.hpp（字段逐字照抄，默认值见头文件）
struct KeyPress {
    KeyCode  code = KeyCode::Other;
    char32_t text = 0;      // 这个键能产出的字符；0 表示没有
    bool shift = false;
    bool control = false;
    bool alt = false;
    bool super = false;
    bool release = false;
};

KeyDecision  decideKey(const KeyPolicyInput &input) noexcept;      // 归属 + 是否消费
GestureIntent decideGesture(const GestureInput &input) noexcept;   // 轻按 / 打断的含义
int pageStep(KeyCode code) noexcept;                               // + / 小键盘- 翻页
```

要点：

- **`KeyCode` 里没有原生值**，`KpSubtract` 与 `Minus` 必须分开：只有小键盘 `-` 翻页，
  主键盘 `-` 是打连字符。合并它们 = 打连字符变翻页。
- **`text` 是「这个键能不能打出可见字符」的唯一来源。** 填不了就别猜，留 0，
  策略会把未知键透传 —— 与旧行为一致，且少一个会不一致的字段。
- **归属由策略说，适配器只返回 true/false。** 不要在新平台里写
  「如果是 Backspace 就……」：`Backspace` 永远透传，因为内联英文已经直接提交到客户端，
  字符本来就归客户端所有（用 `deleteSurroundingText` 删它们，曾在所有不支持周边文本的
  客户端上把 Backspace 弄坏）。
- **修饰键永远不吞。** Shift / Ctrl 的按下必须让应用看到（选区、快捷键）。
- **三段手势的含义已经在 `core::decideGesture`**：Shift 轻按切中英（受
  `inlineEnglish` 开关约束）、右 Ctrl 轻按开关语音（受两个开关共同约束）、
  录音中的 Esc 取消这段话。**不要在新平台各写一份** —— 它们以前就是分散三处、
  各自抄了一份设置判断。

**原生键值仍然要留着**：librime 在**所有**平台都说 X11 keysym 编号（包括 Windows）。
它属于 Rime 的线格式，不进决策层，只在拼音适配器里出现。

### 4.2 `SessionKey`：由宿主铸造，端口只透传

```cpp
struct SessionKey {
    Epoch epoch = 0;        // 进程 / 会话纪元，永不复用
    ContextId context = 0;  // 一个输入上下文的稳定身份
    [[nodiscard]] bool valid() const { return epoch != 0; }
};
```

- **纪元每次启动 +1**，异步结果靠它判「是不是上个进程的」。
- **绝不用对象身份做会话身份**：Android 的 `InputConnection` 每次 `onStartInput` 都可能是
  新对象，TSF 的 composition 能跨焦点存活 —— 两边都会错。
- **`apply` 收到无效 key 是正常语义**，意思是「光标现在在哪就写到哪」（语音场景）。

### 4.3 报事实：两个投影，一个折叠函数

```cpp
// backend/core/include/superior/core/SceneInput.hpp（字段逐字照抄，默认值见头文件）
struct EngineFacts {          // 语言引擎贡献的
    std::string preedit;
    std::size_t preeditCursor = 0;
    std::vector<TextRun> preeditRuns;
    std::vector<EngineCandidate> candidates;   // {label, text, comment}，**没有 id**
    int pageIndex = 0;         // 1 基；0 = 未知
    int pageCount = 0;         // 0 = 未知
    int highlighted = -1;
    bool vertical = false;
    bool composing = false;
    std::string engineId;
};

struct HostFacts {            // 宿主贡献的
    api::Rect cursorRect;
    bool cursorValid = false;
    double scale = 1.0;
    std::string program;
    std::string inputMethod;
    std::vector<api::StatusItem> statusItems;
    bool surroundingText = false;
    bool clientPreedit = false;
    bool password = false;
};

void updateSession(const EngineFacts &, const HostFacts &, ImeSession &,
                   std::string_view skinId, api::SkinGeneration generation);
```

- **候选没有 id**：id 由 `CandidateModel` 按「代次 + 位置」分配。引擎自己编 id 就是重新
  引入那套「位置当身份」的混淆。
- **`program` 只能用于观测、日志、用户显式配置的 per-app 皮肤。** 禁止
  「如果是 VSCode 就 xxx」——行为不可预测、不可测试。
- **`password` 只影响决策**（不学习、不上云），不影响能不能上屏。

### 4.4 连 UI：照抄协议，别自己发明

```cpp
// 一帧就是「完整的期望场景」，不是增量：丢帧永远安全
api::UiSceneFrame frame = session.snapshot(nextFrame_++);
server_.broadcast(protocol::encodeFrame(protocol::MessageType::Scene,
                                        protocol::encodeScene(frame)));
```

- 实现放在 `protocol::SceneServer`（`socketPath = Roots::runtime / "superior-ime.sock"`），
  **不要**在新平台另写一套。Windows 换成命名管道时，只换传输，不换帧格式。
- UI 发来的命令是 `api::UiCommand`（`std::variant`），语义命令，**不含绝对几何**。
- `server_.clientCount() == 0` 时直接丢帧，记 trace。UI 没连上不是错误。

### 4.5 时间：现在用你自己的事件循环

`api::Scheduler` 有声明、没消费者。云输入去抖、语音会话超时目前都由宿主的定时器驱动
（Fcitx5 用 `eventLoop().addTimeEvent`）。等你真的接第二个宿主时再抽，因为那时才有
第二个实现来决定接口长什么样。**不要**在决策层里 `sleep`，也不要在 `core` 里读墙钟
（边界脚本会红）。

### 4.6 降级：现在写日志

`LifecyclePort` 同样没有消费者。当前做法是 `SUPERIOR_WARN` / `SUPERIOR_INFO` 到
`diagnostics` 的日志分类（`diag::categories::SpeechRecognition` 等）。
面板上的「缺引擎 / 缺模型」由 `core::SpeechState` 的 `NeedsSetup` 表达，不走端口。

### 4.7 存储：路径只有一个来源

```cpp
// backend/paths/include/superior/paths/Roots.hpp
struct Roots {
    std::filesystem::path home, data, config, state, cache, runtime;
    std::vector<std::filesystem::path> dataDirs;
    static Roots fromEnvironment();          // 唯一解析 XDG_* / HOME 的地方
};
```

`backend/` 下任何地方（含 `platforms/`、`protocol/`）**都不许** `getenv("XDG_*")` 或
`getenv("HOME")` —— 边界脚本强制。Windows 上把 `fromEnvironment()` 换成 `KnownFolders`
即可，调用点一处不动。

---

## 5. 验收清单（可勾）

### 5.1 机械门槛

- [ ] `git diff --stat` 里 `backend/api/` 与 `backend/core/` **零改动**
      （真要改，说明契约漏了东西 —— 那是架构问题，先讨论再改）
- [ ] `bash scripts/check-boundaries.sh` 绿，且新目录也被扫到
- [ ] 在**没有 librime / 没有 Qt / 没有 Fcitx5** 的机器上编得过
- [ ] `ctest -R 'port-conformance|audio-capture'` 全绿

### 5.2 行为门槛

- [ ] 记事本里打 `Hello`，焦点切换后**无残留预编辑**
- [ ] 中文打 `nihao` → 「你好」；连打 5 次候选顺序稳定（不依赖未初始化状态）
- [ ] `Ctrl+C` / `Ctrl+V` / 方向键 / `Backspace` 全部透传
- [ ] Shift 轻按切中英（`inlineEnglish` 关掉时**不切**）
- [ ] 录音中按 `Esc` 取消：不上屏，且不留尾音
- [ ] 语音**连说两段**，第二段仍有字（上屏唯一性，最容易漏）
- [ ] 密码框：不显示草稿、不学习、不上云；用户自己打的字符照上屏
- [ ] 关掉 UI 进程，输入法仍能上屏
- [ ] 打一半拼音时开始语音：候选窗立刻消失，不残留

### 5.3 三条「只测功能测不出来」的

- [ ] 陈旧点击：点云候选后立刻点别的窗口，候选**不**打进新窗口
- [ ] 失焦时说完一句：转写被**暂存**，焦点回来时按顺序补上（语音不能丢）
- [ ] 同一句话在流式识别中被切成多个 `chunk`：拼起来不重不漏

---

## 6. 十个最容易犯的错

| # | 错 | 后果 | 正确做法 |
|---|---|---|---|
| 1 | `Commit` 时没先丢引擎组合 | 「你好」后面跟着一串 `nihao` | Commit 前先结束组合（§3.1） |
| 2 | 把 `Preedit("")` 当 `Clear` | 取消语音顺手干掉用户正在打的拼音 | 只清草稿（§3.1） |
| 3 | `apply` 里每个 edit 各开一个异步任务 | 顺序不再保证，比单次提交更难查 | 一整批一次提交（§3.1） |
| 4 | 用平台对象身份当 `SessionKey` | 重连后 key 变，暂存文本永远补不上 | 纪元 + 上下文 id（§4.2） |
| 5 | 适配器里判断「这个键归谁」 | 三个平台行为分裂，且无法测试 | `core::decideKey`（§4.1） |
| 6 | 合并 `KpSubtract` 与 `Minus` | 打连字符变成翻页 | 保持两个 `KeyCode`（§4.1） |
| 7 | 吞掉 Shift / Ctrl 的按下 | 选区、快捷键全坏 | 永远返回「不消费」 |
| 8 | 自己 `getenv("HOME")` / 读注册表 | 换平台要重写，且与 `paths` 分叉 | `paths::Roots`（§4.7） |
| 9 | 引擎给候选编 id | 跨页别名、翻页后点错行 | `CandidateModel` 分配（§4.3） |
| 10 | 给没消费者的端口写实现 | 三份不一致的空实现，等注入时全推翻 | 等第二个真实消费者（§2） |

第 10 条是这份手册里唯一「建议你别写代码」的一条，也是最容易被违反的一条：
填满接口看起来像在推进，实际上是在给未来的自己留三份要删的代码。

---

## 7. 各平台的特有问题

### 7.1 Windows TSF

- 文本修改**必须**在 `ITfEditSession::DoEditSession` 里完成；异步靠请求 `ITfEditSession`，
  但端口对外仍然表现为同步（§3.1 规则 1）。
- `ITfKeyEventSink::OnKeyDown` 返回 `TRUE` 才是「吃掉」；修饰键必须返回 `FALSE`。
- 焦点走 `ITfThreadMgrEventSink::OnSetFocus`；composition 能跨焦点存活，别拿它做会话身份。
- 光标矩形用 `ITfContextView::GetTextExt`，失败是常态（应用不响应时），
  `cursorValid = false` 即可，不要退化成「屏幕左上角」。

### 7.2 Android `InputMethodService`

- `onStartInput()` 会给**新的** `InputConnection`，这是 `SessionKey` 必须带纪元的直接原因。
- `setComposingText()` 是「草稿」，`commitText()` 是「提交」，
  `finishComposingText()` 是「结束组合」—— 与 `Preedit` / `Commit` / `Clear` 一一对应，
  所以 Android 是验证契约最快的地方。
- `InputConnection` 的方法在 binder 上可能失败/被丢，但**不许**把这份不可靠性外泄给决策层；
  在实现里重试或降级。
- 光标矩形走 `onUpdateCursorAnchorInfo()`，不是 `onUpdateSelection()`（后者给的是字符偏移）。
- 权限（`RECORD_AUDIO`）缺失时，`AudioSource::available()` 返回 `false` 就够了。

### 7.3 任何桌面宿主（X11 / Wayland / macOS / …）

- 「始终置顶」是窗口服务器的事，不是输入法的事。当前实现（`frontend/desktop/src/Overlay.hpp`）
  在 X11/XWayland 上用 override-redirect：合成器无法把它压到普通窗口之后，点它也不夺焦点。
  Wayland 上同样的标志退化成普通无边框窗口，layer-shell 后端**尚未落地** —— 所以默认走
  XWayland。这些都在 `frontend/` 里，一行也不该出现在适配器里。
- UI 是**独立进程**（`frontend/desktop`）。适配器只负责把帧写到 socket，
  绝不链接 Qt、绝不 `#include` 任何 UI 头文件。
- 关掉 UI 进程后输入法**仍须能上屏** —— 这条是「UI 不是输入法本体」的实测判据。

---

## 8. 维护规则

- 本文的每条签名都能在代码里找到；**改端口时同一次提交里改本文**，
  否则下一个人会照着一份不存在的接口写代码。
- `tests/conformance/` 是契约的可执行版本。加一条规则就加一条断言，
  不要只改文字。
- 新增边界规则时先跑一遍再留下（`架构设计.md` §10.2）；加一条暂时过不了的规则、
  然后注释掉，等于给自己一份假的边界报告。
