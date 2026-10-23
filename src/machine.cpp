#include "machine.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "host.h"
#include "vfs.h"

static constexpr uint32_t TRAP_LINEAR = 0x100000;
static constexpr uint32_t HEAP_START = 0x120000;
static constexpr uint32_t DOS_HEAP_START = 0x2000;
static constexpr uint32_t DOS_HEAP_END = 0xA0000;

Machine::Machine(Host &h, Vfs &v, CdAudio &c, Midi &mi)
	: cpu(*this), host(h), vfs(v), cd(c), midi(mi) {
	mem.assign(MEM_SIZE, 0);
	vram.assign(1 << 20, 0);
	ldt.resize(8192);
	startUs = nowUs();
	lastServiceUs = startUs;
	setupMemory();
	setupTraps();
}

Machine::~Machine() {
	for (auto &f : files)
		if (f.second.fp)
			fclose(f.second.fp);
}

uint64_t Machine::nowUs() const {
	using namespace std::chrono;
	return (uint64_t)duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

void Machine::log(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "[hw] ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
}

// ------------------------------------------------------------------ memory

void Machine::setupMemory() {
	heap.push_back({HEAP_START, MEM_SIZE - HEAP_START, false});
	dosHeap.push_back({DOS_HEAP_START, DOS_HEAP_END - DOS_HEAP_START, false});

	// BIOS data area
	write16(0x410, 0x0026);       // equipment
	write16(0x413, 640);          // conventional memory KB
	write16(0x41A, 0x1E);         // keyboard buffer head
	write16(0x41C, 0x1E);         // tail
	write8(0x449, 3);             // video mode
	write16(0x44A, 80);
	write16(0x463, 0x3D4);
	write8(0x484, 24);
}

static uint32_t align16(uint32_t v) { return (v + 15) & ~15u; }

uint32_t Machine::allocLinear(uint32_t size) {
	size = align16(std::max<uint32_t>(size, 16));
	for (size_t i = 0; i < heap.size(); i++) {
		Block &b = heap[i];
		if (!b.used && b.size >= size) {
			uint32_t addr = b.addr;
			if (b.size > size) {
				Block rest{b.addr + size, b.size - size, false};
				b.size = size;
				heap.insert(heap.begin() + i + 1, rest);
			}
			heap[i].used = true;
			memset(&mem[addr], 0, size);
			return addr;
		}
	}
	log("out of extended memory (%u bytes)", size);
	return 0;
}

void Machine::freeLinear(uint32_t addr) {
	for (size_t i = 0; i < heap.size(); i++) {
		if (heap[i].addr == addr && heap[i].used) {
			heap[i].used = false;
			if (i + 1 < heap.size() && !heap[i + 1].used) {
				heap[i].size += heap[i + 1].size;
				heap.erase(heap.begin() + i + 1);
			}
			if (i > 0 && !heap[i - 1].used) {
				heap[i - 1].size += heap[i].size;
				heap.erase(heap.begin() + i);
			}
			return;
		}
	}
}

uint32_t Machine::linearBlockSize(uint32_t addr) const {
	for (auto &b : heap)
		if (b.addr == addr)
			return b.size;
	return 0;
}

uint32_t Machine::allocDos(uint32_t size) {
	size = align16(std::max<uint32_t>(size, 16));
	for (size_t i = 0; i < dosHeap.size(); i++) {
		Block &b = dosHeap[i];
		if (!b.used && b.size >= size) {
			uint32_t addr = b.addr;
			if (b.size > size) {
				Block rest{b.addr + size, b.size - size, false};
				b.size = size;
				dosHeap.insert(dosHeap.begin() + i + 1, rest);
			}
			dosHeap[i].used = true;
			memset(&mem[addr], 0, size);
			return addr;
		}
	}
	return 0;
}

void Machine::freeDos(uint32_t addr) {
	for (size_t i = 0; i < dosHeap.size(); i++) {
		if (dosHeap[i].addr == addr && dosHeap[i].used) {
			dosHeap[i].used = false;
			if (i + 1 < dosHeap.size() && !dosHeap[i + 1].used) {
				dosHeap[i].size += dosHeap[i + 1].size;
				dosHeap.erase(dosHeap.begin() + i + 1);
			}
			if (i > 0 && !dosHeap[i - 1].used) {
				dosHeap[i - 1].size += dosHeap[i].size;
				dosHeap.erase(dosHeap.begin() + i);
			}
			return;
		}
	}
}

// ------------------------------------------------------------------ descriptors

uint16_t Machine::allocSelectors(int count) {
	// index 0 is never used; selectors are (index << 3) | 7 (LDT, RPL 3)
	for (size_t i = 16; i + count <= ldt.size(); i++) {
		bool ok = true;
		for (int j = 0; j < count; j++)
			if (ldt[i + j].used) {
				ok = false;
				break;
			}
		if (ok) {
			for (int j = 0; j < count; j++) {
				ldt[i + j] = Descriptor();
				ldt[i + j].used = true;
			}
			return (uint16_t)((i << 3) | 7);
		}
	}
	log("out of selectors");
	return 0;
}

void Machine::freeSelector(uint16_t sel) {
	size_t i = sel >> 3;
	if (i > 0 && i < ldt.size())
		ldt[i].used = false;
}

void Machine::setDescriptor(uint16_t sel, uint32_t base, uint32_t limit, bool code, const std::string &name) {
	size_t i = sel >> 3;
	if (i == 0 || i >= ldt.size())
		return;
	ldt[i].used = true;
	ldt[i].base = base;
	ldt[i].limit = limit;
	ldt[i].access = code ? 0xFA : 0xF2;
	if (!name.empty())
		ldt[i].name = name;
}

bool Machine::descriptor(uint16_t sel, uint32_t &base, uint32_t &limit) const {
	size_t i = sel >> 3;
	if (i == 0 || i >= ldt.size() || !ldt[i].used)
		return false;
	base = ldt[i].base;
	limit = ldt[i].limit;
	return true;
}

uint8_t Machine::descriptorAccess(uint16_t sel) const {
	size_t i = sel >> 3;
	if (i == 0 || i >= ldt.size())
		return 0;
	return ldt[i].access;
}

uint32_t Machine::selBase(uint16_t sel) const {
	size_t i = sel >> 3;
	if (i == 0 || i >= ldt.size())
		return 0;
	return ldt[i].base;
}

std::string Machine::selName(uint16_t sel) const {
	size_t i = sel >> 3;
	if (i < ldt.size() && !ldt[i].name.empty())
		return ldt[i].name;
	char buf[16];
	snprintf(buf, sizeof(buf), "%04X", sel);
	return buf;
}

uint16_t Machine::realSegSelector(uint16_t seg) {
	auto it = realSelCache.find(seg);
	if (it != realSelCache.end())
		return it->second;
	uint16_t sel = allocSelectors(1);
	setDescriptor(sel, (uint32_t)seg << 4, 0xFFFF);
	realSelCache[seg] = sel;
	return sel;
}

// ------------------------------------------------------------------ traps / vectors

uint16_t Machine::makeStub(TrapFn fn, const std::vector<uint8_t> &tail) {
	uint16_t off = trapNext;
	uint16_t id = (uint16_t)traps.size();
	traps.push_back(std::move(fn));
	uint32_t a = TRAP_LINEAR + off;
	mem[a] = 0xF1;
	mem[a + 1] = id & 0xFF;
	mem[a + 2] = id >> 8;
	for (size_t i = 0; i < tail.size(); i++)
		mem[a + 3 + i] = tail[i];
	trapNext = (uint16_t)(off + 3 + tail.size() + 1);
	return off;
}

void Machine::trap(uint16_t id) {
	if (id < traps.size()) {
		traps[id]();
	} else {
		log("bad trap %u", id);
		quit = true;
	}
}

void Machine::getVector(int n, uint16_t &sel, uint32_t &off) const {
	sel = pmVectors[n].sel;
	off = pmVectors[n].off;
}

// Default handler stubs: software interrupts emulated by the host return
// with "retf 2" so that the flags set by the handler survive, after the
// interrupt flag of the caller has been restored.
void Machine::setupTraps() {
	trapSel = allocSelectors(1);
	setDescriptor(trapSel, TRAP_LINEAR, 0xFFFF, true, "HOST");
	trapNext = 0;

	auto swInt = [this](std::function<void()> fn) {
		return [this, fn]() {
			fn();
			// restore IF from the flags image pushed by INT
			uint16_t fl = cpu.stackWord(2);
			cpu.flags = (cpu.flags & ~F_IF) | (fl & F_IF);
		};
	};
	const std::vector<uint8_t> retf2 = {0xCA, 0x02, 0x00};
	const std::vector<uint8_t> iretT = {0xCF};

	for (int n = 0; n < 256; n++) {
		TrapFn fn;
		std::vector<uint8_t> tail = retf2;
		switch (n) {
		case 0x10: fn = swInt([this]() { int10(); }); break;
		case 0x16: fn = swInt([this]() { int16(); }); break;
		case 0x1A: fn = swInt([this]() { int1a(); }); break;
		case 0x21: fn = swInt([this]() { int21(); }); break;
		case 0x2F: fn = swInt([this]() { int2f(); }); break;
		case 0x31: fn = swInt([this]() { int31(); }); break;
		case 0x33: fn = swInt([this]() { int33(); }); break;
		case 0x08:
			fn = [this]() {
				biosTicks++;
				write32(0x46C, biosTicks);
				picEoi(0);
			};
			tail = iretT;
			break;
		case 0x09:
			fn = [this]() {
				// BIOS keyboard handler: consume the scancode
				(void)in8(0x60);
				picEoi(0);
			};
			tail = iretT;
			break;
		default:
			if (n >= 0x08 && n <= 0x0F) {
				fn = [this]() { picEoi(0); };
				tail = iretT;
			} else if (n >= 0x70 && n <= 0x77) {
				fn = [this]() {
					picEoi(1);
					picEoi(0);
				};
				tail = iretT;
			} else {
				int nn = n;
				fn = [this, nn]() {
					if (nn != 0x1C && nn != 0x23 && nn != 0x24 && nn != 0x28)
						log("unhandled interrupt %02X at %s", nn, cpu.where().c_str());
				};
				tail = iretT;
			}
			break;
		}
		uint16_t off = makeStub(fn, tail);
		pmVectors[n] = {trapSel, off};
		defaultVectors[n] = pmVectors[n];
	}

	// return point for host-initiated far calls into guest code
	callbackRetOff = makeStub([this]() {
		if (callbackStack.empty())
			return;
		SavedState st = callbackStack.back();
		callbackStack.pop_back();
		for (int i = 0; i < 8; i++)
			cpu.r[i] = st.r[i];
		for (int i = 0; i < 6; i++)
			cpu.s[i] = st.s[i];
		cpu.eip = st.eip;
		cpu.flags = st.flags;
		mouseInHandler = false;
	}, {});
}

void Machine::softInt(int n) {
	cpu.doInterrupt(n, true);
}

void Machine::callGuestFar(uint16_t sel, uint16_t off) {
	SavedState st;
	for (int i = 0; i < 8; i++)
		st.r[i] = cpu.r[i];
	for (int i = 0; i < 6; i++)
		st.s[i] = cpu.s[i];
	st.eip = cpu.eip;
	st.flags = cpu.flags;
	callbackStack.push_back(st);
	// use a fresh area below the current stack
	cpu.set16(R_SP, (uint16_t)(cpu.sp() - 64));
	cpu.push16(trapSel);
	cpu.push16(callbackRetOff);
	cpu.flags &= ~F_IF;
	cpu.loadSeg(S_CS, sel);
	cpu.eip = off;
}

void Machine::hlt() {
	idleHint();
}

void Machine::idleHint() {
	// the guest is waiting for an interrupt: let real time pass
	host.sleepMicros(500);
	service();
}

// ------------------------------------------------------------------ PIC / IRQ

void Machine::raiseIrq(int irq) {
	if (irq < 8)
		picIrr[0] |= 1 << irq;
	else {
		picIrr[1] |= 1 << (irq - 8);
		picIrr[0] |= 1 << 2;
	}
}

void Machine::picEoi(int pic) {
	for (int i = 0; i < 8; i++) {
		if (picIsr[pic] & (1 << i)) {
			picIsr[pic] &= ~(1 << i);
			return;
		}
	}
}

void Machine::deliverIrqs() {
	if (!(cpu.flags & F_IF) || mouseInHandler)
		return;
	for (int irq = 0; irq < 8; irq++) {
		uint8_t bit = 1 << irq;
		if (picIsr[0] & ((bit << 1) - 1)) // higher or same priority in service
			return;
		if ((picIrr[0] & bit) && !(picMask[0] & bit)) {
			if (irq == 2) {
				for (int j = 0; j < 8; j++) {
					uint8_t b2 = 1 << j;
					if (picIsr[1] & ((b2 << 1) - 1))
						return;
					if ((picIrr[1] & b2) && !(picMask[1] & b2)) {
						picIrr[1] &= ~b2;
						if (!picIrr[1])
							picIrr[0] &= ~bit;
						picIsr[1] |= b2;
						picIsr[0] |= bit;
						cpu.doInterrupt(0x70 + j, false);
						return;
					}
				}
				picIrr[0] &= ~bit;
				continue;
			}
			picIrr[0] &= ~bit;
			picIsr[0] |= bit;
			cpu.doInterrupt(0x08 + irq, false);
			return;
		}
	}
}

// ------------------------------------------------------------------ run loop

void Machine::addBreakpoints(const std::string &spec) {
	size_t p = 0;
	while (p < spec.size()) {
		size_t e = spec.find(',', p);
		std::string one = spec.substr(p, e == std::string::npos ? std::string::npos : e - p);
		unsigned seg = 0, off = 0;
		if (sscanf(one.c_str(), "%u:%x", &seg, &off) == 2 && seg > 0 && seg < 64)
			breakpoints.push_back({segSel[seg], (uint16_t)off});
		if (e == std::string::npos)
			break;
		p = e + 1;
	}
}

void Machine::printProfile() {
	std::vector<std::pair<uint64_t, uint32_t>> v;
	uint64_t total = 0;
	for (auto &p : profileHits) {
		v.push_back({p.second, p.first});
		total += p.second;
	}
	std::sort(v.rbegin(), v.rend());
	for (size_t i = 0; i < v.size() && i < 25; i++)
		log("%5.1f%%  %s:%04X", 100.0 * v[i].first / total, selName((uint16_t)(v[i].second >> 16)).c_str(),
		    v[i].second & 0xFFFF);
}

void Machine::dumpState(const char *why) {
	log("%s at %s  AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X BP=%04X SP=%04X DS=%s ES=%s FL=%04X",
	    why, cpu.where().c_str(), cpu.ax(), cpu.bx(), cpu.cx(), cpu.dx(), cpu.si(), cpu.di(), cpu.bp(), cpu.sp(),
	    selName(cpu.s[S_DS].sel).c_str(), selName(cpu.s[S_ES].sel).c_str(), cpu.flags & 0xFFFF);
	char buf[200];
	int n = 0;
	for (int i = 0; i < 12; i++)
		n += snprintf(buf + n, sizeof(buf) - n, " %04X", cpu.stackWord(i));
	log("  stack:%s", buf);
}

bool Machine::runSlice(int instructions) {
	cpu.tightLoops = 0;
	try {
		for (int i = 0; i < instructions && !quit; i++) {
			if (!breakpoints.empty()) {
				for (auto &b : breakpoints)
					if (b.first == cpu.s[S_CS].sel && b.second == (uint16_t)cpu.eip) {
						cpu.curCs = cpu.s[S_CS].sel;
						cpu.curIp = cpu.eip;
						dumpState("break");
					}
			}
			if (profile && (i & 63) == 0)
				profileHits[((uint32_t)cpu.s[S_CS].sel << 16) | (cpu.eip & 0xFFF0)]++;
			cpu.step();
			if ((i & 255) == 0) {
				deliverIrqs();
				if (cpu.tightLoops > 2000)
					return true; // guest is spinning: give the host a break
			}
		}
	} catch (const CpuException &e) {
		log("CPU: %s", e.msg.c_str());
		dumpState("fault");
		quit = true;
		exitCode = 3;
	}
	return false;
}

void Machine::service() {
	uint64_t now = nowUs();
	uint64_t dt = now - lastServiceUs;
	if (dt > 200000)
		dt = 200000; // avoid catching up huge gaps
	lastServiceUs = now;

	// PIT channel 0
	double div = pitDivisor[0] ? pitDivisor[0] : 65536.0;
	double rate = 1193182.0 / div;
	pitAccum += dt * rate / 1e6;
	if (pitAccum >= 1.0) {
		raiseIrq(0);
		pitAccum -= 1.0;
		if (pitAccum > 4.0)
			pitAccum = 4.0;
	}

	// Sound Blaster DMA completion
	if (sbIrqPending && now >= sbIrqAt) {
		sbIrqPending = false;
		raiseIrq(sbIrq);
	}

	// keyboard
	if (!kbdPending && !kbdQueue.empty() && !(picIsr[0] & 2)) {
		kbdData = kbdQueue.front();
		kbdQueue.pop_front();
		kbdPending = true;
		raiseIrq(1);
	}

	if (now - lastPresentUs >= 16666) {
		lastPresentUs = now;
		host.pollEvents(*this);
		present();
	}

	if (!mouseInHandler && mouseEvents && mouseHandlerSel && (cpu.flags & F_IF))
		dispatchMouse();

	deliverIrqs();
}

// ------------------------------------------------------------------ video

void Machine::setBank(int bank) {
	if (bank == vesaBank)
		return;
	syncWindow();
	vesaBank = bank & 15;
	memcpy(&mem[0xA0000], &vram[(size_t)vesaBank << 16], 0x10000);
}

void Machine::syncWindow() {
	memcpy(&vram[(size_t)vesaBank << 16], &mem[0xA0000], 0x10000);
}

void Machine::present() {
	if (vesaMode < 0x100) {
		host.presentText(&mem[0xB8000]);
		return;
	}
	syncWindow();
	host.present(vram.data(), dac, screenOff);
}

// ------------------------------------------------------------------ input

void Machine::keyEvent(uint8_t scancode, bool down, uint16_t biosKey) {
	if (scancode & 0x80) {
		kbdQueue.push_back(0xE0);
		scancode &= 0x7F;
	}
	kbdQueue.push_back(down ? scancode : (scancode | 0x80));
	if (down && biosKey)
		biosKeys.push_back(biosKey);
}

void Machine::mouseMotion(int hostX, int hostY) {
	// host coordinates are in 640x480 screen space; the game programs the
	// driver range in 1/8 pixel units (0..5112 x 0..3192) and divides by 8
	int x = hostX * MOUSE_SCALE;
	int y = hostY * MOUSE_SCALE;
	x = std::clamp(x, mouseMinX, mouseMaxX);
	y = std::clamp(y, mouseMinY, mouseMaxY);
	if (x != mouseX || y != mouseY) {
		mouseX = x;
		mouseY = y;
		mouseEvents |= 1;
	}
}

void Machine::mouseButton(int button, bool down) {
	int bit = 1 << button;
	if (down)
		mouseButtons |= bit;
	else
		mouseButtons &= ~bit;
	static const int evDown[3] = {2, 8, 32}, evUp[3] = {4, 16, 64};
	if (button < 3)
		mouseEvents |= down ? evDown[button] : evUp[button];
}

void Machine::dispatchMouse() {
	int ev = mouseEvents & mouseHandlerMask;
	mouseEvents = 0;
	if (!ev)
		return;
	mouseInHandler = true;
	callGuestFar(mouseHandlerSel, mouseHandlerOff);
	cpu.set16(R_AX, (uint16_t)ev);
	cpu.set16(R_BX, (uint16_t)mouseButtons);
	cpu.set16(R_CX, (uint16_t)mouseX);
	cpu.set16(R_DX, (uint16_t)mouseY);
	cpu.set16(R_SI, 0);
	cpu.set16(R_DI, 0);
}
