import json
from pathlib import Path

src = Path(__file__).resolve().parents[1]
json_path = src.parent / "pokefirered" / "src" / "data" / "region_map" / "region_map_sections.json"
out_path = src / "include" / "constants" / "region_map_sections.h"

sections = json.load(open(json_path))["map_sections"]
lines = [
    "#ifndef GUARD_CONSTANTS_REGION_MAP_SECTIONS_H",
    "#define GUARD_CONSTANTS_REGION_MAP_SECTIONS_H",
    "",
    "enum {",
]
for s in sections:
    lines.append(f"    {s['id']},")
lines += [
    "    MAPSEC_NONE,",
    "    MAPSEC_COUNT",
    "};",
    "",
    "#define KANTO_MAPSEC_START   MAPSEC_PALLET_TOWN",
    "#define SEVII_MAPSEC_START   MAPSEC_ONE_ISLAND",
    "",
    "#endif // GUARD_CONSTANTS_REGION_MAP_SECTIONS_H",
    "",
]
out_path.write_text("\n".join(lines), encoding="utf-8")
print("Wrote", out_path)
