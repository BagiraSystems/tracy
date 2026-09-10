# tracy-csvexport: plots scope for the `-x` mode

Date: 2026-09-10

## Goal

Export Tracy's plot lanes (`TracyPlot`, the built-in memory and CPU usage plots) from the `-x`
mode, so the charts visible in the profiler can be analysed per frame in Excel or pandas. Today
plots are reachable only through upstream's `-u -p`, which prints every unwrapped zone event
alongside them, uses no dictionary, and offers no window or frame attribution.

## CLI

```
-f @plots                  every plot
-f "memory@plots"          plots whose display name contains "memory" (-c for case-sensitive)
-F "Memory/RAM@plots"      plots whose display name is exactly that
-T plots                   default scope for terms without @
```

`plots` is a new `Scope::Kind`, accepted by `ParseScope` like `frames` and `messages`. It is not
part of the `all` scope: without an explicit plots term no plot is collected and no `.plots` file
is written, so existing command lines produce byte-identical output. Terms are OR'd with all other
terms, so one run can export frames, zones and plots together. `-c` applies to plot name matching.

Names are matched against the plot's *display* name (see *Plot names*), i.e. what the profiler's
lane header shows.

## Output

### `<file>.plots`

```
name,format,ns_since_start,value,frame
7,12,737591042755,23345,41370
```

| Column | Content |
|---|---|
| `name` | Dictionary index of the plot's display name. |
| `format` | Dictionary index of `Number`, `Memory`, `Percentage` or `Watt` (Tracy's `PlotValueFormatting`) - the units of `value`, which cannot be recomputed from the samples. |
| `ns_since_start` / `s_since_start` | Sample time, same units and zero base as the main CSV (`-S`, `-z`). |
| `value` | The plot value, `%.15g`: integral values print plainly, fractions keep precision. |
| `frame` | Frame number of the main frame set containing the sample, profiler numbering; empty when the sample lies outside every real frame. |

One row per sample, plots in worker order, samples ascending in time - that is already the source
order, so no sort pass. Every cell is numeric, so no quoting is needed. The file is written only
when a plots term is present; `<file>`, `<file>.dict` and `<file>.threads` are written as before
(a plots-only run leaves a header-only CSV, as any run that matches nothing does).

`name` and `format` share the one dictionary index space of `<file>.dict` with the main CSV.
Plots are written after the main CSV so that the indices of an existing export do not shift.

### Frame attribution

Binary search over the main frame set: frame *f* with `begin(f) <= t < begin(f+1)`, the last frame
bounded by its end. Placeholder frames from before the trace start are skipped, as `CollectFrames`
and the profiler do. This makes per-frame plot statistics a pivot (rows = `frame`,
columns = `name`, values = average or max of `value`) with no external join.

## Semantics of the existing options

- `-b/-l`, `-B/-n/-E`: a sample is kept when its timestamp is in the window, the same
  membership-by-start rule the rows use.
- `-z`: the zero base is the minimum over the exported rows **and** the exported plot samples, so
  both files share a timebase. A plot sample earlier than every zone therefore moves the base.
- `-S`: the plot time column becomes `s_since_start` with 9 decimals.
- `-L`, `-e`, `-P`, `-o`: no effect on `.plots`. It has no location, duration or parent, and one
  fixed layout.

## Plot names

`CollectPlots` resolves the display name as `TimelineItemPlot::HeaderLabel` does, without the
icons: the plot's string for `User` and `Power` plots, `Memory usage` for the unnamed default
memory pool, the pool's name for a named one, and `CPU usage` for the `SysTime` plot. This replaces
the `???` that `GetString(0)` returns for the built-in plots. Upstream's `-u -p` keeps printing
`???`; the classic modes stay byte-identical to upstream.

## Code

New `src/Plots.{hpp,cpp}`:

```cpp
struct PlotPoint  { int64_t time; double value; int64_t frame; };
struct PlotSeries { std::string name; const char* format; std::vector<PlotPoint> points; };

std::vector<PlotSeries> CollectPlots( const tracy::Worker&, const ExportOptions&, const Window& );
bool WritePlots( const char* path, const std::vector<PlotSeries>&, Dictionary&, const ExportOptions& );
```

Series of points rather than flat samples: the name is stored once per plot instead of once per
sample (a 900k-sample trace is common), and the grouping question disappears.

`Dictionary`, `Window` / `ResolveWindow` and `FrameNumberBase` move from `Export.cpp`'s anonymous
namespace to a new internal header `src/ExportCommon.hpp`, shared by both translation units.
`RunExport` keeps the orchestration: resolve window, collect rows, collect plots, apply `-z` to
both, write the CSV, write `.plots`, write `.dict` and `.threads`.

## Errors

- Cannot open `<file>.plots` -> message to stderr, exit 1, as for the other companions.
- A plots term that matches no plot -> header-only `.plots`, exit 0.

## Verification

On a real trace (`t.tracy`, 2808 frames 41369..44176, 11 plots, 884419 samples), against an
oracle built from `-u -p` joined to a `-F Frame@frames` export by timestamp:

- `-f @plots`: sample count per plot equals the `-u -p` count per plot (`Frame` 824290,
  `Memory/RAM` 2807, `Animation.BanAllocator` 25, ...); decoded `name` and `frame` equal the
  oracle row for row.
- `-F "Memory/RAM@plots"`: 2807 rows, one plot name.
- `-B 41400 -n 200 -f @plots`: no sample outside frames 41400..41599.
- `-S -z -f @plots -F Frame@frames`: seconds agree with the ns run after scaling and the zero base
  is shared with the zone rows of the same run.
- `-s ";"`: the new file honours the separator.
- A run without a plots term produces no `.plots` file and a CSV identical to before the change.

## Docs

`-x` section of `csvexport/README.md` (scope table, output files, a per-frame memory recipe) and
the csvexport subsection of `manual/tracy.tex`.
