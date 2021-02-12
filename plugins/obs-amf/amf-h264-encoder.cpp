#include "AMF/include/core/Factory.h"
#include "AMF/include/components/VideoEncoderVCE.h"
#include "AMF/include/core/Context.h"
#include <util/circlebuf.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <obs-avc.h>
#define INITGUID
#include <dxgi.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include "amf-encoder.hpp"
#include "common.hpp"
#include "obs-amf.hpp"
#include <locale>
#include <codecvt>

using namespace amf;

static const char *amf_h264_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "AMD VCE H.264 (new)";
}

static bool amf_h264_update(void *data, obs_data_t *settings)
{
	AMF_RESULT result = AMF_FAIL;
	struct amf_data *enc = (amf_data *)data;
	AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_ENUM rcm =
		static_cast<AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_ENUM>(
			obs_data_get_int(settings, P_RATECONTROLMETHOD));

	SET_AMF_VALUE_OR_FAIL(enc->encoder_amf,
			      AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD, rcm);

	SET_AMF_VALUE(enc->encoder_amf, AMF_VIDEO_ENCODER_ENABLE_VBAQ,
		      obs_data_get_bool(settings, P_VBAQ));

	// Rate Control Properties
	int64_t bitrate = obs_data_get_int(settings, P_BITRATE) * 1000;
	if (rcm != AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CONSTANT_QP) {
		SET_AMF_VALUE_OR_FAIL(enc->encoder_amf,
				      AMF_VIDEO_ENCODER_TARGET_BITRATE,
				      bitrate);

		SET_AMF_VALUE_OR_FAIL(
			enc->encoder_amf, AMF_VIDEO_ENCODER_PEAK_BITRATE,
			static_cast<amf_int64>(
				obs_data_get_int(settings, P_BITRATE_PEAK) *
				bitrate / 100));
	}
	SET_AMF_VALUE_OR_FAIL(enc->encoder_amf, AMF_VIDEO_ENCODER_ENFORCE_HRD,
			      obs_data_get_bool(settings, P_ENFORCEHRD));

	SET_AMF_VALUE(enc->encoder_amf,
		      AMF_VIDEO_ENCODER_HIGH_MOTION_QUALITY_BOOST_ENABLE,
		      obs_data_get_bool(settings, P_HIGHMOTIONQUALITYBOOST));

	// VBV Buffer
	SET_AMF_VALUE_OR_FAIL(
		enc->encoder_amf, AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE,
		static_cast<amf_int64>(
			bitrate *
			obs_data_get_double(settings, P_VBVBUFFER_SIZE)));

	// Picture Control
	double_t keyinterv = obs_data_get_double(settings, P_INTERVAL_KEYFRAME);
	int idrperiod =
		static_cast<uint32_t>(ceil(keyinterv * enc->frame_rate.num));
	SET_AMF_VALUE_OR_FAIL(enc->encoder_amf, AMF_VIDEO_ENCODER_IDR_PERIOD,
			      (int64_t)amf_clamp(idrperiod, 0, 1000000));
	SET_AMF_VALUE(enc->encoder_amf, AMF_VIDEO_ENCODER_DE_BLOCKING_FILTER,
		      obs_data_get_bool(settings, P_DEBLOCKINGFILTER));
	AMF::Instance()->GetTrace()->SetGlobalLevel(
		obs_data_get_double(settings, P_LOG_LEVEL));
	return true;
fail:
	return false;
}

static void *amf_h264_create(obs_data_t *settings, obs_encoder_t *encoder)
{
	AMF_RESULT result = AMF_FAIL;
	AMFVariant p;
	struct amf_data *enc = (amf_data *)bzalloc(sizeof(*enc));
	enc->encoder = encoder;

	result = init_d3d11(settings, enc);
	if (result != AMF_OK)
		goto fail;
	enc->codec = amf_data::CODEC_ID_H264;
	result = amf_create_encoder(settings, enc);
	if (result != AMF_OK) {
		PLOG_ERROR("AMF: Failed to create encoder");
		goto fail;
	}

	SET_AMF_VALUE_OR_FAIL(enc->encoder_amf, AMF_VIDEO_ENCODER_FRAMESIZE,
			      AMFConstructSize(enc->frameW, enc->frameH));

	SET_AMF_VALUE_OR_FAIL(enc->encoder_amf, AMF_VIDEO_ENCODER_USAGE,
			      AMF_VIDEO_ENCODER_USAGE_TRANSCONDING);

	SET_AMF_VALUE_OR_FAIL(enc->encoder_amf,
			      AMF_VIDEO_ENCODER_QUALITY_PRESET,
			      obs_data_get_int(settings, P_QUALITYPRESET));

	SET_AMF_VALUE_OR_FAIL(enc->encoder_amf, AMF_VIDEO_ENCODER_PROFILE,
			      obs_data_get_int(settings, P_PROFILE));

	SET_AMF_VALUE(enc->encoder_amf, AMF_VIDEO_ENCODER_LOWLATENCY_MODE,
		      obs_data_get_bool(settings, P_LOW_LATENCY_MODE));

	SET_AMF_VALUE(enc->encoder_amf, AMF_VIDEO_ENCODER_PRE_ANALYSIS_ENABLE,
		      obs_data_get_bool(settings, P_PREPASSMODE));

	SET_AMF_VALUE_OR_FAIL(enc->encoder_amf, AMF_VIDEO_ENCODER_CABAC_ENABLE,
			      obs_data_get_int(settings, P_CODINGTYPE));

	result = enc->encoder_amf->Init(AMF_SURFACE_NV12, enc->frameW,
					enc->frameH);
	if (result != AMF_OK) {
		PLOG_WARNING("AMF: Failed to init encoder");
		goto fail;
	}
	SET_AMF_VALUE(enc->encoder_amf, AMF_VIDEO_ENCODER_FRAMERATE,
		      enc->frame_rate);

	AMF_RESULT res =
		enc->encoder_amf->GetProperty(AMF_VIDEO_ENCODER_EXTRADATA, &p);
	if (res == AMF_OK && p.type == amf::AMF_VARIANT_INTERFACE) {
		enc->header = AMFBufferPtr(p.pInterface);
	}

	if (AMF::Instance()->GetRuntimeVersion() <
	    AMF_MAKE_FULL_VERSION(1, 4, 0, 0)) {
		// Support for 1.3.x drivers.
		AMF_RESULT res =
			enc->encoder_amf->SetProperty(L"NominalRange", false);
		if (res != AMF_OK) {
			PLOG_WARNING(
				"Failed to set encoder color range, error code %d.",
				result);
		}
	} else {
		SET_AMF_VALUE(enc->encoder_amf,
			      AMF_VIDEO_ENCODER_FULL_RANGE_COLOR, false);
	}

	if (!amf_h264_update(enc, settings))
		goto fail;
	SET_AMF_VALUE(enc->encoder_amf, AMF_VIDEO_ENCODER_MIN_QP, 18);

	return enc;
fail:
	amf_destroy(enc);
	return NULL;
}

void amf_h264_defaults(obs_data_t *settings)
{
	// Preset
	obs_data_set_default_int(settings, P_PRESET,
				 static_cast<int64_t>(Presets::Recording));

	obs_data_set_default_int(settings, ("last" P_PRESET),
				 static_cast<int64_t>(Presets::Recording));
	// Static Properties
	obs_data_set_default_int(
		settings, P_QUALITYPRESET,
		static_cast<int64_t>(
			AMF_VIDEO_ENCODER_QUALITY_PRESET_BALANCED));
	obs_data_set_default_int(
		settings, P_PROFILE,
		static_cast<int64_t>(AMF_VIDEO_ENCODER_PROFILE_HIGH));
	obs_data_set_default_int(settings, P_CODINGTYPE,
				 static_cast<int64_t>(AMF_VIDEO_ENCODER_CABAC));
	obs_data_set_default_bool(settings, P_LOW_LATENCY_MODE, false);
	obs_data_set_default_int(
		settings, P_PREPASSMODE,
		static_cast<int64_t>(AMF_VIDEO_ENCODER_PREENCODE_ENABLED));

	// Rate Control Properties
	obs_data_set_default_int(
		settings, P_RATECONTROLMETHOD,
		static_cast<int64_t>(
			AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_QUALITY_VBR));
	obs_data_set_default_int(settings, P_BITRATE, 6000);
	obs_data_set_default_int(settings, P_BITRATE_PEAK, 150);
	obs_data_set_default_bool(settings, P_VBAQ, true);
	obs_data_set_default_bool(settings, P_ENFORCEHRD, true);
	obs_data_set_default_bool(settings, P_HIGHMOTIONQUALITYBOOST, false);

	// VBV Buffer
	obs_data_set_default_double(settings, P_VBVBUFFER_SIZE, 1);

	// Picture Control
	obs_data_set_default_double(settings, P_INTERVAL_KEYFRAME, 0);
	obs_data_set_default_bool(settings, P_DEBLOCKINGFILTER, true);

	// System Properties
	obs_data_set_default_int(settings, P_VIEW,
				 static_cast<int64_t>(ViewMode::Basic));
	obs_data_set_default_int(settings, P_LOG_LEVEL,
				 static_cast<int32_t>(AMF_TRACE_ERROR));
}

bool amf_h264_properties_modified(obs_properties_t *props, obs_property_t *,
				  obs_data_t *settings)
{
	ViewMode curView =
		static_cast<ViewMode>(obs_data_get_int(settings, P_VIEW));
	AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_ENUM rcm =
		static_cast<AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_ENUM>(
			obs_data_get_int(settings, P_RATECONTROLMETHOD));

	obs_property_set_visible(obs_properties_get(props, P_ENFORCEHRD),
				 curView > ViewMode::Basic);
	obs_property_set_visible(obs_properties_get(props, P_VBVBUFFER_SIZE),
				 curView > ViewMode::Basic);
	obs_property_set_visible(obs_properties_get(props, P_VBAQ),
				 curView > ViewMode::Basic);
	obs_property_set_visible(obs_properties_get(props, P_LOW_LATENCY_MODE),
				 curView > ViewMode::Basic);
	obs_property_set_visible(obs_properties_get(props,
						    P_HIGHMOTIONQUALITYBOOST),
				 curView > ViewMode::Basic);
	obs_property_set_visible(obs_properties_get(props, P_DEBLOCKINGFILTER),
				 curView > ViewMode::Basic);
	obs_property_set_visible(obs_properties_get(props, P_PREPASSMODE),
				 curView > ViewMode::Basic);
	obs_property_set_visible(obs_properties_get(props, P_LOG_LEVEL),
				 curView > ViewMode::Basic);

	if (rcm == AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CONSTANT_QP) {

		obs_property_set_visible(
			obs_properties_get(props, P_BITRATE_PEAK), false);
		obs_property_set_visible(obs_properties_get(props, P_BITRATE),
					 false);
	} else {
		obs_property_set_visible(obs_properties_get(props,
							    P_BITRATE_PEAK),
					 curView > ViewMode::Basic);
		obs_property_set_visible(obs_properties_get(props, P_BITRATE),
					 true);
	}

	return true;
}

bool amf_h264_preset_modified(obs_properties_t *props, obs_property_t *,
			      obs_data_t *data)
{
	bool result = true;
	obs_property_t *p;
	Presets preset = static_cast<Presets>(obs_data_get_int(data, P_PRESET));
	Presets prevPreset =
		static_cast<Presets>(obs_data_get_int(data, ("last" P_PRESET)));
	if (preset == prevPreset)
		return true;
	obs_data_set_int(data, ("last" P_PRESET), static_cast<int64_t>(preset));

	switch (preset) {
	case Presets::Recording:
#pragma region Recording
		obs_data_set_int(
			data, P_RATECONTROLMETHOD,
			static_cast<int32_t>(
				AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_QUALITY_VBR));
		obs_data_set_int(data, P_BITRATE, 6000);
		obs_data_set_double(data, P_INTERVAL_KEYFRAME, 0);
		obs_data_set_int(
			data, P_QUALITYPRESET,
			static_cast<int32_t>(
				AMF_VIDEO_ENCODER_QUALITY_PRESET_QUALITY));
		obs_data_set_int(
			data, P_PROFILE,
			static_cast<int32_t>(AMF_VIDEO_ENCODER_PROFILE_HIGH));
		obs_data_set_int(data, P_BITRATE_PEAK, 150);
		obs_data_set_bool(data, P_ENFORCEHRD, false);
		obs_data_set_double(data, P_VBVBUFFER_SIZE, 1);
		obs_data_set_bool(data, P_VBAQ, true);
		obs_data_set_bool(data, P_LOW_LATENCY_MODE, false);
		obs_data_set_bool(data, P_HIGHMOTIONQUALITYBOOST, true);
		obs_data_set_bool(data, P_DEBLOCKINGFILTER, true);
		obs_data_set_bool(data, P_PREPASSMODE, true);
		break;
#pragma endregion Recording
	case Presets::Streaming:
#pragma region Streaming
		obs_data_set_int(
			data, P_RATECONTROLMETHOD,
			static_cast<int32_t>(
				AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR));
		obs_data_set_int(data, P_BITRATE, 6000);
		obs_data_set_double(data, P_INTERVAL_KEYFRAME, 2);
		obs_data_set_int(
			data, P_QUALITYPRESET,
			static_cast<int32_t>(
				AMF_VIDEO_ENCODER_QUALITY_PRESET_QUALITY));
		obs_data_set_int(
			data, P_PROFILE,
			static_cast<int32_t>(AMF_VIDEO_ENCODER_PROFILE_HIGH));
		obs_data_set_bool(data, P_ENFORCEHRD, false);
		obs_data_set_double(data, P_VBVBUFFER_SIZE, 1);
		obs_data_set_bool(data, P_VBAQ, true);
		obs_data_set_bool(data, P_LOW_LATENCY_MODE, false);
		obs_data_set_bool(data, P_HIGHMOTIONQUALITYBOOST, true);
		obs_data_set_bool(data, P_DEBLOCKINGFILTER, true);
		obs_data_set_bool(data, P_PREPASSMODE, true);
		break;
#pragma endregion Streaming
	case Presets::LowLatency:
#pragma region LowLatency
		obs_data_set_int(
			data, P_RATECONTROLMETHOD,
			static_cast<int32_t>(
				AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR));
		obs_data_set_int(data, P_BITRATE, 6000);
		obs_data_set_double(data, P_INTERVAL_KEYFRAME, 2);
		obs_data_set_int(
			data, P_QUALITYPRESET,
			static_cast<int32_t>(
				AMF_VIDEO_ENCODER_QUALITY_PRESET_QUALITY));
		obs_data_set_int(
			data, P_PROFILE,
			static_cast<int32_t>(AMF_VIDEO_ENCODER_PROFILE_HIGH));
		obs_data_set_bool(data, P_ENFORCEHRD, false);
		obs_data_set_double(data, P_VBVBUFFER_SIZE, 0.5);
		obs_data_set_bool(data, P_VBAQ, true);
		obs_data_set_bool(data, P_LOW_LATENCY_MODE, true);
		obs_data_set_bool(data, P_HIGHMOTIONQUALITYBOOST, false);
		obs_data_set_bool(data, P_DEBLOCKINGFILTER, true);
		obs_data_set_bool(data, P_PREPASSMODE, false);
		break;
#pragma endregion LowLatency
	}
	return result;
}

obs_properties_t *amf_h264_properties(void *unused)
{
	obs_properties *props = obs_properties_create();
	obs_property_t *p;
	int adapterIndex = 0;
	obs_video_info info;
	if (obs_get_video_info(&info)) {
		adapterIndex = info.adapter;
	}
#pragma region Preset
	p = obs_properties_add_list(props, P_PRESET, P_TRANSLATE(P_PRESET),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, P_TRANSLATE(P_PRESET_RECORDING),
				  static_cast<int32_t>(Presets::Recording));
	obs_property_list_add_int(p, P_TRANSLATE(P_PRESET_STREAMING),
				  static_cast<int32_t>(Presets::Streaming));
	obs_property_list_add_int(p, P_TRANSLATE(P_PRESET_LOWLATENCY),
				  static_cast<int32_t>(Presets::LowLatency));
	obs_property_set_modified_callback(p, amf_h264_preset_modified);
#pragma endregion Preset

	// Static Properties
#pragma region Quality Preset
	p = obs_properties_add_list(props, P_QUALITYPRESET,
				    P_TRANSLATE(P_QUALITYPRESET),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_set_long_description(p,
					  P_TRANSLATE(P_DESC(P_QUALITYPRESET)));
	obs_property_list_add_int(
		p, P_TRANSLATE(P_QUALITYPRESET_SPEED),
		static_cast<int32_t>(AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED));
	obs_property_list_add_int(
		p, P_TRANSLATE(P_QUALITYPRESET_BALANCED),
		static_cast<int32_t>(
			AMF_VIDEO_ENCODER_QUALITY_PRESET_BALANCED));
	obs_property_list_add_int(
		p, P_TRANSLATE(P_QUALITYPRESET_QUALITY),
		static_cast<int32_t>(AMF_VIDEO_ENCODER_QUALITY_PRESET_QUALITY));
#pragma endregion Quality Preset

#pragma region Profile
	p = obs_properties_add_list(props, P_PROFILE, P_TRANSLATE(P_PROFILE),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_set_long_description(p, P_TRANSLATE(P_DESC(P_PROFILE)));
	obs_property_list_add_int(
		p, "Constrained Baseline",
		static_cast<int32_t>(
			AMF_VIDEO_ENCODER_PROFILE_CONSTRAINED_BASELINE));
	obs_property_list_add_int(
		p, "Baseline",
		static_cast<int32_t>(AMF_VIDEO_ENCODER_PROFILE_BASELINE));
	obs_property_list_add_int(
		p, "Main",
		static_cast<int32_t>(AMF_VIDEO_ENCODER_PROFILE_MAIN));
	obs_property_list_add_int(
		p, "Constrained High",
		static_cast<int32_t>(
			AMF_VIDEO_ENCODER_PROFILE_CONSTRAINED_HIGH));
	obs_property_list_add_int(
		p, "High",
		static_cast<int32_t>(AMF_VIDEO_ENCODER_PROFILE_HIGH));
#pragma endregion Profile, Levels

#pragma region Coding Type
	p = obs_properties_add_list(props, P_CODINGTYPE,
				    P_TRANSLATE(P_CODINGTYPE),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_set_long_description(p, P_TRANSLATE(P_DESC(P_CODINGTYPE)));
	obs_property_list_add_int(
		p, P_TRANSLATE(P_UTIL_AUTOMATIC),
		static_cast<int32_t>(AMF_VIDEO_ENCODER_UNDEFINED));
	obs_property_list_add_int(
		p, "CABAC", static_cast<int32_t>(AMF_VIDEO_ENCODER_CABAC));
	obs_property_list_add_int(p, "CALVC",
				  static_cast<int32_t>(AMF_VIDEO_ENCODER_CALV));
#pragma endregion Coding Type
	// Rate Control
#pragma region Rate Control Method
	p = obs_properties_add_list(props, P_RATECONTROLMETHOD,
				    P_TRANSLATE(P_RATECONTROLMETHOD),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_set_long_description(
		p, P_TRANSLATE(P_DESC(P_RATECONTROLMETHOD)));

	EncoderCaps caps = AMF::Instance()->GetH264Caps(adapterIndex);
	using convert_type = std::codecvt_utf8<wchar_t>;
	std::wstring_convert<convert_type, wchar_t> converter;
	for (const auto &rcMethod : caps.rate_control_methods) {
		std::string name = converter.to_bytes(rcMethod.name);
		obs_property_list_add_int(p, P_TRANSLATE(name.c_str()),
					  rcMethod.value);
	}
	obs_property_set_modified_callback(p, amf_h264_properties_modified);
#pragma endregion Rate Control Method

#pragma region Parameters
	///Prepass mode
	p = obs_properties_add_bool(props, P_PREPASSMODE,
				    P_TRANSLATE(P_PREPASSMODE));
	obs_property_set_long_description(p,
					  P_TRANSLATE(P_DESC(P_PREPASSMODE)));
	/// Bitrate Constraints
	p = obs_properties_add_int(props, P_BITRATE,
				   P_TRANSLATE(P_BITRATE_TARGET), 10, 100000,
				   10);
	obs_property_set_long_description(
		p, P_TRANSLATE(P_DESC(P_BITRATE_TARGET)));
	p = obs_properties_add_int(props, P_BITRATE_PEAK,
				   P_TRANSLATE(P_BITRATE_PEAK), 100, 1000, 100);
	obs_property_set_long_description(p,
					  P_TRANSLATE(P_DESC(P_BITRATE_PEAK)));
#pragma endregion Parameters

#pragma region VBAQ
	p = obs_properties_add_bool(props, P_VBAQ, P_TRANSLATE(P_VBAQ));
	obs_property_set_long_description(p, P_TRANSLATE(P_DESC(P_VBAQ)));
#pragma endregion VBAQ

#pragma region Enforce Hypothetical Reference Decoder Restrictions
	p = obs_properties_add_bool(props, P_ENFORCEHRD,
				    P_TRANSLATE(P_ENFORCEHRD));
	obs_property_set_long_description(p, P_TRANSLATE(P_DESC(P_ENFORCEHRD)));
#pragma endregion Enforce Hypothetical Reference Decoder Restrictions

#pragma region High Motion Quality Boost
	p = obs_properties_add_bool(props, P_HIGHMOTIONQUALITYBOOST,
				    P_TRANSLATE(P_HIGHMOTIONQUALITYBOOST));
	obs_property_set_long_description(
		p, P_TRANSLATE(P_DESC(P_HIGHMOTIONQUALITYBOOST)));
#pragma endregion High Motion Quality Boost

#pragma region VBV Buffer Size
	p = obs_properties_add_float(props, P_VBVBUFFER_SIZE,
				     P_TRANSLATE(P_VBVBUFFER_SIZE), 0.1f, 1,
				     0.1f);
	obs_property_set_long_description(
		p, P_TRANSLATE(P_DESC(P_VBVBUFFER_SIZE)));
#pragma endregion VBV Buffer Size
	// Picture Control
#pragma region Interval and Periods
	/// Keyframe, IDR
	p = obs_properties_add_float(props, P_INTERVAL_KEYFRAME,
				     P_TRANSLATE(P_INTERVAL_KEYFRAME), 0, 100,
				     0.001);
	obs_property_set_long_description(
		p, P_TRANSLATE(P_DESC(P_INTERVAL_KEYFRAME)));
#pragma endregion Interval and Periods

#pragma region Deblocking Filter
	p = obs_properties_add_bool(props, P_DEBLOCKINGFILTER,
				    P_TRANSLATE(P_DEBLOCKINGFILTER));
	obs_property_set_long_description(
		p, P_TRANSLATE(P_DESC(P_DEBLOCKINGFILTER)));
#pragma endregion Deblocking Filter

#pragma region Low latency
	p = obs_properties_add_bool(props, P_LOW_LATENCY_MODE,
				    P_TRANSLATE(P_LOW_LATENCY_MODE));
	obs_property_set_long_description(
		p, P_TRANSLATE(P_DESC(P_LOW_LATENCY_MODE)));
#pragma endregion Low latency

#pragma region View Mode
	p = obs_properties_add_list(props, P_VIEW, P_TRANSLATE(P_VIEW),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_set_long_description(p, P_TRANSLATE(P_DESC(P_VIEW)));
	obs_property_list_add_int(p, P_TRANSLATE(P_VIEW_BASIC),
				  static_cast<int32_t>(ViewMode::Basic));
	obs_property_list_add_int(p, P_TRANSLATE(P_VIEW_ADVANCED),
				  static_cast<int32_t>(ViewMode::Advanced));
	obs_property_set_modified_callback(p, amf_h264_properties_modified);
#pragma endregion View Mode
#pragma region Log Level
	p = obs_properties_add_list(props, P_LOG_LEVEL,
				    P_TRANSLATE(P_LOG_LEVEL),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_set_long_description(p, P_TRANSLATE(P_DESC(P_LOG_LEVEL)));

	obs_property_list_add_int(p, P_TRANSLATE(P_LOG_LEVEL_ERROR),
				  static_cast<int32_t>(AMF_TRACE_ERROR));
	obs_property_list_add_int(p, P_TRANSLATE(P_LOG_LEVEL_WARNING),
				  static_cast<int32_t>(AMF_TRACE_WARNING));
	obs_property_list_add_int(p, P_TRANSLATE(P_LOG_LEVEL_INFO),
				  static_cast<int32_t>(AMF_TRACE_INFO));
	obs_property_list_add_int(p, P_TRANSLATE(P_LOG_LEVEL_DEBUG),
				  static_cast<int32_t>(AMF_TRACE_DEBUG));
	obs_property_list_add_int(p, P_TRANSLATE(P_LOG_LEVEL_TRACE),
				  static_cast<int32_t>(AMF_TRACE_TRACE));
#pragma endregion Log Level
	return props;
}

struct obs_encoder_info obs_amf_h264_encoder;

void register_amf_h264_encoder()
{
	if (!AMF::Instance()->H264Available())
		return;

	obs_amf_h264_encoder.id = "amf_h264";
	obs_amf_h264_encoder.codec = "h264";
	obs_amf_h264_encoder.type = OBS_ENCODER_VIDEO;
	obs_amf_h264_encoder.caps = OBS_ENCODER_CAP_PASS_TEXTURE |
				    OBS_ENCODER_CAP_DYN_BITRATE;
	obs_amf_h264_encoder.get_name = amf_h264_get_name;
	obs_amf_h264_encoder.create = amf_h264_create;
	obs_amf_h264_encoder.destroy = amf_destroy;
	obs_amf_h264_encoder.update = amf_h264_update;
	obs_amf_h264_encoder.encode_texture = amf_encode_tex;
	obs_amf_h264_encoder.get_defaults = amf_h264_defaults;
	obs_amf_h264_encoder.get_properties = amf_h264_properties;
	obs_amf_h264_encoder.get_extra_data = amf_extra_data;

	obs_register_encoder(&obs_amf_h264_encoder);
}