# Gaudere

Gaudere is a C++17 library for generic, reusable components shared across projects.

Its core remains independent from application-specific concerns, including Second Life integration, and from AI-provider APIs.

## Wake scheduler

`gaudere::scheduling::wake::Scheduler` manages one wake-up deadline. A first request is accepted, an earlier request advances the deadline, and an equal or later request is ignored. `wait()` blocks the calling thread without polling until the deadline is due or the scheduler is permanently stopped.

The public deadline is a `std::chrono::system_clock::time_point`, so `next()` can later be used by an external persistence layer. Waiting uses `std::condition_variable::wait_until` with `system_clock`; clock corrections therefore follow the behavior of the platform's standard-library implementation.

The scheduler owns no thread, invokes no callback, and contains no persistence or application integration.

## Durable exact wake intents

`gaudere::scheduling::wake::WakeIntentRuntime` accepts a bounded delay only inside
an application-selected capability scope. It samples an injected clock once,
persists the derived millisecond deadline through `WakeIntentStore`, and exposes the
earliest scheduled deadline for an event-driven scheduler. The runtime performs no
provider call, callback, task submission, or external effect.

Acceptance is idempotent by source identity and atomically enforces a caller-fixed
lifetime total per scope. Accepted rows are never deleted or recycled. A scheduled
intent may become only `fired`, `revoked`, or `manual_review`; revocation does not
refund its slot. At or after the exact deadline, firing wins over revocation. A
detected wall-clock rollback before the durable acceptance time fails closed into
manual review.

`gaudere::persistence::sqlite::WakeIntentStore` implements those admission and
terminal transitions transactionally in additive SQLite schema v4. Constraints
and triggers prevent a terminal insert, deadline mutation, second transition, or
row deletion. Restart can re-arm a future deadline or fire an overdue intent
exactly once. Firing records observable durable state only and grants no authority
to create successor work.

For recovery observability, `WakeIntentRuntime::inspect_scope()` exposes only its
constructor-fixed scope. The store returns `empty`, exactly `one` validated intent,
or `ambiguous` with no selected record. SQLite orders the read and applies `LIMIT 2`;
the operation changes no schema or durable bytes and performs no reconciliation.

## Recoverable external effects

`gaudere::scheduling::wake::Runtime` separates work that is still safe to retry from an external effect that may already have happened. A running `Action` starts with `EffectResult::none`. Immediately before crossing a side-effect boundary, the caller records `record_effect_started()`, which durably changes the effect result to `unknown` while the action remains leased and running.

That marker is deliberately conservative. If the process dies after it is persisted, lease recovery moves the action to `manual_review` rather than `retry_wait`, so a potentially billed, sent, or otherwise externally visible operation is never repeated blindly. A definite response is completed through `record_confirmed_result()`, which atomically records `EffectResult::confirmed`, marks the action succeeded, and releases its lease. `record_unknown_result()` immediately moves an ambiguous result to manual review.

Generic transitions cannot turn an `unknown` external effect into a retry or success; explicit confirmation is required. Effect-free actions may continue to use ordinary lifecycle transitions.

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
