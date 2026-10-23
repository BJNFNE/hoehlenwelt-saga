// High-level emulation of the services HW.EXE uses: KERNEL/RTM imports,
// DOS (INT 21h), DPMI (INT 31h), video BIOS/VESA (INT 10h), keyboard BIOS,
// mouse driver (INT 33h) and MSCDEX (INT 2Fh).
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>

#include "fileutil.h"
#include "cdaudio.h"
#include "host.h"
#include "machine.h"
#include "vfs.h"

std::string Machine::readAsciizLinear(uint32_t lin) {
	std::string s;
	while (lin < MEM_SIZE && mem[lin] && s.size() < 256)
		s += (char)mem[lin++];
	return s;
}

std::string Machine::readAsciiz(uint16_t sel, uint16_t off) {
	return readAsciizLinear(selBase(sel) + off);
}

// ------------------------------------------------------------------ imports

void Machine::setupImports(std::map<std::pair<std::string, int>, std::pair<uint16_t, uint16_t>> &imports) {
	auto sel0040 = allocSelectors(1);
	setDescriptor(sel0040, 0x400, 0xFFFF, false, "BDA");
	auto selA000 = allocSelectors(1);
	setDescriptor(selA000, 0xA0000, 0xFFFF, false, "A000");
	auto selB000 = allocSelectors(1);
	setDescriptor(selB000, 0xB0000, 0xFFFF, false, "B000");
	auto selB800 = allocSelectors(1);
	setDescriptor(selB800, 0xB8000, 0xFFFF, false, "B800");
	realSelCache[0x0040] = sel0040;
	realSelCache[0xA000] = selA000;

	// constants: the "offset" of the import is its value
	imports[{"KERNEL", 114}] = {0, 8};
	imports[{"KERNEL", 193}] = {0, sel0040};
	imports[{"KERNEL", 174}] = {0, selA000};
	imports[{"KERNEL", 181}] = {0, selB000};
	imports[{"KERNEL", 182}] = {0, selB800};

	int kernelOrd[] = {15, 17, 18, 19, 21, 91, 184, 185};
	for (int ord : kernelOrd) {
		uint16_t off = makeStub([this, ord]() { kernelImport(ord); }, {});
		imports[{"KERNEL", ord}] = {trapSel, off};
	}
	int rtmOrd[] = {5, 6, 21, 35, 36};
	for (int ord : rtmOrd) {
		uint16_t off = makeStub([this, ord]() { rtmImport(ord); }, {});
		imports[{"RTM", ord}] = {trapSel, off};
	}
}

// Global memory handles are the first selector of the block.
void Machine::kernelImport(int ord) {
	auto arg = [this](int i) { return cpu.stackWord(2 + i); };
	switch (ord) {
	case 91: // InitTask
		cpu.set16(R_AX, 1);
		cpu.set16(R_BX, 0x81);
		cpu.set16(R_CX, 0x2000);
		cpu.set16(R_DX, 1);
		cpu.set16(R_SI, 0);
		cpu.set16(R_DI, cpu.s[S_DS].sel);
		cpu.loadSeg(S_ES, pspSel);
		cpu.farReturn(0);
		break;
	case 15: { // GlobalAlloc(flags, size)
		uint32_t size = arg(0) | ((uint32_t)arg(1) << 16);
		if (size == 0)
			size = 16;
		int n = (int)((size + 0xFFFF) >> 16);
		uint32_t lin = allocLinear(size);
		uint16_t sel = lin ? allocSelectors(n) : 0;
		for (int i = 0; i < n && sel; i++) {
			uint32_t rest = size - (uint32_t)i * 0x10000;
			setDescriptor((uint16_t)(sel + i * 8), lin + i * 0x10000, std::min<uint32_t>(rest, 0x10000) - 1);
		}
		if (sel && n > 0)
			ldt[sel >> 3].name = "";
		cpu.set16(R_AX, sel);
		cpu.farReturn(6);
		break;
	}
	case 17: { // GlobalFree(handle)
		uint16_t sel = arg(0);
		uint32_t lin = selBase(sel);
		uint32_t size = linearBlockSize(lin);
		int n = size ? (int)((size + 0xFFFF) >> 16) : 1;
		freeLinear(lin);
		for (int i = 0; i < n; i++)
			freeSelector((uint16_t)(sel + i * 8));
		cpu.set16(R_AX, 0);
		cpu.farReturn(2);
		break;
	}
	case 18: // GlobalLock
		cpu.set16(R_AX, 0);
		cpu.set16(R_DX, arg(0));
		cpu.farReturn(2);
		break;
	case 19: // GlobalUnlock
		cpu.set16(R_AX, 0);
		cpu.farReturn(2);
		break;
	case 21: // GlobalHandle(sel)
		cpu.set16(R_AX, arg(0));
		cpu.set16(R_DX, arg(0));
		cpu.farReturn(2);
		break;
	case 184: { // GlobalDosAlloc(size)
		uint32_t size = arg(0) | ((uint32_t)arg(1) << 16);
		uint32_t lin = allocDos(size);
		if (!lin) {
			cpu.set16(R_AX, 0);
			cpu.set16(R_DX, 0);
		} else {
			uint16_t sel = allocSelectors(1);
			setDescriptor(sel, lin, std::max<uint32_t>(size, 16) - 1);
			cpu.set16(R_AX, sel);
			cpu.set16(R_DX, (uint16_t)(lin >> 4));
		}
		cpu.farReturn(4);
		break;
	}
	case 185: { // GlobalDosFree(sel)
		uint16_t sel = arg(0);
		freeDos(selBase(sel));
		freeSelector(sel);
		cpu.set16(R_AX, 0);
		cpu.farReturn(2);
		break;
	}
	default:
		log("KERNEL.%d not implemented", ord);
		quit = true;
	}
}

void Machine::rtmImport(int ord) {
	auto arg = [this](int i) { return cpu.stackWord(2 + i); };
	switch (ord) {
	case 5: { // MemAllocateBlock(size, attributes, eventProc, var selector)
		uint32_t size = arg(5) | ((uint32_t)arg(6) << 16);
		uint16_t pOff = arg(0), pSel = arg(1);
		if (size == 0)
			size = 16;
		int n = (int)((size + 0xFFFF) >> 16);
		uint32_t lin = allocLinear(size);
		uint16_t sel = lin ? allocSelectors(n) : 0;
		for (int i = 0; i < n && sel; i++) {
			uint32_t rest = size - (uint32_t)i * 0x10000;
			setDescriptor((uint16_t)(sel + i * 8), lin + i * 0x10000, std::min<uint32_t>(rest, 0x10000) - 1);
		}
		write16(selBase(pSel) + pOff, sel);
		cpu.set16(R_AX, sel ? 0 : 8);
		cpu.farReturn(14);
		break;
	}
	case 6: { // MemFreeBlock(selector)
		uint16_t sel = arg(0);
		uint32_t lin = selBase(sel);
		uint32_t size = linearBlockSize(lin);
		int n = size ? (int)((size + 0xFFFF) >> 16) : 1;
		freeLinear(lin);
		for (int i = 0; i < n; i++)
			freeSelector((uint16_t)(sel + i * 8));
		cpu.set16(R_AX, 0);
		cpu.farReturn(2);
		break;
	}
	case 21: { // MemQueryFreeMem(heap, type) -> free bytes
		uint32_t freeBytes = 0;
		for (auto &b : heap)
			if (!b.used)
				freeBytes += b.size;
		cpu.set16(R_AX, (uint16_t)freeBytes);
		cpu.set16(R_DX, (uint16_t)(freeBytes >> 16));
		cpu.farReturn(4);
		break;
	}
	case 35: // MemInitSwapFile(name, size)
		cpu.set16(R_AX, 0);
		cpu.farReturn(6);
		break;
	case 36: // MemCloseSwapFile(delete)
		cpu.set16(R_AX, 0);
		cpu.farReturn(2);
		break;
	default:
		log("RTM.%d not implemented", ord);
		quit = true;
	}
}

// ------------------------------------------------------------------ DOS

static std::string upper(std::string s) {
	for (auto &c : s)
		c = (char)toupper((unsigned char)c);
	return s;
}

bool Machine::dosOpen(const std::string &name, int mode, bool create, int &handle) {
	bool write = create || (mode & 3) != 0;
	std::string path;
	FILE *fp = vfs.openHost(name, write, create, path);
	if (!fp)
		return false;
	int h = 5;
	while (files.count(h))
		h++;
	files[h] = {fp, path, write};
	handle = h;
	return true;
}

void Machine::int21() {
	uint8_t ah = cpu.ah();
	auto ok = [this]() { cpu.setCF(false); };
	auto fail = [this](uint16_t err) {
		cpu.set16(R_AX, err);
		cpu.setCF(true);
	};
	auto dsdx = [this]() { return selBase(cpu.s[S_DS].sel) + cpu.dx(); };

	switch (ah) {
	case 0x02:
		fputc(cpu.dl(), stderr);
		break;
	case 0x09: {
		uint32_t a = dsdx();
		while (a < MEM_SIZE && mem[a] != '$')
			fputc(mem[a++], stderr);
		break;
	}
	case 0x0B:
		cpu.setAL(0);
		break;
	case 0x0E:
		cpu.setAL(26);
		break;
	case 0x19:
		cpu.setAL(2);
		break;
	case 0x1A:
		dtaLinear = dsdx();
		break;
	case 0x25: {
		int n = cpu.al();
		pmVectors[n] = {cpu.s[S_DS].sel, cpu.dx()};
		break;
	}
	case 0x2A: {
		time_t t = time(nullptr);
		struct tm *tm = localtime(&t);
		cpu.set16(R_CX, (uint16_t)(tm->tm_year + 1900));
		cpu.r[R_DX] = (cpu.r[R_DX] & 0xFFFF0000) | ((tm->tm_mon + 1) << 8) | tm->tm_mday;
		cpu.setAL((uint8_t)tm->tm_wday);
		break;
	}
	case 0x2C: {
		uint64_t us = nowUs();
		time_t t = time(nullptr);
		struct tm *tm = localtime(&t);
		cpu.set16(R_CX, (uint16_t)((tm->tm_hour << 8) | tm->tm_min));
		cpu.set16(R_DX, (uint16_t)((tm->tm_sec << 8) | ((us / 10000) % 100)));
		break;
	}
	case 0x2F:
		// DTA as seen from protected mode: we keep a linear address; give
		// back a selector for it
		{
			uint16_t sel = allocSelectors(1);
			setDescriptor(sel, dtaLinear & ~0xFu, 0xFFFF);
			cpu.loadSeg(S_ES, sel);
			cpu.set16(R_BX, (uint16_t)(dtaLinear & 0xF));
		}
		break;
	case 0x30:
		cpu.set16(R_AX, 0x0005);
		cpu.set16(R_BX, 0);
		cpu.set16(R_CX, 0);
		break;
	case 0x33:
		if (cpu.al() == 0)
			cpu.r[R_DX] &= ~0xFFu;
		break;
	case 0x35: {
		int n = cpu.al();
		cpu.loadSeg(S_ES, pmVectors[n].sel);
		cpu.set16(R_BX, (uint16_t)pmVectors[n].off);
		break;
	}
	case 0x36:
		cpu.set16(R_AX, 64);
		cpu.set16(R_BX, 0x7FFF);
		cpu.set16(R_CX, 512);
		cpu.set16(R_DX, 0xFFFF);
		break;
	case 0x3B:
		ok();
		break;
	case 0x3C:
	case 0x3D: {
		std::string name = readAsciizLinear(dsdx());
		int h;
		bool create = ah == 0x3C;
		if (dosOpen(name, cpu.al(), create, h)) {
			if (trace)
				log("open %s -> %d", name.c_str(), h);
			cpu.set16(R_AX, (uint16_t)h);
			ok();
		} else {
			if (trace)
				log("open %s failed", name.c_str());
			fail(2);
		}
		break;
	}
	case 0x3E: {
		auto it = files.find(cpu.bx());
		if (it != files.end()) {
			fclose(it->second.fp);
			files.erase(it);
			ok();
		} else if (cpu.bx() < 5) {
			ok();
		} else {
			fail(6);
		}
		break;
	}
	case 0x3F: {
		auto it = files.find(cpu.bx());
		if (it == files.end()) {
			if (cpu.bx() == 0) {
				cpu.set16(R_AX, 0);
				ok();
			} else {
				fail(6);
			}
			break;
		}
		uint32_t a = dsdx();
		uint32_t n = cpu.cx();
		if (a + n > MEM_SIZE)
			n = MEM_SIZE - a;
		size_t got = fread(&mem[a], 1, n, it->second.fp);
		cpu.set16(R_AX, (uint16_t)got);
		ok();
		break;
	}
	case 0x40: {
		uint32_t a = dsdx();
		uint32_t n = cpu.cx();
		if (cpu.bx() == 1 || cpu.bx() == 2) {
			fwrite(&mem[a], 1, n, stderr);
			cpu.set16(R_AX, (uint16_t)n);
			ok();
			break;
		}
		auto it = files.find(cpu.bx());
		if (it == files.end()) {
			fail(6);
			break;
		}
		if (n == 0) {
			// truncate at current position
			long pos = fsu::tell(it->second.fp);
			fflush(it->second.fp);
			vfs.truncate(it->second.path, pos);
			cpu.set16(R_AX, 0);
			ok();
			break;
		}
		size_t put = fwrite(&mem[a], 1, n, it->second.fp);
		cpu.set16(R_AX, (uint16_t)put);
		ok();
		break;
	}
	case 0x41: {
		std::string name = readAsciizLinear(dsdx());
		if (vfs.remove(name))
			ok();
		else
			fail(2);
		break;
	}
	case 0x42: {
		auto it = files.find(cpu.bx());
		if (it == files.end()) {
			fail(6);
			break;
		}
		int32_t off = (int32_t)(((uint32_t)cpu.cx() << 16) | cpu.dx());
		int whence = cpu.al() == 0 ? SEEK_SET : cpu.al() == 1 ? SEEK_CUR : SEEK_END;
		fsu::seek(it->second.fp, off, whence);
		long pos = fsu::tell(it->second.fp);
		cpu.set16(R_AX, (uint16_t)pos);
		cpu.set16(R_DX, (uint16_t)(pos >> 16));
		ok();
		break;
	}
	case 0x43: {
		std::string name = readAsciizLinear(dsdx());
		if (cpu.al() == 0) {
			if (vfs.exists(name)) {
				cpu.set16(R_CX, 0x20);
				ok();
			} else {
				fail(2);
			}
		} else {
			ok();
		}
		break;
	}
	case 0x44:
		if (cpu.al() == 0) {
			uint16_t h = cpu.bx();
			cpu.set16(R_DX, h < 5 ? 0x80D3 : 0x0002);
			ok();
		} else if (cpu.al() == 8) {
			cpu.set16(R_AX, 1);
			ok();
		} else {
			cpu.set16(R_AX, 0);
			ok();
		}
		break;
	case 0x47: {
		uint32_t a = selBase(cpu.s[S_DS].sel) + cpu.si();
		mem[a] = 0;
		ok();
		break;
	}
	case 0x4C:
		exitCode = cpu.al();
		quit = true;
		log("program exit code %d", exitCode);
		break;
	case 0x4E:
	case 0x4F: {
		if (ah == 0x4E) {
			findPattern = readAsciizLinear(dsdx());
			findResults = vfs.list(findPattern);
			findPos = 0;
		}
		if (findPos >= findResults.size()) {
			fail(0x12);
			break;
		}
		std::string n = upper(findResults[findPos++]);
		uint32_t d = dtaLinear;
		mem[d + 0x15] = 0x20;
		write16(d + 0x16, 0);
		write16(d + 0x18, 0x2821);
		write32(d + 0x1A, (uint32_t)vfs.fileSize(n));
		memset(&mem[d + 0x1E], 0, 13);
		memcpy(&mem[d + 0x1E], n.c_str(), std::min<size_t>(n.size(), 12));
		ok();
		break;
	}
	case 0x56: {
		std::string from = readAsciizLinear(dsdx());
		std::string to = readAsciizLinear(selBase(cpu.s[S_ES].sel) + cpu.di());
		if (vfs.rename(from, to))
			ok();
		else
			fail(2);
		break;
	}
	case 0x57:
		if (cpu.al() == 0) {
			cpu.set16(R_CX, 0);
			cpu.set16(R_DX, 0x2821);
		}
		ok();
		break;
	case 0x62:
		cpu.set16(R_BX, pspSel);
		break;
	default:
		log("INT 21h AH=%02X not implemented (at %s)", ah, cpu.where().c_str());
		fail(1);
		break;
	}
}

// ------------------------------------------------------------------ DPMI

void Machine::int31() {
	uint16_t ax = cpu.ax();
	auto ok = [this]() { cpu.setCF(false); };
	auto fail = [this](uint16_t err) {
		cpu.set16(R_AX, err);
		cpu.setCF(true);
	};
	switch (ax) {
	case 0x0000: {
		int n = std::max<int>(1, cpu.cx());
		uint16_t sel = allocSelectors(n);
		for (int i = 0; i < n; i++)
			setDescriptor((uint16_t)(sel + i * 8), 0, 0);
		if (sel) {
			cpu.set16(R_AX, sel);
			ok();
		} else {
			fail(0x8011);
		}
		break;
	}
	case 0x0001:
		freeSelector(cpu.bx());
		ok();
		break;
	case 0x0002:
		cpu.set16(R_AX, realSegSelector(cpu.bx()));
		ok();
		break;
	case 0x0003:
		cpu.set16(R_AX, 8);
		ok();
		break;
	case 0x0006: {
		uint32_t b = selBase(cpu.bx());
		cpu.set16(R_CX, (uint16_t)(b >> 16));
		cpu.set16(R_DX, (uint16_t)b);
		ok();
		break;
	}
	case 0x0007: {
		size_t i = cpu.bx() >> 3;
		if (i < ldt.size()) {
			ldt[i].used = true;
			ldt[i].base = ((uint32_t)cpu.cx() << 16) | cpu.dx();
		}
		// reload segment registers that use this selector
		for (int s = 0; s < 6; s++)
			if ((cpu.s[s].sel >> 3) == i && i)
				cpu.s[s].base = ldt[i].base;
		ok();
		break;
	}
	case 0x0008: {
		size_t i = cpu.bx() >> 3;
		if (i < ldt.size()) {
			ldt[i].used = true;
			ldt[i].limit = ((uint32_t)cpu.cx() << 16) | cpu.dx();
		}
		ok();
		break;
	}
	case 0x0009: {
		size_t i = cpu.bx() >> 3;
		if (i < ldt.size())
			ldt[i].access = cpu.cl();
		ok();
		break;
	}
	case 0x000A: {
		uint32_t b, l;
		if (!descriptor(cpu.bx(), b, l)) {
			fail(0x8022);
			break;
		}
		uint16_t sel = allocSelectors(1);
		setDescriptor(sel, b, l);
		cpu.set16(R_AX, sel);
		ok();
		break;
	}
	case 0x000B: {
		uint32_t b = 0, l = 0;
		descriptor(cpu.bx(), b, l);
		uint32_t a = selBase(cpu.s[S_ES].sel) + cpu.di();
		write16(a, (uint16_t)l);
		write16(a + 2, (uint16_t)b);
		mem[a + 4] = (uint8_t)(b >> 16);
		mem[a + 5] = descriptorAccess(cpu.bx());
		mem[a + 6] = (uint8_t)((l >> 16) & 0x0F);
		mem[a + 7] = (uint8_t)(b >> 24);
		ok();
		break;
	}
	case 0x000C: {
		uint32_t a = selBase(cpu.s[S_ES].sel) + cpu.di();
		uint32_t l = read16(a) | ((mem[a + 6] & 0x0F) << 16);
		uint32_t b = read16(a + 2) | (mem[a + 4] << 16) | (mem[a + 7] << 24);
		if (mem[a + 6] & 0x80)
			l = (l << 12) | 0xFFF;
		setDescriptor(cpu.bx(), b, l, (mem[a + 5] & 8) != 0);
		ok();
		break;
	}
	case 0x0100: {
		uint32_t size = (uint32_t)cpu.bx() << 4;
		uint32_t lin = allocDos(size);
		if (!lin) {
			cpu.set16(R_BX, 0x1000);
			fail(8);
			break;
		}
		uint16_t sel = allocSelectors(1);
		setDescriptor(sel, lin, size - 1);
		cpu.set16(R_AX, (uint16_t)(lin >> 4));
		cpu.set16(R_DX, sel);
		ok();
		break;
	}
	case 0x0101:
		freeDos(selBase(cpu.dx()));
		freeSelector(cpu.dx());
		ok();
		break;
	case 0x0200:
		cpu.set16(R_CX, 0xF000);
		cpu.set16(R_DX, 0xFF53);
		ok();
		break;
	case 0x0201:
		ok();
		break;
	case 0x0202:
		cpu.set16(R_CX, trapSel);
		cpu.set16(R_DX, (uint16_t)defaultVectors[0x1F].off);
		ok();
		break;
	case 0x0203:
		ok();
		break;
	case 0x0204:
		cpu.set16(R_CX, pmVectors[cpu.bl()].sel);
		cpu.set16(R_DX, (uint16_t)pmVectors[cpu.bl()].off);
		ok();
		break;
	case 0x0205:
		pmVectors[cpu.bl()] = {cpu.cx(), cpu.dx()};
		ok();
		break;
	case 0x0300: {
		uint32_t regs = selBase(cpu.s[S_ES].sel) + cpu.di();
		realModeInt(regs, cpu.bl());
		ok();
		break;
	}
	case 0x0303: {
		// real-mode callbacks are never invoked by our DOS
		static uint16_t next = 0x100;
		cpu.set16(R_CX, 0xF000);
		cpu.set16(R_DX, next);
		next += 4;
		ok();
		break;
	}
	case 0x0304:
		ok();
		break;
	case 0x0400:
		cpu.set16(R_AX, 0x005A);
		cpu.set16(R_BX, 0x0005);
		cpu.setAL(0x5A);
		cpu.r[R_CX] = (cpu.r[R_CX] & ~0xFFu) | 3;
		cpu.set16(R_DX, 0x0870);
		ok();
		break;
	case 0x0500: {
		uint32_t a = selBase(cpu.s[S_ES].sel) + cpu.di();
		uint32_t freeBytes = 0;
		for (auto &b : heap)
			if (!b.used)
				freeBytes += b.size;
		for (int i = 0; i < 48; i += 4)
			write32(a + i, 0xFFFFFFFF);
		write32(a, freeBytes);
		write32(a + 4, freeBytes / 4096);
		write32(a + 8, freeBytes / 4096);
		ok();
		break;
	}
	case 0x0501: {
		uint32_t size = ((uint32_t)cpu.bx() << 16) | cpu.cx();
		uint32_t lin = allocLinear(size);
		if (!lin) {
			fail(0x8013);
			break;
		}
		cpu.set16(R_BX, (uint16_t)(lin >> 16));
		cpu.set16(R_CX, (uint16_t)lin);
		cpu.set16(R_SI, (uint16_t)(lin >> 16));
		cpu.set16(R_DI, (uint16_t)lin);
		ok();
		break;
	}
	case 0x0502:
		freeLinear(((uint32_t)cpu.si() << 16) | cpu.di());
		ok();
		break;
	case 0x0600:
	case 0x0601:
	case 0x0602:
	case 0x0603:
		ok();
		break;
	case 0x0900:
	case 0x0901:
	case 0x0902: {
		bool was = (cpu.stackWord(2) & F_IF) != 0;
		cpu.setAL(was ? 1 : 0);
		ok();
		break;
	}
	default:
		log("INT 31h AX=%04X not implemented (at %s)", ax, cpu.where().c_str());
		fail(0x8001);
		break;
	}
}

void Machine::realModeInt(uint32_t regs, int intno) {
	uint16_t ax = read16(regs + 28);
	uint16_t bx = read16(regs + 16);
	uint16_t cx = read16(regs + 24);
	uint16_t es = read16(regs + 34);
	uint16_t di = read16(regs);
	uint16_t flags = 0x0202;
	if (intno == 0x2F) {
		switch (ax) {
		case 0x1500: // MSCDEX installation check
			if (cd.present()) {
				write16(regs + 16, 1);  // one drive
				write16(regs + 24, 3);  // D:
			} else {
				write16(regs + 16, 0);
			}
			break;
		case 0x150B: // drive check
			write16(regs + 28, (cd.present() && cx == 3) ? 0x5AD8 : 0);
			write16(regs + 16, 0xADAD);
			break;
		case 0x150C: // version
			write16(regs + 16, 0x0215);
			break;
		case 0x150D: // drive letters
			mem[((uint32_t)es << 4) + bx] = 3;
			break;
		case 0x1510:
			mscdexRequest(((uint32_t)es << 4) + bx);
			break;
		default:
			log("real-mode INT 2Fh AX=%04X not implemented", ax);
			break;
		}
	} else if (intno == 0x10) {
		if (ax == 0x4F00) {
			uint32_t a = ((uint32_t)es << 4) + di;
			memcpy(&mem[a], "VESA", 4);
			write16(a + 4, 0x0102);
			write16(a + 18, 16); // 1 MB
			write16(regs + 28, 0x004F);
		} else if (ax == 0x4F01) {
			uint32_t a = ((uint32_t)es << 4) + di;
			memset(&mem[a], 0, 256);
			write16(a, 0x009B);
			mem[a + 2] = 7;
			mem[a + 3] = 0;
			write16(a + 4, 64);
			write16(a + 6, 64);
			write16(a + 8, 0xA000);
			write16(a + 0x10, 640);
			write16(a + 0x12, 640);
			write16(a + 0x14, 480);
			mem[a + 0x19] = 8;
			write16(regs + 28, 0x004F);
		} else {
			log("real-mode INT 10h AX=%04X not implemented", ax);
		}
	} else {
		log("real-mode INT %02Xh AX=%04X not implemented", intno, ax);
	}
	write16(regs + 32, flags);
}

// ------------------------------------------------------------------ MSCDEX device requests

static uint32_t msfToLba(uint32_t msf) {
	uint32_t m = (msf >> 16) & 0xFF, s = (msf >> 8) & 0xFF, f = msf & 0xFF;
	return (m * 60 + s) * 75 + f - 150;
}
static uint32_t lbaToMsf(uint32_t lba) {
	lba += 150;
	uint32_t m = lba / (60 * 75), s = (lba / 75) % 60, f = lba % 75;
	return (m << 16) | (s << 8) | f;
}

void Machine::mscdexRequest(uint32_t req) {
	uint8_t cmd = mem[req + 2];
	if (trace)
		log("MSCDEX cmd %d (ioctl %d) start %08X count %u", cmd,
		    cmd == 3 ? mem[((uint32_t)read16(req + 0x10) << 4) + read16(req + 0x0E)] : -1,
		    read32(req + 0x0E), read32(req + 0x12));
	uint16_t status = 0x0100; // done
	switch (cmd) {
	case 3: { // IOCTL input
		uint32_t buf = ((uint32_t)read16(req + 0x10) << 4) + read16(req + 0x0E);
		uint8_t code = mem[buf];
		switch (code) {
		case 0: // device header address
			write32(buf + 1, 0);
			break;
		case 1: { // head location
			uint32_t pos = cd.position();
			if (mem[buf + 1] == 1)
				write32(buf + 2, lbaToMsf(pos));
			else
				write32(buf + 2, pos);
			break;
		}
		case 6: // device status
			write32(buf + 1, 0x00000210 | (cd.playing() ? 0 : 0));
			break;
		case 7: // sector size
			write16(buf + 2, 2048);
			break;
		case 8: // volume size
			write32(buf + 1, cd.leadOut());
			break;
		case 9: // media changed
			mem[buf + 1] = 1;
			break;
		case 10: { // audio disk info
			mem[buf + 1] = (uint8_t)cd.firstTrack();
			mem[buf + 2] = (uint8_t)cd.lastTrack();
			write32(buf + 3, lbaToMsf(cd.leadOut()));
			break;
		}
		case 11: { // audio track info
			int t = mem[buf + 1];
			write32(buf + 2, lbaToMsf(cd.trackStart(t)));
			mem[buf + 6] = cd.trackIsAudio(t) ? 0x00 : 0x40;
			break;
		}
		case 12: { // Q channel
			uint32_t pos = cd.position();
			int t = cd.trackAt(pos);
			uint32_t rel = pos - cd.trackStart(t);
			mem[buf + 1] = 0x01;
			mem[buf + 2] = (uint8_t)t;
			mem[buf + 3] = 1;
			uint32_t r = lbaToMsf(rel) - 0x000200; // relative msf without 2s offset
			r = rel;
			mem[buf + 4] = (uint8_t)(r / (60 * 75));
			mem[buf + 5] = (uint8_t)((r / 75) % 60);
			mem[buf + 6] = (uint8_t)(r % 75);
			mem[buf + 7] = 0;
			uint32_t a = lbaToMsf(pos);
			mem[buf + 8] = (uint8_t)(a >> 16);
			mem[buf + 9] = (uint8_t)(a >> 8);
			mem[buf + 10] = (uint8_t)a;
			break;
		}
		case 15: { // audio status
			write16(buf + 1, cd.paused() ? 1 : 0);
			write32(buf + 3, lbaToMsf(cd.lastStart()));
			write32(buf + 7, lbaToMsf(cd.lastEnd()));
			break;
		}
		default:
			log("MSCDEX IOCTL input %d", code);
			break;
		}
		break;
	}
	case 12: { // IOCTL output
		uint32_t buf = ((uint32_t)read16(req + 0x10) << 4) + read16(req + 0x0E);
		if (mem[buf] == 3) // audio channel control: volume of output channel 0
			cd.setVolume(mem[buf + 2]);
		break;
	}
	case 13: // open
	case 14: // close
		break;
	case 132: { // play audio
		uint8_t mode = mem[req + 0x0D];
		uint32_t start = read32(req + 0x0E);
		uint32_t count = read32(req + 0x12);
		if (mode == 1)
			start = msfToLba(start);
		cd.play(start, count);
		break;
	}
	case 133: // stop
		if (cd.playing())
			cd.pause();
		else
			cd.stop();
		break;
	case 136: // resume
		cd.resume();
		break;
	default:
		log("MSCDEX request %d not implemented", cmd);
		break;
	}
	if (cd.playing())
		status |= 0x0200; // busy
	write16(req + 3, status);
}

// ------------------------------------------------------------------ BIOS

void Machine::int10() {
	uint16_t ax = cpu.ax();
	uint8_t ah = cpu.ah();
	if (ah == 0x4F) {
		switch (cpu.al()) {
		case 0x02:
			vesaMode = cpu.bx() & 0x1FF;
			if (!(cpu.bx() & 0x8000)) {
				std::fill(vram.begin(), vram.end(), 0);
				memset(&mem[0xA0000], 0, 0x10000);
			}
			vesaBank = 0;
			cpu.set16(R_AX, 0x004F);
			break;
		case 0x03:
			cpu.set16(R_BX, (uint16_t)vesaMode);
			cpu.set16(R_AX, 0x004F);
			break;
		case 0x05:
			if (cpu.bh() == 0)
				setBank(cpu.dx());
			else
				cpu.set16(R_DX, (uint16_t)vesaBank);
			cpu.set16(R_AX, 0x004F);
			break;
		default:
			log("VESA function %02X not implemented", cpu.al());
			cpu.set16(R_AX, 0x014F);
			break;
		}
		return;
	}
	switch (ah) {
	case 0x00:
		vesaMode = cpu.al() & 0x7F;
		if (vesaMode == 0x13) {
			memset(&mem[0xA0000], 0, 0x10000);
		}
		break;
	case 0x01:
	case 0x02:
	case 0x05:
	case 0x06:
	case 0x07:
	case 0x09:
	case 0x0E:
		break;
	case 0x03:
		cpu.set16(R_CX, 0x0607);
		cpu.set16(R_DX, 0);
		break;
	case 0x08: // read character at cursor
		cpu.set16(R_AX, 0x0720);
		break;
	case 0x0F:
		cpu.setAL((uint8_t)(vesaMode < 0x100 ? vesaMode : 0x62));
		cpu.setAH(80);
		cpu.r[R_BX] &= ~0xFF00u;
		break;
	case 0x10:
		if (cpu.al() == 0x12) {
			// set block of DAC registers from ES:DX
			uint32_t a = selBase(cpu.s[S_ES].sel) + cpu.dx();
			for (int i = 0; i < cpu.cx(); i++) {
				int idx = (cpu.bx() + i) & 0xFF;
				dac[idx][0] = mem[a + i * 3] & 63;
				dac[idx][1] = mem[a + i * 3 + 1] & 63;
				dac[idx][2] = mem[a + i * 3 + 2] & 63;
			}
		} else if (cpu.al() == 0x17) {
			uint32_t a = selBase(cpu.s[S_ES].sel) + cpu.dx();
			for (int i = 0; i < cpu.cx(); i++) {
				int idx = (cpu.bx() + i) & 0xFF;
				for (int c = 0; c < 3; c++)
					mem[a + i * 3 + c] = dac[idx][c];
			}
		}
		break;
	case 0x11:
		break;
	case 0x12:
		if (cpu.bl() == 0x36) {
			screenOff = cpu.al() != 0;
			cpu.setAL(0x12);
		} else if (cpu.bl() == 0x10) {
			cpu.set16(R_BX, 0x0003);
		}
		break;
	case 0x1A:
		cpu.setAL(0x1A);
		cpu.set16(R_BX, 0x0008);
		break;
	default:
		log("INT 10h AX=%04X not implemented", ax);
		break;
	}
}

void Machine::int16() {
	uint8_t ah = cpu.ah() & 0xEF;
	switch (ah) {
	case 0x00:
		while (biosKeys.empty() && !quit) {
			host.sleepMicros(2000);
			service();
		}
		if (!biosKeys.empty()) {
			cpu.set16(R_AX, biosKeys.front());
			biosKeys.pop_front();
		}
		break;
	case 0x01:
		if (biosKeys.empty()) {
			cpu.setZF(true);
			idleHint();
		} else {
			cpu.set16(R_AX, biosKeys.front());
			cpu.setZF(false);
		}
		break;
	case 0x02:
		cpu.setAL(0);
		break;
	default:
		break;
	}
}

void Machine::int1a() {
	if (cpu.ah() == 0) {
		cpu.set16(R_CX, (uint16_t)(biosTicks >> 16));
		cpu.set16(R_DX, (uint16_t)biosTicks);
		cpu.setAL(0);
	}
}

void Machine::int2f() {
	uint16_t ax = cpu.ax();
	if (ax == 0x1500) {
		if (cd.present()) {
			cpu.set16(R_BX, 1);
			cpu.set16(R_CX, 3);
		} else {
			cpu.set16(R_BX, 0);
		}
		return;
	}
	if (ax == 0x1687) {
		cpu.set16(R_AX, 1); // no further DPMI host
		return;
	}
	// everything else: not installed (leave registers untouched)
}

void Machine::int33() {
	switch (cpu.ax()) {
	case 0x00:
		cpu.set16(R_AX, 0xFFFF);
		cpu.set16(R_BX, 2);
		mouseHandlerMask = 0;
		break;
	case 0x01:
	case 0x02:
		break;
	case 0x03:
		cpu.set16(R_BX, (uint16_t)mouseButtons);
		cpu.set16(R_CX, (uint16_t)mouseX);
		cpu.set16(R_DX, (uint16_t)mouseY);
		break;
	case 0x04:
		mouseX = std::clamp<int>((int16_t)cpu.cx(), mouseMinX, mouseMaxX);
		mouseY = std::clamp<int>((int16_t)cpu.dx(), mouseMinY, mouseMaxY);
		host.warpMouse(mouseX / MOUSE_SCALE, mouseY / MOUSE_SCALE);
		break;
	case 0x07:
		mouseMinX = (int16_t)cpu.cx();
		mouseMaxX = (int16_t)cpu.dx();
		break;
	case 0x08:
		mouseMinY = (int16_t)cpu.cx();
		mouseMaxY = (int16_t)cpu.dx();
		break;
	case 0x0C:
		mouseHandlerMask = cpu.cx();
		mouseHandlerSel = cpu.s[S_ES].sel;
		mouseHandlerOff = cpu.dx();
		break;
	case 0x0F:
		mouseMickeyX = cpu.cx();
		mouseMickeyY = cpu.dx();
		break;
	default:
		log("INT 33h AX=%04X not implemented", cpu.ax());
		break;
	}
}
