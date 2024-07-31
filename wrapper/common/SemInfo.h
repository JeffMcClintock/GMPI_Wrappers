#pragma once

struct pinInfoSem
{
	int32_t id;
	std::string name;
	gmpi::PinDirection direction;
	gmpi::PinDatatype datatype;
	std::string default_value;
	int32_t parameterId;
	gmpi::Field parameterFieldType;
	int32_t flags;
	std::string hostConnect;
	std::string meta_data;
};
struct paramInfoSem
{
	int32_t id;
	std::string name;
	gmpi::PinDatatype datatype;
	std::string default_value;
	//int32_t parameterId;
	//int32_t flags;
	//std::string hostConnect;
	std::string meta_data;
	bool is_private;
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

inline int countPins(pluginInfoSem const& plugin, gmpi::PinDirection direction, gmpi::PinDatatype datatype)
{
	return std::count_if(
		plugin.dspPins.begin()
		, plugin.dspPins.end()
		, [direction, datatype](const pinInfoSem& p) -> bool
		{
			return p.direction == direction && p.datatype == datatype;
		}
	);
}

inline std::string calcSubCategories(pluginInfoSem const& plugin)
{
	if (countPins(plugin, gmpi::PinDirection::In, gmpi::PinDatatype::Midi) > 0)
	{
		return "Instrument|Synth";
	}
	return "Fx";
}

inline auto getPinName(pluginInfoSem const& plugin, gmpi::PinDirection direction, int index) -> std::string
{
	int i = 0;
	for (auto& p : plugin.dspPins)
	{
		if (p.direction != direction || p.datatype != gmpi::PinDatatype::Audio)
			continue;

		if (i++ == index)
			return p.name;
	}

	return {};
}
