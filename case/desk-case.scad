// Desk MITM enclosure — parametric OpenSCAD
// Board: 132.5 x 48 perfboard, ESP32 USB out one end wall, two RJ45 jacks
// facing UP through the lid at the other end, solder pins protruding below.
//
// Coordinates: X = board length (USB end at x=0, RJ45 end at x=max),
//              Y = board width, Z = up. All openings are parametric — print,
//              test-fit, nudge the offending variable, reprint the one part.
//
// Render:  openscad -D part=\"base\" -o base.stl desk-case.scad
//          openscad -D part=\"lid\"  -o lid.stl  desk-case.scad
// Preview: open in OpenSCAD GUI, set `part` below to "base", "lid" or "both".

part = "both";  // "base" | "lid" | "both" (preview)

/* ---------- board ---------- */
board_l      = 132.5;  // perfboard length
board_w      = 48.0;   // perfboard width
board_th     = 1.6;    // perfboard thickness
pins_below   = 5.0;    // clearance under board for solder pins/wires
above_board  = 24.0;   // tallest thing above board top (29 total - ~5 below)

/* ---------- case ---------- */
wall         = 2.0;    // wall thickness
floor_th     = 2.0;
lid_th       = 2.0;
clearance    = 0.8;    // gap around the board (FDM slop; increase if tight)
headroom     = 1.5;    // extra air above the tallest component
lip_h        = 4.0;    // lid skirt depth

/* ---------- USB opening (end wall at x=0) ---------- */
usb_w        = 13.0;   // opening width
usb_h        = 9.0;    // opening height
usb_y_off    = 0.0;    // sideways offset from board centerline ("fairly centered, but not 100%")
usb_z        = 0.0;    // opening bottom relative to board TOP surface (USB sits just above board)

/* ---------- RJ45 lid openings (jacks face up, near x=max end) ---------- */
// Positions are centers. x measured from the INNER face of the RJ45-end wall
// (i.e. from the far edge of the board), y from the board centerline.
rj45_hole_w  = 17.5;   // hole size along X (jack depth + slop)
rj45_hole_d  = 17.5;   // hole size along Y (jack width + slop)
rj45_a_x     = 12.0;   // jack A center, distance back from board far edge
rj45_a_y     = -11.5;  // jack A center, offset from centerline (negative = one side)
rj45_b_x     = 12.0;   // jack B center
rj45_b_y     = 11.5;   // jack B center

/* ---------- board mounting ---------- */
ledge_w      = 2.5;    // perimeter shelf the board rests on
post_d       = 7.0;    // corner screw posts (lid screws + board corner support)
screw_d      = 2.8;    // pilot hole for M3 self-tappers
screw_head_d = 6.2;    // lid countersink
vent_slots   = true;   // side vents over the ESP32 half
zip_slots    = true;   // zip-tie slots in the base for under-desk mounting

/* ---------- derived ---------- */
in_l  = board_l + 2*clearance;
in_w  = board_w + 2*clearance;
in_h  = pins_below + board_th + above_board + headroom;
out_l = in_l + 2*wall;
out_w = in_w + 2*wall;
base_h = floor_th + in_h - lip_h;          // lid skirt completes the height
board_z = floor_th + pins_below;           // board underside
board_top = board_z + board_th;

$fn = 32;
eps = 0.01;

module screw_posts(hole_d, hole_depth) {
  for (p = [[wall + post_d/2, wall + post_d/2],
            [out_l - wall - post_d/2, wall + post_d/2],
            [wall + post_d/2, out_w - wall - post_d/2],
            [out_l - wall - post_d/2, out_w - wall - post_d/2]])
    translate([p[0], p[1], 0]) difference() {
      cylinder(d = post_d, h = floor_th + in_h);
      translate([0, 0, floor_th + in_h - hole_depth]) cylinder(d = hole_d, h = hole_depth + eps);
    }
}

module base() {
  difference() {
    union() {
      // shell
      difference() {
        cube([out_l, out_w, base_h]);
        translate([wall, wall, floor_th]) cube([in_l, in_w, base_h]);
      }
      // board ledge (overlaps floor and walls slightly for clean manifold union)
      difference() {
        translate([wall - 0.1, wall - 0.1, floor_th - 0.1])
          cube([in_l + 0.2, in_w + 0.2, pins_below + 0.1]);
        translate([wall + ledge_w, wall + ledge_w, floor_th - 0.2])
          cube([in_l - 2*ledge_w, in_w - 2*ledge_w, pins_below + 0.4]);
      }
      screw_posts(screw_d, 12);
    }
    // USB opening in the x=0 end wall
    translate([-eps,
               out_w/2 + usb_y_off - usb_w/2,
               board_top + usb_z])
      cube([wall + 2*eps, usb_w, usb_h]);
    // vents over the ESP32 half, both long sides
    if (vent_slots)
      for (y = [-eps, out_w - wall - eps])
        for (x = [15 : 12 : board_l/2])
          translate([x, y, floor_th + pins_below + 6])
            cube([6, wall + 2*eps, base_h - (floor_th + pins_below + 6) - 2.5]);
    // zip-tie slots through the floor near each end
    if (zip_slots)
      for (x = [out_l*0.2, out_l*0.8])
        for (y = [wall + 2, out_w - wall - 2 - 5])
          translate([x - 2, y, -eps]) cube([4, 5, floor_th + 2*eps]);
  }
}

module lid() {
  difference() {
    union() {
      cube([out_l, out_w, lid_th]);
      // skirt that drops inside the base walls (overlaps plate for clean union)
      difference() {
        translate([wall + 0.25, wall + 0.25, lid_th - 0.1])
          cube([in_l - 0.5, in_w - 0.5, lip_h + 0.1]);
        translate([wall + 0.25 + 1.6, wall + 0.25 + 1.6, lid_th - 0.2])
          cube([in_l - 0.5 - 3.2, in_w - 0.5 - 3.2, lip_h + 0.4]);
      }
    }
    // RJ45 openings (jacks face up through the lid)
    for (j = [[rj45_a_x, rj45_a_y], [rj45_b_x, rj45_b_y]])
      translate([out_l - wall - clearance - j[0] - rj45_hole_w/2,
                 out_w/2 + j[1] - rj45_hole_d/2,
                 -eps])
        cube([rj45_hole_w, rj45_hole_d, lid_th + lip_h + 2*eps]);
    // lid screw holes + countersinks (match base posts)
    for (p = [[wall + post_d/2, wall + post_d/2],
              [out_l - wall - post_d/2, wall + post_d/2],
              [wall + post_d/2, out_w - wall - post_d/2],
              [out_l - wall - post_d/2, out_w - wall - post_d/2]])
      translate([p[0], p[1], -eps]) {
        cylinder(d = 3.4, h = lid_th + lip_h + 2*eps);
        cylinder(d = screw_head_d, h = 1.6);
      }
  }
}

if (part == "base") base();
if (part == "lid") lid();
if (part == "both") {
  base();
  translate([0, out_w + 10, 0]) lid();
}
