// CD-ROM image access: CUE/BIN images (data + audio tracks), extraction of
// the ISO 9660 data track and CD-audio playback for the emulated MSCDEX.
#pragma once

#include <cstdint>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

class CdAudio {
public:
	~CdAudio();

	// CUE sheet with BIN file(s)
	bool openCue(const std::string &cuePath, std::string &error);
	// directory with trackNN.wav / trackNN.cdr (big-endian raw) / trackNN.raw files
	bool openTrackDir(const std::string &dir);
	// extract all files of the ISO data track (track 1) into dir
	bool extractData(const std::string &dir, std::string &error);

	bool present() const { return !tracks_.empty(); }
	bool hasAudio() const;
	int firstTrack() const { return 1; }
	int lastTrack() const { return (int)tracks_.size(); }
	uint32_t trackStart(int t) const;
	bool trackIsAudio(int t) const;
	int trackAt(uint32_t lba) const;
	uint32_t leadOut() const { return leadOut_; }

	void play(uint32_t lba, uint32_t count);
	void stop();
	void pause();
	void resume();
	bool playing();
	bool paused();
	uint32_t position();
	uint32_t lastStart() const { return playStart_; }
	uint32_t lastEnd() const { return playEnd_; }
	void setVolume(int v) { volume_ = v; } // 0..255, set by the game

	// mix 44.1 kHz stereo into out (adds)
	void render(int16_t *out, int frames, int volume);

private:
	struct Track {
		bool audio = false;
		uint32_t start = 0;  // disc LBA
		uint32_t length = 0; // sectors
		int file = -1;
		uint64_t fileOffset = 0; // byte offset of the track start in its file
		int sectorSize = 2352;
		bool bigEndian = false;
	};
	std::vector<Track> tracks_;
	std::vector<FILE *> files_;
	uint32_t leadOut_ = 0;

	std::mutex mtx_;
	bool playing_ = false, paused_ = false;
	uint32_t playStart_ = 0, playEnd_ = 0;
	uint64_t posFrames_ = 0; // audio frames since playStart_
	std::atomic<int> volume_{255};

	bool readSector(uint32_t lba, uint8_t *out2048);
};
