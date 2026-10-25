# 统一日志

`backend/diagnostics` 是全项目唯一的日志入口。它不依赖 Qt、Fcitx5 或平台代码，
所以同一套调用在 Fcitx5 agent、CLI 工具和 Qt 浮层里行为一致。

---

## 1. 快速开始

```bash
# 默认级别是 info，输出到 stderr
SUPERIOR_LOG_LEVEL=debug ./build/linux-ui/frontend/desktop/superior_ui

# 单类别覆盖，支持前缀匹配
SUPERIOR_LOG="superior.protocol=trace,superior.protocol.socket=debug" ./superior_ui

# 写文件（8 MiB 轮转，保留一份 .1）
SUPERIOR_LOG_FILE=~/.cache/superior-ime/log.txt ./superior_ui

# 把所有记录也交给 Qt 的日志系统，让 QT_LOGGING_RULES 能过滤
SUPERIOR_LOG_VIA_QT=1 QT_LOGGING_RULES="superior.*=true" ./superior_ui
```

| 变量 | 作用 |
|---|---|
| `SUPERIOR_LOG_LEVEL` | `trace` / `debug` / `info` / `warning` / `error` / `off` |
| `SUPERIOR_LOG` | `类别=级别` 逗号分隔；长前缀优先 |
| `SUPERIOR_LOG_FILE` | 文件 sink，默认 8 MiB 轮转 |
| `SUPERIOR_LOG_VIA_QT` | 非空且非 `0/false/off` 时，把记录转给 Qt |

---

## 2. 输出格式

```
2026-10-24T17:54:31.859Z [I] superior.rime RimeEngine.cpp:36 rime engine ready schema=wanxiang_pure needsDeploy=false shared=… user=…
```

固定字段：UTC 时间、`[T|D|I|W|E]`、类别、`文件:行`、消息、`key=value`。

稳定、单行、可 grep；字段值经过转义和截断，不会破坏行结构。

---

## 3. 用法

```cpp
#include "superior/diagnostics/Log.hpp"

namespace diag = superior::diagnostics;

SUPERIOR_DEBUG(diag::categories::Skin)
        .field("skin", spec.id)
        .field("bytes", static_cast<std::uint64_t>(size))
    << "skin loaded";
```

- 级别宏：`SUPERIOR_TRACE/DEBUG/INFO/WARN/ERROR`。
- `field()` 自动转义并截断到 256 字节；整数、浮点、bool 都有重载。
- `redacted()` 只记录长度：`preedit=<redacted len=42>`。
- 只有类别启用时才构造 `Log`，关闭时不会拼字符串。
- sink 抛异常会被吞掉：日志永远不会让进程崩溃。

自定义 sink：

```cpp
diag::setSink([](const diag::Record &record) { /* 写文件、转发、测试 */ });
```

---

## 4. 与 Qt 的关系

`frontend/desktop/src/Logging.cpp` 安装一个 `qInstallMessageHandler`：

- `qDebug/qCWarning/qCritical` 被转换成 `qt.<category>` 记录，走统一格式；
- `QT_LOGGING_RULES` 仍然决定 Qt 是否发出这些消息，所以既有的 Qt 调试方式不变；
- 设置 `SUPERIOR_LOG_VIA_QT=1` 后，我们的记录反过来交给 `QMessageLogger`，
  这样就可以用 `QT_LOGGING_RULES` 过滤；未设置 `QT_LOGGING_RULES` 时，
  程序会为 `superior.*` 安装默认规则，不影响 Qt 自己的类别；
- 反向转发用 `thread_local` 守卫，避免两个方向互相递归。

注意：`emit` 是 Qt 的宏，所以 API 命名为 `publish()` / `publishRaw()`。

---

## 5. 安全规则（必须遵守）

日志是**会进入用户可读文件**的数据通道，按不可信数据处理：

1. **不记录用户输入内容。** 预编辑、候选、提交文本、剪贴板一律用
   `redacted()`，只留长度。
2. **不记录凭据。** 不打印环境变量全量、token、cookie、D-Bus 地址。
3. **不记录文件内容。** 皮肤、词库、配置只记录路径、大小、哈希。
4. **不拼接命令行。** 外部工具调用使用 `posix_spawn` + argv 数组，
   不经过 shell；日志里只记录工具名与退出码。
5. **路径按不可信处理。** 所有字段都经过 `escapeField()`：控制字符转成
   `\n` / `\xNN`，引号和反斜杠转义，超长截断。
6. **不做格式化字符串。** 全部走 `operator<<`，不存在 `printf` 风格格式串，
   因此恶意输入无法构造格式串漏洞。
7. **记录数量可由攻击者放大。** trace 级别只用于本地排障；默认 `info`，
   发布构建不要默认开到 `trace`。

代码审查时看到 `<< preedit.text` 或 `<< candidate.text` 一律视为缺陷。

---

## 6. 已埋点的位置

| 类别 | 位置 | 内容 |
|---|---|---|
| `superior.diagnostics` | `Log.cpp` | 配置生效、最小级别 |
| `superior.skin` | `Skin.cpp` | 加载/拒绝、哈希、资源数、每条诊断、扫描结果 |
| `superior.core` | `CandidateModel.cpp` | 候选数量、分页、选中变化、代次 |
| `superior.core` | `ImeSession.cpp` | 提交（文本已脱敏）、拒绝过期 id |
| `superior.protocol` | `Wire.cpp` | 帧解码（类型、字节数） |
| `superior.protocol.socket` | `SceneServer.cpp` | 监听、连接/断开、队列丢弃、关闭 |
| `superior.fcitx5` | `SuperiorAgent.cpp` | 启动/停止、焦点、光标矩形、IM 切换、事实摘要、广播、命令 |
| `superior.rime` | `RimeEngine` / `LibrimePort` | 库加载、schema、部署、按键与候选（只记形状） |
| `superior.ui` | `SceneClient.cpp` | 连接、断线重试、坏帧、收到场景 |

---

## 7. 测试

`tests/diagnostics_tests.cpp` 覆盖：

- 级别过滤与类别前缀覆盖；
- 字段格式化、脱敏、转义、截断；
- 关闭时零开销；
- sink 抛异常被隔离；
- 输出格式与时间戳；
- `publishRaw` 绕过类别过滤（Qt 方向使用）。
