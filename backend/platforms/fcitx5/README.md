# backend/platforms/fcitx5

Fcitx5 适配层。这里有两个**不同**的注册面，混在一起就会“装好了却看不到”。

| 产物 | 类型 | 作用 |
|---|---|---|
| `libsuperioragent.so` + `superioragent.conf` | addon，`Category=Module` | 被动观察者：读 `InputPanel`/`StatusArea`/`cursorRect`，经 socket 推给 UI |
| `super-ime.conf` | 输入法条目（`[InputMethod]`） | 出现在“添加输入法”里；uniqueName = `super-ime` |
| `org.superior.ime.svg` | 图标 | 条目图标 |

`super-ime.conf` 用 `Addon=pinyin` 绑定 Fcitx5 自带的 libime 拼音引擎。
Superior 不重写拼音，只是给引擎一个自己的名称、图标和标签，并让 UI 观察会话。

## 不干扰输入的保证

- agent 不是 `InputMethodEngine`，不处理按键；
- agent 不是 `UserInterface`，不占用 Fcitx5 唯一的 UI addon 槽位；
- 只读 `InputPanel`、`StatusArea`、`InputContext`；
- 从不调用 `setPreedit` / `setClientPreedit` / commit 接口。

因此 Wayland 客户端继续走 Fcitx5 的 wayland 前端，X11 客户端继续走 XCB/XIM 前端，
客户端内的预编辑行为不变。Superior 只负责额外浮层与皮肤。

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
