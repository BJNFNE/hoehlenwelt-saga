// Platform layer (SDL2): window, rendering, input and audio mixing.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

class Machine;
class CdAudio;
class Midi;

struct HostOptions {
	int scale = 2;
	bool fullscreen = false;
	bool smooth = false;
	int musicVolume = 100; // percent
	int sfxVolume = 100;
	int cdVolume = 100;
};

class Host {
public:
	bool init(const HostOptions &opt, CdAudio *cd, Midi *midi);
	void shutdown();

	void present(const uint8_t *vram, const uint8_t dac[256][3], bool blank);
	void presentText(const uint8_t *textMem);
	void pollEvents(Machine &m);
	void sleepMicros(uint32_t us);
	void warpMouse(int x, int y);
	void showError(const std::string &title, const std::string &msg);
	void showInfo(const std::string &title, const std::string &msg);
	// show an empty window and wait for a file/folder to be dropped on it;
	// returns "" if the window was closed
	std::string waitForDrop();
	void attachAudio(CdAudio *cd, Midi *midi);

	// Sound Blaster PCM (unsigned 8-bit mono)
	void playPcm(const uint8_t *data, int len, uint32_t rate);
	void stopPcm();
	void pausePcm(bool p);

	bool quitRequested = false;

	// testing aids: periodic screenshots and scripted input
	std::string shotDir;
	int shotIntervalMs = 0;
	std::string inputScript;
	void loadInputScript(const std::string &path);
	void saveShot(const std::string &path);

private:
	SDL_Window *window_ = nullptr;
	SDL_Renderer *renderer_ = nullptr;
	SDL_Texture *texture_ = nullptr;
	std::vector<uint32_t> pixels_;
	HostOptions opt_;
	CdAudio *cd_ = nullptr;
	Midi *midi_ = nullptr;
	uint32_t audioDev_ = 0;
	bool fullscreen_ = false;

	// PCM voice
	std::vector<uint8_t> pcm_;
	uint64_t pcmPos_ = 0; // 16.16 fixed point
	uint64_t pcmStep_ = 0;
	bool pcmPaused_ = false;

	uint32_t lastShotMs_ = 0;
	int shotCount_ = 0;
	struct ScriptEvent {
		uint32_t ms;
		std::string kind;
		int a = 0, b = 0, c = 0;
	};
	std::vector<ScriptEvent> script_;
	size_t scriptPos_ = 0;
	uint32_t startMs_ = 0;
	void runScript(Machine &m);

	static void audioCallback(void *user, uint8_t *stream, int len);
	void mix(int16_t *out, int frames);
	void toggleFullscreen();
};
