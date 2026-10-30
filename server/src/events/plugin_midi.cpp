// =============================================================================
// events/plugin_midi.cpp — MIDI out through RtMidi
// -----------------------------------------------------------------------------
// The SERVER machine's MIDI outputs (a desk on a USB interface, or a virtual
// port such as loopMIDI / the IAC bus into software on the same machine).
// A connection names the port; ports are matched by name when an event fires,
// so a show still works after the interface is replugged and gets a new index.
// MIDI Show Control is here because it is how desks expect to be cued.
// =============================================================================
#include "liveplay/events/events.hpp"
#include "liveplay/logger.hpp"

#include <rtmidi/RtMidi.h>

#include <algorithm>
#include <cctype>
#include <mutex>

namespace liveplay::events {
namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::vector<std::string> output_port_names() {
    std::vector<std::string> names;
    try {
        RtMidiOut probe;
        const unsigned n = probe.getPortCount();
        for (unsigned i = 0; i < n; ++i) names.push_back(probe.getPortName(i));
    } catch (const RtMidiError& e) {
        Logger::warn("events/midi: listing ports failed: {}", e.getMessage());
    }
    return names;
}

// Channel 1..16 in the UI, 0..15 on the wire.
unsigned char status(unsigned char kind, const json& o) {
    const int ch = std::clamp(opt_int(o, "channel", 1), 1, 16) - 1;
    return static_cast<unsigned char>(kind | ch);
}

unsigned char data7(const json& o, const char* key, int def) {
    return static_cast<unsigned char>(std::clamp(opt_int(o, key, def), 0, 127));
}

// "F0 7F 01 ..." or "f07f01" → bytes. Empty on a malformed string.
std::vector<unsigned char> parse_hex(const std::string& text, std::string& error) {
    std::string digits;
    for (char c : text) {
        if (std::isxdigit(static_cast<unsigned char>(c))) digits.push_back(c);
        else if (!std::isspace(static_cast<unsigned char>(c)) && c != ',') {
            error = "SysEx data must be hex bytes, e.g. F0 7F 01 F7";
            return {};
        }
    }
    if (digits.empty() || digits.size() % 2) {
        error = "SysEx data must be whole hex bytes, e.g. F0 7F 01 F7";
        return {};
    }
    std::vector<unsigned char> out;
    for (std::size_t i = 0; i < digits.size(); i += 2)
        out.push_back(static_cast<unsigned char>(std::stoi(digits.substr(i, 2), nullptr, 16)));
    return out;
}

// Cue / list / path in MSC are ASCII digits and dots.
bool append_msc_number(std::vector<unsigned char>& msg, const std::string& s) {
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c)) && c != '.') return false;
        msg.push_back(static_cast<unsigned char>(c));
    }
    return true;
}

class MidiPlugin final : public Plugin {
public:
    std::string id() const override { return "midi"; }

    json schema() const override {
        json ports = json::array();
        for (const auto& n : output_port_names()) ports.push_back(json{{"value", n}, {"label", n}});

        const auto channel = field("channel", "Channel", "integer",
                                   {{"default", 1}, {"min", 1}, {"max", 16}});
        const auto seven = [](const char* id, const char* label, int def) {
            return field(id, label, "integer", {{"default", def}, {"min", 0}, {"max", 127}});
        };
        const json cue_commands = json::array({"go", "stop", "resume", "load", "go_off"});

        return json{
            {"id", "midi"},
            {"label", "MIDI"},
            {"icon", "piano"},
            {"description", "MIDI out from the server machine: notes, CC, program change, MIDI Show Control, SysEx."},
            {"needsConnection", true},
            {"connectionFields", json::array({
                field("port", "Output port", "enum",
                      {{"options", ports}, {"allowCustom", true}, {"required", true},
                       {"help", "Ports of the machine the LivePlay server runs on, matched by name."}}),
            })},
            {"actions", json::array({
                json{{"id", "note-on"}, {"label", "Note on"}, {"summary", "ch {channel} note {note}"},
                     {"fields", json::array({channel, seven("note", "Note", 60),
                                             seven("velocity", "Velocity", 127)})}},
                json{{"id", "note-off"}, {"label", "Note off"}, {"summary", "ch {channel} note {note}"},
                     {"fields", json::array({channel, seven("note", "Note", 60),
                                             seven("velocity", "Velocity", 0)})}},
                json{{"id", "control-change"}, {"label", "Control change"},
                     {"summary", "ch {channel} CC {controller} = {value}"},
                     {"fields", json::array({channel, seven("controller", "Controller", 1),
                                             seven("value", "Value", 127)})}},
                json{{"id", "program-change"}, {"label", "Program change"},
                     {"summary", "ch {channel} program {program}"},
                     {"fields", json::array({channel,
                         field("program", "Program", "integer",
                               {{"default", 1}, {"min", 1}, {"max", 128}})})}},
                json{{"id", "msc"}, {"label", "MIDI Show Control"},
                     {"summary", "{command} {cue}"},
                     {"fields", json::array({
                         field("deviceId", "Device ID", "integer",
                               {{"default", 127}, {"min", 0}, {"max", 127},
                                {"help", "127 addresses every device."}}),
                         field("commandFormat", "Command format", "enum",
                               {{"default", "lighting"},
                                {"options", options_list({
                                    {"lighting", "Lighting (general)"},
                                    {"moving-lights", "Moving lights"},
                                    {"sound", "Sound (general)"},
                                    {"video", "Video (general)"},
                                    {"all", "All types"}})}}),
                         field("command", "Command", "enum",
                               {{"default", "go"},
                                {"options", options_list({
                                    {"go", "GO"}, {"stop", "STOP"}, {"resume", "RESUME"},
                                    {"load", "LOAD"}, {"go_off", "GO_OFF"},
                                    {"all_off", "ALL_OFF"}, {"restore", "RESTORE"},
                                    {"reset", "RESET"}})}}),
                         field("cue", "Cue", "string",
                               {{"default", "1"}, {"placeholder", "1.5"},
                                {"showIf", {{"field", "command"}, {"equals", cue_commands}}}}),
                         field("list", "Cue list", "string",
                               {{"default", ""},
                                {"showIf", {{"field", "command"}, {"equals", cue_commands}}}}),
                         field("path", "Cue path", "string",
                               {{"default", ""},
                                {"showIf", {{"field", "command"}, {"equals", cue_commands}}}}),
                     })}},
                json{{"id", "sysex"}, {"label", "SysEx (raw)"}, {"summary", "{data}"},
                     {"fields", json::array({
                         field("data", "Bytes (hex)", "string",
                               {{"default", "F0 7F 7F 02 01 01 31 F7"}, {"required", true}})})}},
            })},
        };
    }

    Result execute(const Job& job) override {
        const std::string port = opt_string(job.connection, "port");
        if (port.empty()) return Result::fail("MIDI connection has no output port");

        std::vector<unsigned char> msg;
        std::string error;
        const json& o = job.options;
        if (job.action == "note-on") {
            msg = {status(0x90, o), data7(o, "note", 60), data7(o, "velocity", 127)};
        } else if (job.action == "note-off") {
            msg = {status(0x80, o), data7(o, "note", 60), data7(o, "velocity", 0)};
        } else if (job.action == "control-change") {
            msg = {status(0xB0, o), data7(o, "controller", 1), data7(o, "value", 127)};
        } else if (job.action == "program-change") {
            msg = {status(0xC0, o),
                   static_cast<unsigned char>(std::clamp(opt_int(o, "program", 1), 1, 128) - 1)};
        } else if (job.action == "msc") {
            msg = build_msc(o, error);
        } else if (job.action == "sysex") {
            msg = parse_hex(opt_string(o, "data"), error);
            if (error.empty() && (msg.front() != 0xF0 || msg.back() != 0xF7))
                error = "SysEx must start with F0 and end with F7";
        } else {
            return Result::fail("unknown MIDI action '" + job.action + "'");
        }
        if (!error.empty()) return Result::fail(error);
        return send(port, msg);
    }

private:
    static std::vector<unsigned char> build_msc(const json& o, std::string& error) {
        static const std::map<std::string, unsigned char> formats{
            {"lighting", 0x01}, {"moving-lights", 0x02}, {"sound", 0x10},
            {"video", 0x50}, {"all", 0x7F}};
        static const std::map<std::string, unsigned char> commands{
            {"go", 0x01}, {"stop", 0x02}, {"resume", 0x03}, {"load", 0x05},
            {"all_off", 0x08}, {"restore", 0x09}, {"reset", 0x0A}, {"go_off", 0x0B}};
        const auto fmt = formats.find(opt_string(o, "commandFormat", "lighting"));
        const auto cmd = commands.find(opt_string(o, "command", "go"));
        if (fmt == formats.end() || cmd == commands.end()) {
            error = "unknown MSC command format or command";
            return {};
        }
        std::vector<unsigned char> msg{0xF0, 0x7F, data7(o, "deviceId", 127), 0x02,
                                       fmt->second, cmd->second};
        const bool takes_cue = cmd->second <= 0x05 || cmd->second == 0x0B;
        if (takes_cue) {
            // cue [00 list [00 path]]; each part optional from the right.
            const std::string parts[] = {opt_string(o, "cue"), opt_string(o, "list"),
                                         opt_string(o, "path")};
            int last = -1;
            for (int i = 0; i < 3; ++i) if (!parts[i].empty()) last = i;
            for (int i = 0; i <= last; ++i) {
                if (i > 0) msg.push_back(0x00);
                if (!append_msc_number(msg, parts[i])) {
                    error = "MSC cue, list and path may only contain digits and dots";
                    return {};
                }
            }
        }
        msg.push_back(0xF7);
        return msg;
    }

    Result send(const std::string& port, std::vector<unsigned char>& msg) {
        std::lock_guard lock{mutex_};
        for (int attempt = 0; attempt < 2; ++attempt) {
            try {
                RtMidiOut& out = open(port);
                out.sendMessage(&msg);
                return {};
            } catch (const RtMidiError& e) {
                // The interface may have been unplugged: drop the handle and
                // reopen once by name.
                ports_.erase(port);
                if (attempt == 1) return Result::fail(e.getMessage());
            } catch (const std::exception& e) {
                ports_.erase(port);
                if (attempt == 1) return Result::fail(e.what());
            }
        }
        return Result::fail("MIDI send failed");
    }

    // Under mutex_. Exact name first, then a case-insensitive substring, since
    // some systems decorate names ("loopMIDI Port 1" vs "loopMIDI Port 1 2").
    RtMidiOut& open(const std::string& port) {
        if (auto it = ports_.find(port); it != ports_.end()) return *it->second;
        auto out = std::make_unique<RtMidiOut>();
        const unsigned n = out->getPortCount();
        int index = -1;
        for (unsigned i = 0; i < n && index < 0; ++i)
            if (out->getPortName(i) == port) index = static_cast<int>(i);
        for (unsigned i = 0; i < n && index < 0; ++i)
            if (lower(out->getPortName(i)).find(lower(port)) != std::string::npos)
                index = static_cast<int>(i);
        if (index < 0) throw std::runtime_error("MIDI output '" + port + "' not found");
        out->openPort(static_cast<unsigned>(index), "LivePlay");
        auto& ref = *out;
        ports_[port] = std::move(out);
        return ref;
    }

    std::mutex mutex_;
    std::map<std::string, std::unique_ptr<RtMidiOut>> ports_;
};

} // namespace

std::unique_ptr<Plugin> make_midi_plugin() { return std::make_unique<MidiPlugin>(); }

} // namespace liveplay::events
