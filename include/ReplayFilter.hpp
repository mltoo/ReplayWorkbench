#pragma once

#include <obs.h>
#include <obs-module.h>
#include <util/circlebuf.h>
#include <util/c99defs.h>

#include "ClipEncoder.hpp"

namespace ReplayWorkbench {

struct obs_source_info initFilterInfo();

const char *getFilterName(void *data);
obs_properties_t *getFilterProperties(void *data);
void getFilterDefaults(obs_data_t *defaults);

void *createFilter(obs_data_t *settings, obs_source_t *source);
void updateFilter(void *data, obs_data_t *settings);
void destroyFilter(void *data);
void loadFilter(void *data, obs_data_t *settings);

void filterVideoTick(void *data, float seconds);
void filterVideoRender(void *data, gs_effect_t *effect);

struct obs_audio_data *filterAudio(void *data,
				   struct obs_audio_data *audioData);

struct obs_source_frame *filterVideo(void *data,
				     struct obs_source_frame *frame);

class ReplayFilterInstance {
public:
	ReplayFilterInstance(obs_data_t *settings, obs_source_t *source,
			     ClipEncoder *encoder);
	~ReplayFilterInstance();

	obs_properties_t *getProperties();
	obs_source_t *getSource();
	
	void setEncoder(ClipEncoder *newEncoder);
	
	struct obs_source_frame *handleVideo(struct obs_source_frame *frame);
private:
	obs_properties_t *props;
	obs_source_t *source;

	struct circlebuf *videoEncodeQueue;
	struct circlebuf *audioEncodeQueue;

	ClipEncoder *encoder;
};

}
