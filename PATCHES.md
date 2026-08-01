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

`m_writeBounded()` replaces `writeFully()`. It writes what the peer will take, retries
the remainder, calls `s_serviceFn` on every spin, yields so QNEthernet can service its
stack, and gives up when either the connection drops or the peer has accepted **nothing**
for `s_writeBudgetMs`.

**Two budgets are required, and each was got wrong once before this settled.**

| Budget | Bounds | Default | Why it alone is not enough |
|---|---|---|---|
| `s_writeBudgetMs` | time with the peer accepting **nothing**; reset on every byte | 3 s | Bounds nothing on its own. A peer accepting one byte every 2.9 s resets it for ever. |
| `s_writeTotalBudgetMs` | one whole **response**, absolute, from `Response` construction | 30 s | On its own it truncates an honest slow peer — the original bug. |

The first version budgeted *total* time in the call. `Client::write()` may legally accept
only part of what it is offered, so a large body goes out over many partial writes, and a
peer reading steadily but slowly accumulated elapsed time without ever stalling — cut off
mid-body, which is exactly the truncation the `writeFully()` dependency existed to
prevent. A host test caught it: 3000 bytes at 7 bytes per write lost more than half.

The obvious repair — reset the deadline on every byte accepted — fixed the truncation and
**removed the bound entirely**. Hold time became `bodyBytes × dripInterval`: measured at
24 s for a 200-byte body at an 80 ms drip, and by construction unbounded. At the shipped
budget a 40 KB page works out at roughly 33 hours and the 2 MB capture download at about
67 days, on one unauthenticated GET. That does not reset the board, because `s_serviceFn`
keeps kicking the watchdog — but everything in the host's `loop()` that is *not* a control
task starves for the whole hold. This is the same slow-drip class the request side already
defends against with an absolute `m_headerDeadline`; the write side needed the same thing.

So: reset-on-progress **and** a hard ceiling. Neither alone is correct.

On giving up it latches `m_writeStalled`, marks the response ended and stops the socket.
All three write sites then **discard** their buffer instead of retrying, so the handler
runs to completion at full speed rather than stalling again on every remaining chunk —
which matters for the capture download, where a 2 MB ring would otherwise stall hundreds
of times.

That entry check lives in all three sites deliberately. With it only in `m_flushBuf()`,
the two `Response::write()` overloads re-entered the socket on every later buffer, and
boundedness rested entirely on the concrete client making `connected()` false inside
`stop()`. QNEthernet does; **aWOT's own `StreamClient` does not** (`stop()` is a no-op and
`connected()` returns 1). Measured against such a client, a 3000-byte body burned one full
budget *per buffer* — the very coupling to a concrete client type that patch 3 removes.

A `Client` returning more than it was offered is treated as a hard error, not clamped to
the remaining count. Clamping would exit the loop with `rem == 0` and report success —
silent truncation — and the unsigned subtraction would run the buffer pointer off the end.

### What a caller can observe

`bytesSent()` counts what the handler produced, **not** what reached the peer; after a
stall those differ completely. `stalled()` is how a caller tells. Both `write()` overloads
report their full length back to the handler on the stall path, which is what lets it
drain quickly instead of blocking chunk by chunk.

### Known gaps

- The chunked framing — the length prefix and CRLFs — goes out through
  `m_stream->print(...)`, whose short return is ignored, so the anti-truncation guarantee
  covers the payload only. Pre-existing, and **not reachable in TEG**: no handler sets
  `Connection: keep-alive`, so `setDefaults()` forces `m_contentLengthSet` and every
  chunked branch is dead in the firmware.
- QNEthernet's `EthernetClient::stop()` blocks for up to its 1000 ms connection timeout
  waiting for a FIN that a zero-window peer cannot acknowledge, and it does **not** call
  `s_serviceFn` while waiting. One second of unserviced control tasks immediately after a
  stall. Clear of the 8 s watchdog, but worth knowing.

### Tests

`test/write-bounded.cpp` pins every property that has been broken at least once:

- short writes retried to completion on the byte path, the bulk path, and a
  one-byte-at-a-time trickle — the anti-truncation property;
- bounded give-up against a connected-but-silent peer, and immediate abort against a
  disconnected one;
- **an absolute bound against a dripping peer** that keeps resetting the no-progress
  budget — the regression that reset-on-progress introduced;
- exactly one `stop()` against a client whose `stop()` does not drop `connected()`;
- failure, not success, from a client claiming to have accepted more than it was offered.

`test/CMakeLists.txt` globs `*.cpp`, so the file registers itself.

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
