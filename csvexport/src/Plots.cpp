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

    for( const auto& s : series )
    {
        const auto nameIdx = dict.Index( s.name.c_str() );
        const auto formatIdx = dict.Index( s.format );
        for( const auto& p : s.points )
        {
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
        }
    }
    fclose( f );
    return true;
}
