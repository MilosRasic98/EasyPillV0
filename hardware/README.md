# Hardware

`schematics/` holds the V1 schematic and the review of it. Between them they are
the origin of every constraint the firmware works around, the shared-mux problem
above all: one set of address lines drives three XS3A4051 muxes at once, so the
hall sensors, the analog channels and the output FET gates can never be
addressed independently. [../docs/DESIGN.md](../docs/DESIGN.md) opens with what
that costs.

The pin map, the mux channel map, the divider ratios and every measurement taken
on a real board are in [../docs/HARDWARE.md](../docs/HARDWARE.md).
