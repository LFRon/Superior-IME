# backend/core

纯逻辑层，零平台依赖：不 include Qt / Fcitx5，不读墙钟。

| 模块 | 职责 |
|---|---|
| `CandidateModel` | 稳定 id、分页、按 id 选中；列表更新时尽量保持选中 |
| `MotionTimeline` | tween / spring 的确定性采样，时间由调用方传入 |
| `ImeSession` | 单会话语义状态：预编辑、候选、皮肤、状态栏、提交 |

`core` 只依赖 `backend/api` 与 `backend/skin`（后者提供 `MotionSpec`）。
提交文本、清空组合、隐藏面板等语义都在这里决定，UI 不参与。
