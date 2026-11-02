# backend/platforms/fcitx5

Fcitx5 宿主。这里有一个容易混淆的区分：**addon** 和**输入法条目**是两个
不同的注册面，混在一起就会「装好了却看不到」。

| 产物 | 类型 | 作用 |
|---|---|---|
| `libsuperiorpinyin.so` + `superiorpinyin.conf` | addon，`Category=InputMethod` | 宿主：按键翻译、文本执行、场景呈现、原生候选面板，组装 runtime 与拼音能力 |
| `super-ime.conf` | 输入法条目（`[InputMethod]`） | 出现在「添加输入法」里；uniqueName = `super-ime` |
| `org.superior.ime.svg` | 图标 | 条目图标 |

`super-ime.conf` 用 `Addon=superiorpinyin` 绑定本仓库的宿主。**只有这一个条目**，
也不和系统的 `pinyin` / `rime` addon 共享配置或词库。

## 分工

v3 之后只有一个 addon，三层在同一条加载路径里：

- **宿主**（`Fcitx5Addon`）：把 `fcitx::KeyEvent` 翻成 `api::KeyPress`，把
  `InputContext` 的事实填进 `api::HostFacts`，实现 `TextSessionPort` /
  `ScenePresenter` / `Scheduler` / `NativePanelSink` / `StatusSink`。
  它不持有产品状态，不决定候选顺序，不决定画不画。
- **内核**（`runtime::Runtime`）：每个输入框一个 `Session`，持有 `ImeSession`、
  `Modes`、设置、语音状态；按键、手势、UI 命令、定时器都只经过它。
- **能力**（`pinyin::RimeComposer`）：一个输入框一个 librime session，内部
  合成 engine 候选、学习词和云端词，产出 `api::CompositionFact`。

旧的 `SuperiorAgent`（`Category=Module` 观察者）和 `RimeInputMethod`
（`Category=InputMethod` 引擎）已经被合并。两个 addon 时，候选列表是它们之间的
隐式通道：一方建列表，另一方用 `toModifiable()` 插行、用文本比对认自己的行，
组合因此有两个 owner。v3 把组合收到能力包里。

## 生效方式

新增输入法条目后必须**重启** Fcitx5：

```bash
fcitx5-remote -e && fcitx5 -d
```

`fcitx5-remote -r` 不会重扫条目；`Controller1.ResetIMList` 会重建分组并丢掉
用户条目，不要用。详见 [docs/fcitx5-integration.md](../../../docs/fcitx5-integration.md)。

```bash
fcitx5-remote -n            # 当前输入法
fcitx5-remote -m super-ime  # 条目存在（输出 addon 名）
fcitx5-remote -s super-ime  # 切换
fcitx5-remote -o            # 激活（XIM 客户端接入前可能是 inactive）
```

## 构建与安装

```bash
cmake --preset linux-system
cmake --build --preset linux-system --parallel
sudo cmake --install build/linux-system
```

或 `scripts/install-addon-system.sh`（支持 `--dry-run`）。

安装后还需要方案数据（不属于仓库）：

```bash
scripts/fetch-wanxiang-schema.sh
```

## 从 v2 升级

`scripts/install-user.sh` 会删除用户目录里的旧 `libsuperioragent.so` /
`superioragent.conf`。如果曾用 root 安装过旧版，还需要一次性清理系统副本：

```bash
sudo rm -f /usr/lib/*/fcitx5/libsuperioragent.so \
           /usr/share/fcitx5/addon/superioragent.conf
```

否则 Fcitx5 会尝试加载两个宿主（一个失败、一个成功，或两个都抢同一个 UI socket）。
