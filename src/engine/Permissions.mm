#include <Network/Network.h>
#include <dispatch/dispatch.h>

// Bonjour types used by the app: NDI and OMT sources (found on the network) and the OSC announcement. macOS asks for
// local network access at the first Bonjour browse; the same types are listed in Info.plist (NSBonjourServices).
// The browsers live until the app quits (browsing is what keeps the access).
void requestLocalNetworkAccess()
{
    static const char *const kTypes[] = {"_ndi._tcp", "_omt._tcp", "_oscjson._tcp", "_osc._udp"};
    static bool started = false;
    if (started) return;
    started = true;
    dispatch_queue_t queue = dispatch_get_main_queue();
    for (const char *type : kTypes) {
        nw_browse_descriptor_t descriptor = nw_browse_descriptor_create_bonjour_service(type, nullptr);
        nw_browser_t browser = nw_browser_create(descriptor, nw_parameters_create());
        nw_browser_set_queue(browser, queue);
        nw_browser_start(browser);
    }
}
