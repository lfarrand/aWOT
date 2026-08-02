#include <ArduinoUnitTests.h>
#include <string.h>

#include "../src/aWOT.h"
#include "./mocks/MockStream.h"

struct StatusCase {
  int code;
  const char *reason;
};

static const StatusCase kStatuses[] = {
    {100, "Continue"},
    {101, "Switching Protocols"},
    {102, "Processing"},
    {103, "Early Hints"},
    {200, "OK"},
    {201, "Created"},
    {202, "Accepted"},
    {203, "Non-Authoritative Information"},
    {204, "No Content"},
    {205, "Reset Content"},
    {206, "Partial Content"},
    {207, "Multi-Status"},
    {208, "Already Reported"},
    {226, "IM Used"},
    {300, "Multiple Choices"},
    {301, "Moved Permanently"},
    {302, "Found"},
    {303, "See Other"},
    {304, "Not Modified"},
    {305, "Use Proxy"},
    {306, "(Unused)"},
    {307, "Temporary Redirect"},
    {308, "Permanent Redirect"},
    {400, "Bad Request"},
    {401, "Unauthorized"},
    {402, "Payment Required"},
    {403, "Forbidden"},
    {404, "Not Found"},
    {405, "Method Not Allowed"},
    {406, "Not Acceptable"},
    {407, "Proxy Authentication Required"},
    {408, "Request Timeout"},
    {409, "Conflict"},
    {410, "Gone"},
    {411, "Length Required"},
    {412, "Precondition Failed"},
    {413, "Payload Too Large"},
    {414, "URI Too Long"},
    {415, "Unsupported Media Type"},
    {416, "Range Not Satisfiable"},
    {417, "Expectation Failed"},
    {421, "Misdirected Request"},
    {422, "Unprocessable Entity"},
    {423, "Locked"},
    {424, "Failed Dependency"},
    {425, "Too Early"},
    {426, "Upgrade Required"},
    {428, "Precondition Required"},
    {429, "Too Many Requests"},
    {431, "Request Header Fields Too Large"},
    {451, "Unavailable For Legal Reasons"},
    {500, "Internal Server Error"},
    {501, "Not Implemented"},
    {502, "Bad Gateway"},
    {503, "Service Unavailable"},
    {504, "Gateway Timeout"},
    {505, "HTTP Version Not Supported"},
    {506, "Variant Also Negotiates"},
    {507, "Insufficient Storage"},
    {508, "Loop Detected"},
    {510, "Not Extended"},
    {511, "Network Authentication Required"},
    {599, "599"},
};

void statusHandler(Request &req, Response &res) {
  const StatusCase *status = static_cast<const StatusCase *>(req.context);
  res.sendStatus(status->code);
}

unittest(all_status_reasons_and_unknown_fallback) {
  const char *request = "GET / HTTP/1.1" CRLF CRLF;

  for (size_t i = 0; i < sizeof(kStatuses) / sizeof(kStatuses[0]); ++i) {
    MockStream stream(request);
    Application app;
    app.get("/", &statusHandler);
    app.process(&stream, const_cast<StatusCase *>(&kStatuses[i]));

    char expected[96];
    snprintf(expected, sizeof(expected), "HTTP/1.1 %d %s", kStatuses[i].code,
             kStatuses[i].reason);
    assertTrue(strstr(stream.response(), expected) != NULL);
  }
}

static bool g_responseApiOk = false;

void responseApiHandler(Request &, Response &res) {
  g_responseApiOk = !res.headersSent() && !res.ended() && res.statusSent() == 0 &&
                    res.availableForWrite() == SERVER_OUTPUT_BUFFER_SIZE - 1;

  res.set("X-First", "one");
  res.set("x-second", "two");
  g_responseApiOk = g_responseApiOk && strcmp(res.get("x-first"), "one") == 0 &&
                    strcmp(res.get("X-SECOND"), "two") == 0 && res.get("missing") == NULL;

  // Exercise the fixed-capacity guard without changing the first two entries.
  for (int i = 0; i < SERVER_MAX_HEADERS + 2; ++i) {
    res.set("X-Fill", "v");
  }

  res.status(201);
  res.status(500);  // the first final status is immutable
  g_responseApiOk = g_responseApiOk && res.statusSent() == 201;
  res.setDefaults();
  res.beginHeaders();
  res.endHeaders();
  g_responseApiOk = g_responseApiOk && res.headersSent();

  static const unsigned char flashText[] = "flash";
  res.printP(flashText);
  res.printP("-");
  res.writeP(reinterpret_cast<const unsigned char *>("data"), 4);
  res.flush();
  res.end();
  g_responseApiOk = g_responseApiOk && res.ended() && res.bytesSent() >= 10;
}

unittest(response_public_api_and_header_lookup) {
  g_responseApiOk = false;
  MockStream stream("GET / HTTP/1.0" CRLF CRLF);
  Application app;
  app.get("/", &responseApiHandler);
  app.process(&stream);

  assertTrue(g_responseApiOk);
  assertTrue(strstr(stream.response(), "HTTP/1.1 201 Created") != NULL);
  assertTrue(strstr(stream.response(), "flash-data") != NULL);
}

static bool g_requestApiOk = false;

void requestApiHandler(Request &req, Response &res) {
  char routeValue[8] = {};
  char queryValue[8] = {};
  uint8_t body[8] = {};

  g_requestApiOk = req.method() == Request::POST && req.minorVersion() == 1 &&
                   strcmp(req.path(), "/items/value") == 0 &&
                   strcmp(req.query(), "q=answer") == 0 &&
                   req.query("q", queryValue, sizeof(queryValue)) &&
                   strcmp(queryValue, "answer") == 0 &&
                   !req.query("absent", queryValue, sizeof(queryValue)) &&
                   req.route("id", routeValue, sizeof(routeValue)) &&
                   strcmp(routeValue, "value") == 0 &&
                   !req.route("absent", routeValue, sizeof(routeValue)) &&
                   !req.route(9, routeValue, sizeof(routeValue)) &&
                   req.get("missing") == NULL && req.stream() != NULL &&
                   req.availableForWrite() >= 0;

  const int before = req.bytesRead();
  const int first = req.peek();
  g_requestApiOk = g_requestApiOk && first == 'a' && req.left() == 4;
  req.push('Z');
  const int count = req.read(body, sizeof(body));
  g_requestApiOk = g_requestApiOk && count == 5 && body[0] == 'Z' && body[1] == 'a' &&
                   body[4] == 'd' && req.read() == -1 && req.available() == 0 &&
                   req.bytesRead() >= before + 4;

  req.write((uint8_t)'!');
  uint8_t suffix[] = {'O', 'K'};
  req.write(suffix, sizeof(suffix));
  req.flush();
}

unittest(request_public_api_pushback_and_bounded_bulk_read) {
  g_requestApiOk = false;
  const char *request =
      "POST /items/value?q=answer HTTP/1.1" CRLF
      "Content-Length: 4" CRLF CRLF
      "abcdTRAILING";
  MockStream stream(request);
  Application app;
  app.post("/items/:id", &requestApiHandler);
  app.process(&stream);

  assertTrue(g_requestApiOk);
  assertTrue(strstr(stream.response(), "!OK") != NULL);
}

void simpleHandler(Request &, Response &res) { res.print("hit"); }
void finalHandler(Request &, Response &res) { res.print("-final"); }

unittest(registration_and_process_overloads) {
  Application app;
  Router router;

  // Register every pathless and path-taking convenience overload. Registration
  // itself is part of the public API; only the matching GET is dispatched below.
  router.del("/d", &simpleHandler); router.del(&simpleHandler);
  router.get("/g", &simpleHandler); router.get(&simpleHandler);
  router.head("/h", &simpleHandler); router.head(&simpleHandler);
  router.options("/o", &simpleHandler); router.options(&simpleHandler);
  router.patch("/x", &simpleHandler); router.patch(&simpleHandler);
  router.post("/p", &simpleHandler); router.post(&simpleHandler);
  router.put("/u", &simpleHandler); router.put(&simpleHandler);
  router.use("/all", &simpleHandler); router.use(&simpleHandler);
  Router nested;
  nested.get("/n", &simpleHandler);
  router.use("/nested", &nested);
  router.use(&nested);

  app.del("/d", &simpleHandler); app.del(&simpleHandler);
  app.get("/g", &simpleHandler); app.get(&simpleHandler);
  app.head("/h", &simpleHandler); app.head(&simpleHandler);
  app.options("/o", &simpleHandler); app.options(&simpleHandler);
  app.patch("/x", &simpleHandler); app.patch(&simpleHandler);
  app.post("/p", &simpleHandler); app.post(&simpleHandler);
  app.put("/u", &simpleHandler); app.put(&simpleHandler);
  app.use("/all", &simpleHandler); app.use(&simpleHandler);
  app.use("/router", &router); app.use(&router);
  app.finally(&finalHandler);
  app.setTimeout(20);

  app.process(static_cast<Client *>(NULL));
  app.process(static_cast<Client *>(NULL), static_cast<char *>(NULL), 0);
  app.process(static_cast<Client *>(NULL), static_cast<char *>(NULL), 0,
              static_cast<uint8_t *>(NULL), 0);
  app.process(static_cast<Stream *>(NULL));
  app.process(static_cast<Stream *>(NULL), static_cast<char *>(NULL), 0);
  app.process(static_cast<Stream *>(NULL), static_cast<char *>(NULL), 0,
              static_cast<uint8_t *>(NULL), 0);

  MockStream stream("GET /g HTTP/1.0" CRLF CRLF);
  StreamClient client(&stream);
  char url[32];
  uint8_t output[SERVER_OUTPUT_BUFFER_SIZE];
  app.process(&client, url, sizeof(url), output, sizeof(output));
  assertTrue(strstr(stream.response(), "hit") != NULL);
  assertTrue(strstr(stream.response(), "-final") != NULL);
}

unittest(case_insensitive_compare_ordering) {
  static const unsigned char abc[] PROGMEM = "AbC";
  static const unsigned char abd[] PROGMEM = "abd";
  assertEqual(0, Application::strcmpi("aBc", "AbC"));
  assertEqual(-1, Application::strcmpi("abc", "abd"));
  assertEqual(1, Application::strcmpi("abe", "abd"));
  assertEqual(0, Application::strcmpiP("aBc", abc));
  assertEqual(-1, Application::strcmpiP("abc", abd));
  assertEqual(1, Application::strcmpiP("abe", abd));
}

unittest(parser_error_responses) {
  struct ErrorCase { const char *request; const char *status; int urlSize; };
  const ErrorCase errors[] = {
      {"BREW / HTTP/1.0" CRLF CRLF, "400 Bad Request", 32},
      {"GET /toolong HTTP/1.0" CRLF CRLF, "414 URI Too Long", 5},
      {"GET / HTTP/9.9" CRLF CRLF, "505 HTTP Version Not Supported", 32},
      {"POST / HTTP/1.0" CRLF "Content-Length: -1" CRLF CRLF,
       "431 Request Header Fields Too Large", 32},
      {"POST / HTTP/1.0" CRLF "Content-Length: 999999999999999999999" CRLF CRLF,
       "431 Request Header Fields Too Large", 32},
      {"POST / HTTP/1.0" CRLF "Content-Length:" CRLF CRLF,
       "431 Request Header Fields Too Large", 32},
  };

  for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
    MockStream stream(errors[i].request);
    Application app;
    char url[32];
    app.process(&stream, url, errors[i].urlSize);
    assertTrue(strstr(stream.response(), errors[i].status) != NULL);
  }
}

void decodedHandler(Request &req, Response &res) {
  char name[16];
  char value[16];
  res.print(req.path());
  res.print("|");
  if (req.form(name, sizeof(name), value, sizeof(value))) {
    res.print(name);
    res.print("=");
    res.print(value);
  }
}

unittest(url_and_form_percent_decoding) {
  const char *request =
      "POST /a%2Fb HTTP/1.0" CRLF
      "Content-Length: 17" CRLF CRLF
      "first+name=A%2fB";
  MockStream stream(request);
  Application app;
  app.post("/a/b", &decodedHandler);
  app.process(&stream);
  assertTrue(strstr(stream.response(), "/a/b|first name=A/B") != NULL);
}

unittest_main()
