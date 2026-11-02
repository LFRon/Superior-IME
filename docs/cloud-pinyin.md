# 云拼音

## 结论

先！进！自带云拼音，**默认关闭**。开启后，输入两个字母以上、停顿 200 ms，
就会把当前的**纯拼音**发给服务商，用返回的词补一个候选。

这与 `fcitx5-cloudpinyin` 的思路一致（同一个候选位置、同一套"连续失败就暂停"
策略），但按我们的架构重写了，见下面「与上游的差异」。

---

## 1. 分层

| 层 | 位置 | 职责 |
|---|---|---|
| 纯逻辑 | `backend/cloudpinyin/{Cloud,CloudState}.{hpp,cpp}` | 请求构造（GET/POST）、响应解析、拼音规范化、LRU 缓存、错误预算 |
| 纯逻辑 | `backend/cloudpinyin/{Match}.{hpp,cpp}` | 候选与输入是否对得上：音节对齐、字数核对、按输入类型选服务商（全是纯函数，可单测） |
| 传输 | `backend/cloudpinyin/src/Fetcher.cpp` | libcurl `multi` + **独立工作线程**，请求/结果队列 |
| 候选合成 | `backend/pinyin/src/CandidatePipeline.cpp` | engine + learned + cloud 合成一页、去重、位置 |
| 引擎适配 | `backend/pinyin/src/RimeComposer.cpp` | 云去抖、结果回填、选中即学习 |
| 界面 | `frontend/settings` | 开关与服务商选择 |
| 诊断 | `superior-config cloud probe` | 一条命令判断当前网络能不能用 |

UI 进程不链接 `superior/cloudpinyin`：设置页只读写 `settings.json` 的四个字段，
网络全部发生在拼音能力包（`CloudService`）里。宿主不链接云模块，也不参与候选插入。

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
        → 否则只登记待查询，不占位
去抖 200 ms 后仍未变化 → Fetcher::submit()（工作线程，不阻塞主循环）
结果回来 → 插入真实候选 / 与引擎已有候选重复则丢弃
选中     → resetCompose() + commitString()
```

去抖是必需的：最短长度降到 2 以后，`xingqiongtiedao` 会在打字过程中产生
13 次中间状态。200 ms 既能过滤掉打字节奏，又不至于让人等。

---

## 3. 与上游的差异（都是踩出来的）

| 差异 | 原因 |
|---|---|
| **独立工作线程** + 请求/结果队列 | 同步解析 DNS 或等待 TLS 握手会把 Fcitx5 主循环卡住；宿主是共享库，Fcitx5 会 `dlclose`，所以 `Fetcher` 的析构必须停止并 `join` 工作线程，不能让线程活过卸载（浮层回收线程踩过同一个坑） |
| 40 ms 事件定时器每次回调都 `setNextInterval()` | Fcitx5 的 `addTimeEvent` 是**单次**时间源，第三个参数是精度不是周期。只靠重复 `setEnabled(true)` 续命会把 deadline 留在过去，sd-event 就会在云请求期间反复回调、饿死 XCB/XIM（Telegram 按键约 200 ms 才到一个） |
| **不插占位符**，结果到了才插入 | 占位符会把你要按的第 2 位换成云朵，并把它后面所有候选挤后一格 |
| 状态以「拼音」而不是「列表指针」为身份 | 引擎可能原地重建列表内容，指针不变；只认指针会让过期的插入位置指向真实候选，云端候选直接消失 |
| 插入前校验列表身份并去重 | 否则结果会落进已经换过的组合，或覆盖/重复引擎刚生成的候选 |
| 结果与引擎候选重复就丢弃 | 本地词库已经很好，`shurufa` / `rengongzhineng` 这类词的云端结果与引擎完全一致，硬塞一行只是噪音 |
| 拼音规范化处理 `ü` | 引擎的 preedit 写 `lü`，服务商只认 `lv`；早期版本把整串判成"非拼音"直接跳过，于是所有带 ü 的词都没有云候选 |
| 失败三次即暂停 60 秒，网络类错误一次顶三次 | 上游是十次错误 + 5 分钟冷却：用户看到的是"开了但没反应"，而且很难等回来 |
| 只接受含中文的候选 | 纯 ASCII 的"候选"一定是解析错位（通常是拼音被原样返回），插进去毫无意义 |
| 候选要过"读法对不对得上输入"这一关 | 云端列表顺序不可靠：`yyds` 的第一条是拼音回显，`zhongguoren` 的第一条是同音词，按位置取会插进一个读音完全不同的词 |
| 按输入类型选服务商 | 两个词库在两个方向上强弱相反（见 §4），一次请求拿到正确的那一家，比并发问两家再投票便宜 |
| 服务商可显式探测 | 用户不该靠猜来判断是"没打开"还是"打不开"；`cloud probe` 现在连 `auto` 解析到哪一家一起打出来 |

---

## 4. 服务商（2026-11 实测）

| 服务商 | 端点 / 形状 | 强项 | 弱项 |
|---|---|---|---|
| `sogou` | `shouji.sogou.com/web_ime/mobile.php`，**二进制 POST** | 完整拼音：`zhongguoren` → 中国人 | 简拼只给通用短语：`yyds` → 永远都是 |
| `baidu` | `olimenew.baidu.com/py?inputtype=py&input=`，GET | 简拼：`yyds` → 永远的神、`zhrmghg` → 中华人民共和国 | 完整拼音会给同音词：`zhongguoren` → 中果人 |
| `googlecn` | `www.google.cn/inputtools`，GET | 国内可直连，回答稳定 | 只有候选列表，没有音节信息 |
| `auto`（默认） | 按输入类型分派 | 见下 | — |

**`auto` 的规则**：整音节输入（`nihao`、`zhongguoren`、`xi'an`）→ 搜狗；其余
（`yyds`、`zhrmghg`、末音节没打完的 `yuanshenqd`）→ 百度。判断"是不是整音节"用的是
一张音节表 + 分词，不猜、不联网。

两个服务商在两个方向上的强弱是**实测**的，不是偏好问题：

```text
zhongguoren   百度: 中果人（同音）      搜狗: 中国人
yyds          百度: 永远的神            搜狗: 永远都是
zhrmghg       百度: 中华人民共和国      搜狗: 无结果
```

### 响应形状

- **百度**：`{"status":"T","errno":"0","result":[[["你好",5,{"pinyin":"ni'hao"}],...]]}`。
  数字是候选覆盖的读法长度；`pinyin` 字段直接给出音节边界。
- **搜狗**：20 字节头（记录数在偏移 18，u16 LE），每条记录是
  `u16 字长 | UTF-16LE 词 | u16 音节 id 字节数 | u16 位置字节数 | u16[] 累计位置 | 1 旗标`。
  它**不发读音**，只发音节 id 和位置，所以读法是拿位置去切输入拼回来的。
- **Google**：`["SUCCESS",[["nihao",["你好","拟好"],[],{...}]]]`，没有音节边界，
  读法就是回显的那串，靠音节表分词。

### 一条命令验证

```bash
./build/linux-ui/tools/superior-config/superior_config_cli cloud probe --backend auto yyds
# backend   : auto (resolved to baidu)
# query     : yyds
# endpoint  : https://olimenew.baidu.com/py?inputtype=py&input=yyds
# http      : 200
# candidate : 永远的神
# verdict   : available
```

`auto` 会把解析到的服务商一起打出来：结果不对劲时，第一个要看的就是这个。

---

## 4.1 候选与输入对不对得上

云端返回的是一个**列表**，而且列表顺序不可靠：`yyds` 的第一条是拼音原样回显，
`zhongguoren` 的第一条是同音词。所以不取第 0 条，而是逐条过一遍
`usableCandidate()`，第一个通过的才用：

| 关卡 | 拒绝什么 | 为什么 |
|---|---|---|
| 覆盖整段输入 | `jintianqhh` → 今天 | 剩下的输入没人解释 |
| 读法能对齐输入 | 见下 | 对不上就是别的词的读音 |
| 至少两个汉字、只有汉字 | 单字、中英混排、符号 | 单字本地必有；混排无法"一个音节一个字" |
| 字数 == 音节数 | `gan'jing` → 干二净 | 同上 |

**对齐**（`alignReading`）要求每个音节按顺序出现，可以只打前缀，合起来必须覆盖整段：

```text
nihao        ni'hao             → 完整
yyds         yong'yuan'dou'shi  → 缩写（每个音节都出现了，只是打了前缀）
jintianqhh   jin'tian'qu        → 对不上（少了一个音节）
deepseek     de'e'pi'se'e'ka    → 对不上（元音开头的音节缺分隔符）
```

最后一条是故意的：`deepseek` 这种英文串按拼音切能切出一堆音节，但真正的拼音不会
在没有分隔符的情况下让一个元音开头的音节跟在别的音节后面。所以 `xi'an` 必须带
分隔符——`normalizeSegments()` 保留分隔符正是为了这个（`normalizePinyin()` 给 URL
用的是去掉分隔符的版本）。

搜狗的简拼结果**没有**可用读法（音节位置每段只有一个字母，那是首字母不是读音），
所以它会被"读法能对齐"这一关拒掉。这是正确的：没有读法就无法验证，而没有验证的
候选不值得占一格。`auto` 本来也不会把简拼交给搜狗。

### 为什么不做多源投票

微软拼音的做法是同时问两个词库，只采纳"至少两个词库都排在前两名"的词。那需要一个
并行的 fan-out 和一套确认规则，而我们的传输层是单请求队列 —— 为一个候选把延迟翻倍
不划算。按输入类型分派拿到了大部分收益（完整拼音走搜狗、简拼走百度），代价是一次
请求。

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

Rime 的做法是 `userdb`：选过的词被写进用户词典，下次同一拼音优先它。但它有两个
够不到的地方，所以这一层自己记：

- **云端候选是我们提交的**，引擎从没见过这个词；
- **引擎自己的学习是"慢慢调频"**：`wo` 选了 `握`，要连着选好几次它才肯超过 `我`。
  这里记的是"这个拼音用户要哪个词"，一次选中就成立。

文件位置（配置目录，不是数据目录）：

```
$XDG_CONFIG_HOME/superior-ime/pinyin/learning.json   # 默认 ~/.config/superior-ime/pinyin/
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
| 记录时机 | 用户**主动选中一个候选**就记——引擎的行、云端行都算；标点、`commit_code` 上屏的字母、语音片段都不算 |
| 位置 | 该拼音的已学词放到候选**第 1 位**（其余候选顺延） |
| 已存在 | 引擎自己已经给出同一个词时把它**移到**第 1 位，不再另插一行 |
| 改主意 | 同一拼音选了另一个词就**替换**，不并存 |
| 容量 | 512 条，超出按最久未用淘汰 |
| 权限 | 强制 `0600`：`~/.config` 常常是 0755，光靠 umask 不够 |
| 隐私 | 密码框与英文模式既不查询也不记录；日志只记长度 |

**关键点：本地偏好不依赖网络。** 关闭云拼音后，已学词依然会被提到第一位；网络
查询只是"发现新词"的途径。早期版本把整段逻辑塞在 `if (cloudEnabled_)` 里面，导致
关掉云拼音就把用户积累的偏好一起废掉了。

### 为什么学习发生在 select() 里

v2 曾在 `InputContextCommitString` 事件里学习，实测发现事件到达时引擎已经把自己的
组合清空了——`preedit=0`、`candidateList()` 也没了。于是 v2 只能在组合可见时
快照 `{pinyin, candidates}`，再用文本比对判断「用户是不是选了屏幕上的词」。这个
快照有时序 bug，而且只对 Fcitx5 有效。

v3 把学习放回它本来该在的位置：`Composer::select(row)`。

```text
select(row)
  ├─ pipeline.selectionAt(row) → { source=Engine, engineRow=N } 或 { source=Learned/Cloud, text }
  ├─ Engine: engine.selectCandidate(N) → takeCommit() → 引擎真正产出的文本
  ├─ Learned/Cloud: 直接提交该文本
  ├─ LearningStore.record(normalizedPinyin, commit)
  └─ pipeline.rebuild()
```

pinyin 和最终文本都在手里，不需要快照、不需要事件、也不需要判断「这个词当时在
不在候选里」。由 `processKey` 产生的提交（标点、空格上屏）不学习，因为用户没有选词。
这条规则在 Fcitx5、TSF、Android 上完全一致。

---

## 7. 设置项

`~/.config/superior-ime/settings.json`：

| 键 | 默认 | 说明 |
|---|---|---|
| `cloudPinyinEnabled` | `false` | 总开关 |
| `cloudPinyinBackend` | `"auto"` | `auto` / `sogou` / `baidu` / `googlecn` |
| `cloudPinyinMinLength` | `2` | 少于这个长度不查 |
| `cloudPinyinIndex` | `2` | 云端候选出现的位置（1 起） |

运行时把这四个值折成 `api::PipelineOptions`，通过 `Composer::setOptions()` 推给
现有会话；改设置后不需要重建输入框，也不会留下第二份会过期的设置。

改动 500 ms 内生效，不需要重启。

---

## 8. 已知取舍

- 云端候选上屏走 `commitString()`，**不进入引擎的用户词频学习**：我们没有引擎内部
  句柄，硬塞会有副作用。代价是选过的云端词不会因此提升引擎的权重——所以才有
  第 6 节那套自己的记录。
- 只取服务商返回的**一条**候选（第一条通过校验的），不铺开整个列表：云端一次
  返回十几条会把本地排序冲掉，而那正是本地语言模型更擅长的部分。
- 不做跨源投票：那要并行 fan-out，为一个候选把延迟翻倍不划算（见 §4.1 末尾）。
- 缓存只有容量上限（256 条），没有 TTL：同一拼音的云词几乎不会变，换服务商时整表
  清空已经足够。上游按"完整/部分/空"分 30 分钟 / 60 秒 / 2 秒三档，是因为它的答案
  综合了多个词库的投票，晚到的结果还可能回填；我们只有一个来源，重试没有新信息。
- 不再有上游 `fcitx5-cloudpinyin` 共存的问题；占位符检测保留，用于识别列表里已有的云标记。
