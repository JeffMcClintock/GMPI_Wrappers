#include "./Factory_CLAP.h"
#include "Processor_CLAP.h"
#include "Hosting/gmpi_factory.h"

namespace gmpi
{
namespace hosting
{

ClapFactory* ClapFactory::GetInstance()
{
    static ClapFactory instance;
    return &instance;
}

ClapFactory::ClapFactory()
{
    auto plugin = gmpi::hosting::factory::getInstance().getPluginInfo();

	const bool hasMidiInput = 0 < countPins(*plugin, gmpi::PinDirection::In, gmpi::PinDatatype::Midi);

	static const char* instrument_features[] = { CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER, nullptr };
	static const char* effect_features[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, nullptr };

	clap_desciptor.id          = plugin->id.c_str();
	clap_desciptor.name        = plugin->name.c_str();
	clap_desciptor.vendor      = plugin->vendorName.c_str();
	clap_desciptor.url         = plugin->vendorUrl.c_str();
	clap_desciptor.manual_url  = "";
	clap_desciptor.support_url = "";

	// The plugin's declared version - the `version` attribute of <Plugin> in its
	// metadata XML, read by the SDK's own parser, so this and what the VST3
	// factory reports are one value. Borrowed from the pluginInfo like every
	// other string here; that object belongs to the factory singleton and
	// outlives this descriptor.
	//
	// gmpi_plugin.cmake stamps the .clap's VERSIONINFO resource from that same
	// attribute, but by a textual search of the sources rather than through this
	// parser, so the two agree in the ordinary case and not by construction -
	// gmpi::hosting::pluginInfo::version says where they can part company. This
	// is the value a host is told, and the authoritative one.
	//
	// Guarded because pluginInfo::version arrived in the SDK after this
	// wrapper, and the two are separate repositories that are routinely at
	// different revisions. An older SDK keeps the "1.0.0" this line held
	// unconditionally before, so nothing regresses.
#ifdef GMPI_HOSTING_PLUGININFO_HAS_VERSION
	clap_desciptor.version     = plugin->version.c_str();
#else
	clap_desciptor.version     = "1.0.0";
#endif
	clap_desciptor.description = plugin->name.c_str();
	clap_desciptor.features    = hasMidiInput ? instrument_features : effect_features;
}

const clap_plugin* ClapFactory::createInstance(const clap_host* host, const char* plugin_id)
{
    if (strcmp(plugin_id, clap_desciptor.id) != 0)
        return nullptr;

	auto pluginInfo = gmpi::hosting::factory::getInstance().getPluginInfo();
    if (!pluginInfo)
        return nullptr;

    auto p = new Processor_CLAP(&clap_desciptor, *pluginInfo, host);
    return p->clapPlugin();
}

}}
