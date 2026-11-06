# Hardware

The electrical and mechanical source for the box. The firmware's view of this
hardware — pin map, mux channel map, divider ratios, and every measurement taken
on a real board — lives in [../docs/HARDWARE.md](../docs/HARDWARE.md), which is
the document the firmware is written against. Read that first; this directory
holds the artefacts it refers to.

## `schematics/`

`Schematic_EaSyPill_V1_2026-09-27.pdf` is the V1 schematic, and
`schematic-review.md` is the review of it. Between them they are the origin of
every constraint the firmware works around, the shared-mux problem above all:
one set of address lines drives three XS3A4051 muxes at once, so the hall
sensors, the analog channels and the output FET gates can never be addressed
independently. [../docs/DESIGN.md](../docs/DESIGN.md) opens with what that costs.

## `cad/`

Enclosure and tray CAD. Empty so far.

Worth recording here when it lands: **which way round the PCB mounts**, because
the sensor row runs opposite to the days. `hall_to_lid` is `[6,5,4,3,2,1,0]` —
sensor 0 is Sunday and sensor 6 is Monday — measured one sensor at a time on the
first board. That is a mechanical fact, not an electrical one, so a different
mounting orientation changes it and `tools/mapdays.py` re-derives it over HTTP
without a reflash.

Prefer committing a neutral exchange format (STEP for solids, STL or 3MF for
anything to be printed) alongside the native file, so the geometry stays
readable without the original CAD package.

## File sizes

GitHub rejects any single file over 100 MB and starts warning about
repositories over 1 GB. Large CAD assemblies and renders should go through Git
LFS rather than straight into history, because once a big file is committed it
stays in the history even if a later commit deletes it. See
[../media/README.md](../media/README.md) for the same problem with video.
