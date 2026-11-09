#include <tm_kit/basic/simple_shared_chain/InMemoryLockFreeChain.hpp>
#include <tm_kit/basic/IntIDComponent.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

using namespace dev::cd606::tm;
namespace sc = basic::simple_shared_chain;
using TP = std::chrono::system_clock::time_point;
struct Clock {
    using TimePointType = TP;
    using DurationType = TP::duration;
    static constexpr bool PreserveInputRelativeOrder = false;
    static constexpr bool CanBeActualTimeClock = true;
    TP now()
    {
        return std::chrono::system_clock::now();
    }
    TP resolveTime()
    {
        return now();
    }
    TP resolveTime(TP const &)
    {
        return now();
    }
    void log(infra::LogLevel, std::string const &)
    {
    }
};
using Env = infra::Environment<infra::CheckTimeComponent<false>, infra::TrivialExitControlComponent, Clock, basic::IntIDComponent<>>;
using M = infra::RealTimeApp<Env>;
using Chain = sc::InMemoryLockFreeChain<int>;
int failures = 0;
void check(bool ok, char const *msg)
{
    std::cout << (ok ? "ok: " : "FAIL: ") << msg << '\n';
    failures += !ok;
}
template <class F>
bool waitUntil(F pred)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!pred() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return pred();
}
struct Folder {
    using ResultType = int;
    int initialize(Env *, Chain *)
    {
        return 0;
    }
    void foldInPlace(int &state, std::string_view, int x)
    {
        state += x;
    }
};
struct Input {
    using InputType = int;
    using ResponseType = int;
    static constexpr bool SingleWritingMode = false;
    void initialize(Env *, Chain *)
    {
    }
    std::tuple<int, std::vector<std::tuple<std::string, int>>> handleInput(Env *, Chain *, infra::WithTime<M::Key<int>, TP> const &data, int const &)
    {
        int x = data.value.key();
        return {x, {{std::to_string(x), x}}};
    }
};
void testChain(bool busy, bool noYield)
{
    // Preserve the baseline detached lifecycle: graph objects live to process exit.
    auto *env = new Env;
    auto *chain = new Chain;
    using Writer = sc::ChainWriter<M, Chain, Folder, Input>;
    auto *writer = new Writer(chain, sc::ChainPollingPolicy{}.BusyLoop(busy).NoYield(noYield));
    writer->start(env);
    constexpr int n = 5000;
    for (int i = 1; i <= n; ++i) {
        writer->handle(M::pureInnerData(env, M::Key<int>{static_cast<uint64_t>(i), int(i)}));
    }
    auto *item = chain->head(env);
    int count = 0;
    bool ordered = true;
    bool complete = waitUntil([&] {
        while (auto next = chain->fetchNext(item)) {
            item = *next;
            ordered = ordered && *Chain::extractData(item) == ++count;
        }
        return count == n;
    });
    check(complete && ordered, busy ? (noYield ? "ChainWriter noYield writes FIFO" : "ChainWriter busyLoop writes FIFO") : "ChainWriter blocking writes FIFO");
}

// Exercise the exact local blocking loop with an item arriving while a callback
// is running. Its notification precedes wait_for, so the predicate is essential.
class Loop : public sc::chain_writer_detail::ThreadedHandler<Env, int, Loop> {
public:
    std::atomic<int> idle{0}, seen{0};
    std::atomic<bool> entered{false}, release{false};
    void waitForStart()
    {
    }
    void idleWork()
    {
        ++idle;
    }
    void actuallyHandle(M::InnerData<int> &&x)
    {
        if (x.timedData.value == 0) {
            entered = true;
            while (!release.load()) {
                std::this_thread::yield();
            }
        }
        ++seen;
    }
};
void testLoop()
{
    auto *env = new Env;
    auto *loop = new Loop;
    loop->startThread();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    int ticks = loop->idle.load();
    check(ticks >= 20 && ticks < 200, "blocking idle retains approximately 1ms cadence");
    std::vector<int64_t> latencies;
    for (int i = 0; i < 200; ++i) {
        loop->entered = false;
        loop->release = false;
        loop->handle(M::pureInnerData(env, 0));
        bool entered = waitUntil([&] { return loop->entered.load(); });
        loop->handle(M::pureInnerData(env, 1)); // queued while processing, notify lost on old loop
        auto begin = std::chrono::steady_clock::now();
        loop->release = true;
        bool done = waitUntil([&] { return loop->seen.load() == 2 * (i + 1); });
        if (!entered || !done) {
            check(false, "local loop completed");
            break;
        }
        latencies.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count());
    }
    std::sort(latencies.begin(), latencies.end());
    auto p50 = latencies[latencies.size() / 2] / 1000.0;
    std::cout << "pending-during-callback p50 us: " << p50 << '\n';
    check(p50 < 300, "queued work skips the old 1ms wait");
    loop->stop();
}
int main()
{
    testLoop();
    testChain(false, false);
    testChain(true, false);
    testChain(true, true);
    std::cout << (failures ? "FAILED" : "ALL PASSED") << std::endl;
    return failures ? 1 : 0;
}
