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
