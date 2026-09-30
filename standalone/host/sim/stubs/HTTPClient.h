#pragma once
#include "Arduino.h"
#include "WiFiClientSecure.h"
#define HTTP_CODE_OK 200
String sim_http(const String &url, int *code);
class HTTPClient {
 public:
  void setTimeout(int) {}
  bool begin(WiFiClientSecure &, const String &url) { url_ = url; return true; }
  int GET() { body_ = sim_http(url_, &code_); return code_; }
  String getString() { return body_; }
  void end() {}
 private:
  String url_, body_;
  int code_ = 0;
};
