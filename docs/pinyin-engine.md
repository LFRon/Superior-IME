# 内置 librime 拼音引擎

拼音引擎是**自己内嵌的 librime**，不是 `fcitx5-rime`，也不是 Fcitx5 的 libime。
Fcitx5 里只有一个条目 `super-ime`，绑定本仓库的 `libsuperiorpinyin.so`；旧的
libime 条目已经删除，不存在第二个“Rime 版”可选。

皮肤、状态栏、云拼音、学习记录和 Shift 中英文切换都挂在 `super-ime` 上，
不随引擎实现变化。

## 为什么不是 fcitx5-rime

`fcitx5-rime` 是一个独立引擎，它的候选列表、组合文本和按键处理都在自己的
addon 里，Superior 只能旁观，无法把「云拼音插到第 2 位」「学过的词提到第 1 位」
这类后处理做对。内嵌 librime 后：

- 引擎是**我们自己的** `InputMethodEngineV2`，候选列表归我们所有；
- 后处理行（云端词、学习词）和引擎词共用一张可见列表，选中哪个由行自己决定；
- 数据目录独立，绝不碰用户已有的 `~/.config/fcitx5/rime`。

`librime` 通过 `dlopen("librime.so.1")` 加载，没有链接期依赖：没装 librime 的
机器上 `libsuperiorpinyin.so` 仍能加载，并在日志里说明原因，而不是让整个 addon
加载失败。

## 分层

```
backend/pinyin/                纯逻辑，不含 Qt / Fcitx5，可单测
  Types.hpp                  按键、组合、菜单、快照、部署状态
  RimePort.hpp               引擎需要的全部 librime 能力（接口）
  RimeEngine.{hpp,cpp}       状态机：按键、提交、翻页、ASCII 模式
  Layout.{hpp,cpp}           数据目录布局与旧目录迁移判断
  LibrimePort.{hpp,cpp}      dlopen + RimeApi 函数表；公共头不出现 rime_api.h

backend/platforms/fcitx5/
  Fcitx5Addon.{h,cpp}        Fcitx5 宿主 + 组装根：按键翻译、文本、面板、场景
  RimeComposer.{hpp,cpp}     api::Composer 实现：一个输入框一个 librime session
  CandidatePipeline.{hpp,cpp} engine + learned + cloud 合成一页
  LearningStore.{hpp,cpp}    选中即学习的持久化
  CloudService.{hpp,cpp}     云去抖、请求、缓存、错误预算
```

`backend/pinyin` 的公共头禁止包含 `rime_api.h` / `dlfcn.h`，也禁止 Qt/Fcitx5，
由 `scripts/check-boundaries.sh` 强制。这样同一套状态机以后可以直接给
Windows TSF 或 Android 用。

`reset()` / `deactivate()` **不销毁 librime session**。早期实现每次 `forget()` 再
`create_session()`，一次约 30 ms；Telegram 的 XIM 客户端会在每个键上触发
`XResetIC`，于是每个字母都重新加载一次方案。现在用
`clear_composition()` 原地清空组合，并保留面板的 custom callback，只有
`InputContextDestroyed` 才真正销毁 session。

## 数据目录与部署

```
$XDG_DATA_HOME/superior-ime/pinyin/
  shared/     方案、词库、opencc 数据（只读）
  user/       userdb、learning、编译产物 build/*.bin
```

librime **不会**自动编译方案：没有 `user/build/*.bin` 时 `create_session` 成功
但 `schema_id` 为空，输入没有候选。因此部署是显式的一步：

```bash
scripts/fetch-wanxiang-schema.sh          # 解压随仓库的万象 Pure 并部署
scripts/fetch-wanxiang-schema.sh --no-deploy
scripts/fetch-wanxiang-schema.sh --gram   # 额外下载 399 MB 语法模型
scripts/fetch-wanxiang-schema.sh --force  # 忽略随附归档，重新下载上游
scripts/fetch-wanxiang-schema.sh --offline  # 绝不联网（归档不可用就报错）

superior-config pinyin status         # 库版本、目录、产物是否存在
superior-config pinyin deploy
superior-config pinyin lookup nihao
```

`install-user.sh` 会把 CLI 装成 `~/.local/bin/superior-config`；没安装时用
`build/<preset>/tools/superior-config/superior_config_cli`。

- 默认方案：**万象拼音 Pure**（`wanxiang_pure`），版本固定在脚本里并校验
  SHA-256，避免词库被静默替换。
- 首次部署约 10 秒（本机 SSD），产物在 `user/build/`，约 100 MB。
- 部署放在 CLI 而不是 addon：首次编译会阻塞数秒到数分钟，放在 Fcitx5 进程里
  要么卡住所有按键，要么得开线程，而 Fcitx5 会 `dlclose` addon，线程不安全。
- `pinyin lookup` 是最快的端到端自检：它真的开一个 session、按键、打印候选，
  能把「没候选」和「addon 没启用」区分开。

## 候选与按键：为什么选中要绕过引擎

云端词和学习词是 `CandidatePipeline` 合成到**可见页**里的行，librime 根本不知道
它们存在。如果数字键直接交给 librime，按 `2` 会选中引擎自己的第 2 个词，而不是
屏幕上显示的第 2 行。

v3 的分工：

- 内核（`runtime::Runtime`）先识别选择/翻页键：
  `1`–`9`/`0` → `Composer::select(row)`，`Space` → 选中高亮行，
  `↑`/`↓` → `Composer::moveSelection()`，`+`/小键盘 `-` → `Composer::page()`；
- **没有候选时空格按「上屏原文」处理**：`weibrds` 这种输入引擎只能切出 `wei`，
  选完之后合成里只剩文本（`为brds`）、候选页是空的。此时空格不是「选中第 0 行」
  （那会什么都不发生，按键被吞掉），而是 `Composer::commitLetters()` → 把
  `为brds` 原样交给宿主。这与 librime 自己处理空格的结果逐字相同（实测：
  `select_candidate(0)` 之后 preedit 为 `为brds`、无候选，引擎空格 commit `为brds`），
  也不再依赖「空格总能选中点什么」这个不成立的前提；
- `CandidatePipeline` 保存每一行的 `source` 与 `engineRow`：
  - `Engine` 行：`engine.selectCandidate(engineRow)`，再取引擎真正产出的文本；
  - `Learned`/`Cloud` 行：直接提交该文本；
- 学习发生在 `select()` 里，pinyin 与最终文本都在手里，不依赖 commit 事件的快照；
- 浮层点击和数字键走同一条 `select()` 路径，键盘和鼠标行为一致。

引擎行保存的是 **librime 当前页的行号**而不是合成后的行号，所以前面插入学习词
或云端词也不会错位。候选页由 `CandidatePipeline` 重建，宿主不再比较
`builtTexts`、也不再往 `fcitx::CandidateList` 里插行。

### 高亮行同样属于合成后的列表

`CompositionFact::highlighted` 是**可见页**的下标，不是引擎行号。规则：

| 情况 | 高亮 |
|---|---|
| 新词（preedit 变了，包括刚上屏后的第一个键） | 第 0 行 |
| 翻页 | 新页第 0 行 |
| 同一个词、同一页的列表重建（云端到货、学习词刷新） | 原来那一行（按文本找回） |
| 引擎自己移动了高亮（schema 绑定了选择键） | 跟随引擎，映射回合成后的下标 |

曾经的做法是「列表变了就按 `menu.highlighted` 找引擎行」。学习词被提升到第 0 位后，
引擎的第 0 行正好落到第 1 行，于是标记停在第二个候选上，空格上屏的也是它 ——
用户看到的正是「输入 z 之后选框跳到第二个词」。引擎行号只在
`selectionAt()` 里换算成真正的选择，不参与高亮。

## 中英文切换：只有一个所有者

Shift 轻按切换由 **agent** 实现（`ShiftTapDetector` + 英文模式直接提交字母）。
librime 的 `ascii_composer` 默认也会在 `Shift_L`/`Shift_R`/`Caps_Lock` 上切换它
自己的 `ascii_mode`，两个状态会各自翻转、互相错位：表现就是**切回中文后仍然只能
打英文**，而且切换瞬间的 `commit_code` 还会把未完成的拼音原样上屏。

因此部署脚本会在 **user 目录**写一份 `default.custom.yaml`，把所有开关键设为
`noop`：

```yaml
patch:
  ascii_composer/switch_key:
    Caps_Lock: noop
    Shift_L: noop
    Shift_R: noop
    Control_L: noop
    Control_R: noop
```

- 文件由 `scripts/fetch-wanxiang-schema.sh` 写入，随后自动重新部署；升级代码后需要
  再跑一次，否则旧部署里仍是 `commit_code`。
- `default.custom.yaml` 属于内置引擎自己的数据目录，不要复制到
  `~/.config/fcitx5/rime`。
- `RimeEngine::start` 在会话开始时强制 `ascii_mode=false`；内核是语言模式的
  唯一 owner，不会把 `Shift` 交给 librime 去翻转。`default.custom.yaml` 仍把
  `Shift`/`CapsLock` 绑成 `noop`，是为了照顾还没来得及重新部署的数据目录。

## 标点全角/半角

状态栏的月牙/实心圆不是装饰，它对应 schema 的 `ascii_punct` 开关：

- `ascii_punct=false`（默认，中标/全角）：`,` → `，`、`.` → `。`；
- `ascii_punct=true`（英标/半角）：`,` → `,`、`.` → `.`。

标点宽度由内核的 `core::Modes` 唯一持有：

1. 点击状态栏的月牙/实心圆 → `runtime.command(ToggleStatusItem{"punctuation"})`；
2. 内核 `togglePunctuation()`，再把 `ascii_punct` 下发给该会话的 composer；
3. 宿主把 runtime 的状态项映射成 Fcitx5 tray action，并重绘状态区。

切窗口不会让标点突然变回另一种宽度：新建 composer 时内核会重新下发 `Modes`。
状态目前不跨 Fcitx5 重启持久化：重启后回到方案默认的中标/全角。

## 表情候选（`emoji`）

万象方案自带表情词表：`wanxiang_pure.schema.yaml` 的 `filters` 里有
`simplifier@emoji`，`switches` 里有 `emoji`（`表情关`/`表情开`）。打开之后 librime
会把表情当成**独立候选**返回，所以输入 `ku` 时列表里会同时出现 `哭` 和 `😭`：

```text
emoji=false → 库 苦 哭 酷 裤 枯
emoji=true  → 库 苦 哭 😢 😭 酷      （实测，同一份词库）
```

这个开关默认**开**，唯一的所有者是 `settings.json` 的 `emojiCandidates`：

| 项 | 位置 |
|---|---|
| 偏好 | `$XDG_CONFIG_HOME/superior-ime/settings.json` → `"emojiCandidates": true/false` |
| 设置页 | 「拼音」卡片 → 「表情候选」 |
| 下发 | `runtime::Runtime::applyPreferences()` 在创建/激活会话时写一次 `setOption("emoji", …)` |
| 热生效 | 内核 `reloadSettings()` 按 `settings.json` 的 mtime 重读，最多 200 ms 一次 |

有两个细节值得记下：

1. **方案自己也会记这个开关**。`default.yaml` 的 `switcher/save_options` 包含
   `emoji`，所以 librime 会把状态存进 `user.yaml`。我们每次激活都显式写一遍，谁最后
   写谁生效——这就是"设置文件是唯一所有者"的机制，不需要禁用方案的记忆功能。
2. **切换后只影响下一个组合**。已经显示在屏幕上的候选列表是上一次按键算出来的，
   不会凭空重排；再按一个键就正常了。

### 彩色渲染：为什么要显式指定字体

候选文字是纯文本，表情也是一段 UTF-8（例如 `😭` = `f0 9f 98 ad`）。Qt 会为缺字回退，
但在装了多个 emoji 字体的桌面上，回退**不保证**落到彩色那一个。实测（同一段文本、
同一个 10.5 pt 字号）：

| 字体设置 | 画出来 |
|---|---|
| 只给系统默认族 | 单色轮廓（看起来像个"空框"） |
| `setFamilies({默认族, "Noto Color Emoji"})` | 黄脸 `😭` |

所以 `EmojiFont.{hpp,cpp}` 里有一张偏好表（`Noto Color Emoji` / `Apple Color Emoji`
/ `Segoe UI Emoji` / …），`CandidateWindow::candidateFont()` 把**实际装有**的那一族
追加到候选字体族列表末尾。注意测量与绘制必须用同一个 `QFontMetrics`，否则格子按没有
表情的宽度算，表情会被裁掉——这也是两者共用一个 `candidateFont()` 的原因。

**emoji 族必须排在最后，具体族名必须排在最前。** Qt 的应用默认字体常常是一个通用名
（"Sans Serif"），在 Wayland 会话里没有桌面主题提供字体时尤其如此。把 emoji 族追加在
通用名后面，Qt 会把**整个字体**解析成 emoji 族：

```
QFontInfo(candidateFont()).family() == "Noto Color Emoji"
```

Noto Color Emoji 对 ASCII 数字有 glyph（给 keycap `1️⃣` 用的那种），于是候选编号被画成
暗淡的灰字，汉字却因为 emoji 族没有 CJK 而回退到正常字体——看起来就是「数字显示不对，
而且只有 Wayland 下明显」。`EmojiFont` 的 `candidateFamilies()` 因此把
`QFontInfo(默认字体).family()` 解析出的**具体族**放在第一位，原列表其次，emoji 族最后：

| 字体设置 | 数字 | 表情 |
|---|---|---|
| `{默认族(通用名), emoji}`（旧） | 暗灰 keycap | 彩色 |
| `{具体族, 默认族, emoji}`（现） | 与汉字同色 | 彩色 |

没有装任何 emoji 字体时不会报错，候选照常显示，只是单色；日志里会有一条
`no emoji font installed; emoji candidates will be monochrome`。

## 翻页

`RimeApi::change_page()` 在 librime 1.17 里是空实现（返回 false，菜单不动）。
翻页改为向引擎发送 `Page_Up` / `Page_Down` 按键，与键盘 `-`/`=` 走同一条
`key_binder` 路径，因此也能尊重方案自己的键位。

键盘侧另有一条 Sogou 习惯的旁路：候选列表可见时 `+`（含小键盘 `+`）直接翻到
下一页，小键盘 `-` 翻回上一页。之所以不能只靠 `key_binder`，是因为大多数布局
上 `+` 就是 `Shift+=`，方案只绑了 `minus`/`equal`，`+` 会被标点引擎当成文本
提交并结束组合——这正是「按 + 想翻页，结果上屏了第一页的词」的成因。`+` 在
末页同样被消费掉，不会退回去插入一个加号；没有候选列表时它仍然是普通字符。
`-`/`=` 保持交给 librime，避免覆盖用户自定义键位。

翻页之后选择候选用的是**页内行号**：`RimeApi::select_candidate()` 接受的是整个
候选列表的绝对索引，`LibrimePort::selectCandidate()` 会先读一次 `RimeContext`，
按 `page_no * page_size + index` 换算再交给 librime，行号超出当前页则直接失败。
少了这层换算，翻到第二页选「第 1 行」提交的仍是第一页的同位置词。

滚轮由 UI 发 `PageCandidates`，agent 调 `PageableCandidateList::next()/prev()`，
`RimeCandidateList` 覆写这两个方法后转发给引擎。

## 语法模型（`.gram`）

万象方案自带 `grammar:` 段。没有 `.gram` 文件时，octagram 每次建 session 都会
打两条错误日志。默认流程因此**把 `grammar:` 段从解压出来的 schema 里删掉**，
行为就是纯词库 + 用户词典。

需要语法模型时：

```bash
scripts/fetch-wanxiang-schema.sh --gram
```

它会把 `wanxiang-lts-zh-hans.gram` 放到 **`user/`**（librime 是按 user 目录找这个
文件，不是 shared），保留 schema 的 `grammar:` 段并重新部署。系统里的
`librime-plugin-octagram` 会被 librime 自动加载；没有该插件时 gram 不生效，
但不会影响候选。

## 已知边界

- 万象的模糊音是 `wanxiang_algebra.yaml` 里的可选 patch，本版没有把它接到
  设置页；当前的「打错能纠正」来自 `cuoyin` 词库和 librime 自身。
- `super-ime` 的 `Configurable=False`：还没有引擎专用的配置页。
- 用户词典按方案名存放（`user/wanxiang_pure.userdb`），删除该目录即清空学习。
- 表情候选只是"把方案自带的表情当成候选"，不做 emoji 搜索、不做分类联想；想要
  `:smile` 这类输入法式表情，需要方案里另加 recognizer，本版没有。

## 许可

- 引擎代码：GPL-3.0-or-later（本仓库）。
- `third_party/rime/rime_api.h`：BSD-3-Clause，版权归 RIME Developers。
- 万象拼音方案与词库（**随仓库分发**于 `data/wanxiang/rime-wanxiang-pure.zip`，
  由 `fetch-wanxiang-schema.sh` 解压部署）：CC-BY-4.0，作者 amzxyz，来源
  <https://github.com/amzxyz/rime-wanxiang>。署名、许可链接与我们的改动说明见
  [../data/wanxiang/README.md](../data/wanxiang/README.md)。

## 测试

```bash
ctest --test-dir build/linux-ui -R 'pinyin-engine|rime-dict'
```

`tests/pinyin_engine_tests.cpp` 用假 port 覆盖：release 位、修饰键透传、提交只
drain 一次、部署后重开会话、候选索引校验、路径隔离、产物检测。
真实 librime 的行为用 `superior-config pinyin lookup` 验证，不在 ctest 里跑
（它需要下载数据）。
