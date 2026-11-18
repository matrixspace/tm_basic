#include <tm_kit/basic/ByteDataWithTopicRecordFileImporterExporter.hpp>
#include <tm_kit/basic/CommonFlowUtils.hpp>
#include <tm_kit/basic/GenericIOStreamImporterExporter.hpp>
#include <tm_kit/basic/IStreamPollingLinesImporter.hpp>
#include <tm_kit/infra/Environments.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <sstream>
#include <thread>

using namespace dev::cd606::tm;
using namespace std::chrono_literals;

namespace
{
    using Clock = std::chrono::system_clock;
    using TP = Clock::time_point;
    struct SleepForClock {
        using TimePointType = TP;
        using DurationType = TP::duration;
        static constexpr bool PreserveInputRelativeOrder = false;
        static constexpr bool CanBeActualTimeClock = true;
        std::atomic<bool> waiting{false};
        TP now()
        {
            return Clock::now();
        }
        TP resolveTime()
        {
            return now();
        }
        TP resolveTime(TP const &t)
        {
            return t;
        }
        void log(infra::LogLevel, std::string const &)
        {
        }
        void sleepFor(DurationType const &duration)
        {
            waiting = true;
            std::this_thread::sleep_for(duration);
        }
    };
    struct ConvertedClock : SleepForClock {
        DurationType actualDuration(DurationType const &duration)
        {
            waiting = true;
            return duration / 2;
        }
    };

    template <class C>
    using Env = infra::Environment<infra::CheckTimeComponent<false>, infra::TrivialExitControlComponent, C>;
    template <class C>
    using App = infra::RealTimeApp<Env<C>>;

    int failures = 0;
    void check(bool ok, char const *what)
    {
        std::cout << (ok ? "ok: " : "FAIL: ") << what << std::endl;
        failures += !ok;
    }

    template <class C>
    bool waitForClock(Env<C> &env)
    {
        auto const deadline = std::chrono::steady_clock::now() + 3s;
        while (!env.waiting && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
        return env.waiting;
    }

    struct Reader {
        TP time;
        bool read = false;
        template <class E>
        void start(E *, std::istream &)
        {
        }
        template <class E>
        std::optional<std::tuple<TP, int>> readOne(E *, std::istream &)
        {
            if (read) {
                return std::nullopt;
            }
            read = true;
            return std::make_tuple(time, 1);
        }
    };

    template <class C, class T>
    void checkReplay(Env<C> &env, std::shared_ptr<typename App<C>::template Importer<T>> importer, char const *what)
    {
        using M = App<C>;
        bool delivered = false;
        auto sink = M::template pureExporter<T>([&](T &&) { delivered = true; });
        auto runner = std::make_unique<infra::AppRunner<M>>(&env);
        runner->connect(runner->importItem("source", importer), runner->exporterAsSink("sink", sink));
        env.waiting = false;
        runner->finalize();
        bool const waiting = waitForClock<C>(env);
        auto const begin = std::chrono::steady_clock::now();
        runner->controlAll("source", "stop", {});
        importer.reset();
        runner.reset();
        check(waiting && !delivered && std::chrono::steady_clock::now() - begin < 2s, what);
    }

    template <class C>
    void checkClock()
    {
        using M = App<C>;
        using Generic = basic::GenericIOStreamImporterExporter<M>;
        Env<C> env;
        auto const future = env.now() + 1h;
        auto stream = std::make_shared<std::istringstream>();
        basic::GenericIStreamImporterSpec<M> spec{{stream, {}}};
        checkReplay<C, int>(env, Generic::template createImporter<int>(stream, Reader{future}), "single-stream replay stops during timestamp wait");
        checkReplay<C, int>(env, Generic::template createImporter<int>(spec, Reader{future}), "multi-stream replay stops during timestamp wait");

        using Format = basic::ByteDataWithTopicRecordFileFormat<std::chrono::milliseconds>;
        std::stringstream recording;
        basic::ByteDataWithTopicRecordFileWriter<Format> writer({}, {});
        writer.startWritingByteDataWithTopicRecordFile(recording);
        writer.writeByteDataWithTopicRecord(recording, {future, {"topic", "value"}, true});
        checkReplay<C, basic::ByteDataWithTopic>(
            env, basic::ByteDataWithTopicRecordFileImporterExporter<M>::template createImporter<Format>(recording), "byte recording replay stops during timestamp wait");

        auto delayer = basic::CommonFlowUtilComponents<M>::template delayer<int>(5ms);
        auto *core = static_cast<typename M::template AbstractAction<int, int> *>(*delayer->getUnderlyingPointers().begin());
        env.waiting = false;
        core->handle(typename M::template InnerData<int>{&env, {future, 1, false}});
        bool const waiting = waitForClock<C>(env);
        auto const begin = std::chrono::steady_clock::now();
        delayer.reset();
        check(waiting && std::chrono::steady_clock::now() - begin < 2s, "Delayer tears down while waiting for a future timestamp");
    }

    void checkPollingLinesStop()
    {
        using E = Env<SleepForClock>;
        using M = App<SleepForClock>;
        E env;
        auto stream = std::make_shared<std::istringstream>("line\n");
        auto importer = basic::IStreamPollingLinesImporter<E>::createImporter(stream, 1h);
        auto *core = static_cast<M::AbstractImporter<std::vector<std::string>> *>(*importer->getUnderlyingPointers().begin());
        core->start(&env);
        std::this_thread::sleep_for(20ms);
        auto const begin = std::chrono::steady_clock::now();
        importer.reset();
        check(std::chrono::steady_clock::now() - begin < 2s, "line importer tears down during a long polling interval");
    }

    void checkStopDuringCallback()
    {
        using C = ConvertedClock;
        using M = App<C>;
        Env<C> env;
        std::promise<void> entered, release;
        auto enteredFuture = entered.get_future();
        auto releaseFuture = release.get_future();
        auto stream = std::make_shared<std::istringstream>();
        auto importer = basic::GenericIOStreamImporterExporter<M>::createImporter<int>(stream, Reader{env.now()});
        auto sink = M::pureExporter<int>([&](int &&) {
            entered.set_value();
            releaseFuture.wait();
        });
        auto runner = std::make_unique<infra::AppRunner<M>>(&env);
        runner->connect(runner->importItem("source", importer), runner->exporterAsSink("sink", sink));
        runner->finalize();
        bool const enteredCallback = enteredFuture.wait_for(3s) == std::future_status::ready;
        auto stop = std::async(std::launch::async, [&] { runner->controlAll("source", "stop", {}); });
        bool const stopReturned = stop.wait_for(1s) == std::future_status::ready;
        release.set_value();
        stop.get();
        importer.reset();
        runner.reset();
        check(enteredCallback && stopReturned, "controlAll returns while the replay callback is running");
    }
} // namespace

int main()
{
    checkClock<SleepForClock>();
    checkClock<ConvertedClock>();
    checkStopDuringCallback();
    checkPollingLinesStop();
    return failures ? 1 : 0;
}
