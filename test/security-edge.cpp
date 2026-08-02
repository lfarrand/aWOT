#include <ArduinoUnitTests.h>
#include <string.h>

#include "../src/aWOT.h"

class GuardClient : public Client {
 public:
  explicit GuardClient(const char *request)
      : input_(request), inputLength_(strlen(request)), readPos_(0), writePos_(0),
        stopped_(false) {
    output_[0] = '\0';
  }

  int connect(IPAddress, uint16_t) override { return 1; }
  int connect(const char *, uint16_t) override { return 1; }

  size_t write(uint8_t byte) override {
    if (writePos_ + 1 >= sizeof(output_)) {
      return 0;
    }
    output_[writePos_++] = static_cast<char>(byte);
    output_[writePos_] = '\0';
    return 1;
  }

  size_t write(const uint8_t *buffer, size_t size) override {
    const size_t room = sizeof(output_) - writePos_ - 1;
    const size_t count = size < room ? size : room;
    memcpy(output_ + writePos_, buffer, count);
    writePos_ += count;
    output_[writePos_] = '\0';
    return count;
  }

  int available() override { return static_cast<int>(inputLength_ - readPos_); }
  int read() override {
    return readPos_ < inputLength_ ? static_cast<unsigned char>(input_[readPos_++]) : -1;
  }
  int read(uint8_t *buffer, size_t size) override {
    if (readPos_ >= inputLength_) {
      return -1;
    }
    const size_t left = inputLength_ - readPos_;
    const size_t count = size < left ? size : left;
    memcpy(buffer, input_ + readPos_, count);
    readPos_ += count;
    return static_cast<int>(count);
  }
  int peek() override {
    return readPos_ < inputLength_ ? static_cast<unsigned char>(input_[readPos_]) : -1;
  }
  void flush() override {}
  void stop() override { stopped_ = true; }
  uint8_t connected() override { return stopped_ ? 0 : 1; }
  operator bool() override { return !stopped_; }

  const char *response() const { return output_; }
  bool stopped() const { return stopped_; }

 private:
  const char *input_;
  size_t inputLength_;
  size_t readPos_;
  char output_[4096];
  size_t writePos_;
  bool stopped_;
};

static int g_available = -1;
static bool g_called = false;

void largeBodyHandler(Request &, Response &res) {
  g_called = true;
  g_available = res.availableForWrite();
  res.set("Content-Length", "128");
  for (int i = 0; i < 128; ++i) {
    res.write(static_cast<uint8_t>('x'));
  }
}

void calledHandler(Request &, Response &res) {
  g_called = true;
  res.sendStatus(204);
}

unittest(custom_response_buffer_capacity_is_honoured) {
  struct GuardedBuffer {
    uint8_t before;
    uint8_t bytes[8];
    uint8_t after;
  } guarded = {0xA5, {}, 0x5A};
  char url[32] = {};
  GuardClient client("GET / HTTP/1.0" CRLF CRLF);
  Application app;
  app.get("/", &largeBodyHandler);

  g_called = false;
  g_available = -1;
  app.process(&client, url, sizeof(url), guarded.bytes, sizeof(guarded.bytes));

  assertTrue(g_called);
  assertEqual(7, g_available);
  assertEqual(0xA5, guarded.before);
  assertEqual(0x5A, guarded.after);
  assertTrue(strstr(client.response(), "HTTP/1.1 200 OK") != NULL);
}

unittest(one_byte_response_buffer_does_not_overwrite_canaries) {
  struct GuardedBuffer {
    uint8_t before;
    uint8_t byte;
    uint8_t after;
  } guarded = {0x11, 0, 0x22};
  char url[8] = {};
  GuardClient client("GET / HTTP/1.0" CRLF CRLF);
  Application app;
  app.get("/", &calledHandler);

  app.process(&client, url, sizeof(url), &guarded.byte, 1);
  assertEqual(0x11, guarded.before);
  assertEqual(0x22, guarded.after);
  assertTrue(strstr(client.response(), "204 No Content") != NULL);
}

unittest(invalid_custom_buffers_fail_safely) {
  uint8_t output[64] = {};
  struct GuardedUrl {
    uint8_t before;
    char byte;
    uint8_t after;
  } guarded = {0x33, 0, 0x44};

  GuardClient zeroUrl("GET / HTTP/1.0" CRLF CRLF);
  Application app;
  app.process(&zeroUrl, &guarded.byte, 0, output, sizeof(output));
  assertEqual(0x33, guarded.before);
  assertEqual(0x44, guarded.after);
  assertTrue(strstr(zeroUrl.response(), "414 URI Too Long") != NULL);

  GuardClient oneUrl("GET / HTTP/1.0" CRLF CRLF);
  app.process(&oneUrl, &guarded.byte, 1, output, sizeof(output));
  assertEqual(0x33, guarded.before);
  assertEqual(0x44, guarded.after);
  assertTrue(strstr(oneUrl.response(), "414 URI Too Long") != NULL);

  char url[8] = {};
  GuardClient noOutput("GET / HTTP/1.0" CRLF CRLF);
  app.process(&noOutput, url, sizeof(url), static_cast<uint8_t *>(NULL), 0);
  assertTrue(noOutput.stopped());
}

unittest(http_version_must_be_the_exact_request_line_token) {
  const char *badVersions[] = {
      "GET / NOTHTTP/1.1" CRLF CRLF,
      "GET / HTTP/9.9 1.1" CRLF CRLF,
      "GET / HTTP/1.1 suffix" CRLF CRLF,
  };

  for (size_t i = 0; i < sizeof(badVersions) / sizeof(badVersions[0]); ++i) {
    GuardClient client(badVersions[i]);
    Application app;
    app.get("/", &calledHandler);
    g_called = false;
    app.process(&client);
    assertTrue(!g_called);
    assertTrue(strstr(client.response(), "505 HTTP Version Not Supported") != NULL);
  }
}

unittest(malformed_or_control_percent_escapes_are_rejected) {
  const char *badTargets[] = {
      "GET /bad% HTTP/1.0" CRLF CRLF,
      "GET /bad%2 HTTP/1.0" CRLF CRLF,
      "GET /bad%GG HTTP/1.0" CRLF CRLF,
      "GET /bad%00 HTTP/1.0" CRLF CRLF,
      "GET /bad%0d%0aInjected HTTP/1.0" CRLF CRLF,
  };

  for (size_t i = 0; i < sizeof(badTargets) / sizeof(badTargets[0]); ++i) {
    GuardClient client(badTargets[i]);
    Application app;
    app.get("/bad", &calledHandler);
    g_called = false;
    app.process(&client);
    assertTrue(!g_called);
    assertTrue(strstr(client.response(), "414 URI Too Long") != NULL);
  }
}

static bool g_formAccepted = false;
static bool g_formCanariesIntact = false;

void malformedFormHandler(Request &req, Response &res) {
  char name[8];
  char value[8];
  g_formAccepted = req.form(name, sizeof(name), value, sizeof(value));
  res.sendStatus(g_formAccepted ? 204 : 400);
}

void boundedFormHandler(Request &req, Response &res) {
  struct FormBuffers {
    char beforeName;
    char name[4];
    char between;
    char value[4];
    char afterValue;
  } buffers = {'A', {}, 'B', {}, 'C'};

  g_formAccepted =
      req.form(buffers.name, sizeof(buffers.name), buffers.value,
               sizeof(buffers.value));
  g_formCanariesIntact = buffers.beforeName == 'A' && buffers.between == 'B' &&
                         buffers.afterValue == 'C';
  res.sendStatus(g_formAccepted ? 204 : 400);
}

unittest(overlong_form_fields_are_rejected_without_overwriting_buffers) {
  const char *body = "name-is-far-too-long=value-is-also-far-too-long";
  char request[192];
  snprintf(request, sizeof(request),
           "POST / HTTP/1.0" CRLF "Content-Length: %u" CRLF CRLF "%s",
           static_cast<unsigned>(strlen(body)), body);

  GuardClient client(request);
  Application app;
  app.post("/", &boundedFormHandler);
  g_formAccepted = true;
  g_formCanariesIntact = false;
  app.process(&client);

  assertTrue(!g_formAccepted);
  assertTrue(g_formCanariesIntact);
  assertTrue(strstr(client.response(), "400 Bad Request") != NULL);
}

unittest(malformed_or_control_form_percent_escapes_are_rejected) {
  const char *badBodies[] = {"x=%", "x=%2", "x=%GG", "x=%00", "x=%0d", "x=%0a",
                             "x=%7f"};

  for (size_t i = 0; i < sizeof(badBodies) / sizeof(badBodies[0]); ++i) {
    char request[128];
    snprintf(request, sizeof(request),
             "POST / HTTP/1.0" CRLF "Content-Length: %u" CRLF CRLF "%s",
             static_cast<unsigned>(strlen(badBodies[i])), badBodies[i]);
    GuardClient client(request);
    Application app;
    app.post("/", &malformedFormHandler);
    g_formAccepted = true;
    app.process(&client);
    assertTrue(!g_formAccepted);
    assertTrue(strstr(client.response(), "400 Bad Request") != NULL);
  }
}

unittest(ambiguous_request_framing_is_rejected_before_dispatch) {
  const char *badFraming[] = {
      "POST / HTTP/1.1" CRLF "Content-Length: 1" CRLF
          "Content-Length: 2" CRLF CRLF "xx",
      "POST / HTTP/1.1" CRLF "Transfer-Encoding: chunked" CRLF CRLF
          "0" CRLF CRLF,
      "POST / HTTP/1.1" CRLF "Content-Length: 0" CRLF
          "Transfer-Encoding: chunked" CRLF CRLF "0" CRLF CRLF,
  };

  for (size_t i = 0; i < sizeof(badFraming) / sizeof(badFraming[0]); ++i) {
    GuardClient client(badFraming[i]);
    Application app;
    app.post("/", &calledHandler);
    g_called = false;
    app.process(&client);
    assertTrue(!g_called);
    assertTrue(strstr(client.response(), "431 Request Header Fields Too Large") != NULL);
  }

  GuardClient identical("POST / HTTP/1.1" CRLF "Content-Length: 0" CRLF
                        "Content-Length: 0" CRLF CRLF);
  Application app;
  app.post("/", &calledHandler);
  g_called = false;
  app.process(&identical);
  assertTrue(g_called);
}

unittest(router_mount_requires_a_path_segment_boundary) {
  Router api;
  api.get("/x", &calledHandler);
  Application app;
  app.use("/api", &api);

  GuardClient match("GET /api/x HTTP/1.0" CRLF CRLF);
  g_called = false;
  app.process(&match);
  assertTrue(g_called);

  GuardClient collision("GET /apiary/x HTTP/1.0" CRLF CRLF);
  g_called = false;
  app.process(&collision);
  assertTrue(!g_called);
  assertTrue(strstr(collision.response(), "404 Not Found") != NULL);
}

void guardedAccessorHandler(Request &req, Response &res) {
  char canary = 'Q';
  char value = 'V';
  const bool formOk = req.form(&canary, 0, &value, 0);
  const bool queryOk = req.query("", &value, 0);
  const bool routeNameOk = req.route("", &value, 0);
  const bool routeIndexOk = req.route(-1, &value, 0);
  if (!formOk && !queryOk && !routeNameOk && !routeIndexOk &&
      canary == 'Q' && value == 'V') {
    res.sendStatus(204);
  } else {
    res.sendStatus(500);
  }
}

unittest(zero_length_request_accessors_leave_memory_untouched) {
  GuardClient client("POST /item/value?q=x HTTP/1.0" CRLF
                     "Content-Length: 0" CRLF CRLF);
  Application app;
  app.post("/item/:id", &guardedAccessorHandler);
  app.process(&client);
  assertTrue(strstr(client.response(), "204 No Content") != NULL);
}

unittest_main()
