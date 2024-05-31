#pragma once
#include <string>
#include <vector>
#include <alsa/asoundlib.h>
#include <iostream>
#include "wavheader.hpp"

_snd_pcm_format inttoformat(int i, int audioFormat) {
	switch (i) {
		case 8: {
			return SND_PCM_FORMAT_U8;
		}
		
		case 32: {
			if (audioFormat == 1) return SND_PCM_FORMAT_S32_LE;
			else if (audioFormat == 3) return SND_PCM_FORMAT_FLOAT;
			break;
		}
	}

	return SND_PCM_FORMAT_S16_LE;
}

class PCM {
	bool isopened = false;
	_snd_pcm_format _format;

	public:
	snd_pcm_hw_params_t *params;
	snd_pcm_t *pcm;
	PCM() {}
	PCM(std::string device, _snd_pcm_stream stream, int mode) { open(device, stream, mode); }

	~PCM() {
		pcm_exit();
	}

	void setup(std::string device, wav_header_t header, int mode) {
		std::cout << "device" << std::endl;
		try { open(device, (_snd_pcm_stream)mode, 0); } catch (int e) {  if (e == 1) open(cardname("plughw"), (_snd_pcm_stream)mode, 0); }
		setAccess(SND_PCM_ACCESS_RW_INTERLEAVED);
		setFormat(inttoformat(header.bitsPerSample, header.audioFormat));
		setChannels(header.numChannels);
		setRate(header.sampleRate);
	}

	void open(std::string device, _snd_pcm_stream stream, int mode) {
		int error;
		if ((error = snd_pcm_open(&pcm, device.c_str(), stream, mode)) != 0) 
		if ((error = snd_pcm_open(&pcm, cardname("plughw").c_str(), stream, mode)) != 0) throw error;

		snd_pcm_hw_params_malloc(&params);
		snd_pcm_hw_params_any(pcm, params);
		isopened = true;
	}
	
	void setAccess(_snd_pcm_access _access) {
		int error;
		if ((error = snd_pcm_hw_params_set_access(pcm, params, _access)) < 0) throw error;
	}

	void setFormat(_snd_pcm_format format) {
		_format = format;
		int error;
		if ((error = snd_pcm_hw_params_set_format(pcm, params, format)) < 0) throw error;
	}

	void setChannels(int channels) {
		int error;
		if ((error = snd_pcm_hw_params_set_channels(pcm, params, channels)) < 0) throw error;
	}

	void setRate(unsigned int rate) {
		int error;
		if ((error = snd_pcm_hw_params_set_rate_near(pcm, params, &rate, 0)) < 0) throw error;
	}

	void setBufferSize(int size) {
		int error;
		if (snd_pcm_hw_params_test_buffer_size(pcm, params, size) == 0)
		if ((error = snd_pcm_hw_params_set_buffer_size(pcm, params, size)) < 0) throw error;
	}

	void setPeriodSize(int size) {
		int error;
		if (snd_pcm_hw_params_test_period_size(pcm, params, size, 0) == 0)
		if ((error = snd_pcm_hw_params_set_period_size(pcm, params, size, 0)) < 0) throw error;	
	}

	void paramsApply() {
		int error;
		if ((error = snd_pcm_hw_params(pcm, params)) < 0) throw error;
		snd_pcm_hw_params_free(params);
	}

	std::string getName() {
		return std::string(snd_pcm_name(pcm));
	}

	std::string getState() {
		return std::string(snd_pcm_state_name(snd_pcm_state(pcm)));
	}

	int getChannels() {
		unsigned int tmp;
		snd_pcm_hw_params_get_channels(params, &tmp);
		return tmp;
	}

	unsigned int getRate() {
		unsigned int tmp;
		snd_pcm_hw_params_get_rate(params, &tmp, 0);
		return tmp;
	}

	unsigned int getMaxRate() {
		unsigned int val;

		snd_pcm_hw_params_get_rate_max(params, &val, 0);
		return val;
	}

	unsigned int getMinRate() {
		unsigned int val;

		snd_pcm_hw_params_get_rate_min(params, &val, 0);
		return val;
	}

	int getPeriod() {
		snd_pcm_uframes_t frames;

		snd_pcm_hw_params_get_period_size(params, &frames, 0);
		return frames;
	}

	int getMaxPeriod() {
		snd_pcm_uframes_t frames;

		snd_pcm_hw_params_get_period_size_max(params, &frames, 0);
		return frames;
	}

	int getMinPeriod() {
		snd_pcm_uframes_t frames;

		snd_pcm_hw_params_get_period_size_min(params, &frames, 0);
		return frames;
	}

	int getBufferSize() {
		snd_pcm_uframes_t buf_size;
		snd_pcm_hw_params_get_buffer_size(params, &buf_size);
		return buf_size;
	}

	int getMaxBuffer() {
		snd_pcm_uframes_t frames;

		snd_pcm_hw_params_get_buffer_size_max(params, &frames);
		return frames;
	}

	int getMinBuffer() {
		snd_pcm_uframes_t frames;

		snd_pcm_hw_params_get_buffer_size_min(params, &frames);
		return frames;
	}

	int getFormatWidth() {
		return snd_pcm_format_width(_format);
	}

	void start() {
		int error;
		if ((error = snd_pcm_start(pcm)) < 0) throw error;
	}

	void prepare() {
		int error;
		if ((error = snd_pcm_prepare(pcm)) < 0) throw error;
	}

	void recover(int err, int silent) {
		int error;
		if ((error = snd_pcm_recover(pcm, err, silent)) < 0) throw error;
	}

	void writei(const void * buff, snd_pcm_uframes_t frames) {
		if (getState() == "PAUSED") resume();

		int error;
		if ((error = snd_pcm_writei(pcm, buff, frames)) < 0) recover(error, 0);
	}

	void readi(void * buff, snd_pcm_uframes_t frames) {
		if (getState() == "PAUSED") resume();
		int error;
		if ((error = snd_pcm_readi(pcm, buff, frames)) < 0) recover(error, 1);
	}

	int pause() {
		return snd_pcm_pause(pcm, 1);
	}

	int resume() {
		return snd_pcm_pause(pcm, 0);
	}

	int bufferAvailable() {
		return snd_pcm_avail(pcm);
	}

	void drain() {
		int error;
		if ((error = snd_pcm_drain(pcm)) < 0) throw error;

	}

	void drop() {
		int error;
		if ((error = snd_pcm_drop(pcm)) < 0) throw error;
	}

	void close() {
		isopened = false;
		snd_pcm_close(pcm);
		snd_pcm_hw_free(pcm);
	}

	void pcm_exit() {
		if (isopened) close();
		isopened = false;
	}

	std::vector<std::string> cardlist() {
		std::vector<std::string> list;
		char **hints;

		int err = snd_device_name_hint(-1, "pcm", (void***)&hints);
		if (err != 0) return {};

		char** n = hints;
		while (*n) {
			char *name = snd_device_name_get_hint(*n, "NAME");

			if (name && strcmp("null", name)) {
				list.push_back(name);
				free(name);
			}

			n++;
		}

		snd_device_name_free_hint((void**)hints);

		return list;
	}

	std::string cardname(std::string str) {
		std::vector<std::string> names = cardlist();

		for (std::string i : names) {
			if (i.find(str) != std::string::npos) return i;
		}

		return "default";
	}
};