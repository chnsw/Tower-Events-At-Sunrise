#include "server_ui_module_runtime.h"

#include <string_view>

#include "../../../core/ui/modules/registry/ui_module_registry.h"
#include "../../../core/ui/modules/ui_module_descriptor.h"
#include "../activity_override/activity_override_panel.h"
#include "../tower_events/tower_events_panel.h"

namespace sunrise::server::ui::runtime {
namespace {

/** A namespaced stable ID keeps Server modules from clashing with Client modules. */
constexpr std::string_view kOverrideStableId = "server.activity_override";
/** Short menu label for the activity override page. */
constexpr std::string_view kOverrideDisplayName = "Activity";
/** The Tower events page, which picks the seasonal dressing the roster publishes. */
constexpr std::string_view kEventsStableId = "server.tower_events";
/** Short menu label for the Tower events page. */
constexpr std::string_view kEventsDisplayName = "Events";

core::ui::modules::registry::PageRegistration g_overridePage;
core::ui::modules::registry::PageRegistration g_eventsPage;

} // namespace

/** @return True when the Server module owns both of its Core UI registry slots. */
bool initialize() noexcept {
    const bool overridePage = g_overridePage.acquire(core::ui::modules::Owner::server,
                                                     kOverrideStableId,
                                                     kOverrideDisplayName,
                                                     &activity_override::draw);
    const bool eventsPage = g_eventsPage.acquire(core::ui::modules::Owner::server,
                                                 kEventsStableId,
                                                 kEventsDisplayName,
                                                 &tower_events::draw);
    return overridePage && eventsPage;
}

/** Removes the Server module's pages from the Core UI registry. */
void shutdown() noexcept {
    g_eventsPage.release();
    g_overridePage.release();
}

} // namespace sunrise::server::ui::runtime
