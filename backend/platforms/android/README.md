# backend/platforms/android

Android `InputMethodService` 适配（预留）。

## 起点

与 Windows 共用同一份契约与同一份验收：`backend/platforms/stub/` 的
`RecordingTextSession` 换成 `InputConnection` 的实现即可 ——
`TextEdit::Commit` → `commitText`、`Preedit` → `setComposingText`、
`Clear` → `finishComposingText`。三者一一对应，所以 Android 是第二宿主的首选
（`架构设计.md` §11 P6）。

`AudioSource` 换成 `AudioRecord` 即可：端口签名里没有 fd、没有设备节点，采集线程
留在实现内部（§6.2）。`available()` 返回 false 时，面板会自己解释为什么没有语音，
适配器不需要造一条错误路径。
