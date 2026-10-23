// The emulated machine: memory, descriptor table, interrupt vectors and the
// high-level emulation of DOS, DPMI, the KERNEL/RTM imports and the PC
// hardware the game talks to.
#pragma once

#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "cpu.h"

class Host;
class Vfs;
class CdAudio;
class Midi;

struct Descriptor {
	bool used = false;
	uint32_t base = 0;
	uint32_t limit = 0;
	uint8_t access = 0xF2; // present, DPL3, data r/w
	std::string name;
};

struct DosFile {
	FILE *fp = nullptr;
	std::string path;
	bool writable = false;
};

class Machine {
public:
	explicit Machine(Host &host, Vfs &vfs, CdAudio &cd, Midi &midi);
	~Machine();

	// ---- memory
	static constexpr uint32_t MEM_SIZE = 64u << 20;
	std::vector<uint8_t> mem;

	uint8_t read8(uint32_t a) const { return a < MEM_SIZE ? mem[a] : 0xFF; }
	uint16_t read16(uint32_t a) const {
		if (a + 1 < MEM_SIZE) {
			uint16_t v;
			memcpy(&v, &mem[a], 2);
			return v;
		}
		return 0xFFFF;
	}
	uint32_t read32(uint32_t a) const {
		if (a + 3 < MEM_SIZE) {
			uint32_t v;
			memcpy(&v, &mem[a], 4);
			return v;
		}
		return 0xFFFFFFFF;
	}
	void write8(uint32_t a, uint8_t v) {
		if (a < MEM_SIZE)
			mem[a] = v;
	}
	void write16(uint32_t a, uint16_t v) {
		if (a + 1 < MEM_SIZE)
			memcpy(&mem[a], &v, 2);
	}
	void write32(uint32_t a, uint32_t v) {
		if (a + 3 < MEM_SIZE)
			memcpy(&mem[a], &v, 4);
	}

	// linear allocator for extended memory (above 1 MB)
	uint32_t allocLinear(uint32_t size);
	void freeLinear(uint32_t addr);
	uint32_t linearBlockSize(uint32_t addr) const;
	// conventional memory (below 640 KB) in paragraphs
	uint32_t allocDos(uint32_t size);
	void freeDos(uint32_t addr);

	// ---- descriptors
	std::vector<Descriptor> ldt;
	uint16_t allocSelectors(int count);
	void freeSelector(uint16_t sel);
	void setDescriptor(uint16_t sel, uint32_t base, uint32_t limit, bool code = false, const std::string &name = "");
	bool descriptor(uint16_t sel, uint32_t &base, uint32_t &limit) const;
	uint8_t descriptorAccess(uint16_t sel) const;
	std::string selName(uint16_t sel) const;
	uint32_t selBase(uint16_t sel) const;
	uint32_t farToLinear(uint16_t sel, uint16_t off) const { return selBase(sel) + off; }
	// selector for a real-mode segment (cached)
	uint16_t realSegSelector(uint16_t seg);

	// ---- interrupts
	struct Vector {
		uint16_t sel = 0;
		uint32_t off = 0;
	};
	std::array<Vector, 256> pmVectors;
	std::array<Vector, 256> defaultVectors;
	void getVector(int n, uint16_t &sel, uint32_t &off) const;
	void softInt(int n);
	void trap(uint16_t id);
	void hlt();
	void raiseIrq(int irq);

	// ---- host traps
	using TrapFn = std::function<void()>;
	uint16_t trapSel = 0;
	uint16_t trapNext = 0;
	std::vector<TrapFn> traps;
	// create a stub "F1 id; <tail>" in the trap segment, returns its offset
	uint16_t makeStub(TrapFn fn, const std::vector<uint8_t> &tail);

	// ---- ports
	uint8_t in8(uint16_t port);
	void out8(uint16_t port, uint8_t v);
	uint16_t in16(uint16_t port);
	void out16(uint16_t port, uint16_t v);

	// ---- program
	bool loadNE(const std::string &path);
	uint16_t segSel[64] = {};
	int numSegs = 0;
	uint16_t pspSel = 0, envSel = 0;
	std::vector<uint8_t> exeImage;

	// ---- execution
	Cpu cpu;
	bool quit = false;
	int exitCode = 0;
	bool runSlice(int instructions); // true if the guest was busy-waiting
	void service(); // timers, input, IRQs, display

	// ---- diagnostics
	void log(const char *fmt, ...);
	bool trace = false;
	// "seg:off" locations (NE segment numbers) that log the CPU state when executed
	std::vector<std::pair<uint16_t, uint16_t>> breakpoints;
	void addBreakpoints(const std::string &spec);
	void dumpState(const char *why);
	bool profile = false;
	std::map<uint32_t, uint64_t> profileHits;
	void printProfile();

	// ---- callbacks into guest code (mouse handler)
	struct SavedState {
		uint32_t r[8];
		SegReg s[6];
		uint32_t eip, flags;
	};
	std::vector<SavedState> callbackStack;
	uint16_t callbackRetOff = 0;
	void callGuestFar(uint16_t sel, uint16_t off);

	// ---- hardware state
	// PIC
	uint8_t picMask[2] = {0, 0};
	uint8_t picIsr[2] = {0, 0};
	uint8_t picIrr[2] = {0, 0};
	bool picReadIsr[2] = {false, false};
	void picEoi(int pic);
	void deliverIrqs();

	// PIT
	uint16_t pitDivisor[3] = {0, 0, 0};
	uint8_t pitMode[3] = {0, 0, 0};
	bool pitLowByte[3] = {true, true, true};
	uint16_t pitLatch[3] = {0, 0, 0};
	bool pitLatched[3] = {false, false, false};
	bool pitReadLow[3] = {true, true, true};
	double pitAccum = 0.0;
	double biosAccum = 0.0;
	uint64_t lastServiceUs = 0;
	uint64_t startUs = 0;
	uint64_t nowUs() const;

	// keyboard
	std::deque<uint8_t> kbdQueue;
	uint8_t kbdData = 0;
	bool kbdPending = false;
	std::deque<uint16_t> biosKeys;
	void keyEvent(uint8_t scancode, bool down, uint16_t biosKey);

	// mouse
	static constexpr int MOUSE_SCALE = 8;
	int mouseX = 0, mouseY = 0; // driver coordinates
	int mouseMinX = 0, mouseMaxX = 639, mouseMinY = 0, mouseMaxY = 199;
	int mouseButtons = 0;
	int mouseEvents = 0;
	uint16_t mouseHandlerMask = 0;
	uint16_t mouseHandlerSel = 0, mouseHandlerOff = 0;
	int mouseMickeyX = 8, mouseMickeyY = 16;
	bool mouseInHandler = false;
	void mouseMotion(int hostX, int hostY);
	void mouseButton(int button, bool down);
	void dispatchMouse();

	// video
	static constexpr int SCREEN_W = 640, SCREEN_H = 480;
	std::vector<uint8_t> vram; // 1 MB banked video memory
	int vesaBank = 0;
	int vesaMode = 3;
	uint8_t dac[256][3] = {};
	int dacWriteIdx = 0, dacReadIdx = 0, dacComp = 0, dacReadComp = 0;
	uint8_t dacMask = 0xFF;
	bool screenOff = false;
	void setBank(int bank);
	void syncWindow(); // copy window memory into vram
	uint64_t lastPresentUs = 0;
	void present();

	// Sound Blaster
	uint16_t sbBase = 0x220;
	int sbIrq = 7, sbDma = 1;
	std::deque<uint8_t> sbOut;
	std::vector<uint8_t> sbCmd;
	int sbCmdLen = 0;
	uint8_t sbCurCmd = 0;
	bool sbResetting = false;
	int sbTimeConst = 0;
	uint32_t sbRate = 11025;
	bool sbSpeaker = false;
	uint64_t sbIrqAt = 0;
	bool sbIrqPending = false;
	int sbDmaLen = 0;
	void sbWrite(uint8_t v);
	void sbStartDma(int len);

	// DMA controller (8-bit channels)
	uint16_t dmaAddr[4] = {}, dmaCount[4] = {};
	uint8_t dmaPage[4] = {};
	bool dmaFlip = false;
	uint8_t dmaModeReg[4] = {};

	// MPU-401
	uint16_t mpuBase = 0x330;
	bool mpuUart = false;
	std::deque<uint8_t> mpuIn;

	// OPL (AdLib) - register writes forwarded to the synth
	uint8_t oplIndex = 0;

	// BIOS data
	uint32_t biosTicks = 0;

	// DOS state
	std::map<int, DosFile> files;
	uint32_t dtaLinear = 0;
	std::string findPattern;
	std::vector<std::string> findResults;
	size_t findPos = 0;
	uint16_t dosAllocStrategy = 0;

	// MSCDEX
	void mscdexRequest(uint32_t reqLinear);
	void realModeInt(uint32_t regsLinear, int intno);

	Host &host;
	Vfs &vfs;
	CdAudio &cd;
	Midi &midi;

	// idle detection
	uint64_t idleSpins = 0;
	void idleHint();

private:
	struct Block {
		uint32_t addr, size;
		bool used;
	};
	std::vector<Block> heap;
	std::vector<Block> dosHeap;
	std::map<uint16_t, uint16_t> realSelCache;

	void setupMemory();
	void setupTraps();
	void setupImports(std::map<std::pair<std::string, int>, std::pair<uint16_t, uint16_t>> &imports);

	// HLE handlers
	void int21();
	void int31();
	void int2f();
	void int10();
	void int16();
	void int1a();
	void int33();
	void kernelImport(int ordinal);
	void rtmImport(int ordinal);

	bool dosOpen(const std::string &name, int mode, bool create, int &handle);
	std::string readAsciiz(uint16_t sel, uint16_t off);
	std::string readAsciizLinear(uint32_t lin);
};
