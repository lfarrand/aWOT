#include <ArduinoUnitTests.h>
#include "../src/aWOT.h"
#include "./mocks/MockStream.h"

static bool called;

static void postHandler(Request &, Response &res) {
  called = true;
  res.sendStatus(204);
}

static bool isRejected(const char *contentLength) {
  char request[128]{};
  snprintf(request, sizeof(request),
           "POST / HTTP/1.0" CRLF "Content-Length: %s" CRLF CRLF,
           contentLength);

  const char *expected =
    "HTTP/1.1 431 Request Header Fields Too Large" CRLF
    "Content-Type: text/plain" CRLF
    "Connection: close" CRLF
    CRLF
    "Request Header Fields Too Large";

  called = false;
  MockStream stream(request);
  Application app;
  app.post("/", &postHandler);
  app.process(&stream);

  return !called && strcmp(expected, stream.response()) == 0;
}

unittest(negative_content_length_is_rejected) {
  assertTrue(isRejected("-1"));
}

unittest(signed_positive_content_length_is_rejected) {
  assertTrue(isRejected("+1"));
}

unittest(overflowing_content_length_is_rejected) {
  assertTrue(isRejected("2147483648"));
}

unittest_main()
