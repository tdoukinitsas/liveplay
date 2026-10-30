// =============================================================================
// events/plugin_http.cpp — HTTP requests (Postman-style)
// -----------------------------------------------------------------------------
// A connection is a base URL plus headers sent with every request (an API key,
// say); each event adds a method, path, its own headers and a body. Plain HTTP
// only: show-control gear on a LAN rarely speaks TLS, and HTTPS would mean
// shipping OpenSSL in the server.
// =============================================================================
#include "liveplay/events/plugin.hpp"

#include <httplib.h>

#include <algorithm>
#include <sstream>

namespace liveplay::events {
namespace {

std::string trim(std::string s) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

// "Name: value" per line.
void add_header_lines(httplib::Headers& headers, const std::string& text) {
    std::istringstream in{text};
    std::string line;
    while (std::getline(in, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        auto name = trim(line.substr(0, colon));
        if (!name.empty()) headers.emplace(std::move(name), trim(line.substr(colon + 1)));
    }
}

class HttpPlugin final : public Plugin {
public:
    std::string id() const override { return "http"; }

    json schema() const override {
        return json{
            {"id", "http"},
            {"label", "HTTP"},
            {"icon", "http"},
            {"description", "HTTP requests to anything with a web API: Companion, media servers, home automation."},
            {"needsConnection", true},
            {"connectionFields", json::array({
                field("baseUrl", "Base URL", "string",
                      {{"default", "http://127.0.0.1:8000"}, {"required", true},
                       {"placeholder", "http://192.168.1.20:8000"},
                       {"help", "http:// only; HTTPS isn't supported yet."}}),
                field("headers", "Headers", "text",
                      {{"default", ""}, {"placeholder", "Authorization: Bearer ..."},
                       {"help", "One \"Name: value\" per line, sent with every request."}}),
                field("timeoutMs", "Timeout (ms)", "integer",
                      {{"default", 2000}, {"min", 100}, {"max", 30000}}),
            })},
            {"actions", json::array({
                json{
                    {"id", "request"},
                    {"label", "Request"},
                    {"summary", "{method} {path}"},
                    {"fields", json::array({
                        field("method", "Method", "enum",
                              {{"default", "GET"},
                               {"options", options_list({{"GET", "GET"}, {"POST", "POST"},
                                                         {"PUT", "PUT"}, {"PATCH", "PATCH"},
                                                         {"DELETE", "DELETE"}})}}),
                        field("path", "Path", "string",
                              {{"default", "/"}, {"placeholder", "/api/location/1/0/0/press"}}),
                        field("headers", "Headers", "text",
                              {{"default", ""}, {"help", "One \"Name: value\" per line."}}),
                        field("contentType", "Body type", "enum",
                              {{"default", "none"},
                               {"options", options_list({{"none", "None"},
                                                         {"application/json", "JSON"},
                                                         {"application/x-www-form-urlencoded", "Form"},
                                                         {"text/plain", "Text"}})}}),
                        field("body", "Body", "text",
                              {{"default", ""},
                               {"showIf", {{"field", "contentType"},
                                           {"equals", json::array({"application/json",
                                                                   "application/x-www-form-urlencoded",
                                                                   "text/plain"})}}}}),
                    })},
                },
            })},
        };
    }

    Result execute(const Job& job) override {
        if (job.action != "request") return Result::fail("unknown HTTP action '" + job.action + "'");
        std::string base = trim(opt_string(job.connection, "baseUrl"));
        if (base.empty()) return Result::fail("HTTP connection has no base URL");
        if (base.rfind("https://", 0) == 0) return Result::fail("HTTPS isn't supported yet; use http://");
        if (base.rfind("http://", 0) != 0) base = "http://" + base;
        while (!base.empty() && base.back() == '/') base.pop_back();

        httplib::Client cli{base};
        if (!cli.is_valid()) return Result::fail("invalid base URL '" + base + "'");
        const int timeout_ms = std::clamp(opt_int(job.connection, "timeoutMs", 2000), 100, 30000);
        cli.set_connection_timeout(std::chrono::milliseconds{timeout_ms});
        cli.set_read_timeout(std::chrono::milliseconds{timeout_ms});
        cli.set_write_timeout(std::chrono::milliseconds{timeout_ms});

        httplib::Headers headers;
        add_header_lines(headers, opt_string(job.connection, "headers"));
        add_header_lines(headers, opt_string(job.options, "headers"));

        std::string path = trim(opt_string(job.options, "path", "/"));
        if (path.empty() || path[0] != '/') path = "/" + path;
        const std::string method = opt_string(job.options, "method", "GET");
        const std::string type = opt_string(job.options, "contentType", "none");
        const std::string body = type == "none" ? std::string{} : opt_string(job.options, "body");
        const std::string ctype = type == "none" ? std::string{"text/plain"} : type;

        httplib::Result res;
        if (method == "GET")         res = cli.Get(path, headers);
        else if (method == "POST")   res = cli.Post(path, headers, body, ctype);
        else if (method == "PUT")    res = cli.Put(path, headers, body, ctype);
        else if (method == "PATCH")  res = cli.Patch(path, headers, body, ctype);
        else if (method == "DELETE") res = cli.Delete(path, headers, body, ctype);
        else return Result::fail("unsupported method '" + method + "'");

        if (!res) return Result::fail(httplib::to_string(res.error()));
        if (res->status >= 400)
            return Result::fail("HTTP " + std::to_string(res->status) + " from " + base + path);
        return {};
    }
};

} // namespace

std::unique_ptr<Plugin> make_http_plugin(Host&) { return std::make_unique<HttpPlugin>(); }

} // namespace liveplay::events
