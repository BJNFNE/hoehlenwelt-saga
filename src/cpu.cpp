#include "cpu.h"

#include <cstdio>

#include "machine.h"

static inline uint32_t sizeMask(int size) {
	return size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu;
}
static inline uint32_t signBit(int size) {
	return size == 1 ? 0x80u : size == 2 ? 0x8000u : 0x80000000u;
}
static inline int32_t sext(int size, uint32_t v) {
	return size == 1 ? (int32_t)(int8_t)v : size == 2 ? (int32_t)(int16_t)v : (int32_t)v;
}
static inline bool parity(uint8_t v) {
	v ^= v >> 4;
	v ^= v >> 2;
	v ^= v >> 1;
	return !(v & 1);
}

Cpu::Cpu(Machine &mach) : m(mach) {}

std::string Cpu::where() const {
	char buf[64];
	snprintf(buf, sizeof(buf), "%s:%04X", m.selName(curCs).c_str(), curIp);
	return buf;
}

uint32_t Cpu::linear(int seg, uint32_t off) const {
	return s[seg].base + off;
}

uint32_t Cpu::rd(int seg, uint32_t off, int size) {
	uint32_t a = s[seg].base + off;
	if (size == 1)
		return m.read8(a);
	if (size == 2)
		return m.read16(a);
	return m.read32(a);
}

void Cpu::wr(int seg, uint32_t off, int size, uint32_t v) {
	uint32_t a = s[seg].base + off;
	if (size == 1)
		m.write8(a, (uint8_t)v);
	else if (size == 2)
		m.write16(a, (uint16_t)v);
	else
		m.write32(a, v);
}

uint8_t Cpu::fetch8() {
	uint8_t v = m.read8(s[S_CS].base + (eip & 0xFFFF));
	eip = (eip + 1) & 0xFFFF;
	return v;
}
uint16_t Cpu::fetch16() {
	uint16_t lo = fetch8();
	return lo | (fetch8() << 8);
}
uint32_t Cpu::fetch32() {
	uint32_t lo = fetch16();
	return lo | ((uint32_t)fetch16() << 16);
}

void Cpu::loadSeg(int seg, uint16_t sel) {
	uint32_t base, limit;
	if (!m.descriptor(sel, base, limit)) {
		if ((sel & 0xFFFC) != 0 || seg == S_CS || seg == S_SS)
			m.log("invalid selector %04X loaded into seg %d at %s", sel, seg, where().c_str());
		base = 0;
		limit = 0;
	}
	s[seg].sel = sel;
	s[seg].base = base;
	s[seg].limit = limit;
}

void Cpu::push16(uint16_t v) {
	uint16_t sp = (uint16_t)(r[R_SP] - 2);
	set16(R_SP, sp);
	m.write16(s[S_SS].base + sp, v);
}
uint16_t Cpu::pop16() {
	uint16_t sp = (uint16_t)r[R_SP];
	uint16_t v = m.read16(s[S_SS].base + sp);
	set16(R_SP, (uint16_t)(sp + 2));
	return v;
}
void Cpu::push32(uint32_t v) {
	uint16_t sp = (uint16_t)(r[R_SP] - 4);
	set16(R_SP, sp);
	m.write32(s[S_SS].base + sp, v);
}
uint32_t Cpu::pop32() {
	uint16_t sp = (uint16_t)r[R_SP];
	uint32_t v = m.read32(s[S_SS].base + sp);
	set16(R_SP, (uint16_t)(sp + 4));
	return v;
}
uint16_t Cpu::stackWord(int idx) const {
	return m.read16(s[S_SS].base + (uint16_t)(r[R_SP] + idx * 2));
}

void Cpu::decodeModrm() {
	modrm = fetch8();
	mrMod = modrm >> 6;
	mrReg = (modrm >> 3) & 7;
	mrRm = modrm & 7;
	eaIsReg = mrMod == 3;
	if (eaIsReg)
		return;
	if (!adsz32) {
		uint16_t off = 0;
		int defSeg = S_DS;
		switch (mrRm) {
		case 0: off = (uint16_t)(r[R_BX] + r[R_SI]); break;
		case 1: off = (uint16_t)(r[R_BX] + r[R_DI]); break;
		case 2: off = (uint16_t)(r[R_BP] + r[R_SI]); defSeg = S_SS; break;
		case 3: off = (uint16_t)(r[R_BP] + r[R_DI]); defSeg = S_SS; break;
		case 4: off = (uint16_t)r[R_SI]; break;
		case 5: off = (uint16_t)r[R_DI]; break;
		case 6:
			if (mrMod == 0) {
				off = fetch16();
			} else {
				off = (uint16_t)r[R_BP];
				defSeg = S_SS;
			}
			break;
		case 7: off = (uint16_t)r[R_BX]; break;
		}
		if (mrMod == 1)
			off = (uint16_t)(off + (int8_t)fetch8());
		else if (mrMod == 2)
			off = (uint16_t)(off + fetch16());
		eaOff = off;
		eaSeg = segOvr != S_NONE ? segOvr : defSeg;
	} else {
		uint32_t off = 0;
		int defSeg = S_DS;
		if (mrRm == 4) {
			uint8_t sib = fetch8();
			int scale = sib >> 6, idx = (sib >> 3) & 7, base = sib & 7;
			if (base == 5 && mrMod == 0) {
				off = fetch32();
			} else {
				off = r[base];
				if (base == R_SP || base == R_BP)
					defSeg = S_SS;
			}
			if (idx != 4)
				off += r[idx] << scale;
		} else if (mrRm == 5 && mrMod == 0) {
			off = fetch32();
		} else {
			off = r[mrRm];
			if (mrRm == R_BP)
				defSeg = S_SS;
		}
		if (mrMod == 1)
			off += (int8_t)fetch8();
		else if (mrMod == 2)
			off += fetch32();
		eaOff = off;
		eaSeg = segOvr != S_NONE ? segOvr : defSeg;
	}
}

uint32_t Cpu::getReg(int size, int reg) const {
	if (size == 1)
		return reg < 4 ? (r[reg] & 0xFF) : ((r[reg - 4] >> 8) & 0xFF);
	if (size == 2)
		return r[reg] & 0xFFFF;
	return r[reg];
}

void Cpu::setReg(int size, int reg, uint32_t v) {
	if (size == 1) {
		if (reg < 4)
			r[reg] = (r[reg] & ~0xFFu) | (v & 0xFF);
		else
			r[reg - 4] = (r[reg - 4] & ~0xFF00u) | ((v & 0xFF) << 8);
	} else if (size == 2) {
		r[reg] = (r[reg] & 0xFFFF0000u) | (v & 0xFFFF);
	} else {
		r[reg] = v;
	}
}

uint32_t Cpu::readRM(int size) {
	if (eaIsReg)
		return getReg(size, mrRm);
	return rd(eaSeg, eaOff, size);
}

void Cpu::writeRM(int size, uint32_t v) {
	if (eaIsReg)
		setReg(size, mrRm, v);
	else
		wr(eaSeg, eaOff, size, v);
}

void Cpu::setSZP(int size, uint32_t res) {
	res &= sizeMask(size);
	flags &= ~(F_SF | F_ZF | F_PF);
	if (res == 0)
		flags |= F_ZF;
	if (res & signBit(size))
		flags |= F_SF;
	if (parity((uint8_t)res))
		flags |= F_PF;
}

uint32_t Cpu::alu(int op, int size, uint32_t a, uint32_t b) {
	uint32_t mask = sizeMask(size), sb = signBit(size);
	a &= mask;
	b &= mask;
	uint32_t res = 0;
	uint64_t wide;
	uint32_t cin;
	switch (op) {
	case 0: // add
	case 2: // adc
		cin = (op == 2 && (flags & F_CF)) ? 1 : 0;
		wide = (uint64_t)a + b + cin;
		res = (uint32_t)wide & mask;
		flags &= ~(F_CF | F_OF | F_AF);
		if (wide > mask)
			flags |= F_CF;
		if ((~(a ^ b) & (a ^ res)) & sb)
			flags |= F_OF;
		if ((a ^ b ^ res) & 0x10)
			flags |= F_AF;
		setSZP(size, res);
		return res;
	case 3: // sbb
	case 5: // sub
	case 7: // cmp
		cin = (op == 3 && (flags & F_CF)) ? 1 : 0;
		res = (a - b - cin) & mask;
		flags &= ~(F_CF | F_OF | F_AF);
		if ((uint64_t)b + cin > a)
			flags |= F_CF;
		if (((a ^ b) & (a ^ res)) & sb)
			flags |= F_OF;
		if ((a ^ b ^ res) & 0x10)
			flags |= F_AF;
		setSZP(size, res);
		return res;
	case 1: res = a | b; break;
	case 4: res = a & b; break;
	case 6: res = a ^ b; break;
	}
	flags &= ~(F_CF | F_OF | F_AF);
	setSZP(size, res);
	return res;
}

uint32_t Cpu::incdec(bool dec, int size, uint32_t a) {
	uint32_t mask = sizeMask(size), sb = signBit(size);
	uint32_t res = (dec ? a - 1 : a + 1) & mask;
	flags &= ~(F_OF | F_AF);
	if (!dec && res == sb)
		flags |= F_OF;
	if (dec && (a & mask) == sb)
		flags |= F_OF;
	if ((a ^ res) & 0x10)
		flags |= F_AF;
	setSZP(size, res);
	return res;
}

uint32_t Cpu::shift(int op, int size, uint32_t a, uint8_t cnt) {
	uint32_t mask = sizeMask(size), sb = signBit(size);
	int bits = size * 8;
	a &= mask;
	cnt &= 0x1F;
	if (cnt == 0)
		return a;
	uint32_t res = a;
	bool cf = false;
	switch (op) {
	case 0: { // rol
		int c = cnt % bits;
		res = c ? ((a << c) | (a >> (bits - c))) & mask : a;
		cf = res & 1;
		flags = (flags & ~(F_CF | F_OF)) | (cf ? (uint32_t)F_CF : 0u);
		if (((res & sb) != 0) != cf)
			flags |= F_OF;
		return res;
	}
	case 1: { // ror
		int c = cnt % bits;
		res = c ? ((a >> c) | (a << (bits - c))) & mask : a;
		cf = (res & sb) != 0;
		flags = (flags & ~(F_CF | F_OF)) | (cf ? (uint32_t)F_CF : 0u);
		if (((res ^ (res << 1)) & sb) != 0)
			flags |= F_OF;
		return res;
	}
	case 2: { // rcl
		int c = cnt % (bits + 1);
		cf = (flags & F_CF) != 0;
		for (int i = 0; i < c; i++) {
			bool nc = (res & sb) != 0;
			res = ((res << 1) | (cf ? 1 : 0)) & mask;
			cf = nc;
		}
		flags = (flags & ~(F_CF | F_OF)) | (cf ? (uint32_t)F_CF : 0u);
		if (((res & sb) != 0) != cf)
			flags |= F_OF;
		return res;
	}
	case 3: { // rcr
		int c = cnt % (bits + 1);
		cf = (flags & F_CF) != 0;
		bool of = (((res & sb) != 0) != cf);
		for (int i = 0; i < c; i++) {
			bool nc = res & 1;
			res = (res >> 1) | (cf ? sb : 0);
			cf = nc;
		}
		flags = (flags & ~(F_CF | F_OF)) | (cf ? (uint32_t)F_CF : 0u) | (of ? (uint32_t)F_OF : 0u);
		return res;
	}
	case 4:
	case 6: // shl/sal
		if (cnt <= bits) {
			cf = cnt <= bits && ((uint64_t)a << (cnt - 1)) & sb;
			res = (uint32_t)(((uint64_t)a << cnt) & mask);
		} else {
			cf = false;
			res = 0;
		}
		flags &= ~(F_CF | F_OF | F_AF);
		if (cf)
			flags |= F_CF;
		if (((res & sb) != 0) != cf)
			flags |= F_OF;
		setSZP(size, res);
		return res;
	case 5: // shr
		cf = cnt <= bits ? ((a >> (cnt - 1)) & 1) : 0;
		res = cnt < bits ? (a >> cnt) : 0;
		flags &= ~(F_CF | F_OF | F_AF);
		if (cf)
			flags |= F_CF;
		if (cnt == 1 && (a & sb))
			flags |= F_OF;
		setSZP(size, res);
		return res;
	case 7: { // sar
		int32_t sv = sext(size, a);
		int c = cnt >= bits ? bits - 1 : cnt;
		cf = (sv >> (c - 1)) & 1;
		if (cnt >= bits)
			cf = sv < 0;
		res = (uint32_t)(sv >> c) & mask;
		flags &= ~(F_CF | F_OF | F_AF);
		if (cf)
			flags |= F_CF;
		setSZP(size, res);
		return res;
	}
	}
	return res;
}

bool Cpu::cond(int cc) const {
	bool cf = flags & F_CF, zf = flags & F_ZF, sf = flags & F_SF, of = flags & F_OF, pf = flags & F_PF;
	bool v;
	switch (cc >> 1) {
	case 0: v = of; break;
	case 1: v = cf; break;
	case 2: v = zf; break;
	case 3: v = cf || zf; break;
	case 4: v = sf; break;
	case 5: v = pf; break;
	case 6: v = sf != of; break;
	default: v = zf || (sf != of); break;
	}
	return (cc & 1) ? !v : v;
}

void Cpu::farJump(uint16_t sel, uint32_t off) {
	loadSeg(S_CS, sel);
	eip = off & 0xFFFF;
}

void Cpu::farCall(uint16_t sel, uint32_t off, int size) {
	if (size == 4) {
		push32(s[S_CS].sel);
		push32(eip);
	} else {
		push16(s[S_CS].sel);
		push16((uint16_t)eip);
	}
	farJump(sel, off);
}

void Cpu::farReturn(int popBytes) {
	uint16_t ip = pop16();
	uint16_t cs = pop16();
	set16(R_SP, (uint16_t)(r[R_SP] + popBytes));
	farJump(cs, ip);
}

void Cpu::iret() {
	uint16_t ip = pop16();
	uint16_t cs = pop16();
	uint16_t fl = pop16();
	farJump(cs, ip);
	flags = (flags & 0xFFFF0000u) | (fl & 0x7FD5) | 2;
}

void Cpu::doInterrupt(int n, bool software) {
	(void)software;
	uint16_t sel;
	uint32_t off;
	m.getVector(n, sel, off);
	push16((uint16_t)flags);
	push16(s[S_CS].sel);
	push16((uint16_t)eip);
	flags &= ~(F_IF | F_TF);
	farJump(sel, off);
}

void Cpu::grp3(int size) {
	uint32_t mask = sizeMask(size);
	uint32_t v = readRM(size);
	switch (mrReg) {
	case 0:
	case 1: {
		uint32_t imm = fetchImm(size);
		alu(4, size, v, imm);
		break;
	}
	case 2:
		writeRM(size, ~v & mask);
		break;
	case 3: {
		uint32_t res = alu(5, size, 0, v);
		writeRM(size, res);
		setCF(v != 0);
		break;
	}
	case 4: { // mul
		if (size == 1) {
			uint32_t res = (r[R_AX] & 0xFF) * v;
			set16(R_AX, (uint16_t)res);
			bool hi = (res & 0xFF00) != 0;
			flags = (flags & ~(F_CF | F_OF)) | (hi ? (uint32_t)(F_CF | F_OF) : 0u);
		} else if (size == 2) {
			uint32_t res = (r[R_AX] & 0xFFFF) * v;
			set16(R_AX, (uint16_t)res);
			set16(R_DX, (uint16_t)(res >> 16));
			bool hi = (res >> 16) != 0;
			flags = (flags & ~(F_CF | F_OF)) | (hi ? (uint32_t)(F_CF | F_OF) : 0u);
		} else {
			uint64_t res = (uint64_t)r[R_AX] * v;
			r[R_AX] = (uint32_t)res;
			r[R_DX] = (uint32_t)(res >> 32);
			bool hi = (res >> 32) != 0;
			flags = (flags & ~(F_CF | F_OF)) | (hi ? (uint32_t)(F_CF | F_OF) : 0u);
		}
		break;
	}
	case 5: { // imul
		if (size == 1) {
			int32_t res = (int8_t)r[R_AX] * (int8_t)v;
			set16(R_AX, (uint16_t)res);
			bool ov = res != (int8_t)res;
			flags = (flags & ~(F_CF | F_OF)) | (ov ? (uint32_t)(F_CF | F_OF) : 0u);
		} else if (size == 2) {
			int32_t res = (int16_t)r[R_AX] * (int16_t)v;
			set16(R_AX, (uint16_t)res);
			set16(R_DX, (uint16_t)(res >> 16));
			bool ov = res != (int16_t)res;
			flags = (flags & ~(F_CF | F_OF)) | (ov ? (uint32_t)(F_CF | F_OF) : 0u);
		} else {
			int64_t res = (int64_t)(int32_t)r[R_AX] * (int32_t)v;
			r[R_AX] = (uint32_t)res;
			r[R_DX] = (uint32_t)((uint64_t)res >> 32);
			bool ov = res != (int32_t)res;
			flags = (flags & ~(F_CF | F_OF)) | (ov ? (uint32_t)(F_CF | F_OF) : 0u);
		}
		break;
	}
	case 6: { // div
		if (v == 0) {
			m.log("divide by zero at %s", where().c_str());
			doInterrupt(0, false);
			return;
		}
		if (size == 1) {
			uint32_t num = r[R_AX] & 0xFFFF;
			uint32_t q = num / v;
			if (q > 0xFF) {
				m.log("divide overflow at %s", where().c_str());
				doInterrupt(0, false);
				return;
			}
			setAL((uint8_t)q);
			setAH((uint8_t)(num % v));
		} else if (size == 2) {
			uint32_t num = ((r[R_DX] & 0xFFFF) << 16) | (r[R_AX] & 0xFFFF);
			uint32_t q = num / v;
			if (q > 0xFFFF) {
				m.log("divide overflow at %s", where().c_str());
				doInterrupt(0, false);
				return;
			}
			set16(R_AX, (uint16_t)q);
			set16(R_DX, (uint16_t)(num % v));
		} else {
			uint64_t num = ((uint64_t)r[R_DX] << 32) | r[R_AX];
			uint64_t q = num / v;
			if (q > 0xFFFFFFFFull) {
				doInterrupt(0, false);
				return;
			}
			r[R_AX] = (uint32_t)q;
			r[R_DX] = (uint32_t)(num % v);
		}
		break;
	}
	case 7: { // idiv
		if (v == 0) {
			m.log("divide by zero at %s", where().c_str());
			doInterrupt(0, false);
			return;
		}
		if (size == 1) {
			int32_t num = (int16_t)r[R_AX];
			int32_t d = (int8_t)v;
			int32_t q = num / d;
			if (q != (int8_t)q) {
				doInterrupt(0, false);
				return;
			}
			setAL((uint8_t)q);
			setAH((uint8_t)(num % d));
		} else if (size == 2) {
			int32_t num = (int32_t)(((r[R_DX] & 0xFFFF) << 16) | (r[R_AX] & 0xFFFF));
			int32_t d = (int16_t)v;
			int32_t q = num / d;
			if (q != (int16_t)q) {
				m.log("idiv overflow at %s", where().c_str());
				doInterrupt(0, false);
				return;
			}
			set16(R_AX, (uint16_t)q);
			set16(R_DX, (uint16_t)(num % d));
		} else {
			int64_t num = (int64_t)(((uint64_t)r[R_DX] << 32) | r[R_AX]);
			int64_t d = (int32_t)v;
			int64_t q = num / d;
			if (q != (int32_t)q) {
				doInterrupt(0, false);
				return;
			}
			r[R_AX] = (uint32_t)q;
			r[R_DX] = (uint32_t)(num % d);
		}
		break;
	}
	}
}

void Cpu::grp5(int size) {
	switch (mrReg) {
	case 0:
		writeRM(size, incdec(false, size, readRM(size)));
		break;
	case 1:
		writeRM(size, incdec(true, size, readRM(size)));
		break;
	case 2: { // call near
		uint32_t t = readRM(size);
		if (size == 4)
			push32(eip);
		else
			push16((uint16_t)eip);
		eip = t & 0xFFFF;
		break;
	}
	case 3: { // call far m16:16
		uint32_t off = rd(eaSeg, eaOff, size);
		uint16_t sel = (uint16_t)rd(eaSeg, eaOff + size, 2);
		farCall(sel, off, size);
		break;
	}
	case 4:
		eip = readRM(size) & 0xFFFF;
		break;
	case 5: {
		uint32_t off = rd(eaSeg, eaOff, size);
		uint16_t sel = (uint16_t)rd(eaSeg, eaOff + size, 2);
		farJump(sel, off);
		break;
	}
	case 6: {
		uint32_t v = readRM(size);
		if (size == 4)
			push32(v);
		else
			push16((uint16_t)v);
		break;
	}
	default:
		throw CpuException("invalid grp5 op at " + where());
	}
}

void Cpu::stringOp(uint8_t op, int size) {
	int srcSeg = segOvr != S_NONE ? segOvr : S_DS;
	int delta = (flags & F_DF) ? -size : size;
	auto getSI = [&]() -> uint32_t { return adsz32 ? r[R_SI] : (r[R_SI] & 0xFFFF); };
	auto getDI = [&]() -> uint32_t { return adsz32 ? r[R_DI] : (r[R_DI] & 0xFFFF); };
	auto addSI = [&]() {
		if (adsz32)
			r[R_SI] += delta;
		else
			set16(R_SI, (uint16_t)(r[R_SI] + delta));
	};
	auto addDI = [&]() {
		if (adsz32)
			r[R_DI] += delta;
		else
			set16(R_DI, (uint16_t)(r[R_DI] + delta));
	};
	auto count = [&]() -> uint32_t { return adsz32 ? r[R_CX] : (r[R_CX] & 0xFFFF); };
	auto decCount = [&]() {
		if (adsz32)
			r[R_CX]--;
		else
			set16(R_CX, (uint16_t)(r[R_CX] - 1));
	};

	bool cmpOp = (op == 0xA6 || op == 0xA7 || op == 0xAE || op == 0xAF);
	for (;;) {
		if (rep && count() == 0)
			break;
		switch (op) {
		case 0xA4:
		case 0xA5:
			wr(S_ES, getDI(), size, rd(srcSeg, getSI(), size));
			addSI();
			addDI();
			break;
		case 0xA6:
		case 0xA7:
			alu(7, size, rd(srcSeg, getSI(), size), rd(S_ES, getDI(), size));
			addSI();
			addDI();
			break;
		case 0xAA:
		case 0xAB:
			wr(S_ES, getDI(), size, getReg(size, R_AX));
			addDI();
			break;
		case 0xAC:
		case 0xAD:
			setReg(size, R_AX, rd(srcSeg, getSI(), size));
			addSI();
			break;
		case 0xAE:
		case 0xAF:
			alu(7, size, getReg(size, R_AX), rd(S_ES, getDI(), size));
			addDI();
			break;
		case 0x6C:
		case 0x6D:
			wr(S_ES, getDI(), size, size == 1 ? m.in8(dx()) : m.in16(dx()));
			addDI();
			break;
		case 0x6E:
		case 0x6F:
			if (size == 1)
				m.out8(dx(), (uint8_t)rd(srcSeg, getSI(), 1));
			else
				m.out16(dx(), (uint16_t)rd(srcSeg, getSI(), 2));
			addSI();
			break;
		}
		if (!rep)
			break;
		decCount();
		if (cmpOp) {
			bool zf = flags & F_ZF;
			if (rep == 1 && !zf)
				break;
			if (rep == 2 && zf)
				break;
		}
	}
}

void Cpu::run(uint64_t maxInstr) {
	uint64_t end = icount + maxInstr;
	while (icount < end && !halted)
		step();
}

void Cpu::step() {
	curCs = s[S_CS].sel;
	curIp = eip;
	icount++;
	segOvr = S_NONE;
	opsz32 = false;
	adsz32 = false;
	rep = 0;

	uint8_t op;
	for (;;) {
		op = fetch8();
		switch (op) {
		case 0x26: segOvr = S_ES; continue;
		case 0x2E: segOvr = S_CS; continue;
		case 0x36: segOvr = S_SS; continue;
		case 0x3E: segOvr = S_DS; continue;
		case 0x64: segOvr = S_FS; continue;
		case 0x65: segOvr = S_GS; continue;
		case 0x66: opsz32 = true; continue;
		case 0x67: adsz32 = true; continue;
		case 0xF0: continue;
		case 0xF2: rep = 2; continue;
		case 0xF3: rep = 1; continue;
		}
		break;
	}

	int osz = opSize();

	// ALU ops 00-3F (pattern)
	if (op < 0x40 && (op & 7) < 6) {
		int aop = op >> 3;
		int size;
		switch (op & 7) {
		case 0: // rm8, r8
		case 1: // rm16, r16
			size = (op & 1) ? osz : 1;
			decodeModrm();
			{
				uint32_t res = alu(aop, size, readRM(size), getReg(size, mrReg));
				if (aop != 7)
					writeRM(size, res);
			}
			return;
		case 2:
		case 3:
			size = (op & 1) ? osz : 1;
			decodeModrm();
			{
				uint32_t res = alu(aop, size, getReg(size, mrReg), readRM(size));
				if (aop != 7)
					setReg(size, mrReg, res);
			}
			return;
		case 4: {
			uint32_t res = alu(aop, 1, r[R_AX], fetch8());
			if (aop != 7)
				setAL((uint8_t)res);
			return;
		}
		case 5: {
			uint32_t res = alu(aop, osz, r[R_AX], fetchImm(osz));
			if (aop != 7)
				setReg(osz, R_AX, res);
			return;
		}
		}
	}

	switch (op) {
	case 0x06: push16(s[S_ES].sel); break;
	case 0x07: loadSeg(S_ES, pop16()); break;
	case 0x0E: push16(s[S_CS].sel); break;
	case 0x16: push16(s[S_SS].sel); break;
	case 0x17: loadSeg(S_SS, pop16()); break;
	case 0x1E: push16(s[S_DS].sel); break;
	case 0x1F: loadSeg(S_DS, pop16()); break;
	case 0x0F: opcode0F(); break;

	case 0x27: { // daa
		uint8_t al = this->al();
		bool cf = flags & F_CF;
		bool af = false;
		if ((al & 0x0F) > 9 || (flags & F_AF)) {
			cf = cf || al > 0xF9;
			al += 6;
			af = true;
		}
		if (this->al() > 0x99 || (flags & F_CF)) {
			al += 0x60;
			cf = true;
		}
		setAL(al);
		setSZP(1, al);
		flags = (flags & ~(F_CF | F_AF)) | (cf ? (uint32_t)F_CF : 0u) | (af ? (uint32_t)F_AF : 0u);
		break;
	}
	case 0x2F: { // das
		uint8_t al = this->al(), old = al;
		bool cf = flags & F_CF, af = false;
		if ((al & 0x0F) > 9 || (flags & F_AF)) {
			cf = cf || al < 6;
			al -= 6;
			af = true;
		}
		if (old > 0x99 || (flags & F_CF)) {
			al -= 0x60;
			cf = true;
		}
		setAL(al);
		setSZP(1, al);
		flags = (flags & ~(F_CF | F_AF)) | (cf ? (uint32_t)F_CF : 0u) | (af ? (uint32_t)F_AF : 0u);
		break;
	}
	case 0x37: // aaa
		if ((al() & 0x0F) > 9 || (flags & F_AF)) {
			set16(R_AX, (uint16_t)(ax() + 0x106));
			flags |= F_AF | F_CF;
		} else {
			flags &= ~(F_AF | F_CF);
		}
		setAL(al() & 0x0F);
		break;
	case 0x3F: // aas
		if ((al() & 0x0F) > 9 || (flags & F_AF)) {
			set16(R_AX, (uint16_t)(ax() - 6));
			setAH(ah() - 1);
			flags |= F_AF | F_CF;
		} else {
			flags &= ~(F_AF | F_CF);
		}
		setAL(al() & 0x0F);
		break;

	case 0x40: case 0x41: case 0x42: case 0x43:
	case 0x44: case 0x45: case 0x46: case 0x47: {
		bool cf = flags & F_CF;
		setReg(osz, op & 7, incdec(false, osz, getReg(osz, op & 7)));
		setCF(cf);
		break;
	}
	case 0x48: case 0x49: case 0x4A: case 0x4B:
	case 0x4C: case 0x4D: case 0x4E: case 0x4F: {
		bool cf = flags & F_CF;
		setReg(osz, op & 7, incdec(true, osz, getReg(osz, op & 7)));
		setCF(cf);
		break;
	}
	case 0x50: case 0x51: case 0x52: case 0x53:
	case 0x54: case 0x55: case 0x56: case 0x57:
		if (osz == 4)
			push32(r[op & 7]);
		else
			push16((uint16_t)r[op & 7]);
		break;
	case 0x58: case 0x59: case 0x5A: case 0x5B:
	case 0x5C: case 0x5D: case 0x5E: case 0x5F:
		if (osz == 4)
			r[op & 7] = pop32();
		else
			set16(op & 7, pop16());
		break;
	case 0x60: { // pusha
		uint32_t sp = r[R_SP];
		for (int i = 0; i < 8; i++) {
			uint32_t v = i == R_SP ? sp : r[i];
			if (osz == 4)
				push32(v);
			else
				push16((uint16_t)v);
		}
		break;
	}
	case 0x61: // popa
		for (int i = 7; i >= 0; i--) {
			if (osz == 4) {
				uint32_t v = pop32();
				if (i != R_SP)
					r[i] = v;
			} else {
				uint16_t v = pop16();
				if (i != R_SP)
					set16(i, v);
			}
		}
		break;
	case 0x62: // bound
		decodeModrm();
		break;
	case 0x63: // arpl
		decodeModrm();
		setZF(false);
		break;
	case 0x68:
		if (osz == 4)
			push32(fetch32());
		else
			push16(fetch16());
		break;
	case 0x6A: {
		int32_t v = (int8_t)fetch8();
		if (osz == 4)
			push32((uint32_t)v);
		else
			push16((uint16_t)v);
		break;
	}
	case 0x69:
	case 0x6B: {
		decodeModrm();
		int32_t a = sext(osz, readRM(osz));
		int32_t b = op == 0x6B ? (int8_t)fetch8() : sext(osz, fetchImm(osz));
		int64_t res = (int64_t)a * b;
		setReg(osz, mrReg, (uint32_t)res);
		bool ov = osz == 2 ? res != (int16_t)res : res != (int32_t)res;
		flags = (flags & ~(F_CF | F_OF)) | (ov ? (uint32_t)(F_CF | F_OF) : 0u);
		break;
	}
	case 0x6C: case 0x6D: case 0x6E: case 0x6F:
		stringOp(op, (op & 1) ? osz : 1);
		break;

	case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
	case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
		int8_t d = (int8_t)fetch8();
		if (cond(op & 0xF)) {
			eip = (eip + d) & 0xFFFF;
			if (d < 0 && d >= -24)
				tightLoops++;
		}
		break;
	}
	case 0x80:
	case 0x82:
	case 0x81:
	case 0x83: {
		int size = (op == 0x81 || op == 0x83) ? osz : 1;
		decodeModrm();
		uint32_t a = readRM(size);
		uint32_t b = op == 0x81 ? fetchImm(size) : (op == 0x83 ? (uint32_t)(int32_t)(int8_t)fetch8() : fetch8());
		uint32_t res = alu(mrReg, size, a, b);
		if (mrReg != 7)
			writeRM(size, res);
		break;
	}
	case 0x84:
	case 0x85: {
		int size = op == 0x85 ? osz : 1;
		decodeModrm();
		alu(4, size, readRM(size), getReg(size, mrReg));
		break;
	}
	case 0x86:
	case 0x87: {
		int size = op == 0x87 ? osz : 1;
		decodeModrm();
		uint32_t a = readRM(size);
		writeRM(size, getReg(size, mrReg));
		setReg(size, mrReg, a);
		break;
	}
	case 0x88: decodeModrm(); writeRM(1, getReg(1, mrReg)); break;
	case 0x89: decodeModrm(); writeRM(osz, getReg(osz, mrReg)); break;
	case 0x8A: decodeModrm(); setReg(1, mrReg, readRM(1)); break;
	case 0x8B: decodeModrm(); setReg(osz, mrReg, readRM(osz)); break;
	case 0x8C:
		decodeModrm();
		if (eaIsReg)
			setReg(osz, mrRm, s[mrReg].sel);
		else
			writeRM(2, s[mrReg].sel);
		break;
	case 0x8D:
		decodeModrm();
		setReg(osz, mrReg, eaOff);
		break;
	case 0x8E:
		decodeModrm();
		loadSeg(mrReg, (uint16_t)readRM(2));
		break;
	case 0x8F: {
		// pop must happen before the effective address is computed for sp-relative modes
		uint32_t v = osz == 4 ? pop32() : pop16();
		decodeModrm();
		writeRM(osz, v);
		break;
	}
	case 0x90: break;
	case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
		uint32_t a = getReg(osz, R_AX);
		setReg(osz, R_AX, getReg(osz, op & 7));
		setReg(osz, op & 7, a);
		break;
	}
	case 0x98:
		if (osz == 4)
			r[R_AX] = (uint32_t)(int32_t)(int16_t)r[R_AX];
		else
			set16(R_AX, (uint16_t)(int16_t)(int8_t)r[R_AX]);
		break;
	case 0x99:
		if (osz == 4)
			r[R_DX] = (r[R_AX] & 0x80000000u) ? 0xFFFFFFFFu : 0;
		else
			set16(R_DX, (r[R_AX] & 0x8000) ? 0xFFFF : 0);
		break;
	case 0x9A: {
		uint32_t off = fetchImm(osz);
		uint16_t sel = fetch16();
		farCall(sel, off, osz);
		break;
	}
	case 0x9B: break;
	case 0x9C:
		if (osz == 4)
			push32(flags & 0x00FCFFFF);
		else
			push16((uint16_t)flags);
		break;
	case 0x9D:
		if (osz == 4)
			flags = (pop32() & 0x00247FD5) | 2;
		else
			flags = (flags & 0xFFFF0000u) | (pop16() & 0x7FD5) | 2;
		break;
	case 0x9E:
		flags = (flags & ~0xD5u) | (ah() & 0xD5) | 2;
		break;
	case 0x9F:
		setAH((uint8_t)(flags & 0xD7) | 2);
		break;
	case 0xA0:
	case 0xA1:
	case 0xA2:
	case 0xA3: {
		uint32_t off = adsz32 ? fetch32() : fetch16();
		int seg = segOvr != S_NONE ? segOvr : S_DS;
		int size = (op & 1) ? osz : 1;
		if (op < 0xA2)
			setReg(size, R_AX, rd(seg, off, size));
		else
			wr(seg, off, size, getReg(size, R_AX));
		break;
	}
	case 0xA4: case 0xA5: case 0xA6: case 0xA7:
	case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
		stringOp(op, (op & 1) ? osz : 1);
		break;
	case 0xA8: alu(4, 1, r[R_AX], fetch8()); break;
	case 0xA9: alu(4, osz, r[R_AX], fetchImm(osz)); break;
	case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
		setReg(1, op & 7, fetch8());
		break;
	case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
		setReg(osz, op & 7, fetchImm(osz));
		break;
	case 0xC0:
	case 0xC1:
	case 0xD0:
	case 0xD1:
	case 0xD2:
	case 0xD3: {
		int size = (op & 1) ? osz : 1;
		decodeModrm();
		uint8_t cnt;
		uint32_t v = readRM(size);
		if (op <= 0xC1)
			cnt = fetch8();
		else if (op <= 0xD1)
			cnt = 1;
		else
			cnt = cl();
		writeRM(size, shift(mrReg, size, v, cnt));
		break;
	}
	case 0xC2: {
		uint16_t n = fetch16();
		eip = osz == 4 ? pop32() : pop16();
		set16(R_SP, (uint16_t)(r[R_SP] + n));
		break;
	}
	case 0xC3:
		eip = osz == 4 ? (pop32() & 0xFFFF) : pop16();
		break;
	case 0xC4:
	case 0xC5: {
		decodeModrm();
		uint32_t off = rd(eaSeg, eaOff, osz);
		uint16_t sel = (uint16_t)rd(eaSeg, eaOff + osz, 2);
		loadSeg(op == 0xC4 ? S_ES : S_DS, sel);
		setReg(osz, mrReg, off);
		break;
	}
	case 0xC6: decodeModrm(); writeRM(1, fetch8()); break;
	case 0xC7: decodeModrm(); writeRM(osz, fetchImm(osz)); break;
	case 0xC8: { // enter
		uint16_t size = fetch16();
		uint8_t level = fetch8() & 31;
		push16(bp());
		uint16_t frame = sp();
		if (level > 0) {
			uint16_t bpv = bp();
			for (int i = 1; i < level; i++) {
				bpv -= 2;
				push16(m.read16(s[S_SS].base + bpv));
			}
			push16(frame);
		}
		set16(R_BP, frame);
		set16(R_SP, (uint16_t)(sp() - size));
		break;
	}
	case 0xC9: // leave
		set16(R_SP, bp());
		set16(R_BP, pop16());
		break;
	case 0xCA: {
		uint16_t n = fetch16();
		if (osz == 4) {
			uint32_t ip = pop32();
			uint16_t cs = (uint16_t)pop32();
			set16(R_SP, (uint16_t)(sp() + n));
			farJump(cs, ip);
		} else {
			farReturn(n);
		}
		break;
	}
	case 0xCB:
		if (osz == 4) {
			uint32_t ip = pop32();
			uint16_t cs = (uint16_t)pop32();
			farJump(cs, ip);
		} else {
			farReturn(0);
		}
		break;
	case 0xCC: m.softInt(3); break;
	case 0xCD: m.softInt(fetch8()); break;
	case 0xCE:
		if (flags & F_OF)
			m.softInt(4);
		break;
	case 0xCF:
		if (osz == 4) {
			uint32_t ip = pop32();
			uint16_t cs = (uint16_t)pop32();
			uint32_t fl = pop32();
			farJump(cs, ip);
			flags = (fl & 0x00247FD5) | 2;
		} else {
			iret();
		}
		break;
	case 0xD4: { // aam
		uint8_t base = fetch8();
		if (base == 0) {
			doInterrupt(0, false);
			break;
		}
		uint8_t al = this->al();
		setAH(al / base);
		setAL(al % base);
		setSZP(1, this->al());
		break;
	}
	case 0xD5: { // aad
		uint8_t base = fetch8();
		uint8_t v = (uint8_t)(al() + ah() * base);
		set16(R_AX, v);
		setSZP(1, v);
		break;
	}
	case 0xD6: setAL((flags & F_CF) ? 0xFF : 0); break;
	case 0xD7: {
		int seg = segOvr != S_NONE ? segOvr : S_DS;
		setAL((uint8_t)rd(seg, (uint16_t)(bx() + al()), 1));
		break;
	}
	case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:
		decodeModrm();
		m.log("FPU instruction %02X at %s ignored", op, where().c_str());
		break;
	case 0xE0:
	case 0xE1:
	case 0xE2: {
		int8_t d = (int8_t)fetch8();
		uint32_t c;
		if (adsz32) {
			c = --r[R_CX];
		} else {
			set16(R_CX, (uint16_t)(cx() - 1));
			c = cx();
		}
		bool take = c != 0;
		if (op == 0xE0)
			take = take && !(flags & F_ZF);
		else if (op == 0xE1)
			take = take && (flags & F_ZF);
		if (take)
			eip = (eip + d) & 0xFFFF;
		break;
	}
	case 0xE3: {
		int8_t d = (int8_t)fetch8();
		if ((adsz32 ? r[R_CX] : cx()) == 0)
			eip = (eip + d) & 0xFFFF;
		break;
	}
	case 0xE4: setAL(m.in8(fetch8())); break;
	case 0xE5: {
		uint8_t p = fetch8();
		setReg(osz, R_AX, m.in16(p));
		break;
	}
	case 0xE6: m.out8(fetch8(), al()); break;
	case 0xE7: m.out16(fetch8(), ax()); break;
	case 0xE8: {
		int32_t d = osz == 4 ? (int32_t)fetch32() : (int16_t)fetch16();
		if (osz == 4)
			push32(eip);
		else
			push16((uint16_t)eip);
		eip = (eip + d) & 0xFFFF;
		break;
	}
	case 0xE9: {
		int32_t d = osz == 4 ? (int32_t)fetch32() : (int16_t)fetch16();
		eip = (eip + d) & 0xFFFF;
		break;
	}
	case 0xEA: {
		uint32_t off = fetchImm(osz);
		uint16_t sel = fetch16();
		farJump(sel, off);
		break;
	}
	case 0xEB: {
		int8_t d = (int8_t)fetch8();
		eip = (eip + d) & 0xFFFF;
		break;
	}
	case 0xEC: setAL(m.in8(dx())); break;
	case 0xED: setReg(osz, R_AX, m.in16(dx())); break;
	case 0xEE: m.out8(dx(), al()); break;
	case 0xEF: m.out16(dx(), ax()); break;
	case 0xF1: { // host trap
		uint16_t id = fetch16();
		m.trap(id);
		break;
	}
	case 0xF4:
		m.hlt();
		break;
	case 0xF5: flags ^= F_CF; break;
	case 0xF6: decodeModrm(); grp3(1); break;
	case 0xF7: decodeModrm(); grp3(osz); break;
	case 0xF8: flags &= ~F_CF; break;
	case 0xF9: flags |= F_CF; break;
	case 0xFA: flags &= ~F_IF; break;
	case 0xFB: flags |= F_IF; break;
	case 0xFC: flags &= ~F_DF; break;
	case 0xFD: flags |= F_DF; break;
	case 0xFE:
		decodeModrm();
		if (mrReg == 0 || mrReg == 1) {
			bool cf = flags & F_CF;
			writeRM(1, incdec(mrReg == 1, 1, readRM(1)));
			setCF(cf);
		} else {
			throw CpuException("invalid FE op at " + where());
		}
		break;
	case 0xFF:
		decodeModrm();
		if (mrReg <= 1) {
			bool cf = flags & F_CF;
			grp5(osz);
			setCF(cf);
		} else {
			grp5(osz);
		}
		break;
	default: {
		char buf[96];
		snprintf(buf, sizeof(buf), "unimplemented opcode %02X at %s", op, where().c_str());
		throw CpuException(buf);
	}
	}
}

void Cpu::opcode0F() {
	uint8_t op = fetch8();
	int osz = opSize();
	if (op >= 0x80 && op <= 0x8F) {
		int32_t d = osz == 4 ? (int32_t)fetch32() : (int16_t)fetch16();
		if (cond(op & 0xF))
			eip = (eip + d) & 0xFFFF;
		return;
	}
	if (op >= 0x90 && op <= 0x9F) {
		decodeModrm();
		writeRM(1, cond(op & 0xF) ? 1 : 0);
		return;
	}
	switch (op) {
	case 0x00: { // sldt/str/lldt/ltr/verr/verw
		decodeModrm();
		uint16_t sel = (uint16_t)readRM(2);
		if (mrReg == 4 || mrReg == 5) {
			uint32_t b, l;
			setZF(m.descriptor(sel, b, l));
		} else if (mrReg == 0 || mrReg == 1) {
			writeRM(2, 0);
		}
		break;
	}
	case 0x01: // smsw etc.
		decodeModrm();
		if (mrReg == 4)
			writeRM(2, 0xFFF1); // PE set
		break;
	case 0x02: { // lar
		decodeModrm();
		uint16_t sel = (uint16_t)readRM(2);
		uint32_t b, l;
		if (m.descriptor(sel, b, l)) {
			setReg(osz, mrReg, m.descriptorAccess(sel) << 8);
			setZF(true);
		} else {
			setZF(false);
		}
		break;
	}
	case 0x03: { // lsl
		decodeModrm();
		uint16_t sel = (uint16_t)readRM(2);
		uint32_t b, l;
		if (m.descriptor(sel, b, l)) {
			setReg(osz, mrReg, l);
			setZF(true);
		} else {
			setZF(false);
		}
		break;
	}
	case 0x06: break; // clts
	case 0xA0: push16(s[S_FS].sel); break;
	case 0xA1: loadSeg(S_FS, pop16()); break;
	case 0xA8: push16(s[S_GS].sel); break;
	case 0xA9: loadSeg(S_GS, pop16()); break;
	case 0xA2: // cpuid -> pretend 386
		throw CpuException("cpuid at " + where());
	case 0xA3:
	case 0xAB:
	case 0xB3:
	case 0xBB: {
		decodeModrm();
		uint32_t bit = getReg(osz, mrReg);
		uint32_t v;
		if (eaIsReg) {
			bit &= osz * 8 - 1;
			v = readRM(osz);
		} else {
			int32_t sb = sext(osz, bit);
			eaOff += (sb >> (osz == 4 ? 5 : 4)) * osz;
			if (!adsz32)
				eaOff &= 0xFFFF;
			bit &= osz * 8 - 1;
			v = readRM(osz);
		}
		setCF((v >> bit) & 1);
		if (op == 0xAB)
			writeRM(osz, v | (1u << bit));
		else if (op == 0xB3)
			writeRM(osz, v & ~(1u << bit));
		else if (op == 0xBB)
			writeRM(osz, v ^ (1u << bit));
		break;
	}
	case 0xBA: {
		decodeModrm();
		uint32_t v = readRM(osz);
		uint8_t bit = fetch8() & (osz * 8 - 1);
		setCF((v >> bit) & 1);
		if (mrReg == 5)
			writeRM(osz, v | (1u << bit));
		else if (mrReg == 6)
			writeRM(osz, v & ~(1u << bit));
		else if (mrReg == 7)
			writeRM(osz, v ^ (1u << bit));
		break;
	}
	case 0xA4:
	case 0xA5:
	case 0xAC:
	case 0xAD: {
		decodeModrm();
		uint8_t cnt = (op == 0xA4 || op == 0xAC) ? fetch8() : cl();
		cnt &= 31;
		if (cnt == 0)
			break;
		int bits = osz * 8;
		uint64_t dst = readRM(osz), src = getReg(osz, mrReg);
		uint32_t res;
		bool cf;
		if (op == 0xA4 || op == 0xA5) { // shld
			uint64_t comb = (dst << bits) | src;
			comb <<= cnt;
			res = (uint32_t)(comb >> bits) & (uint32_t)((1ull << bits) - 1);
			cf = (dst >> (bits - cnt)) & 1;
		} else { // shrd
			uint64_t comb = (src << bits) | dst;
			cf = (dst >> (cnt - 1)) & 1;
			comb >>= cnt;
			res = (uint32_t)comb & (uint32_t)((1ull << bits) - 1);
		}
		writeRM(osz, res);
		setSZP(osz, res);
		setCF(cf);
		break;
	}
	case 0xAF: {
		decodeModrm();
		int64_t res = (int64_t)sext(osz, getReg(osz, mrReg)) * sext(osz, readRM(osz));
		setReg(osz, mrReg, (uint32_t)res);
		bool ov = osz == 2 ? res != (int16_t)res : res != (int32_t)res;
		flags = (flags & ~(F_CF | F_OF)) | (ov ? (uint32_t)(F_CF | F_OF) : 0u);
		break;
	}
	case 0xB2:
	case 0xB4:
	case 0xB5: {
		decodeModrm();
		uint32_t off = rd(eaSeg, eaOff, osz);
		uint16_t sel = (uint16_t)rd(eaSeg, eaOff + osz, 2);
		loadSeg(op == 0xB2 ? S_SS : op == 0xB4 ? S_FS : S_GS, sel);
		setReg(osz, mrReg, off);
		break;
	}
	case 0xB6: decodeModrm(); setReg(osz, mrReg, readRM(1)); break;
	case 0xB7: decodeModrm(); setReg(osz, mrReg, readRM(2)); break;
	case 0xBE: decodeModrm(); setReg(osz, mrReg, (uint32_t)(int32_t)(int8_t)readRM(1)); break;
	case 0xBF: decodeModrm(); setReg(osz, mrReg, (uint32_t)(int32_t)(int16_t)readRM(2)); break;
	case 0xBC:
	case 0xBD: {
		decodeModrm();
		uint32_t v = readRM(osz);
		if (v == 0) {
			setZF(true);
			break;
		}
		setZF(false);
		int i;
		if (op == 0xBC) {
			for (i = 0; !((v >> i) & 1); i++) {}
		} else {
			for (i = osz * 8 - 1; !((v >> i) & 1); i--) {}
		}
		setReg(osz, mrReg, i);
		break;
	}
	default: {
		char buf[96];
		snprintf(buf, sizeof(buf), "unimplemented opcode 0F %02X at %s", op, where().c_str());
		throw CpuException(buf);
	}
	}
}
