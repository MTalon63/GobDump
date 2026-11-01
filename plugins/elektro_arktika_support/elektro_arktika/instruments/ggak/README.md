# GGAK decoder (Elektro-L / Arktika-M)

This decoder is a port of the reverse-engineered GGAK work from
[SatDump PR #1143](https://github.com/SatDump/SatDump/pull/1143), with a
streaming CSV sink added.

The protocol is **not** CCSDS. It is a proprietary 224-byte frame carried in the
5 kbps SEM telemetry downlink (1693 MHz Elektro-L, 1703 MHz Arktika-M).

## Frame layout

| Offset | Size | Field |
| --- | ---: | --- |
| 0 | 4 | ASM / sync |
| 4 | 1 | frame type |
| 5 | 2 | master counter (BE) |
| 7 | 2 | channel counter (BE) |
| 9 | 1 | source ID: `0x30` = BND-E, `0x31`/`0x32`/`0x33` = BND-VE |
| 10 | 2 | coarse time (BE), 254.84 s tick |
| 12 | 1 | fine time, 1/256 of a tick |
| 13 | 209 | payload |
| 222 | 2 | checksum (BE, 16-bit sum of bytes 0..221) |

The decoder auto-detects the generation from the first data frame. BND-E uses
`0xAA` as its fill byte, BND-VE uses `0x33`. `0x32` is not in the
reference table; it is treated as BND-VE with a warning.

| Type | Meaning |
| --- | --- |
| `0x00` | Housekeeping-A / ISP-2M |
| `0x10` | SKIF-VE SER summary |
| `0x20` | SKIF-VE/V ESA |
| `0x30` | SKIF-VE/G ESA |
| `0x40` | GALS-VE + SKIF-VE MIP |
| `0x50` | SKL-E calibration |
| `0x5C` | SKL-E spectral |
| `0x60` | Housekeeping-B (BND-E only, recognized but not parsed) |
| `0x70` | FM-VE magnetometer |
| `0x77`, `0x88` | fill |

## CSV output

The decoder writes **one file per instrument group**, with one row per reading
or packet. Only instruments that actually produced data get a file. Raw frames
are opt-in and off by default.

Files are written under `<output>/GGAK` unless `csv_dir` is set. The default
prefix is `ggak`.

| File | Contents |
| --- | --- |
| `ggak_mag_YYYY-MM-DD.csv` | FM-VE magnetometer, one row per reading |
| `ggak_particle_YYYY-MM-DD.csv` | GALS-VE + SKIF-VE MIP, one row per reading |
| `ggak_esa_YYYY-MM-DD.csv` | SKIF-VE/V and /G ESA, one row per packet |
| `ggak_skl_YYYY-MM-DD.csv` | SKL-E spectral/calibration, one row per packet |
| `ggak_hk_YYYY-MM-DD.csv` | housekeeping, one row per 12-byte block |
| `ggak_frames_YYYY-MM-DD.csv` | optional raw frames, one row per unique frame |

All files are split at **UTC midnight** using `host_utc`. Within a day, a file
that reaches `csv_max_mb` (default **64 MB**) continues as
`..._YYYY-MM-DD_0002.csv`, `..._0003.csv`, etc. A row is never split across
files. Set `csv_max_mb=0` to disable the size cap (daily splitting still
applies). On restart the newest part for the current day is appended if its
header matches; a different header starts a new numbered part. Old files are
never rewritten.

Exact duplicate frames are skipped in the reader (bounded to the last 8192
frames). This removes duplicates that appear in the input stream, such as a
repeated uplink frame; the decoder summary reports how many were skipped.

### Common columns

| Column | Meaning |
| --- | --- |
| `host_utc` | Host clock at CSV write time, ISO 8601 UTC with second resolution. This is the decode time, not the onboard measurement epoch. For live decoding it is close to acquisition; for offline replay it is the replay time. |
| `elapsed_s` | Onboard elapsed time: `coarse_time * 254.84 + fine_time * (254.84 / 256)`. The absolute epoch is unknown; this is the value to use as the telemetry time axis. |
| `master_counter` | 16-bit frame counter from the 224-byte frame. Identifies the frame that produced the row. Wraps at 65536. |
| `record_index` | Zero-based index of the reading/packet within the records appended for that frame. It normally starts at 0 for each frame; it is not a global sequence number. |
| `checksum_ok` | 1 if the frame checksum matches, 0 otherwise. It is per-frame and repeated on every row from that frame. Filter to 1 for clean data. |

Rows are sparse: fill frames, unknown frame types, and frames whose payload
contains no valid readings produce no row in the per-instrument files.

### `ggak_mag` — FM-VE magnetometer

```text
host_utc,elapsed_s,master_counter,record_index,B_total_nT,Bx_nT,By_nT,Bz_nT,voltage_V,B_total_raw,Bx_raw,By_raw,Bz_raw,status_flag,valid,checksum_ok
```

| Column | Meaning |
| --- | --- |
| `B_total_nT`, `Bx_nT`, `By_nT`, `Bz_nT` | Current best-effort conversion to nT. Provisional; check `valid` before using. |
| `voltage_V` | `voltage_raw / 125`. |
| `B_total_raw`, `Bx_raw`, `By_raw`, `Bz_raw` | Raw 16-bit ADC counts. Use these for any recalibration. |
| `status_flag` | Raw status byte from the reading. |
| `valid` | 1 only when the raw counts are within the decoder sanity range and `status_flag != 0xFF`. 0 means do not trust the nT values. |

Up to 13 readings are decoded from a `0x70` frame; tests using `source_id`
`0x32` produced raw values above the current sanity limit (all `valid=0`).
Do not graph the nT columns for those rows without checking the calibration.

### `ggak_particle` — GALS-VE + SKIF-VE MIP

```text
host_utc,elapsed_s,master_counter,record_index,sub_type,Ep600,Ep800,Ep1100,Cg1,Cg2,Cg3,Cg4,MIP,checksum_ok
```

One row per `A0`/`A1` particle reading in a `0x40` frame. `sub_type` is
the raw subtype byte. `Ep600`, `Ep800`, `Ep1100`, `Cg1`–`Cg4` and
`MIP` are raw 16-bit counts; no absolute calibration is applied. Up to 11
readings are possible per frame.

### `ggak_esa` — SKIF-VE/V and SKIF-VE/G ESA

```text
host_utc,elapsed_s,master_counter,direction,record_index,seq,e0_05keV,...,e20_00keV,bin13,...,bin32,checksum_ok
```

| Column | Meaning |
| --- | --- |
| `direction` | `V` for `0x20` frames, `G` for `0x30` frames. |
| `seq` | Packet sequence byte. |
| `e0_05keV` ... `e20_00keV` | First 12 energy steps: 0.05, 0.25, 0.45, 0.80, 1.30, 2.50, 4.50, 7.50, 10.00, 13.00, 16.00, 20.00 keV. |
| `bin13` ... `bin32` | Extra 16-bit channels beyond the named energy table. |

Each `0x20`/`0x30` frame carries up to three 69-byte ESA packets, so
`record_index` is 0–2. Values are raw counts.

### `ggak_skl` — SKL-E spectral

```text
host_utc,elapsed_s,frame_type,master_counter,record_index,seq,n_values,bin1,bin2,...,bin206,checksum_ok
```

| Column | Meaning |
| --- | --- |
| `frame_type` | `92` (`0x5C`, spectral) or `80` (`0x50`, calibration). |
| `seq` | Packet sequence byte. |
| `n_values` | Number of 16-bit values actually decoded for this packet (1–206). |
| `bin1` ... `bin206` | Signed 16-bit counts. Empty fields mean the packet was shorter than that bin. |

The fixed 206-column schema is larger than a semicolon-separated list but needs
no custom parsing: pandas, Excel, R, etc. read every bin as a number. Use
`n_values` to know which bins are real. Spectral packets can span frames; in
that case the row uses `master_counter`, `elapsed_s` and `host_utc` from the
frame that completed the packet.

### `ggak_hk` — housekeeping

```text
host_utc,elapsed_s,master_counter,record_type,record_index,seq,ch0,ch1,ch2,ch3,ch4,checksum_ok
```

`record_type` is `hk_a` (type `0x00`, marker `0x80`) or `hk_ser` (type
`0x10`, marker `0x88`). `ch0`–`ch4` are raw 16-bit values.

For `hk_a`:
- `ch0` is the HK multiplexer channel.
- `ch1` is the BND-VE thermal value (14-bit).
- `ch2` is ISP-2M TSI in counts; multiply by `0.0331` for W/m².

For `hk_ser`:
- `ch2` is SKIF-VE/V SER.
- `ch4` is SKIF-VE/G SER.

### `ggak_frames` — optional raw frames

```text
host_utc,elapsed_s,asm_hex,frame_type,frame_type_name,master_counter,channel_counter,source_id,coarse_time,fine_time,checksum_ok,checksum_raw,payload_hex
```

One row per unique 224-byte frame, including fill and unknown frame types.
`asm_hex` is 4 bytes as hex; `payload_hex` is the full 209-byte payload as hex
(418 characters). `checksum_raw` is the 16-bit checksum from the frame in
decimal. This file is large and is **off by default**. Enable the **Write raw
frames CSV** checkbox or pass `--csv_raw_frames`.

### Reading the files with third-party tools

Use `elapsed_s` for the measurement time axis. `host_utc` is useful for
knowing when the row was decoded, but it repeats for all rows in a frame and is
not an absolute measurement epoch.

```python
import pandas as pd

mag = pd.read_csv("ggak_mag_2026-10-27.csv")
mag = mag[(mag.valid == 1) & (mag.checksum_ok == 1)]

skl = pd.read_csv("ggak_skl_2026-10-27.csv")
long = skl.melt(
    id_vars=["elapsed_s", "master_counter", "seq", "n_values"],
    value_vars=[f"bin{i}" for i in range(1, 207)],
    var_name="bin",
    value_name="counts",
).dropna(subset=["counts"])
```

For frame-level joins, `master_counter` plus `elapsed_s` is the practical key
within a day; `master_counter` wraps at 65536.

## Usage

GUI: choose the **ELEKTRO-L GGAK**, **ARKTIKA-M N°1 GGAK**, or
**ARKTIKA-M N°2 GGAK** pipeline. CSV is enabled by default; raw frames are off
until you tick the box.

Offline CLI (input must already be 224-byte GGAK CADUs, or 1792-byte CADUs
containing 8 of them):

```text
gobdump pipeline elektro_ggak cadu input.cadu output_dir --csv
```

Enable raw frames:

```text
gobdump pipeline elektro_ggak cadu input.cadu output_dir --csv --csv_raw_frames
```

Choose a CSV directory or a 32 MB part limit (use the `=` form; the generic
pipeline string options are CLI flags):

```text
gobdump pipeline elektro_ggak cadu input.cadu output_dir --csv --csv_dir=D:\ggak-csv --csv_max_mb=32
```

## Caveats

Daily boundaries use the host UTC clock (`host_utc`). For live decoding that is
the acquisition day. For offline reprocessing it is the decode day, not the
original capture day; use `elapsed_s` for the telemetry time axis, or write to
a fresh output directory per capture.

This is reverse-engineered data, not an official GGAK ICD. The PR was not
merged upstream, and the author noted that some scales/units come from Russian
papers rather than a byte-level specification. The magnetometer and particle
mappings are the best available; treat absolute values as provisional. In
particular, `source_id 0x32` is not in the reference table, the magnetometer
nT values for it can be far outside the physical range, and the `valid` flag
may be 0. Use the raw count columns for calibration work.

DIR-E and VUSS-E do not appear in the BND-VE stream at all, and `0x60`
Housekeeping-B is recognized but not decoded.

Files written by older builds used a different SKL header
(`n_values,values` with a semicolon-separated list) and a shorter mag header.
Those files are left untouched; a new `_0002` part is started when the header
differs. Move or delete old CSVs if you want a clean, duplicate-free run.
