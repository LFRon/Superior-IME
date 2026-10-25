# 词库：与 fcitx5-pinyin 对齐

Superior-IME **不实现自己的词库格式**。拼音引擎是 `fcitx5-pinyin` + `libime`，
所以唯一正确的做法是读取同一批文件、放在同样的位置、用同样的工具导入。

参考源码：`域外源码/fcitx5-pinyin-5.1.6`。

---

## 1. 目录布局

`backend/platforms/fcitx5/src/Dictionary.cpp` 复刻了
`PinyinEngine::PinyinEngine()`、`loadExtraDict()`、`save()` 的查找逻辑。

| 类别 | 路径 | 说明 |
|---|---|---|
| 系统词库 | `<datadir>/libime/sc.dict` | `PinyinDictionary::SystemDict` |
| 扩展字库 | `<datadir>/libime/extb.dict` | 生僻字 |
| 拆字 | `<pkgdatadir>/pinyin/chaizi.dict` | 可选 |
| 用户词库 | `<pkgdata>/pinyin/user.dict` | `UserDict`，Fcitx5 自己维护 |
| 用户语言模型 | `<pkgdata>/pinyin/user.history` | 用户习惯 |
| 额外词库 | `<pkgdata>/pinyin/dictionaries/*.dict` | **搜狗导入结果落在这里** |
| 禁用额外词库 | `<pkgdata>/pinyin/dictionaries/*.dict.disable` | 改名即禁用 |
| 语言模型 | `zh_CN.lm`（libime resolver） | 通常 `/usr/lib/<triplet>/libime/` |

其中：

- `datadir` 来自 `XDG_DATA_DIRS`（默认 `/usr/local/share:/usr/share`）
- `pkgdata` 是 `<datadir>/fcitx5`
- 用户目录优先 `XDG_DATA_HOME`，否则 `~/.local/share`

查看当前解析结果：

```bash
./build/linux-ui/tools/dict-tool/superior_dict diagnose
```

输出示例（本机）：

```
system dict     : /usr/share/libime/sc.dict
extension dict  : /usr/share/libime/extb.dict
chaizi dict     : /usr/share/fcitx5/pinyin/chaizi.dict
user dict       : (not created yet)
user history    : (not created yet)
extra dict dir  : (missing)
language model  : /usr/lib/x86_64-linux-gnu/libime/zh_CN.lm
extra dicts     : 0
disabled dicts  : 0
engine usable   : yes
scel converter  : missing (install fcitx-tools)
```

---

## 2. 为什么不做“自己的词库”

1. **引擎负责词库。** 候选生成、语言模型、用户学习都在 libime 里；
   另建一份只会产生两个真相。
2. **格式是二进制的、随版本演进。** 复制解析器意味着跟随 libime 升级。
3. **用户已经在 Fcitx5 里积累了词库。** 复用可以让用户零迁移。

Superior 的职责是**观测与呈现**：检测数据是否齐备、把问题写进日志、
在缺失时给出可执行的建议。

```text
superior.pinyin ... libime dictionary available dict=... lm=... userDict=absent
superior.pinyin ... libime dictionary incomplete; pinyin candidates may be empty.
                    Install libime-data and libime-data-language-model.
```

---

## 3. 搜狗细胞词库导入

### 3.1 与 fcitx5-pinyin 完全一致的流程

`fcitx5-pinyin` 的词典管理器（`gui/pinyindictmanager/pinyindictmanager.cpp`）
对本地的 `.scel` 文件执行：

```
scel2org5 <input.scel> -o <tmp.txt>
libime_pinyindict <tmp.txt> <tmp.dict>
rename <tmp.dict>  <pkgdata>/pinyin/dictionaries/<stem>.dict
```

`Superior::Fcitx5Mapping::importSogouScel()` 做同样的事：

```bash
./build/linux-ui/tools/dict-tool/superior_dict import 网络流行.scel
# imported: ~/.local/share/fcitx5/pinyin/dictionaries/网络流行.dict
```

然后重启 Fcitx5 生效：

```bash
fcitx5-remote -e && fcitx5 -d
```

### 3.2 依赖

| 工具 | 来源 | 作用 |
|---|---|---|
| `scel2org5` | `fcitx-tools`（或发行版的 scel2org 包） | `.scel` → 文本 |
| `libime_pinyindict` | `libime-bin` | 文本 → 二进制 `.dict` |

两者缺失时导入会失败，并给出明确提示，不会静默产出坏文件。

### 3.3 安全实现

外部工具调用没有使用 `system()` / `popen()`：

- `posix_spawnp` + 显式 argv 数组，**不经过 shell**，文件名无法注入命令；
- stdin 接到 `/dev/null`，stdout/stderr 接到管道并截断到 4 KiB；
- 临时文件用 `mkstemp` 创建在目标目录内，并设置 `FD_CLOEXEC`；
- 所有失败路径都会删除临时文件；
- 输入大小上限 64 MiB，文本上限 256 MiB，词典上限 512 MiB；
- 目标已存在时默认拒绝，必须显式 `--overwrite`；
- 日志只记录文件名、字节数、工具名和退出码，**不记录词条内容**。

### 3.4 不做的事

- 不内置 `.scel` 二进制解析器。格式未公开且会变，交给 `scel2org5`。
- 不下载在线词库。需要联网的行为不在运行时里静默发生。
- 不修改 `user.dict` / `user.history`。那是 Fcitx5 的所有权。

---

## 4. 词库与"分词差"不是一回事

实测过一组容易误判的案例：输入 `yubianyi` 得到四字的「御笔安逸」。看起来像词库
质量问题，实际是 libime 的 `[Fuzzy] Inner`：

| 选项 | 含义 | 默认 |
|---|---|---|
| `Inner` | 允许一个音节拆成两个：`xian` → `xi'an`，`bian` → `bi'an` | **True** |
| `InnerShort` | 只对短输入生效：`qie` → `qi'e` | True |
| `PartialFinal` | 补全不完整韵母：`e` → `en/eng/ei` | True |

`yu bian yi` 只有三个音节，但 `Inner` 允许把 `bian` 拆成 `bi`+`an`，于是
「御笔安逸」（yu'bi + an'yi）作为四音节句子的分数更高。

关掉 `Inner`（保留 `InnerShort`）后：

```
yubianyi →  1. 预编译   2. 与变异   3. 与便以  …
qie      →  1. 其二     2. vie      3. 企鹅    …   ← InnerShort 仍生效
xian     →  1. 先  2. 县  3. 现  …            ← 不再有「西安」
```

代价是 `xian` 不再给出「西安」（要打 `xi'an`）。这是引擎级取舍，**不是词库能修的**：
补再多词条，只要 `Inner` 开着，`bian` 依然会被拆开。

设置页「输入 → 拼音切分」提供这个开关，写的是 `conf/pinyin.conf` 的 `[Fuzzy] Inner`；
因为 `super-ime` 与「拼音」共用 `Addon=pinyin`，改它对两者同时生效。

---

## 5. 搜狗细胞词库能补到什么程度（实测）

爬了 `pinyin.sogou.com/dict/` 的分类目录，共 **3,411** 个词库（43 个分类 × 最多
25 页）。抽样结论：

| 事实 | 数据 |
|---|---|
| "官方推荐"里大量是影视/动漫/游戏/粉丝词库 | 220 个「官方推荐」中通用词汇类不到 20 个 |
| 单个通用词库很小 | 最大「日常大词库」17,089 条 |
| 很多词库高度冗余 | 「全国地名」2,595 条里只有 51 条系统词库没有 |
| 权重全是 0 | `scel2org5` 输出的每条权重都是 0，**只补词，不调排序** |
| 新格式会让转换器 abort | 文件头 `ECS`（如「网络流行新词」）触发 `scel2org5` 断言，SIGABRT |

挑 60 个通用/专业词库合并后：**269,359 条**，其中 88.6% 是系统词库没有的；
但相对 rime-ice（881,612 条）只有 **198,519 条是新的**，且抽样看多为垂直术语
（`腭裂语音`、`枢轴线`、`拒绝域`、`捶方程`），日常覆盖增量有限。

因此定位：**搜狗细胞词库当可选领域补充，不当底色**。底色用 Rime 系（雾凇），
新词交给云拼音，个人习惯交给 `learning.json`。

---

## 6. 代码位置

| 文件 | 内容 |
|---|---|
| `backend/platforms/fcitx5/include/superior/fcitx5/Dictionary.hpp` | 布局、导入 API |
| `backend/platforms/fcitx5/src/Dictionary.cpp` | XDG 解析、进程调用、临时文件 |
| `backend/platforms/fcitx5/include/superior/fcitx5/PinyinAssets.hpp` | 面向日志的可用性探测 |
| `backend/platforms/fcitx5/src/PinyinAssets.cpp` | 基于同一布局的摘要 |
| `tools/dict-tool/main.cpp` | `diagnose` / `import` CLI |

---

## 7. 测试

`tests/dictionary_tests.cpp`：

- `XDG_DATA_DIRS` / `XDG_DATA_HOME` 覆盖下的布局解析；
- 额外词库与 `.dict.disable` 识别；
- `describeDictionaryLayout` 字段完整；
- 导入拒绝：缺失输入、目录、超大输入、无转换器；
- 空 `PATH` 时 `findScelConverter()` 返回空。

测试不会触碰真实的 `~/.local/share/fcitx5`，全部使用临时目录。
