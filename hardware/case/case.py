#!/usr/bin/env python3
"""Parametric case for a 38-pin ESP32 dev board (ESP32-DevKitC style), Dupont wires from above.

Two parts, both printable without supports:
  case_base.stl  tray with two ledges under the short ends of the board, USB opening,
                 screw tabs; print bottom down
  case_lid.stl   push-in lid with slots over both header rows (Dupont plugs pass through),
                 a window over the USB end (EN/BOOT buttons, LED) and vent slots; print
                 top down (as exported)

Coordinates: x along the board (x = 0 at the USB end of the board), y across, z up.
All sizes in mm. Change the parameters below to fit a different board, then run:
    python3 case.py            (needs manifold3d, numpy, trimesh)
"""
import os
import sys

import numpy as np
from manifold3d import Manifold, Mesh
import trimesh

# ---------------------------------------------------------------- parameters
PCB_L = 55.0          # board length incl. antenna (DevKitC V4: 54.4; clones up to 55.3)
PCB_W = 28.2          # board width (27.9 .. 28.5)
PCB_T = 1.6           # board thickness
FIT = 0.6             # clearance around the board, each side

ROW_PITCH = 25.4      # distance between the two header rows (centre to centre)
SLOT_W = 3.4          # header slot width: a row of Dupont housings (2.54 mm) + play
SLOT_END = 1.0        # slot ends this far inside the board ends (covers any header offset)

UNDER = 4.0           # space under the board (solder joints of the headers)
ABOVE = 5.0           # space above the board up to the lid (module 3.2 mm, caps, USB socket)

WALL = 2.0
FLOOR = 2.0
LID_T = 2.0
LIP_H = 3.0           # lid lip that sits inside the walls
LIP_W = 1.4
LIP_FIT = 0.2         # clearance between lip and walls (press fit)

LEDGE_D = 3.0         # how far the end ledges reach under the board
LEDGE_SIDE = 3.5      # ledges stop this far from the long edges (header solder joints)

USB_W = 16.0          # opening for a micro/USB-C plug with overmould (12 mm was too tight)
USB_BELOW = 4.5       # opening starts this far below the top of the board (room for the plug sleeve)

WIN_L = 9.0           # window over the USB end (EN/BOOT, LED), from the board end
WIN_W = 18.0          # window width (between the header slots)

MODULE_L = 18.0       # WROOM module (incl. antenna) length and width, for the lip cut-out
MODULE_W = 18.0

VENT_W = 1.6
VENT_LEN = 12.0
VENT_N = 5

TAB_L = 9.0           # screw tabs at both short ends
TAB_W = 14.0
TAB_T = 3.0
SCREW_D = 4.2

SEGMENTS = 48

# ---------------------------------------------------------------- derived
IN_L = PCB_L + 2 * FIT
IN_W = PCB_W + 2 * FIT
OUT_L = IN_L + 2 * WALL
OUT_W = IN_W + 2 * WALL
X0 = -FIT                                  # inner cavity starts here (board at x = 0)
Z_LEDGE = FLOOR + UNDER                    # board rests here
Z_PCB_TOP = Z_LEDGE + PCB_T
Z_TOP = Z_PCB_TOP + ABOVE                  # top of the walls = underside of the lid plate


def box(x0, y0, z0, x1, y1, z1):
    return Manifold.cube([x1 - x0, y1 - y0, z1 - z0]).translate([x0, y0, z0])


def cyl(x, y, z0, z1, d):
    return Manifold.cylinder(z1 - z0, d / 2, d / 2, SEGMENTS).translate([x, y, z0])


def rounded_box(x0, y0, z0, x1, y1, z1, r):
    """Box with rounded vertical edges."""
    core = box(x0 + r, y0, z0, x1 - r, y1, z1) + box(x0, y0 + r, z0, x1, y1 - r, z1)
    for cx in (x0 + r, x1 - r):
        for cy in (y0 + r, y1 - r):
            core += cyl(cx, cy, z0, z1, 2 * r)
    return core


# ---------------------------------------------------------------- base
def make_base():
    ox0, ox1 = X0 - WALL, X0 + IN_L + WALL
    oy = OUT_W / 2
    shell = rounded_box(ox0, -oy, 0, ox1, oy, Z_TOP, 2.0)
    cavity = box(X0, -IN_W / 2, FLOOR, X0 + IN_L, IN_W / 2, Z_TOP + 1)
    base = shell - cavity

    # ledges under both short ends of the board
    yl = IN_W / 2 - LEDGE_SIDE
    base += box(X0, -yl, FLOOR - 0.01, LEDGE_D, yl, Z_LEDGE)
    base += box(PCB_L - LEDGE_D, -yl, FLOOR - 0.01, X0 + IN_L, yl, Z_LEDGE)

    # USB opening in the end wall at x < 0, open to the top (no supports needed)
    base -= box(ox0 - 1, -USB_W / 2, Z_PCB_TOP - USB_BELOW, X0 + 0.01, USB_W / 2, Z_TOP + 1)

    # screw tabs
    for sign, xe in ((-1, ox0), (1, ox1)):
        x_far = xe + sign * TAB_L
        tab = rounded_box(min(xe - sign * 1.0, x_far), -TAB_W / 2, 0, max(xe - sign * 1.0, x_far), TAB_W / 2, TAB_T, 3.0)
        tab -= cyl(xe + sign * TAB_L / 2, 0, -1, TAB_T + 1, SCREW_D)
        base += tab
    # the screw tab at the USB end must not block the USB opening
    base -= box(ox0 - TAB_L - 1, -USB_W / 2, Z_PCB_TOP - USB_BELOW, X0 + 0.01, USB_W / 2, Z_TOP + 1)

    # a small notch on one long side to lever the lid out
    base -= box(OUT_L / 2 + X0 - WALL - 5, oy - WALL - 0.01, Z_TOP - 1.2, OUT_L / 2 + X0 - WALL + 5, oy + 1, Z_TOP + 1)
    return base


# ---------------------------------------------------------------- lid (modelled in place)
def make_lid():
    ox0, ox1 = X0 - WALL, X0 + IN_L + WALL
    oy = OUT_W / 2
    plate = rounded_box(ox0, -oy, Z_TOP, ox1, oy, Z_TOP + LID_T, 2.0)
    lip_o = box(X0 + LIP_FIT, -IN_W / 2 + LIP_FIT, Z_TOP - LIP_H, X0 + IN_L - LIP_FIT, IN_W / 2 - LIP_FIT, Z_TOP + 0.01)
    lip_i = box(X0 + LIP_FIT + LIP_W, -IN_W / 2 + LIP_FIT + LIP_W, Z_TOP - LIP_H - 1,
                X0 + IN_L - LIP_FIT - LIP_W, IN_W / 2 - LIP_FIT - LIP_W, Z_TOP + 0.02)
    lid = plate + (lip_o - lip_i)

    # header slots (cut through plate and lip)
    for ys in (-ROW_PITCH / 2, ROW_PITCH / 2):
        lid -= box(SLOT_END, ys - SLOT_W / 2, Z_TOP - LIP_H - 1, PCB_L - SLOT_END, ys + SLOT_W / 2, Z_TOP + LID_T + 1)
    # window over the USB end (buttons, LED); also opens the lip there
    lid -= box(X0 - 0.01, -WIN_W / 2, Z_TOP - LIP_H - 1, WIN_L, WIN_W / 2, Z_TOP + LID_T + 1)
    # USB plug clearance through the lid edge, matching the base opening
    lid -= box(ox0 - 1, -USB_W / 2, Z_TOP - LIP_H - 1, X0 + 0.01, USB_W / 2, Z_TOP + LID_T + 1)
    # no lip over the WROOM module at the antenna end (the module ends flush with the board)
    lid -= box(PCB_L - MODULE_L, -MODULE_W / 2 - 1, Z_TOP - LIP_H - 1, X0 + IN_L + 1, MODULE_W / 2 + 1, Z_TOP + 0.001)
    # vent slots across the middle, between the header slots
    span_x0, span_x1 = WIN_L + 6, PCB_L - 8
    for i in range(VENT_N):
        x = span_x0 + (span_x1 - span_x0) * (i + 0.5) / VENT_N
        lid -= box(x - VENT_W / 2, -VENT_LEN / 2, Z_TOP - 1, x + VENT_W / 2, VENT_LEN / 2, Z_TOP + LID_T + 1)
    return lid


def board_dummy():
    """The PCB and the two header rows, for the assembly preview only."""
    b = box(0, -PCB_W / 2, Z_LEDGE, PCB_L, PCB_W / 2, Z_PCB_TOP)
    for ys in (-ROW_PITCH / 2, ROW_PITCH / 2):
        b += box(3.0, ys - 1.27, Z_PCB_TOP, 3.0 + 19 * 2.54, ys + 1.27, Z_PCB_TOP + 2.5)   # header plastic
    b += box(PCB_L - 18, -9, Z_PCB_TOP, PCB_L - 0.5, 9, Z_PCB_TOP + 3.2)                 # WROOM module
    b += box(-1.0, -4, Z_PCB_TOP, 5, 4, Z_PCB_TOP + 3.0)                                  # USB socket
    return b


def to_trimesh(m):
    mesh = m.to_mesh()
    return trimesh.Trimesh(vertices=np.asarray(mesh.vert_properties)[:, :3], faces=np.asarray(mesh.tri_verts))


def main():
    out = os.path.dirname(os.path.abspath(__file__))
    base = make_base()
    lid = make_lid()

    # overlap check: the board (with headers etc.) must not intersect the case
    board = board_dummy()
    clash_base = (board ^ base).volume()
    clash_lid = (board ^ lid).volume()

    # lid for printing: flipped, top face down on the bed
    lid_print = lid.rotate([180, 0, 0]).translate([0, 0, Z_TOP + LID_T])

    for name, m in (("case_base", base), ("case_lid", lid_print)):
        t = to_trimesh(m)
        t.export(os.path.join(out, name + ".stl"))
        ext = t.bounds[1] - t.bounds[0]
        print(f"{name}.stl: {ext[0]:.1f} x {ext[1]:.1f} x {ext[2]:.1f} mm, volume {t.volume / 1000:.1f} cm3, "
              f"watertight {t.is_watertight}, {len(t.faces)} triangles")
    to_trimesh(lid).export(os.path.join(out, "preview_lid_in_place.stl"))   # for the 3D preview
    to_trimesh(board).export(os.path.join(out, "board_dummy.stl"))
    print(f"assembled outside: {OUT_L:.1f} x {OUT_W:.1f} x {Z_TOP + LID_T:.1f} mm (+ tabs {TAB_L:.0f} mm each end)")
    print(f"board clash with base: {clash_base:.2f} mm3, with lid: {clash_lid:.2f} mm3 (must be 0)")
    print(f"header pins (8.5 mm above the board) stick out {Z_PCB_TOP + 8.5 - (Z_TOP + LID_T):.1f} mm above the lid")
    return 0 if clash_base < 1e-6 and clash_lid < 1e-6 else 1


if __name__ == "__main__":
    sys.exit(main())
