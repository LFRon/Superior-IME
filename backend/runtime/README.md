# backend/runtime

串行运行时，把纯逻辑接到端口上。公共头文件只暴露 `superior::api` 类型；
`superior::core` 只在实现文件里出现。

| 模块 | 职责 |
|---|---|
| `LatestOperation<T>` | 票据化异步：新操作取消旧操作，回调先于句柄返回也安全 |
| `SerialPump` | owner 线程串行队列；回调内投递的任务在同一轮排空 |
| `SessionRuntime` | 绑定 `ImeSession` + `ScenePresenter` + `Scheduler`，合并帧请求 |

`SessionRuntime` 保证同一时刻最多一个帧请求在途；调度器回调到达时用最新状态
构建完整 `UiSceneFrame`，因此高频状态变化不会产生帧堆积。
