#ifndef __TRACY_CSVEXPORT_EXPORT_HPP__
#define __TRACY_CSVEXPORT_EXPORT_HPP__

#include <stdint.h>
#include <string>
#include <vector>

namespace tracy { class Worker; }

// Where a filter term looks for zones. Frames are the FrameMark frame sets, not zones.
struct Scope
{
    enum class Kind { All, Cpu, Gpu, Thread, Frames };

    Kind kind = Kind::All;
    // Kind::Thread only: either an exact OS thread id, or a substring of the thread name.
    bool byId = false;
    uint64_t threadId = 0;
    std::string threadName;
};

// One "-f NAME[@SCOPE]" (substring) or "-F NAME[@SCOPE]" (whole name) term.
// An empty name matches every zone name.
struct FilterTerm
{
    std::string name;
    Scope scope;
    bool exact = false;
};

enum class RowOrder
{
    Sequential,     // by name, then start
    Interleaved,    // by start
    Columns         // one column group per event type, row k = k-th occurrence
};

struct ExportOptions
{
    const char* outputPath = "";
    const char* separator = ",";
    bool caseSensitive = false;
    bool selfTime = false;
    bool noLocation = false;
    bool zeroShift = false;
    bool seconds = false;
    double beginSec = 0;
    double lengthSec = -1;      // < 0 = unbounded
    RowOrder order = RowOrder::Sequential;
    std::vector<FilterTerm> terms;
};

// Case-insensitive unless caseSensitive; matches when term is a substring of s.
bool IsSubstring( const char* term, const char* s, bool caseSensitive );

// Keywords "all", "cpu", "gpu", "frames" (case-insensitive); anything else is a thread spec.
Scope ParseScope( const char* spec );

// Splits at the first '@'; a missing scope falls back to defaultScope.
FilterTerm ParseFilterTerm( const char* term, const Scope& defaultScope, bool exact = false );

// "sequential" | "interleaved" | "columns" (case-insensitive). Returns false on anything else.
bool ParseRowOrder( const char* spec, RowOrder& out );

// Writes the per-event CSV and its .dict companion. Returns the process exit code.
int RunExport( const tracy::Worker& worker, const ExportOptions& opts );

#endif
