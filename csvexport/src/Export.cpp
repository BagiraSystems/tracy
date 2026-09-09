#include <algorithm>
#include <cctype>
#include <deque>
#include <map>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../server/TracyWorker.hpp"
#include "Export.hpp"

namespace
{

std::string ToLower( const char* s )
{
    std::string out( s );
    std::transform( out.begin(), out.end(), out.begin(), []( unsigned char c ) { return std::tolower( c ); } );
    return out;
}

bool EqualsIgnoreCase( const char* a, const char* b )
{
    return ToLower( a ) == ToLower( b );
}

bool IsAllDigits( const char* s )
{
    if( *s == '\0' ) return false;
    for( ; *s; ++s )
    {
        if( !std::isdigit( (unsigned char)*s ) ) return false;
    }
    return true;
}

// Strings may contain the separator, quotes or newlines, so they are CSV-quoted when written.
void WriteQuoted( FILE* f, const char* s )
{
    fputc( '"', f );
    for( ; *s; ++s )
    {
        if( *s == '"' ) fputc( '"', f );
        fputc( *s, f );
    }
    fputc( '"', f );
}

// Assigns each distinct string an index in first-seen order and writes them out as the .dict file.
class Dictionary
{
public:
    uint32_t Index( const char* s )
    {
        auto it = m_index.find( s );
        if( it != m_index.end() ) return it->second;
        const auto idx = uint32_t( m_strings.size() );
        m_strings.emplace_back( s );
        m_index.emplace( m_strings.back(), idx );
        return idx;
    }

    bool Write( const char* path, const char* sep ) const
    {
        FILE* f = fopen( path, "wb" );
        if( !f ) return false;
        fprintf( f, "index%sstring\n", sep );
        for( size_t i = 0; i < m_strings.size(); ++i )
        {
            fprintf( f, "%zu%s", i, sep );
            WriteQuoted( f, m_strings[i].c_str() );
            fputc( '\n', f );
        }
        fclose( f );
        return true;
    }

private:
    std::vector<std::string> m_strings;
    std::unordered_map<std::string, uint32_t> m_index;
};

// One exported zone occurrence. String pointers refer to worker-owned storage (or the
// context-name pool in Collector), so they stay valid until the export finishes.
struct Row
{
    const char* name;
    const char* file;
    uint32_t line;
    int64_t start;
    int64_t duration;
    const char* thread;
    const char* value;      // nullptr = no zone text
    bool gpu;
};

const char* GetZoneName( const tracy::Worker& worker, int16_t srcloc )
{
    const auto& sl = worker.GetSourceLocation( srcloc );
    return worker.GetString( sl.name.active ? sl.name : sl.function );
}

bool NameMatches( const FilterTerm& term, const char* name, bool caseSensitive )
{
    return term.name.empty() || IsSubstring( term.name.c_str(), name, caseSensitive );
}

bool ThreadMatches( const Scope& scope, const tracy::Worker& worker, uint16_t threadIdx, bool caseSensitive )
{
    const auto tid = worker.DecompressThread( threadIdx );
    if( scope.byId ) return tid == scope.threadId;
    return IsSubstring( scope.threadName.c_str(), worker.GetThreadName( tid ), caseSensitive );
}

// Terms whose name part matches; the scope part is checked per occurrence.
std::vector<const FilterTerm*> TermsForName( const ExportOptions& opts, const char* name )
{
    std::vector<const FilterTerm*> out;
    for( const auto& term : opts.terms )
    {
        if( NameMatches( term, name, opts.caseSensitive ) ) out.push_back( &term );
    }
    return out;
}

bool AnyCpuScope( const std::vector<const FilterTerm*>& terms, const tracy::Worker& worker, uint16_t threadIdx, bool caseSensitive )
{
    for( auto term : terms )
    {
        switch( term->scope.kind )
        {
        case Scope::Kind::All:
        case Scope::Kind::Cpu:
            return true;
        case Scope::Kind::Thread:
            if( ThreadMatches( term->scope, worker, threadIdx, caseSensitive ) ) return true;
            break;
        case Scope::Kind::Gpu:
            break;
        }
    }
    return false;
}

bool AnyGpuScope( const std::vector<const FilterTerm*>& terms )
{
    for( auto term : terms )
    {
        if( term->scope.kind == Scope::Kind::All || term->scope.kind == Scope::Kind::Gpu ) return true;
    }
    return false;
}

// From TracyView.cpp
int64_t GetZoneChildTimeFast( const tracy::Worker& worker, const tracy::ZoneEvent& zone )
{
    int64_t time = 0;
    if( zone.HasChildren() )
    {
        auto& children = worker.GetZoneChildren( zone.Child() );
        if( children.is_magic() )
        {
            auto& vec = *(tracy::Vector<tracy::ZoneEvent>*)&children;
            for( auto& v : vec ) time += v.End() - v.Start();
        }
        else
        {
            for( auto& v : children ) time += v->End() - v->Start();
        }
    }
    return time;
}

// Gathers the matching zone occurrences from the worker into Row records.
class Collector
{
public:
    Collector( const tracy::Worker& worker, const ExportOptions& opts )
        : m_worker( worker ), m_opts( opts )
    {
        // GPU timestamps may precede the first CPU event, so only an explicit -b bounds the start.
        const auto first = worker.GetFirstTime();
        m_windowBegin = opts.beginSec > 0 ? first + int64_t( opts.beginSec * 1e9 ) : INT64_MIN;
        m_windowEnd = opts.lengthSec < 0 ? INT64_MAX : first + int64_t( ( opts.beginSec + opts.lengthSec ) * 1e9 );
    }

    // The rows point into this collector's string pool, so it must outlive their use.
    std::vector<Row>& Run()
    {
        CollectCpu();
        CollectGpu();
        return m_rows;
    }

private:
    bool InWindow( int64_t start ) const
    {
        return start >= m_windowBegin && start < m_windowEnd;
    }

    void CollectCpu()
    {
        for( const auto& slz : m_worker.GetSourceLocationZones() )
        {
            if( slz.second.total == 0 ) continue;
            const auto terms = TermsForName( m_opts, GetZoneName( m_worker, slz.first ) );
            if( terms.empty() ) continue;

            for( const auto& ztd : slz.second.zones )
            {
                const auto& zone = *ztd.Zone();
                if( !InWindow( zone.Start() ) ) continue;
                if( !AnyCpuScope( terms, m_worker, ztd.Thread(), m_opts.caseSensitive ) ) continue;

                auto duration = zone.End() - zone.Start();
                if( m_opts.selfTime ) duration -= GetZoneChildTimeFast( m_worker, zone );

                const char* text = nullptr;
                if( m_worker.HasZoneExtra( zone ) )
                {
                    const auto& ref = m_worker.GetZoneExtra( zone ).text;
                    if( ref.Active() ) text = m_worker.GetString( ref );
                }

                const auto threadName = m_worker.GetThreadName( m_worker.DecompressThread( ztd.Thread() ) );
                Push( slz.first, zone.Start(), duration, threadName, false, text );
            }
        }
    }

    void CollectGpu()
    {
        bool anyGpu = false;
        for( const auto& term : m_opts.terms ) anyGpu |= term.scope.kind == Scope::Kind::All || term.scope.kind == Scope::Kind::Gpu;
        if( !anyGpu ) return;

        int ctxIdx = 0;
        for( const auto ctx : m_worker.GetGpuData() )
        {
            // Pooled so Row::thread can point at it; the worker owns named contexts, we own the fallbacks.
            const char* ctxName;
            if( ctx->name.Active() )
            {
                ctxName = m_worker.GetString( ctx->name );
            }
            else
            {
                m_ctxNames.push_back( "GPU context " + std::to_string( ctxIdx ) );
                ctxName = m_ctxNames.back().c_str();
            }
            ++ctxIdx;
            for( const auto& td : ctx->threadData )
            {
                CollectGpuTimeline( ctxName, td.second.timeline );
            }
        }
    }

    void CollectGpuTimeline( const char* ctxName, const tracy::Vector<tracy::short_ptr<tracy::GpuEvent>>& timeline )
    {
        auto visit = [&]( const tracy::GpuEvent& ev )
        {
            // Negative timestamps mean the GPU never reported this zone; the worker skips those too.
            if( ev.GpuStart() >= 0 && ev.GpuEnd() >= 0 && InWindow( ev.GpuStart() ) )
            {
                const auto terms = TermsForName( m_opts, GetZoneName( m_worker, ev.SrcLoc() ) );
                if( !terms.empty() && AnyGpuScope( terms ) )
                {
                    Push( ev.SrcLoc(), ev.GpuStart(), ev.GpuEnd() - ev.GpuStart(), ctxName, true, nullptr );
                }
            }
            if( ev.Child() >= 0 ) CollectGpuTimeline( ctxName, m_worker.GetGpuChildren( ev.Child() ) );
        };

        if( timeline.is_magic() )
        {
            for( const auto& ev : *(tracy::Vector<tracy::GpuEvent>*)&timeline ) visit( ev );
        }
        else
        {
            for( const auto& ev : timeline ) visit( *ev );
        }
    }

    void Push( int16_t srcloc, int64_t start, int64_t duration, const char* thread, bool gpu, const char* value )
    {
        const auto& sl = m_worker.GetSourceLocation( srcloc );
        m_rows.push_back( Row { GetZoneName( m_worker, srcloc ), m_worker.GetString( sl.file ), sl.line, start, duration, thread, value, gpu } );
    }

    const tracy::Worker& m_worker;
    const ExportOptions& m_opts;
    int64_t m_windowBegin;
    int64_t m_windowEnd;
    std::deque<std::string> m_ctxNames;
    std::vector<Row> m_rows;
};

bool ByNameThenStart( const Row& a, const Row& b )
{
    const int c = strcmp( a.name, b.name );
    if( c != 0 ) return c < 0;
    if( a.gpu != b.gpu ) return !a.gpu;
    return a.start < b.start;
}

bool ByStart( const Row& a, const Row& b )
{
    return a.start < b.start;
}

// Formats cells and writes the header and rows for all orderings.
class Writer
{
public:
    Writer( FILE* f, const ExportOptions& opts, Dictionary& dict )
        : m_f( f ), m_opts( opts ), m_dict( dict ), m_sep( opts.separator )
    {
    }

    void WriteFlat( const std::vector<Row>& rows )
    {
        fprintf( m_f, "name%s", m_sep );
        if( !m_opts.noLocation ) fprintf( m_f, "src_file%ssrc_line%s", m_sep, m_sep );
        fprintf( m_f, "%s%s%s%sthread%sgpu%svalue\n", StartHeader(), m_sep, DurationHeader(), m_sep, m_sep, m_sep );

        for( const auto& row : rows )
        {
            fprintf( m_f, "%u%s", m_dict.Index( row.name ), m_sep );
            if( !m_opts.noLocation ) fprintf( m_f, "%u%s%u%s", m_dict.Index( row.file ), m_sep, row.line, m_sep );
            WriteTime( row.start );
            fputs( m_sep, m_f );
            WriteTime( row.duration );
            fprintf( m_f, "%s%u%s%d%s", m_sep, m_dict.Index( row.thread ), m_sep, row.gpu ? 1 : 0, m_sep );
            WriteValue( row.value );
            fputc( '\n', m_f );
        }
    }

    // rows must already be sorted by name, gpu, start.
    void WriteColumns( const std::vector<Row>& rows )
    {
        struct Group { std::string key; size_t begin, end; };
        std::vector<Group> groups;
        for( size_t i = 0; i < rows.size(); )
        {
            size_t j = i;
            while( j < rows.size() && strcmp( rows[j].name, rows[i].name ) == 0 && rows[j].gpu == rows[i].gpu ) ++j;
            groups.push_back( Group { rows[i].name, i, j } );
            i = j;
        }
        // A GPU zone sharing its name with a CPU zone gets a distinguishing suffix.
        for( size_t g = 1; g < groups.size(); ++g )
        {
            if( groups[g].key == groups[g-1].key ) groups[g].key += "@gpu";
        }

        bool first = true;
        for( const auto& g : groups )
        {
            const char* cols[] = { "src_file", "src_line", StartHeader(), DurationHeader(), "thread", "value" };
            for( size_t c = m_opts.noLocation ? 2 : 0; c < 6; ++c )
            {
                if( !first ) fputs( m_sep, m_f );
                first = false;
                WriteQuoted( m_f, ( g.key + "." + cols[c] ).c_str() );
            }
        }
        fputc( '\n', m_f );

        size_t maxRows = 0;
        for( const auto& g : groups ) maxRows = std::max( maxRows, g.end - g.begin );

        for( size_t k = 0; k < maxRows; ++k )
        {
            first = true;
            for( const auto& g : groups )
            {
                const size_t cells = m_opts.noLocation ? 4 : 6;
                if( !first ) fputs( m_sep, m_f );
                first = false;
                if( g.begin + k >= g.end )
                {
                    for( size_t c = 1; c < cells; ++c ) fputs( m_sep, m_f );
                    continue;
                }
                const auto& row = rows[g.begin + k];
                if( !m_opts.noLocation ) fprintf( m_f, "%u%s%u%s", m_dict.Index( row.file ), m_sep, row.line, m_sep );
                WriteTime( row.start );
                fputs( m_sep, m_f );
                WriteTime( row.duration );
                fprintf( m_f, "%s%u%s", m_sep, m_dict.Index( row.thread ), m_sep );
                WriteValue( row.value );
            }
            fputc( '\n', m_f );
        }
    }

private:
    const char* StartHeader() const { return m_opts.seconds ? "s_since_start" : "ns_since_start"; }
    const char* DurationHeader() const { return m_opts.seconds ? "exec_time_s" : "exec_time_ns"; }

    void WriteTime( int64_t ns )
    {
        if( m_opts.seconds )
        {
            fprintf( m_f, "%.9f", ns / 1e9 );
        }
        else
        {
            fprintf( m_f, "%lld", (long long)ns );
        }
    }

    void WriteValue( const char* value )
    {
        if( value ) fprintf( m_f, "%u", m_dict.Index( value ) );
    }

    FILE* m_f;
    const ExportOptions& m_opts;
    Dictionary& m_dict;
    const char* m_sep;
};

}

bool IsSubstring( const char* term, const char* s, bool caseSensitive )
{
    if( caseSensitive ) return std::string( s ).find( term ) != std::string::npos;
    return ToLower( s ).find( ToLower( term ) ) != std::string::npos;
}

Scope ParseScope( const char* spec )
{
    Scope scope;
    if( EqualsIgnoreCase( spec, "all" ) )
    {
        scope.kind = Scope::Kind::All;
    }
    else if( EqualsIgnoreCase( spec, "cpu" ) )
    {
        scope.kind = Scope::Kind::Cpu;
    }
    else if( EqualsIgnoreCase( spec, "gpu" ) )
    {
        scope.kind = Scope::Kind::Gpu;
    }
    else
    {
        scope.kind = Scope::Kind::Thread;
        if( IsAllDigits( spec ) )
        {
            scope.byId = true;
            scope.threadId = strtoull( spec, nullptr, 10 );
        }
        else
        {
            scope.threadName = spec;
        }
    }
    return scope;
}

FilterTerm ParseFilterTerm( const char* term, const Scope& defaultScope )
{
    FilterTerm out;
    const char* at = strchr( term, '@' );
    if( at )
    {
        out.name.assign( term, at - term );
        out.scope = ParseScope( at + 1 );
    }
    else
    {
        out.name = term;
        out.scope = defaultScope;
    }
    return out;
}

bool ParseRowOrder( const char* spec, RowOrder& out )
{
    if( EqualsIgnoreCase( spec, "sequential" ) ) { out = RowOrder::Sequential; return true; }
    if( EqualsIgnoreCase( spec, "interleaved" ) ) { out = RowOrder::Interleaved; return true; }
    if( EqualsIgnoreCase( spec, "columns" ) ) { out = RowOrder::Columns; return true; }
    return false;
}

int RunExport( const tracy::Worker& worker, const ExportOptions& opts )
{
    Collector collector( worker, opts );
    auto& rows = collector.Run();

    if( opts.zeroShift && !rows.empty() )
    {
        int64_t minStart = INT64_MAX;
        for( const auto& row : rows ) minStart = std::min( minStart, row.start );
        for( auto& row : rows ) row.start -= minStart;
    }

    std::stable_sort( rows.begin(), rows.end(), opts.order == RowOrder::Interleaved ? ByStart : ByNameThenStart );

    FILE* f = fopen( opts.outputPath, "wb" );
    if( !f )
    {
        fprintf( stderr, "Could not open output file %s\n", opts.outputPath );
        return 1;
    }

    Dictionary dict;
    Writer writer( f, opts, dict );
    if( opts.order == RowOrder::Columns )
    {
        writer.WriteColumns( rows );
    }
    else
    {
        writer.WriteFlat( rows );
    }
    fclose( f );

    const std::string dictPath = std::string( opts.outputPath ) + ".dict";
    if( !dict.Write( dictPath.c_str(), opts.separator ) )
    {
        fprintf( stderr, "Could not open dictionary file %s\n", dictPath.c_str() );
        return 1;
    }
    return 0;
}
