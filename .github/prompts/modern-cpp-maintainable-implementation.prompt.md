---
description: "Implement hpserver C++ changes with module reuse, style consistency, modern C++20 design, and maintainable extensible structure"
name: "Modern C++ Maintainable Implementation"
argument-hint: "Change request, constraints, target files/modules, and acceptance criteria"
agent: "agent"
model: "GPT-5 (copilot)"
---

Implement the requested change in this repository using these rules:

1. Reuse existing modules first. Extend code under `src/net/http` and `src/net/core` before introducing new abstractions. If a new module is necessary, explicitly justify why reuse is insufficient and keep the new surface minimal.
2. Keep style consistent with the existing project conventions in `src/` and current ownership patterns (`std::unique_ptr`, clear non-owning references).
3. Use modern C++20 design and syntax with explicit ownership, RAII, and robust error-path handling.
4. Preserve maintainability and extensibility: keep patches focused, avoid broad rewrites, and keep behavior compatibility unless explicitly requested.
5. Validate boundary conditions and non-blocking/EPOLLET semantics where relevant.
6. After edits, run targeted build/tests and summarize:
   - What changed
   - Why this approach was chosen
   - How compatibility and correctness were validated
   - Residual risks or follow-up work

Use these project references while implementing:
- [Workspace Instructions](../copilot-instructions.md)
- [Modern C++ Core Rules](../instructions/modern-cpp-core.instructions.md)
- [HTTP Connection Behavior](../../doc/http_conn.md)
- [Event Loop Notes](../../doc/loop.md)

Treat the invocation argument as the concrete change request and constraints.