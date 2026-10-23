#include "fileutil.h"
#include "host.h"

#include <SDL.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "cdaudio.h"
#include "machine.h"
#include "midi.h"

static constexpr int OUT_RATE = 44100;

bool Host::init(const HostOptions &opt, CdAudio *cd, Midi *midi) {
	opt_ = opt;
	cd_ = cd;
	midi_ = midi;
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) {
		fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		return false;
	}
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, opt.smooth ? "linear" : "nearest");
	Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
	window_ = SDL_CreateWindow("Die Höhlenwelt-Saga", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
	                           640 * opt.scale, 480 * opt.scale, flags);
	if (!window_) {
		fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
		return false;
	}
	renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if (!renderer_)
		renderer_ = SDL_CreateRenderer(window_, -1, 0);
	if (!renderer_) {
		fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
		return false;
	}
	SDL_RenderSetLogicalSize(renderer_, 640, 480);
	texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 640, 480);
	pixels_.assign(640 * 480, 0xFF000000);
	SDL_ShowCursor(SDL_DISABLE);
	if (opt.fullscreen)
		toggleFullscreen();

	SDL_AudioSpec want{}, have{};
	want.freq = OUT_RATE;
	want.format = AUDIO_S16SYS;
	want.channels = 2;
	want.samples = 1024;
	want.callback = audioCallback;
	want.userdata = this;
	audioDev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
	if (audioDev_)
		SDL_PauseAudioDevice(audioDev_, 0);
	else
		fprintf(stderr, "no audio: %s\n", SDL_GetError());
	return true;
}

void Host::shutdown() {
	if (audioDev_)
		SDL_CloseAudioDevice(audioDev_);
	if (texture_)
		SDL_DestroyTexture(texture_);
	if (renderer_)
		SDL_DestroyRenderer(renderer_);
	if (window_)
		SDL_DestroyWindow(window_);
	SDL_Quit();
}

void Host::showError(const std::string &title, const std::string &msg) {
	fprintf(stderr, "%s: %s\n", title.c_str(), msg.c_str());
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title.c_str(), msg.c_str(), window_);
}

void Host::showInfo(const std::string &title, const std::string &msg) {
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, title.c_str(), msg.c_str(), window_);
}

std::string Host::waitForDrop() {
	SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
	std::fill(pixels_.begin(), pixels_.end(), 0xFF000000u);
	for (;;) {
		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			if (e.type == SDL_QUIT)
				return "";
			if (e.type == SDL_DROPFILE) {
				std::string p = e.drop.file;
				SDL_free(e.drop.file);
				return p;
			}
		}
		SDL_RenderClear(renderer_);
		SDL_RenderPresent(renderer_);
		SDL_Delay(20);
	}
}

void Host::attachAudio(CdAudio *cd, Midi *midi) {
	if (audioDev_)
		SDL_LockAudioDevice(audioDev_);
	cd_ = cd;
	midi_ = midi;
	if (audioDev_)
		SDL_UnlockAudioDevice(audioDev_);
}

void Host::toggleFullscreen() {
	fullscreen_ = !fullscreen_;
	SDL_SetWindowFullscreen(window_, fullscreen_ ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

void Host::present(const uint8_t *vram, const uint8_t dac[256][3], bool blank) {
	uint32_t pal[256];
	for (int i = 0; i < 256; i++) {
		uint32_t r = dac[i][0], g = dac[i][1], b = dac[i][2];
		r = (r << 2) | (r >> 4);
		g = (g << 2) | (g >> 4);
		b = (b << 2) | (b >> 4);
		pal[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
	}
	if (blank) {
		std::fill(pixels_.begin(), pixels_.end(), 0xFF000000u);
	} else {
		for (int i = 0; i < 640 * 480; i++)
			pixels_[i] = pal[vram[i]];
	}
	if (!shotDir.empty() && shotIntervalMs > 0 && SDL_GetTicks() - lastShotMs_ >= (uint32_t)shotIntervalMs) {
		lastShotMs_ = SDL_GetTicks();
		char name[64];
		snprintf(name, sizeof(name), "/shot%04d.bmp", shotCount_++);
		saveShot(shotDir + name);
	}
	SDL_UpdateTexture(texture_, nullptr, pixels_.data(), 640 * 4);
	SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
	SDL_RenderClear(renderer_);
	SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);
	SDL_RenderPresent(renderer_);
}

void Host::presentText(const uint8_t *textMem) {
	(void)textMem;
	std::fill(pixels_.begin(), pixels_.end(), 0xFF000000u);
	SDL_UpdateTexture(texture_, nullptr, pixels_.data(), 640 * 4);
	SDL_RenderClear(renderer_);
	SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);
	SDL_RenderPresent(renderer_);
}

void Host::sleepMicros(uint32_t us) {
	SDL_Delay(std::max<uint32_t>(1, us / 1000));
}

void Host::warpMouse(int x, int y) {
	int wx, wy;
	SDL_RenderLogicalToWindow(renderer_, (float)x, (float)y, &wx, &wy);
	SDL_WarpMouseInWindow(window_, wx, wy);
}

// SDL scancode -> PC/XT scancode set 1 (0x80 flag = extended E0 key)
static uint8_t xtScancode(SDL_Scancode sc) {
	switch (sc) {
	case SDL_SCANCODE_ESCAPE: return 0x01;
	case SDL_SCANCODE_1: return 0x02;
	case SDL_SCANCODE_2: return 0x03;
	case SDL_SCANCODE_3: return 0x04;
	case SDL_SCANCODE_4: return 0x05;
	case SDL_SCANCODE_5: return 0x06;
	case SDL_SCANCODE_6: return 0x07;
	case SDL_SCANCODE_7: return 0x08;
	case SDL_SCANCODE_8: return 0x09;
	case SDL_SCANCODE_9: return 0x0A;
	case SDL_SCANCODE_0: return 0x0B;
	case SDL_SCANCODE_MINUS: return 0x0C;
	case SDL_SCANCODE_EQUALS: return 0x0D;
	case SDL_SCANCODE_BACKSPACE: return 0x0E;
	case SDL_SCANCODE_TAB: return 0x0F;
	case SDL_SCANCODE_Q: return 0x10;
	case SDL_SCANCODE_W: return 0x11;
	case SDL_SCANCODE_E: return 0x12;
	case SDL_SCANCODE_R: return 0x13;
	case SDL_SCANCODE_T: return 0x14;
	case SDL_SCANCODE_Y: return 0x15;
	case SDL_SCANCODE_U: return 0x16;
	case SDL_SCANCODE_I: return 0x17;
	case SDL_SCANCODE_O: return 0x18;
	case SDL_SCANCODE_P: return 0x19;
	case SDL_SCANCODE_LEFTBRACKET: return 0x1A;
	case SDL_SCANCODE_RIGHTBRACKET: return 0x1B;
	case SDL_SCANCODE_RETURN: return 0x1C;
	case SDL_SCANCODE_LCTRL: return 0x1D;
	case SDL_SCANCODE_A: return 0x1E;
	case SDL_SCANCODE_S: return 0x1F;
	case SDL_SCANCODE_D: return 0x20;
	case SDL_SCANCODE_F: return 0x21;
	case SDL_SCANCODE_G: return 0x22;
	case SDL_SCANCODE_H: return 0x23;
	case SDL_SCANCODE_J: return 0x24;
	case SDL_SCANCODE_K: return 0x25;
	case SDL_SCANCODE_L: return 0x26;
	case SDL_SCANCODE_SEMICOLON: return 0x27;
	case SDL_SCANCODE_APOSTROPHE: return 0x28;
	case SDL_SCANCODE_GRAVE: return 0x29;
	case SDL_SCANCODE_LSHIFT: return 0x2A;
	case SDL_SCANCODE_BACKSLASH: return 0x2B;
	case SDL_SCANCODE_Z: return 0x2C;
	case SDL_SCANCODE_X: return 0x2D;
	case SDL_SCANCODE_C: return 0x2E;
	case SDL_SCANCODE_V: return 0x2F;
	case SDL_SCANCODE_B: return 0x30;
	case SDL_SCANCODE_N: return 0x31;
	case SDL_SCANCODE_M: return 0x32;
	case SDL_SCANCODE_COMMA: return 0x33;
	case SDL_SCANCODE_PERIOD: return 0x34;
	case SDL_SCANCODE_SLASH: return 0x35;
	case SDL_SCANCODE_RSHIFT: return 0x36;
	case SDL_SCANCODE_KP_MULTIPLY: return 0x37;
	case SDL_SCANCODE_LALT: return 0x38;
	case SDL_SCANCODE_SPACE: return 0x39;
	case SDL_SCANCODE_CAPSLOCK: return 0x3A;
	case SDL_SCANCODE_F1: return 0x3B;
	case SDL_SCANCODE_F2: return 0x3C;
	case SDL_SCANCODE_F3: return 0x3D;
	case SDL_SCANCODE_F4: return 0x3E;
	case SDL_SCANCODE_F5: return 0x3F;
	case SDL_SCANCODE_F6: return 0x40;
	case SDL_SCANCODE_F7: return 0x41;
	case SDL_SCANCODE_F8: return 0x42;
	case SDL_SCANCODE_F9: return 0x43;
	case SDL_SCANCODE_F10: return 0x44;
	case SDL_SCANCODE_NUMLOCKCLEAR: return 0x45;
	case SDL_SCANCODE_SCROLLLOCK: return 0x46;
	case SDL_SCANCODE_KP_7: return 0x47;
	case SDL_SCANCODE_KP_8: return 0x48;
	case SDL_SCANCODE_KP_9: return 0x49;
	case SDL_SCANCODE_KP_MINUS: return 0x4A;
	case SDL_SCANCODE_KP_4: return 0x4B;
	case SDL_SCANCODE_KP_5: return 0x4C;
	case SDL_SCANCODE_KP_6: return 0x4D;
	case SDL_SCANCODE_KP_PLUS: return 0x4E;
	case SDL_SCANCODE_KP_1: return 0x4F;
	case SDL_SCANCODE_KP_2: return 0x50;
	case SDL_SCANCODE_KP_3: return 0x51;
	case SDL_SCANCODE_KP_0: return 0x52;
	case SDL_SCANCODE_KP_PERIOD: return 0x53;
	case SDL_SCANCODE_NONUSBACKSLASH: return 0x56;
	case SDL_SCANCODE_F11: return 0x57;
	case SDL_SCANCODE_F12: return 0x58;
	case SDL_SCANCODE_KP_ENTER: return 0x80 | 0x1C;
	case SDL_SCANCODE_RCTRL: return 0x80 | 0x1D;
	case SDL_SCANCODE_RALT: return 0x80 | 0x38;
	case SDL_SCANCODE_HOME: return 0x80 | 0x47;
	case SDL_SCANCODE_UP: return 0x80 | 0x48;
	case SDL_SCANCODE_PAGEUP: return 0x80 | 0x49;
	case SDL_SCANCODE_LEFT: return 0x80 | 0x4B;
	case SDL_SCANCODE_RIGHT: return 0x80 | 0x4D;
	case SDL_SCANCODE_END: return 0x80 | 0x4F;
	case SDL_SCANCODE_DOWN: return 0x80 | 0x50;
	case SDL_SCANCODE_PAGEDOWN: return 0x80 | 0x51;
	case SDL_SCANCODE_INSERT: return 0x80 | 0x52;
	case SDL_SCANCODE_DELETE: return 0x80 | 0x53;
	default: return 0;
	}
}

static uint8_t asciiFor(const SDL_Keysym &k) {
	SDL_Keycode c = k.sym;
	bool shift = (k.mod & KMOD_SHIFT) != 0;
	if (c >= 'a' && c <= 'z')
		return (uint8_t)(shift ? c - 32 : c);
	if (c >= ' ' && c < 127)
		return (uint8_t)c;
	switch (c) {
	case SDLK_RETURN: return 13;
	case SDLK_BACKSPACE: return 8;
	case SDLK_ESCAPE: return 27;
	case SDLK_TAB: return 9;
	default: return 0;
	}
}

void Host::saveShot(const std::string &path) {
	SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormatFrom(pixels_.data(), 640, 480, 32, 640 * 4,
	                                                       SDL_PIXELFORMAT_ARGB8888);
	if (surf) {
		SDL_SaveBMP(surf, path.c_str());
		SDL_FreeSurface(surf);
	}
}

// Script lines: "<ms> move X Y", "<ms> click X Y [button]", "<ms> key SCANCODE",
// "<ms> shot NAME", "<ms> quit"; times are relative to program start.
void Host::loadInputScript(const std::string &path) {
	std::ifstream in(fsu::path(path));
	std::string line;
	while (std::getline(in, line)) {
		std::istringstream ls(line);
		ScriptEvent e;
		if (!(ls >> e.ms >> e.kind))
			continue;
		if (e.kind == "shot") {
			ls >> inputScript;
			e.a = (int)script_.size();
		}
		std::string a, b, c;
		ls >> a >> b >> c;
		e.a = a.empty() ? 0 : (int)strtol(a.c_str(), nullptr, 0);
		e.b = b.empty() ? 0 : (int)strtol(b.c_str(), nullptr, 0);
		e.c = c.empty() ? 0 : (int)strtol(c.c_str(), nullptr, 0);
		script_.push_back(e);
		if (e.kind == "shot")
			script_.back().kind = "shot:" + inputScript;
	}
	inputScript.clear();
	startMs_ = SDL_GetTicks();
}

void Host::runScript(Machine &m) {
	uint32_t now = SDL_GetTicks() - startMs_;
	while (scriptPos_ < script_.size() && script_[scriptPos_].ms <= now) {
		const ScriptEvent &e = script_[scriptPos_++];
		if (e.kind == "move") {
			m.mouseMotion(e.a, e.b);
		} else if (e.kind == "click") {
			m.mouseMotion(e.a, e.b);
			m.mouseButton(e.c, true);
			script_.insert(script_.begin() + scriptPos_, ScriptEvent{e.ms + 80, "release", e.a, e.b, e.c});
		} else if (e.kind == "down") {
			m.mouseMotion(e.a, e.b);
			m.mouseButton(e.c, true);
		} else if (e.kind == "up") {
			m.mouseMotion(e.a, e.b);
			m.mouseButton(e.c, false);
		} else if (e.kind == "release") {
			m.mouseButton(e.c, false);
		} else if (e.kind == "key") {
			m.keyEvent((uint8_t)e.a, true, (uint16_t)(e.a << 8));
			script_.insert(script_.begin() + scriptPos_, ScriptEvent{e.ms + 80, "keyup", e.a, 0, 0});
		} else if (e.kind == "keyup") {
			m.keyEvent((uint8_t)e.a, false, 0);
		} else if (e.kind.rfind("shot:", 0) == 0) {
			saveShot(shotDir + "/" + e.kind.substr(5) + ".bmp");
		} else if (e.kind == "poke") {
			// poke SEG OFF VALUE (NE segment number, offset, byte)
			if (e.a > 0 && e.a < 64)
				m.write8(m.selBase(m.segSel[e.a]) + (uint32_t)e.b, (uint8_t)e.c);
		} else if (e.kind == "quit") {
			quitRequested = true;
		}
	}
}

void Host::pollEvents(Machine &m) {
	if (!script_.empty())
		runScript(m);
	SDL_Event e;
	while (SDL_PollEvent(&e)) {
		switch (e.type) {
		case SDL_QUIT:
			quitRequested = true;
			break;
		case SDL_KEYDOWN:
		case SDL_KEYUP: {
			const SDL_Keysym &k = e.key.keysym;
			if (e.type == SDL_KEYDOWN && k.sym == SDLK_RETURN && (k.mod & KMOD_ALT)) {
				toggleFullscreen();
				break;
			}
			if (e.key.repeat)
				break;
			uint8_t sc = xtScancode(k.scancode);
			if (!sc)
				break;
			uint16_t bios = (uint16_t)(((sc & 0x7F) << 8) | asciiFor(k));
			m.keyEvent(sc, e.type == SDL_KEYDOWN, bios);
			break;
		}
		case SDL_MOUSEMOTION:
			m.mouseMotion(std::clamp(e.motion.x, 0, 639), std::clamp(e.motion.y, 0, 479));
			break;
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP: {
			int b = e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_RIGHT ? 1 : 2;
			m.mouseMotion(std::clamp(e.button.x, 0, 639), std::clamp(e.button.y, 0, 479));
			m.mouseButton(b, e.type == SDL_MOUSEBUTTONDOWN);
			break;
		}
		default:
			break;
		}
	}
}

// ------------------------------------------------------------------ audio

void Host::playPcm(const uint8_t *data, int len, uint32_t rate) {
	if (audioDev_)
		SDL_LockAudioDevice(audioDev_);
	pcm_.assign(data, data + len);
	pcmPos_ = 0;
	pcmStep_ = ((uint64_t)rate << 16) / OUT_RATE;
	pcmPaused_ = false;
	if (audioDev_)
		SDL_UnlockAudioDevice(audioDev_);
}

void Host::stopPcm() {
	if (audioDev_)
		SDL_LockAudioDevice(audioDev_);
	pcm_.clear();
	pcmPos_ = 0;
	if (audioDev_)
		SDL_UnlockAudioDevice(audioDev_);
}

void Host::pausePcm(bool p) {
	if (audioDev_)
		SDL_LockAudioDevice(audioDev_);
	pcmPaused_ = p;
	if (audioDev_)
		SDL_UnlockAudioDevice(audioDev_);
}

void Host::audioCallback(void *user, uint8_t *stream, int len) {
	auto *h = static_cast<Host *>(user);
	auto *out = reinterpret_cast<int16_t *>(stream);
	int frames = len / 4;
	memset(stream, 0, (size_t)len);
	h->mix(out, frames);
}

void Host::mix(int16_t *out, int frames) {
	if (cd_)
		cd_->render(out, frames, opt_.cdVolume * 256 / 100);
	if (midi_)
		midi_->render(out, frames);
	if (!pcm_.empty() && !pcmPaused_) {
		size_t n = pcm_.size();
		for (int i = 0; i < frames; i++) {
			size_t idx = (size_t)(pcmPos_ >> 16);
			if (idx >= n) {
				pcm_.clear();
				break;
			}
			int s = ((int)pcm_[idx] - 128) * 256 * opt_.sfxVolume / 100;
			int l = out[i * 2] + s, r = out[i * 2 + 1] + s;
			out[i * 2] = (int16_t)std::clamp(l, -32768, 32767);
			out[i * 2 + 1] = (int16_t)std::clamp(r, -32768, 32767);
			pcmPos_ += pcmStep_;
		}
	}
}
