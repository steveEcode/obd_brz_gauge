# Custom Themes / 自定义主题

Create a UI theme for the gauge **without writing any C code** — a folder, a
manifest, one registry line.
不用写任何 C 代码就能给仪表做一套 UI 主题 —— 一个文件夹、一份清单、一行登记。

```
themes/
├── registry.txt           # slot -> id, APPEND ONLY / 槽位表，只能追加
├── _TEMPLATE/             # copy this / 复制这个
│   └── theme.toml
├── builtin/               # themes shipped with the firmware / 固件自带主题
│   ├── default/
│   ├── amber/
│   └── ocean/
└── community/             # ← your theme goes here / ← 你的主题放这里
```

The build turns these manifests into `main/export_path/ui_theme_generated.c`
via [`tools/gen_themes.py`](../tools/gen_themes.py). **Never edit that file.**
构建时由 [`tools/gen_themes.py`](../tools/gen_themes.py) 把这些清单生成成
`main/export_path/ui_theme_generated.c`。**不要手改那个文件。**

Architecture reference (English): [`../docs/THEMING.md`](../docs/THEMING.md)

---

## 中文

### 现在的主题能改什么

**只换皮，不换布局。** 颜色和形状可以改；元素位置、字号、页面排布不在主题范围内
（那是 20 多个 `ui_ScreenPageXxx.c` 各自的事）。字体同理 —— 换字体等于换字符尺寸，
等于换布局，所以刻意不做。

主题能改两样东西：

1. **8 个装饰色角色**（必填）
2. **3 类美术素材**（可选）：表框、指针、表盘背景 —— 见下面的[素材](#美术素材可选)一节

一个主题提供 8 个**装饰色角色**：

| 角色 | 用在哪 |
|------|--------|
| `bg` | 屏幕/页面背景 |
| `ring` | 外圈表框 |
| `arc_track` | 仪表弧线底轨、滑块凹槽、小刻度（"还没走到"的部分） |
| `arc_indicator` | 仪表弧线进度、滑块填充和滑块头（"已走到"的部分） |
| `text_primary` | 数值、主文字 |
| `text_secondary` | 提示文字、单位、小标签 |
| `needle` | 指针页的指针 |
| `panel` | 面板/列表/滚轮背景 |

### 主题不能改什么

**语义色**全局固定，任何主题都不许动（定义在 `ui_theme.h`）：

| 宏 | 值 | 含义 |
|----|----|------|
| `UI_SEM_ALERT` | `0xFF4D4D` | 报警/超阈值文字 |
| `UI_SEM_FLASH` | `0xFF0000` | 转速报警全屏闪烁背景 |
| `UI_SEM_ON` | `0x06D6A0` | 开关 ON / 正常状态 |
| `UI_SEM_WARN` | `0xFFD166` | 注意（机油压力等） |

这是装在车上的仪表，红色必须永远意味着"出事了"。换了皮肤，警告还得一眼认出来。

### 做一个主题

**第 1 步 — 复制模板**

```bash
cp -r themes/_TEMPLATE themes/community/sunset
```

**第 2 步 — 填 `theme.toml`**

```toml
id   = "sunset"        # 必须和文件夹名一致
name = "SUNSET"        # 设置页显示名，1~10 个大写字符

author      = "your-github-handle"
description = "Warm orange face with a deep purple ring."

[colors]
bg             = 0x140800
ring           = 0x6A2C8A
arc_track      = 0x3A1810
arc_indicator  = 0xFF7A18
text_primary   = 0xFFE0C0
text_secondary = 0x9A6440
needle         = 0xFF3020
panel          = 0x2A1408

[bezel]
style_id = 0
```

**第 3 步 — 登记到 `registry.txt`**

在**末尾追加**一行（不要插队，理由见下）：

```
3  sunset
```

**第 4 步 — 编译**

```bash
idf.py build flash monitor
```

开机进 **设置 → THEME**，滚轮里就有了。选中后设备**自动重启**生效 —— 这是正常的，
所有页面只在开机创建一次，重启是最省事也最可靠的换肤方式。

不确定的话可以先单独跑一下生成器，它会把问题指到具体行号：

```bash
python3 tools/gen_themes.py            # 重新生成
python3 tools/gen_themes.py --check    # 只校验，不写文件（适合 CI）
```

### ⚠️ 为什么 `registry.txt` 只能追加

NVS 里存的是**槽位号**（`theme_cfg.theme`），不是主题名。所以一旦调换顺序：

> 排在后面的主题槽位集体位移 → **所有已有设备在 OTA 之后被静默换成另一个主题**。

而且这个问题本地测试完全发现不了，只有用户升级后才炸。所以：

- **只能在末尾追加**，不能改号、调序、删行
- **槽位 0 必须是 `default`**
- 想废弃某个主题，把它的行和文件夹**原样留着**就行；只删文件夹会让构建失败

生成器会在构建时强制检查以上全部规则，违反了直接编译失败并告诉你怎么改。

### 配色建议

- `arc_indicator` 要明显亮于 `arc_track`，否则看不出走了多少（同色的话生成器会警告）
- `text_secondary` 要比 `text_primary` 暗，但 360×360 圆屏开车瞄一眼还得看得清
- `bg` 一般用很深的颜色，这块屏亮色背景在夜里很晃眼

### 美术素材（可选）

把 PNG 放进主题的 `assets/`，在 `theme.toml` 里声明，代码画的形状就会被你的图替换。
每一项独立 —— 有什么写什么，没声明的继续用代码画 + 对应的颜色角色。

```toml
[assets]
ring   = "assets/ring.png"
needle = "assets/needle.png"
needle_pivot_x = 24      # 指针图的旋转中心，从左上角算起
needle_pivot_y = 12
dial   = "assets/dial.png"
```

| 素材 | 尺寸要求 | 透明通道 | 编译后大小 | 替换掉什么 |
|------|----------|----------|-----------|-----------|
| `ring` | **正好 360×360** | 需要（中间镂空） | 380 KB | 外圈表框（原来是画的圆环边框） |
| `needle` | ≤ 360×360 | 需要 | 宽×高×3 B | 指针页的指针（原来是画的线） |
| `dial` | **正好 360×360** | 忽略 | 253 KB | 所有页面的背景（原来是纯 `bg` 色） |

[`_TEMPLATE/assets/`](_TEMPLATE/assets/) 里有三张**能直接用的参考素材**，尺寸和朝向都是对的，
复制过去打开看一眼就明白，然后换成你自己的。

**⚠️ 指针必须画成朝右（东）。** LVGL 绕 pivot 旋转图片，角度 0 = 原样绘制
（见 `lv_meter.c` 的 `LV_METER_INDICATOR_TYPE_NEEDLE_IMG`）。画成朝上的话整体偏 90°。
`needle_pivot_x/y` 是旋转中心在图里的像素坐标，也就是指针根部那个轴心。

**⚠️ 空间是硬约束。** 素材编译进固件，而且**所有已注册主题**的素材都会被链接进去，
不只是当前激活的那个（因为 `g_ui_themes[]` 引用了全部，`--gc-sections` 丢不掉）。
一整套约 **645 KB**。总预算 1536 KB，超了直接构建失败并列出每个文件的占用。
app 分区 4 MB，固件本身约 2 MB。

素材转换要装 Pillow（`pip install Pillow`），但**只在图片变动时**才需要 —— 转换出来的
C 文件是入库的，编译没改过的主题完全不需要 Pillow。生成器靠记录源图的 SHA-256 判断
是否要重新转换。

生成的 C 文件在 `main/export_path/theme_assets/`，**不要手改**，删主题时会自动清理。

### 上传/分享

把整个文件夹放进 [`community/`](community/)，有预览图就放一张同目录的 `preview.png`
（直接拍屏幕照片就行）。PR 标题写 `theme: add SUNSET`。

### 以后会支持的

`ui_theme_t` 里已经预留了字段，目前是 `NULL`，写主题时不用管：

```c
const lv_img_dsc_t *dial_face;   // 表盘背景图
const lv_font_t    *font;        // 字体替换
ui_bezel_style_t    bezel;       // 表框样式变体
```

新能力一律**往结构体末尾加字段**。因为用的是指定初始化器，没写到的字段自动补零，
所以已有主题不用改也能继续编译。加素材时 `theme.toml` 会扩展出对应的段，
清单格式本身不会破坏兼容。

---

## English

### What a theme controls today

**Skin only, not layout.** Colors and shapes are themeable; element positions,
font sizes and page composition are not (those live in the ~20
`ui_ScreenPageXxx.c` files). Fonts are deliberately excluded for the same
reason — swapping a font changes glyph metrics, which changes layout.

A theme controls two things:

1. **Eight decorative color roles** (required)
2. **Three kinds of artwork** (optional): bezel, needle, dial face — see
   [Artwork](#artwork-optional) below

The eight **decorative color roles**:

| Role | Used for |
|------|----------|
| `bg` | Screen / page background |
| `ring` | Outer bezel ring |
| `arc_track` | Arc track, slider groove, minor ticks (the "not reached" part) |
| `arc_indicator` | Arc progress, slider fill and knob (the "reached" part) |
| `text_primary` | Values / main text |
| `text_secondary` | Hints / units / small labels |
| `needle` | Meter needle |
| `panel` | Panel / list / roller background |

### What a theme must NOT control

**Semantic colors** are global and off-limits (defined in `ui_theme.h`):

| Macro | Value | Meaning |
|-------|-------|---------|
| `UI_SEM_ALERT` | `0xFF4D4D` | Alarm / over-threshold text |
| `UI_SEM_FLASH` | `0xFF0000` | Full-screen RPM warning flash |
| `UI_SEM_ON` | `0x06D6A0` | Toggle ON / positive state |
| `UI_SEM_WARN` | `0xFFD166` | Caution (oil pressure, etc.) |

This is a gauge bolted into a car. Red has to mean "something is wrong" in every
skin, at a glance.

### Build a theme

**Step 1 — copy the template**

```bash
cp -r themes/_TEMPLATE themes/community/sunset
```

**Step 2 — fill in `theme.toml`**

```toml
id   = "sunset"        # must match the folder name
name = "SUNSET"        # Settings roller label, 1-10 uppercase chars

author      = "your-github-handle"
description = "Warm orange face with a deep purple ring."

[colors]
bg             = 0x140800
ring           = 0x6A2C8A
arc_track      = 0x3A1810
arc_indicator  = 0xFF7A18
text_primary   = 0xFFE0C0
text_secondary = 0x9A6440
needle         = 0xFF3020
panel          = 0x2A1408

[bezel]
style_id = 0
```

**Step 3 — register it in `registry.txt`**

**Append** one line at the end (never insert — see below):

```
3  sunset
```

**Step 4 — build**

```bash
idf.py build flash monitor
```

Go to **Settings → THEME**; your theme is in the roller. Selecting it **reboots
the device** — that is expected. Screens are created once at boot, so a restart
is the simplest reliable way to re-skin all of them.

Run the generator directly if you want fast feedback — it reports the exact
file and line:

```bash
python3 tools/gen_themes.py            # regenerate
python3 tools/gen_themes.py --check    # validate only, writes nothing (CI)
```

### ⚠️ Why `registry.txt` is append-only

NVS stores the **slot number** (`theme_cfg.theme`), not the theme name. Reorder
the file and:

> every later theme shifts by one → **every existing device silently switches to
> a different theme after an OTA update.**

Local testing will never catch it; it only surfaces on users' devices. So:

- **Append only** — never renumber, reorder, or delete a line
- **Slot 0 must stay `default`**
- To retire a theme, leave both its line and its folder in place; deleting only
  the folder fails the build

The generator enforces all of this at build time and tells you how to fix it.

### Palette guidance

- Keep `arc_indicator` clearly brighter than `arc_track`, or progress is
  unreadable (the generator warns if they are identical)
- `text_secondary` should be dimmer than `text_primary` but still legible at a
  glance on a 360×360 round display while driving
- Keep `bg` very dark — a bright background on this panel is harsh at night

### Artwork (optional)

Drop PNGs into the theme's `assets/` and declare them in `theme.toml` to replace
the drawn shapes. Each key is independent — declare what you have; anything
omitted keeps the drawn version and its color role.

```toml
[assets]
ring   = "assets/ring.png"
needle = "assets/needle.png"
needle_pivot_x = 24      # rotation center in the needle art, from top-left
needle_pivot_y = 12
dial   = "assets/dial.png"
```

| Asset | Size | Alpha | Compiled size | Replaces |
|-------|------|-------|---------------|----------|
| `ring` | **exactly 360×360** | required (transparent centre) | 380 KB | Bezel ring (was a drawn circle border) |
| `needle` | ≤ 360×360 | required | w×h×3 B | Needle-page needle (was a drawn line) |
| `dial` | **exactly 360×360** | ignored | 253 KB | Page background (was flat `bg`) |

[`_TEMPLATE/assets/`](_TEMPLATE/assets/) ships three **working reference files**
with the right sizes and orientation — copy them, look at them, then replace
them with your own.

**⚠️ Needle artwork must point RIGHT (east).** LVGL rotates the bitmap around
the pivot and angle 0 draws it unrotated (see `LV_METER_INDICATOR_TYPE_NEEDLE_IMG`
in `lv_meter.c`). Art drawn pointing up will be 90° off. `needle_pivot_x/y` is
the rotation center in artwork pixels — the hub at the base of the needle.

**⚠️ Space is a hard constraint.** Artwork is compiled into the firmware, and
**every registered theme's** artwork is linked in, not just the active one
(`g_ui_themes[]` references them all, so `--gc-sections` cannot drop them). A
full set is about **645 KB**. The budget is 1536 KB; going over fails the build
with a per-file breakdown. The app partition is 4 MB and the firmware is ~2 MB.

Converting artwork needs Pillow (`pip install Pillow`), but **only when a PNG
changes** — the converted C is checked in, so building unmodified themes never
needs it. The generator records each source's SHA-256 to decide.

Generated C lands in `main/export_path/theme_assets/`. **Do not edit it**; it is
cleaned up automatically when a theme is removed.

### Uploading / sharing

Drop the whole folder into [`community/`](community/), optionally with a
`preview.png` beside the manifest (a photo of the screen is fine). Open a PR
titled `theme: add SUNSET`.

### Reserved for later

`ui_theme_t` already carries fields that are `NULL` today. Ignore them for now:

```c
const lv_img_dsc_t *dial_face;   // dial-face background image
const lv_font_t    *font;        // font override
ui_bezel_style_t    bezel;       // bezel style variants
```

Capabilities are added by **appending fields at the end** of the struct. Because
themes use designated initializers, omitted fields are zero-filled and existing
themes keep compiling untouched. When assets land, `theme.toml` grows a new
section — the manifest format itself stays backward compatible.

---

## Manifest format notes

`theme.toml` is parsed by a small built-in parser, not a full TOML library, so
the generator runs on the Python that ESP-IDF ships (3.8+, where `tomllib` does
not exist). The supported subset is what the template uses:

- `# comments`, full-line or trailing
- `[section]` headers
- `key = value` with quoted strings, decimal ints, and `0x` hex ints

Anything outside that subset is a build error with a file:line pointer.

`theme.toml` 由一个内置的小解析器读取，不是完整的 TOML 库 —— 这样生成器在
ESP-IDF 自带的 Python（3.8+，没有 `tomllib`）上也能跑。支持的子集就是模板里
用到的那些：注释、`[段落]`、`key = value`（字符串/十进制/0x 十六进制）。
超出这个子集会直接构建失败，并指到具体文件和行号。
