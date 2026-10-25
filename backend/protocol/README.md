# backend/protocol

前后端通信，仅依赖 `backend/api` 与 nlohmann/json。

| 文件 | 职责 |
|---|---|
| `Wire.hpp` / `Wire.cpp` | 帧格式、JSON 编解码、`Hello` / `Scene` / `Command` |
| `SceneServer.hpp` / `SceneServer.cpp` | Unix socket 广播服务，独立 poll 线程，`SO_PEERCRED` 校验 |

帧格式：

```
"SUPI" | u16 version | u16 type | u32 payload length | JSON payload
```

控制流量是打字频率而不是帧率，因此 v1 选择自描述的 JSON；动画由 UI 本地采样。
二进制负载可以在不改变帧头的情况下作为 v2 引入。

服务端默认路径 `$XDG_RUNTIME_DIR/superior-ime.sock`（否则 `/tmp/superior-ime-$UID.sock`），
权限 0600，最多 8 个客户端。`broadcast()` 线程安全；命令回调运行在 poll 线程。

## 谁拥有这个 socket

`<socket>.lock` 上的排他 `flock` 决定归属，`listen()` 先抢锁再拆旧服务，抢不到就失败返回。
只有这样做才安全：`fcitx5 -r` 会让新旧两个 agent 在几十毫秒内同时启动，而「探测一下有没有人
在监听」看不见尚未 `listen()` 的对手——旧代码于是把它当成残留文件删掉再 bind，两个服务都报成功，
其中一个从此挂在一个没人能连上的 inode 上，面板再也不出现。

持有锁的进程崩溃或退出时内核自动释放，接手的 agent 只要重试 `listen()` 即可。
`ownsSocketFile()` 用来判断 socket 文件是否还属于自己：文件被别人删掉时为假，
调用方据此重新 `listen()`，而不是对着一个连不上的 inode 一直广播。
