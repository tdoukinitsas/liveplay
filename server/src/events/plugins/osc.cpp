// =============================================================================
// events/plugin_osc.cpp — Open Sound Control over UDP
// -----------------------------------------------------------------------------
// The lingua franca of lighting desks (Eos, grandMA, QLab, Companion...). A
// connection is a host + port; the one action sends a single OSC 1.0 message.
// Messages are built with the shared OSC codec (liveplay/events/osc.hpp);
// asio, which comes with Crow, carries them.
// =============================================================================
#include "liveplay/events/osc.hpp"
#include "liveplay/events/plugin.hpp"

#include <asio.hpp>

#include <mutex>
#include <optional>

namespace liveplay::events {
namespace {

// One OSC message from the event's args: [{ type: i|f|s|T|F|N, value }].
std::optional<osc::Message> build_message(const std::string& address, const json& args,
                                          std::string& error) {
    osc::Message m{address, {}};
    if (args.is_array()) {
        for (const auto& a : args) {
            if (!a.is_object()) continue;
            const std::string type = opt_string(a, "type", "s");
            if (type == "i")      m.args.push_back(osc::Arg::integer(opt_int(a, "value", 0)));
            else if (type == "f") m.args.push_back(osc::Arg::real(static_cast<float>(opt_number(a, "value", 0.0))));
            else if (type == "s") m.args.push_back(osc::Arg::string(opt_string(a, "value")));
            else if (type == "T" || type == "F" || type == "N") {
                osc::Arg tag;
                tag.type = type[0];
                m.args.push_back(tag);
            } else {
                error = "unsupported OSC argument type '" + type + "'";
                return std::nullopt;
            }
        }
    }
    return m;
}

class OscPlugin final : public Plugin {
public:
    std::string id() const override { return "osc"; }

    json schema() const override {
        const json arg_types = options_list({
            {"i", "Integer"}, {"f", "Float"}, {"s", "String"},
            {"T", "True"}, {"F", "False"}, {"N", "Nil"},
        });
        return json{
            {"id", "osc"},
            {"label", "OSC"},
            {"icon", "settings_input_antenna"},
            {"description", "Open Sound Control over UDP: lighting desks, QLab, Companion and more."},
            {"needsConnection", true},
            {"connectionFields", json::array({
                field("host", "Host", "string",
                      {{"default", "127.0.0.1"}, {"required", true},
                       {"placeholder", "192.168.1.50"}}),
                field("port", "Port", "integer",
                      {{"default", 8000}, {"min", 1}, {"max", 65535}, {"required", true}}),
            })},
            {"actions", json::array({
                json{
                    {"id", "message"},
                    {"label", "Send message"},
                    {"summary", "{address}"},
                    {"fields", json::array({
                        field("address", "Address", "string",
                              {{"default", "/cue/1/go"}, {"required", true},
                               {"placeholder", "/eos/cue/1/go"}}),
                        field("args", "Arguments", "list",
                              {{"default", json::array()},
                               {"fields", json::array({
                                   field("type", "Type", "enum",
                                         {{"options", arg_types}, {"default", "i"}}),
                                   field("value", "Value", "string",
                                         {{"default", ""},
                                          {"showIf", {{"field", "type"},
                                                      {"equals", json::array({"i", "f", "s"})}}}}),
                               })}}),
                    })},
                },
            })},
        };
    }

    Result execute(const Job& job) override {
        const std::string host = opt_string(job.connection, "host");
        const int port = opt_int(job.connection, "port", 0);
        if (host.empty() || port <= 0 || port > 65535)
            return Result::fail("OSC connection needs a host and a port");
        if (job.action != "message") return Result::fail("unknown OSC action '" + job.action + "'");

        const std::string address = opt_string(job.options, "address");
        if (address.empty() || address[0] != '/')
            return Result::fail("OSC address must start with '/'");

        std::string error;
        const auto message = build_message(
            address, job.options.contains("args") ? job.options["args"] : json::array(), error);
        if (!message) return Result::fail(error);
        const std::string packet = osc::encode(*message);

        // One socket for the plugin, guarded: execute() is called from this
        // plugin's lane and from Test requests at the same time.
        std::lock_guard lock{mutex_};
        asio::error_code ec;
        asio::ip::udp::resolver resolver{io_};
        auto endpoints = resolver.resolve(asio::ip::udp::v4(), host, std::to_string(port), ec);
        if (ec || endpoints.empty())
            return Result::fail("can't resolve '" + host + "': " + ec.message());
        if (!socket_.is_open()) {
            socket_.open(asio::ip::udp::v4(), ec);
            if (ec) return Result::fail("can't open UDP socket: " + ec.message());
            // Lets a desk listen on a broadcast address too.
            socket_.set_option(asio::socket_base::broadcast(true), ec);
        }
        socket_.send_to(asio::buffer(packet), endpoints.begin()->endpoint(), 0, ec);
        if (ec) {
            socket_.close();
            return Result::fail("send failed: " + ec.message());
        }
        return {};
    }

private:
    std::mutex             mutex_;
    asio::io_context       io_;
    asio::ip::udp::socket  socket_{io_};
};

} // namespace

std::unique_ptr<Plugin> make_osc_plugin(Host&) { return std::make_unique<OscPlugin>(); }

} // namespace liveplay::events
