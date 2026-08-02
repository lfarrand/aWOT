#include <ArduinoUnitTests.h>
#include "../src/aWOT.h"
#include "./mocks/MockStream.h"

void handler(Request & req, Response & res) {
  char * test1 = req.get("Test1");
  char * test2 = req.get("Test2");

  res.set("Content-Type", "text/plain");
  res.print(test1);
  res.print(" ");
  res.print(test2);
}

unittest(headers_ok) {
  char test1Header[6]{};
  char test2Header[6]{};

  const char *request =
    "GET / HTTP/1.0" CRLF
    "Test1: test1" CRLF
    "Test2:   test2" CRLF
    CRLF;

  const char *expected =
    "HTTP/1.1 200 OK" CRLF
    "Content-Type: text/plain" CRLF
    "Connection: close" CRLF
    CRLF
    "test1 test2";

  MockStream stream(request);
  Application app;
 
  app.header("Test1", test1Header, 6);
  app.header("Test2", test2Header, 6);
  app.get("/", &handler);
  app.process(&stream);

  assertEqual(expected, stream.response());
}

unittest(same_header_multiple_times) {
  char test1Header[12]{};
  char test2Header[6]{};

  const char *request =
    "GET / HTTP/1.0" CRLF
    "Test1: test1" CRLF
    "Test1: test1" CRLF
    "Test2:   test2" CRLF
    CRLF;

  const char *expected =
    "HTTP/1.1 200 OK" CRLF
    "Content-Type: text/plain" CRLF
    "Connection: close" CRLF
    CRLF
    "test1,test1 test2";

  MockStream stream(request);
  Application app;
 
  app.header("Test1", test1Header, 12);
  app.header("Test2", test2Header, 6);
  app.get("/", &handler);
  app.process(&stream);

  assertEqual(expected, stream.response());
}

unittest(headers_field_too_large) {
  char test1Header[6];
  char test2Header[6];

  const char *request =
    "GET / HTTP/1.0" CRLF
    "Test1: test1" CRLF
    "Test2: 123456" CRLF
    CRLF;

  const char *expected =
    "HTTP/1.1 431 Request Header Fields Too Large" CRLF
    "Content-Type: text/plain" CRLF
    "Connection: close" CRLF
    CRLF
    "Request Header Fields Too Large";

  MockStream stream(request);
  Application app;
 
  app.header("Test1", test1Header, 6);
  app.header("Test2", test2Header, 6);
  app.get("/", &handler);
  app.process(&stream);

  assertEqual(expected, stream.response());
}

unittest(repeated_header_requires_comma_and_terminator_space) {
  // Six bytes hold "test1" plus NUL, but a repeated value would also need a comma
  // and another terminator.  Reject it without writing beyond the registered object.
  struct {
    char value[6];
    char canary;
  } test1Header{{}, 'Z'};

  const char *request =
    "GET / HTTP/1.0" CRLF
    "Test1: test1" CRLF
    "Test1: x" CRLF
    CRLF;

  const char *expected =
    "HTTP/1.1 431 Request Header Fields Too Large" CRLF
    "Content-Type: text/plain" CRLF
    "Connection: close" CRLF
    CRLF
    "Request Header Fields Too Large";

  MockStream stream(request);
  Application app;
  app.header("Test1", test1Header.value, sizeof(test1Header.value));
  app.get("/", &handler);
  app.process(&stream);

  assertEqual(expected, stream.response());
  assertEqual('Z', test1Header.canary);
}

unittest_main()
