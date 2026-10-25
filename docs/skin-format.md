# `superior.skin/1` 皮肤格式

本文档描述 Superior-IME 自有的皮肤格式。它与任何第三方输入法皮肤格式无关：不加密、不含二进制归档头、不依赖运行时代码。

- 状态：v1，随 M1 一起实现。
- 参考实现：`backend/skin`。
- 校验工具：`tools/skin-lint`。

---

## 1. 包布局

```
my-skin/
├─ skin.json          # 必需，UTF-8，唯一入口
├─ preview.png        # 可选，供皮肤管理器展示
└─ assets/            # 资源；位置不限，只要在包内
   ├─ panel-h1.svg
   └─ ...
```

规则：

- P0 只支持**目录**形式。`.skin`（zip）规划在 P2，`skin.json` 必须位于包根。
- 资源路径必须是**包内相对路径**，使用 `/`，禁止绝对路径、盘符、反斜杠和 `..`。
- 允许的扩展名：`.png` `.svg` `.webp` `.gif` `.jpg` `.jpeg` `.avif`。
- 所有被引用的资源必须存在，否则加载失败。

---

## 2. 顶层字段

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `schema` | string | 是 | 必须等于 `superior.skin/1` |
| `id` | string | 是 | 稳定标识，`[A-Za-z0-9._-]`，首字符为字母或数字 |
| `name` | string | 是 | 展示名 |
| `version` | string | 是 | `major.minor.patch`，三段整数 |
| `author` | string | 否 | 作者 |
| `license` | string | 否 | 许可证标识 |
| `description` | string | 否 | 描述 |
| `preview` | string | 否 | 预览图，必须是包内资源 |
| `modes` | string[] | 否 | 例如 `["light","dark"]` |
| `defaultMode` | string | 否 | 必须在 `modes` 中；缺省取第一个 |
| `tokens` | object | 否 | 设计变量，见 §3 |
| `panel` | object | 是 | 候选栏，见 §5 |
| `status` | object | 否 | 状态栏，见 §6 |
| `voice` | object | 否 | 语音面板，见 §7 |
| `motion` | object | 否 | 动画参数，见 §8 |

未知字段产生 `field.unknown` **warning**；`--strict` 下升级为 error。这保证旧版本能打开新皮肤。

---

## 3. 设计与 token

`tokens` 是名字到值的映射。值可以是颜色、数字、字体对象，或另一个 token 引用。

```json
"tokens": {
  "color.text": "#E8EAED",
  "font.candidate": { "family": "Noto Sans CJK SC", "size": 16, "weight": 500 },
  "radius.panel": 10,
  "panel.radius": "$radius.panel"
}
```

- 引用写法：字符串以 `$` 开头，后接 token 名。
- 引用可以链式，但**禁止循环**；循环产生 `token.cycle` error。
- 未知 token 产生 `token.unknown` error。
- token 只做值替换，不做表达式求值。

---

## 4. 基础类型

### 4.1 颜色

`#RGB`、`#RRGGBB`、`#RRGGBBAA`（alpha 在最后）。非法值产生 `color.invalid`。

### 4.2 字体

```json
{ "family": "Noto Sans CJK SC", "size": 16, "weight": 500, "italic": false }
```

`size` 为逻辑像素，范围 `[1, 512]`；`weight` 为 `[1, 1000]`。

### 4.3 边距

```json
8                       // 四边相同
[8, 12]                 // 垂直, 水平
[8, 12, 8, 12]          // 上, 右, 下, 左
{ "top": 8, "right": 12, "bottom": 8, "left": 12 }
```

范围 `[0, 4096]`。

### 4.4 图片与九宫格

```json
"assets/panel.png"
{ "image": "assets/panel.png", "slice": [24, 20, 24, 20], "fit": "stretch" }
{ "image": "assets/panel.png", "slice": { "left": 24, "top": 20, "right": 24, "bottom": 20 } }
```

- `slice` 顺序：`[left, top, right, bottom]`，单位是源图像像素。
- `fit`：`stretch`（默认）、`tile`、`contain`、`cover`。
- 未提供 `slice` 表示整图拉伸，不做九宫格。

### 4.5 画刷

```json
"#RRGGBBAA"
{ "color": "#RRGGBBAA", "radius": 8 }
{ "image": "assets/bg.png", "color": "#1E2126", "radius": 10 }
{ "image": { "image": "assets/bg.png", "slice": [12, 12, 12, 12] }, "color": "#1E2126" }
```

- 字符串等价于 `{ "color": ... }`。
- 同时给出 `color` 与 `image` 时，渲染器先填色再绘图层。
- `radius` 是逻辑像素，作用于颜色层与图片裁剪。

### 4.6 阴影

```json
{ "color": "#00000066", "blur": 16, "offset": [0, 4] }
```

`blur ∈ [0, 512]`。

---

## 5. `panel`

```json
"panel": {
  "variants": [
    { "when": { "orientation": "horizontal", "layout": "single-row" }, "use": "h1" },
    { "when": { "orientation": "vertical" }, "use": "v1" }
  ],
  "styles": {
    "h1": {
      "background": { "image": "assets/panel-h1.svg", "color": "#1E2126F2", "radius": 10 },
      "padding": [8, 12, 8, 12],
      "gap": 8,
      "preedit": { "font": "$font.preedit", "color": "$color.text" },
      "candidates": {
        "flow": "horizontal",
        "maxVisible": 7,
        "cell": { "padding": [4, 8, 4, 8], "gap": 2 },
        "text": {
          "font": "$font.candidate",
          "color": "$color.text",
          "firstColor": "$color.text.first",
          "commentColor": "$color.text.comment"
        },
        "selected": { "color": "$color.selection", "radius": 6 },
        "hover": { "color": "#FFFFFF14", "radius": 6 },
        "paging": "dots"
      },
      "separator": { "enabled": false, "color": "#FFFFFF1F", "thickness": 1 },
      "shadow": { "color": "#00000066", "blur": 16, "offset": [0, 4] }
    }
  }
}
```

### 5.1 variants

`variants` 是**有序**列表，第一个满足条件的生效；都不满足时使用 `styles` 中的第一个（按 key 排序）作为回退。

`when` 支持：

| 字段 | 取值 |
|---|---|
| `orientation` | `horizontal` / `vertical` |
| `layout` | `single-row` / `split` |

`use` 必须引用已存在的 style，否则 `variant.style.unknown`。

### 5.2 style 字段

| 字段 | 说明 |
|---|---|
| `background` | 画刷 |
| `backgroundColor` | 颜色；覆盖 `background.color` |
| `padding` | 内边距 |
| `gap` | 预编辑与候选区间距 |
| `preedit` | `{ font, color }` |
| `candidates` | 见下 |
| `separator` | `{ enabled, color, thickness }` |
| `shadow` | 阴影 |

### 5.3 candidates

| 字段 | 说明 |
|---|---|
| `flow` | `horizontal` / `vertical` |
| `maxVisible` | `[1, 32]`，每页最多显示多少项 |
| `cell.padding` | 单元格内边距 |
| `cell.gap` | 单元格间距 |
| `text.font` / `text.color` | 候选文字 |
| `text.firstColor` | 首候选颜色（可选） |
| `text.commentColor` | 注释颜色（可选） |
| `selected` | 选中态画刷 |
| `hover` | 悬停态画刷（桌面） |
| `paging` | `none` / `dots` / `arrows` |

状态集合是封闭的：`normal`、`hover`、`pressed`、`selected`、`disabled`。自定义状态名会被忽略。

---

## 6. `status`

```json
"status": {
  "background": { "image": "assets/bar.svg", "color": "#1E2126F2", "radius": 8 },
  "orientation": "horizontal",
  "padding": [4, 8, 4, 8],
  "spacing": 6,
  "items": [
    {
      "id": "ime",
      "kind": "toggle",
      "action": "toggle-input-mode",
      "size": [22, 22],
      "icons": { "cn": "assets/icon-cn.svg", "en": "assets/icon-en.svg" }
    }
  ]
}
```

- `id` 在包内唯一；重复产生 error。
- `kind`：`toggle` / `button`。
- `action` 是**语义动作名**，由前端映射到 `UiCommand`；不是窗口操作。
- `icons` 的 key 由皮肤自定义（`cn` / `en` / `full` / `half` / ...），由渲染器按当前状态选择。
- `background` 可选，用于单个按钮的底色。

---

## 7. `voice`

```json
"voice": {
  "background": { "color": "#1E2126F2", "radius": 10 },
  "mic": {
    "idle": "assets/mic-idle.svg",
    "active": "assets/mic-active.svg",
    "size": [56, 56]
  },
  "meter": { "bars": 24, "color": "#7CC4FF", "decay": 0.85 },
  "partialText": { "font": "$font.candidate", "color": "$color.text" }
}
```

`meter.bars ∈ [1, 256]`，`meter.decay ∈ [0, 1]`。

---

## 8. `motion`

```json
"motion": {
  "panel.show": { "type": "spring", "stiffness": 420, "damping": 34, "mass": 1 },
  "panel.hide": { "type": "tween", "durationMs": 120, "easing": "cubic-out" }
}
```

- `type` 只允许 `tween` / `spring`。
- `tween.durationMs ∈ [0, 10000]`；`easing` 是渲染器认识的命名曲线。
- `spring` 参数必须为正。
- 动画由 UI 在本地按单调时间采样，**不通过 IPC 逐帧传输**。

---

## 9. 诊断

每条诊断包含：

- `severity`：`info` / `warning` / `error`
- `pointer`：RFC 6901 JSON Pointer，指向出错节点
- `code`：稳定错误码
- `message`：英文说明

主要错误码：

| code | 含义 |
|---|---|
| `file.missing` / `file.read` | 包或文件不可读 |
| `json.parse` | JSON 语法错误 |
| `schema.missing` / `schema.unsupported` | schema 缺失或版本不符 |
| `field.missing` / `field.type` / `field.value` | 必填、类型或取值错误 |
| `field.unknown` | 未知字段（默认 warning） |
| `token.unknown` / `token.cycle` | token 问题 |
| `color.invalid` | 颜色格式错误 |
| `asset.path.invalid` / `asset.missing` / `asset.extension` | 资源问题 |
| `variant.style.unknown` / `panel.style.missing` | 面板引用问题 |
| `motion.type.invalid` | 动画类型错误 |

`loadSkin` 只在**没有任何 error** 时返回 `spec`。warning 不影响加载，`--strict` 除外。

---

## 10. 内容哈希

`SkinSpec::hash` 是 16 位十六进制 FNV-1a 64，输入为：

1. 规范化 JSON（未知字段已剔除、默认值已materialize、key 有序）；
2. 按路径排序的资源清单：`path`、字节数、内容 FNV-1a。

用途：

- UI 换肤时判断是否需要重新解码资源；
- 皮肤管理器去重；
- golden 测试。

FNV 不是密码学哈希，不用于签名或安全边界。

---

## 11. 兼容策略

- `schema` 主版本变化才可能破坏兼容；v1 内只做**加法**。
- 新字段必须可选，且缺省行为与旧版本一致。
- 未知字段忽略并 warning，保证旧解析器能打开新皮肤。
- 删除或改变既有字段语义必须升级 `schema`。
- 渲染器必须为缺失的可选字段提供内置默认值，不得依赖皮肤作者补齐。

---

## 12. 最小示例

```json
{
  "schema": "superior.skin/1",
  "id": "org.example.minimal",
  "name": "Minimal",
  "version": "1.0.0",
  "panel": {
    "styles": {
      "h1": {
        "background": { "color": "#1E2126", "radius": 8 },
        "padding": [8, 10, 8, 10],
        "candidates": { "flow": "horizontal", "maxVisible": 5 }
      }
    }
  }
}
```
