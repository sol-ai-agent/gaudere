# Gaudere

Gaudere is a C++17 library for generic, reusable components shared across projects.

Its core remains independent from application-specific concerns, including Second Life integration, and from AI-provider APIs. The first planned domain is scheduling and wake-up behavior; no scheduler is implemented yet.

## Namespace conventions

- The root namespace is `gaudere`.
- Nested namespaces represent explicit concepts, for example `gaudere::scheduling::wake`.
- Catch-all namespaces such as `tools`, `utils`, or `misc` are avoided.
- Generic components belong in Gaudere; project-specific adapters and integrations stay in their respective projects.

## Build

Gaudere uses Autotools and requires a C++17 compiler.

License: MIT.
