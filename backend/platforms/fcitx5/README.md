# backend/platforms/fcitx5

Fcitx5 适配层。这里有两个**不同**的注册面，混在一起就会“装好了却看不到”。

| 产物 | 类型 | 作用 |
|---|---|---|
| `libsuperioragent.so` + `superioragent.conf` | addon，`Category=Module` | 观察者 + 浮层：读 `InputPanel`/`StatusArea`/`cursorRect`，经 socket 推给 UI |
| `libsuperiorrime.so` + `superiorrime.conf` | addon，`Category=InputMethod` | 真正的拼音引擎：内置 librime，万象方案 |
| `super-ime.conf` | 输入法条目（`[InputMethod]`） | 出现在“添加输入法”里；uniqueName = `super-ime` |
| `org.superior.ime.svg` | 图标 | 条目图标 |

`super-ime.conf` 用 `Addon=superiorrime` 绑定本仓库的引擎。**只有这一个条目**：
原来绑定 `Addon=pinyin`（libime）的条目已经删除，不再有第二个“Rime 版”可选，
也不和系统的 `pinyin` addon 共享配置或词库。

## 分工

- `RimeInputMethod`（`libsuperiorrime.so`）负责按键、组合、候选、提交和翻页，
  一个输入框一个 librime session。
- `SuperiorAgent` 不是引擎：它接管 `InputPanel` 的绘制、把场景发给 Qt 浮层、
  在候选列表里插入云端词/学习词，并处理 Shift 轻按（英文模式直接提交字母）。
- 两者都只通过 Fcitx5 的公开接口交互，互不 include。

## 生效方式

新增输入法条目后必须**重启** Fcitx5：

```bash
fcitx5-remote -e && fcitx5 -d
```

`fcitx5-remote -r` 不会重扫条目；`Controller1.ResetIMList` 会重建分组并丢掉
用户条目，不要用。详见 [docs/fcitx5-integration.md](../../../docs/fcitx5-integration.md)。

```bash
fcitx5-remote -m super-ime   # 条目存在
fcitx5-remote -s super-ime   # 切换
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
scripts/fetch-rime-data.sh
```
