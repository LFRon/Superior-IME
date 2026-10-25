# backend/diagnostics

全项目统一日志入口。无 Qt、Fcitx5、平台依赖。

| 文件 | 内容 |
|---|---|
| `include/superior/diagnostics/Log.hpp` | 级别、类别、`Log` builder、级别宏、sink API |
| `src/Log.cpp` | 过滤、格式化、stderr/file sink、轮转、环境配置 |

用法：

```cpp
SUPERIOR_DEBUG(superior::diagnostics::categories::Skin)
        .field("skin", spec.id)
        .redacted("preedit", preedit.text)
    << "skin loaded";
```

安全约束见 [docs/logging.md](../../docs/logging.md)：**不记录用户输入内容**，
不拼接命令行，所有字段转义并截断，日志永不让进程崩溃。

Qt 桥在 `frontend/desktop/src/Logging.cpp`：Qt 消息汇入统一格式；设置
`SUPERIOR_LOG_VIA_QT=1` 时反过来交给 `QMessageLogger`。
