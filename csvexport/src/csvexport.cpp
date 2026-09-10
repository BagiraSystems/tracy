#ifdef _WIN32
#  include <windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../../server/TracyFileRead.hpp"
#include "../../server/TracyWorker.hpp"
#include "../../getopt/getopt.h"
#include "../../public/common/TracyVersion.hpp"
#include "GitRef.hpp"
#include "Export.hpp"

void print_usage_exit(int e)
{
    fprintf(stderr, "tracy-csvexport %i.%i.%i / %s\n\n", tracy::Version::Major, tracy::Version::Minor, tracy::Version::Patch, tracy::GitRef);
    fprintf(stderr, "Extract statistics from a trace to a CSV format\n");
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  extract [OPTION...] <trace file>\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "  -h, --help                 Print usage\n");
    fprintf(stderr, "  -V, --version              Show version information\n");
    fprintf(stderr, "  -f, --filter arg           Filter zone names (default: "")\n");
    fprintf(stderr, "  -s, --sep arg              CSV separator (default: ,)\n");
    fprintf(stderr, "  -c, --case                 Case sensitive filtering\n");
    fprintf(stderr, "  -e, --self                 Get self times\n");
    fprintf(stderr, "  -u, --unwrap               Report each cpu zone event\n");
    fprintf(stderr, "  -g, --gpu                  Report each gpu zone event\n" );
    fprintf(stderr, "  -m, --messages             Report only messages\n");
    fprintf(stderr, "  -p, --plot                 Report plot data (only with -u)\n");
    fprintf(stderr, "  -t, --truncated_mean[=arg] Report truncated mean (arg is the percentile. Default is 90)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Per-event export mode (mutually exclusive with -u, -g, -m, -p):\n");
    fprintf(stderr, "  -x, --export arg           Write per-event rows to arg, strings to arg.dict\n");
    fprintf(stderr, "  -f NAME[@SCOPE]            Repeatable; SCOPE is all | cpu | gpu | frames | <thread name or id>\n");
    fprintf(stderr, "                             (frames = FrameMark frame sets; adds a numeric frame column)\n");
    fprintf(stderr, "  -F, --filter-exact arg     Like -f, but NAME must match the whole zone name\n");
    fprintf(stderr, "  -T, --scope arg            Default scope for -f terms without @ (default: all)\n");
    fprintf(stderr, "  -L, --no-location          Omit src_file and src_line columns\n");
    fprintf(stderr, "  -z, --zero                 Shift times so the earliest exported event starts at 0\n");
    fprintf(stderr, "  -b, --begin arg            Export only events starting arg seconds after the trace start\n");
    fprintf(stderr, "  -l, --length arg           Length of the exported window in seconds (default: unbounded)\n");
    fprintf(stderr, "  -B, --begin-frame arg      Window starts at frame arg of the main frame set (profiler numbering)\n");
    fprintf(stderr, "  -n, --frames arg           Window length in frames\n");
    fprintf(stderr, "  -E, --end-frame arg        Last frame included in the window (alternative to -n)\n");
    fprintf(stderr, "  -S, --seconds              Emit times as floating-point seconds instead of integer ns\n");
    fprintf(stderr, "  -o, --order arg            Row order: sequential (default) | interleaved | columns\n");

    exit(e);
}

struct Args {
    const char* filter = "";
    const char* separator = ",";
    const char* trace_file = "";
    bool case_sensitive = false;
    bool self_time = false;
    bool unwrap = false;
    bool show_gpu = false;
    bool unwrapMessages = false;
    bool plot = false;
    int truncated_mean_percentile = 0;
    // -x mode
    const char* export_path = nullptr;
    const char* default_scope = "all";
    bool no_location = false;
    bool zero_shift = false;
    bool seconds = false;
    double begin_sec = 0;
    double length_sec = -1;
    int64_t begin_frame = -1;
    int64_t frame_count = -1;
    int64_t end_frame = -1;
    RowOrder order = RowOrder::Sequential;
    bool export_only_flag_used = false;
    std::vector<const char*> filters;
    std::vector<const char*> exact_filters;
};

double parse_seconds(const char* arg, const char* opt)
{
    char* end = nullptr;
    const double v = strtod(arg, &end);
    if (end == arg || *end != '\0' || v < 0)
    {
        fprintf(stderr, "%s expects a non-negative number of seconds, got '%s'\n", opt, arg);
        print_usage_exit(1);
    }
    return v;
}

int64_t parse_frame(const char* arg, const char* opt)
{
    char* end = nullptr;
    const long long v = strtoll(arg, &end, 10);
    if (end == arg || *end != '\0' || v < 0)
    {
        fprintf(stderr, "%s expects a non-negative frame number, got '%s'\n", opt, arg);
        print_usage_exit(1);
    }
    return v;
}

Args parse_args(int argc, char** argv)
{
    if (argc == 1)
    {
        print_usage_exit(1);
    }

    Args args;

    struct option long_opts[] = {
        { "help", no_argument, NULL, 'h' },
        { "version", no_argument, NULL, 'V' },
        { "filter", required_argument, NULL, 'f' },
        { "sep", required_argument, NULL, 's' },
        { "case", no_argument, NULL, 'c' },
        { "self", no_argument, NULL, 'e' },
        { "unwrap", no_argument, NULL, 'u' },
        { "gpu", no_argument, NULL, 'g' },
        { "messages", no_argument, NULL, 'm' },
        { "plot", no_argument, NULL, 'p' },
        { "truncated_mean", optional_argument, NULL, 't' },
        { "export", required_argument, NULL, 'x' },
        { "filter-exact", required_argument, NULL, 'F' },
        { "scope", required_argument, NULL, 'T' },
        { "no-location", no_argument, NULL, 'L' },
        { "zero", no_argument, NULL, 'z' },
        { "begin", required_argument, NULL, 'b' },
        { "length", required_argument, NULL, 'l' },
        { "begin-frame", required_argument, NULL, 'B' },
        { "frames", required_argument, NULL, 'n' },
        { "end-frame", required_argument, NULL, 'E' },
        { "seconds", no_argument, NULL, 'S' },
        { "order", required_argument, NULL, 'o' },
        { NULL, 0, NULL, 0 }
    };

    int c;
    while ((c = getopt_long(argc, argv, "hf:s:t:ceugmpVx:F:T:Lzb:l:B:n:E:So:", long_opts, NULL)) != -1)
    {
        switch (c)
        {
        case 'h':
            print_usage_exit(0);
            break;
        case 'V':
            printf( "tracy-csvexport %i.%i.%i / %s\n", tracy::Version::Major, tracy::Version::Minor, tracy::Version::Patch, tracy::GitRef );
            exit( 0 );
        case 'f':
            args.filter = optarg;
            args.filters.push_back(optarg);
            break;
        case 'x':
            args.export_path = optarg;
            break;
        case 'F':
            args.exact_filters.push_back(optarg);
            args.export_only_flag_used = true;
            break;
        case 'T':
            args.default_scope = optarg;
            break;
        case 'L':
            args.no_location = true;
            break;
        case 'z':
            args.zero_shift = true;
            args.export_only_flag_used = true;
            break;
        case 'b':
            args.begin_sec = parse_seconds(optarg, "-b");
            args.export_only_flag_used = true;
            break;
        case 'l':
            args.length_sec = parse_seconds(optarg, "-l");
            args.export_only_flag_used = true;
            break;
        case 'B':
            args.begin_frame = parse_frame(optarg, "-B");
            args.export_only_flag_used = true;
            break;
        case 'n':
            args.frame_count = parse_frame(optarg, "-n");
            args.export_only_flag_used = true;
            break;
        case 'E':
            args.end_frame = parse_frame(optarg, "-E");
            args.export_only_flag_used = true;
            break;
        case 'S':
            args.seconds = true;
            args.export_only_flag_used = true;
            break;
        case 'o':
            if (!ParseRowOrder(optarg, args.order))
            {
                fprintf(stderr, "-o expects sequential, interleaved or columns, got '%s'\n", optarg);
                print_usage_exit(1);
            }
            args.export_only_flag_used = true;
            break;
        case 's':
            args.separator = optarg;
            break;
        case 'c':
            args.case_sensitive = true;
            break;
        case 'e':
            args.self_time = true;
            break;
        case 'u':
            args.unwrap = true;
            break;
        case 'g':
            args.show_gpu = true;
            break;
        case 'm':
            args.unwrapMessages = true;
            break;
        case 'p':
            args.plot = true;
            break;
        case 't':
            args.truncated_mean_percentile = std::clamp<int>(optarg ? std::atoi(optarg) : 90, 1, 99);
            break;
        default:
            print_usage_exit(1);
            break;
        }
    }

    if (argc != optind + 1)
    {
        print_usage_exit(1);
    }

    args.trace_file = argv[optind];

    if (args.export_path)
    {
        if (args.unwrap || args.show_gpu || args.unwrapMessages || args.plot || args.truncated_mean_percentile)
        {
            fprintf(stderr, "-x cannot be combined with -u, -g, -m, -p or -t\n");
            print_usage_exit(1);
        }
        const bool frame_window = args.begin_frame >= 0 || args.frame_count >= 0 || args.end_frame >= 0;
        if (frame_window && (args.begin_sec > 0 || args.length_sec >= 0))
        {
            fprintf(stderr, "-B, -n, -E (frame window) cannot be combined with -b, -l (time window)\n");
            print_usage_exit(1);
        }
        if (args.frame_count >= 0 && args.end_frame >= 0)
        {
            fprintf(stderr, "-n and -E are alternatives; give only one\n");
            print_usage_exit(1);
        }
    }
    else
    {
        // The pre-existing modes keep their single-substring filter semantics.
        bool scoped_filter = false;
        for (auto f : args.filters) scoped_filter |= strchr(f, '@') != nullptr;
        if (args.no_location || args.export_only_flag_used || strcmp(args.default_scope, "all") != 0 || args.filters.size() > 1 || scoped_filter)
        {
            fprintf(stderr, "-F, -L, -T, -z, -b, -l, -B, -n, -E, -S, -o, repeated -f and NAME@SCOPE filters require -x\n");
            print_usage_exit(1);
        }
    }

    return args;
}

bool is_substring(
    const char* term,
    const char* s,
    bool case_sensitive = false
){
    auto new_term = std::string(term);
    auto new_s = std::string(s);

    if (!case_sensitive) {
        std::transform(
            new_term.begin(),
            new_term.end(),
            new_term.begin(),
            [](unsigned char c){ return std::tolower(c); }
        );

        std::transform(
            new_s.begin(),
            new_s.end(),
            new_s.begin(),
            [](unsigned char c){ return std::tolower(c); }
        );
    }

    return new_s.find(new_term) != std::string::npos;
}

const char* get_name(int32_t id, const tracy::Worker& worker)
{
    auto& srcloc = worker.GetSourceLocation(id);
    return worker.GetString(srcloc.name.active ? srcloc.name : srcloc.function);
}

template <typename T>
std::string join(const T& v, const char* sep) {
    std::ostringstream s;
    for (const auto& i : v) {
        if (&i != &v[0]) {
            s << sep;
        }
        s << i;
    }
    return s.str();
}

// Returns {pN, truncated_mean}
std::pair<int64_t, int64_t> percentile_and_truncated_mean(std::vector<int64_t>& data, const double p)
{
    assert(p >= 0.0 && p <= 1.0);

    if (data.empty()) {
        return {0, 0};
    }

    std::sort(data.begin(), data.end());

    const std::size_t n = data.size();
    const double idx = p * (static_cast<double>(n) - 1.0);
    const std::size_t idxLow = static_cast<std::size_t>(std::floor(idx));
    const std::size_t idxHigh = std::min(idxLow + 1, n - 1);
    const double frac = idx - static_cast<double>(idxLow);

    const double low = static_cast<double>(data[idxLow]);
    const double high = static_cast<double>(data[idxHigh]);

    // percentile value
    const double pval_double = low + (high - low) * frac;
    const int64_t pval_int = static_cast<int64_t>(std::llround(pval_double));

    // Compute truncated mean: average of all values <= pval_double
    int64_t sum = 0;
    std::size_t count = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (static_cast<double>(data[i]) <= pval_double) {
            sum += data[i];
            ++count;
        } else {
            break; // sorted, so we can stop once we hit > pval_double
        }
    }

    if (count == 0) {
        // should not happen for p in [0,1] unless data empty, but keep defensive behaviour
        return {pval_int, 0};
    }

    const int64_t truncated_mean = sum / count;

    return {pval_int, truncated_mean};
}


// From TracyView.cpp
int64_t GetZoneChildTimeFast(
    const tracy::Worker& worker,
    const tracy::ZoneEvent& zone
){
    int64_t time = 0;
    if( zone.HasChildren() )
    {
        auto& children = worker.GetZoneChildren( zone.Child() );
        if( children.is_magic() )
        {
            auto& vec = *(tracy::Vector<tracy::ZoneEvent>*)&children;
            for( auto& v : vec )
            {
                assert( v.IsEndValid() );
                time += v.End() - v.Start();
            }
        }
        else
        {
            for( auto& v : children )
            {
                assert( v->IsEndValid() );
                time += v->End() - v->Start();
            }
        }
    }
    return time;
}

int main(int argc, char** argv)
{
#ifdef _WIN32
    if (!AttachConsole(ATTACH_PARENT_PROCESS))
    {
        AllocConsole();
        SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), 0x07);
    }
#endif

    Args args = parse_args(argc, argv);

    auto f = std::unique_ptr<tracy::FileRead>(
        tracy::FileRead::Open(args.trace_file)
    );
    if (!f)
    {
        fprintf(stderr, "Could not open file %s\n", args.trace_file);
        return 1;
    }

    auto worker = tracy::Worker(*f);

    if (args.unwrapMessages) 
    {
        const auto& msgs = worker.GetMessages();
    
        if (msgs.size() > 0)
        {
            std::vector<const char*> columnsForMessages;
            columnsForMessages = {
                    "MessageName", "total_ns"
                };
            std::string headerForMessages = join(columnsForMessages, args.separator);
            printf("%s\n", headerForMessages.data());

            for(auto& it : msgs)
            {
                std::vector<std::string> values(columnsForMessages.size());

                values[0] = worker.GetString(it->ref);
                values[1] = std::to_string(it->time);

                std::string row = join(values, args.separator);
                printf("%s\n", row.data());
            }
        }
        else
        {
            printf("There are currently no messages!\n");
        }
    
        return 0;
    }

    while (!worker.AreSourceLocationZonesReady())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (args.export_path)
    {
        ExportOptions opts;
        opts.outputPath = args.export_path;
        opts.separator = args.separator;
        opts.caseSensitive = args.case_sensitive;
        opts.selfTime = args.self_time;
        opts.noLocation = args.no_location;
        opts.zeroShift = args.zero_shift;
        opts.seconds = args.seconds;
        opts.beginSec = args.begin_sec;
        opts.lengthSec = args.length_sec;
        opts.beginFrame = args.begin_frame;
        opts.frameCount = args.frame_count;
        opts.endFrame = args.end_frame;
        opts.order = args.order;

        const Scope defaultScope = ParseScope(args.default_scope);
        for (auto f : args.filters)
        {
            opts.terms.push_back(ParseFilterTerm(f, defaultScope));
        }
        for (auto f : args.exact_filters)
        {
            opts.terms.push_back(ParseFilterTerm(f, defaultScope, true));
        }
        if (opts.terms.empty())
        {
            opts.terms.push_back(ParseFilterTerm("", defaultScope));
        }

        return RunExport(worker, opts);
    }

    if (args.show_gpu)
    {
        auto& gpu_slz = worker.GetGpuSourceLocationZones();
        tracy::Vector<decltype( gpu_slz.begin() )> gpu_slz_selected;
        gpu_slz_selected.reserve( gpu_slz.size() );

        uint32_t total_cnt = 0;
        for (auto it = gpu_slz.begin(); it != gpu_slz.end(); ++it)
        {
            if (it->second.total != 0)
            {
                ++total_cnt;
                if (args.filter[0] == '\0')
                {
                    gpu_slz_selected.push_back_no_space_check( it );
                }
                else
                {
                    auto name = get_name( it->first, worker );
                    if (is_substring( args.filter, name, args.case_sensitive))
                    {
                        gpu_slz_selected.push_back_no_space_check( it );
                    }
                }
            }
        }

        std::vector<const char*> columns;
        columns = {"name", "src_file", "Time from start of program", "GPU execution time"};

        std::string header = join(columns, args.separator);
        printf("%s\n", header.data());

        const auto last_time = worker.GetLastTime();
        for (auto& it : gpu_slz_selected)
        {
            std::vector<std::string> values( columns.size() );

            values[0] = get_name( it->first, worker );

            const auto& srcloc = worker.GetSourceLocation( it->first );
            values[1] = worker.GetString( srcloc.file );

            const auto& zone_data = it->second;
            for (const auto& zone_thread_data : zone_data.zones)
            {
                tracy::GpuEvent* gpu_event = zone_thread_data.Zone();
                const auto start = gpu_event->GpuStart();
                const auto end = gpu_event->GpuEnd();

                values[2] = std::to_string( start );

                auto timespan = end - start;
                values[3] = std::to_string( timespan );

                std::string row = join( values, args.separator );
                printf( "%s\n", row.data() );
            }
        }
        return 0;
    }

    auto& slz = worker.GetSourceLocationZones();
    tracy::Vector<decltype(slz.begin())> slz_selected;
    slz_selected.reserve(slz.size());

    uint32_t total_cnt = 0;
    for(auto it = slz.begin(); it != slz.end(); ++it)
    {
        if(it->second.total != 0)
        {
            ++total_cnt;
            if(args.filter[0] == '\0')
            {
                slz_selected.push_back_no_space_check(it);
            }
            else
            {
                auto name = get_name(it->first, worker);
                if(is_substring(args.filter, name, args.case_sensitive))
                {
                    slz_selected.push_back_no_space_check(it);
                }
            }
        }
    }

    std::vector<const char*> columns;
    if (args.unwrap)
    {
        columns = {
            "name", "src_file", "src_line", "ns_since_start", "exec_time_ns", "thread", "value"
        };
    }
    else
    {
        columns = {
            "name", "src_file", "src_line", "total_ns", "total_perc",
            "counts", "mean_ns", "min_ns", "max_ns", "std_ns"
        };

        if(args.truncated_mean_percentile)
        {
            columns.push_back("percentile_ns");
            columns.push_back("truncated_mean_ns");
        }
    }
    std::string header = join(columns, args.separator);
    printf("%s\n", header.data());

    const auto last_time = worker.GetLastTime();
    for(auto& it : slz_selected)
    {
        std::vector<std::string> values(columns.size());

        values[0] = get_name(it->first, worker);

        const auto& srcloc = worker.GetSourceLocation(it->first);
        values[1] = worker.GetString(srcloc.file);
        values[2] = std::to_string(srcloc.line);

        const auto& zone_data = it->second;

        if (args.unwrap)
        {
            int i = 0;
            for (const auto& zone_thread_data : zone_data.zones) {
                const auto zone_event = zone_thread_data.Zone();
                const auto tId = zone_thread_data.Thread();
                const auto start = zone_event->Start();
                const auto end = zone_event->End();

                values[3] = std::to_string(start);

                auto timespan = end - start;
                if (args.self_time) {
                    timespan -= GetZoneChildTimeFast(worker, *zone_event);
                }
                values[4] = std::to_string(timespan);
                values[5] = std::to_string(tId);
                if (worker.HasZoneExtra(*zone_event)) {
                    const auto& text = worker.GetZoneExtra(*zone_event).text;
                    if (text.Active()) {
                        values[6] = worker.GetString(text);
                    }
                }

                std::string row = join(values, args.separator);
                printf("%s\n", row.data());
            }
        }
        else
        {
            const auto time = args.self_time ? zone_data.selfTotal : zone_data.total;
            values[3] = std::to_string(time);
            values[4] = std::to_string(100. * time / last_time);

            const auto sz = zone_data.zones.size();
            values[5] = std::to_string(sz);

            const auto avg = time / sz;

            values[6] = std::to_string(avg);

            const auto tmin = args.self_time ? zone_data.selfMin : zone_data.min;
            const auto tmax = args.self_time ? zone_data.selfMax : zone_data.max;
            values[7] = std::to_string(tmin);
            values[8] = std::to_string(tmax);

            const auto ss = zone_data.sumSq
                - 2. * zone_data.total * avg
                + avg * avg * sz;
            double std = 0;
            if( sz > 1 )
                std = sqrt(ss / (sz - 1));
            values[9] = std::to_string(std);

            if(args.truncated_mean_percentile)
            {
                std::vector<int64_t> samples;
                samples.reserve( zone_data.zones.size() );
                for(const auto& zone_thread_data : zone_data.zones)
                {
                    const auto zone_event = zone_thread_data.Zone();
                    auto timespan = zone_event->End() - zone_event->Start();
                    if(args.self_time)
                        timespan -= GetZoneChildTimeFast( worker, *zone_event );
                    samples.push_back( timespan );
                }

                std::pair<int64_t, int64_t> pN = percentile_and_truncated_mean(samples, args.truncated_mean_percentile / 100.0);
                values[10] = std::to_string(pN.first);
                values[11] = std::to_string(pN.second);
            }

            std::string row = join(values, args.separator);
            printf("%s\n", row.data());
        }
    }

    if(args.plot && args.unwrap)
    {
        auto& plots = worker.GetPlots();
        for(const auto& plot : plots)
        {
            std::vector<std::string> values(columns.size());
            values[0] = worker.GetString(plot->name);

            for(const auto& val : plot->data)
            {
                if (args.unwrap)
                {
                    values[3] = std::to_string(val.time.Val());
                    values[6] = std::to_string(val.val);
                }
                std::string row = join(values, args.separator);
                printf("%s\n", row.data());
            }
        }
    }

    return 0;
}
