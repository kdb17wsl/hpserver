---
description: "Use when editing C/C++ code in hpserver for modern C++ refactor, RAII, ownership semantics, error-path handling, and boundary-condition safety. Keywords: modern cpp, RAII, ownership, error path, edge case, 重构, 现代C++"
name: "Modern C++ Core Rules"
applyTo: "**/*.{h,hpp,hxx,c,cc,cpp,cxx}"
---
# Modern C++ Core Rules (hpserver)

Apply these rules when modifying C/C++ files. Prioritize behavior compatibility, minimal necessary change, and testable outcomes.

## 1. RAII First

- Replace manual acquire/release patterns with RAII wrappers when ownership is local and clear.
- Avoid raw `new`/`delete` in business logic; encapsulate resources in object lifetime.
- Ensure cleanup is exception-safe and early-return-safe through destructors.
- Prefer standard wrappers (`std::unique_ptr`, `std::lock_guard`, containers) over custom cleanup code unless required.

## 2. Ownership Clarity

- Make ownership explicit at API boundaries.
- Use `std::unique_ptr` for exclusive ownership.
- Use `std::shared_ptr` only for real shared lifetime, not convenience.
- Use raw pointers/references as non-owning views; never imply ownership with raw pointers.
- Avoid hidden ownership transfer; if transfer is intended, represent it explicitly in signature and call site.

## 3. Error-Path Discipline

- Handle all syscall/library failures explicitly and close to the failure site.
- Keep success path readable; use early returns for error exits.
- Preserve and propagate actionable error context (errno/status/cause) when useful.
- Do not swallow errors silently.
- For interruptible/non-blocking syscalls, distinguish retryable conditions (`EINTR`, `EAGAIN`, `EWOULDBLOCK`) from hard failures according to existing reactor semantics.

## 4. Boundary-Condition Safety

- Validate fd/index/size boundaries before access.
- Keep MAX_FD and direct-fd-indexing constraints intact when touching connection storage.
- Check empty/overflow/truncation paths for parser and buffer operations.
- Under EPOLLET flows, preserve drain-until-would-block behavior.
- Treat timeout, half-close, and repeated-event scenarios as first-class test cases.

## 5. Refactor Scope Guardrails

- Refactor only where measurable safety/maintainability benefit exists.
- Do not perform wide style-only rewrites.
- Keep public behavior stable unless interface change is necessary and synchronized with call sites/tests.
- Prefer small patches that are easy to review and verify.

## 6. Validation Requirements

- After meaningful refactor chunks, run relevant build targets and related tests.
- If behavior-sensitive paths changed, update/add focused tests.
- In summaries, include: what changed, why it was necessary, how behavior compatibility was validated, and any residual risk.
