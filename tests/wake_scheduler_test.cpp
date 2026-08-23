#include <gaudere/scheduling/wake/Scheduler.hpp>

#include <chrono>
#include <future>
#include <iostream>
#include <string>
#include <thread>

namespace {

using gaudere::scheduling::wake::Scheduler;
using gaudere::scheduling::wake::Update;
using gaudere::scheduling::wake::WaitResult;
using namespace std::chrono_literals;

int failures = 0;

void expect(const bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void test_first_deadline()
{
    Scheduler scheduler;
    const auto deadline = Scheduler::Clock::now() + 1h;

    expect(scheduler.request_at(deadline) == Update::scheduled,
           "the first deadline is scheduled");
    expect(scheduler.next() == deadline,
           "next exposes the scheduled deadline");
}

void test_earlier_and_later_deadlines()
{
    Scheduler scheduler;
    const auto first = Scheduler::Clock::now() + 2h;
    const auto earlier = first - 1h;
    const auto later = first + 1h;

    scheduler.request_at(first);
    expect(scheduler.request_at(earlier) == Update::advanced,
           "an earlier deadline advances the wake-up");
    expect(scheduler.request_at(later) == Update::unchanged,
           "a later deadline is ignored");
    expect(scheduler.next() == earlier,
           "the earlier deadline remains scheduled");
}

void test_past_deadline_is_due()
{
    Scheduler scheduler;
    scheduler.request_at(Scheduler::Clock::now() - 1s);

    const auto started = std::chrono::steady_clock::now();
    expect(scheduler.wait() == WaitResult::due,
           "a past deadline is immediately due");
    expect(std::chrono::steady_clock::now() - started < 100ms,
           "a past deadline does not block");
    expect(!scheduler.next(), "a due deadline is consumed");
}

void test_interrupt_preserves_future_deadline()
{
    Scheduler scheduler;
    const auto deadline = Scheduler::Clock::now() + 1h;
    scheduler.request_at(deadline);
    auto result = std::async(std::launch::async, [&scheduler] {
        return scheduler.wait();
    });

    std::this_thread::sleep_for(20ms);
    scheduler.interrupt();

    expect(result.wait_for(1s) == std::future_status::ready,
           "interrupt wakes a blocked waiter");
    expect(result.get() == WaitResult::interrupted,
           "wait distinguishes a non-deadline interrupt");
    expect(scheduler.next() == deadline,
           "interrupt preserves the exact scheduled deadline");
}

void test_interrupt_without_deadline()
{
    Scheduler scheduler;
    auto result = std::async(std::launch::async, [&scheduler] {
        return scheduler.wait();
    });

    std::this_thread::sleep_for(20ms);
    scheduler.interrupt();

    expect(result.wait_for(1s) == std::future_status::ready,
           "interrupt wakes a waiter with no deadline");
    expect(result.get() == WaitResult::interrupted,
           "no-deadline interrupt is observational");
    expect(!scheduler.next(),
           "no-deadline interrupt does not fabricate a deadline");
}

void test_due_deadline_wins_over_interrupt()
{
    Scheduler scheduler;
    scheduler.request_at(Scheduler::Clock::now() - 1s);
    scheduler.interrupt();

    expect(scheduler.wait() == WaitResult::due,
           "a genuinely due deadline takes precedence over observation interrupt");
    expect(!scheduler.next(), "due deadline remains consumed exactly once");
}

void test_stop()
{
    Scheduler scheduler;
    auto result = std::async(std::launch::async, [&scheduler] {
        return scheduler.wait();
    });

    std::this_thread::sleep_for(20ms);
    scheduler.stop();

    expect(result.wait_for(1s) == std::future_status::ready,
           "stop wakes a blocked waiter");
    expect(result.get() == WaitResult::stopped,
           "wait reports a permanent stop");
    expect(scheduler.request_after(1s) == Update::unchanged,
           "a stopped scheduler rejects new deadlines");
    expect(!scheduler.next(), "stop discards the pending deadline");
}

void test_wait_recalculates_for_earlier_deadline()
{
    Scheduler scheduler;
    scheduler.request_after(2s);

    auto result = std::async(std::launch::async, [&scheduler] {
        return scheduler.wait();
    });

    std::this_thread::sleep_for(20ms);
    expect(scheduler.request_after(40ms) == Update::advanced,
           "a nearer deadline advances a running wait");
    expect(result.wait_for(1s) == std::future_status::ready,
           "wait wakes for the nearer deadline");
    expect(result.get() == WaitResult::due,
           "the recalculated wait reports a due deadline");
}

} // namespace

int main()
{
    test_first_deadline();
    test_earlier_and_later_deadlines();
    test_past_deadline_is_due();
    test_interrupt_preserves_future_deadline();
    test_interrupt_without_deadline();
    test_due_deadline_wins_over_interrupt();
    test_stop();
    test_wait_recalculates_for_earlier_deadline();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }

    std::cout << "All wake scheduler tests passed\n";
    return 0;
}
