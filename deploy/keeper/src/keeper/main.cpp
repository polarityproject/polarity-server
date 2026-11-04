// sfvr-keeper: see keeper.h.
//
//   sfvr-keeper --key <file> [--server host:port] [--state <file>] [--words <file>] [--ports A-B] [--protocol N]
//   sfvr-keeper --new-key <file>    writes a key, prints its public half
//
// Logs to stdout (the journal on the server).

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

void OnSignal(int) { g_stop = true; }

int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void LogLine(const std::string& line)
{
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
}

int Usage()
{
    std::fprintf(stderr,
                 "usage: sfvr-keeper --key <file> [--server host:port] [--state <file>] [--words <file>] [--ports A-B] [--protocol N]\n"
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
    std::string keyPath;
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
    while (!g_stop) {
        keeper.Step(NowMs());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    LogLine("sfvr-keeper: stopping");
    return 0;
}
