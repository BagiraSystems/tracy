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
enum class RowKind { Cpu, Gpu, Frame };

struct Row
{
    const char* name;
    const char* file;       // nullptr = no source location (frames)
    uint32_t line;
    int64_t start;
    int64_t duration;
    int64_t threadId;       // OS thread id (CPU) or GPU context index (GPU); -1 = none (frames)
    const char* threadName; // nullptr when threadId is -1
    const char* value;      // nullptr = no zone text
    int64_t frame;          // frame number for RowKind::Frame, -1 otherwise
    RowKind kind;
    int64_t parentStart;    // start of the enclosing -P zone, kNoParent if none
};

constexpr int64_t kNoParent = INT64_MIN;

// Threads and GPU contexts referenced by the exported rows, written as the .threads file.
// CPU threads and GPU contexts have separate id spaces, so the key includes the gpu flag.
class ThreadTable
{
public:
    void Add( const Row& row )
    {
        if( row.threadId < 0 ) return;
        m_names.emplace( std::make_pair( row.kind == RowKind::Gpu, row.threadId ), row.threadName );
    }

    bool Write( const char* path, const char* sep ) const
    {
        FILE* f = fopen( path, "wb" );
        if( !f ) return false;
        fprintf( f, "id%sgpu%sname\n", sep, sep );
        for( const auto& t : m_names )
        {
            fprintf( f, "%lld%s%d%s", (long long)t.first.second, sep, t.first.first ? 1 : 0, sep );
            WriteQuoted( f, t.second );
            fputc( '\n', f );
        }
        fclose( f );
        return true;
    }

private:
    std::map<std::pair<bool, int64_t>, const char*> m_names;
};

const char* GetZoneName( const tracy::Worker& worker, int16_t srcloc )
{
    const auto& sl = worker.GetSourceLocation( srcloc );
    return worker.GetString( sl.name.active ? sl.name : sl.function );
}

bool NameMatches( const FilterTerm& term, const char* name, bool caseSensitive )
{
    if( term.name.empty() ) return true;
    if( !term.exact ) return IsSubstring( term.name.c_str(), name, caseSensitive );
    return caseSensitive ? term.name == name : EqualsIgnoreCase( term.name.c_str(), name );
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
        case Scope::Kind::Frames:
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

bool AnyFramesScope( const std::vector<const FilterTerm*>& terms )
{
    for( auto term : terms )
    {
        if( term->scope.kind == Scope::Kind::Frames ) return true;
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

// Frame number of frame index i in the main frame set, as the profiler's View::GetFrameNumber.
uint64_t FrameNumberBase( const tracy::Worker& worker, const tracy::FrameData& fd )
{
    if( &fd != worker.GetFramesBase() ) return 1;
    const auto offset = worker.GetFrameOffset();
    return offset == 0 ? 0 : offset - 1;
}

// Half-open [begin, end) time range selected by the -b/-l or -B/-n/-E options.
struct Window
{
    int64_t begin = INT64_MIN;
    int64_t end = INT64_MAX;
};

// Returns false with a message in err when a frame number is outside the trace.
bool ResolveWindow( const tracy::Worker& worker, const ExportOptions& opts, Window& out, std::string& err )
{
    const auto first = worker.GetFirstTime();
    if( opts.beginFrame < 0 && opts.frameCount < 0 && opts.endFrame < 0 )
    {
        // GPU timestamps may precede the first CPU event, so only an explicit -b bounds the start.
        if( opts.beginSec > 0 ) out.begin = first + int64_t( opts.beginSec * 1e9 );
        if( opts.lengthSec >= 0 ) out.end = first + int64_t( ( opts.beginSec + opts.lengthSec ) * 1e9 );
        return true;
    }

    const auto& fd = *worker.GetFramesBase();
    const auto count = int64_t( worker.GetFrameCount( fd ) );
    const auto base = int64_t( FrameNumberBase( worker, fd ) );
    // Placeholder frames before the trace start are not addressable, as in the profiler.
    int64_t firstIdx = 0;
    while( firstIdx < count && worker.GetFrameBegin( fd, firstIdx ) < first ) ++firstIdx;
    if( firstIdx >= count )
    {
        err = "the trace has no frames";
        return false;
    }

    auto checkFrame = [&]( int64_t number, const char* what ) -> bool
    {
        const auto idx = number - base;
        if( idx < firstIdx || idx >= count )
        {
            err = std::string( what ) + " " + std::to_string( number ) + " is outside the trace (frames " + std::to_string( firstIdx + base ) + " to " + std::to_string( count - 1 + base ) + ")";
            return false;
        }
        return true;
    };

    const auto beginIdx = opts.beginFrame >= 0 ? opts.beginFrame - base : firstIdx;
    if( opts.beginFrame >= 0 && !checkFrame( opts.beginFrame, "begin frame" ) ) return false;
    out.begin = worker.GetFrameBegin( fd, beginIdx );

    if( opts.endFrame >= 0 )
    {
        if( !checkFrame( opts.endFrame, "end frame" ) ) return false;
        if( opts.endFrame - base < beginIdx )
        {
            err = "end frame precedes begin frame";
            return false;
        }
        out.end = worker.GetFrameEnd( fd, opts.endFrame - base );
    }
    else if( opts.frameCount >= 0 )
    {
        // A window past the last frame simply ends where the trace ends.
        const auto lastIdx = std::min( beginIdx + opts.frameCount - 1, count - 1 );
        out.end = opts.frameCount == 0 ? out.begin : worker.GetFrameEnd( fd, lastIdx );
    }
    return true;
}

// Gathers the matching zone occurrences from the worker into Row records.
class Collector
{
public:
    Collector( const tracy::Worker& worker, const ExportOptions& opts, const Window& window )
        : m_worker( worker ), m_opts( opts ), m_windowBegin( window.begin ), m_windowEnd( window.end )
    {
    }

    // The rows point into this collector's string pool, so it must outlive their use.
    std::vector<Row>& Run()
    {
        CollectCpu();
        CollectGpu();
        CollectFrames();
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

                const auto tid = m_worker.DecompressThread( ztd.Thread() );
                Push( slz.first, zone.Start(), duration, int64_t( tid ), m_worker.GetThreadName( tid ), RowKind::Cpu, text, kNoParent );
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
            // Pooled so Row::threadName can point at it; the worker owns named contexts, we own the fallbacks.
            const char* ctxName;
            if( ctx->name.Active() )
            {
                ctxName = m_worker.GetString( ctx->name );
            }
            else
            {
                ctxName = Pool( "GPU context " + std::to_string( ctxIdx ) );
            }
            for( const auto& td : ctx->threadData )
            {
                CollectGpuTimeline( ctxIdx, ctxName, td.second.timeline, kNoParent );
            }
            ++ctxIdx;
        }
    }

    // parentStart is the start of the nearest enclosing zone named by -P above this timeline.
    void CollectGpuTimeline( int ctxIdx, const char* ctxName, const tracy::Vector<tracy::short_ptr<tracy::GpuEvent>>& timeline, int64_t parentStart )
    {
        auto visit = [&]( const tracy::GpuEvent& ev )
        {
            const char* name = GetZoneName( m_worker, ev.SrcLoc() );
            // Children of a -P parent follow the parent's window membership so a parent is
            // exported either with all of its children or not at all.
            const auto windowTime = parentStart != kNoParent ? parentStart : ev.GpuStart();
            // Negative timestamps mean the GPU never reported this zone; the worker skips those too.
            if( ev.GpuStart() >= 0 && ev.GpuEnd() >= 0 && InWindow( windowTime ) )
            {
                const auto terms = TermsForName( m_opts, name );
                if( !terms.empty() && AnyGpuScope( terms ) )
                {
                    // A parent zone references itself, so one pivot key covers the parent's own
                    // duration and its children's sums.
                    const auto rowParent = IsParent( name ) ? ev.GpuStart() : parentStart;
                    Push( ev.SrcLoc(), ev.GpuStart(), ev.GpuEnd() - ev.GpuStart(), ctxIdx, ctxName, RowKind::Gpu, nullptr, rowParent );
                }
            }
            if( ev.Child() >= 0 )
            {
                const auto childParent = IsParent( name ) && ev.GpuStart() >= 0 ? ev.GpuStart() : parentStart;
                CollectGpuTimeline( ctxIdx, ctxName, m_worker.GetGpuChildren( ev.Child() ), childParent );
            }
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

    // One row per frame of every matching frame set, numbered as the profiler displays them;
    // frame length is the distance to the next frame's begin.
    void CollectFrames()
    {
        bool anyFrames = false;
        for( const auto& term : m_opts.terms ) anyFrames |= term.scope.kind == Scope::Kind::Frames;
        if( !anyFrames ) return;

        for( const auto fd : m_worker.GetFrames() )
        {
            const char* name = m_worker.GetString( fd->name );
            const auto terms = TermsForName( m_opts, name );
            if( terms.empty() || !AnyFramesScope( terms ) ) continue;

            const uint64_t numberBase = FrameNumberBase( m_worker, *fd );
            // On-demand traces start with placeholder frames from before the connection; the
            // profiler's timeline (GetFirstTime) begins after them, so they are skipped here too.
            const auto firstTime = m_worker.GetFirstTime();
            const auto count = m_worker.GetFrameCount( *fd );
            for( size_t i = 0; i < count; ++i )
            {
                const auto begin = m_worker.GetFrameBegin( *fd, i );
                if( begin < firstTime || !InWindow( begin ) ) continue;
                m_rows.push_back( Row { name, nullptr, 0, begin, m_worker.GetFrameTime( *fd, i ), -1, nullptr, nullptr, int64_t( numberBase + i ), RowKind::Frame, kNoParent } );
            }
        }
    }

    void Push( int16_t srcloc, int64_t start, int64_t duration, int64_t threadId, const char* threadName, RowKind kind, const char* value, int64_t parentStart )
    {
        const auto& sl = m_worker.GetSourceLocation( srcloc );
        m_rows.push_back( Row { GetZoneName( m_worker, srcloc ), m_worker.GetString( sl.file ), sl.line, start, duration, threadId, threadName, value, -1, kind, parentStart } );
    }

    bool IsParent( const char* name ) const
    {
        if( m_opts.parentName.empty() ) return false;
        return m_opts.caseSensitive ? m_opts.parentName == name : EqualsIgnoreCase( m_opts.parentName.c_str(), name );
    }

    // Strings not owned by the worker live here so Row pointers stay valid until the export ends.
    const char* Pool( std::string s )
    {
        m_pool.push_back( std::move( s ) );
        return m_pool.back().c_str();
    }

    const tracy::Worker& m_worker;
    const ExportOptions& m_opts;
    int64_t m_windowBegin;
    int64_t m_windowEnd;
    std::deque<std::string> m_pool;
    std::vector<Row> m_rows;
};

bool ByNameThenStart( const Row& a, const Row& b )
{
    const int c = strcmp( a.name, b.name );
    if( c != 0 ) return c < 0;
    if( a.kind != b.kind ) return a.kind < b.kind;
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
    Writer( FILE* f, const ExportOptions& opts, Dictionary& dict, ThreadTable& threads )
        : m_f( f ), m_opts( opts ), m_dict( dict ), m_threads( threads ), m_sep( opts.separator )
    {
        for( const auto& term : opts.terms ) m_hasFrames |= term.scope.kind == Scope::Kind::Frames;
        m_hasParent = !opts.parentName.empty();
    }

    // The frame column exists only when frame sets were requested, so zone-only exports keep
    // their schema.
    void WriteFlat( const std::vector<Row>& rows )
    {
        fprintf( m_f, "name%s", m_sep );
        if( !m_opts.noLocation ) fprintf( m_f, "src_file%ssrc_line%s", m_sep, m_sep );
        fprintf( m_f, "%s%s%s%sthread%sgpu%svalue", StartHeader(), m_sep, DurationHeader(), m_sep, m_sep, m_sep );
        if( m_hasFrames ) fprintf( m_f, "%sframe", m_sep );
        if( m_hasParent ) fprintf( m_f, "%sparent_%s", m_sep, StartHeader() );
        fputc( '\n', m_f );

        for( const auto& row : rows )
        {
            fprintf( m_f, "%u%s", m_dict.Index( row.name ), m_sep );
            if( !m_opts.noLocation ) WriteLocation( row );
            WriteTime( row.start );
            fputs( m_sep, m_f );
            WriteTime( row.duration );
            fputs( m_sep, m_f );
            WriteThread( row );
            fprintf( m_f, "%s%d%s", m_sep, row.kind == RowKind::Gpu ? 1 : 0, m_sep );
            WriteIndex( row.value );
            if( m_hasFrames )
            {
                fputs( m_sep, m_f );
                WriteFrame( row );
            }
            if( m_hasParent )
            {
                fputs( m_sep, m_f );
                WriteParent( row );
            }
            fputc( '\n', m_f );
        }
    }

    // rows must already be sorted by name, kind, start.
    void WriteColumns( const std::vector<Row>& rows )
    {
        struct Group { std::string key; size_t begin, end; };
        std::vector<Group> groups;
        for( size_t i = 0; i < rows.size(); )
        {
            size_t j = i;
            while( j < rows.size() && strcmp( rows[j].name, rows[i].name ) == 0 && rows[j].kind == rows[i].kind ) ++j;
            groups.push_back( Group { rows[i].name, i, j } );
            i = j;
        }
        // Groups of another kind sharing a name with a preceding group get a distinguishing suffix.
        for( size_t g = 1; g < groups.size(); ++g )
        {
            if( groups[g].key != groups[g-1].key ) continue;
            groups[g].key += rows[groups[g].begin].kind == RowKind::Gpu ? "@gpu" : "@frames";
        }

        bool first = true;
        for( const auto& g : groups )
        {
            // Frame groups carry the frame number where zone groups carry the zone text.
            const bool frames = rows[g.begin].kind == RowKind::Frame;
            const std::string parentCol = std::string( "parent_" ) + StartHeader();
            const char* cols[] = { "src_file", "src_line", StartHeader(), DurationHeader(), "thread", frames ? "frame" : "value", parentCol.c_str() };
            for( size_t c = m_opts.noLocation ? 2 : 0; c < ( m_hasParent ? 7 : 6 ); ++c )
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
                const size_t cells = ( m_opts.noLocation ? 4 : 6 ) + ( m_hasParent ? 1 : 0 );
                if( !first ) fputs( m_sep, m_f );
                first = false;
                if( g.begin + k >= g.end )
                {
                    for( size_t c = 1; c < cells; ++c ) fputs( m_sep, m_f );
                    continue;
                }
                const auto& row = rows[g.begin + k];
                if( !m_opts.noLocation ) WriteLocation( row );
                WriteTime( row.start );
                fputs( m_sep, m_f );
                WriteTime( row.duration );
                fputs( m_sep, m_f );
                WriteThread( row );
                fputs( m_sep, m_f );
                if( row.kind == RowKind::Frame )
                {
                    WriteFrame( row );
                }
                else
                {
                    WriteIndex( row.value );
                }
                if( m_hasParent )
                {
                    fputs( m_sep, m_f );
                    WriteParent( row );
                }
            }
            fputc( '\n', m_f );
        }
    }

private:
    // Thread id (or GPU context index) as a plain number, registered for the .threads file;
    // empty cell for rows without a thread.
    void WriteThread( const Row& row )
    {
        if( row.threadId < 0 ) return;
        fprintf( m_f, "%lld", (long long)row.threadId );
        m_threads.Add( row );
    }

    // Start of the enclosing -P zone in the time units of the start column; empty when none.
    void WriteParent( const Row& row )
    {
        if( row.parentStart != kNoParent ) WriteTime( row.parentStart );
    }

    // Frame number as a plain number, or an empty cell for zone rows.
    void WriteFrame( const Row& row )
    {
        if( row.frame >= 0 ) fprintf( m_f, "%lld", (long long)row.frame );
    }

    // "src_file<sep>src_line<sep>"; both cells empty when the row has no source location.
    void WriteLocation( const Row& row )
    {
        if( row.file )
        {
            fprintf( m_f, "%u%s%u%s", m_dict.Index( row.file ), m_sep, row.line, m_sep );
        }
        else
        {
            fprintf( m_f, "%s%s", m_sep, m_sep );
        }
    }

    // Dictionary index of s, or an empty cell for nullptr.
    void WriteIndex( const char* s )
    {
        if( s ) fprintf( m_f, "%u", m_dict.Index( s ) );
    }

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

    FILE* m_f;
    const ExportOptions& m_opts;
    Dictionary& m_dict;
    ThreadTable& m_threads;
    const char* m_sep;
    bool m_hasFrames = false;
    bool m_hasParent = false;
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
    else if( EqualsIgnoreCase( spec, "frames" ) )
    {
        scope.kind = Scope::Kind::Frames;
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

FilterTerm ParseFilterTerm( const char* term, const Scope& defaultScope, bool exact )
{
    FilterTerm out;
    out.exact = exact;
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
    Window window;
    std::string err;
    if( !ResolveWindow( worker, opts, window, err ) )
    {
        fprintf( stderr, "%s\n", err.c_str() );
        return 1;
    }

    Collector collector( worker, opts, window );
    auto& rows = collector.Run();

    if( opts.zeroShift && !rows.empty() )
    {
        int64_t minStart = INT64_MAX;
        for( const auto& row : rows ) minStart = std::min( minStart, row.start );
        for( auto& row : rows )
        {
            row.start -= minStart;
            if( row.parentStart != kNoParent ) row.parentStart -= minStart;
        }
    }

    std::stable_sort( rows.begin(), rows.end(), opts.order == RowOrder::Interleaved ? ByStart : ByNameThenStart );

    FILE* f = fopen( opts.outputPath, "wb" );
    if( !f )
    {
        fprintf( stderr, "Could not open output file %s\n", opts.outputPath );
        return 1;
    }

    Dictionary dict;
    ThreadTable threads;
    Writer writer( f, opts, dict, threads );
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
    const std::string threadsPath = std::string( opts.outputPath ) + ".threads";
    if( !threads.Write( threadsPath.c_str(), opts.separator ) )
    {
        fprintf( stderr, "Could not open threads file %s\n", threadsPath.c_str() );
        return 1;
    }
    return 0;
}
