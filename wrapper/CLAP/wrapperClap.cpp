#include <iostream>
#include <cstring>
#include "Factory_CLAP.h"

namespace sst::clap_saw_demo::pluginentry
{

uint32_t clap_get_plugin_count(const clap_plugin_factory *f) { return 1; }
const clap_plugin_descriptor *clap_get_plugin_descriptor(const clap_plugin_factory *f, uint32_t w)
{
    return & gmpi::hosting::ClapFactory::GetInstance()->clap_desciptor;
}

static const clap_plugin *clap_create_plugin(const clap_plugin_factory *f, const clap_host *host,
                                             const char *plugin_id)
{
    return gmpi::hosting::ClapFactory::GetInstance()->createInstance(host, plugin_id);
}

const CLAP_EXPORT struct clap_plugin_factory clap_saw_demo_factory = {
    sst::clap_saw_demo::pluginentry::clap_get_plugin_count,
    sst::clap_saw_demo::pluginentry::clap_get_plugin_descriptor,
    sst::clap_saw_demo::pluginentry::clap_create_plugin,
};
static const void *get_factory(const char *factory_id)
{
    return (!strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID)) ? &clap_saw_demo_factory : nullptr;
}

// clap_init and clap_deinit are required to be fast, but we have nothing we need to do here
bool clap_init(const char *p) { return true; }
void clap_deinit() {}

} // namespace sst::clap_saw_demo::pluginentry

extern "C"
{
    // clang-format off
    const CLAP_EXPORT struct clap_plugin_entry clap_entry = {
        CLAP_VERSION,
        sst::clap_saw_demo::pluginentry::clap_init,
        sst::clap_saw_demo::pluginentry::clap_deinit,
        sst::clap_saw_demo::pluginentry::get_factory
    };
    // clang-format on
}
