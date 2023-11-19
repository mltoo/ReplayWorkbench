#include "ReplayFilter.hpp"
#include "RawEncoder.hpp"
#include <obs-module.h>
#include <obs-properties.h>
#include <obs.h>
#include <stdexcept>
#include <util/c99defs.h>

namespace ReplayWorkbench {

const char *getFilterName(void *data)
{
	UNUSED_PARAMETER(data);
	return obs_module_text("ReplayWorkbench.FilterName");
}

obs_properties_t *getFilterProperties(void *data)
{
	ReplayFilterInstance *filter = (ReplayFilterInstance *)data;
	return filter->getProperties();
}

void getFilterDefaults(obs_data_t *defaults)
{
	obs_data_set_default_string(defaults, "test", "defaultTestVal");
}

void *createFilter(obs_data_t *settings, obs_source_t *source)
{
	return (void *)new ReplayFilterInstance(settings, source,
						new RawEncoder());
}

void updateFilter(void *data, obs_data_t *settings)
{
	UNUSED_PARAMETER(settings);
	auto filter = (ReplayFilterInstance *)data;
	filter->getSource();
}

void loadFilter(void *data, obs_data_t *settings)
{
	UNUSED_PARAMETER(settings);
	auto filter = (ReplayFilterInstance *)data;
	filter->getSource();
}

void destroyFilter(void *data)
{
	delete (ReplayFilterInstance *)data;
}

void filterVideoTick(void *data, float seconds)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(seconds);
}

void filterVideoRender(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	auto filter = (ReplayFilterInstance *)data;
	obs_source_skip_video_filter(filter->getSource());
}

struct obs_source_frame *filterVideo(void *data, struct obs_source_frame *frame) {
	auto filter = (ReplayFilterInstance *)data;
	return filter->handleVideo(frame);
}

struct obs_audio_data *filterAudio(void *data, struct obs_audio_data *frame) {
	UNUSED_PARAMETER(data);
	throw std::runtime_error("NOT IMPLEMENTED");
	return frame;
}

struct obs_source_info initFilterInfo()
{
	struct obs_source_info filterInfo = {};

	filterInfo.id = "replay_filter";
	filterInfo.type = OBS_SOURCE_TYPE_FILTER;
	filterInfo.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO;

	filterInfo.get_name = getFilterName;
	filterInfo.get_properties = getFilterProperties;
	filterInfo.get_defaults = getFilterDefaults;

	filterInfo.create = createFilter;
	filterInfo.update = updateFilter;
	filterInfo.destroy = destroyFilter;
	filterInfo.load = loadFilter;

	filterInfo.video_tick = filterVideoTick;
	filterInfo.video_render = filterVideoRender;

	filterInfo.filter_audio = filterAudio;
	filterInfo.filter_video = filterVideo;

	return filterInfo;
}

ReplayFilterInstance::ReplayFilterInstance(obs_data_t *settings,
					   obs_source_t *source,
					   ClipEncoder *encoder)
{
	UNUSED_PARAMETER(settings);
	this->props = obs_properties_create();
	this->source = source;
	this->encoder = encoder;
	obs_properties_set_flags(this->props, OBS_PROPERTIES_DEFER_UPDATE);

	obs_properties_add_text(this->props, "test", "test123",
				OBS_TEXT_DEFAULT);
}

ReplayFilterInstance::~ReplayFilterInstance()
{
	obs_properties_destroy(this->props);
}

obs_properties_t *ReplayFilterInstance::getProperties()
{
	return this->props;
}

obs_source_t *ReplayFilterInstance::getSource()
{
	return this->source;
}

struct obs_source_frame *ReplayFilterInstance::handleVideo(struct obs_source_frame *frame) {

	return frame;
}

}
