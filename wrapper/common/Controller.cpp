#include <codecvt>
#include <locale>
#include <thread>
#include <fstream>
#include <filesystem>
#include "Controller.h"
#include "tinyXml2/tinyxml2.h"
#include "RawConversions.h"
#include "HostControls.h"
#include "GmpiResourceManager.h"
//#include "./Presenter.h"
#include "BundleInfo.h"
#include "FileFinder.h"
#include "midi_defs.h"
#include "ListBuilder.h"
#include "wrapper/common/it_enum_list.h"

#if !defined(SE_USE_JUCE_UI)
//#include "GuiPatchAutomator3.h"
#endif
#ifndef GMPI_VST3_WRAPPER
#include "../../UgDatabase.h"
#include "PresetReader.h"
#endif

#if 0
#include "../shared/unicode_conversion.h"
#include "../../UgDatabase.h"
#include "../../modules/shared/string_utilities.h"
#include "../shared/FileWatcher.h"
#include "PresetReader.h"
#include "./ProcessorStateManager.h"
#include "../../mfc_emulation.h"
#endif

extern "C"
gmpi::ReturnCode MP_GetFactory(void** returnInterface);

using namespace std;
namespace wrapper
{

// Plugin GUI is sending param to host
gmpi::ReturnCode ControllerManager::setParameter(int32_t parameterHandle, gmpi::Field fieldId, int32_t voice, int32_t size, const void* data)
{
// nope. wrong direction.	return controller2_->setParameter(parameterHandle, fieldId, voice, size, data);
	return patchManager->setParameter(parameterHandle, fieldId, voice, size, data);
}

gmpi::ReturnCode ControllerManager::getParameterHandle(int32_t moduleParameterId, int32_t& returnHandle)
{
	return (gmpi::ReturnCode)patchManager->getParameterHandle(moduleParameterId, returnHandle);
}


MpController::~MpController()
{
    //if(presenter_)
    //{
    //    presenter_->OnControllerDeleted();
    //}
}

void MpController::ScanPresets()
{
	presets.clear(); // fix crash on JUCE
	presets = scanNativePresets(); // Scan VST3 presets (both VST2 and VST3)

	// Factory presets from bundles presets folder.
	{
		auto presets2 = scanFactoryPresets();

		// skip duplicates of disk presets (because VST3 plugin will have them all on disk, and VST2 will scan same folder.
		const auto nativePresetsCount = presets.size();
		for (auto& preset : presets2)
		{
			// Is this a duplicate?
			bool isDuplicate = false;
			for (size_t i = 0; i < nativePresetsCount; ++i)
			{
				if (presets[i].name == preset.name)
				{
					// preset occurs in VST presets folder and ALSO in preset XML.
					isDuplicate = true;
					//presets[i].index = preset.index; // Don't insert it twice, but note that it is an internal preset. (VST3 Preset will be ignored).
					presets[i].isFactory = true;
					break;
				}
			}

			if (!isDuplicate)
			{
				preset.isFactory = true;
				presets.push_back({ preset });
			}
		}
	}
	{
#if 0
	// Factory presets from factory.xmlpreset resource.
		auto nativePresetsCount = presets.size();

		// Harvest factory preset names.
		auto factoryPresetFolder = ToPlatformString(BundleInfo::instance()->getImbedded FileFolder());
		string filenameUtf8 = ToUtf8String(factoryPresetFolder) + "factory.xmlpreset";

		TiXmlDocument doc;
		doc.LoadFile(filenameUtf8);

		if (!doc.Error()) // if file does not exist, that's OK. Only means we're a VST3 plugin and don't have internal presets.
		{
			TiXmlHandle hDoc(&doc);
			TiXmlElement* pElem;
			{
				pElem = hDoc.FirstChildElement().Element();

				// should always have a valid root but handle gracefully if it does not.
				if (!pElem)
					return;
			}

			const char* pKey = pElem->Value();
			assert(strcmp(pKey, "Presets") == 0);

			int i = 0;
			for (auto preset_xml = pElem->FirstChildElement("Preset"); preset_xml; preset_xml = preset_xml->NextSiblingElement())
			{
				presetInfo preset;
				preset.index = i++;

				preset_xml->QueryStringAttribute("name", &preset.name);
				preset_xml->QueryStringAttribute("category", &preset.category);

				// skip duplicates of disk presets (because VST3 plugin will have them all on disk, and VST2 will scan same folder.
				// Is this a duplicate?
				bool isDuplicate = false;
				for (size_t i = 0; i < nativePresetsCount; ++i)
				{
					if (presets[i].name == preset.name)
					{
						// preset occurs in VST presets folder and ALSO in preset XML.
						isDuplicate = true;
						presets[i].index = preset.index; // Don't insert it twice, but note that it is an internal preset. (VST3 Preset will be ignored).
						break;
					}
				}

				if (!isDuplicate)
				{
					presets.push_back(preset);
				}
			}
		}
#endif
		// sort all presets by category.
		std::sort(presets.begin(), presets.end(),
			[=](const presetInfo& a, const presetInfo& b) -> bool
			{
				// Sort by category
				if (a.category != b.category)
				{
					// blank category last
					if (a.category.empty() != b.category.empty())
						return a.category.empty() < b.category.empty();

					return a.category < b.category;
				}

				// ..then by index
				if (a.index != b.index)
					return a.index < b.index;

				return a.name < b.name;
			});
	}

#ifdef _DEBUG
	for (auto& preset : presets)
	{
		assert(!preset.filename.empty() || preset.isSession || preset.isFactory);
	}
#endif
}

void MpController::UpdatePresetBrowser()
{
	// Update preset browser
	for (auto& p : parameters_)
	{
		if (p->getHostControl() == HC_PROGRAM_CATEGORIES_LIST || p->getHostControl() == HC_PROGRAM_NAMES_LIST)
		{
			UpdateProgramCategoriesHc(p.get());
			updateGuis(p.get(), gmpi::Field::Value);
		}
	}
}

void MpController::Initialize()
{
    if(isInitialized)
    {
        return; // Prevent double-up on parameters.
    }

	// ensure resource manager knows where to find things
	{
		auto& resourceFolders = GmpiResourceManager::Instance()->resourceFolders;
		const auto pluginResourceFolder = BundleInfo::instance()->getResourceFolder();

		resourceFolders[GmpiResourceType::Midi] = pluginResourceFolder;
		resourceFolders[GmpiResourceType::Image] = pluginResourceFolder;
		resourceFolders[GmpiResourceType::Audio] = pluginResourceFolder;
		resourceFolders[GmpiResourceType::Soundfont] = pluginResourceFolder;
	}

	// Parameters
	{
		int hostParameterIndex = 0;
		int ParameterHandle = 0;

		for (auto& i : ParameterHandleIndex)
		{
			ParameterHandle = (std::max)(ParameterHandle, i.first + 1);
		}

		for (auto& param : info.parameters)
		{
			bool isPrivate =
				param.is_private ||
				param.datatype == gmpi::PinDatatype::String ||
				param.datatype == gmpi::PinDatatype::Blob;

			float pminimum = 0.0f;
			float pmaximum = 1.0f;

			if (!param.meta_data.empty())
			{
				it_enum_list it(Utf8ToWstring(param.meta_data));

				pminimum = it.RangeLo();
				pmaximum = it.RangeHi();
			}

			MpParameter_base* seParameter = {};
			if (isPrivate)
			{
				auto param = new MpParameter_private(this);
				seParameter = param;
				//					param->isPolyphonic_ = isPolyphonic_;
			}
			else
			{
				//					assert(ParameterTag >= 0);
				seParameter = makeNativeParameter(hostParameterIndex++, pminimum > pmaximum);
			}

			seParameter->hostControl_ = -1; // TODO hostControl;
			seParameter->minimum = pminimum;
			seParameter->maximum = pmaximum;
			seParameter->parameterHandle_ = ParameterHandle;
			seParameter->datatype_ = param.datatype;
			seParameter->moduleHandle_ = 0;
			seParameter->moduleParamId_ = param.id;
			seParameter->stateful_ = true; // stateful_;
			seParameter->name_ = Utf8ToWstring(param.name);
			seParameter->enumList_ = Utf8ToWstring(param.meta_data); // enumList_;
			seParameter->ignorePc_ = false; // ignorePc != 0;

			// add one patch value
			seParameter->rawValues_.push_back(ParseToRaw(seParameter->datatype_, param.default_value));

			parameters_.push_back(std::unique_ptr<MpParameter>(seParameter));
			ParameterHandleIndex.insert(std::make_pair(ParameterHandle, seParameter));
			moduleParameterIndex.insert(std::make_pair(std::make_pair(seParameter->moduleHandle_, seParameter->moduleParamId_), ParameterHandle));

			// Ensure host queries return correct value.
			seParameter->upDateImmediateValue();

			++ParameterHandle;
		}
	}

#ifndef GMPI_VST3_WRAPPER

	// Ensure we can access SEM Controllers info
	ModuleFactory()->RegisterExternalPluginsXmlOnce(nullptr);

//	TiXmlDocument doc;
	tinyxml2::XMLDocument doc;
	{
		const auto xml = BundleInfo::instance()->getResource("parameters.se.xml");
		doc.Parse(xml.c_str());
		assert(!doc.Error());
	}

	auto controllerE = doc.FirstChildElement("Controller");
	assert(controllerE);

	auto patchManagerE = controllerE->FirstChildElement();
	assert(strcmp(patchManagerE->Value(), "PatchManager") == 0);

	std::wstring_convert<std::codecvt_utf8<wchar_t>> convert;

	auto parameters_xml = patchManagerE->FirstChildElement("Parameters");

	::init(parametersInfo, parameters_xml); // for parsing presets

	for (auto parameter_xml = parameters_xml->FirstChildElement("Parameter"); parameter_xml; parameter_xml = parameter_xml->NextSiblingElement("Parameter"))
	{
		int dataType = DT_FLOAT;
		int ParameterTag = -1;
		int ParameterHandle = -1;
		int Private = 0;

		std::string Name = parameter_xml->Attribute("Name");
		parameter_xml->QueryIntAttribute("ValueType", &dataType);
		parameter_xml->QueryIntAttribute("Index", &ParameterTag);
		parameter_xml->QueryIntAttribute("Handle", &ParameterHandle);
		parameter_xml->QueryIntAttribute("Private", &Private);

		if (dataType == gmpi::PinDatatype::String || dataType == gmpi::PinDatatype::Blob)
		{
			Private = 1; // VST and AU can't handle this type of parameter.
		}
		else
		{
			if (Private != 0)
			{
				// Check parameter is numeric and a valid type.
				assert(dataType == DT_ENUM || dataType == DT_DOUBLE || dataType == gmpi::PinDatatype::Bool || dataType == DT_FLOAT || dataType == gmpi::PinDatatype::Int32 || dataType == gmpi::PinDatatype::Int3264);
			}
		}

		int stateful_ = 1;
		parameter_xml->QueryIntAttribute("persistant", &stateful_);
		int hostControl = -1;
		parameter_xml->QueryIntAttribute("HostControl", &hostControl);
		int ignorePc = 0;
		parameter_xml->QueryIntAttribute("ignoreProgramChange", &ignorePc);

		double pminimum = 0.0;
		double pmaximum = 10.0;

		parameter_xml->QueryDoubleAttribute("RangeMinimum", &pminimum);
		parameter_xml->QueryDoubleAttribute("RangeMaximum", &pmaximum);

		int moduleHandle_ = -1;
		int moduleParamId_ = 0;
		bool isPolyphonic_ = false;
		wstring enumList_;

		parameter_xml->QueryIntAttribute("Module", &(moduleHandle_));
		parameter_xml->QueryIntAttribute("ModuleParamId", &(moduleParamId_));
		parameter_xml->QueryBoolAttribute("isPolyphonic", &(isPolyphonic_));

		if (dataType == gmpi::PinDatatype::Int32 || dataType == gmpi::PinDatatype::String /*|| dataType == DT_ENUM */)
		{
			auto s = parameter_xml->Attribute("MetaData");
			if (s)
				enumList_ = convert.from_bytes(s);
		}

		MpParameter_base* seParameter = nullptr;

		if (Private == 0)
		{
			assert(ParameterTag >= 0);
			seParameter = makeNativeParameter(ParameterTag, pminimum > pmaximum);
		}
		else
		{
			auto param = new MpParameter_private(this);
			seParameter = param;
			param->isPolyphonic_ = isPolyphonic_;
		}

		seParameter->hostControl_ = hostControl;
		seParameter->minimum = pminimum;
		seParameter->maximum = pmaximum;

		parameter_xml->QueryIntAttribute("MIDI", &(seParameter->MidiAutomation));
		if (seParameter->MidiAutomation != -1)
		{
			const char* temp{};
			parameter_xml->QueryStringAttribute("MIDI_SYSEX", &temp);
			seParameter->MidiAutomationSysex = Utf8ToWstring(temp);
		}

		// Preset values from patch list.
		ParseXmlPreset(
			parameter_xml,
			[seParameter, dataType](int voiceId, int preset, const char* xmlvalue)
			{
				seParameter->rawValues_.push_back(ParseToRaw(dataType, xmlvalue));
			}
		);

		// no patch-list?, init to zero.
		if (!parameter_xml->FirstChildElement("patch-list"))
		{
			assert(!stateful_);

			// Special case HC_VOICE_PITCH needs to be initialized to standard western scale
			if (HC_VOICE_PITCH == hostControl)
			{
				const int middleA = 69;
				constexpr float invNotesPerOctave = 1.0f / 12.0f;
				seParameter->rawValues_.reserve(128);
				for (float key = 0; key < 128; ++key)
				{
					const float pitch = 5.0f + static_cast<float>(key - middleA) * invNotesPerOctave;
					std::string raw((const char*) &pitch, sizeof(pitch));
					seParameter->rawValues_.push_back(raw);
				}
			}
			else
			{
				// init to zero
				const char* nothing = "\0\0\0\0\0\0\0\0";
				std::string raw(nothing, getDataTypeSize(dataType));
				seParameter->rawValues_.push_back(raw);
			}
		}

		seParameter->parameterHandle_ = ParameterHandle;
		seParameter->datatype_ = dataType;
		seParameter->moduleHandle_ = moduleHandle_;
		seParameter->moduleParamId_ = moduleParamId_;
		seParameter->stateful_ = stateful_;
		seParameter->name_ = convert.from_bytes(Name);
		seParameter->enumList_ = enumList_;
		seParameter->ignorePc_ = ignorePc != 0;

		parameters_.push_back(std::unique_ptr<MpParameter>(seParameter));
		ParameterHandleIndex.insert({ ParameterHandle, seParameter });
		moduleParameterIndex.insert({ {moduleHandle_, moduleParamId_}, ParameterHandle });
        
        // Ensure host queries return correct value.
        seParameter->upDateImmediateValue();
	}

	// SEM Controllers.
	{
		assert(controllerE);

		auto childPluginsE = controllerE->FirstChildElement("ChildControllers");
		for (auto childE = childPluginsE->FirstChildElement("ChildController"); childE; childE = childE->NextSiblingElement("ChildController"))
		{
			std::string typeId = childE->Attribute("Type");

			auto mi = ModuleFactory()->GetById(Utf8ToWstring(typeId));

			if (!mi)
			{
				continue;
			}

			gmpi_sdk::mp_shared_ptr<gmpi::IMpUnknown> obj;
			obj.Attach(mi->Build(gmpi::api::PluginSubtype::Controller, true));

			if (obj)
			{
				gmpi_sdk::mp_shared_ptr<gmpi::IMpController> controller;
				/*auto r = */ obj->queryInterface(gmpi::MP_IID_CONTROLLER, controller.asIMpUnknownPtr());

				if (controller)
				{
					int32_t handle = 0;
					childE->QueryIntAttribute("Handle", &(handle));
					semControllers.addController(handle, controller);

					// Duplicating all the pins and defaults seems a bit redundant, they may not even be needed.
					// Perhaps controller needs own dedicated pins????

					// Create IO and autoduplicating Plugs. Set defaults.
					auto plugsElement = childE->FirstChildElement("Pins");

					if (plugsElement)
					{
						int32_t i = 0;
						for (auto plugElement = plugsElement->FirstChildElement(); plugElement; plugElement = plugElement->NextSiblingElement())
						{
							assert(strcmp(plugElement->Value(), "Pin") == 0);

							plugElement->QueryIntAttribute("idx", &i);
							int32_t pinType = 0;
							plugElement->QueryIntAttribute("type", &pinType);
							auto d = plugElement->Attribute("default");

							if (!d)
								d = "";

							controller->setPinDefault(pinType, i, d);

							++i;
						}
					}
				}
			}
		}
	}
#endif

	// crashes in JUCE VST3 plugin helper: EXEC : error : FindFirstChangeNotification function failed. [D:\a\1\s\build\Optimus\Optimus_VST3.vcxproj]
#if (GMPI_IS_PLATFORM_JUCE==0)
	{
		auto presetFolderPath = toPlatformString(BundleInfo::instance()->getPresetFolder());
		if (!presetFolderPath.empty())
		{
			fileWatcher.Start(
				presetFolderPath,
				[this]()
				{
					// note: called from background thread.
					presetsFolderChanged = true;
				}
			);
		}
	}
#endif
    
	undoManager.initial(this, getPreset());

    isInitialized = true;
}

void MpController::initSemControllers()
{
	// Create Controller object
	if (!isSemControllersInitialised)
	{
		gmpi::shared_ptr<gmpi::api::IUnknown> factoryBase;
		auto r = MP_GetFactory(factoryBase.put_void());

		if (auto factory = factoryBase.as<gmpi::api::IPluginFactory>(); factory)
		{
			gmpi::shared_ptr<gmpi::api::IUnknown> pluginUnknown;
			const auto r2 = factory->createInstance(info.id.c_str(), gmpi::api::PluginSubtype::Controller, pluginUnknown.put_void());
			if (pluginUnknown && r == gmpi::ReturnCode::Ok)
			{
				semControllers.addController(0, pluginUnknown.as<gmpi::api::IController>());
			}
		}
	}

#if 0 // TODO
	if (!isSemControllersInitialised)
	{
		semControllers.initialize();
		//		_RPT0(_CRT_WARN, "ADelayController::initSemControllers\n");

		for (auto& cp : semControllers.childPluginControllers)
		{
			cp.second->controller_->open();
		}

		isSemControllersInitialised = true;
	}
#endif
	isSemControllersInitialised = true;
}

gmpi::ReturnCode MpController::getParameterHandle(int32_t moduleParameterId, int32_t& returnHandle)
{
	const int32_t moduleHandle = 0;

	auto it = moduleParameterIndex.find({ moduleHandle, moduleParameterId });
	if (it == moduleParameterIndex.end())
	{
		return gmpi::ReturnCode::Fail;
	}

	returnHandle = (*it).second;

	return gmpi::ReturnCode::Ok;
}

gmpi::ReturnCode MpController::setParameter(int32_t parameterHandle, gmpi::Field fieldId, int32_t voice, int32_t size, const void* data)
{
	setParameterValue(RawView((const char*)data, size), parameterHandle, fieldId, voice);
	return gmpi::ReturnCode::Ok;
}

#if 0 // TODO
int32_t MpController::getController(int32_t moduleHandle, gmpi::IMpController** returnController)
{
	for (auto& m : semControllers.childPluginControllers)
	{
		if (m.first == moduleHandle)
		{
			*returnController = m.second->controller_;
			break;
		}
	}
	return gmpi::MP_OK;
}
#endif

std::vector< MpController::presetInfo > MpController::scanNativePresets()
{
	platform_string PresetFolder = toPlatformString(BundleInfo::instance()->getPresetFolder());

	auto extension = ToPlatformString(getNativePresetExtension());

	return scanPresetFolder(PresetFolder, extension);
}

void MpController::FileToString(const platform_string& path, std::string& buffer)
{
#if 0
	FILE* fp = fopen(path.c_str(), "rb");

	if(fp != NULL)
	{
		/* Go to the end of the file. */
		if(fseek(fp, 0L, SEEK_END) == 0) {
			/* Get the size of the file. */
			auto bufsize = ftell(fp);
			if(bufsize == -1) { /* Error */ }

			/* Allocate our buffer to that size. */
			buffer.resize(bufsize);

			/* Go back to the start of the file. */
			if(fseek(fp, 0L, SEEK_SET) == 0) { /* Error */ }

			/* Read the entire file into memory. */
			size_t newLen = fread((void*)buffer.data(), sizeof(char), bufsize, fp);
			if(newLen == 0) {
				fputs("Error reading file", stderr);
			}
		}
		fclose(fp);
	}
#else
	// fast file read.
	std::ifstream t(path, std::ifstream::in | std::ifstream::binary);
	t.seekg(0, std::ios::end);
	const size_t size = t.tellg();
	if (t.fail())
	{
		buffer.clear();
	}
	else
	{
		buffer.resize(size);
		t.seekg(0);
		t.read((char*)buffer.data(), buffer.size());
	}
#endif
}

MpController::presetInfo MpController::parsePreset(const std::wstring& filename, const std::string& xml)
{
	// file name overrides the name from XML
	std::string presetName;
	{
		std::wstring shortName, path_unused, extension;
		decompose_filename(filename, shortName, path_unused, extension);

		presetName = JmUnicodeConversions::WStringToUtf8(shortName);

		// Remove preset number prefix if present. "0023_Sax" -> "Sax"
		if (presetName.size() > 6
			&& presetName[4] == '_'
			&& isdigit(presetName[0])
			&& isdigit(presetName[1])
			&& isdigit(presetName[2])
			&& isdigit(presetName[3])
			)
		{
			presetName = presetName.substr(5);
		}
	}

	// load preset into a temporary object to get hash.
	auto preset = std::make_unique<DawPreset>(parametersInfo, xml);

	if (preset->name != presetName && !presetName.empty())
	{
		preset->name = presetName;

		// recalc hash with new name
		preset->calcHash();
	}

	return
	{
		preset->name,
		preset->category,
		-1,
		filename,
		preset->hash,
		false, // isFactory
		false  // isSession
	};
	}

std::vector< MpController::presetInfo > MpController::scanPresetFolder(platform_string PresetFolder, platform_string extension)
{
	std::vector< MpController::presetInfo > returnValues;

	const auto searchString = PresetFolder + platform_string(_T("*.")) + extension;
	const bool isXmlPreset = ToUtf8String(extension) == "xmlpreset";

	FileFinder it(searchString.c_str());
	for (; !it.done(); ++it)
	{
		if (!(*it).isFolder)
		{
			const auto sourceFilename = (*it).fullPath;

			std::string xml;
			if (isXmlPreset)
			{
				FileToString(sourceFilename, xml);
            }
			else
			{
                xml = loadNativePreset(ToWstring(sourceFilename));
			}

			if (!xml.empty())
			{
				const auto preset = parsePreset(ToWstring(sourceFilename), xml);
				if(!preset.filename.empty()) // avoid ones that fail to parse
				{
					returnValues.push_back(preset);
				}
			}
		}
	}

	return returnValues;
}

void MpController::setParameterValue(RawView value, int32_t parameterHandle, gmpi::Field paramField, int32_t voice)
{
	auto it = ParameterHandleIndex.find(parameterHandle);
	if (it == ParameterHandleIndex.end())
	{
		return;
	}

	auto seParameter = (*it).second;

	bool takeUndoSnapshot = false;

	// Special case for MIDI Learn
	if (paramField == gmpi::Field::MenuSelection)
	{
		auto choice = (int32_t)value;// RawToValue<int32_t>(value.data(), value.size());

			// 0 not used as it gets passed erroneously during init
		int cc = 0;
		if (choice == 1) // learn
		{
			cc = ControllerType::Learn;
		}
		else
		{
			if (choice == 2) // un-learn
			{
				cc = ControllerType::None;

				// set automation on GUI to 'none'
				seParameter->MidiAutomation = cc;
				updateGuis(seParameter, gmpi::Field::Automation);
			}
		}
		/*
		if( choice == 3 ) // Set via dialog
		{
		dlg_assign_controller dlg(getPatchManager(), this, CWnd::GetDesktopWindow());
		dlg.DoModal();
		}
		*/
		// Send MIDI learn message to DSP.
		//---send a binary message
		if (cc != 0)
		{
			my_msg_que_output_stream s(getQueueToDsp(), parameterHandle, "CCID");

			s << (int)sizeof(int);
			s << cc;
			s.Send();
		}
	}
	else
	{
		if (seParameter->setParameterRaw(paramField, value.size(), value.data(), voice))
		{
			seParameter->updateProcessor(paramField, voice);

			if (seParameter->stateful_ && paramField == gmpi::Field::Value)
			{
				if (!seParameter->isGrabbed()) // e.g. momentary button
				{
					takeUndoSnapshot = true;
				}
			}
		}
	}

	// take an undo snapshot anytime a knob is released
	if (paramField == gmpi::Field::Grab)
	{
		const bool grabbed = (bool)value;
		if (!grabbed && seParameter->stateful_)
		{
			takeUndoSnapshot = true;
		}
	}

	if (takeUndoSnapshot)
	{
		setModified(true);

		const auto paramName = WStringToUtf8((std::wstring)seParameter->getValueRaw(gmpi::Field::ShortName, 0));

		const std::string desc = "Changed parameter: " + paramName;
		undoManager.snapshot(this, desc);
	}
}

void UndoManager::debug()
{
#ifdef _WIN32
	_RPT0(0, "\n======UNDO=======\n");
	for (int i = 0 ; i < size() ; ++i)
	{
		_RPT1(0, "%c", i == undoPosition ? '>' : ' ');
		_RPTN(0, "%s\n", history[i].first.empty() ? "<init>" : history[i].first.c_str());
	}
	_RPTN(0, "CAN UNDO %d\n", (int)canUndo());
	_RPTN(0, "CAN REDO %d\n", (int)canRedo());
#endif
}

void UndoManager::setPreset(MpController* controller, DawPreset const* preset)
{
//	controller->dawStateManager.setPreset(preset);
	controller->setPresetFromSelf(preset);

#if defined(_DEBUG) && defined(_WIN32)
	_RPT0(0, "UndoManager::setPreset\n");
	debug();
#endif
}

void UndoManager::initial(MpController* controller, std::unique_ptr<const DawPreset> preset)
{
	history.clear();
	push({}, std::move(preset));

	UpdateGui(controller);

#ifdef _DEBUG
//	_RPT0(0, "UndoManager::initial (2)\n");
	debug();
#endif
}

bool UndoManager::canUndo()
{
	return undoPosition > 0 && undoPosition < size();
}

bool UndoManager::canRedo()
{
	return undoPosition >= 0 && undoPosition < size() - 1;
}

void UndoManager::UpdateGui(MpController* controller)
{
	*(controller->getHostParameter(HC_CAN_UNDO)) = canUndo();
	*(controller->getHostParameter(HC_CAN_REDO)) = canRedo();
	*(controller->getHostParameter(HC_PROGRAM_MODIFIED)) = canUndo();
}

DawPreset const* UndoManager::push(std::string description, std::unique_ptr<const DawPreset> preset)
{
	if (undoPosition < size() - 1)
	{
		history.resize(undoPosition + 1);
	}
	auto raw = preset.get();

	undoPosition = size();
	history.push_back({ description, std::move(preset) });

#if defined(_DEBUG) && defined(_WIN32)
	_RPT0(0, "UndoManager::push\n");
	debug();
#endif

	return raw;
}

void UndoManager::snapshot(MpController* controller, std::string description)
{
	if (!enabled)
		return;

	const auto couldUndo = canUndo();
	const auto couldRedo = canRedo();

	push(description, controller->getPreset());

	if(!couldUndo || couldRedo) // enable undo button
		UpdateGui(controller);

#if defined(_DEBUG) && defined(_WIN32)
	_RPT0(0, "UndoManager::snapshot\n");
	debug();
#endif
}

void UndoManager::undo(MpController* controller)
{
	if (undoPosition <= 0 || undoPosition >= size())
		return;

	--undoPosition;

	auto& preset = history[undoPosition].second;
	preset->resetUndo = false;

	setPreset(controller, preset.get());

	// if we're back to the original preset, set modified=false.
	if (!canUndo())
		controller->setModified(false);

	UpdateGui(controller);

#if defined(_DEBUG) && defined(_WIN32)
	_RPT0(0, "UndoManager::undo\n");
	debug();
#endif
}

void UndoManager::redo(MpController* controller)
{
	const auto next = undoPosition + 1;
	if (next < 0 || next >= size())
		return;

	auto& preset = history[next].second;

	preset->resetUndo = false;

	setPreset(controller, preset.get());

	undoPosition = next;

	controller->setModified(true);

	UpdateGui(controller);

#if defined(_DEBUG) && defined(_WIN32)
	_RPT0(0, "UndoManager::redo\n");
	debug();
#endif
}

void UndoManager::getA(MpController* controller)
{
	if (AB_is_A)
		return;

	AB_is_A = true;

	auto current = push("Choose A", controller->getPreset());

	setPreset(controller, &AB_storage);

	AB_storage = *current;
}

void UndoManager::getB(MpController* controller)
{
	if (!AB_is_A)
		return;

	AB_is_A = false;

	// first time clicking 'B' just assign current preset to 'B'
	if (AB_storage.empty())
	{
		AB_storage = *controller->getPreset();
		return;
	}

	auto current = push("Choose B", controller->getPreset());

//	controller->setPreset(AB_storage);
	setPreset(controller, &AB_storage);
	AB_storage = *current;
}

void UndoManager::copyAB(MpController* controller)
{
	if (AB_is_A)
	{
		AB_storage = *controller->getPreset();
	}
	else
	{
		setPreset(controller, &AB_storage);
	}
}
#if 0 // TODO

gmpi_gui::IMpGraphicsHost* MpController::getGraphicsHost()
{
#if !defined(SE_USE_JUCE_UI)
	for (auto g : m_guis2)
	{
		auto pa = dynamic_cast<GuiPatchAutomator3*>(g);
		if (pa)
		{
			auto gh = dynamic_cast<gmpi_gui::IMpGraphicsHost*>(pa->getHost());
			if (gh)
				return gh;
		}
	}
#endif

	return nullptr;
}
#endif

void MpController::OnSetHostControl(int hostControl, gmpi::Field paramField, int32_t size, const void* data, int32_t voice)
{
	switch (hostControl)
	{
	case HC_PROGRAM:
		if (!inhibitProgramChangeParameter && paramField == gmpi::Field::Value)
		{
			auto preset = RawToValue<int32_t>(data, size);

			MpParameter* programNameParam = nullptr;
			for (auto& p : parameters_)
			{
				if (p->getHostControl() == HC_PROGRAM_NAME)
				{
					programNameParam = p.get();
					break;
				}
			}

			if (preset >= 0 && preset < presets.size())
			{
				if (programNameParam)
				{
					const auto nameW = Utf8ToWstring(presets[preset].name);
					const auto raw2 = ToRaw4(nameW);
					const auto field = gmpi::Field::Value;
					if(programNameParam->setParameterRaw(field, raw2.size(), raw2.data()))
					{
						programNameParam->updateProcessor(field, voice);
					}
				}

				if (presets[preset].isSession)
				{
					// set Preset(&session_preset);
					setPresetFromSelf(&session_preset);
				}
				else if (presets[preset].isFactory)
				{
					loadFactoryPreset(preset, false);
				}
				else
				{
					std::string xml;
					if (presets[preset].filename.find(L".xmlpreset") != string::npos)
					{
						platform_string nativePath = toPlatformString(presets[preset].filename);
						FileToString(nativePath, xml);
					}
					else
					{
						xml = loadNativePreset(presets[preset].filename);
					}
					setPresetXmlFromSelf(xml);
				}

// already done by setPresetXmlFromSelf				undoManager.initial(this, getPreset());

				setModified(false);
			}
		}
		break;


	case HC_PATCH_COMMANDS:
		if (paramField == gmpi::Field::Value)
		{
			const auto patchCommand = *(int32_t*)data;

            if(patchCommand <= 0)
                break;
            
            // JUCE toolbar commands
			switch (patchCommand)
			{
			case (int) EPatchCommands::Undo:
				undoManager.undo(this);
				break;

			case (int)EPatchCommands::Redo:
				undoManager.redo(this);
				break;

			case (int)EPatchCommands::CompareGet_A:
				undoManager.getA(this);
				break;

			case (int)EPatchCommands::CompareGet_B:
				undoManager.getB(this);
				break;

			case (int)EPatchCommands::CompareGet_CopyAB:
				undoManager.copyAB(this);
				break;

			default:
				break;
			};

//#if !defined(SE_USE_JUCE_UI)
#if 0 // TODO
			// L"Load Preset=2,Save Preset,Import Bank,Export Bank"
            if (patchCommand > 5)
                break;

			auto gh = getGraphicsHost();

			if (!gh)
                break;
            
            int dialogMode = (patchCommand == 2 || patchCommand == 4) ? 0 : 1; // load or save.
            nativeFileDialog = nullptr; // release any existing dialog.
            gh->createFileDialog(dialogMode, nativeFileDialog.GetAddressOf());

            if (nativeFileDialog.isNull())
                break;
            
            if (patchCommand > 3)
            {
                nativeFileDialog.AddExtension("xmlbank", "XML Bank");
                auto fullPath = WStringToUtf8(BundleInfo::instance()->getUserDocumentFolder());
                combinePathAndFile(fullPath.c_str(), "bank.xmlbank");
                nativeFileDialog.SetInitialFullPath(fullPath);
            }
            else
            {
                const auto presetFolder = BundleInfo::instance()->getPresetFolder();
                CreateFolderRecursive(presetFolder);

				// default extension is the first one.
				if (getNativePresetExtension() == L"vstpreset")
				{
					nativeFileDialog.AddExtension("vstpreset", "VST3 Preset");
					//#ifdef _WIN32
					//					nativeFileDialog.AddExtension("fxp", "VST2 Preset");
					//#endif
				}
				else
				{
					nativeFileDialog.AddExtension("aupreset", "Audio Unit Preset");
				}
                nativeFileDialog.AddExtension("xmlpreset", "XML Preset");

				// least-relevant option last
				if (getNativePresetExtension() == L"vstpreset")
				{
					nativeFileDialog.AddExtension("aupreset", "Audio Unit Preset");
				}
				else
				{
					nativeFileDialog.AddExtension("vstpreset", "VST3 Preset");
				}
                nativeFileDialog.AddExtension("*", "All Files");
                nativeFileDialog.SetInitialFullPath(WStringToUtf8(presetFolder));
            }

            nativeFileDialog.ShowAsync([this, patchCommand](int32_t result) -> void { this->OnFileDialogComplete(patchCommand, result); });
#endif
		}

		break;
	}
}

#if 0 // TODO
int32_t MpController::sendSdkMessageToAudio(int32_t handle, int32_t id, int32_t size, const void* messageData)
{
	auto queue = getQueueToDsp();

	// discard any too-big message.
	const auto totalMessageSize = 4 * static_cast<int>(sizeof(int)) + size;
	if(totalMessageSize > queue->freeSpace())
		return gmpi::MP_FAIL;

	my_msg_que_output_stream s(queue, (int32_t)handle, "sdk\0");
    
	s << (int32_t)(size + 2 * sizeof(int32_t)); // size of ID plus sizeof message.

	s << id;

	s << size;
	s.Write(messageData, size);

    s.Send();
    
	return gmpi::MP_OK;
}
#endif

#if 0
// these can't update processor with normal handle-based method becuase their handles are assigned at runtime, only in the controller.
void MpController::HostControlToDsp(MpParameter* param, int32_t voice)
{
	assert(param->getHostControl() >= 0);

	my_msg_que_output_stream s(getQueueToDsp(), UniqueSnowflake::APPLICATION, "hstc");

	SerialiseParameterValueToDsp(s, param);
}
#endif

void MpController::SerialiseParameterValueToDsp(my_msg_que_output_stream& stream, MpParameter* param, int32_t voice)
{
	//---send a binary message
	bool isVariableSize = param->datatype_ == gmpi::PinDatatype::String || param->datatype_ == gmpi::PinDatatype::Blob;

	auto raw = param->getValueRaw(gmpi::Field::Value, voice);

	bool due_to_program_change = false;
	int32_t recievingMessageLength = (int)(sizeof(bool) + raw.size());
	if (isVariableSize)
	{
		recievingMessageLength += (int)sizeof(int32_t);
	}

	if (param->isPolyPhonic())
	{
		recievingMessageLength += (int)sizeof(int32_t);
	}

	stream << recievingMessageLength;
	stream << due_to_program_change;

	if (param->isPolyPhonic())
	{
		stream << voice;
	}

	if (isVariableSize)
	{
		stream << (int32_t)raw.size();
	}

	stream.Write(raw.data(), (unsigned int)raw.size());

	stream.Send();
}

void MpController::ParamToDsp(MpParameter* param, int32_t voice)
{
	assert(dynamic_cast<SeParameter_vst3_hostControl*>(param) == nullptr); // These have (not) "unique" handles that may map to totally random DSP parameters.

	my_msg_que_output_stream s(getQueueToDsp(), param->parameterHandle_, "ppc\0"); // "ppc"
	SerialiseParameterValueToDsp(s, param, voice);
}

void MpController::UpdateProgramCategoriesHc(MpParameter* param)
{
	ListBuilder_base<char> l;
	for (auto& preset : presets)
	{
		if(param->getHostControl() == HC_PROGRAM_CATEGORIES_LIST)
			l.Add(preset.category);
		else
		{
			assert(param->getHostControl() == HC_PROGRAM_NAMES_LIST);
			l.Add(preset.name);
		}
	}
	std::wstring_convert<std::codecvt_utf8<wchar_t>> convert;

	auto enumList = convert.from_bytes(l.str());

	param->setParameterRaw(gmpi::Field::Value, RawView(enumList));
}

MpParameter* MpController::createHostParameter(int32_t hostControl)
{
	SeParameter_vst3_hostControl* p = {};

	switch (hostControl)
	{
	case HC_PATCH_COMMANDS:
		p = new SeParameter_vst3_hostControl(this, hostControl);
		p->enumList_ = L"Load Preset=2,Save Preset,Import Bank,Export Bank";
		if (undoManager.enabled)
		{
			p->enumList_ += L", Undo=17, Redo";
		}
		break;

	case HC_CAN_UNDO:
	case HC_CAN_REDO:
		p = new SeParameter_vst3_hostControl(this, hostControl);
		break;

	case HC_PROGRAM:
	{
		p = new SeParameter_vst3_hostControl(this, hostControl);
		p->datatype_ = gmpi::PinDatatype::Int32;
		p->maximum = (std::max)(0.0, static_cast<double>(presets.size() - 1));
		const int32_t initialVal = -1; // ensure patch-browser shows <NULL> at first.
		RawView raw(initialVal);
		p->setParameterRaw(gmpi::Field::Value, (int32_t)raw.size(), raw.data());
	}
	break;

	case HC_PROGRAM_NAME:
		p = new SeParameter_vst3_hostControl(this, hostControl);
		{
			auto raw2 = ToRaw4(L"Factory");
			p->setParameterRaw(gmpi::Field::Value, (int32_t)raw2.size(), raw2.data());
		}
		break;

	case HC_PROGRAM_NAMES_LIST:
	{
		auto param = new SeParameter_vst3_hostControl(this, hostControl);
		p = param;
		p->datatype_ = gmpi::PinDatatype::String;

		UpdateProgramCategoriesHc(param);
	}
	break;

	case HC_PROGRAM_CATEGORIES_LIST:
	{
		auto param = new SeParameter_vst3_hostControl(this, hostControl);
		p = param;
		p->datatype_ = gmpi::PinDatatype::String;

		UpdateProgramCategoriesHc(param);
	}
	break;

	case HC_PROGRAM_MODIFIED:
	{
		auto param = new SeParameter_vst3_hostControl(this, hostControl);
		p = param;
		p->datatype_ = gmpi::PinDatatype::Bool;
	}
	break;


	/* what would it do?
	case HC_MIDI_CHANNEL:
	break;
	*/
	}

	if (!p)
		return {};

	p->stateful_ = false;

	// clashes with valid handles on DSP, ensure NEVER sent to DSP!!

	// generate unique parameter handle, assume all other parameters already registered.
	p->parameterHandle_ = 0;
	if (!ParameterHandleIndex.empty())
		p->parameterHandle_ = ParameterHandleIndex.rbegin()->first + 1;

	ParameterHandleIndex.insert({ p->parameterHandle_, p });
	parameters_.push_back(std::unique_ptr<MpParameter>(p));

	return p;
}

MpParameter* MpController::getHostParameter(int32_t hostControl)
{
	const auto it = std::find_if(
		parameters_.begin()
		, parameters_.end()
		, [hostControl](std::unique_ptr<MpParameter>& p) {return p->getHostControl() == hostControl; }
	);
	
	if(it != parameters_.end())
		return (*it).get();

	return createHostParameter(hostControl);
}

#if 0 // TODO

int32_t MpController::getParameterHandle(int32_t moduleHandle, int32_t moduleParameterId)
{
	int hostControl = -1 - moduleParameterId;

	if (hostControl >= 0)
	{
		// why not just shove it in with negative handle? !!! A: becuase of potential attachment to container.
		for (auto& p : parameters_)
		{
			if (p->getHostControl() == hostControl && (moduleHandle == -1 || moduleHandle == p->ModuleHandle()))
			{
				return p->parameterHandle_;
				break;
			}
		}

		if (auto p = createHostParameter(hostControl); p)
		{
			return p->parameterHandle_;
		}
	}
	else
	{
		auto it = moduleParameterIndex.find(std::make_pair(moduleHandle, moduleParameterId));
		if (it != moduleParameterIndex.end())
			return (*it).second;
	}

	return -1;
}
#endif

void MpController::initializeGui(gmpi::api::IParameterObserver* gui, int32_t parameterHandle, gmpi::Field FieldId)
{
	auto it = ParameterHandleIndex.find(parameterHandle);

	if (it != ParameterHandleIndex.end())
	{
		auto p = (*it).second;

		for (int voice = 0; voice < p->getVoiceCount(); ++voice)
		{
			auto raw = p->getValueRaw(FieldId, voice);
			gui->setParameter(parameterHandle, FieldId, voice, (int32_t)raw.size(), raw.data());
		}
	}
}

bool MpController::onQueMessageReady(int recievingHandle, int recievingMessageId, class my_input_stream& p_stream)
{
	auto it = ParameterHandleIndex.find(recievingHandle);
	if (it != ParameterHandleIndex.end())
	{
		auto p = (*it).second;
		p->updateFromDsp(recievingMessageId, p_stream);
		return true;
	}
	else
	{
		switch(recievingMessageId)
		{
		case id_to_long2("sdk"):
		{
			struct DspMsgInfo2
			{
				int id;
				int size;
				void* data;
				int handle;
			};
			DspMsgInfo2 nfo;
			p_stream >> nfo.id;
			p_stream >> nfo.size;
			nfo.data = malloc(nfo.size);
			p_stream.Read(nfo.data, nfo.size);
			nfo.handle = recievingHandle;

#if 0 // TODO
			if (presenter_)
				presenter_->OnChildDspMessage(&nfo);
#endif

			free(nfo.data);

			return true;
		}
		break;

		case id_to_long2("ltnc"): // latency changed. VST3 or AU.
		{
			OnLatencyChanged();
		}
		break;

#if defined(_DEBUG) && defined(_WIN32)
		default:
		{
			const char* msgstr = (const char*)&recievingMessageId;
			_RPT1(_CRT_WARN, "MpController::onQueMessageReady() Unhandled message id %c%c%c%c\n", msgstr[3], msgstr[2], msgstr[1], msgstr[0] );
		}
		break;
#endif
		}
	}
	return false;
}

bool MpController::onTimer()
{
	message_que_dsp_to_ui.pollMessage(this);

	if (startupTimerCounter-- == 0)
	{
		OnStartupTimerExpired();
	}
	
	if (presetsFolderChanged)
	{
		presetsFolderChanged = false;
		ScanPresets();
		UpdatePresetBrowser();
	}

	return true;
}

void MpController::OnStartupTimerExpired()
{
	if (BundleInfo::instance()->getPluginInfo().emulateIgnorePC)
	{
		// class UniqueSnowflake
		enum { NONE = -1, DEALLOCATED = -2, APPLICATION = -4 };

		my_msg_que_output_stream s(getQueueToDsp(), /*UniqueSnowflake::*/ APPLICATION, "EIPC"); // Emulate Ignore Program Change
		s << (uint32_t)0;
		s.Send();
	}
}
#if 0 // TODO

int32_t MpController::resolveFilename(const wchar_t* shortFilename, int32_t maxChars, wchar_t* returnFullFilename)
{
	// copied from CSynthEditAppBase.

	std::wstring l_filename(shortFilename);
	std::wstring file_ext;
	file_ext = GetExtension(l_filename);

    const bool isUrl = l_filename.find(L"://") != string::npos;
    
    // Is this a relative or absolute filename?
#ifdef _WIN32
    const bool has_root_path = l_filename.find(L':') != string::npos;
#else
    const bool has_root_path = l_filename.size() > 0 && l_filename[0] == L'/';
#endif
    
	if (!has_root_path && !isUrl)
	{
//		auto default_path = BundleInfo::instance()->getImbedded FileFolder();
		const auto default_path = BundleInfo::instance()->getResourceFolder();

		l_filename = combine_path_and_file(default_path, l_filename);
	}

	if (l_filename.size() >= static_cast<size_t> (maxChars))
	{
		// return empty string (if room).
		if (maxChars > 0)
			returnFullFilename[0] = 0;

		return gmpi::MP_FAIL;
	}

	WStringToWchars(l_filename, returnFullFilename, maxChars);

	return gmpi::MP_OK;
}
#endif

void MpController::OnFileDialogComplete(int patchCommand, int32_t result)
{
#if 0 // TODO
	if (result == gmpi::MP_OK)
	{
		auto fullpath = nativeFileDialog.GetSelectedFilename();
		auto filetype = GetExtension(fullpath);
		bool isXmlPreset = filetype == "xmlpreset";

		switch (patchCommand) // L"Load Preset=2,Save Preset,Import Bank,Export Bank"
		{
		case 2:	// Load Preset
			if (isXmlPreset)
				ImportPresetXml(fullpath.c_str());
			else
			{
				auto xml = loadNativePreset( Utf8ToWstring(fullpath) );
				setPresetXmlFromSelf(xml);
			}
			break;

		case 3:	// Save Preset
			if (isXmlPreset)
				ExportPresetXml(fullpath.c_str());
			else
			{
				// Update preset name and category, so filename matches name in browser (else very confusing).
				std::wstring r_file, r_path, r_extension;
				decompose_filename(Utf8ToWstring(fullpath), r_file, r_path, r_extension);

				// Update program name and category (as they are queried by getPreset() ).
				for (auto& p : parameters_)
				{
					if (p->getHostControl() == HC_PROGRAM_NAME)
					{
						p->setParameterRaw(gmpi::Field::Value, RawView(r_file));
						p->updateProcessor(gmpi::Field::Value, 0); // Important that processor has correct name when DAW saves the session.
						updateGuis(p.get(), gmpi::Field::Value);
					}

					// Presets saved by user go into "User" category.
					if (p->getHostControl() == HC_PROGRAM_CATEGORY)
					{
						std::wstring category{L"User"};
						p->setParameterRaw(gmpi::Field::Value, RawView(category));
						p->updateProcessor(gmpi::Field::Value, 0);
						updateGuis(p.get(), gmpi::Field::Value);
					}
				}

				saveNativePreset(fullpath.c_str(), WStringToUtf8(r_file), getPreset()->toString(BundleInfo::instance()->getPluginId()));

				ScanPresets();
				UpdatePresetBrowser();

				// Update current preset name in browser.
				for (auto& p : parameters_)
				{
					if (p->getHostControl() == HC_PROGRAM)
					{
						auto nameU = WStringToUtf8(r_file);

						for (int32_t i = 0; i < presets.size(); ++i)
						{
							if (presets[i].name == nameU && presets[i].category == "User")
							{
								p->setParameterRaw(gmpi::Field::Value, RawView(i));

								updateGuis(p.get(), gmpi::Field::Value);
								break;
							}
						}
					}
				}
			}
			break;

		case 4:
			ImportBankXml(fullpath.c_str());
			break;

		case 5:
			ExportBankXml(fullpath.c_str());
			break;
		}
	}

	nativeFileDialog.setNull(); // release it.
#endif
}

void MpController::ImportPresetXml(const char* filename, int presetIndex)
{
	platform_string nativePath = toPlatformString(filename);
	std::string newXml;
	FileToString(nativePath, newXml);

	setPresetXmlFromSelf(newXml);
}

std::unique_ptr<const DawPreset> MpController::getPreset(std::string presetNameOverride)
{
	auto preset = std::make_unique<DawPreset>();

	for (auto& p : parameters_)
	{
		if (p->getHostControl() == HC_PROGRAM_NAME)
		{
			preset->name = WStringToUtf8((std::wstring)p->getValueRaw(gmpi::Field::Value, 0));
			continue; // force non-save
		}
		if (p->getHostControl() == HC_PROGRAM_CATEGORY)
		{
			preset->category = WStringToUtf8((std::wstring)p->getValueRaw(gmpi::Field::Value, 0));
			continue; // force non-save
		}

		if (p->stateful_)
		{
			const auto paramHandle = p->parameterHandle_;
			auto& values = preset->params[paramHandle];

			values.dataType = (gmpi::PinDatatype)p->datatype_;

			const int voice = 0;
			const auto raw = p->getValueRaw(gmpi::Field::Value, voice);
			values.rawValues_.push_back({ (char* const)raw.data(), raw.size() });

#if 0
	// MIDI learn.
			if (parameter->MidiAutomation != -1)
			{
				paramElement->SetAttribute("MIDI", parameter->MidiAutomation);

				if (!parameter->MidiAutomationSysex.empty())
					paramElement->SetAttribute("MIDI_SYSEX", WStringToUtf8(parameter->MidiAutomationSysex));
			}
#endif
		}
	}

	if (!presetNameOverride.empty())
	{
		preset->name = presetNameOverride;
	}

#if 0 // ??
	{
		char buffer[20];
		sprintf(buffer, "%08x", BundleInfo::instance()->getPluginId());
		element->SetAttribute("pluginId", buffer);
	}
#endif

	preset->calcHash();

	return preset; // dawStateManager.retainPreset(preset);
}

int32_t MpController::getParameterModuleAndParamId(int32_t parameterHandle, int32_t* returnModuleHandle, int32_t* returnModuleParameterId)
{
	auto it = ParameterHandleIndex.find(parameterHandle);
	if (it != ParameterHandleIndex.end())
	{
		auto seParameter = (*it).second;
		*returnModuleHandle = seParameter->moduleHandle_;
		*returnModuleParameterId = seParameter->moduleParamId_;
		return 0;// gmpi::MP_OK;
	}
	return -1;// gmpi::MP_FAIL;
}
#if 0 // TODO

RawView MpController::getParameterValue(int32_t parameterHandle, int32_t fieldId, int32_t voice)
{
	auto it = ParameterHandleIndex.find(parameterHandle);
	if (it != ParameterHandleIndex.end())
	{
		auto param = (*it).second;
		return param->getValueRaw((gmpi::Field) fieldId, 0);
	}

	return {};
}
#endif

void MpController::OnEndPresetChange()
{
	// try to 'debounce' multiple preset changes at startup.
	if (startupTimerCounter > 0)
	{
		startupTimerCounter = startupTimerInit;
	}
}

// new: set preset UI only. Processor is updated in parallel
void MpController::setPreset(DawPreset const* preset)
{
#if 0 // TODO
	//	_RPTN(0, "MpController::setPreset. IPC %d\n", (int)preset->ignoreProgramChangeActive);

	constexpr int patch = 0;
	constexpr bool updateProcessor = false;

	const std::wstring categoryNameW = Utf8ToWstring(preset->category);
	auto parameterHandle = getParameterHandle(-1, -1 - HC_PROGRAM_CATEGORY);
	auto it = ParameterHandleIndex.find(parameterHandle);
	if (it != ParameterHandleIndex.end())
	{
		auto p = (*it).second;
		p->setParameterRaw(gmpi::Field::Value, RawView(categoryNameW)); // don't check changed flag, if even originated from GUI, param is already changed. Still need top go to DSP.
/*
		if (updateProcessor)
		{
			p->updateProcessor(gmpi::Field::Value, voiceId);
		}
*/
	}
	{
		const std::wstring nameW = Utf8ToWstring(preset->name);
		auto parameterHandle = getParameterHandle(-1, -1 - HC_PROGRAM_NAME);
		auto it = ParameterHandleIndex.find(parameterHandle);
		if (it != ParameterHandleIndex.end())
		{
			auto p = (*it).second;
			p->setParameterRaw(gmpi::Field::Value, RawView(nameW)); // don't check changed flag, if even originated from GUI, param is already changed. Still need top go to DSP.
/*
			if (updateProcessor)
			{
				p->updateProcessor(gmpi::Field::Value, voiceId);
			}
*/
		}
	}

	for (const auto& [handle, val] : preset->params)
	{
		assert(handle != -1);

		auto it = ParameterHandleIndex.find(handle);
		if (it == ParameterHandleIndex.end())
			continue;

		auto& parameter = (*it).second;

		assert(parameter->datatype_ == (int)val.dataType);

		if (parameter->datatype_ != (int)val.dataType)
			continue;

		if (parameter->ignorePc_ && preset->ignoreProgramChangeActive)
			continue;

		for (int voice = 0; voice < val.rawValues_.size(); ++voice)
		{
			const auto& raw = val.rawValues_[voice];

			// This block seems messy. Should updating a parameter be a single function call?
			// (would need to pass 'updateProcessor')
			{
				// calls controller_->updateGuis(this, voice)
				parameter->setParameterRaw(gmpi::Field::Value, (int32_t)raw.size(), raw.data(), voice);

				// updated cached value.
				parameter->upDateImmediateValue();

				if (updateProcessor) // For non-private parameters, update DAW.
				{
					parameter->updateProcessor(gmpi::Field::Value, voice);
				}
			}
#if 0
			// TODO I guess
			// MIDI learn.
			if (updateProcessor && formatVersion > 0)
			{
				int32_t midiController = -1;
				ParamElement->QueryIntAttribute("MIDI", &midiController);
				{
					my_msg_que_output_stream s(getQueueToDsp(), parameter->parameterHandle_, "CCID");
					s << static_cast<uint32_t>(sizeof(midiController));
					s << midiController;
					s.Send();
				}

				std::string sysexU;
				ParamElement->QueryStringAttribute("MIDI_SYSEX", &sysexU);
				{
					const auto sysex = Utf8ToWstring(sysexU);

					my_msg_que_output_stream s(getQueueToDsp(), parameter->parameterHandle_, "CCSX");
					s << static_cast<uint32_t>(sizeof(int32_t) + sizeof(wchar_t) * sysex.size());
					s << sysex;
					s.Send();
				}
			}
#endif
		}
	}

	if (preset->resetUndo)
	{
		auto copyofpreset = std::make_unique<DawPreset>(*preset);
		undoManager.initial(this, std::move(copyofpreset));
	}
#endif

	syncPresetControls(preset);
}

// after setting a preset, try to make sense of it in terms of the existing preset list.
void MpController::syncPresetControls(DawPreset const* preset)
{
	// if this is an undo/redo, no need to update preset list
	if (!preset->resetUndo)
		return;

	constexpr bool updateProcessor = false;

//	_RPTN(0, "syncPresetControls Preset: %s hash %4x\n", preset->name.c_str(), preset->hash);

	const std::string presetName = preset->name.empty() ? "Default" : preset->name;
	//{
	//	auto parameterHandle = getParameterHandle(-1, -1 - HC_PROGRAM_NAME);
	//	if (auto it = ParameterHandleIndex.find(parameterHandle) ; it != ParameterHandleIndex.end())
	//	{
	//		const auto raw = (*it).second->getValueRaw(gmpi::Field::Value, 0);
	//		auto presetNameW = RawToValue<std::wstring>(raw.data(), raw.size());
	//		presetName = WStringToUtf8(presetNameW);
	//	}
	//}

	// When DAW loads preset XML, try to determine if it's a factory preset, and update browser to suit.
	int32_t presetIndex = -1; // exact match
	int32_t presetSameNameIndex = -1; // name matches, but not settings.
#if 0 // TODO

	/*
	XML will not match if any parameter was set outside the normalized range, because it will get clamped in the plugin.
	*/
  //  _RPT2(_CRT_WARN, "setPresetFromDaw: hash=%d\nXML:\n%s\n", (int) std::hash<std::string>{}(xml), xml.c_str());

	// If preset has no name, treat it as a (modified) "Default"
	//if (presetName.empty())
	//	presetName = "Default";

	// Check if preset coincides with a factory preset, if so update browser to suit.
	int idx = 0;
	for (const auto& factoryPreset : presets)
	{
		assert(factoryPreset.hash);

//		_RPTN(0, "                   factoryPreset: %s hash %4x\n", factoryPreset.name.c_str(), factoryPreset.hash);
		if (factoryPreset.hash == preset->hash)
		{
			presetIndex = idx;
			presetSameNameIndex = -1;
			break;
		}
		if (factoryPreset.name == presetName && !factoryPreset.isSession)
		{
			presetSameNameIndex = idx;
		}
		
		++idx;
	}

	if (presetIndex == -1)
	{
		if (presetSameNameIndex != -1)
		{
			// same name as an existing preset, but not the same parameter values.
			// assume it's the same preset, except it's been modified
			presetIndex = presetSameNameIndex;

			DawPreset const* preset{};

			// put original as undo state
			std::string newXml;
			if (presets[presetIndex].isFactory) // preset is contained in binary
			{
				newXml = getFactoryPresetXml(presets[presetIndex].name + ".xmlpreset");
			}
			else
			{
				if (presets[presetIndex].filename.find(L".xmlpreset") != string::npos)
				{
					platform_string nativePath = toPlatformString(presets[presetIndex].filename);
					FileToString(nativePath, newXml);
				}
				else
				{
					newXml = loadNativePreset(presets[presetIndex].filename);
				}
			}
			auto unmodifiedPreset = std::make_unique<DawPreset>(parametersInfo, newXml);
			undoManager.initial(this, std::move(unmodifiedPreset));

			undoManager.snapshot(this, "Load Session Preset");
			setModified(true);
		}
		else
		{
			// remove any existing "Session preset"
			presets.erase(
				std::remove_if(presets.begin(), presets.end(), [](presetInfo& preset) { return preset.isSession; })
				, presets.end()
			);
//			session_preset_xml.clear();
			
			// preset not available and not the same name as any existing ones, add it to presets as 'session' preset.
			presetIndex = static_cast<int32_t>(presets.size());

			presets.push_back(
			{
				presetName,
				preset->category,
				presetIndex,			// Internal Factory presets only.
				{},						// filename: External disk presets only.
				preset->hash,
				false,					// isFactory
				true					// isSession
				}
			);

			session_preset = *preset;
		}
	}

	{
		auto parameterHandle = getParameterHandle(-1, -1 - HC_PROGRAM);
		auto it = ParameterHandleIndex.find(parameterHandle);
		if (it != ParameterHandleIndex.end())
		{
			auto p = (*it).second;
			inhibitProgramChangeParameter = true;
//			_RPTN(0, "syncPresetControls Preset index: %d\n", presetIndex);
			if(p->setParameterRaw(gmpi::Field::Value, RawView(presetIndex)))
			{
				updateGuis(p, gmpi::Field::Value);
// VST2 only I think				p->updateProcessor(gmpi::Field::Value, 0); // Unusual. Informs VST2 DAW of program number.
			}

			inhibitProgramChangeParameter = false;
		}
		parameterHandle = getParameterHandle(-1, -1 - HC_PROGRAM_NAME);
		it = ParameterHandleIndex.find(parameterHandle);
		if (it != ParameterHandleIndex.end())
		{
			auto p = (*it).second;

			std::wstring name;
			if(presetIndex == -1)
			{
				const auto raw = p->getValueRaw(gmpi::Field::Value, 0);
				name = RawToValue<std::wstring>(raw.data(), raw.size());
			}
			else
			{
				// Preset found.
				name = Utf8ToWstring(presets[presetIndex].name);
			}

			if(p->setParameterRaw(gmpi::Field::Value, RawView(name)) && updateProcessor)
			{
				p->updateProcessor(gmpi::Field::Value, 0);
			}
		}
	}
#endif
}

bool MpController::isPresetModified()
{
	return undoManager.canUndo();
}

void MpController::SavePreset(int32_t presetIndex)
{
	const auto presetFolderW = BundleInfo::instance()->getPresetFolder();
	CreateFolderRecursive(presetFolderW);

	auto preset = getPresetInfo(presetIndex);
	const auto presetFolder = WStringToUtf8(presetFolderW);
	const auto fullPath = combinePathAndFile(presetFolder, preset.name) + ".xmlpreset";

	ExportPresetXml(fullPath.c_str());

	setModified(false);
	undoManager.initial(this, getPreset());

	ScanPresets();
	UpdatePresetBrowser();
}

void MpController::SavePresetAs(const std::string& presetName)
{
	const auto presetFolderW = BundleInfo::instance()->getPresetFolder();

	assert(!presetFolderW.empty()); // you need to call BundleInfo::initPresetFolder(manufacturer, product) when initializing this plugin.

	CreateFolderRecursive(presetFolderW);

	const auto presetFolder = WStringToUtf8(presetFolderW);
	const auto fullPath = combinePathAndFile(presetFolder, presetName ) + ".xmlpreset";

	ExportPresetXml(fullPath.c_str(), presetName);

	setModified(false);

	ScanPresets();

	// Add new preset to combo
	UpdatePresetBrowser();
#if 0 // TODO

	// find the new preset and select it.
	for (int32_t presetIndex = 0; presetIndex < presets.size(); ++presetIndex)
	{
		if (presets[presetIndex].name == presetName)
		{
			auto parameterHandle = getParameterHandle(-1, -1 - HC_PROGRAM);
			auto it = ParameterHandleIndex.find(parameterHandle);
			if (it != ParameterHandleIndex.end())
			{
				auto p = (*it).second;
				p->setParameterRaw(gmpi::Field::Value, RawView(presetIndex));
			}
			break;
		}
	}
#endif
}

void MpController::DeletePreset(int presetIndex)
{
#if 0 // TODO
	assert(presetIndex >= 0 && presetIndex < presets.size());

	auto parameterHandle = getParameterHandle(-1, -1 - HC_PROGRAM);
	auto it = ParameterHandleIndex.find(parameterHandle);
	if (it != ParameterHandleIndex.end())
	{
		auto p = (*it).second;

		auto currentPreset = (int32_t) p->getValueRaw(gmpi::Field::Value, 0);

		// if we're deleting the current preset, switch back to preset 0
		if (currentPreset == presetIndex)
		{
			int32_t newCurrentPreset = 0;
			(*p) = newCurrentPreset;
		}
	}
#if defined(__cpp_lib_filesystem)
	std::filesystem::remove(presets[presetIndex].filename);
#else
    std::remove(WStringToUtf8(presets[presetIndex].filename).c_str());
#endif

	ScanPresets();
	UpdatePresetBrowser();
#endif
}

// Note: Don't handle polyphonic stateful parameters.
void MpController::ExportPresetXml(const char* filename, std::string presetNameOverride)
{
	ofstream myfile;
	myfile.open(filename);

	myfile << getPreset(presetNameOverride)->toString(BundleInfo::instance()->getPluginId());

	myfile.close();
}

void MpController::ExportBankXml(const char* filename)
{
	// Create output XML document.
	tinyxml2::XMLDocument xml;
	xml.LinkEndChild(xml.NewDeclaration());

	auto presets_xml = xml.NewElement("Presets");
	xml.LinkEndChild(presets_xml);

	// Iterate native preset files, combine them into bank, and export.
	auto srcFolder = ToPlatformString(BundleInfo::instance()->getPresetFolder());
	auto searchString = srcFolder + _T("*.");
	searchString += ToPlatformString(getNativePresetExtension());
	for (FileFinder it = searchString.c_str(); !it.done(); ++it)
	{
		if (!(*it).isFolder)
		{
			auto sourceFilename = combine_path_and_file(srcFolder, (*it).filename);

			auto presetName = (*it).filename;
			{
				// chop off extension
				auto p = presetName.find_last_of(L'.');
				if (p != std::string::npos)
					presetName = presetName.substr(0, p);
			}

			auto chunk = loadNativePreset( ToWstring(sourceFilename) );

			{
				tinyxml2::XMLDocument presetDoc;

				presetDoc.Parse(chunk.c_str());

				if (!presetDoc.Error())
				{
					auto parameters = presetDoc.FirstChildElement("Preset");
					auto copyOfParameters = parameters->DeepClone(&xml)->ToElement();
					presets_xml->LinkEndChild(copyOfParameters);
					copyOfParameters->SetAttribute("name", ToUtf8String(presetName).c_str());
				}
			}
		}
	}

	// Save output XML document.
	xml.SaveFile(filename);
}

void MpController::ImportBankXml(const char* xmlfilename)
{
#if 0 // TODO
	auto presetFolder = BundleInfo::instance()->getPresetFolder();

	CreateFolderRecursive(presetFolder);

//	TiXmlDocument doc; // Don't use tinyXML2. XML must match *exactly* the current format, including indent, declaration, everything. Else Preset Browser won't correctly match presets.
//	doc.LoadFile(xmlfilename);
	tinyxml2::XMLDocument doc;
	doc.LoadFile(xmlfilename);

	if (doc.Error())
	{
		assert(false);
		return;
	}

	auto presetsE = doc.FirstChildElement("Presets");

	for (auto PresetE = presetsE->FirstChildElement("Preset"); PresetE; PresetE = PresetE->NextSiblingElement())
	{
		// Query plugin's 4-char code. Presence Indicates also that preset format supports MIDI learn.
		int32_t fourCC = -1; // -1 = not specified.
		int formatVersion = 0;
		{
			const char* hexcode{};
			if (tinyxml2::XMLError::XML_SUCCESS == PresetE->QueryStringAttribute("pluginId", &hexcode))
			{
				formatVersion = 1;
				try
				{
					fourCC = std::stoul(hexcode, nullptr, 16);
				}
				catch (...)
				{
					// who gives a f*ck
				}
			}
		}

		// TODO !!! Check fourCC.

		const char* name{};
		if (tinyxml2::XML_SUCCESS != PresetE->QueryStringAttribute("name", &name))
		{
			PresetE->QueryStringAttribute("Name", &name); // old format used to be capitalized.
		}
		auto filename = presetFolder + Utf8ToWstring(name) + L".";
		filename += getNativePresetExtension();

		// Create a new XML document, containing only one preset.
		tinyxml2::XMLDocument doc2;
		doc2.LinkEndChild(doc2.NewDeclaration());// new TiXmlDeclaration("1.0", "", "") );
		doc2.LinkEndChild(PresetE->DeepClone(&doc2));

		tinyxml2::XMLPrinter printer;
//		printer. .SetIndent(" ");
		doc2.Accept(&printer);
		const std::string presetXml{ printer.CStr() };

		// dialog if file exists.
//		auto result = gmpi::MP_OK;

/* no mac support
		fs::path fn(filename);
		if (fs::exists(fn))
*/
#ifdef _WIN32
        auto file = _wfopen(filename.c_str(), L"r"); // fs::exists(filename)
#else
        auto file = fopen(WStringToUtf8(filename).c_str(), "r");
#endif
        if(file)
		{
			fclose(file);

			auto gh = getGraphicsHost();

			if (gh)
			{
				okCancelDialog.setNull(); // free previous.
				gh->createOkCancelDialog(0, okCancelDialog.GetAddressOf());

				if (okCancelDialog.isNull())
					return;

				std::ostringstream oss;
				oss << "Overwrite preset '" << name << "'?";
				okCancelDialog.SetText(oss.str().c_str());

				okCancelDialog.ShowAsync([this, name, presetXml, filename] (int32_t result) -> void
					{
						if( result == gmpi::MP_OK )
                            saveNativePreset(
                                 WStringToUtf8(filename).c_str(),
                                 name,
                                 presetXml
                            );
					}
				);
			}
		}
		else
		{
			saveNativePreset(WStringToUtf8(filename).c_str(), name, presetXml);
		}
	}

	ScanPresets();
	UpdatePresetBrowser();
#endif
}

void MpController::setModified(bool presetIsModified)
{
	(*getHostParameter(HC_PROGRAM_MODIFIED)) = presetIsModified;
}
}