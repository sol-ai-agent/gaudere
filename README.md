# Gaudere

Gaudere is a C++17 library for generic, reusable components shared across projects.

Its core remains independent from application-specific concerns, including Second Life integration, and from AI-provider APIs.

## Wake scheduler

`gaudere::scheduling::wake::Scheduler` manages one wake-up deadline. A first request is accepted, an earlier request advances the deadline, and an equal or later request is ignored. `wait()` blocks the calling thread without polling until the deadline is due or the scheduler is permanently stopped.

The public deadline is a `std::chrono::system_clock::time_point`, so `next()` can later be used by an external persistence layer. Waiting uses `std::condition_variable::wait_until` with `system_clock`; clock corrections therefore follow the behavior of the platform's standard-library implementation.

The scheduler owns no thread, invokes no callback, and contains no persistence or application integration.

## Bounded work tasks

`gaudere::work` defines provider-agnostic task and result contracts for controlled work. A task carries an idempotency key, a kind, typed opaque input, and explicit limits for input bytes, output bytes, runtime lease duration, and attempts.

`gaudere::work::Runtime` owns the lifecycle from pending work through running, cancellation, success, failure, or manual review. Starting work creates a lease bounded by `max_runtime`; expired leases are recovered within the configured attempt budget. Output larger than `max_output_bytes` becomes a durable failure instead of an unbounded result.

`gaudere::persistence::sqlite::TaskStore` persists the task definition, lease, cancellation state, and terminal result in one SQLite row. It shares the versioned state database with the wake `ActionStore` and keeps idempotency keys unique at the database boundary.

The task contract executes no provider call and grants no network or host capability by itself.

## Namespace conventions

- The root namespace is `gaudere`.
- Nested namespaces represent explicit concepts, for example `gaudere::scheduling::wake`.
- Catch-all namespaces such as `tools`, `utils`, or `misc` are avoided.
- Generic components belong in Gaudere; project-specific adapters and integrations stay in their respective projects.

## Build

Gaudere uses Autotools and requires a C++17 compiler.

```sh
autoreconf --install
mkdir build
cd build
../configure
make
make check
```

License: MIT.
