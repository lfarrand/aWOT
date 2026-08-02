#include <aWOT.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <utility>
#include <vector>

using namespace awot;

namespace {

class FuzzClient final : public Client {
 public:
  explicit FuzzClient(std::vector<uint8_t> input)
      : input_(std::move(input)), position_(0), stopped_(false) {}

  int connect(IPAddress, uint16_t) override { return 1; }
  int connect(const char *, uint16_t) override { return 1; }
  size_t write(uint8_t) override { return 1; }
  size_t write(const uint8_t *, size_t size) override { return size; }
  int available() override {
    const size_t remaining = input_.size() - position_;
    return static_cast<int>(std::min<size_t>(remaining, 0x7fffffffu));
  }
  int read() override {
    return position_ < input_.size() ? input_[position_++] : -1;
  }
  int read(uint8_t *buffer, size_t size) override {
    if (position_ >= input_.size()) {
      return -1;
    }
    const size_t count = std::min(size, input_.size() - position_);
    memcpy(buffer, input_.data() + position_, count);
    position_ += count;
    return static_cast<int>(count);
  }
  int peek() override {
    return position_ < input_.size() ? input_[position_] : -1;
  }
  void flush() override {}
  void stop() override { stopped_ = true; }
  uint8_t connected() override {
    return (!stopped_ && position_ < input_.size()) ? 1 : 0;
  }
  operator bool() override { return !stopped_; }

 private:
  std::vector<uint8_t> input_;
  size_t position_;
  bool stopped_;
};

void probeHandler(Request &request, Response &response) {
  char value[64] = {};
  (void)request.query("q", value, sizeof(value));
  (void)request.route("id", value, sizeof(value));
  (void)request.route(0, value, sizeof(value));
  (void)request.get("X-Fuzz");

  char name[32] = {};
  (void)request.form(name, sizeof(name), value, sizeof(value));
  uint8_t body[64];
  (void)request.read(body, sizeof(body));
  response.sendStatus(204);
}

void runRequest(std::vector<uint8_t> input) {
  FuzzClient client(std::move(input));
  Application app;
  app.setTimeout(0);

  char header[64] = {};
  char url[257] = {};
  uint8_t output[128] = {};
  app.header("X-Fuzz", header, sizeof(header));
  app.use(&probeHandler);
  app.process(&client, url, sizeof(url), output, sizeof(output));
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size > 16384) {
    return 0;
  }

  Request::setHeaderBudget(0);
  Response::setWriteBudget(0);
  Response::setWriteTotalBudget(0);

  std::vector<uint8_t> input;
  if (size != 0) {
    input.assign(data, data + size);
  }
  static const uint8_t terminator[] = {'\r', '\n', '\r', '\n'};
  input.insert(input.end(), terminator, terminator + sizeof(terminator));
  runRequest(std::move(input));

  static const char getPrefix[] = "GET /";
  static const char getSuffix[] =
      " HTTP/1.1\r\nHost: fuzz\r\nX-Fuzz: value\r\n\r\n";
  std::vector<uint8_t> path(getPrefix, getPrefix + sizeof(getPrefix) - 1);
  if (size != 0) {
    path.insert(path.end(), data, data + size);
  }
  path.insert(path.end(), getSuffix, getSuffix + sizeof(getSuffix) - 1);
  runRequest(std::move(path));

  char postPrefix[160];
  const int prefixLength = snprintf(
      postPrefix, sizeof(postPrefix),
      "POST /item/1?q=value HTTP/1.1\r\nHost: fuzz\r\n"
      "X-Fuzz: value\r\nContent-Type: application/x-www-form-urlencoded\r\n"
      "Content-Length: %u\r\n\r\n",
      static_cast<unsigned>(size));
  if (prefixLength > 0 && static_cast<size_t>(prefixLength) < sizeof(postPrefix)) {
    std::vector<uint8_t> body(postPrefix, postPrefix + prefixLength);
    if (size != 0) {
      body.insert(body.end(), data, data + size);
    }
    runRequest(std::move(body));
  }
  return 0;
}
