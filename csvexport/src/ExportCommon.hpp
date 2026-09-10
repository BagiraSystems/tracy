#ifndef __TRACY_CSVEXPORT_EXPORTCOMMON_HPP__
#define __TRACY_CSVEXPORT_EXPORTCOMMON_HPP__

// Pieces shared by the row export (Export.cpp) and the plot export (Plots.cpp): the string
// dictionary both write into, the time window both filter by, and the frame numbering both use.

#include <stdint.h>
#include <stdio.h>
#include <string>
#include <unordered_map>
#include <vector>

#include "Export.hpp"

namespace tracy { class Worker; struct FrameData; }

// Strings may contain the separator, quotes or newlines, so they are CSV-quoted when written.
void WriteQuoted( FILE* f, const char* s );

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

// Half-open [begin, end) time range selected by the -b/-l or -B/-n/-E options.
struct Window
{
    int64_t begin = INT64_MIN;
    int64_t end = INT64_MAX;

    bool Contains( int64_t time ) const { return time >= begin && time < end; }
};

// Fills out from the window options; on failure returns false with a message in err.
bool ResolveWindow( const tracy::Worker& worker, const ExportOptions& opts, Window& out, std::string& err );

// Frame number of frame index i in the main frame set, as the profiler's View::GetFrameNumber.
uint64_t FrameNumberBase( const tracy::Worker& worker, const tracy::FrameData& fd );

#endif
