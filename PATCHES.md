# Local changes to aWOT

This copy is vendored and modified. Anyone syncing it against upstream must reapply
what follows, or reintroduce a remotely-triggerable reset of a running inverter.

## Patch 1 — bound the header phase and service the watchdog while waiting

**Files:** `src/aWOT.h` (Request members and two static setters), `src/aWOT.cpp`
(`Request::Request` initialiser, `Request::m_timedRead`).

### The problem

Upstream has a **per-byte** timeout only (`_timeout`, 1000 ms by default), and it is
enforced by Arduino's `Stream::timedRead()`, which busy-waits.

Two consequences combine badly on this hardware:

1. A client sending one header byte just inside that window **resets the timer every
   time**, so the header phase never ends. The classic slow-loris.
2. Each wait burns up to a second with **nothing servicing the 8 s hardware
   watchdog**.

Eight dribbled bytes therefore reset the device — and on this project the device is
driving a switching bridge. No authentication is required to do it, because it
happens before a route is matched.

### The change

`m_timedRead()` no longer calls `Stream::timedRead()`. It runs the wait itself, and:

- calls `s_serviceFn` (the application's watchdog kick) on every spin;
- keeps the existing per-byte timeout, unchanged;
- additionally fails the read once `m_headerDeadline` passes.

**Two things it must keep doing, and did not in the first version of this patch:**

- It calls the **virtual `read()`**, not `m_stream->read()`. `Stream::timedRead()`
  dispatched virtually to `Request::read()`, which drains the pushback buffer the
  parser uses for lookahead, honours content-length exhaustion (`m_readingContent &&
  !m_left`), and decrements `m_left` as the body is consumed. Reading the stream
  directly skips all three and mis-parses requests — bodies over-read past their
  content-length, and pushed-back bytes are lost.
- It calls **`yield()`** on every spin. QNEthernet services its receive path from
  `yield()`; without it a request whose bytes have not already arrived can never
  arrive, and the loop spins until it times out.

Both were regressions in the first version and were caught by review before shipping.

`m_headerDeadline` is set in the constructor from `s_headerBudgetMs`, default
**4000 ms** — enormously generous for a real client, which sends headers in
milliseconds, and clear of the 8 s watchdog even if no service callback is wired, so
the defence does not depend on the application remembering to wire one.

The deadline is **deliberately not applied to the body** (`m_readingContent`). A
firmware upload is legitimately slow, and its handler services the watchdog itself.
Applying the budget there would break OTA.

Rollover safety: the deadline comparison is signed (`(long)(now - deadline) >= 0`), so
the `millis()` wrap at ~49 days cannot extend the budget indefinitely.

### Application wiring

`configureWebServer()` in `src/web_handlers.cpp`:

```cpp
Request::setServiceCallback(&kickWatchdog);
Request::setHeaderBudget(4000);
```

Both are optional. Without them the deadline still applies at its default; without
the callback the watchdog simply is not fed during the wait, which the 4 s budget is
chosen to survive.

### What this does not fix

A slow client still occupies the single connection for up to 4 s. That is a
degradation of the web interface, not of the inverter, and is the acceptable half of
the trade — the reset was the part that mattered.
