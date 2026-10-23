#include "fileutil.h"
#include "cdaudio.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "vfs.h"

namespace fs = std::filesystem;

static constexpr int FRAMES_PER_SECTOR = 2352 / 4;

CdAudio::~CdAudio() {
	for (FILE *f : files_)
		if (f)
			fclose(f);
}

static uint32_t parseMsf(const std::string &s) {
	int m = 0, sec = 0, f = 0;
	sscanf(s.c_str(), "%d:%d:%d", &m, &sec, &f);
	return (uint32_t)((m * 60 + sec) * 75 + f);
}

bool CdAudio::openCue(const std::string &cuePath, std::string &error) {
	std::ifstream in(fsu::path(cuePath));
	if (!in) {
		error = "cannot open " + cuePath;
		return false;
	}
	fs::path base = fsu::path(cuePath).parent_path();
	std::string line;
	int curFile = -1;
	uint32_t pregapTotal = 0;
	Track *cur = nullptr;
	std::vector<uint32_t> fileIndex; // INDEX 01 position in file (sectors) per track
	while (std::getline(in, line)) {
		std::istringstream ls(line);
		std::string kw;
		ls >> kw;
		for (auto &c : kw)
			c = (char)toupper((unsigned char)c);
		if (kw == "FILE") {
			size_t q1 = line.find('"'), q2 = line.rfind('"');
			std::string name;
			if (q1 != std::string::npos && q2 > q1)
				name = line.substr(q1 + 1, q2 - q1 - 1);
			else
				ls >> name;
			std::string p = Vfs::findInDir(base.u8string(), fsu::path(name).filename().u8string());
			if (p.empty()) {
				error = "file from cue sheet not found: " + name;
				return false;
			}
			FILE *f = fsu::open(p, "rb");
			if (!f) {
				error = "cannot open " + p;
				return false;
			}
			files_.push_back(f);
			curFile = (int)files_.size() - 1;
		} else if (kw == "TRACK") {
			int num;
			std::string type;
			ls >> num >> type;
			tracks_.emplace_back();
			cur = &tracks_.back();
			cur->audio = type == "AUDIO";
			cur->file = curFile;
			cur->sectorSize = 2352;
			if (type == "MODE1/2048")
				cur->sectorSize = 2048;
			fileIndex.push_back(0);
		} else if (kw == "PREGAP" && cur) {
			std::string t;
			ls >> t;
			pregapTotal += parseMsf(t);
		} else if (kw == "INDEX" && cur) {
			int idx;
			std::string t;
			ls >> idx >> t;
			if (idx == 1) {
				uint32_t pos = parseMsf(t);
				fileIndex.back() = pos;
				cur->start = pos + pregapTotal;
				cur->fileOffset = (uint64_t)pos * cur->sectorSize;
			}
		}
	}
	if (tracks_.empty()) {
		error = "no tracks in cue sheet";
		return false;
	}
	// lengths: up to next track start within the same file, last track to file end
	for (size_t i = 0; i < tracks_.size(); i++) {
		Track &t = tracks_[i];
		if (i + 1 < tracks_.size() && tracks_[i + 1].file == t.file) {
			t.length = (uint32_t)(fileIndex[i + 1] - fileIndex[i]);
		} else {
			FILE *f = files_[t.file];
			fsu::seek(f, 0, SEEK_END);
			uint64_t sz = (uint64_t)fsu::tell(f);
			t.length = (uint32_t)((sz - t.fileOffset) / t.sectorSize);
		}
	}
	leadOut_ = tracks_.back().start + tracks_.back().length;
	return true;
}

bool CdAudio::openTrackDir(const std::string &dir) {
	// track 1 is the (absent) data track; audio tracks follow with 2 s gaps
	tracks_.clear();
	Track data;
	data.audio = false;
	data.start = 0;
	data.length = 30000;
	tracks_.push_back(data);
	uint32_t lba = data.length + 150;
	for (int n = 2; n < 100; n++) {
		char name[32];
		std::string p;
		bool be = false;
		uint64_t hdr = 0;
		snprintf(name, sizeof(name), "track%02d.wav", n);
		p = Vfs::findInDir(dir, name);
		if (!p.empty()) {
			hdr = 44;
		} else {
			snprintf(name, sizeof(name), "track%02d.cdr", n);
			p = Vfs::findInDir(dir, name);
			be = true;
			if (p.empty()) {
				snprintf(name, sizeof(name), "track%02d.raw", n);
				p = Vfs::findInDir(dir, name);
				be = false;
			}
		}
		if (p.empty())
			break;
		FILE *f = fsu::open(p, "rb");
		if (!f)
			break;
		if (hdr) {
			// locate the "data" chunk
			uint8_t buf[12];
			if (fread(buf, 1, 12, f) == 12 && !memcmp(buf, "RIFF", 4)) {
				for (;;) {
					uint8_t ch[8];
					if (fread(ch, 1, 8, f) != 8)
						break;
					uint32_t len = ch[4] | (ch[5] << 8) | (ch[6] << 16) | ((uint32_t)ch[7] << 24);
					if (!memcmp(ch, "data", 4)) {
						hdr = (uint64_t)fsu::tell(f);
						break;
					}
					fsu::seek(f, (int64_t)len, SEEK_CUR);
				}
			}
		}
		fsu::seek(f, 0, SEEK_END);
		uint64_t sz = (uint64_t)fsu::tell(f);
		files_.push_back(f);
		Track t;
		t.audio = true;
		t.start = lba;
		t.length = (uint32_t)((sz - hdr) / 2352);
		t.file = (int)files_.size() - 1;
		t.fileOffset = hdr;
		t.bigEndian = be;
		tracks_.push_back(t);
		lba += t.length + 150;
	}
	if (tracks_.size() <= 1) {
		tracks_.clear();
		return false;
	}
	leadOut_ = lba;
	return true;
}

bool CdAudio::hasAudio() const {
	for (auto &t : tracks_)
		if (t.audio)
			return true;
	return false;
}

bool CdAudio::readSector(uint32_t lba, uint8_t *out) {
	const Track &t = tracks_[0];
	if (t.audio || t.file < 0 || lba >= t.length)
		return false;
	FILE *f = files_[t.file];
	uint64_t off = t.fileOffset + (uint64_t)lba * t.sectorSize + (t.sectorSize == 2352 ? 16 : 0);
	fsu::seek(f, (int64_t)off, SEEK_SET);
	return fread(out, 1, 2048, f) == 2048;
}

bool CdAudio::extractData(const std::string &dir, std::string &error) {
	uint8_t pvd[2048];
	if (!readSector(16, pvd) || memcmp(pvd + 1, "CD001", 5) != 0) {
		error = "track 1 is not an ISO 9660 data track";
		return false;
	}
	std::error_code ec;
	fs::create_directories(fsu::path(dir), ec);
	const uint8_t *root = pvd + 156;
	uint32_t dirLba = root[2] | (root[3] << 8) | (root[4] << 16) | ((uint32_t)root[5] << 24);
	uint32_t dirLen = root[10] | (root[11] << 8) | (root[12] << 16) | ((uint32_t)root[13] << 24);
	std::vector<uint8_t> d(((dirLen + 2047) / 2048) * 2048);
	for (uint32_t i = 0; i < d.size() / 2048; i++)
		if (!readSector(dirLba + i, &d[i * 2048])) {
			error = "cannot read ISO directory";
			return false;
		}
	size_t p = 0;
	int count = 0;
	std::vector<uint8_t> sec(2048);
	while (p < dirLen) {
		uint8_t len = d[p];
		if (len == 0) {
			p = (p / 2048 + 1) * 2048;
			continue;
		}
		const uint8_t *r = &d[p];
		uint32_t lba = r[2] | (r[3] << 8) | (r[4] << 16) | ((uint32_t)r[5] << 24);
		uint32_t size = r[10] | (r[11] << 8) | (r[12] << 16) | ((uint32_t)r[13] << 24);
		uint8_t flags = r[25];
		uint8_t nlen = r[32];
		std::string name((const char *)r + 33, nlen);
		p += len;
		if (flags & 2 || nlen == 1)
			continue; // directories, "." and ".."
		size_t semi = name.find(';');
		if (semi != std::string::npos)
			name = name.substr(0, semi);
		if (!name.empty() && name.back() == '.')
			name.pop_back();
		FILE *out = fsu::open((fsu::path(dir) / name).u8string(), "wb");
		if (!out) {
			error = "cannot write " + name;
			return false;
		}
		uint32_t left = size;
		for (uint32_t s = 0; left > 0; s++) {
			if (!readSector(lba + s, sec.data())) {
				fclose(out);
				error = "read error in " + name;
				return false;
			}
			uint32_t n = std::min<uint32_t>(left, 2048);
			fwrite(sec.data(), 1, n, out);
			left -= n;
		}
		fclose(out);
		count++;
	}
	if (count == 0) {
		error = "no files on the CD";
		return false;
	}
	return true;
}

uint32_t CdAudio::trackStart(int t) const {
	if (t < 1 || t > (int)tracks_.size())
		return leadOut_;
	return tracks_[t - 1].start;
}

bool CdAudio::trackIsAudio(int t) const {
	return t >= 1 && t <= (int)tracks_.size() && tracks_[t - 1].audio;
}

int CdAudio::trackAt(uint32_t lba) const {
	int r = 1;
	for (size_t i = 0; i < tracks_.size(); i++)
		if (lba >= tracks_[i].start)
			r = (int)i + 1;
	return r;
}

void CdAudio::play(uint32_t lba, uint32_t count) {
	std::lock_guard<std::mutex> lk(mtx_);
	playStart_ = lba;
	playEnd_ = lba + count;
	posFrames_ = 0;
	playing_ = count > 0;
	paused_ = false;
}

void CdAudio::stop() {
	std::lock_guard<std::mutex> lk(mtx_);
	playing_ = false;
	paused_ = false;
}

void CdAudio::pause() {
	std::lock_guard<std::mutex> lk(mtx_);
	if (playing_) {
		playing_ = false;
		paused_ = true;
	}
}

void CdAudio::resume() {
	std::lock_guard<std::mutex> lk(mtx_);
	if (paused_) {
		paused_ = false;
		playing_ = true;
	}
}

bool CdAudio::playing() {
	std::lock_guard<std::mutex> lk(mtx_);
	return playing_;
}

bool CdAudio::paused() {
	std::lock_guard<std::mutex> lk(mtx_);
	return paused_;
}

uint32_t CdAudio::position() {
	std::lock_guard<std::mutex> lk(mtx_);
	return playStart_ + (uint32_t)(posFrames_ / FRAMES_PER_SECTOR);
}

void CdAudio::render(int16_t *out, int frames, int volume) {
	std::lock_guard<std::mutex> lk(mtx_);
	if (!playing_)
		return;
	std::vector<uint8_t> buf;
	int done = 0;
	while (done < frames && playing_) {
		uint32_t lba = playStart_ + (uint32_t)(posFrames_ / FRAMES_PER_SECTOR);
		if (lba >= playEnd_) {
			playing_ = false;
			break;
		}
		int ti = trackAt(lba) - 1;
		const Track &t = tracks_[ti];
		if (!t.audio || lba >= t.start + t.length) {
			// gap or data: skip to the next track start
			posFrames_ += FRAMES_PER_SECTOR;
			continue;
		}
		uint64_t frameInTrack = posFrames_ + (uint64_t)(playStart_ - t.start) * FRAMES_PER_SECTOR;
		uint64_t trackFrames = (uint64_t)t.length * FRAMES_PER_SECTOR;
		uint64_t endFrames = (uint64_t)(playEnd_ - t.start) * FRAMES_PER_SECTOR;
		uint64_t limit = std::min(trackFrames, endFrames);
		int n = (int)std::min<uint64_t>(frames - done, limit - frameInTrack);
		if (n <= 0) {
			posFrames_ += FRAMES_PER_SECTOR;
			continue;
		}
		buf.resize((size_t)n * 4);
		FILE *f = files_[t.file];
		fsu::seek(f, (int64_t)(t.fileOffset + frameInTrack * 4), SEEK_SET);
		size_t got = fread(buf.data(), 4, (size_t)n, f);
		for (size_t i = 0; i < got * 2; i++) {
			int16_t s = t.bigEndian ? (int16_t)((buf[i * 2] << 8) | buf[i * 2 + 1])
			                        : (int16_t)(buf[i * 2] | (buf[i * 2 + 1] << 8));
			int v = out[done * 2 + i] + s * volume / 256 * volume_ / 255;
			out[done * 2 + i] = (int16_t)std::clamp(v, -32768, 32767);
		}
		if (got < (size_t)n) {
			playing_ = false;
			break;
		}
		done += n;
		posFrames_ += (uint64_t)n;
	}
}
