// Regression tests for the bounded response write (PATCHES.md, patches 2 and 3).
//
// These exist because of a specific history. aWOT ignored the return value of
// Client::write(), which is allowed to accept fewer bytes than it is offered, so
// responses were silently truncated in the browser. That was fixed by depending on
// QNEthernet's EthernetClient::writeFully(), which loops until everything is accepted -
// but writeFully() also spins for ever against a peer that stops reading, which on a
// Teensy driving an inverter is an unauthenticated watchdog reset.
//
// m_writeBounded() has to satisfy BOTH constraints at once: retry short writes to
// completion, and still give up on a peer that has stopped accepting. The tests below
// pin down each half, so neither can be lost again to a well-meaning simplification.

#include <ArduinoUnitTests.h>
#include <string.h>
#include "../src/aWOT.h"

static const size_t kBodyLen = 3000; // > SERVER_OUTPUT_BUFFER_SIZE, so the buffer-full
                                     // path in Response::write(uint8_t) is exercised too
static char g_body[kBodyLen + 1];

// A repeating digit pattern rather than a constant fill: a constant fill would still
// compare equal if the retry loop double-wrote or mis-advanced its pointer.
static void buildBody() {
  for (size_t i = 0; i < kBodyLen; i++) {
    g_body[i] = (char)('0' + (i % 10));
  }
  g_body[kBodyLen] = '\0';
}

// A Client that accepts at most `chunk` bytes per write(buf, size) call. This is the
// legal-but-inconvenient behaviour that Client::write() is allowed to have and that the
// original truncation bug fell over: a correct caller must loop on the return value.
//
//   chunk > 0  -> always makes progress, just slowly. Nothing may be lost.
//   chunk == 0 -> accepts nothing while still reporting connected. The stalled peer.
class ChunkedClient : public Client {
 public:
  ChunkedClient(const char *request, size_t chunk)
      : m_request(request),
        m_len(strlen(request)),
        m_pos(0),
        m_chunk(chunk),
        m_outLen(0),
        m_connected(1),
        m_stopped(false),
        m_writeCalls(0) {
    m_out[0] = '\0';
  }

  size_t write(uint8_t b) override {
    if (!m_connected || m_outLen + 1 >= sizeof(m_out)) {
      return 0;
    }
    m_out[m_outLen++] = (char)b;
    return 1;
  }

  size_t write(const uint8_t *buf, size_t size) override {
    m_writeCalls++;
    if (!m_connected) {
      return 0;
    }
    size_t n = (size < m_chunk) ? size : m_chunk;
    if (m_outLen + n >= sizeof(m_out)) {
      n = sizeof(m_out) - 1 - m_outLen;
    }
    memcpy(m_out + m_outLen, buf, n);
    m_outLen += n;
    return n;
  }

  int available() override { return (int)(m_len - m_pos); }
  int read() override { return m_pos < m_len ? (uint8_t)m_request[m_pos++] : -1; }

  int read(uint8_t *buf, size_t size) override {
    size_t n = 0;
    while (n < size && m_pos < m_len) {
      buf[n++] = (uint8_t)m_request[m_pos++];
    }
    return (int)n;
  }

  int peek() override { return m_pos < m_len ? (uint8_t)m_request[m_pos] : -1; }
  void flush() override {}

  // Matches a real stack: once the socket is closed nothing more is accepted and
  // connected() reports false. m_writeBounded relies on that to bail out promptly
  // rather than burning a fresh budget on every subsequent buffer.
  void stop() override {
    m_stopped = true;
    m_connected = 0;
  }

  uint8_t connected() override { return m_connected; }
  operator bool() override { return m_connected != 0; }
  int connect(IPAddress, uint16_t) override { return 1; }
  int connect(const char *, uint16_t) override { return 1; }

  void disconnect() { m_connected = 0; }
  const char *response() { m_out[m_outLen] = '\0'; return m_out; }
  size_t responseLength() const { return m_outLen; }
  bool stopped() const { return m_stopped; }
  unsigned long writeCalls() const { return m_writeCalls; }

 private:
  const char *m_request;
  size_t m_len;
  size_t m_pos;
  size_t m_chunk;
  char m_out[8192];
  size_t m_outLen;
  uint8_t m_connected;
  bool m_stopped;
  unsigned long m_writeCalls;
};

// Accepts exactly one byte every `intervalMs`, then refuses until the next tick.
// It always makes progress, so it never trips the no-progress budget - which is
// precisely why that budget cannot be the only one. Left unbounded, this peer holds
// the write loop for bodyBytes x intervalMs: hours for a gzipped page, months for
// the capture download, on one unauthenticated GET.
class DripClient : public Client {
 public:
  DripClient(const char *request, unsigned long intervalMs)
      : m_request(request),
        m_len(strlen(request)),
        m_pos(0),
        m_interval(intervalMs),
        m_lastAccept(0),
        m_outLen(0),
        m_connected(1),
        m_stopped(false) {}

  size_t write(uint8_t b) override {
    if (!m_connected || m_outLen + 1 >= sizeof(m_out)) {
      return 0;
    }
    m_out[m_outLen++] = (char)b;
    return 1;
  }

  size_t write(const uint8_t *buf, size_t size) override {
    if (!m_connected || size == 0) {
      return 0;
    }
    const unsigned long now = millis();
    if (m_lastAccept != 0 && (now - m_lastAccept) < m_interval) {
      return 0;
    }
    m_lastAccept = now;
    if (m_outLen + 1 >= sizeof(m_out)) {
      return 0;
    }
    m_out[m_outLen++] = (char)buf[0];
    return 1; // one byte, just often enough to look alive
  }

  int available() override { return (int)(m_len - m_pos); }
  int read() override { return m_pos < m_len ? (uint8_t)m_request[m_pos++] : -1; }
  int read(uint8_t *buf, size_t size) override {
    size_t n = 0;
    while (n < size && m_pos < m_len) { buf[n++] = (uint8_t)m_request[m_pos++]; }
    return (int)n;
  }
  int peek() override { return m_pos < m_len ? (uint8_t)m_request[m_pos] : -1; }
  void flush() override {}
  void stop() override { m_stopped = true; m_connected = 0; }
  uint8_t connected() override { return m_connected; }
  operator bool() override { return m_connected != 0; }
  int connect(IPAddress, uint16_t) override { return 1; }
  int connect(const char *, uint16_t) override { return 1; }
  bool stopped() const { return m_stopped; }

 private:
  const char *m_request;
  size_t m_len;
  size_t m_pos;
  unsigned long m_interval;
  unsigned long m_lastAccept;
  char m_out[8192];
  size_t m_outLen;
  uint8_t m_connected;
  bool m_stopped;
};

// stop() does NOT drop connected(), like aWOT's own StreamClient. If the write
// paths do not short-circuit on m_writeStalled, every later buffer re-enters the
// socket and burns a fresh budget.
class NonDroppingClient : public ChunkedClient {
 public:
  NonDroppingClient(const char *request) : ChunkedClient(request, 0), m_stops(0) {}
  void stop() override { m_stops++; } // deliberately keeps connected() == 1
  int stops() const { return m_stops; }

 private:
  int m_stops;
};

// Claims to have accepted more than it was offered. Must be treated as an error,
// never as completion - clamping and reporting success would be silent truncation.
class LyingClient : public ChunkedClient {
 public:
  LyingClient(const char *request) : ChunkedClient(request, 0) {}
  size_t write(const uint8_t *buf, size_t size) override {
    (void)buf;
    return size + 1;
  }
};

static const char *kRequest = "GET / HTTP/1.0" CRLF CRLF;

static const char *kExpectedHead =
    "HTTP/1.1 200 OK" CRLF
    "Content-Type: text/plain" CRLF
    "Connection: close" CRLF
    CRLF;

// Writes the body one byte at a time, so the response goes out through the
// buffer-full branch of Response::write(uint8_t).
void byteBodyHandler(Request &req, Response &res) {
  for (size_t i = 0; i < kBodyLen; i++) {
    res.write((uint8_t)g_body[i]);
  }
}

// Writes the body in one call, so the response goes out through
// Response::write(uint8_t*, size_t) - the path sendAsset() and the capture download use.
void bulkBodyHandler(Request &req, Response &res) {
  res.write((uint8_t *)g_body, kBodyLen);
}

// A deliberately small body for the one-byte-trickle case. The host's yield() is a
// usleep with millisecond-or-worse granularity, so a 3000-byte body at one byte per
// iteration takes ~30 s here and trips the absolute ceiling - a property of the test
// harness, not of the code. 200 bytes still proves every byte is retried in order.
static const size_t kTrickleLen = 200;

void trickleBodyHandler(Request &req, Response &res) {
  res.write((uint8_t *)g_body, kTrickleLen);
}

static char g_expected[8192];

static void buildExpected() {
  strcpy(g_expected, kExpectedHead);
  strcat(g_expected, g_body);
}

// A macro, not a function: the assert* macros expand to a call on the test-case object
// and only compile inside a unittest() body.
#define EXPECT_FULL_BODY(client)                            \
  do {                                                      \
    buildExpected();                                        \
    assertEqual(strlen(g_expected), (client).responseLength()); \
    assertEqual(g_expected, (client).response());            \
  } while (0)

// The anti-truncation half. A peer that accepts only 7 bytes at a time must still
// receive every byte, in order. This fails if anyone replaces m_writeBounded() with a
// single unchecked write() - which is exactly the regression that motivated depending
// on QNEthernet in the first place.
unittest(short_writes_are_retried_to_completion_byte_path) {
  buildBody();
  Response::setWriteBudget(3000);
  ChunkedClient client(kRequest, 7);
  Application app;

  app.get("/", &byteBodyHandler);
  app.process(&client);

  EXPECT_FULL_BODY(client);
}

unittest(short_writes_are_retried_to_completion_bulk_path) {
  buildBody();
  Response::setWriteBudget(3000);
  ChunkedClient client(kRequest, 7);
  Application app;

  app.get("/", &bulkBodyHandler);
  app.process(&client);

  EXPECT_FULL_BODY(client);
}

// A single-byte trickle is the pathological-but-legal case, and the one most likely to
// expose an off-by-one in the pointer advance.
unittest(single_byte_writes_are_retried_to_completion) {
  buildBody();
  Response::setWriteBudget(3000);
  Response::setWriteTotalBudget(30000);
  ChunkedClient client(kRequest, 1);
  Application app;

  app.get("/", &trickleBodyHandler);
  app.process(&client);

  static char expected[8192];
  strcpy(expected, kExpectedHead);
  strncat(expected, g_body, kTrickleLen);
  assertEqual(strlen(expected), client.responseLength());
  assertEqual(expected, client.response());
}

// The anti-hang half. A peer that stays connected but accepts nothing must not be
// retried for ever. Before this bound, this test would never return - which on the
// target is the watchdog reset of a running inverter.
unittest(stalled_peer_gives_up_within_budget) {
  buildBody();
  Response::setWriteBudget(50);
  ChunkedClient client(kRequest, 0); // connected, accepts nothing
  Application app;

  app.get("/", &bulkBodyHandler);

  unsigned long start = millis();
  app.process(&client);
  unsigned long elapsed = millis() - start;

  // Generous ceiling: what matters is that it terminates at all, and in a time
  // proportional to the budget rather than to the size of the response.
  assertLess(elapsed, (unsigned long)3000);
  assertTrue(client.stopped());

  Response::setWriteBudget(3000);
}

// A peer that has gone away should be abandoned immediately, without waiting out the
// budget at all - connected() going false is the fast path out of the retry loop.
unittest(disconnected_peer_aborts_without_waiting_out_the_budget) {
  buildBody();
  Response::setWriteBudget(5000);
  ChunkedClient client(kRequest, 0);
  client.disconnect();
  Application app;

  app.get("/", &bulkBodyHandler);

  unsigned long start = millis();
  app.process(&client);
  unsigned long elapsed = millis() - start;

  assertLess(elapsed, (unsigned long)1000);

  Response::setWriteBudget(3000);
}

// The no-progress budget on its own bounds NOTHING. This peer accepts a byte often
// enough to keep resetting it, so without an absolute ceiling the write loop runs
// for bodyBytes x dripInterval - 150 s for this small body, and days for a real one.
// Everything in loop() that is not a control task starves for the duration.
unittest(dripping_peer_is_bounded_by_the_total_budget) {
  buildBody();
  Response::setWriteBudget(300);      // never tripped: the peer drips every 50 ms
  Response::setWriteTotalBudget(400); // the ceiling that actually ends it
  DripClient client(kRequest, 50);
  Application app;

  app.get("/", &bulkBodyHandler);

  unsigned long start = millis();
  app.process(&client);
  unsigned long elapsed = millis() - start;

  assertLess(elapsed, (unsigned long)3000);
  assertTrue(client.stopped());

  Response::setWriteBudget(3000);
  Response::setWriteTotalBudget(30000);
}

// Once stalled, later buffers must be discarded rather than re-entering the socket.
// If they re-enter, boundedness quietly depends on the concrete client dropping
// connected() inside stop() - which QNEthernet does and aWOT's own StreamClient does
// not. One stop() is the observable signature of getting this right.
unittest(stalled_response_does_not_reenter_a_socket_that_stays_connected) {
  buildBody();
  Response::setWriteBudget(100);
  Response::setWriteTotalBudget(30000);
  NonDroppingClient client(kRequest);
  Application app;

  app.get("/", &byteBodyHandler);

  unsigned long start = millis();
  app.process(&client);
  unsigned long elapsed = millis() - start;

  assertEqual(1, client.stops());
  assertLess(elapsed, (unsigned long)3000);

  Response::setWriteBudget(3000);
}

// A Client that claims more than it was offered must fail the write, not be clamped
// into apparent success - clamping produces exactly the silent truncation this whole
// patch exists to prevent, and would run the buffer pointer past its end.
unittest(client_claiming_more_than_offered_is_an_error_not_success) {
  buildBody();
  Response::setWriteBudget(3000);
  Response::setWriteTotalBudget(30000);
  LyingClient client(kRequest);
  Application app;

  app.get("/", &bulkBodyHandler);
  app.process(&client);

  assertTrue(client.stopped());
}

unittest_main()
