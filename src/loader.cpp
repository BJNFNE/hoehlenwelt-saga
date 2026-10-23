// NE (16-bit protected mode) executable loader.
#include <cstdio>

#include "machine.h"
#include "vfs.h"

static uint16_t rd16(const std::vector<uint8_t> &d, size_t o) {
	return o + 1 < d.size() ? (uint16_t)(d[o] | (d[o + 1] << 8)) : 0;
}
static uint32_t rd32(const std::vector<uint8_t> &d, size_t o) {
	return rd16(d, o) | ((uint32_t)rd16(d, o + 2) << 16);
}

bool Machine::loadNE(const std::string &path) {
	if (!vfs.readWhole(path, exeImage)) {
		log("cannot read %s", path.c_str());
		return false;
	}
	const auto &d = exeImage;
	if (d.size() < 0x40 || d[0] != 'M' || d[1] != 'Z')
		return false;
	uint32_t ne = rd32(d, 0x3C);
	if (ne + 0x40 > d.size() || d[ne] != 'N' || d[ne + 1] != 'E')
		return false;

	uint16_t autodata = rd16(d, ne + 0x0E);
	uint16_t ip = rd16(d, ne + 0x14), cs = rd16(d, ne + 0x16);
	uint16_t sp = rd16(d, ne + 0x18), ss = rd16(d, ne + 0x1A);
	uint16_t nseg = rd16(d, ne + 0x1C), nmod = rd16(d, ne + 0x1E);
	uint16_t segtab = rd16(d, ne + 0x22);
	uint16_t modref = rd16(d, ne + 0x28), impname = rd16(d, ne + 0x2A);
	uint16_t align = rd16(d, ne + 0x32);
	numSegs = nseg;

	std::vector<std::string> mods;
	for (int i = 0; i < nmod; i++) {
		uint16_t o = rd16(d, ne + modref + i * 2);
		size_t p = ne + impname + o;
		mods.emplace_back((const char *)&d[p + 1], d[p]);
	}

	// allocate selectors: every segment gets a pair (segment, writable alias),
	// as RTM does; the Pascal RTL relies on CS + __AHINCR being a data alias.
	struct SegInfo {
		uint32_t fileOff, len, minAlloc, linear;
		uint16_t flags;
	};
	std::vector<SegInfo> segs(nseg + 1);
	for (int i = 1; i <= nseg; i++) {
		size_t e = ne + segtab + (i - 1) * 8;
		SegInfo &s = segs[i];
		uint16_t off = rd16(d, e);
		s.fileOff = (uint32_t)off << align;
		s.len = rd16(d, e + 2);
		if (s.len == 0 && off)
			s.len = 0x10000;
		s.flags = rd16(d, e + 4);
		s.minAlloc = rd16(d, e + 6);
		if (s.minAlloc == 0)
			s.minAlloc = 0x10000;
		bool data = s.flags & 1;
		uint32_t size = data ? 0x10000 : std::max(s.len, s.minAlloc);
		s.linear = allocLinear(size);
		uint16_t sel = allocSelectors(2);
		segSel[i] = sel;
		char name[16];
		snprintf(name, sizeof(name), "seg%d", i);
		setDescriptor(sel, s.linear, data ? 0xFFFF : size - 1, !data, name);
		snprintf(name, sizeof(name), "seg%d'", i);
		setDescriptor((uint16_t)(sel + 8), s.linear, data ? 0xFFFF : size - 1, false, name);
		if (off && s.len)
			memcpy(&mem[s.linear], &d[s.fileOff], std::min<size_t>(s.len, d.size() - s.fileOff));
	}

	// imports -> host stubs or constants
	std::map<std::pair<std::string, int>, std::pair<uint16_t, uint16_t>> imports;
	setupImports(imports);

	// relocations
	for (int i = 1; i <= nseg; i++) {
		SegInfo &s = segs[i];
		if (!(s.flags & 0x100) || !s.len)
			continue;
		size_t p = s.fileOff + s.len;
		uint16_t cnt = rd16(d, p);
		p += 2;
		for (int r = 0; r < cnt; r++, p += 8) {
			uint8_t stype = d[p], rflags = d[p + 1];
			uint16_t srcOff = rd16(d, p + 2);
			uint16_t a = rd16(d, p + 4), b = rd16(d, p + 6);
			uint16_t tsel = 0, toff = 0;
			int kind = rflags & 3;
			if (kind == 0) {
				uint8_t tseg = a & 0xFF;
				if (tseg == 0xFF) {
					log("movable entry relocation not supported");
					continue;
				}
				tsel = segSel[tseg];
				toff = b;
			} else if (kind == 1) {
				auto it = imports.find({mods[a - 1], b});
				if (it == imports.end()) {
					log("unresolved import %s.%u", mods[a - 1].c_str(), b);
					continue;
				}
				tsel = it->second.first;
				toff = it->second.second;
			} else {
				log("unsupported relocation kind %d", kind);
				continue;
			}
			bool additive = rflags & 4;
			uint32_t base = s.linear;
			uint16_t o = srcOff;
			for (int guard = 0; guard < 10000; guard++) {
				uint16_t next = read16(base + o);
				switch (stype) {
				case 2: // selector
					write16(base + o, additive ? (uint16_t)(next + tsel) : tsel);
					break;
				case 3: // far pointer
					if (additive)
						write16(base + o, (uint16_t)(next + toff));
					else
						write16(base + o, toff);
					write16(base + o + 2, tsel);
					break;
				case 5: // offset
					write16(base + o, additive ? (uint16_t)(next + toff) : toff);
					break;
				default:
					log("unsupported relocation source type %d", stype);
					break;
				}
				if (additive || next == 0xFFFF)
					break;
				o = next;
			}
		}
	}

	// PSP and environment (real-mode memory so that DOS calls can use it)
	uint32_t env = allocDos(256);
	const char envStr[] = "PATH=C:\\\0COMSPEC=C:\\COMMAND.COM\0\0\x01\0C:\\HW.EXE\0";
	memcpy(&mem[env], envStr, sizeof(envStr));
	envSel = allocSelectors(1);
	setDescriptor(envSel, env, 0xFF, false, "ENV");
	uint32_t psp = allocDos(256);
	mem[psp] = 0xCD;
	mem[psp + 1] = 0x20;
	write16(psp + 2, 0xA000);
	write16(psp + 0x2C, envSel);
	write16(psp + 0x32, 20);      // handle table size
	mem[psp + 0x80] = 0;
	mem[psp + 0x81] = 0x0D;
	for (int i = 0; i < 20; i++)
		mem[psp + 0x18 + i] = i < 5 ? i : 0xFF;
	pspSel = allocSelectors(1);
	setDescriptor(pspSel, psp, 0xFF, false, "PSP");

	// initial register state (Windows/RTM task start convention)
	cpu.loadSeg(S_CS, segSel[cs]);
	cpu.eip = ip;
	cpu.loadSeg(S_SS, segSel[ss]);
	cpu.r[R_SP] = sp;
	cpu.loadSeg(S_DS, segSel[autodata]);
	cpu.loadSeg(S_ES, pspSel);
	cpu.r[R_BX] = 0x2000;
	cpu.r[R_CX] = 0;
	cpu.r[R_DI] = segSel[autodata];
	cpu.r[R_SI] = 0;
	cpu.r[R_BP] = 0;
	cpu.flags = 0x0202;
	log("loaded %s: %d segments, entry %s:%04X", path.c_str(), nseg, selName(segSel[cs]).c_str(), ip);
	return true;
}
