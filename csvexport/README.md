# tracy-csvexport (Bagira fork)

`tracy-csvexport` is the command-line exporter from the [Tracy profiler](https://github.com/wolfpld/tracy).
This fork keeps upstream's tool intact and adds a **per-event export mode (`-x`)** that writes
one row per zone occurrence, with all strings replaced by dictionary indices, plus filters,
time/frame windows, row layouts, parent attribution and a plot export built for analysing large
traces in Excel, pandas and similar tools.

Base: upstream Tracy `v0.14.1` (+87 commits, `f60889e1`). Everything below the
*Per-event export mode* heading of `tracy-csvexport --help` is Bagira's addition; the classic
modes (`-u`, `-g`, `-m`, `-p`, `-t`) behave exactly as upstream.

## Quick start

```bat
:: 1. Every occurrence of the zone named Tick on the main thread, plus every GPUFrame
tracy-csvexport.exe -x out.csv -F "Tick@Main thread" -F GPUFrame@gpu trace.tracy

:: 2. Per-GPUFrame breakdown of all GPU zones for frames 41400..41599, times in seconds from 0
tracy-csvexport.exe -x gpu.csv -F GPUFrame@gpu -f @gpu -P GPUFrame -B 41400 -n 200 -S -z -L trace.tracy

:: 3. Frame, GPUFrame and Tick side by side, one row per frame
tracy-csvexport.exe -x view.csv -F Frame@frames -F GPUFrame@gpu -F "Tick@Main thread" -o columns -S -z -L trace.tracy

:: 4. Every plot lane (memory usage, CPU usage, TracyPlot values), stamped with its frame
tracy-csvexport.exe -x plots.csv -f @plots trace.tracy
```

Each run writes `out.csv`, `out.csv.dict` (string table) and `out.csv.threads` (thread table),
plus `out.csv.plots` when a `plots` term is used.

## Options of the `-x` mode

Everything here requires `-x`; `-c` (case-sensitive matching), `-e` (self time, CPU zones) and
`-s <sep>` (separator) from upstream also apply. `-x` cannot be combined with `-u`, `-g`, `-m`,
`-p` or `-t`.

### Output

| Option | Meaning |
|---|---|
| `-x, --export <file>` | Write rows to `<file>`, strings to `<file>.dict`, threads to `<file>.threads`. |
| `-L, --no-location` | Omit the `src_file` and `src_line` columns. |
| `-S, --seconds` | Times as floating-point seconds with 9 decimals instead of integer nanoseconds. Column names become `s_since_start` / `exec_time_s`. |
| `-z, --zero` | Shift start times so the earliest exported event starts at 0. |
| `-o, --order <mode>` | `sequential` (by name, then start; default), `interleaved` (by start), `columns` (see below). |

### Selecting events

| Option | Meaning |
|---|---|
| `-f NAME[@SCOPE]` | Repeatable. Substring match on the zone name (case-insensitive unless `-c`); terms are OR'd. Empty NAME matches everything, so `-f @gpu` = all GPU zones. |
| `-F, --filter-exact NAME[@SCOPE]` | Like `-f`, but NAME must equal the whole zone name. Mix freely with `-f`. |
| `-T, --scope <scope>` | Default scope for terms without `@`, and for the no-filter case. Default `all`. |

SCOPE says where the name is looked for:

| Scope | Selects |
|---|---|
| `all` | CPU zones on every thread + GPU zones (default) |
| `cpu` | CPU zones on every thread |
| `gpu` | GPU zones only |
| `frames` | FrameMark frame sets (see *Frames*) |
| `messages` | TracyMessage texts (see *Messages*); NAME matches the text |
| `plots` | Plot lanes (see *Plots*); NAME matches the plot name, rows go to `<file>.plots` |
| anything else | CPU zones on threads whose **name contains** the text, or whose **id equals** it when it is all digits (ids are listed in `.threads`) |

Zone names are often C++ function names containing `::`, which is why `@` is the separator.

### Window

Only one of the two windows can be given. Membership is decided by the event's **start**; an
event starting inside the window is kept even if it ends outside it.

| Option | Meaning |
|---|---|
| `-b, --begin <sec>` | Start at `<sec>` seconds after the trace start (the profiler's 0). |
| `-l, --length <sec>` | Window length in seconds (default unbounded). |
| `-B, --begin-frame <n>` | Start at the begin of frame `n` of the main frame set. |
| `-n, --frames <n>` | Window length in frames. |
| `-E, --end-frame <n>` | Last frame included (alternative to `-n`). |

**Frame numbers are the ones the profiler displays** ("Frame 41369") and the `frame` column prints,
*not* a 0-based index. A trace captured on demand typically starts at a high number; a number
outside the trace is rejected with the valid range in the message. `-n` running past the last
frame ends at the trace end.

### Parent attribution

| Option | Meaning |
|---|---|
| `-P, --parent NAME` | Add a `parent_ns_since_start` / `parent_s_since_start` column: start of the nearest enclosing GPU zone named exactly NAME. |

Rows of NAME itself reference their own start, so one pivot key covers the parent's duration and
its children's sums. Rows without such an ancestor (and all CPU zones and frames) leave the
column empty. Children follow their parent's window membership, so a parent is exported either
with all of its children or not at all.

## Output files

### `<file>` (CSV)

Default columns: `name, src_file, src_line, ns_since_start, exec_time_ns, thread, gpu, value`.

| Column | Content |
|---|---|
| `name`, `src_file`, `value` | Dictionary indices into `.dict`. `value` is the zone text; empty when the zone has none. |
| `src_line` | Plain number. Both location columns disappear with `-L`. |
| `ns_since_start` / `s_since_start` | Zone start (raw trace timestamp; 0-based with `-z`). |
| `exec_time_ns` / `exec_time_s` | Zone duration (self time with `-e`, CPU zones only). |
| `thread` | OS thread id for CPU zones, GPU context index for GPU zones (separate id spaces; see `gpu`). Empty for frames. |
| `gpu` | `1` for GPU zones, `0` otherwise. |
| `frame` | Added when a `frames` term is used: frame number (profiler numbering); empty for zones. |
| `parent_*_since_start` | Added with `-P`: start of the enclosing parent zone. |

The CSV never contains a string, so no quoting is needed. GPU zones with a zero time span are
included (upstream `-g` omits them); GPU zones whose timestamps were never reported by the GPU
are skipped, as in the profiler.

### `<file>.dict`

`index<sep>string`, indices assigned from 0 in first-seen order, one shared index space for all
string columns. Strings are CSV-quoted (`"..."`, embedded `"` doubled), so paths or texts
containing the separator survive.

### `<file>.threads`

`id<sep>gpu<sep>name` for every thread and GPU context referenced by the exported rows, sorted by
id. Thread names are not unique (a worker pool shares one name), which is why the CSV carries ids.

### `<file>.plots`

Written only when a `plots` term is used. `name<sep>format<sep>ns_since_start<sep>value<sep>frame`,
one row per plot sample.

| Column | Content |
|---|---|
| `name` | Dictionary index of the plot name as the profiler's lane header shows it. |
| `format` | Dictionary index of `Number`, `Memory` (bytes), `Percentage` or `Watt` — the unit of `value`. |
| `ns_since_start` / `s_since_start` | Sample time, same units and zero base as the CSV (`-S`, `-z`). |
| `value` | The plot value; integral values print without a decimal point. |
| `frame` | Frame of the main frame set holding the sample (profiler numbering), empty outside every frame. |

### Frames

`-f @frames` / `-F Frame@frames` export FrameMark frame sets as rows: `name` is the frame set
name (`Frame` for the main set), one row per frame with begin time and length (to the next
frame's begin), empty location and thread cells, and the `frame` column. Placeholder frames from
before the trace start (present in on-demand captures) are skipped, as the profiler does.

### Messages

`-f @messages` / `-f "campos@messages"` export TracyMessage texts as rows: `name` is the constant
`Message`, `value` is the text (dictionary index), `thread` the emitting thread, `exec_time` 0,
`gpu` 0. The text is what NAME is matched against, so `-f "sensor@messages"` selects the sensor
messages only. Combine with `-F Frame@frames` (or `-b/-l`, `-B/-n`) to place messages in frames.

### Plots

`-f @plots` exports every plot lane the profiler draws; `-f "memory@plots"` or
`-F "Memory/RAM@plots"` select by plot name. Rows go to `<file>.plots`, never into the CSV: a
plot sample has a numeric value and no thread, duration or source location, so mixing it into the
zone rows would only add empty columns.

Names are the profiler's lane headers, which is why the built-in plots come out as `Memory usage`
(allocation tracking) and `CPU usage` (system time) rather than under the name of whatever string
happens to sit at handle 0 — upstream's `-u -p` merges both of those into one bogus group.

The window (`-b/-l`, `-B/-n/-E`), `-S` and `-z` apply as they do to rows, and `-z` uses one zero
base for both files so their times stay comparable. `-L`, `-e`, `-P` and `-o` do not affect
`.plots`; it has one fixed layout.

### `columns` order

Every event type gets its own group of columns (`<name>.src_file`, `<name>.src_line`,
`<name>.ns_since_start`, `<name>.exec_time_ns`, `<name>.thread`, `<name>.value` — `frame`
instead of `value` for frame groups, plus `<name>.parent_...` with `-P`). Row *k* holds the
*k*-th occurrence of every type; a type with fewer occurrences leaves its cells empty. Headers
use the readable names (CSV-quoted); cells are indices / numbers as in the other orders. Handy
for a handful of event types, unreadable for dozens.

## Recipe: per-frame GPU breakdown in Excel

1. Export with parent attribution over the frames of interest:

   ```bat
   tracy-csvexport.exe -x gpu.csv -F GPUFrame@gpu -f @gpu -P GPUFrame -B 41400 -n 200 -S -z -L trace.tracy
   ```

2. Load `gpu.csv` (Data > From Text/CSV) and `gpu.csv.dict` as a second sheet.
3. Insert a pivot table: **Rows** = `parent_s_since_start`, **Columns** = `name`,
   **Values** = Sum of `exec_time_s`. Each row is one GPUFrame; the `GPUFrame` column is the
   frame length (the GPUFrame row references itself), the other columns are the summed time of
   each pass in that frame.
4. Label the columns via `VLOOKUP` into the `.dict` sheet, add a "% of frame" block dividing by
   the `GPUFrame` column, and an Average row.

Caveat: `-f @gpu` exports every nesting level, so a parent pass and its sub-passes are both
counted and the column sum exceeds the frame length. Either export one level by name
(`-F SSAO@gpu -F SSGI@gpu ...`) or sum only the passes you compare.

## Recipe: plots per frame in Excel

1. Export the plots of interest over the frames of interest:

   ```bat
   tracy-csvexport.exe -x plots.csv -f "memory@plots" -F "Memory/RAM@plots" -B 41400 -n 200 trace.tracy
   ```

2. Load `plots.csv.plots` and `plots.csv.dict` as two sheets.
3. Insert a pivot table: **Rows** = `frame`, **Columns** = `name`, **Values** = Average (or Max)
   of `value`. One row per frame, one column per plot — the same curves the profiler draws, now
   aggregated per frame.
4. Label the columns via `VLOOKUP` into the `.dict` sheet.

Plots that Tracy samples more often than once per frame (the allocation-tracking `Memory usage`
plot fires on every alloc and free) give many samples per frame, which is why the aggregation is
Average or Max rather than Sum; per-frame plots give exactly one row per frame.

## Building

Requires CMake >= 3.16, a C++20 compiler and Git (dependencies are fetched by CPM at configure
time). On Windows with Visual Studio 2022:

```bat
cmake -S csvexport -B csvexport\build -G "Visual Studio 17 2022" -A x64 -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build csvexport\build --config Release --target tracy-csvexport
```

The binary is `csvexport\build\Release\tracy-csvexport.exe`; `tracy-csvexport -V` prints the
Tracy version, the git revision it was built from and the Bagira release
(`tracy-csvexport 0.14.1 / <hash> / bagira.1`). Releases are tagged
`tracy-csvexport-<tracy version>-bagira.<n>`, with a `...-win64.zip` asset holding the exe and
this README. Traces saved by Tracy versions older than
the one the tool is built against may not load (the worker rejects them without a message).

## Where things live

- `csvexport/src/Export.hpp`, `Export.cpp` — the `-x` mode (filtering, collection, row layouts).
- `csvexport/src/Plots.hpp`, `Plots.cpp` — the `plots` scope and the `.plots` file.
- `csvexport/src/ExportCommon.hpp` — the dictionary, time window and frame numbering both share.
- `csvexport/src/csvexport.cpp` — upstream's tool plus the option parsing for `-x`.
- `docs/superpowers/specs/2026-09-09-csvexport-export-mode-design.md`,
  `docs/superpowers/specs/2026-09-10-csvexport-plots-scope-design.md` — design notes and decisions.
- `manual/tracy.tex` — the upstream manual, extended with a subsection on this mode.
