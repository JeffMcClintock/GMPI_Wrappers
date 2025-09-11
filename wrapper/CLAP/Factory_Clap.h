#pragma once
#include "clap/helpers/plugin.hh"

namespace gmpi { namespace hosting
{

struct ClapFactory
{
    clap_plugin_descriptor clap_desciptor{};

    static ClapFactory* GetInstance();

    ClapFactory();

    const clap_plugin* createInstance(const clap_host* host, const char* plugin_id);
};

}}