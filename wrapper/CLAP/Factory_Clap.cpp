#pragma once
#include "./Factory_Clap.h"
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
    const auto& plugin = gmpi::hosting::factory::getInstance().getPluginInfo();

	static const char* features[] = { CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER, nullptr };

	clap_desciptor.id          = plugin->id.c_str();
	clap_desciptor.name        = plugin->name.c_str();
	clap_desciptor.vendor      = plugin->vendorName.c_str();
	clap_desciptor.url         = plugin->vendorUrl.c_str();
	clap_desciptor.manual_url  = "";
	clap_desciptor.support_url = "";
	clap_desciptor.version     = "1.0.0";
	clap_desciptor.description = plugin->name.c_str();
	clap_desciptor.features    = features;
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