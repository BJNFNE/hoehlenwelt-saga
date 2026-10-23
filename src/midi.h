// Music output: receives the raw MIDI byte stream the game sends to the
// MPU-401 and the AdLib register writes, and renders them to PCM.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class Synth {
public:
	virtual ~Synth() = default;
	virtual void shortMessage(uint8_t status, uint8_t d1, uint8_t d2) = 0;
	virtual void sysex(const uint8_t *data, size_t len) { (void)data; (void)len; }
	virtual void render(int16_t *out, int frames) = 0; // adds to stereo buffer
	virtual void reset() {}
};

class Midi {
public:
	Midi();
	~Midi();

	// returns false if no synthesizer could be created
	bool openSoundFont(const std::string &path, std::string &error);
	bool active() const { return synth_ != nullptr; }

	void sendByte(uint8_t b);
	void oplWrite(uint8_t reg, uint8_t val);
	void render(int16_t *out, int frames);

private:
	std::mutex mtx_;
	std::unique_ptr<Synth> synth_;
	uint8_t running_ = 0;
	uint8_t data_[2];
	int need_ = 0, have_ = 0;
	bool inSysex_ = false;
	std::vector<uint8_t> sysex_;
};
