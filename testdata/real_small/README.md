# testdata/real_small

Мелкие JSON, как в реальной работе движка: BMFont/atlas, UI/config, короткий объект, массив записей.
Не копии 5 MB дампов. Engine (`base_craft_core`) не меняли.

| file | bytes | source | what |
|------|------:|--------|------|
| `min_object.json` | 9 | `base_craft_core/debug/min_object.json` | debug lookup `{"a":1}` |
| `nested_object.json` | 15 | `base_craft_core/debug/nested_object.json` | nested `{"o":{"x":1}}` |
| `array_obj.json` | 29 | `base_craft_core/debug/array_obj.json` | one BMFont `chars` record |
| `tileset_mapping.json` | 5290 | `base_craft_core/assets/textures/tileset_mapping.json` | tile atlas config (Cyrillic strings) |
| `tileset_coordinates.json` | 10368 | `base_craft_core/assets/textures/tileset_coordinates_full.json` | tile categories / helpers |
| `atlas_tiny.json` | 853 | cut of `assets/fonts/atlas.json` (info/common/pages + 3 glyphs) | BMFont-like; full atlas stays in `testdata/engine/` (~33 KiB) |
| `ui_config.json` | 225 | style of engine UI/font path (`assets/ui_atlas.json`) | nested short keys, ints, bool, strings |
| `empty_obj.json` | 3 | compact `{}` | speed fixture (RFC `valid/empty_object.json` already exists) |
| `empty_arr.json` | 3 | compact `[]` | speed fixture |
| `nums_arr.json` | 8 | `[1,2,3]` | tiny numeric array |
| `ok_true.json` | 12 | `{"ok":true}` | one-key object |
| `records_32.json` | 2563 | 32 records in `large_50k` shape | tiny array-of-objects |

Runtime parse in engine is `ParseJSONFile` of `assets/ui_atlas.json` / `assets/fonts/atlas.json` (`font_manager.asm`). Debug JSON is the same shape as those lookups. Tileset JSON is asset metadata, not a binary tileset.
