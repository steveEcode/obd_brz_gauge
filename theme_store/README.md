# Theme Store / 主题商店

This is where **packaged, ready-to-flash theme binaries** live — the output of
`tools/theme_packer/pack_theme.py`, plus the metadata an app needs to show a
theme in a picker before the user downloads it (title, description, cover
image). This is separate from [`themes/`](../themes/), which holds theme
*source* (manifest + assets + layout, before packing).

这里存放**打包好、可以直接刷机的主题二进制文件**——也就是
`tools/theme_packer/pack_theme.py` 的产出物,以及 App 在下载前展示主题选择器
所需的元信息(标题、简介、封面图)。这跟 [`themes/`](../themes/) 不是一回事——
那边放的是主题的**源文件**(manifest + 素材 + 布局,打包前的状态)。

```
theme_store/
├── catalog.json              # generated index of every theme below — the app fetches this
├── README.md                 # this file
└── <theme_id>/                # one folder per theme, named after its id
    ├── info.json              # required — title, description, author, version
    ├── preview.png            # required — 360x360 cover image shown in the picker
    └── theme.bin              # required — packed 4MB binary (pack_theme.py output)
```

## Adding a theme to the store

1. Build the theme source under `themes/<id>/` (see [`themes/README.md`](../themes/README.md)
   for the layout/manifest format), then pack it:

   ```bash
   python3 tools/theme_packer/pack_theme.py themes/<id> theme_store/<id>/theme.bin
   ```

2. Create `theme_store/<id>/info.json`:

   ```json
   {
     "id": "boost_oil_example",
     "title": "TURBO PRO",
     "description": "Boost gauge + oil pressure bar, orange/yellow scheme.",
     "author": "example",
     "version": "1.0.0"
   }
   ```

   `id` must match the theme's `theme.manifest.theme.id` and the folder name.
   `title`/`description` are what a phone app shows in a theme picker — free
   text, not compiled into firmware, can be edited without repacking `theme.bin`.

3. Add `theme_store/<id>/preview.png` — a 360x360 screenshot or rendered mockup
   of the theme's main gauge page. This is purely for the picker UI; it is
   never flashed to the device.

4. Regenerate the catalog:

   ```bash
   python3 tools/gen_theme_store.py
   ```

   This scans every `theme_store/<id>/`, validates the three required files
   are present, computes `theme.bin`'s size/sha256, and writes `catalog.json`.
   Commit the result together with your new folder.

## catalog.json

The generated index an app fetches to render "available themes" without
downloading every `theme.bin` up front:

```json
{
  "generated": "2026-08-26 22:30:00",
  "themes": [
    {
      "id": "boost_oil_example",
      "title": "TURBO PRO",
      "description": "Boost gauge + oil pressure bar, orange/yellow scheme.",
      "author": "example",
      "version": "1.0.0",
      "preview": "boost_oil_example/preview.png",
      "bin": {
        "path": "boost_oil_example/theme.bin",
        "size": 4194304,
        "sha256": "..."
      }
    }
  ]
}
```

An app flow looks like: fetch `catalog.json` → show title/description/preview
per theme → user picks one → download `bin.path` → send over BLE OTA (once
`OTA_KIND_THEME` exists — not implemented yet) or flash via USB to `theme_0`
at `0x620000` (see [`../THEME_PARTITION_SYSTEM.md`](../THEME_PARTITION_SYSTEM.md)).

## Notes

- `theme.bin` is always exactly 4MB (all-`0xFF` padded) — matches the
  `theme_0` partition size in `partitions.csv`. `pack_theme.py` enforces this.
- Nothing here is compiled into the firmware or read by `idf.py build`. This
  is pure distribution/catalog data, analogous to `firmware/release/` for
  full firmware images.
- `gen_theme_store.py` only *reads* `theme.bin`/`preview.png`/`info.json`; it
  never regenerates `theme.bin` itself — rerun `pack_theme.py` first if the
  theme source changed.
