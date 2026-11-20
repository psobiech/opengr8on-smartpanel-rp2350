// Waveshare RP2350-Touch-LCD-4 surface mount enclosure for a flush box (puszka podtynkowa) Ø60 mm.
// Slots at 0° and 90° fit the box in any orientation.
// Two parts: front shell (glass pocket + window) and back plate (box screw slots, cable hole, PCB posts).
// Coordinates: viewed from the BACK (component side), x right, y up, USB-C on the left.
// Part to render: "shell", "back", "both" (assembled, exploded) or "section".
part = "both";

// ---- module (mm) ----
glass      = 84.2;     // front glass, square
glass_r    = 3;        // glass corner radius
active_w   = 71.86;    // visible area
active_h   = 70.18;
active_dy  = -1.4;     // visible area shift vs. glass centre (negative = towards terminals)
pcb_w      = 74.5;
pcb_h      = 76.5;
hole_dx    = 66.5;     // PCB mounting hole pitch
hole_dy    = 68.5;
hole_d     = 1.5;      // PCB hole (M2.5)
module_t   = 5;         // glass + PCB stack thickness (verify with calipers)
module_d   = 16;        // total module depth incl. terminal block
comp_h     = module_d - module_t;  // parts above PCB back
term_w     = 41;        // terminal block width
term_cx    = 0.6;       // terminal centre offset from PCB centre
usb_y      = -10.5;        // USB-C centre vs PCB centre
usb_w      = 14;
usb_h      = 7;
usb_z_off  = 5;       // USB-C centre above PCB back face

// ---- enclosure (mm) ----
clr        = 0.3;       // fit clearance
front_t    = 2;         // front wall behind window
wall       = 6;         // side wall
back_t     = 6;         // back plate thickness (drop-in, edge takes side screws)
corner_r   = 4;
screw_d    = 2.6;       // self-tapping M3 pilot
screw_cl   = 3.4;       // M3 clearance
head_d     = 6;         // M3 head counterbore in shell wall
head_depth = 2.5;
side_screws = [[-1, 0], [1, 0], [0, 1]];   // wall sides with screws (not bottom: terminal slot)
screw_y    = 20;        // offset of side screws along the wall (clear of USB-C)

// ---- flush box ----
box_pitch  = 60;        // screw pitch of Ø60 box
slot_w     = 4.6;       // M4 slot
slot_len   = 6;         // adjustment travel
cable_d    = 36;        // cable passage (box inner Ø60)

post_trim = 9;         // shorten PCB posts (PCB back is not flat)

// ---- derived ----
cav     = glass + 2*clr;
outer   = cav + 2*wall;
cav_d   = module_d + 0.2;   // module space, tiny slack
shell_h = front_t + cav_d + back_t;   // back plate drops into the rear of the cavity
plate_w = cav - 0.4;
screw_z = shell_h - back_t/2;
post_h  = cav_d - module_t - 0.1 - post_trim;   // posts stop short of the uneven PCB back

$fa = 1; $fs = 0.4;
eps = 0.01;

module rsq(w, h, r) {
    offset(r = r) offset(delta = -r) square([w, h], center = true);
}

module shell() {
    difference() {
        union() {
            linear_extrude(shell_h) rsq(outer, outer, corner_r);
        }
        translate([0, 0, front_t]) linear_extrude(cav_d + back_t + eps) rsq(cav, cav, glass_r + clr);
        // display window
        translate([0, active_dy, -eps]) linear_extrude(front_t + 2*eps)
            square([active_w + 0.4, active_h + 0.4], center = true);
        // side screws holding the back plate (inserted from outside after mounting)
        for (d = side_screws) {
            ang = atan2(d[1], d[0]);
            // x-walls: shift along y so the screw avoids the USB-C cutout
            translate([0, d[0] != 0 ? screw_y : 0, screw_z]) rotate([0, 0, ang]) rotate([0, 90, 0]) {
                translate([0, 0, cav/2 - 1]) cylinder(d = screw_cl, h = wall + 2);
                translate([0, 0, outer/2 - head_depth]) cylinder(d = head_d, h = head_depth + eps);
            }
        }
        // terminal wire slot, bottom wall
        translate([term_cx, -outer/2 + wall/2, front_t + module_t + comp_h/2 + 1])
            cube([term_w + 4, wall + 2*eps + 4, comp_h + 2], center = true);
        // USB-C on left wall
        translate([-outer/2 + wall/2, usb_y, front_t + module_t + usb_z_off])
            cube([wall + 4, usb_w + 1, usb_h + 1], center = true);
    }
}

module back_plate() {
    difference() {
        union() {
            linear_extrude(back_t) rsq(plate_w, plate_w, glass_r);
            // PCB hold-down posts with locating pins
            for (sx = [-1, 1], sy = [-1, 1])
                translate([sx*hole_dx/2, sy*hole_dy/2, back_t]) {
                    cylinder(d = 6, h = post_h);
                    cylinder(d = hole_d - 0.3, h = post_h + 1.5);
                }
        }
        // cable passage
        translate([0, 0, -eps]) cylinder(d = cable_d, h = back_t + 2*eps);
        // M4 slots for the Ø60 box, at 0° and 90° so the box orientation does not matter
        for (rot = [0, 90], sx = [-1, 1])
            rotate(rot) hull() for (dx = [-slot_len/2, slot_len/2])
                translate([sx*box_pitch/2 + dx, 0, -eps])
                    cylinder(d = slot_w, h = back_t + 2*eps);
        // pilot holes in the plate edge for the side screws
        for (d = side_screws)
            translate([0, d[0] != 0 ? screw_y : 0, back_t/2]) rotate([0, 0, atan2(d[1], d[0])]) rotate([0, 90, 0])
                translate([0, 0, plate_w/2 - 7]) cylinder(d = screw_d, h = 8);
    }
}

module back_plate_placed() {
    // flip so posts point to the shell cavity
    translate([0, 0, shell_h]) mirror([0, 0, 1]) back_plate();
}

if (part == "shell") shell();
else if (part == "back") back_plate();
else if (part == "section") difference() {
    union() { shell(); color("orange") back_plate_placed(); }
    translate([0, -100, -1]) cube([200, 100, 200]);   // cut y<0... keeps y>0
}
else {
    shell();
    translate([0, 0, shell_h + 15]) mirror([0, 0, 1]) color("orange") back_plate();
}
