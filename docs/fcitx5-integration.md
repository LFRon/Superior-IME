# Fcitx5 集成：addon 与输入法条目是两件事

这是「装好了却在『添加输入法』里找不到」的根因。

Fcitx5 有两个彼此独立的注册面：

| 注册面 | 文件 | 作用 | 出现在「添加输入法」 |
|---|---|---|---|
| addon | `share/fcitx5/addon/*.conf` + `lib*.so` | 插件：引擎、前端、UI、模块 | 否 |
| 输入法条目 | `share/fcitx5/inputmethod/*.conf` | 用户可添加的输入法 | **是** |

v3 之后 Superior-IME 只提供一个 addon 和一个条目：

| 文件 | 类别 | 说明 |
|---|---|---|
| `superiorpinyin.conf` + `libsuperiorpinyin.so` | `Category=InputMethod` | 宿主 + 内核 + 拼音能力：按键翻译、文本执行、场景呈现、原生候选面板、librime |
| `super-ime.conf` | `[InputMethod]` | 唯一出现在「添加输入法」里的条目，绑定 `superiorpinyin` |

> v2 曾把观察者（`superioragent`，`Category=Module`）和引擎
> （`superiorpinyin`，`Category=InputMethod`）拆成两个 addon。候选列表因此成为
> 两者之间的隐式通道：引擎建 `fcitx::CandidateList`，观察者用
> `toModifiable()` 插行，再靠文本比对认自己的行。组合有两个 owner，是
> `CloudContext` 那批 hack 的来源。v3 合并成一个 addon，组合只属于
> `pinyin::RimeComposer`。

---

## 1. 输入法条目

`backend/platforms/fcitx5/data/super-ime.conf.in`：

```ini
[InputMethod]
Name=先！进！输入法
Name[zh_CN]=先！进！输入法
Name[zh_TW]=先！進！輸入法
Comment=Superior IME: librime (Wanxiang) with the Superior runtime and overlay
Icon=org.superior.ime
Label=进
LangCode=zh_CN
Addon=superiorpinyin
Configurable=False
```

关键点：

- **文件名即 uniqueName**：`super-ime.conf` → `super-ime`。所以
  `fcitx5-remote -s super-ime` 可用，且不会和已有的 `pinyin` 条目冲突。
- `Addon=superiorpinyin`：宿主是本仓库的 `libsuperiorpinyin.so`。**只有这一个条目**。
- `Configurable=False`：还没有独立的引擎配置页。
- `Icon=org.superior.ime` 对应 `data/icons/org.superior.ime.svg`，
  安装到 `share/icons/hicolor/scalable/apps/`。
- `Label=进` 用于托盘/状态区文字。

---

## 2. 安装

```bash
cmake --preset linux-system
cmake --build --preset linux-system --parallel
sudo cmake --install build/linux-system
```

产物（系统安装）：

```
/usr/bin/superior_ui
/usr/bin/superior_settings
/usr/lib/<multiarch>/fcitx5/libsuperiorpinyin.so
/usr/share/fcitx5/addon/superiorpinyin.conf
/usr/share/fcitx5/inputmethod/super-ime.conf
/usr/share/icons/hicolor/scalable/apps/org.superior.ime.svg
/usr/share/superior-ime/skins/
```

产物（用户安装，`scripts/install-user.sh`，推荐）：

```
~/.local/share/superior-ime/bin/superior_ui           # 真正的可执行文件
~/.local/share/superior-ime/bin/superior_settings
~/.local/share/superior-ime/skins/
~/.local/bin/superior_ui                              # 200 字节包装脚本
~/.local/bin/superior_settings
~/.local/lib/fcitx5/libsuperiorpinyin.so
~/.local/share/fcitx5/addon/superiorpinyin.conf
~/.local/share/fcitx5/inputmethod/super-ime.conf
~/.local/bin/superior-config                         # rime status/deploy/lookup
~/.local/share/icons/hicolor/scalable/apps/org.superior.ime.svg
~/.local/share/applications/superior-settings.desktop
~/.config/environment.d/90-superior-ime.conf
```

大文件不放在 `~/.local/bin`：那里在 `PATH` 上，塞一个 13 MB 的 UI 只会让日常
命令补全和备份变脏。宿主自动拉起浮层时按
`~/.local/share/superior-ime/bin` → `~/.local/bin` → `/usr/bin` → `PATH`
的顺序查找，所以两种安装方式都能自启动。

### 从 v2 升级

v2 的 `libsuperioragent.so` / `superioragent.conf` 必须清理，否则 Fcitx5 会
加载两个宿主。用户安装由 `scripts/install-user.sh` 删除；系统安装需要一次：

```bash
sudo rm -f /usr/lib/*/fcitx5/libsuperioragent.so \
           /usr/share/fcitx5/addon/superioragent.conf
```

---

## 3. 生效方式（重要）

**必须重启一次 Fcitx5**，输入法列表才会包含新条目：

```bash
fcitx5-remote -e && fcitx5 -d
```

以下做法**不生效**或**有害**：

| 命令 | 结果 |
|---|---|
| `fcitx5-remote -r` | 只重新加载全局配置，**不重扫输入法条目** |
| `Controller1.ResetIMList` | 会重扫，但**按默认值重建输入法分组**：实测会把用户自己的条目（含刚加的 `super-ime`）从分组里删掉，还会把 `keyboard-cn` 换成 `keyboard-us` |

所以：**重启，不要 ResetIMList。**

验证：

```bash
fcitx5-remote -n            # 当前输入法名
fcitx5-remote -m super-ime  # 输出 super-ime 即条目存在
fcitx5-remote -s super-ime  # 切换过去
fcitx5-remote -o            # 激活（没有 XIM 客户端时可能是 inactive）
```

也可以打开 `fcitx5-configtool`，在「输入法」页点「添加」，列表里会看到
**先！进！输入法**。

---

## 4. 为什么 `-m` 输出的是 `super-ime` 而不是 `pinyin`

上游 `Instance::addonForInputMethod()` 的实现是：

```cpp
if (const auto *entry = inputMethodManager().entry(imName)) {
    return entry->uniqueName();
}
```

也就是返回条目名而不是 addon 名。输出 `super-ime` 恰好证明条目已被识别。

---

## 5. 当前边界

- 拼音引擎、词库、用户学习、候选生成全部由 `pinyin::RimeComposer` 负责，
  方案数据在 `$XDG_DATA_HOME/superior-ime/pinyin`，与系统的 `fcitx5-rime` 隔离。
- 宿主注册观察者、接管面板、处理 Shift 轻按；它不注册输入法条目，也不持有
  候选、云、学习或模式状态。
- 这个条目目前没有独立配置页（`Configurable=False`）。
- 从旧版本升级时可能残留 `super-ime-rime.conf` / `superiorrime.conf`；
  `uninstall-user.sh` 会清理，也可以手动删除后重启 Fcitx5。
- 图标是普通 SVG；Fcitx5 直接读文件，其他桌面环境可能需要
  `gtk-update-icon-cache` 才在应用菜单里显示。
