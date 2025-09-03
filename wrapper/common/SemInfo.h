#pragma once

//#include "HostControls.h"

#if 0
struct pinInfoSem
{
	int32_t id;
	std::string name;
	gmpi::PinDirection direction;
	gmpi::PinDatatype datatype;
	std::string default_value;
	int32_t parameterId = -1;
	gmpi::Field parameterFieldType;
	int32_t flags;
	wrapper::HostControls hostConnect = wrapper::HC_NONE;
	std::string meta_data;
};

// todo might be helpful to flag if it's used on UI/Processor to save on pointless updates
struct paramInfoSem
{
	// parameter can have id or host control, the special VST3 BYPASS parameter can be both.
	int32_t id{-1};
	wrapper::HostControls hostConnect{wrapper::HC_NONE};

	std::string name;
	gmpi::PinDatatype datatype;
	std::string default_value;
	//int32_t parameterId;
	//int32_t flags;
	//std::string hostConnect;
	std::string enum_list;
	double minimum = 0.0;
	double maximum = 1.0;
	bool is_private{};
};

struct pluginInfoSem
{
	std::string id;
	std::string name;
	int inputCount = {};
	int outputCount = {};
	std::vector<pinInfoSem> dspPins;
	std::vector<pinInfoSem> guiPins;
	std::vector<paramInfoSem> parameters;

	//	platform_string pluginPath;
	std::string pluginPath;
};
#endif


