#pragma once
// The keeper's connects and teardowns, off its loop (docs/room-listing-plan.md
// L1). Connecting a Weyvelength client blocks (a name lookup, then TCP; a
// firewall that drops new connections makes it tens of seconds), and the
// keeper's one loop runs every room: a connect on it held them all up (the
// VPS slowdown, 2026-11-06). Here workers make and connect clients and hand
// each one to the loop by its ticket; the loop alone uses a client once it
// has it, so no client is ever used by two threads. Clients the loop is done
// with are left and destroyed by the workers too.
//
// Header-only and on any client type, so the tests drive it with fakes.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace sfv::keeper {

template <typename Client>
class Connector {
public:
    using Make = std::function<Client*()>;        // a connected client, or null (on a worker)
    using Destroy = std::function<void(Client*)>; // leaves and frees one (on a worker)

    Connector(Make make, Destroy destroy, int threads = 4) : make_(std::move(make)), destroy_(std::move(destroy)) {
        for (int i = 0; i < threads; i++) threads_.emplace_back([this] { Work(); });
    }

    ~Connector() {
        {
            std::lock_guard lock(m_);
            stop_ = true;
        }
        cv_.notify_all();
        for (std::thread& t : threads_) t.join();
        // What is left: finished connects nobody took, and teardowns not
        // reached (at exit; done here, on the caller's thread).
        for (auto& [ticket, done] : done_) {
            if (done.client) destroy_(done.client);
        }
        for (const Job& j : jobs_) {
            if (j.retire) destroy_(j.retire);
        }
    }

    Connector(const Connector&) = delete;
    Connector& operator=(const Connector&) = delete;

    // Starts a connect: its ticket (never 0).
    uint64_t Request() {
        uint64_t ticket;
        {
            std::lock_guard lock(m_);
            ticket = next_++;
            jobs_.push_back({ticket, nullptr});
            pending_++;
        }
        cv_.notify_one();
        return ticket;
    }

    // True once the connect is over: *client is the connected client (the
    // caller's from now on) or null if it failed; *seconds what it took.
    bool Take(uint64_t ticket, Client** client, double* seconds = nullptr) {
        std::lock_guard lock(m_);
        const auto it = done_.find(ticket);
        if (it == done_.end()) return false;
        *client = it->second.client;
        if (seconds) *seconds = it->second.seconds;
        done_.erase(it);
        pending_--;
        return true;
    }

    // Not wanted any more: not started, it never starts; started or done,
    // its client is destroyed by a worker.
    void Cancel(uint64_t ticket) {
        if (!ticket) return;
        Client* finished = nullptr;
        {
            std::lock_guard lock(m_);
            for (auto it = jobs_.begin(); it != jobs_.end(); ++it) {
                if (!it->retire && it->ticket == ticket) {
                    jobs_.erase(it);
                    pending_--;
                    return;
                }
            }
            const auto done = done_.find(ticket);
            if (done != done_.end()) {
                finished = done->second.client;
                done_.erase(done);
                pending_--;
            } else {
                cancelled_.insert(ticket);
                pending_--;
            }
        }
        if (finished) Retire(finished);
    }

    // The loop is done with it: a worker leaves its room and destroys it.
    void Retire(Client* client) {
        if (!client) return;
        {
            std::lock_guard lock(m_);
            jobs_.push_back({0, client});
        }
        cv_.notify_one();
    }

    // Connects asked for and not yet taken or cancelled.
    size_t Pending() const {
        std::lock_guard lock(m_);
        return pending_;
    }

private:
    struct Job {
        uint64_t ticket = 0;
        Client* retire = nullptr; // a teardown, not a connect
    };

    struct Done {
        Client* client = nullptr;
        double seconds = 0;
    };

    void Work() {
        while (true) {
            Job job;
            {
                std::unique_lock lock(m_);
                cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
                if (stop_) return;
                job = jobs_.front();
                jobs_.pop_front();
            }
            if (job.retire) {
                destroy_(job.retire);
                continue;
            }
            const auto started = std::chrono::steady_clock::now();
            Client* client = make_();
            const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            bool unwanted = false;
            {
                std::lock_guard lock(m_);
                unwanted = cancelled_.erase(job.ticket) > 0;
                if (!unwanted) done_[job.ticket] = {client, took};
            }
            if (unwanted && client) destroy_(client);
        }
    }

    Make make_;
    Destroy destroy_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    std::map<uint64_t, Done> done_;
    std::set<uint64_t> cancelled_;
    uint64_t next_ = 1;
    size_t pending_ = 0;
    bool stop_ = false;
    std::vector<std::thread> threads_; // last: started once the rest is set up
};

} // namespace sfv::keeper
