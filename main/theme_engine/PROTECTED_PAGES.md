# Theme System Design Rules

## Protected Boot Pages

The following pages are **NEVER themed** and always use core firmware implementation:

1. **`ui_ScreenPageLogo`** (page_id: `"logo"`)
   - Sky Gauge logo screen
   - Shown immediately on boot
   - Cannot be replaced by themes

2. **`ui_ScreenPageIntro`** (page_id: `"intro"`)
   - Multi-gauge boot animation (RACE AS ONE)
   - Controlled by `nvs_intro_enable_get()` (0=OFF, 1=RACE, 2=VIDEO)
   - Cannot be replaced by themes

3. **Boot Video** (page_id: `"boot_video"`)
   - Custom boot video from boot_block partition
   - Played via `boot_block_player.c`
   - Cannot be replaced by themes

## Why These Pages Are Protected

1. **Branding Consistency**: Sky Gauge logo must be shown on every device
2. **Boot Flow Integrity**: Ensures predictable startup sequence
3. **User Expectation**: Users expect to see familiar boot screen
4. **Safety**: Boot animations don't depend on potentially corrupted theme data

## Theme Engine Behavior

When `theme_create_page()` is called with a protected page ID:
- Returns `NULL`
- Logs: "Page 'logo' is a protected boot page, using system implementation"
- Caller must use original `ui_ScreenPageLogo_screen_init()` etc.

## Theme Manifest Requirements

All themes **MUST** list boot pages in `system_pages`:

```json
{
  "pages": {
    "system_pages": [
      "logo",       // ← REQUIRED
      "intro",      // ← REQUIRED
      "boot_video", // ← REQUIRED
      "settings",
      "ota",
      ...
    ],
    "theme_pages": [...]
  }
}
```

If a theme attempts to add boot pages to `theme_pages`, they will be ignored.

## Implementation Details

### theme_loader.c
```c
lv_obj_t* theme_create_page(const char *page_id) {
    // CRITICAL: Boot pages hardcoded whitelist
    if (strcmp(page_id, "logo") == 0 ||
        strcmp(page_id, "intro") == 0 ||
        strcmp(page_id, "boot_video") == 0) {
        return NULL;  // Force caller to use system implementation
    }
    // ... rest of page routing
}
```

### ui.c Integration (Phase 2)
```c
void ui_init(void) {
    theme_engine_init();
    
    // Boot pages ALWAYS use system implementation
    ui_ScreenPageLogo_screen_init();     // Sky Gauge logo
    ui_ScreenPageIntro = NULL;           // Lazy-loaded when needed
    
    // Other pages route through theme engine
    lv_obj_t *page_rpm = theme_create_page("page_rpm");
    if (!page_rpm) {
        page_rpm = ui_ScreenPageRpm_screen_init();  // Fallback
    }
    
    // Always start with logo
    lv_disp_load_scr(ui_ScreenPageLogo);
}
```

## Testing Checklist

When testing themes, verify:
- [ ] Device boots to Sky Gauge logo (not theme logo)
- [ ] RACE AS ONE animation works (if enabled in NVS)
- [ ] Boot video plays (if boot_mode = 2)
- [ ] Theme pages load AFTER boot sequence completes
- [ ] Switching themes does NOT change boot screen

## Community Theme Guidelines

When creating themes:
1. Do NOT attempt to theme boot pages
2. Always include `logo/intro/boot_video` in `system_pages`
3. Document that boot screen is unchanged
4. Test with boot animation enabled (`nvs_intro_enable = 1`)

## Future Considerations

If we ever allow custom boot screens:
1. Require explicit user opt-in (not default)
2. Keep Sky Gauge logo as fallback
3. Validate boot assets before applying
4. Add recovery mechanism if boot theme fails
