# frontend/desktop

Qt Widgets 桌面壳。只依赖 `backend/api`、`backend/protocol`；不包含任何
`backend/core` / `backend/runtime` / Fcitx5 头文件。

| 文件 | 职责 |
|---|---|
| `Overlay.cpp` | 唯一的窗口策略入口：override-redirect、置顶、不抢焦点 |
| `StatusWindow.cpp` | 右下角状态栏，屏幕锚定 |
| `CandidateWindow.cpp` | X11 候选窗/预编辑浮层 |
| `SceneClient.cpp` | 连接 agent socket，重组帧 |
| `main.cpp` | 进程装配 |

运行（强制 XWayland）：

```bash
QT_QPA_PLATFORM=xcb scripts/run-ui.sh            # 仅状态栏
QT_QPA_PLATFORM=xcb scripts/run-ui.sh --candidates  # 实验性 X11 候选窗
```

默认不显示候选窗，因为 Fcitx5 classicui 仍在绘制它；两者同时开启会出现两个
候选面板。

窗口语义与实测验证见 [docs/xwayland-ui.md](../../docs/xwayland-ui.md)。
Wayland 候选窗不使用本目录：它由 Fcitx5 classicui 主题负责。
