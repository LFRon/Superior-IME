> 状态：**可选**。默认路径是自绘输入窗（见 [input-panel.md](input-panel.md)），
> 本页仅用于“希望 Fcitx5 自己画候选窗、但用 Superior 配色”的场景。

# 把 Superior 皮肤导出为 Fcitx5 classicui 主题

## 为什么需要

Wayland 下第三方 UI 拿不到 `input-popup-surface` 的公共接口（`getInputPopupSurface`
未导出），所以 **Wayland 应用的候选窗必须由 Fcitx5 的 classicui 绘制**。

让候选窗长得像 Superior 的受支持做法：把 `superior.skin/1` 投影成 classicui 主题。
这不会改变输入路径，只是换了一套 Fcitx5 自己的绘制素材。

## 用法

```bash
# 导出到用户主题目录
./build/linux-debug/tools/skin-lint/skin_lint skins/default --strict \
    --export-theme ~/.local/share/fcitx5/themes/org.superior.default

# 导出到系统主题目录（开机可用）
sudo ./build/linux-system/tools/skin-lint/skin_lint skins/default --strict \
    --export-theme /usr/share/fcitx5/themes/org.superior.default
```

然后在 Fcitx5 配置里选择该主题（附加组件 → 经典用户界面 → 主题），或写配置：

```ini
# ~/.config/fcitx5/conf/classicui.conf
Theme=org.superior.default
```

## 产物

```
<theme-dir>/
├─ theme.conf
├─ panel.svg        # 候选栏/菜单背景（九宫格）
├─ highlight.svg    # 选中项
├─ next.svg
└─ prev.svg
```

classicui 的 `loadImage()` 支持 `svg` / `svgz` / `png` / `bmp` / `xpm`，
所以这里直接生成 SVG，**不需要 Qt、cairo 或任何光栅化依赖**。

## 映射规则

| superior.skin | classicui |
|---|---|
| `panel.styles.<id>.background.color` | `[InputPanel/Background] Color` |
| `background.radius` | 九宫格 margin + SVG 圆角 |
| `panel.styles.<id>.padding` | `[InputPanel/ContentMargin]` |
| `candidates.cell.padding` | `[InputPanel/TextMargin]` |
| `candidates.text.color` | `NormalColor` |
| `candidates.selected.color` | `[InputPanel/Highlight] Color` |
| `candidates.text.firstColor` | 暂未映射（classicui 无此键） |
| `separator` | `BorderColor` / `BorderWidth` |
| `panel.styles.<id>.shadow` | 暂未映射（classicui 用 `ShadowMargin`，需额外素材） |

高亮文字颜色按选中背景亮度自动取黑/白。

## 限制

- `--theme-style` 默认取 `h1`，没有则 `v1`，再没有则第一个样式。
- classicui 只有一个候选栏外观，**不会**随横/竖排切换不同背景；
  这是 classicui 的能力边界，不是导出的缺陷。
- 字体不在主题里设置，需要在 classicui 配置里单独指定。
- 导出不会安装主题，也不会修改 `classicui.conf`。

## 代码

| 文件 | 内容 |
|---|---|
| `backend/platforms/fcitx5/include/superior/fcitx5/ClassicUiTheme.hpp` | `exportClassicUiTheme()` |
| `backend/platforms/fcitx5/src/ClassicUiTheme.cpp` | SVG 生成、主题配置写入 |
| `tools/skin-lint/main.cpp` | `--export-theme` / `--theme-style` / `--theme-overwrite` |

测试：`tests/theme_tests.cpp`（27 项断言，覆盖产物、覆盖保护、样式回退、空皮肤）。
