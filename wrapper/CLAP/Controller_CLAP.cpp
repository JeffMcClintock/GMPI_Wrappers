#include "./Factory_Clap.h"
#include "Processor_CLAP.h"
#include "Hosting/gmpi_factory.h"

namespace gmpi
{
namespace hosting
{

Controller_CLAP::Controller_CLAP() :
	message_que_ui_to_dsp(0x500000) // 5MB. see also AUDIO_MESSAGE_QUE_SIZE
{
	gmpiController.notifyDaw = [this](gmpi::hosting::GmpiParameter* param)
		{
			pendingQueueClients.AddWaiter(param);
		};


	const int timerPeriodMs = 35;
	startTimer(timerPeriodMs);
}

Controller_CLAP::~Controller_CLAP()
{
	stopTimer();
}

bool Controller_CLAP::onTimer()
{
	// parameter updates from the Processor
	gmpiController.message_que_dsp_to_ui.pollMessage(&gmpiController);

	// parameter updates to the Processor
	//gmpi::hosting::my_msg_que_output_stream toProcessor(&message_que_ui_to_dsp);
	//if (pendingQueueClients.ServiceWaiters(
	//	toProcessor,
	//	message_que_ui_to_dsp.freeSpace(),
	//	message_que_ui_to_dsp.freeSpace()
	//))
	//{
	//	message_que_ui_to_dsp.Send();
	//}

	pendingQueueClients.ServiceWaitersIncremental(&message_que_ui_to_dsp, 100000);

	/*
	if (!queueToDsp_.empty())
	{
		sendMessageToProcessor(queueToDsp_.data(), queueToDsp_.size());
		queueToDsp_.clear();
	}
	*/

	return true;
}

}}
