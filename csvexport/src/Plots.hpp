#ifndef __TRACY_CSVEXPORT_PLOTS_HPP__
#define __TRACY_CSVEXPORT_PLOTS_HPP__

// The plots scope of the -x mode: Tracy's plot lanes (TracyPlot, the built-in memory and CPU
// usage plots) exported to a <file>.plots companion, one row per sample. Plot samples share
// nothing with the zone rows but the window, the time format and the string dictionary, so they
// live in their own file with their own layout.

#include <stdint.h>
#include <string>
#include <vector>

#include "Export.hpp"

namespace tracy { class Worker; }

class Dictionary;
struct Window;

struct PlotPoint
{
    int64_t time;
    double value;
    int64_t frame;      // frame number of the main frame set, -1 outside every frame
};

// One plot lane. The name is stored once per plot rather than once per sample; traces routinely
// hold hundreds of thousands of samples.
struct PlotSeries
{
    std::string name;
    const char* format;     // "Number", "Memory", "Percentage" or "Watt"
    std::vector<PlotPoint> points;
};

// True when any filter term selects plots; without one no plot is collected and no file written.
bool HasPlotsTerm( const ExportOptions& opts );

// Plots whose display name matches a plots-scoped term, with their samples inside the window,
// in worker order and ascending in time.
std::vector<PlotSeries> CollectPlots( const tracy::Worker& worker, const ExportOptions& opts, const Window& window );

// Writes the .plots file; names and formats are interned in dict. Returns false if path cannot
// be opened.
bool WritePlots( const char* path, const std::vector<PlotSeries>& series, Dictionary& dict, const ExportOptions& opts );

#endif
