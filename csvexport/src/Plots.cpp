#include <algorithm>
#include <cctype>
#include <stdio.h>
#include <string.h>
#include <vector>

#include "../../server/TracyWorker.hpp"
#include "Export.hpp"
#include "ExportCommon.hpp"
#include "Plots.hpp"

namespace
{

bool EqualsIgnoreCase( const char* a, const char* b )
{
    for( ; *a && *b; ++a, ++b )
    {
        if( std::tolower( (unsigned char)*a ) != std::tolower( (unsigned char)*b ) ) return false;
    }
    return *a == *b;
}

// As Export.cpp matches zone names: an empty term name matches every plot.
bool PlotNameMatches( const FilterTerm& term, const char* name, bool caseSensitive )
{
    if( term.name.empty() ) return true;
    if( !term.exact ) return IsSubstring( term.name.c_str(), name, caseSensitive );
    return caseSensitive ? term.name == name : EqualsIgnoreCase( term.name.c_str(), name );
}

// The lane header the profiler shows, without the icons; see TimelineItemPlot::HeaderLabel.
// The built-in memory and CPU plots carry no name of their own, so GetString would yield "???".
std::string PlotDisplayName( const tracy::Worker& worker, const tracy::PlotData& plot )
{
    switch( plot.type )
    {
    case tracy::PlotType::Memory:
        return plot.name == 0 ? "Memory usage" : worker.GetString( plot.name );
    case tracy::PlotType::SysTime:
        return "CPU usage";
    default:
        return worker.GetString( plot.name );
    }
}

// Units of the value column; the one property of a plot that cannot be recomputed from its samples.
const char* FormatName( tracy::PlotValueFormatting format )
{
    switch( format )
    {
    case tracy::PlotValueFormatting::Memory: return "Memory";
    case tracy::PlotValueFormatting::Percentage: return "Percentage";
    case tracy::PlotValueFormatting::Watt: return "Watt";
    default: return "Number";
    }
}

// Frame number of the main frame set containing a timestamp, in the numbering the profiler shows.
class FrameLookup
{
public:
    explicit FrameLookup( const tracy::Worker& worker )
    {
        const auto fd = worker.GetFramesBase();
        if( !fd ) return;
        const auto count = worker.GetFrameCount( *fd );
        if( count == 0 ) return;
        const auto base = int64_t( FrameNumberBase( worker, *fd ) );
        // On-demand traces open with placeholder frames from before the connection; the profiler
        // does not address those, and neither do the frame rows, so they are skipped here too.
        const auto first = worker.GetFirstTime();
        for( size_t i = 0; i < count; ++i )
        {
            const auto begin = worker.GetFrameBegin( *fd, i );
            if( begin < first ) continue;
            if( m_begins.empty() ) m_firstNumber = base + int64_t( i );
            m_begins.push_back( begin );
        }
        m_end = worker.GetFrameEnd( *fd, count - 1 );
    }

    int64_t Number( int64_t time ) const
    {
        if( m_begins.empty() || time < m_begins.front() || time >= m_end ) return -1;
        const auto it = std::upper_bound( m_begins.begin(), m_begins.end(), time );
        return m_firstNumber + int64_t( it - m_begins.begin() ) - 1;
    }

private:
    std::vector<int64_t> m_begins;
    int64_t m_firstNumber = 0;
    int64_t m_end = 0;
};

}

bool HasPlotsTerm( const ExportOptions& opts )
{
    for( const auto& term : opts.terms )
    {
        if( term.scope.kind == Scope::Kind::Plots ) return true;
    }
    return false;
}

std::vector<PlotSeries> CollectPlots( const tracy::Worker& worker, const ExportOptions& opts, const Window& window )
{
    std::vector<PlotSeries> out;
    if( !HasPlotsTerm( opts ) ) return out;

    const FrameLookup frames( worker );
    for( const auto plot : worker.GetPlots() )
    {
        const auto name = PlotDisplayName( worker, *plot );
        bool match = false;
        for( const auto& term : opts.terms )
        {
            if( term.scope.kind != Scope::Kind::Plots ) continue;
            if( PlotNameMatches( term, name.c_str(), opts.caseSensitive ) ) { match = true; break; }
        }
        if( !match ) continue;

        PlotSeries series;
        series.name = name;
        series.format = FormatName( plot->format );
        // Samples are stored sorted by time, so the window is a contiguous run and the points
        // come out ascending without a sort.
        for( const auto& item : plot->data )
        {
            const auto time = item.time.Val();
            if( !window.Contains( time ) ) continue;
            series.points.push_back( PlotPoint { time, item.val, frames.Number( time ) } );
        }
        out.push_back( std::move( series ) );
    }
    return out;
}

bool WritePlots( const char* path, const std::vector<PlotSeries>& series, Dictionary& dict, const ExportOptions& opts )
{
    FILE* f = fopen( path, "wb" );
    if( !f ) return false;

    const char* sep = opts.separator;
    fprintf( f, "name%sformat%s%s%svalue%sframe\n", sep, sep, opts.seconds ? "s_since_start" : "ns_since_start", sep, sep );

    // Dictionary indices are assigned as the rows are written, so they follow the chosen order
    // exactly as they do for the zone rows.
    auto writeRow = [&]( const PlotSeries& s, const PlotPoint& p )
    {
        // Interned in two statements: the evaluation order of arguments in one call is
        // unspecified, and it decides which of the two strings gets the lower index.
        const auto nameIdx = dict.Index( s.name.c_str() );
        const auto formatIdx = dict.Index( s.format );
        fprintf( f, "%u%s%u%s", nameIdx, sep, formatIdx, sep );
        if( opts.seconds )
        {
            fprintf( f, "%.9f", p.time / 1e9 );
        }
        else
        {
            fprintf( f, "%lld", (long long)p.time );
        }
        // %.15g keeps integral values integral and still round-trips fractions.
        fprintf( f, "%s%.15g%s", sep, p.value, sep );
        if( p.frame >= 0 ) fprintf( f, "%lld", (long long)p.frame );
        fputc( '\n', f );
    };

    if( opts.order == RowOrder::Interleaved )
    {
        // One time-ordered stream of every plot, the counterpart of the interleaved row order.
        // Each plot's own samples are already ascending, so a stable sort by time keeps samples
        // sharing a timestamp in worker order.
        struct Ref { int64_t time; uint32_t series; uint32_t point; };
        std::vector<Ref> refs;
        size_t total = 0;
        for( const auto& s : series ) total += s.points.size();
        refs.reserve( total );
        for( uint32_t i = 0; i < series.size(); ++i )
        {
            const auto& points = series[i].points;
            for( uint32_t j = 0; j < points.size(); ++j ) refs.push_back( Ref { points[j].time, i, j } );
        }
        std::stable_sort( refs.begin(), refs.end(), []( const Ref& a, const Ref& b ) { return a.time < b.time; } );
        for( const auto& r : refs ) writeRow( series[r.series], series[r.series].points[r.point] );
    }
    else
    {
        // sequential and columns: one block per plot, samples ascending. A column group per plot
        // would pair unrelated samples in a row, so the flat layout is kept.
        for( const auto& s : series )
        {
            for( const auto& p : s.points ) writeRow( s, p );
        }
    }
    fclose( f );
    return true;
}
