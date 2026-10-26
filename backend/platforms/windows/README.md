# backend/platforms/windows

Windows TSF 适配（预留）。

薄 TSF DLL + 独立 UI broker；输入法状态与 UI 通过 `backend/protocol` 通信。

## 起点

动手前先读 `backend/platforms/stub/`：它只用 `superior/api/` 就实现了
`TextSessionPort` 与 `AudioSource`（`RecordingTextSession` / `SilentAudioSource`），
并在 `tests/conformance/` 里跑通 §6.6–6.8 的契约。TSF 适配的实现只要让同一套
断言成立即可，这也是 `core/` 一行都不用改的判据（附录 D）。

进入本目录的第一件事不是写 `ITfTextInputProcessor`，而是把
`RecordingTextSession` 换成真的 `apply()`：`TextEdit::Commit` → `ITfRange::SetText`，
`Preedit` → 显示属性、`Clear` → 结束组合。其余判断（是否敏感字段、是否该上屏）
已经在决策层做完，适配器不重复判断（P-4）。
