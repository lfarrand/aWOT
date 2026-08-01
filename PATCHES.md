# Local changes to aWOT

This copy is vendored and modified. Anyone syncing it against upstream must reapply
what follows, or reintroduce a remotely-triggerable reset of a running inverter.

## Patch 3 — drop the hard QNEthernet dependency

**Files:** `src/aWOT.h` (removed `#include "QNEthernet.h"` and its `using namespace`;
`EthernetClient*` → `Client*`), `src/aWOT.cpp` (same, plus the include at the top).

### Why the dependency existed

It was added deliberately, in `1a2d9ce "Fixed truncated output"`, and it fixed a real bug.
Arduino's `Client::write(buf, len)` may accept **fewer bytes than asked for**; aWOT
ignored the return value, so whatever the first call did not take was silently dropped and
the browser got a truncated page. Switching to QNEthernet's concrete `EthernetClient` gave
access to `writeFully()`, which loops until the whole buffer is accepted.

### Why it can now go

Patch 2 replaced `writeFully()` with `m_writeBounded()`, which is itself a full-write
retry loop — it calls `write()`, advances by the returned count, and repeats until
`rem == 0`. **The truncation guarantee is unchanged**; short writes are still retried to
completion. `m_writeBounded()` additionally bails out on a dead or non-reading peer, which
`writeFully()` would not.

Once patch 2 covered all three call sites, `writeFully()` was the only QNEthernet-specific
method left in the library. Everything else `m_stream` is asked to do — `available`,
`connected`, `flush`, `print`, `read`, `stop`, `write` — is plain Arduino `Client`/`Print`.

### What it buys

The dependency made the library uncompilable anywhere without QNEthernet, which is to say
anywhere that is not a Teensy 4.x. Its own CI targets Linux and macOS hosts and an Arduino
Uno, so **every CI run had failed** with `fatal error: QNEthernet.h: No such file or
directory` for as long as the include existed — the regression suite was dark. Taking
`Client*` restores upstream's portability and lets that suite run again.

Callers are unaffected: `EthernetClient` derives from `Client`, so an `EthernetClient*`
still binds to `Application::process()` with no cast.

### Knock-on in the firmware

`src/main.h` had been relying on aWOT to pull QNEthernet in transitively. It now includes
`<QNEthernet.h>` itself. Same stack, declared where it is actually used.

## Patch 2 — bound the response write

**Files:** `src/aWOT.h` (Response members and two static setters), `src/aWOT.cpp`
(`Response::Response` initialiser, `Response::m_flushBuf`, `Response::write(uint8_t)`,
`Response::write(uint8_t*, size_t)`, new `Response::m_writeBounded`).

> **Note.** The first version of this patch converted only `m_flushBuf()`. The two
> `Response::write()` overloads kept calling `writeFully()` directly, and between them
> they carry *most* of a large response — the buffer-full path and the bulk-write path
> used by `sendAsset()` and the capture download. The stall fix therefore covered only the
> final partial buffer, and the unauthenticated reset remained reachable on any response
> bigger than one buffer. All three sites are converted now.

### The problem

`m_flushBuf()` called `EthernetClient::writeFully()`, which spins until every byte is
accepted and only gives up if the connection drops. QNEthernet's own source says of that
loop:

> "This may spin forever if `p.write()` always returns zero"

A client that completes a request and then simply **stops reading** advertises a zero TCP
window. `write()` returns 0 indefinitely, the loop never exits, and nothing services the
8 s watchdog. That is an **unauthenticated one-request reset of a running inverter**, on
any GET — no credentials, no malformed input, just a socket that goes quiet.

### The change

`m_writeBounded()` replaces `writeFully()`. It writes what the peer will take, calls
`s_serviceFn` on every spin, yields so QNEthernet can service its stack, and gives up
when either the connection drops or `s_writeBudgetMs` elapses.

On giving up it latches `m_writeStalled`, marks the response ended and stops the socket.
Subsequent flushes then **discard** their buffer instead of retrying, so the handler runs
to completion at full speed rather than stalling again on every remaining chunk — which
matters for the capture download, where a 2 MB ring would otherwise stall hundreds of
times.

Budget defaults to **3000 ms** — far longer than any healthy LAN peer needs to accept a
buffer, and short enough that even several consecutive stalled flushes stay clear of the
watchdog if no service callback is wired.

### Application wiring

```cpp
Response::setServiceCallback(&serviceControlTasks);
Response::setWriteBudget(3000);
```

### What this costs

A genuinely slow but healthy client — a browser over a congested link — can now have a
response truncated after 3 s of no progress. That is the right trade against resetting the
power stage, but it is a behaviour change: responses are no longer guaranteed complete.

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
