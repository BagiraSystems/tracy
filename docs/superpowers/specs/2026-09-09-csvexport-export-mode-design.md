# tracy-csvexport: per-event export mode (`-x`)

Date: 2026-09-09

## Goal

Add a dedicated export mode to `tracy-csvexport` that writes per-event zone rows to a file with
all strings dictionary-encoded, optional omission of source location, and a filter that accepts
multiple event names, each with its own thread/GPU scope. Existing modes stay byte-identical.

## CLI

```
-x, --export <file>     New mode: per-event rows to <file>, dictionary to <file>.dict.
                        Mutually exclusive with -u, -g, -m, -p, -t.
-f NAME[@SCOPE]         Repeatable in -x mode; terms are OR'd. NAME may be empty
                        ("-f @gpu" = every GPU zone).
-F NAME[@SCOPE]         Like -f, but NAME must equal the whole zone name. Mixable with -f.
-T SCOPE                Default scope for terms without @, and for the no-filter case.
                        SCOPE is all | cpu | gpu | frames | <thread>. Default: all.
-L, --no-location       Drop src_file and src_line columns.
-c, -e, -s              Unchanged meaning; honoured in -x mode.
```

Outside `-x`, behaviour is unchanged. Using `-L`, `-T`, more than one `-f`, or a `-f` containing
`@` without `-x` is a usage error.

## Output

### CSV (`<file>`)

Columns: `name, src_file, src_line, ns_since_start, exec_time_ns, thread, gpu, value`.
With `-L`: `name, ns_since_start, exec_time_ns, thread, gpu, value`.

- `name`, `src_file`, `value` are dictionary indices.
- `src_line`, `ns_since_start`, `exec_time_ns`, `thread`, `gpu` (0/1) are plain numbers.
- Absent zone text -> empty `value` cell (not an index), so the column loads as nullable int.
- CPU rows first, grouped by source location in worker iteration order (as `-u`), then GPU rows.
- `thread` = OS thread id for CPU rows, GPU context index for GPU rows (separate id spaces; the
  `gpu` column tells which). Names are not unique (worker pools), hence ids. Referenced threads and
  contexts are listed in `<file>.threads` as `id<sep>gpu<sep>name` (name CSV-quoted), sorted by id.
- GPU rows: `ns_since_start = GpuStart`, `exec_time_ns = GpuEnd - GpuStart` (as `-g`).
- GPU zones whose timestamps were never reported by the GPU (`GpuStart`/`GpuEnd` negative) are
  skipped, as the worker's own statistics do.
- GPU rows come from walking the per-context timelines, so zero-duration GPU zones are included.
  `-g` omits them because the worker only enters zones with a positive time span into its GPU
  statistics map; filter on `exec_time_ns > 0` to get the `-g` row set.
- `-e` (self time) subtracts child time for CPU rows only.
- No quoting: the CSV never contains a string.

### Dictionary (`<file>.dict`)

Header `index<sep>string`. Indices assigned from 0 in first-seen order; one shared index space
for all string columns. Strings are CSV-quoted (`"..."`, embedded `"` doubled) so separators,
quotes and newlines in paths/texts survive. Written after the CSV is complete.

## Time window, units and ordering

```
-z, --zero              Shift start times so the earliest exported event starts at 0. Default off.
-b, --begin <sec>       Export only events starting at or after <sec> from the trace start
                        (the profiler's 0), measured on the event start. Default 0.
-l, --length <sec>      Window length in seconds, from --begin. Default unbounded.
-B, --begin-frame <n>   Window starts at the begin of frame n of the main frame set. n is the
                        frame number the profiler displays ("Frame 41369") and the frame column
                        prints - not a 0-based index; on-demand traces start at a high number.
                        Default: first real frame.
-n, --frames <n>        Window length in frames.
-E, --end-frame <n>     Last frame included; alternative to -n. The frame window (-B/-n/-E) is
                        exclusive with the time window (-b/-l); frame numbers outside the trace
                        are an error, a count running past the last frame ends at the trace end.
-S, --seconds           Emit times as floating-point seconds with 9 decimals; the columns are
                        then named s_since_start / exec_time_s. Default nanosecond integers.
-o, --order <mode>      sequential (name, then start; default) | interleaved (start) | columns.
```

- The window is applied to the event start only; an event that starts inside and ends outside
  the window is kept.
- `--zero` subtracts the earliest start among the exported rows (after the window filter).
- Rows are buffered in memory, filtered, shifted and sorted, then written; the dictionary is
  filled in row order at write time.

### `columns` order

One row per occurrence index: row k holds the k-th occurrence of every event type; a type that
has fewer occurrences leaves its cells empty. Event type = zone name, with `@gpu` appended when a
GPU zone shares its name with a CPU zone. Types are ordered by name; occurrences by start.

Per type the columns are `<type>.ns_since_start, <type>.exec_time_ns, <type>.thread, <type>.value`,
preceded by `<type>.src_file, <type>.src_line` unless `-L`. There is no `gpu` column. Header cells
use the readable type name (CSV-quoted when needed); data cells are dictionary indices / numbers
as in the other orders.

## Filtering

A term splits at the first `@` into NAME and SCOPE. Missing SCOPE -> the `-T` default.

- SCOPE keywords (case-insensitive): `all` (CPU on every thread + GPU), `cpu` (CPU only),
  `frames` (FrameMark frame sets: name = frame set name, one row per frame with begin time and
  length, empty location and thread cells, and a numeric `frame` column - present only when a
  `frames` term is used, replacing `value` for frame groups in `columns` order - holding the
  frame number as the profiler shows it),
  `gpu` (GPU only).
- Any other SCOPE is a thread spec: all digits -> exact OS thread id; otherwise substring match
  on the thread name, case-insensitive unless `-c`.
- `-f` NAME matches by substring, `-F` NAME by whole-name equality (both case-insensitive unless
  `-c`); empty NAME matches every name.
- An occurrence is exported if any term matches both its name and its scope; it is emitted once
  even when several terms match.
- No `-f` -> single implicit term with empty NAME and the `-T` scope.

## Code structure

- `csvexport/src/Export.hpp` / `Export.cpp` (added to `PROGRAM_FILES` in `csvexport/CMakeLists.txt`):
  - `FilterTerm` + `ParseFilterTerm(const char*, defaultScope)` - pure parsing.
  - `Dictionary` - string -> index in first-seen order; writes the `.dict` file.
  - `RunExport(const tracy::Worker&, const ExportOptions&)` - row generation and file output.
- `csvexport.cpp` gains only the new options, validation, and a dispatch call.

## Errors

- Cannot open `<file>` or `<file>.dict` -> message to stderr, exit 1.
- Conflicting/misplaced flags -> usage text, exit 1.
- Nothing matched -> header-only CSV and header-only `.dict`, exit 0.

## Verification

Build, then on a real trace:
- `-x -T cpu` vs `-u`: same row count; identical values after decoding through `.dict`.
- `-x -f @gpu` vs `-g`: same rows.
- A few `@<thread>` and mixed-scope runs; `-L`; a `-s ";"` run to check the `.dict` quoting.

## Docs

Short subsection for `-x` in the csvexport chapter of `manual/tracy.md`.
