# PROBA support

Instruments of ESA's PROBA satellites:

| Satellite | Instruments |
|---|---|
| PROBA-1 | CHRIS, HRC |
| PROBA-2 | SWAP, LYRA |
| PROBA-V | Vegetation (S and X band), GPS logs |

The PROBA-2 readers follow the packet formats and the time reconstruction of the
[`proba-2_gif_generator`](https://github.com/BUSH222/proba-2_gif_generator) project:
`swap_pipeline/NOTES.md` and `ALGORITHM.md` for SWAP, `lyra_pipeline/NOTES.md` for LYRA.
All multi-byte numbers in the telemetry are big-endian.

## SWAP (APID 20)

The image packets are assembled by their end-of-acquisition stamp (payload bytes 1-6), and the
first packet of an image is the one with the JPEG SOI at payload byte 92. An image is
`ceil((80 + SIZCOMPI) / 65520)` packets, with consecutive sequence counts; every packet but the
last is exactly 65 536 bytes long.

* **JPEG data**: payload bytes 92 to the end - 2 in the first packet (the 84 byte image header
  comes first), 8 to the end - 2 in the others. The last 2 bytes are the CRC. Earlier versions
  took 14 bytes of header from every packet and dropped only one CRC byte, which lost 6 JPEG
  bytes per packet, kept a CRC byte, and made libjpeg report corrupt data.
* **Trimming**: the 4 byte JPEG length at payload bytes 88-91 removes the 16 byte padding of a
  complete image.
* **Lost packets**: a missing packet, or one whose length does not match (a packet glued across
  lost frames), cuts the stream there and closes it with an EOI. The JPEG is progressive, so the
  part that came through is the whole image at a lower quality.
* **Bit errors**: packets that only fail their CRC-16 are kept, unless the image does not decode,
  in which case assembly is retried without them. Images that still do not decode are skipped.
* **Time**: the 48 bit stamps are turned into UTC with the GPS receiver's F4 records (APID 18) of
  the same dump: the median GPS - tick and its drift of 1.064 s a day, minus 18 leap seconds.
  File names use the start of acquisition (payload bytes 8-13): `SWAP_YYYYMMDDTHHMMSSZ.png`.
* **Coarse north up**: `LANG_ROT` (payload bytes 23-30, the telemetry's commanded roll) is
  `sin(roll / 2)`; roll 0/90/180/270 becomes 3/2/1/0 counter-clockwise quarter turns, exactly as the
  reference pipeline's `np.rot90` (`north_up_steps`). If the field is damaged the image is left as
  it came down, with a warning.

The JPEGs are the level 0 images, coarsely turned north up (ecliptic north). They are not
calibrated. Output goes to `<output>/SWAP`.

### Optional limb alignment (off by default)

The `proba2_dump` pipeline has a **SWAP: centre sun** checkbox (off by default; `--swap_align`
on the command line). When on, every frame is placed with the reference's `p2sw_imgcor`
geometry, in one cubic interpolation:

* the solar limb is found on the detector values (ring correlation for a start, then a circle
  through the brightest limb point of 720 angles), so the sun centre is measured from the image;
* the sun is moved to (511.5, 511.5), the pixels are made square (`CDELT2/CDELT1` = 1.0036), and
  the frame is turned by SWAP's -0.3 degree mounting offset and by `SOLAR_EP` (the 0-360 degree
  angle from ecliptic north to solar north) to solar north up.

`SOLAR_EP` is computed from the image date and the IAU solar pole, reproducing the archive's
value to about 0.003 degree; the photospheric radius uses the archive's 696 000 km. Frames whose
limb fit fails keep the coarse north-up orientation, with a warning. This only changes geometry,
not photometry.

### Optional animation GIF (needs the alignment)

With both **SWAP: centre sun** and **SWAP: animation GIF** enabled (`--swap_align --swap_gif` on
the command line), the aligned frames are also written to `<output>/SWAP/SWAP.gif`: the frames in
time order, 10 fps, looping forever, on one shared 256 colour palette — ImageMagick's gold tint
(`-fill #edb103 -tint 100`, the same palette as the reference's `render.py`). The GIF is built
from the saved, aligned frames, so the pixels are the PNGs' grey levels looked up in that palette;
nothing is re-encoded.

The animation checkbox alone does nothing on purpose: an uncentred, rotating GIF is not what the
reference produces, so it is ignored with a warning unless the sun-centring option is also on.

## LYRA (APID 21, housekeeping APID 2)

LYRA's samples arrive in APID 21, compressed with Unix `compress` (LZW), 4 channels x 501
samples per packet, with the first value and 500 differences per channel, and time marks at the
end of the block. The even and odd samples of the 20 Hz series are in two alternating packet
streams; both are merged, sorted and deduplicated by sample counter.

Output goes to `<output>/LYRA`:

* `lyra_YYYYMMDD-HHMMSS.csv`: one row per sample, with the UTC time, seconds of the UTC day,
  sample counter, quality factor, level 0 counts, level 1 count rates (kHz), level 2 irradiance
  (W/m^2) and the unit 2 temperature. Lines starting with `#` describe the run.
* `lyra_YYYYMMDD-HHMMSS_lev2.png`: the four channels of level 2, stacked.

Level 1 is `counts / 49.9894193 ms`. Level 2 is
`k (d / 1 AU)^2 (level 1 - D0 - A exp(B T)) + G`, with `T` the temperature of LYRA's nominal
unit (unit 2) from the APID 2 housekeeping reports (structure 70) and `G` the degradation
correction, tabulated and interpolated by day. The constants and the table are the ones fitted in
`lyra_pipeline/lyra_caldb/` of the reference project; they are valid from 2025-03-25.
