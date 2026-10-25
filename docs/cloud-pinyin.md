# 云拼音

## 结论

先！进！自带云拼音，**默认关闭**。开启后，输入两个字母以上、停顿 200 ms，
就会把当前的**纯拼音**发给服务商，用返回的词补一个候选。

这与 `fcitx5-cloudpinyin` 的思路一致（同一个占位符 `☁`、同一个"连续失败就暂停"
策略），但有四处按我们的架构重写了，见下面「与上游的差异」。

---

## 1. 分层

| 层 | 位置 | 职责 |
|---|---|---|
| 纯逻辑 | `backend/cloudpinyin/{Cloud,CloudState}.{hpp,cpp}` | URL 构造、响应解析、拼音规范化、LRU 缓存、错误预算 |
| 传输 | `backend/cloudpinyin/src/Fetcher.cpp` | libcurl `multi`，**无独立线程** |
| 引擎适配 | `backend/platforms/fcitx5/src/SuperiorAgent.cpp` | 占位符插入、结果回填、去重、提交 |
| 界面 | `frontend/settings` | 开关与服务商选择 |
| 诊断 | `tools/superior-config cloud probe` | 一条命令判断当前网络能不能用 |

UI 进程不链接 `superior/cloudpinyin`：设置页只读写 `settings.json` 的四个字段，
网络全部发生在 agent 里。

---

## 2. 一次查询的完整路径

**加载期间不占位。** 早期版本在等待响应时先插一个 `☁` 占位符，结果是：你正要按的
第 2 位变成了一个还不知道内容的云朵，它后面所有候选都往后挪了一格——按错一次就
上屏了别的词。现在列表在等待期间**完全等于引擎自己的输出**，云端结果到了才在第 2
位插入（原第 2 位及之后顺延）。代价是插入的瞬间列表会跳一下，但这发生在你已经看到
结果之后，而不是在你按键之前。


```text
用户输入 → 面板 preedit（我们自己接管，所以就是拼音）
        → normalizePinyin()       ni hao / ni'hao → nihao；lü → lv
        → worthQuerying()         >= cloudPinyinMinLength（默认 2）
        → 敏感输入？              密码框/英文模式直接跳过
        → 候选列表可写？          toModifiable() 为 null 就放弃
        → 缓存命中？              直接插入真实候选，不发请求
        → 否则插入 ☁ 占位符，交给去抖
去抖 200 ms 后仍未变化 → Fetcher::submit()（异步，不阻塞）
结果回来 → 回填占位符 / 与引擎已有候选重复则删除占位符
选中     → resetCompose() + commitString()
```

去抖是必需的：最短长度降到 2 以后，`xingqiongtiedao` 会在打字过程中产生
13 次中间状态。200 ms 既能过滤掉打字节奏，又不至于让人等。

---

## 3. 与上游的差异（都是踩出来的）

| 差异 | 原因 |
|---|---|
| **无独立线程**，用 `curl_multi` + 40 ms 事件定时器 | agent 是共享库，Fcitx5 会 `dlclose`。线程一旦活过卸载就会跑到已释放的内存上（浮层回收线程踩过同一个坑） |
| **不插占位符**，结果到了才插入 | 占位符会把你要按的第 2 位换成云朵，并把它后面所有候选挤后一格 |
| 状态以「拼音」而不是「列表指针」为身份 | libime 会**原地重建**列表内容，指针不变；只认指针会让过期的插入位置指向真实候选，云端候选直接消失 |
| 回填前校验占位符是否还在原处 | 否则 `replace()` 会覆盖引擎刚生成的候选 |
| 结果与引擎候选重复就丢弃 | 本地词库已经很好，`shurufa` / `rengongzhineng` 这类词的云端结果与引擎完全一致，硬塞一行只是噪音 |
| 拼音规范化处理 `ü` | libime 的 preedit 写 `lü`，服务商只认 `lv`；早期版本把整串判成"非拼音"直接跳过，于是所有带 ü 的词都没有云候选 |
| 失败三次即暂停 60 秒，网络类错误一次顶三次 | 上游是十次错误 + 5 分钟冷却：用户看到的是"开了但没反应"，而且很难等回来 |
| 只接受含中文的候选 | 纯 ASCII 的"候选"一定是解析错位（通常是拼音被原样返回），插进去毫无意义 |
| 服务商可显式探测 | 百度接口已经失效，用户不该靠猜来判断是"没打开"还是"打不开" |

---

## 4. 服务商现状（2026-10 实测）

| 服务商 | 端点 | 状态 |
|---|---|---|
| `googlecn`（默认） | `www.google.cn/inputtools` | 可用，直连约 0.2 s，解析到国内 CDN |
| `baidu` | `olime.baidu.com/py` | **失效**：任何输入都返回 `{"status":"F","errno":"19"}`，解析为"无候选" |
| `sogou` | 无公开端点 | 占位，选中即跳过 |

一条命令验证：

```bash
./build/linux-ui/tools/superior-config/superior_config_cli cloud probe --backend googlecn nihao
# backend   : googlecn
# http      : 200
# candidate : 你好
# verdict   : available
```

---

## 5. 隐私与安全

- **默认关闭**，设置页明确写着"拼音会离开本机"。
- 密码框（`CapabilityFlag::PasswordOrSensitive`）与英文模式永不查询，也永不进缓存。
- 只发送规范化后的 `a-z` 串：上屏文字、占位符、CJK 一律不发。
- 日志只记**长度**、服务商、HTTP 码，不记拼音也不记结果。
- HTTPS（curl 默认校验证书），`FOLLOWLOCATION=0`，响应体上限 8 KiB。
- 缓存有界（256 条），换服务商立刻清空：Google 的答案不能在选百度时被复用。

---

## 6. 用户输入习惯（本地偏好）

Rime 的做法是 `userdb`：选过的词被写进用户词典，下次同一拼音优先它。libime 也有
等价的机制（`user.dict` + `user.history`），但**只覆盖引擎自己产出的候选**——云端
候选是我们提交的，引擎不知道，所以那部分必须自己记。

文件位置（配置目录，不是数据目录）：

```
$XDG_CONFIG_HOME/superior-ime/learning.json      # 默认 ~/.config/superior-ime/
```

```json
{
  "schema": "superior.learning/1",
  "entries": [
    { "pinyin": "yuanshen", "word": "原神", "count": 3, "lastUsedMs": 1792820292936 }
  ]
}
```

规则：

| 行为 | 说明 |
|---|---|
| 记录时机 | 只有用户**主动选中**我们插入的候选时才记，不记录单纯输入 |
| 位置 | 该拼音的已学词放到候选**第 1 位**（其余候选顺延） |
| 已存在 | 引擎自己已经给出同一个词时不再插入，避免重复行 |
| 改主意 | 同一拼音选了另一个词就**替换**，不并存 |
| 容量 | 512 条，超出按最久未用淘汰 |
| 权限 | 强制 `0600`：`~/.config` 常常是 0755，光靠 umask 不够 |
| 隐私 | 密码框与英文模式既不查询也不记录；日志只记长度 |

**关键点：本地偏好不依赖网络。** 关闭云拼音后，已学词依然会被提到第一位；网络
查询只是"发现新词"的途径。早期版本把整段逻辑塞在 `if (cloudEnabled_)` 里面，导致
关掉云拼音就把用户积累的偏好一起废掉了。

已知取舍：引擎自己产生的候选被选中时我们观察不到（`commitString` 由引擎发出），
那部分交给 libime 的 `user.dict`；两套学习各管各的，互不覆盖。

---

## 7. 设置项

`~/.config/superior-ime/settings.json`：

| 键 | 默认 | 说明 |
|---|---|---|
| `cloudPinyinEnabled` | `false` | 总开关 |
| `cloudPinyinBackend` | `"googlecn"` | `googlecn` / `baidu` / `sogou` |
| `cloudPinyinMinLength` | `2` | 少于这个长度不查 |
| `cloudPinyinIndex` | `2` | 云端候选出现的位置（1 起） |

改动 500 ms 内生效，不需要重启。

---

## 8. 已知取舍

- 云端候选上屏走 `commitString()`，**不进入引擎的用户词频学习**：我们没有引擎内部
  句柄，硬塞会有副作用。代价是选过的云端词不会因此提升 libime 的权重——所以才有
  第 6 节那套自己的记录。
- 只取服务商返回的**第一条**候选，不铺开整个列表：云端一次返回十几条会把本地
  排序冲掉，而那正是 `libime` 更擅长的部分。
- 与上游 `fcitx5-cloudpinyin` 同时启用时，检测到已有 `☁` 占位符就不再加第二个。
