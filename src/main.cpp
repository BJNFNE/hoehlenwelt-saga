// hwsaga - runtime for "Die Höhlenwelt-Saga - Das leuchtende Kristall"
// (Software 2000, 1994). Runs the original HW.EXE supplied by the user.
#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "fileutil.h"
#include "cdaudio.h"
#include "host.h"
#include "machine.h"
#include "midi.h"
#include "vfs.h"

namespace fs = std::filesystem;

static void usage() {
	printf("Usage: hwsaga [options] [GAME]\n"
	       "\n"
	       "GAME is the CD image (HW.CUE with HW.BIN) or a directory containing\n"
	       "either the image or the game files (HW.EXE, HW.001 ... HW.015).\n"
	       "Without GAME the last used game is started.\n"
	       "\n"
	       "Options:\n"
	       "  --fullscreen      start in fullscreen (toggle with Alt+Enter)\n"
	       "  --scale N         window size factor (default 2)\n"
	       "  --smooth          linear filtering when scaling\n"
	       "  --no-music        disable music\n"
	       "  --trace           log file accesses\n"
	       "  --help            this text\n");
}

static std::string findCue(const std::string &dir) {
	std::error_code ec;
	for (auto &e : fs::directory_iterator(fsu::path(dir), ec)) {
		std::string ext = e.path().extension().u8string();
		for (auto &c : ext)
			c = (char)tolower((unsigned char)c);
		if (ext == ".cue")
			return e.path().u8string();
	}
	return "";
}

static void writeFile(const fs::path &p, const void *data, size_t n) {
	FILE *f = fsu::open(p.u8string(), "wb");
	if (f) {
		fwrite(data, 1, n, f);
		fclose(f);
	}
}

int main(int argc, char **argv) {
	HostOptions opt;
	std::string game;
	bool noMusic = false, trace = false;
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		if (a == "--help" || a == "-h") {
			usage();
			return 0;
		} else if (a == "--fullscreen") {
			opt.fullscreen = true;
		} else if (a == "--scale" && i + 1 < argc) {
			opt.scale = std::max(1, atoi(argv[++i]));
		} else if (a == "--smooth") {
			opt.smooth = true;
		} else if (a == "--no-music") {
			noMusic = true;
		} else if (a == "--trace") {
			trace = true;
		} else if (!a.empty() && a[0] == '-') {
			fprintf(stderr, "unknown option %s\n", a.c_str());
			usage();
			return 1;
		} else {
			game = a;
		}
	}

	char *pref = SDL_GetPrefPath("hwsaga", "hoehlenwelt");
	fs::path prefDir = pref ? fsu::path(pref) : fs::current_path() / "hwsaga-data";
	SDL_free(pref);
	fs::path lastFile = prefDir / "lastgame.txt";

	Host host;
	CdAudio cd;
	Midi midi;
	Vfs vfs;
	std::string error;
	if (!host.init(opt, nullptr, nullptr))
		return 1;

	// Locate the game: CD image (cue sheet) or a directory with the files.
	std::string dataDir, cue;
	auto tryGame = [&](const std::string &path) -> bool {
		std::error_code ec;
		std::string c;
		if (path.empty())
			return false;
		if (fs::is_directory(fsu::path(path), ec)) {
			if (!Vfs::findInDir(path, "HW.EXE").empty() && !Vfs::findInDir(path, "HW.001").empty()) {
				dataDir = path;
				if (!cd.openTrackDir(path)) {
					std::string c2 = findCue(path);
					if (!c2.empty())
						cd.openCue(c2, error);
				}
				return true;
			}
			c = findCue(path);
		} else if (fs::is_regular_file(fsu::path(path), ec)) {
			c = path;
		}
		if (c.empty())
			return false;
		if (!cd.openCue(c, error)) {
			fprintf(stderr, "%s\n", error.c_str());
			return false;
		}
		fs::path extracted = prefDir / "data";
		if (Vfs::findInDir(extracted.u8string(), "HW.EXE").empty()) {
			fprintf(stderr, "extracting game files from %s ...\n", c.c_str());
			if (!cd.extractData(extracted.u8string(), error)) {
				fprintf(stderr, "%s\n", error.c_str());
				return false;
			}
		}
		cue = c;
		dataDir = extracted.u8string();
		return true;
	};

	std::vector<std::string> candidates;
	if (!game.empty()) {
		candidates.push_back(game);
	} else {
		FILE *f = fsu::open(lastFile.u8string(), "rb");
		if (f) {
			char buf[4096] = {};
			size_t n = fread(buf, 1, sizeof(buf) - 1, f);
			fclose(f);
			std::string last(buf, n);
			while (!last.empty() && (last.back() == '\n' || last.back() == '\r'))
				last.pop_back();
			candidates.push_back(last);
		}
		// next to the program (for a macOS app bundle: next to the .app)
		if (char *base = SDL_GetBasePath()) {
			fs::path b = fsu::path(base);
			SDL_free(base);
			if (b.filename().empty())
				b = b.parent_path(); // strip trailing separator
			candidates.push_back(fsu::str(b));
			candidates.push_back(fsu::str(b / "HW"));
			fs::path app = b.parent_path().parent_path().parent_path();
			candidates.push_back(fsu::str(app));
		}
		candidates.push_back(fsu::str(fs::current_path()));
	}
	bool found = false;
	for (const auto &c : candidates)
		if ((found = tryGame(c)))
			break;
	while (!found) {
		host.showInfo("Die Höhlenwelt-Saga",
		              "Spieldaten nicht gefunden.\n\nBitte das CD-Image (HW.CUE mit HW.BIN) oder den Ordner "
		              "mit den Spieldateien in das Fenster ziehen.\n\nAlternativ: hwsaga /pfad/zu/HW.CUE");
		std::string dropped = host.waitForDrop();
		if (dropped.empty()) {
			host.shutdown();
			return 1;
		}
		found = tryGame(dropped);
		if (!found)
			host.showError("Die Höhlenwelt-Saga", "Dort wurden keine Spieldaten gefunden:\n" + dropped);
	}
	std::error_code ec;
	{
		FILE *f = fsu::open(lastFile.u8string(), "wb");
		if (f) {
			fs::path abs = fs::absolute(cue.empty() ? fsu::path(dataDir) : fsu::path(cue), ec);
			std::string s = abs.u8string();
			fwrite(s.data(), 1, s.size(), f);
			fclose(f);
		}
	}

	fs::path saveDir = prefDir / "save";
	vfs.setDirs(dataDir, saveDir.u8string());

	// configuration normally written by INSTALL.EXE
	bool cdMusic = cd.hasAudio() && !noMusic;
	{
		const uint8_t wsa[6] = {0xFF, 0xFF, 0x07, 0x00, 0x00, 0x00};
		writeFile(saveDir / "HW.WSA", wsa, sizeof(wsa));
		// text speed is the x position of the menu slider (0x1AC..0x225)
		uint8_t cfg[14] = {4, 1, 0x30, 0x03, 0, 0x20, 0x02, 7, 1, 0xE8, 0x01, 100, 100, 0};
		std::string p = Vfs::findInDir(saveDir.u8string(), "HW.CFG");
		if (!p.empty()) {
			FILE *f = fsu::open(p, "rb");
			if (f) {
				uint8_t old[14];
				int speed = 0;
				if (fread(old, 1, 14, f) == 14 && (speed = old[9] | (old[10] << 8)) >= 0x1AC && speed <= 0x225) {
					// keep the player's volumes and text speed
					cfg[9] = old[9];
					cfg[10] = old[10];
					cfg[11] = old[11];
					cfg[12] = old[12];
					cfg[13] = old[13];
				}
				fclose(f);
			}
		}
		cfg[0] = cdMusic ? 4 : 0;
		cfg[1] = 1;
		writeFile(saveDir / "HW.CFG", cfg, sizeof(cfg));
	}

	host.attachAudio(&cd, &midi);

	if (const char *d = getenv("HWSAGA_SHOTS")) {
		host.shotDir = d;
		const char *iv = getenv("HWSAGA_SHOT_MS");
		host.shotIntervalMs = iv ? atoi(iv) : 0;
	}
	if (const char *sc = getenv("HWSAGA_SCRIPT"))
		host.loadInputScript(sc);

	Machine m(host, vfs, cd, midi);
	m.trace = trace;
	if (!m.loadNE("HW.EXE")) {
		host.showError("Die Höhlenwelt-Saga", "HW.EXE konnte nicht geladen werden.");
		host.shutdown();
		return 1;
	}

	m.profile = getenv("HWSAGA_PROFILE") != nullptr;
	if (const char *bp = getenv("HWSAGA_BREAK"))
		m.addBreakpoints(bp);

	while (!m.quit) {
		bool idle = m.runSlice(20000);
		m.service();
		if (idle)
			host.sleepMicros(1000);
		if (host.quitRequested)
			m.quit = true;
	}
	if (m.profile)
		m.printProfile();
	host.shutdown();
	return m.exitCode;
}
