// =============================================================================
// events/builtin_plugins.cpp — the one list of plugins the server ships
// -----------------------------------------------------------------------------
// Adding a plugin? Append its factory here (see docs/EVENT_PLUGINS.md). The
// order is the order the client offers them in.
// =============================================================================
#include "liveplay/events/plugin.hpp"

namespace liveplay::events {

const std::vector<PluginFactory>& builtin_plugins() {
    static const std::vector<PluginFactory> list{
        make_osc_plugin,
        make_midi_plugin,
        make_http_plugin,
        make_xair_plugin,
        make_liveplay_plugin,
    };
    return list;
}

} // namespace liveplay::events
