// =============================================================================
// events/plugins/xair.cpp — Behringer X Air / Midas M Air mixers over OSC
// -----------------------------------------------------------------------------
// XR12, XR16, XR18, X18, MR12, MR18: one firmware family, one OSC dialect on
// UDP 10024. The mixer answers a message carrying only an address with that
// address's value, which is how strip names, snapshot names and current fader
// positions are read; it answers a "/xinfo" broadcast with its address, name,
// model and firmware, which is discovery.
//
// Every X Air exposes the XR18's surface whatever its sockets: channels 1-16,
// the aux return, four FX returns, six buses, four FX sends and Main LR. A
// model without a socket simply has nothing behind that strip.
//
// Levels are in dB through the mixer's own fader law (fader_to_db). A change
// can crossfade: execute() reads the current value, starts a ramp in Ramps
// and returns, so a 30 s fade never holds up the next cue. The connection's
// "Max level" caps every fader and send this plugin sets.
//
// Protocol notes come from Behringer's X AIR OSC tables (the community's
// extraction of them), the Bitfocus Companion X Air module, and Patrick-Gilles
// Maillot's X32/X-Air tools; FX parameter ranges from xair_fx_table.inc. All
// of it was checked against an XR12 running firmware 1.17.
// =============================================================================
#include "liveplay/events/helpers.hpp"
#include "liveplay/events/osc.hpp"
#include "liveplay/events/plugin.hpp"
#include "liveplay/logger.hpp"

#include <asio.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

namespace liveplay::events {
namespace {

using namespace std::chrono_literals;

struct FxParam { const char* name; double min, max; int steps; const char* unit; };
struct FxType  { const char* code; const char* name; int first; int count; };
#include "xair_fx_table.inc"

constexpr int    kPort        = 10024;
constexpr double kSilenceDb   = -90.0;   // the bottom of the fader: -inf
constexpr auto   kGetTimeout  = 300ms;

std::string two(int n) { return (n < 10 ? "0" : "") + std::to_string(n); }

// ---- Fader law ---------------------------------------------------------------
// The X32 / X Air fader: four straight segments, 0.0 = -inf, 0.75 = 0 dB,
// 1.0 = +10 dB. Faders and send levels share it.
double fader_to_db(double f) {
    f = std::clamp(f, 0.0, 1.0);
    if (f >= 0.5)    return f * 40.0 - 30.0;
    if (f >= 0.25)   return f * 80.0 - 50.0;
    if (f >= 0.0625) return f * 160.0 - 70.0;
    return f * 480.0 - 90.0;
}

double db_to_fader(double db) {
    if (db <= kSilenceDb) return 0.0;
    double f;
    if (db < -60.0)      f = (db + 90.0) / 480.0;
    else if (db < -30.0) f = (db + 70.0) / 160.0;
    else if (db < -10.0) f = (db + 50.0) / 80.0;
    else                 f = (db + 30.0) / 40.0;
    return std::clamp(f, 0.0, 1.0);
}

// ---- FX parameter law --------------------------------------------------------
bool fx_is_log(const FxParam& p) { return p.steps < 0 && p.min > 0.0 && p.max > p.min; }

double fx_to_float(const FxParam& p, double v) {
    v = std::clamp(v, std::min(p.min, p.max), std::max(p.min, p.max));
    if (p.max == p.min) return 0.0;
    if (fx_is_log(p)) return std::log(v / p.min) / std::log(p.max / p.min);
    return (v - p.min) / (p.max - p.min);
}

const FxType* fx_type(int index) {
    constexpr int n = static_cast<int>(sizeof kFxTypes / sizeof kFxTypes[0]);
    return index >= 0 && index < n ? &kFxTypes[index] : nullptr;
}

std::string fmt_number(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

// ---- The surface -------------------------------------------------------------
struct Strip { std::string value, prefix; };   // "ch/05", "Ch 05"

std::vector<Strip> strips(bool with_buses_and_main) {
    std::vector<Strip> out;
    for (int c = 1; c <= 16; ++c) out.push_back({"ch/" + two(c), "Ch " + two(c)});
    out.push_back({"rtn/aux", "Aux in"});
    for (int r = 1; r <= 4; ++r) out.push_back({"rtn/" + std::to_string(r), "FX return " + std::to_string(r)});
    if (with_buses_and_main) {
        for (int b = 1; b <= 6; ++b) out.push_back({"bus/" + std::to_string(b), "Bus " + std::to_string(b)});
        for (int f = 1; f <= 4; ++f) out.push_back({"fxsend/" + std::to_string(f), "FX send " + std::to_string(f)});
        out.push_back({"lr", "Main LR"});
    }
    return out;
}

bool valid_strip(const std::string& s, bool with_buses_and_main) {
    for (const auto& st : strips(with_buses_and_main)) if (st.value == s) return true;
    return false;
}

// Send targets of a channel: buses 1-6 are mix 01-06, FX sends 1-4 are 07-10.
struct Send { std::string value, prefix, name_address; };
std::vector<Send> sends() {
    std::vector<Send> out;
    for (int b = 1; b <= 6; ++b)
        out.push_back({two(b), "Bus " + std::to_string(b), "/bus/" + std::to_string(b) + "/config/name"});
    for (int f = 1; f <= 4; ++f)
        out.push_back({two(6 + f), "FX send " + std::to_string(f), "/fxsend/" + std::to_string(f) + "/config/name"});
    return out;
}

// Routing sources, in the mixer's order (0..55; USB outputs accept 0..37).
std::vector<std::string> routing_sources() {
    std::vector<std::string> out;
    for (int c = 1; c <= 16; ++c) out.push_back("Ch " + two(c));
    out.push_back("Aux L");
    out.push_back("Aux R");
    for (int f = 1; f <= 4; ++f) { out.push_back("FX " + std::to_string(f) + " L"); out.push_back("FX " + std::to_string(f) + " R"); }
    for (int b = 1; b <= 6; ++b) out.push_back("Bus " + std::to_string(b));
    for (int f = 1; f <= 4; ++f) out.push_back("FX send " + std::to_string(f));
    out.push_back("Main L");
    out.push_back("Main R");
    for (int u = 1; u <= 18; ++u) out.push_back("USB return " + std::to_string(u));
    return out;
}

// ---- One mixer ---------------------------------------------------------------
// A UDP socket bound to the interface on the mixer's subnet, a thread
// receiving its replies, and the latest reply per address for get().
class Mixer {
public:
    Mixer(const std::string& host, int port) : host_(host) {
        asio::error_code ec;
        asio::ip::udp::resolver resolver{io_};
        auto results = resolver.resolve(asio::ip::udp::v4(), host, std::to_string(port), ec);
        if (ec || results.empty()) throw std::runtime_error("can't resolve '" + host + "'");
        remote_ = results.begin()->endpoint();

        socket_.open(asio::ip::udp::v4(), ec);
        if (ec) throw std::runtime_error("can't open a UDP socket: " + ec.message());
        const std::string local = local_address_for(remote_.address().to_string());
        const auto bind_to = local.empty() ? asio::ip::address_v4::any()
                                           : asio::ip::make_address_v4(local, ec);
        socket_.bind(asio::ip::udp::endpoint(ec ? asio::ip::address_v4::any() : bind_to, 0), ec);
        if (ec) throw std::runtime_error("can't bind a UDP socket: " + ec.message());

        receive();
        thread_ = std::thread([this] { io_.run(); });
    }

    ~Mixer() {
        work_.reset();
        io_.stop();
        if (thread_.joinable()) thread_.join();
    }

    const std::string& host() const { return host_; }

    std::string send(const osc::Message& m) {
        const auto packet = osc::encode(m);
        std::lock_guard lock{send_mutex_};
        asio::error_code ec;
        socket_.send_to(asio::buffer(packet), remote_, 0, ec);
        return ec ? "send to " + host_ + " failed: " + ec.message() : std::string{};
    }

    // The values of `addresses`, by address; missing ones didn't answer in time.
    std::map<std::string, osc::Message> get(const std::vector<std::string>& addresses,
                                            std::chrono::milliseconds timeout = kGetTimeout) {
        std::uint64_t since;
        {
            std::lock_guard lock{mutex_};
            since = seq_;
        }
        for (const auto& a : addresses) {
            if (!send(osc::Message{a, {}}).empty()) break;
            // A long burst can overrun the mixer's input; a breath every few.
            if (addresses.size() > 8) std::this_thread::sleep_for(1ms);
        }
        std::map<std::string, osc::Message> out;
        std::unique_lock lock{mutex_};
        const auto collect = [&] {
            for (const auto& a : addresses) {
                if (out.count(a)) continue;
                auto it = last_.find(a);
                if (it != last_.end() && it->second.second > since) out[a] = it->second.first;
            }
            return out.size() == addresses.size();
        };
        cv_.wait_for(lock, timeout + std::chrono::milliseconds(addresses.size() * 2), collect);
        collect();
        return out;
    }

    std::optional<osc::Message> get_one(const std::string& address) {
        auto r = get({address});
        if (auto it = r.find(address); it != r.end()) return it->second;
        return std::nullopt;
    }

private:
    void receive() {
        socket_.async_receive_from(asio::buffer(buf_), sender_,
            [this](const asio::error_code& ec, std::size_t n) {
                if (ec == asio::error::operation_aborted) return;
                if (!ec) {
                    if (auto m = osc::decode(buf_.data(), n)) {
                        // Key first: in `last_[m->address] = {std::move(*m), ...}`
                        // the right side runs first and empties the address.
                        const std::string address = m->address;
                        std::lock_guard lock{mutex_};
                        last_[address] = {std::move(*m), ++seq_};
                    }
                    cv_.notify_all();
                }
                receive();
            });
    }

    std::string                     host_;
    asio::io_context                io_;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work_{
        asio::make_work_guard(io_)};
    asio::ip::udp::socket           socket_{io_};
    asio::ip::udp::endpoint         remote_, sender_;
    std::array<char, 8192>          buf_{};
    std::thread                     thread_;
    std::mutex                      send_mutex_;
    std::mutex                      mutex_;
    std::condition_variable         cv_;
    std::unordered_map<std::string, std::pair<osc::Message, std::uint64_t>> last_;
    std::uint64_t                   seq_ = 0;
};

// ---- The plugin --------------------------------------------------------------
class XAirPlugin final : public Plugin {
public:
    std::string id() const override { return "xair"; }

    json schema() const override {
        json strip_opts = json::array(), all_strip_opts = json::array();
        for (const auto& s : strips(false)) strip_opts.push_back(json{{"value", s.value}, {"label", s.prefix}});
        for (const auto& s : strips(true)) all_strip_opts.push_back(json{{"value", s.value}, {"label", s.prefix}});
        json send_opts = json::array();
        for (const auto& s : sends()) send_opts.push_back(json{{"value", s.value}, {"label", s.prefix}});
        json snap_opts = json::array();
        for (int n = 1; n <= 64; ++n) snap_opts.push_back(json{{"value", n}, {"label", two(n)}});
        json slot_opts = json::array();
        for (int n = 1; n <= 4; ++n) slot_opts.push_back(json{{"value", n}, {"label", "FX " + std::to_string(n)}});
        json type_opts = json::array();
        for (int t = 0; t < 61; ++t) type_opts.push_back(json{{"value", t}, {"label", kFxTypes[t].name}});
        json param_opts = json::array();
        for (int n = 1; n <= 64; ++n) param_opts.push_back(json{{"value", n}, {"label", two(n)}});
        json src_opts = json::array();
        const auto sources = routing_sources();
        for (int i = 0; i < static_cast<int>(sources.size()); ++i)
            src_opts.push_back(json{{"value", i}, {"label", sources[i]}});
        json out_opts = json::array();
        for (int a = 1; a <= 6; ++a)  out_opts.push_back(json{{"value", "aux/" + two(a)}, {"label", "Aux out " + std::to_string(a)}});
        for (int u = 1; u <= 18; ++u) out_opts.push_back(json{{"value", "usb/" + two(u)}, {"label", "USB send " + std::to_string(u)}});
        for (int p = 1; p <= 16; ++p) out_opts.push_back(json{{"value", "p16/" + two(p)}, {"label", "Ultranet " + std::to_string(p)}});
        json main_opts = json::array({json{{"value", 0}, {"label", "Main LR"}},
                                      json{{"value", 1}, {"label", "Monitor"}}});
        for (int u = 1; u <= 17; u += 2)
            main_opts.push_back(json{{"value", (u + 3) / 2}, {"label", "USB " + std::to_string(u) + "/" + std::to_string(u + 1)}});

        const auto level = field("level", "Level", "number",
            {{"default", -20.0}, {"min", kSilenceDb}, {"max", 10.0}, {"step", 0.5}, {"unit", "dB"},
             {"help", "-90 is off (-inf). Capped by the connection's max level."}});
        const auto fade = field("fade", "Crossfade", "number",
            {{"default", 0.0}, {"min", 0.0}, {"max", 600.0}, {"step", 0.1}, {"unit", "s"},
             {"help", "0 changes it at once; longer fades from where it is now."}});
        const auto slot = field("slot", "FX slot", "enum",
            {{"default", 1}, {"options", slot_opts}, {"optionsFrom", "fx-slots"}});

        return json{
            {"id", "xair"},
            {"label", "Behringer X Air"},
            {"icon", "tune"},
            {"description", "XR12, XR16, XR18 and Midas MR mixers: snapshots, faders, sends, FX, routing and the USB recorder."},
            {"needsConnection", true},
            {"discoverable", true},
            {"connectionFields", json::array({
                field("host", "Mixer address", "string",
                      {{"default", ""}, {"required", true}, {"placeholder", "192.168.1.105"}}),
                field("port", "Port", "integer", {{"default", kPort}, {"min", 1}, {"max", 65535}}),
                field("maxLevelDb", "Max level", "number",
                      {{"default", 10.0}, {"min", kSilenceDb}, {"max", 10.0}, {"step", 0.5}, {"unit", "dB"},
                       {"help", "No fader or send set by an event goes above this."}}),
            })},
            {"actions", json::array({
                // For the connection editor's Test: changes nothing on the desk.
                json{{"id", "check"}, {"label", "Check connection"}, {"testOnly", true},
                     {"fields", json::array()}},
                json{{"id", "snapshot"}, {"label", "Recall snapshot"}, {"summary", "{snapshot}"},
                     {"fields", json::array({
                         field("snapshot", "Snapshot", "enum",
                               {{"default", 1}, {"options", snap_opts}, {"optionsFrom", "snapshots"},
                                {"required", true}}),
                     })}},
                json{{"id", "fader"}, {"label", "Set fader"}, {"summary", "{strip} → {level} dB"},
                     {"fields", json::array({
                         field("strip", "Strip", "enum",
                               {{"default", "ch/01"}, {"options", all_strip_opts}, {"optionsFrom", "strips"},
                                {"required", true}}),
                         level, fade,
                     })}},
                json{{"id", "send"}, {"label", "Set send level"}, {"summary", "{strip} → {send}: {level} dB"},
                     {"fields", json::array({
                         field("strip", "From", "enum",
                               {{"default", "ch/01"}, {"options", strip_opts}, {"optionsFrom", "send-sources"},
                                {"required", true}}),
                         field("send", "To", "enum",
                               {{"default", "01"}, {"options", send_opts}, {"optionsFrom", "sends"},
                                {"required", true}}),
                         level, fade,
                     })}},
                json{{"id", "mute"}, {"label", "Mute / unmute"}, {"summary", "{strip}: {state}"},
                     {"fields", json::array({
                         field("strip", "Strip", "enum",
                               {{"default", "ch/01"}, {"options", all_strip_opts}, {"optionsFrom", "strips"},
                                {"required", true}}),
                         field("state", "State", "enum",
                               {{"default", "mute"},
                                {"options", options_list({{"mute", "Mute"}, {"unmute", "Unmute"}})}}),
                     })}},
                json{{"id", "recorder"}, {"label", "USB recorder"}, {"summary", "{command}"},
                     {"fields", json::array({
                         field("command", "Command", "enum",
                               {{"default", "record"},
                                {"options", options_list({{"record", "Record"}, {"stop", "Stop"},
                                                          {"record-pause", "Pause recording"},
                                                          {"play", "Play"}, {"play-pause", "Pause playback"}})},
                                {"help", "Records onto the USB drive in the mixer."}}),
                     })}},
                json{{"id", "fx-type"}, {"label", "Set FX type"}, {"summary", "{slot} → {type}"},
                     {"fields", json::array({
                         slot,
                         field("type", "Effect", "enum", {{"default", 0}, {"options", type_opts}}),
                     })}},
                json{{"id", "fx-param"}, {"label", "Set FX parameter"}, {"summary", "{slot} #{param} = {value}"},
                     {"fields", json::array({
                         slot,
                         field("param", "Parameter", "enum",
                               {{"default", 1}, {"options", param_opts}, {"optionsFrom", "fx-params"},
                                {"dependsOn", json::array({"slot"})}}),
                         field("value", "Value", "number",
                               {{"default", 0.0},
                                {"help", "In the parameter's own unit (shown beside it); switches are 0/1, choices count from 0."}}),
                         fade,
                     })}},
                json{{"id", "route-output"}, {"label", "Route an output"}, {"summary", "{output} ← {source}"},
                     {"fields", json::array({
                         field("output", "Output", "enum", {{"default", "aux/01"}, {"options", out_opts}}),
                         field("source", "Source", "enum",
                               {{"default", 36}, {"options", src_opts},
                                {"help", "USB sends can't take USB returns."}}),
                         field("tap", "Tap point", "enum",
                               {{"default", "keep"},
                                {"options", options_list({
                                    {"keep", "Don't change"}, {"0", "Analog in"}, {"1", "Analog in + mute"},
                                    {"2", "Input"}, {"3", "Input + mute"}, {"4", "Pre EQ"},
                                    {"5", "Pre EQ + mute"}, {"6", "Post EQ"}, {"7", "Post EQ + mute"},
                                    {"8", "Pre fader"}, {"9", "Pre fader + mute"}, {"10", "Post fader"}})}}),
                     })}},
                json{{"id", "route-main"}, {"label", "Route main out"}, {"summary", "{assign}"},
                     {"fields", json::array({
                         field("assign", "Main out carries", "enum", {{"default", 0}, {"options", main_opts}}),
                     })}},
            })},
        };
    }

    Result execute(const Job& job) override {
        std::shared_ptr<Mixer> mixer;
        try { mixer = mixer_for(job.connection); }
        catch (const std::exception& e) { return Result::fail(e.what()); }
        const json& o = job.options;
        const double cap = std::clamp(opt_number(job.connection, "maxLevelDb", 10.0), kSilenceDb, 10.0);

        if (job.action == "check") {
            if (auto info = mixer->get_one("/xinfo"); info && info->args.size() >= 3) return {};
            return Result::fail("the mixer at " + mixer->host() + " isn't answering");
        }
        if (job.action == "snapshot") {
            const int n = opt_int(o, "snapshot", 0);
            if (n < 1 || n > 64) return Result::fail("snapshot must be 1 to 64");
            return sent(mixer->send({"/-snap/load", {osc::Arg::integer(n)}}));
        }
        if (job.action == "fader" || job.action == "send") {
            const std::string strip = opt_string(o, "strip");
            const bool is_send = job.action == "send";
            if (!valid_strip(strip, !is_send)) return Result::fail("unknown strip '" + strip + "'");
            std::string address = "/" + strip + "/mix/fader";
            if (is_send) {
                const std::string to = opt_string(o, "send");
                const auto all = sends();
                const bool ok = std::any_of(all.begin(), all.end(),
                                            [&](const Send& s) { return s.value == to; });
                if (!ok) return Result::fail("unknown send '" + to + "'");
                address = "/" + strip + "/mix/" + to + "/level";
            }
            const double target = std::min(opt_number(o, "level", kSilenceDb), cap);
            return level_to(mixer, address, target, opt_number(o, "fade", 0.0));
        }
        if (job.action == "mute") {
            const std::string strip = opt_string(o, "strip");
            if (!valid_strip(strip, true)) return Result::fail("unknown strip '" + strip + "'");
            const bool mute = opt_string(o, "state", "mute") != "unmute";
            // "on" is the channel's ON button: 0 = muted.
            return sent(mixer->send({"/" + strip + "/mix/on", {osc::Arg::integer(mute ? 0 : 1)}}));
        }
        if (job.action == "recorder") {
            static const std::map<std::string, int> states{
                {"stop", 0}, {"play-pause", 1}, {"play", 2}, {"record-pause", 3}, {"record", 4}};
            const auto it = states.find(opt_string(o, "command", "record"));
            if (it == states.end()) return Result::fail("unknown recorder command");
            if (it->second >= 1) {
                if (auto usb = mixer->get_one("/-stat/usbmounted"); usb && !usb->args.empty() &&
                                                                      usb->args[0].number() == 0)
                    return Result::fail("no USB drive in the mixer");
            }
            return sent(mixer->send({"/-stat/tape/state", {osc::Arg::integer(it->second)}}));
        }
        if (job.action == "fx-type") {
            const int slot = opt_int(o, "slot", 0), type = opt_int(o, "type", -1);
            if (slot < 1 || slot > 4) return Result::fail("FX slot must be 1 to 4");
            if (!fx_type(type)) return Result::fail("unknown effect type");
            return sent(mixer->send({"/fx/" + std::to_string(slot) + "/type", {osc::Arg::integer(type)}}));
        }
        if (job.action == "fx-param") {
            const int slot = opt_int(o, "slot", 0), param = opt_int(o, "param", 0);
            if (slot < 1 || slot > 4) return Result::fail("FX slot must be 1 to 4");
            if (param < 1 || param > 64) return Result::fail("parameter must be 1 to 64");
            const std::string address = "/fx/" + std::to_string(slot) + "/par/" + two(param);
            // The mapping depends on what is loaded in the slot right now.
            const FxParam* def = nullptr;
            if (auto t = mixer->get_one("/fx/" + std::to_string(slot) + "/type"); t && !t->args.empty()) {
                if (const auto* ft = fx_type(static_cast<int>(t->args[0].number())); ft && param <= ft->count)
                    def = &kFxParams[ft->first + param - 1];
            }
            const double v = opt_number(o, "value", 0.0);
            const double target = def ? fx_to_float(*def, v) : std::clamp(v / 100.0, 0.0, 1.0);
            return float_to(mixer, address, target, opt_number(o, "fade", 0.0));
        }
        if (job.action == "route-output") {
            const std::string out = opt_string(o, "output");
            const auto slash = out.find('/');
            const std::string kind = out.substr(0, slash);
            const int n = slash == std::string::npos ? 0 : std::atoi(out.c_str() + slash + 1);
            const int max_n = kind == "aux" ? 6 : kind == "usb" ? 18 : kind == "p16" ? 16 : 0;
            if (n < 1 || n > max_n) return Result::fail("unknown output '" + out + "'");
            const int source = opt_int(o, "source", -1);
            if (source < 0 || source > (kind == "usb" ? 37 : 55))
                return Result::fail("that source can't feed this output");
            const std::string base = "/routing/" + kind + "/" + two(n);
            if (auto err = mixer->send({base + "/src", {osc::Arg::integer(source)}}); !err.empty())
                return Result::fail(err);
            const std::string tap = opt_string(o, "tap", "keep");
            if (tap != "keep") {
                const int pos = opt_int(o, "tap", -1);
                if (pos < 0 || pos > 10) return Result::fail("unknown tap point");
                return sent(mixer->send({base + "/pos", {osc::Arg::integer(pos)}}));
            }
            return {};
        }
        if (job.action == "route-main") {
            const int v = opt_int(o, "assign", -1);
            if (v < 0 || v > 10) return Result::fail("unknown main out assignment");
            return sent(mixer->send({"/routing/main/01", {osc::Arg::integer(v)}}));
        }
        return Result::fail("unknown X Air action '" + job.action + "'");
    }

    OptionsResult options(const std::string& source, const json& connection,
                          const json& values) override {
        OptionsResult r;
        std::shared_ptr<Mixer> mixer;
        try { mixer = mixer_for(connection); }
        catch (const std::exception& e) { r.error = e.what(); }

        // A list of { value, prefix, name address }: names read live, the
        // prefix alone when the mixer doesn't answer (or the name is empty).
        struct Row { json value; std::string prefix, address; };
        const auto named = [&](const std::vector<Row>& rows, const char* sep = " · ") {
            std::map<std::string, osc::Message> names;
            if (mixer) {
                std::vector<std::string> addrs;
                for (const auto& row : rows) addrs.push_back(row.address);
                names = mixer->get(addrs);
                if (names.empty() && r.error.empty())
                    r.error = "the mixer at " + mixer->host() + " isn't answering";
            }
            for (const auto& row : rows) {
                std::string label = row.prefix;
                if (auto it = names.find(row.address); it != names.end() && !it->second.args.empty()) {
                    const std::string n = it->second.args[0].text();
                    if (!n.empty() && n != "-") label += sep + n;
                }
                r.options.push_back(json{{"value", row.value}, {"label", label}});
            }
        };
        const auto strip_rows = [](bool all) {
            std::vector<Row> rows;
            for (const auto& s : strips(all)) {
                const std::string addr = s.value == "lr" ? "/lr/config/name" : "/" + s.value + "/config/name";
                rows.push_back({s.value, s.prefix, addr});
            }
            return rows;
        };

        if (source == "strips") { named(strip_rows(true)); return r; }
        if (source == "send-sources") { named(strip_rows(false)); return r; }
        if (source == "sends") {
            std::vector<Row> rows;
            for (const auto& s : sends()) rows.push_back({s.value, s.prefix, s.name_address});
            named(rows);
            return r;
        }
        if (source == "snapshots") {
            std::vector<Row> rows;
            for (int n = 1; n <= 64; ++n) rows.push_back({n, two(n), "/-snap/" + two(n) + "/name"});
            named(rows);
            return r;
        }
        if (source == "fx-slots" || source == "fx-params") {
            std::map<std::string, osc::Message> types;
            if (mixer) types = mixer->get({"/fx/1/type", "/fx/2/type", "/fx/3/type", "/fx/4/type"});
            if (mixer && types.empty() && r.error.empty())
                r.error = "the mixer at " + mixer->host() + " isn't answering";
            const auto type_of = [&](int slot) -> const FxType* {
                auto it = types.find("/fx/" + std::to_string(slot) + "/type");
                if (it == types.end() || it->second.args.empty()) return nullptr;
                return fx_type(static_cast<int>(it->second.args[0].number()));
            };
            if (source == "fx-slots") {
                for (int s = 1; s <= 4; ++s) {
                    const auto* t = type_of(s);
                    r.options.push_back(json{{"value", s},
                        {"label", "FX " + std::to_string(s) + (t ? std::string{" · "} + t->name : "")}});
                }
                return r;
            }
            const int slot = std::clamp(opt_int(values, "slot", 1), 1, 4);
            const auto* t = type_of(slot);
            if (t && t->count > 0) {
                for (int p = 0; p < t->count; ++p) {
                    const auto& d = kFxParams[t->first + p];
                    std::string label = two(p + 1) + " · " + d.name + " (" + fmt_number(d.min) + "–" +
                                        fmt_number(d.max) + (d.unit[0] ? std::string{" "} + d.unit : "") + ")";
                    r.options.push_back(json{{"value", p + 1}, {"label", label}});
                }
            } else {
                for (int p = 1; p <= 64; ++p)
                    r.options.push_back(json{{"value", p}, {"label", two(p) + " (0–100 %)"}});
            }
            return r;
        }
        r.error = "no option source '" + source + "'";
        return r;
    }

    std::vector<Discovered> discover(std::chrono::milliseconds budget) override {
        std::vector<Discovered> found;
        std::mutex found_mutex;
        asio::io_context io;
        std::vector<std::unique_ptr<asio::ip::udp::socket>> sockets;
        std::vector<std::unique_ptr<std::array<char, 2048>>> bufs;
        std::vector<std::unique_ptr<asio::ip::udp::endpoint>> senders;
        const std::string probe = osc::encode({"/xinfo", {}});

        // One socket per interface, bound to it: its replies come back to it,
        // whatever a VPN does to the routing table.
        std::vector<std::pair<std::string, std::string>> targets;   // local, broadcast
        for (const auto& li : local_ipv4_interfaces()) targets.emplace_back(li.address, li.broadcast);
        targets.emplace_back("", "255.255.255.255");

        std::function<void(std::size_t)> listen = [&](std::size_t i) {
            sockets[i]->async_receive_from(asio::buffer(*bufs[i]), *senders[i],
                [&, i](const asio::error_code& ec, std::size_t n) {
                    if (ec) return;
                    if (auto m = osc::decode(bufs[i]->data(), n); m && m->address == "/xinfo" &&
                                                                  m->args.size() >= 3) {
                        const std::string ip = m->args[0].text(), name = m->args[1].text(),
                                          model = m->args[2].text();
                        const std::string fw = m->args.size() > 3 ? m->args[3].text() : "";
                        std::lock_guard lock{found_mutex};
                        const bool dup = std::any_of(found.begin(), found.end(), [&](const Discovered& d) {
                            return d.config.value("host", std::string{}) == ip;
                        });
                        if (!dup)
                            found.push_back({name + " — " + model + " at " + ip + (fw.empty() ? "" : " (firmware " + fw + ")"),
                                             name, json{{"host", ip}, {"port", kPort}}});
                    }
                    listen(i);
                });
        };
        for (const auto& [local, bcast] : targets) {
            asio::error_code ec;
            auto s = std::make_unique<asio::ip::udp::socket>(io);
            s->open(asio::ip::udp::v4(), ec);
            if (ec) continue;
            s->set_option(asio::socket_base::broadcast(true), ec);
            const auto addr = local.empty() ? asio::ip::address_v4::any()
                                            : asio::ip::make_address_v4(local, ec);
            if (ec) continue;
            s->bind(asio::ip::udp::endpoint(addr, 0), ec);
            if (ec) continue;
            s->send_to(asio::buffer(probe),
                       asio::ip::udp::endpoint(asio::ip::make_address_v4(bcast, ec), kPort), 0, ec);
            sockets.push_back(std::move(s));
            bufs.push_back(std::make_unique<std::array<char, 2048>>());
            senders.push_back(std::make_unique<asio::ip::udp::endpoint>());
            listen(sockets.size() - 1);
        }
        io.run_for(budget);
        io.stop();
        return found;
    }

    void stop() override {
        ramps_.stop();
        std::lock_guard lock{mutex_};
        mixers_.clear();
    }

private:
    static Result sent(const std::string& err) { return err.empty() ? Result{} : Result::fail(err); }

    std::shared_ptr<Mixer> mixer_for(const json& connection) {
        const std::string host = opt_string(connection, "host");
        if (host.empty()) throw std::runtime_error("the connection has no mixer address");
        const int port = opt_int(connection, "port", kPort);
        const std::string key = host + ":" + std::to_string(port);
        std::lock_guard lock{mutex_};
        auto& m = mixers_[key];
        if (!m) m = std::make_shared<Mixer>(host, port);
        return m;
    }

    // A fader or send level, in dB, faded from wherever it is now.
    Result level_to(const std::shared_ptr<Mixer>& mixer, const std::string& address,
                    double target_db, double fade_s) {
        double from_db = target_db;
        if (fade_s > 0.0) {
            if (auto cur = mixer->get_one(address); cur && !cur->args.empty())
                from_db = fader_to_db(cur->args[0].number());
        }
        ramps_.start(mixer->host() + address, from_db, target_db, fade_s,
                     [mixer, address](double db) {
                         mixer->send({address, {osc::Arg::real(static_cast<float>(db_to_fader(db)))}});
                     });
        return {};
    }

    // A raw 0..1 parameter, faded along the knob's own travel.
    Result float_to(const std::shared_ptr<Mixer>& mixer, const std::string& address,
                    double target, double fade_s) {
        double from = target;
        if (fade_s > 0.0) {
            if (auto cur = mixer->get_one(address); cur && !cur->args.empty())
                from = cur->args[0].number();
        }
        ramps_.start(mixer->host() + address, from, target, fade_s, [mixer, address](double v) {
            mixer->send({address, {osc::Arg::real(static_cast<float>(std::clamp(v, 0.0, 1.0)))}});
        });
        return {};
    }

    std::mutex                                     mutex_;
    std::map<std::string, std::shared_ptr<Mixer>>  mixers_;
    Ramps                                          ramps_;
};

} // namespace

std::unique_ptr<Plugin> make_xair_plugin(Host&) { return std::make_unique<XAirPlugin>(); }

} // namespace liveplay::events
