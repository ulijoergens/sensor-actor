# Case for a 38-pin ESP32 dev board

Two-part, support-free case for an ESP32-DevKitC-style board (38 pins, about 55 × 28 mm) with the
header pins pointing up, so Dupont wires plug in through the lid.

| File | Part | Print orientation |
|---|---|---|
| `case_base.stl` | tray with ledges under the board ends, USB opening, two screw tabs (Ø 4.2 mm) | bottom down |
| `case_lid.stl` | push-in lid: slots over both header rows, window over EN/BOOT/LED, vent slots | top down (as exported) |

Outside 60.2 × 33.4 × 14.6 mm, 78.2 mm long with the tabs. PLA or PETG, 0.2 mm layers, 3 perimeters,
no supports.

`case.py` is parametric (board size, fit, slot width, heights, tabs). It needs `manifold3d`,
`numpy` and `trimesh`; running it rewrites the STL files and checks that the board (with headers,
module and USB socket) does not intersect the case. `board_dummy.stl` and
`preview_lid_in_place.stl` are only for previews.

Check before printing: the board's length and width, the header row distance (25.4 mm here) and
whether the tallest component on the board stays below 5 mm (`ABOVE`).
