// sfvr-keeper: see keeper.h.
//
//   sfvr-keeper --key <file> [--server host:port] [--state <file>] [--words <file>] [--ports A-B] [--per-link-ports] [--protocol N]
//               [--metrics <file.prom>] [--status <file.json>]
//   sfvr-keeper --new-key <file>    writes a key, prints its public half
//
// Logs to stdout (the journal on the server). Every 5 s, when asked: its
// metrics in the Prometheus text format (--metrics, for node_exporter's
// textfile collector) and the public room list as JSON (--status, for the
// rooms API), each written to a temporary file and renamed over the old.

#include "core/lobby.h"
#include "keeper/keeper.h"
#include "keeper/sign.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

using namespace sfv::keeper;

namespace {

std::atomic<bool> g_stop{false};

// Log lines and the time spent writing them (stdout can block: a slow
// journal or Docker log driver holds up the whole loop).
uint64_t g_logLines = 0;
double g_logSeconds = 0;

void OnSignal(int) { g_stop = true; }

int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void LogLine(const std::string& line)
{
    const auto started = std::chrono::steady_clock::now();
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    const int ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::printf("%02d:%02d:%02d.%03d %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec, ms, line.c_str());
    std::fflush(stdout);
    g_logLines++;
    g_logSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
}

// Writes beside the file and renames over it (readers never see half a file).
void WriteFileAtomically(const std::string& path, const std::string& text)
{
    const std::string temp = path + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) return;
    }
#ifdef _WIN32
    std::remove(path.c_str());
#endif
    std::rename(temp.c_str(), path.c_str());
}

int Usage()
{
    std::fprintf(stderr,
                 "usage: sfvr-keeper --key <file> [--server host:port] [--state <file>] [--words <file>] [--ports A-B] [--per-link-ports] [--protocol N]\n"
                 "                   [--metrics <file.prom>] [--status <file.json>]\n"
                 "       sfvr-keeper --new-key <file>\n");
    return 2;
}

int NewKey(const std::string& path)
{
    KeyPair key;
    if (!GenerateKey(&key)) {
        std::fprintf(stderr, "cannot make a key\n");
        return 1;
    }
    {
        std::ifstream exists(path);
        if (exists) {
            std::fprintf(stderr, "%s exists; not overwriting it\n", path.c_str());
            return 1;
        }
    }
    std::ofstream out(path, std::ios::trunc);
    out << KeyToText(key) << "\n";
    if (!out) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return 1;
    }
    std::printf("wrote %s (keep it private)\npublic key for the mod:\n%s\n", path.c_str(), Hex(key.pub, 64).c_str());
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    KeeperOptions o;
    o.protocol = sfv::lobby::kProtocol;
    std::string keyPath, metricsPath, statusPath;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        const bool hasValue = i + 1 < argc;
        if (a == "--new-key" && hasValue) return NewKey(argv[i + 1]);
        if (a == "--key" && hasValue) keyPath = argv[++i];
        else if (a == "--state" && hasValue) o.statePath = argv[++i];
        else if (a == "--words" && hasValue) {
            std::ifstream words(argv[++i]);
            std::string line;
            while (std::getline(words, line)) {
                const std::string w = FoldName(line);
                if (w.size() >= 3) o.words.push_back(w); // shorter ones would catch ordinary names
            }
        }
        else if (a == "--protocol" && hasValue) o.protocol = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--ports" && hasValue) {
            const std::string r = argv[++i];
            const size_t dash = r.find('-');
            o.portBegin = static_cast<uint16_t>(std::atoi(r.c_str()));
            o.portEnd = dash == std::string::npos ? o.portBegin : static_cast<uint16_t>(std::atoi(r.c_str() + dash + 1));
        }
        else if (a == "--per-link-ports") o.linkMux = false;
        else if (a == "--metrics" && hasValue) metricsPath = argv[++i];
        else if (a == "--status" && hasValue) statusPath = argv[++i];
        else if (a == "--server" && hasValue) {
            std::string s = argv[++i];
            const size_t colon = s.rfind(':');
            if (colon != std::string::npos) {
                o.port = static_cast<uint16_t>(std::atoi(s.c_str() + colon + 1));
                s.resize(colon);
            }
            o.host = s;
        }
        else return Usage();
    }
    if (keyPath.empty()) return Usage();
    std::ifstream keyFile(keyPath);
    std::stringstream text;
    text << keyFile.rdbuf();
    if (!keyFile || !KeyFromText(text.str(), &o.key)) {
        std::fprintf(stderr, "cannot read a key from %s\n", keyPath.c_str());
        return 1;
    }
    o.log = LogLine;
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    LogLine("sfvr-keeper: lobby protocol " + std::to_string(o.protocol) + ", public key " + Hex(o.key.pub, 8) + "..., " +
            std::to_string(o.words.size()) + " filtered words");

    Keeper keeper(o);
    keeper.Start(NowMs());
    const double startedUnix = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    int64_t filesAtMs = -1;
    while (!g_stop) {
        keeper.Step(NowMs());
        const int64_t nowMs = NowMs();
        if ((!metricsPath.empty() || !statusPath.empty()) && (filesAtMs < 0 || nowMs - filesAtMs >= 5000)) {
            filesAtMs = nowMs;
            const auto sinceEpoch = std::chrono::system_clock::now().time_since_epoch();
            if (!metricsPath.empty()) {
                std::ostringstream extra;
                extra << std::fixed;
                extra.precision(3);
                extra << "# HELP sfvr_keeper_info The keeper's lobby protocol.\n# TYPE sfvr_keeper_info gauge\n"
                      << "sfvr_keeper_info{protocol=\"" << o.protocol << "\"} 1\n"
                      << "# HELP sfvr_keeper_start_time_seconds When the keeper started (Unix time).\n# TYPE sfvr_keeper_start_time_seconds gauge\n"
                      << "sfvr_keeper_start_time_seconds " << startedUnix << "\n"
                      << "# HELP sfvr_keeper_log_lines_total Log lines written.\n# TYPE sfvr_keeper_log_lines_total counter\n"
                      << "sfvr_keeper_log_lines_total " << g_logLines << "\n"
                      << "# HELP sfvr_keeper_log_seconds_total Time spent writing log lines (stdout can block the loop).\n# TYPE sfvr_keeper_log_seconds_total counter\n"
                      << "sfvr_keeper_log_seconds_total " << g_logSeconds << "\n";
                WriteFileAtomically(metricsPath, keeper.MetricsText() + extra.str());
            }
            if (!statusPath.empty())
                WriteFileAtomically(statusPath, keeper.StatusJson(std::chrono::duration_cast<std::chrono::milliseconds>(sinceEpoch).count()) + "\n");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    LogLine("sfvr-keeper: stopping");
    return 0;
}
