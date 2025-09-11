#pragma once
#include "Hosting/controller_holder.h"
#include "helpers/Timer.h"

namespace gmpi { namespace hosting
{

struct Controller_CLAP :
    public gmpi::TimerClient

{
    gmpi::hosting::gmpi_controller_holder gmpiController;

    gmpi::hosting::interThreadQue message_que_ui_to_dsp;
    QueuedUsers pendingQueueClients; // parameters waiting to be sent to Processor

    Controller_CLAP();
    ~Controller_CLAP();

    bool onTimer() override;
};

}}