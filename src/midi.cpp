#include "midi.h"

Midi::Midi() = default;
Midi::~Midi() = default;

bool Midi::openSoundFont(const std::string &path, std::string &error) {
	(void)path;
	error = "no software synthesizer built in";
	return false;
}

static int dataBytes(uint8_t status) {
	switch (status & 0xF0) {
	case 0xC0:
	case 0xD0:
		return 1;
	case 0xF0:
		return 0;
	default:
		return 2;
	}
}

void Midi::sendByte(uint8_t b) {
	std::lock_guard<std::mutex> lk(mtx_);
	if (inSysex_) {
		if (b == 0xF7) {
			inSysex_ = false;
			if (synth_)
				synth_->sysex(sysex_.data(), sysex_.size());
			sysex_.clear();
		} else if (!(b & 0x80)) {
			sysex_.push_back(b);
		} else {
			inSysex_ = false;
			sysex_.clear();
		}
		if (inSysex_ || b == 0xF7)
			return;
	}
	if (b >= 0xF8)
		return; // real-time
	if (b == 0xF0) {
		inSysex_ = true;
		sysex_.clear();
		return;
	}
	if (b & 0x80) {
		running_ = b;
		need_ = dataBytes(b);
		have_ = 0;
		return;
	}
	if (!running_)
		return;
	data_[have_++] = b;
	if (have_ >= need_) {
		if (synth_)
			synth_->shortMessage(running_, data_[0], need_ > 1 ? data_[1] : 0);
		have_ = 0;
	}
}

void Midi::oplWrite(uint8_t reg, uint8_t val) {
	(void)reg;
	(void)val;
}

void Midi::render(int16_t *out, int frames) {
	std::lock_guard<std::mutex> lk(mtx_);
	if (synth_)
		synth_->render(out, frames);
}
