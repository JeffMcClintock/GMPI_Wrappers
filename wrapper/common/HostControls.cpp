//#include "pch.h"

// Fix for <sstream> on Mac (sstream uses undefined int_64t)
//#include "mp_api.h"
#include <sstream>
#include <assert.h>
#include <unordered_map>

#include "HostControls.h"
#include "midi_defs.h"

using namespace std;

namespace wrapper
{

struct HostControlStruct // holds XML -> enum info
{
	const char* display;
	HostControls id;
	gmpi::PinDatatype datatype;
	int automation;
};

/* categories:
- Voice - Note expression. Polyphonic parameters targeted at individual voices.
- Time  - Song position and tempo related.
*/

static const HostControlStruct lookup[] =
{
	{("PatchCommands")			,HC_PATCH_COMMANDS, gmpi::PinDatatype::Int32,ControllerType::None},

	{("MidiChannelIn")			,HC_MIDI_CHANNEL, gmpi::PinDatatype::Int32,ControllerType::None},

	{("ProgramNamesList")		,HC_PROGRAM_NAMES_LIST, gmpi::PinDatatype::String,ControllerType::None},

	{("Program")				,HC_PROGRAM, gmpi::PinDatatype::Int32,ControllerType::None},

	{("ProgramName")			,HC_PROGRAM_NAME, gmpi::PinDatatype::String,ControllerType::None},

	{("Voice/Trigger")			,HC_VOICE_TRIGGER, gmpi::PinDatatype::Float32,ControllerType::Trigger << 24},

	{("Voice/Gate")			,HC_VOICE_GATE, gmpi::PinDatatype::Float32,ControllerType::Gate << 24},

	{("Voice/Pitch")			,HC_VOICE_PITCH, gmpi::PinDatatype::Float32,ControllerType::Pitch << 24},

	{("Voice/VelocityKeyOn")	,HC_VOICE_VELOCITY_KEY_ON, gmpi::PinDatatype::Float32,ControllerType::VelocityOn << 24},

	{("Voice/VelocityKeyOff")	,HC_VOICE_VELOCITY_KEY_OFF, gmpi::PinDatatype::Float32,ControllerType::VelocityOff << 24},

	{("Voice/Aftertouch")		,HC_VOICE_AFTERTOUCH, gmpi::PinDatatype::Float32,ControllerType::PolyAftertouch << 24},

	{("Voice/VirtualVoiceId")	,HC_VOICE_VIRTUAL_VOICE_ID, gmpi::PinDatatype::Int32,ControllerType::VirtualVoiceId << 24},

	{("Voice/Active")			,HC_VOICE_ACTIVE				, gmpi::PinDatatype::Float32,ControllerType::Active << 24},

	{("XXXXXXXXXX")			,HC_UNUSED						, gmpi::PinDatatype::Float32, ControllerType::None},// was HC_VOICE_RESET -"Voice/Reset"

	{("VoiceAllocationMode")	,HC_VOICE_ALLOCATION_MODE		, gmpi::PinDatatype::Int32, ControllerType::None},

	{("Bender")				,HC_PITCH_BENDER				, gmpi::PinDatatype::Float32, ControllerType::Bender << 24},

	{("HoldPeda")				,HC_HOLD_PEDAL					, gmpi::PinDatatype::Float32, (ControllerType::CC << 24) | 64}, // CC 64 = Hold Pedal

	{("Channel Pressure")		,HC_CHANNEL_PRESSURE			, gmpi::PinDatatype::Float32, ControllerType::ChannelPressure << 24},

	{("Time/BPM")				,HC_TIME_BPM					, gmpi::PinDatatype::Float32, ControllerType::BPM << 24},

	{("Time/SongPosition")		,HC_TIME_QUARTER_NOTE_POSITION	, gmpi::PinDatatype::Float32, ControllerType::SongPosition << 24},

	{("Time/TransportPlaying")	,HC_TIME_TRANSPORT_PLAYING		, gmpi::PinDatatype::Bool, ControllerType::TransportPlaying << 24},
	{("Polyphony")				,HC_POLYPHONY					, gmpi::PinDatatype::Int32, ControllerType::None},
	{("ReserveVoices")			,HC_POLYPHONY_VOICE_RESERVE		, gmpi::PinDatatype::Int32, ControllerType::None},
	{("Oversampling/Rate")		,HC_OVERSAMPLING_RATE			, gmpi::PinDatatype::Enum, ControllerType::None},
	{("Oversampling/Filter")	, HC_OVERSAMPLING_FILTER		, gmpi::PinDatatype::Enum, ControllerType::None},
	{("User/Int0")				, HC_USER_SHARED_PARAMETER_INT0	, gmpi::PinDatatype::Int32, ControllerType::None},
	{("Time/BarStartPosition"), HC_TIME_BAR_START				, gmpi::PinDatatype::Float32, ControllerType::barStartPosition << 24},
	{("Time/Timesignature/Numerator"), HC_TIME_NUMERATOR		, gmpi::PinDatatype::Int32, ControllerType::timeSignatureNumerator << 24},
	{("Time/Timesignature/Denominator"), HC_TIME_DENOMINATOR	, gmpi::PinDatatype::Int32, ControllerType::timeSignatureDenominator << 24},

	// VST3 Note-expression (and MIDI 2.0 support)
	// multitrack studio: "VST3 instruments that support VST3 note expression receive per-note Pitch Bend, Volume, Pan, Expression, Brightness and Vibrato Depth. Poly Aftertouch is sent as well."

	// MIDI 2.0 Per-Note Expression 7
	{("Voice/Volume")		, HC_VOICE_VOLUME, gmpi::PinDatatype::Float32, (ControllerType::VoiceNoteExpression << 24) | (ControllerType::kVolumeTypeID << 16)},

	// MIDI 2.0 Per-Note Expression 10
	{("Voice/Pan")			, HC_VOICE_PAN, gmpi::PinDatatype::Float32, (ControllerType::VoiceNoteExpression << 24) | (ControllerType::kPanTypeID << 16)},

	// MIDI 2.0 Per-Note Pitch Bend Message (was "Tuning"). bi-polar. 0 = -10 octaves, 1.0 = +10 octaves. 
	{("Voice/Bender")		, HC_VOICE_PITCH_BEND, gmpi::PinDatatype::Float32, (ControllerType::VoiceNoteExpression << 24) | (ControllerType::kTuningTypeID << 16)},

	// MIDI 2.0 Per-Note Vibrato Depth. per-note CC 8
	{("Voice/Vibrato")		, HC_VOICE_VIBRATO, gmpi::PinDatatype::Float32, (ControllerType::VoiceNoteExpression << 24) | (ControllerType::kVibratoTypeID << 16)},

	// MIDI 2.0 Per-Note Expression. per-note CC 11
	{("Voice/Expression")	, HC_VOICE_EXPRESSION, gmpi::PinDatatype::Float32, (ControllerType::VoiceNoteExpression << 24) | (ControllerType::kExpressionTypeID << 16)},

	// MIDI 2.0 Per-Note Expression 5
	{("Voice/Brightness")	, HC_VOICE_BRIGHTNESS, gmpi::PinDatatype::Float32, (ControllerType::VoiceNoteExpression << 24) | (ControllerType::kBrightnessTypeID << 16)},

	{("Voice/UserControl0")	, HC_VOICE_USER_CONTROL0, gmpi::PinDatatype::Float32, (ControllerType::VoiceNoteExpression << 24) | (ControllerType::kCustomStart << 16)},
	{("Voice/UserControl1")	, HC_VOICE_USER_CONTROL1, gmpi::PinDatatype::Float32, (ControllerType::VoiceNoteExpression << 24) | ((ControllerType::kCustomStart + 1) << 16)},
	{("Voice/UserControl2")	, HC_VOICE_USER_CONTROL2, gmpi::PinDatatype::Float32, (ControllerType::VoiceNoteExpression << 24) | ((ControllerType::kCustomStart + 2) << 16)},
	{("Voice/PortamentoEnable")	, HC_VOICE_PORTAMENTO_ENABLE, gmpi::PinDatatype::Float32, (ControllerType::VoiceNoteControl << 24) | ((ControllerType::kPortamentoEnable) << 16)},


	{("SnapModulation")		, HC_SNAP_MODULATION__DEPRECATED, gmpi::PinDatatype::Int32, ControllerType::None},
	{( "Portamento" )		, HC_PORTAMENTO, gmpi::PinDatatype::Float32, ( ControllerType::CC << 24 ) | 5}, // CC 5 = Portamento Time 
	{( "Voice/GlideStartPitch" ), HC_GLIDE_START_PITCH, gmpi::PinDatatype::Float32, ControllerType::GlideStartPitch << 24},
	{( "BenderRange" )		, HC_BENDER_RANGE, gmpi::PinDatatype::Float32, ( ControllerType::RPN << 24 ) | 0}, // RPN 0000 = Pitch bend range (course = semitones, fine = cents).

	{"SubPatchCommands"		, HC_SUB_PATCH_COMMANDS, gmpi::PinDatatype::Int32, ControllerType::None},
	{"Processor/OfflineRenderMode", HC_PROCESS_RENDERMODE, gmpi::PinDatatype::Enum, ControllerType::None}, // 0 = :Live", 2 = "Preview" (Offline)

	{"User/Int1"				, HC_USER_SHARED_PARAMETER_INT1	, gmpi::PinDatatype::Int32, ControllerType::None},
	{"User/Int2"				, HC_USER_SHARED_PARAMETER_INT2	, gmpi::PinDatatype::Int32, ControllerType::None},
	{"User/Int3"				, HC_USER_SHARED_PARAMETER_INT3	, gmpi::PinDatatype::Int32, ControllerType::None},
	{"User/Int4"				, HC_USER_SHARED_PARAMETER_INT4	, gmpi::PinDatatype::Int32, ControllerType::None},
	{"PatchCables"				, HC_PATCH_CABLES				, gmpi::PinDatatype::Blob, ControllerType::None},

	{"Processor/SilenceOptimisation", HC_SILENCE_OPTIMISATION	, gmpi::PinDatatype::Bool, ControllerType::None},

	{"ProgramCategory"			,HC_PROGRAM_CATEGORY			, gmpi::PinDatatype::String, ControllerType::None},
	{"ProgramCategoriesList"	,HC_PROGRAM_CATEGORIES_LIST		, gmpi::PinDatatype::String, ControllerType::None},

	{"MpeMode"					,HC_MPE_MODE					, gmpi::PinDatatype::Int32, ControllerType::None},
	{"Presets/ProgramModified"	,HC_PROGRAM_MODIFIED			, gmpi::PinDatatype::Bool, ControllerType::None},
	{"Presets/CanUndo"			,HC_CAN_UNDO					, gmpi::PinDatatype::Bool, ControllerType::None},
	{"Presets/CanRedo"			,HC_CAN_REDO					, gmpi::PinDatatype::Bool, ControllerType::None },

	// MAINTAIN ORDER TO PRESERVE OLDER WAVES EXPORTS DSP.XML consistency
};

std::unordered_map<std::string_view, HostControls> hostControlNames =
{
	{"PatchCommands", HC_PATCH_COMMANDS},
	{"MidiChannelIn", HC_MIDI_CHANNEL},
	{"ProgramNamesList", HC_PROGRAM_NAMES_LIST},
	{"Program", HC_PROGRAM},
	{"ProgramName", HC_PROGRAM_NAME},

	{"Voice/Trigger", HC_VOICE_TRIGGER},
	{"Voice/Gate", HC_VOICE_GATE},
	{"Voice/Pitch", HC_VOICE_PITCH},
	{"Voice/VelocityKeyOn", HC_VOICE_VELOCITY_KEY_ON},
	{"Voice/VelocityKeyOff", HC_VOICE_VELOCITY_KEY_OFF},
	{"Voice/Aftertouch", HC_VOICE_AFTERTOUCH},
	{"Voice/VirtualVoiceId", HC_VOICE_VIRTUAL_VOICE_ID},
	{"Voice/Active", HC_VOICE_ACTIVE},
	{"XXXXXXXXXX", HC_UNUSED}, // was "Voice/Reset"

	{"VoiceAllocationMode", HC_VOICE_ALLOCATION_MODE},

	{"Bender", HC_PITCH_BENDER},
	{"HoldPeda", HC_HOLD_PEDAL}, // CC 64
	{"Channel Pressure", HC_CHANNEL_PRESSURE},

	{"Time/BPM", HC_TIME_BPM},
	{"Time/SongPosition", HC_TIME_QUARTER_NOTE_POSITION},
	{"Time/TransportPlaying", HC_TIME_TRANSPORT_PLAYING},
	{"Polyphony", HC_POLYPHONY},
	{"ReserveVoices", HC_POLYPHONY_VOICE_RESERVE},
	{"Oversampling/Rate", HC_OVERSAMPLING_RATE},
	{"Oversampling/Filter", HC_OVERSAMPLING_FILTER},
	{"User/Int0", HC_USER_SHARED_PARAMETER_INT0},
	{"Time/BarStartPosition", HC_TIME_BAR_START},
	{"Time/Timesignature/Numerator", HC_TIME_NUMERATOR},
	{"Time/Timesignature/Denominator", HC_TIME_DENOMINATOR},

	{"Voice/Volume", HC_VOICE_VOLUME},
	{"Voice/Pan", HC_VOICE_PAN},
	{"Voice/Bender", HC_VOICE_PITCH_BEND},
	{"Voice/Vibrato", HC_VOICE_VIBRATO},
	{"Voice/Expression", HC_VOICE_EXPRESSION},
	{"Voice/Brightness", HC_VOICE_BRIGHTNESS},
	{"Voice/UserControl0", HC_VOICE_USER_CONTROL0},
	{"Voice/UserControl1", HC_VOICE_USER_CONTROL1},
	{"Voice/UserControl2", HC_VOICE_USER_CONTROL2},
	{"Voice/PortamentoEnable", HC_VOICE_PORTAMENTO_ENABLE},

	{"SnapModulation", HC_SNAP_MODULATION__DEPRECATED},
	{"Portamento", HC_PORTAMENTO},
	{"Voice/GlideStartPitch", HC_GLIDE_START_PITCH},
	{"BenderRange", HC_BENDER_RANGE},

	{"SubPatchCommands", HC_SUB_PATCH_COMMANDS},
	{"Processor/OfflineRenderMode", HC_PROCESS_RENDERMODE},

	{"User/Int1", HC_USER_SHARED_PARAMETER_INT1},
	{"User/Int2", HC_USER_SHARED_PARAMETER_INT2},
	{"User/Int3", HC_USER_SHARED_PARAMETER_INT3},
	{"User/Int4", HC_USER_SHARED_PARAMETER_INT4},
	{"PatchCables", HC_PATCH_CABLES},

	{"Processor/SilenceOptimisation", HC_SILENCE_OPTIMISATION},

	{"ProgramCategory", HC_PROGRAM_CATEGORY},
	{"ProgramCategoriesList", HC_PROGRAM_CATEGORIES_LIST},

	{"MpeMode", HC_MPE_MODE},
	{"Presets/ProgramModified", HC_PROGRAM_MODIFIED},
	{"Presets/CanUndo", HC_CAN_UNDO},
	{"Presets/CanRedo", HC_CAN_REDO},
};

struct hcInfo
{
	const char* name;
	gmpi::PinDatatype datatype;
	int automation;
};

static hcInfo hostControlsInfo[] = {
	{"PatchCommands", gmpi::PinDatatype::Int32, ControllerType::None},

	{"MidiChannelIn", gmpi::PinDatatype::Int32, ControllerType::None},

	{"ProgramNamesList", gmpi::PinDatatype::String, ControllerType::None},

	{"Program", gmpi::PinDatatype::Int32, ControllerType::None},

	{"ProgramName", gmpi::PinDatatype::String, ControllerType::None},

	{"Voice/Trigger", gmpi::PinDatatype::Float32, ControllerType::Trigger << 24},

	{"Voice/Gate", gmpi::PinDatatype::Float32, ControllerType::Gate << 24},

	{"Voice/Pitch", gmpi::PinDatatype::Float32, ControllerType::Pitch << 24},

	{"Voice/VelocityKeyOn", gmpi::PinDatatype::Float32, ControllerType::VelocityOn << 24},

	{"Voice/VelocityKeyOff", gmpi::PinDatatype::Float32, ControllerType::VelocityOff << 24},

	{"Voice/Aftertouch", gmpi::PinDatatype::Float32, ControllerType::PolyAftertouch << 24},

	{"Voice/VirtualVoiceId", gmpi::PinDatatype::Int32, ControllerType::VirtualVoiceId << 24},

	{"Voice/Active", gmpi::PinDatatype::Float32, ControllerType::Active << 24},

	{"XXXXXXXXXX", gmpi::PinDatatype::Float32, ControllerType::None}, // was "Voice/Reset"

	{"VoiceAllocationMode", gmpi::PinDatatype::Int32, ControllerType::None},

	{"Bender", gmpi::PinDatatype::Float32, ControllerType::Bender << 24},

	{"HoldPeda", gmpi::PinDatatype::Float32, (ControllerType::CC << 24) | 64}, // CC 64

	{"Channel Pressure", gmpi::PinDatatype::Float32, ControllerType::ChannelPressure << 24},

	{"Time/BPM", gmpi::PinDatatype::Float32, ControllerType::BPM << 24},

	{"Time/SongPosition", gmpi::PinDatatype::Float32, ControllerType::SongPosition << 24},

	{"Time/TransportPlaying", gmpi::PinDatatype::Bool, ControllerType::TransportPlaying << 24},

	{"Polyphony", gmpi::PinDatatype::Int32, ControllerType::None},

	{"ReserveVoices", gmpi::PinDatatype::Int32, ControllerType::None},

	{"Oversampling/Rate", gmpi::PinDatatype::Enum, ControllerType::None},

	{"Oversampling/Filter", gmpi::PinDatatype::Enum, ControllerType::None},

	{"User/Int0", gmpi::PinDatatype::Int32, ControllerType::None},

	{"Time/BarStartPosition", gmpi::PinDatatype::Float32, ControllerType::barStartPosition << 24},

	{"Time/Timesignature/Numerator", gmpi::PinDatatype::Int32, ControllerType::timeSignatureNumerator << 24},

	{"Time/Timesignature/Denominator", gmpi::PinDatatype::Int32, ControllerType::timeSignatureDenominator << 24},

	// VST3 note-expression / MIDI 2.0
	{"Voice/Volume", gmpi::PinDatatype::Float32, (ControllerType::VoiceNoteExpression << 24) | (ControllerType::kVolumeTypeID << 16)},

	{"Voice/Pan", gmpi::PinDatatype::Float32, (ControllerType::VoiceNoteExpression << 24) | (ControllerType::kPanTypeID << 16)},

	{"Voice/Bender", gmpi::PinDatatype::Float32, (ControllerType::VoiceNoteExpression << 24) | (ControllerType::kTuningTypeID << 16)},

	{"Voice/Vibrato", gmpi::PinDatatype::Float32, (ControllerType::VoiceNoteExpression << 24) | (ControllerType::kVibratoTypeID << 16)},

	{"Voice/Expression", gmpi::PinDatatype::Float32, (ControllerType::VoiceNoteExpression << 24) | (ControllerType::kExpressionTypeID << 16)},

	{"Voice/Brightness", gmpi::PinDatatype::Float32, (ControllerType::VoiceNoteExpression << 24) | (ControllerType::kBrightnessTypeID << 16)},

	{"Voice/UserControl0", gmpi::PinDatatype::Float32, (ControllerType::VoiceNoteExpression << 24) | (ControllerType::kCustomStart << 16)},

	{"Voice/UserControl1", gmpi::PinDatatype::Float32, (ControllerType::VoiceNoteExpression << 24) | ((ControllerType::kCustomStart + 1) << 16)},

	{"Voice/UserControl2", gmpi::PinDatatype::Float32, (ControllerType::VoiceNoteExpression << 24) | ((ControllerType::kCustomStart + 2) << 16)},

	{"Voice/PortamentoEnable", gmpi::PinDatatype::Float32, (ControllerType::VoiceNoteControl << 24) | (ControllerType::kPortamentoEnable << 16)},

	{"SnapModulation", gmpi::PinDatatype::Int32, ControllerType::None},

	{"Portamento", gmpi::PinDatatype::Float32, (ControllerType::CC << 24) | 5},

	{"Voice/GlideStartPitch", gmpi::PinDatatype::Float32, ControllerType::GlideStartPitch << 24},

	{"BenderRange", gmpi::PinDatatype::Float32, (ControllerType::RPN << 24) | 0},

	{"SubPatchCommands", gmpi::PinDatatype::Int32, ControllerType::None},

	{"Processor/OfflineRenderMode", gmpi::PinDatatype::Enum, ControllerType::None},

	{"User/Int1", gmpi::PinDatatype::Int32, ControllerType::None},
	{"User/Int2", gmpi::PinDatatype::Int32, ControllerType::None},
	{"User/Int3", gmpi::PinDatatype::Int32, ControllerType::None},
	{"User/Int4", gmpi::PinDatatype::Int32, ControllerType::None},

	{"PatchCables", gmpi::PinDatatype::Blob, ControllerType::None},

	{"Processor/SilenceOptimisation", gmpi::PinDatatype::Bool, ControllerType::None},

	{"ProgramCategory", gmpi::PinDatatype::String, ControllerType::None},
	{"ProgramCategoriesList", gmpi::PinDatatype::String, ControllerType::None},

	{"MpeMode", gmpi::PinDatatype::Int32, ControllerType::None},
	{"Presets/ProgramModified", gmpi::PinDatatype::Bool, ControllerType::None},
	{"Presets/CanUndo", gmpi::PinDatatype::Bool, ControllerType::None},
	{"Presets/CanRedo", gmpi::PinDatatype::Bool, ControllerType::None},
};

HostControls StringToHostControl(std::string_view txt )
{
	if(auto it = hostControlNames.find(txt); it != hostControlNames.end())
	{
		return it->second;
	}
	return HC_NONE;
}

gmpi::PinDatatype GetHostControlDatatype( HostControls hostControlId )
{
	if (hostControlId < 0 || hostControlId >= HC_NUM_HOST_CONTROLS)
	{
		assert(false);
		return gmpi::PinDatatype::Float32;
	}

	// just check sanity of the two sets of data.
	assert(hostControlId == StringToHostControl(hostControlsInfo[hostControlId].name));

	return hostControlsInfo[hostControlId].datatype;
}

const char* GetHostControlName( HostControls hostControlId )
{
	if (hostControlId < 0 || hostControlId >= HC_NUM_HOST_CONTROLS)
	{
		assert(false);
		return "";
	}

	return hostControlsInfo[hostControlId].name;
}

#if 0
const wchar_t* GetHostControlNameByAutomation(int automation)
{
	const int DATAYPE_INFO_COUNT = (sizeof(lookup) / sizeof(HostControlStruct));

	for (int j = 0; j < DATAYPE_INFO_COUNT; j++)
	{
		if (lookup[j].automation == automation)
		{
			return lookup[j].display;
			break;
		}
	}

	assert(false);
	return L"";
}

int GetHostControlAutomation( HostControls hostControlId )
{
	const int DATAYPE_INFO_COUNT = (sizeof(lookup) / sizeof(HostControlStruct));
	if(hostControlId < DATAYPE_INFO_COUNT)
	{
		return lookup[hostControlId].automation;
	}

	assert(false);
	return ControllerType::None;
}
#endif

// Most host controls 'belong' to the Patch Automator, however a handfull apply to the local parent container.
bool HostControlAttachesToParentContainer( HostControls hostControlId )
{
	switch( hostControlId )
	{
		case HC_VOICE_TRIGGER:
		case HC_VOICE_GATE:
		case HC_VOICE_PITCH:
		case HC_VOICE_VELOCITY_KEY_ON:
		case HC_VOICE_VELOCITY_KEY_OFF:
		case HC_VOICE_AFTERTOUCH:
		case HC_VOICE_VIRTUAL_VOICE_ID:
		case HC_VOICE_ACTIVE:
		case HC_VOICE_ALLOCATION_MODE:
		case HC_PITCH_BENDER:
		case HC_HOLD_PEDAL:
		case HC_CHANNEL_PRESSURE :
        case HC_POLYPHONY:
        case HC_POLYPHONY_VOICE_RESERVE:
        case HC_OVERSAMPLING_RATE:
        case HC_OVERSAMPLING_FILTER:
		case HC_VOICE_VOLUME:
		case HC_VOICE_PAN:
		case HC_VOICE_TUNING:
		case HC_VOICE_PITCH_BEND:
		case HC_VOICE_VIBRATO:
		case HC_VOICE_EXPRESSION:
		case HC_VOICE_BRIGHTNESS:
		case HC_VOICE_USER_CONTROL0:
		case HC_VOICE_USER_CONTROL1:
		case HC_VOICE_USER_CONTROL2:
		case HC_VOICE_PORTAMENTO_ENABLE:
		case HC_SNAP_MODULATION__DEPRECATED:
		case HC_PORTAMENTO:
		case HC_GLIDE_START_PITCH:
		case HC_BENDER_RANGE:
			return true;
		break;

		default:
            return false;
		break;
	}

	return false;
}

bool HostControlisPolyphonic(HostControls hostControlId)
{
	switch (hostControlId)
	{
	case HC_VOICE_TRIGGER:
	case HC_VOICE_GATE:
	case HC_VOICE_PITCH:
	case HC_VOICE_VELOCITY_KEY_ON:
	case HC_VOICE_VELOCITY_KEY_OFF:
	case HC_VOICE_AFTERTOUCH:
	case HC_VOICE_VIRTUAL_VOICE_ID:
	case HC_VOICE_ACTIVE:
	case HC_VOICE_PORTAMENTO_ENABLE:
	case HC_VOICE_VOLUME:
	case HC_VOICE_PAN:
//nope	case HC_VOICE_TUNING:
	case HC_VOICE_PITCH_BEND:
	case HC_VOICE_VIBRATO:
	case HC_VOICE_EXPRESSION:
	case HC_VOICE_BRIGHTNESS:
	case HC_VOICE_USER_CONTROL0:
	case HC_VOICE_USER_CONTROL1:
	case HC_VOICE_USER_CONTROL2:
	case HC_GLIDE_START_PITCH:

		return true;
		break;

	default:
		return false;
		break;
	}

	return false;
}

bool AffectsVoiceAllocation(HostControls hostControlId)
{
	switch( hostControlId )
	{
	case HC_VOICE_ALLOCATION_MODE:
	case HC_PORTAMENTO:
		return true;
		break;

	default:
		return false;
		break;
	}

	return false;
}
}