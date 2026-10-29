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
  RimeInputMethod.{h,cpp}    Fcitx5 适配层，一个 IC 一个 librime session
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

云端词和学习词是 agent 插入到**可见列表**里的行，librime 根本不知道它们存在。
如果数字键直接交给 librime，按 `2` 会选中引擎自己的第 2 个词，而不是屏幕上
显示的第 2 行。

所以 `RimeInputMethod::keyEvent()` 先处理选择键：

- `1`–`9`、`0`：按**可见行**调用 `CandidateWord::select()`；
- `Space`：选中当前高亮行；
- `↑`/`↓`：只移动可见列表的光标；
- 其他键：交给 librime。

引擎词保存的是 **librime 的索引**而不是行号，所以前面插入了学习词也不会错位。
点击候选走同一条 `select()` 路径，键盘和鼠标行为一致。

列表对象在候选集合不变时**复用**（比较 `builtTexts`），否则每次按键都重建，
agent 插入的云端行会立刻消失。

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
- 运行期还有一道保险：`RimeInputMethod` 在 Shift/CapsLock 事件后如果发现
  `ascii_mode` 被打开，会立即关掉并记一条 debug 日志；`RimeEngine::start` 也会
  在会话开始时强制 `ascii_mode=false`。这是为了照顾还没来得及重新部署的数据目录，
  不是替代上面的 patch。

## 标点全角/半角

状态栏的月牙/实心圆不是装饰，它对应 schema 的 `ascii_punct` 开关：

- `ascii_punct=false`（默认，中标/全角）：`,` → `，`、`.` → `。`；
- `ascii_punct=true`（英标/半角）：`,` → `,`、`.` → `.`。

`RimeInputMethod` 注册一个名为 `punctuation` 的 `SimpleAction`，agent 已有的
`StatusMapping` 直接把它画成月牙/实心圆；点击时：

1. 读取当前会话的 `ascii_punct` 并取反；
2. 把这个值写入**所有**打开的 session（这个开关是方案级的，下一个窗口也必须一致）；
3. 更新 action 的 checked/短文本/图标，触发 `StatusArea` 重绘。

`activate` 时（以及确实需要重建会话时）会复制工具栏状态，所以切窗口不会让标点
突然变回另一种宽度。状态目前不跨 Fcitx5 重启持久化：重启后回到方案默认的中标/全角。

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
