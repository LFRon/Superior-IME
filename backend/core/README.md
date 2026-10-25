# backend/core

纯逻辑层，零平台依赖：不 include Qt / Fcitx5，不读墙钟。

| 模块 | 职责 |
|---|---|
| `Modes` | 中英 / 全半角 / 全角的**唯一 owner**，以及到引擎 option 的映射 |
| `KeyPolicy` | 纯函数：这个按键归谁（引擎 / 透传 / 上屏 / 切模式 / 取消） |
| `CandidateModel` | 代次稳定的 id（`generation << 16 | position + 1`）、分页、按 id 选中 |
| `SceneInput` | 把「引擎事实」与「宿主事实」折叠成场景，二者严格分离 |
| `ImeSession` | 单会话场景状态：预编辑、候选、皮肤、状态栏、提交 |

`core` 只依赖 `backend/api` 与 `backend/skin`。
**这里不实现任何输入法能力**（拼音在 `backend/rime`，云在 `backend/cloudpinyin`），
也不碰任何平台头；它只决定「现在该做什么」。
