// x86 interpreter for 16-bit protected-mode code (286 instruction set plus the
// 386 extensions Borland Pascal 7 programs use: operand-size prefix, movzx,
// shld/shrd, setcc, ...). Segment registers hold selectors that are resolved
// through the host-managed descriptor table.
#pragma once

#include <cstdint>
#include <string>

class Machine;

enum { R_AX, R_CX, R_DX, R_BX, R_SP, R_BP, R_SI, R_DI };
enum { S_ES, S_CS, S_SS, S_DS, S_FS, S_GS, S_NONE = 7 };

enum : uint32_t {
	F_CF = 1 << 0,
	F_PF = 1 << 2,
	F_AF = 1 << 4,
	F_ZF = 1 << 6,
	F_SF = 1 << 7,
	F_TF = 1 << 8,
	F_IF = 1 << 9,
	F_DF = 1 << 10,
	F_OF = 1 << 11,
};

struct SegReg {
	uint16_t sel = 0;
	uint32_t base = 0;
	uint32_t limit = 0xFFFF;
};

class CpuException {
public:
	explicit CpuException(std::string m) : msg(std::move(m)) {}
	std::string msg;
};

class Cpu {
public:
	explicit Cpu(Machine &m);

	uint32_t r[8] = {};
	SegReg s[6];
	uint32_t eip = 0;
	uint32_t flags = 0x0202;

	// set when an instruction requests the run loop to stop (HLE exit, etc.)
	bool halted = false;
	uint64_t icount = 0;
	// taken short backward jumps over a few bytes: busy-wait loops
	uint32_t tightLoops = 0;

	// address of the instruction being executed (for diagnostics)
	uint16_t curCs = 0;
	uint32_t curIp = 0;

	void run(uint64_t maxInstr);
	void step();

	// segment helpers
	void loadSeg(int seg, uint16_t sel);

	// register accessors
	uint16_t ax() const { return (uint16_t)r[R_AX]; }
	uint16_t bx() const { return (uint16_t)r[R_BX]; }
	uint16_t cx() const { return (uint16_t)r[R_CX]; }
	uint16_t dx() const { return (uint16_t)r[R_DX]; }
	uint16_t si() const { return (uint16_t)r[R_SI]; }
	uint16_t di() const { return (uint16_t)r[R_DI]; }
	uint16_t sp() const { return (uint16_t)r[R_SP]; }
	uint16_t bp() const { return (uint16_t)r[R_BP]; }
	uint8_t al() const { return (uint8_t)r[R_AX]; }
	uint8_t ah() const { return (uint8_t)(r[R_AX] >> 8); }
	uint8_t bl() const { return (uint8_t)r[R_BX]; }
	uint8_t bh() const { return (uint8_t)(r[R_BX] >> 8); }
	uint8_t cl() const { return (uint8_t)r[R_CX]; }
	uint8_t ch() const { return (uint8_t)(r[R_CX] >> 8); }
	uint8_t dl() const { return (uint8_t)r[R_DX]; }
	uint8_t dh() const { return (uint8_t)(r[R_DX] >> 8); }
	void set16(int reg, uint16_t v) { r[reg] = (r[reg] & 0xFFFF0000) | v; }
	void setAL(uint8_t v) { r[R_AX] = (r[R_AX] & ~0xFFu) | v; }
	void setAH(uint8_t v) { r[R_AX] = (r[R_AX] & ~0xFF00u) | (v << 8); }
	void setCF(bool c) { flags = c ? (flags | F_CF) : (flags & ~F_CF); }
	void setZF(bool c) { flags = c ? (flags | F_ZF) : (flags & ~F_ZF); }

	// stack (16-bit stack segment)
	void push16(uint16_t v);
	uint16_t pop16();
	void push32(uint32_t v);
	uint32_t pop32();
	uint16_t stackWord(int idx) const; // word at SS:SP + 2*idx

	// software/hardware interrupt entry through the protected-mode vector table
	void doInterrupt(int n, bool software);
	// far call from host into guest code; returns when the callee returns
	// (used for mouse callbacks)
	void farReturn(int popBytes);
	void iret();

	std::string where() const;

private:
	Machine &m;

	// decode state
	int segOvr;
	bool opsz32, adsz32;
	int rep; // 0 none, 1 REPE/REP (F3), 2 REPNE (F2)

	// modrm
	uint8_t modrm;
	int mrMod, mrReg, mrRm;
	uint32_t eaOff;
	int eaSeg;
	bool eaIsReg;

	uint8_t fetch8();
	uint16_t fetch16();
	uint32_t fetch32();
	uint32_t fetchImm(int size) { return size == 1 ? fetch8() : size == 2 ? fetch16() : fetch32(); }

	void decodeModrm();
	uint32_t linear(int seg, uint32_t off) const;

	uint32_t readRM(int size);
	void writeRM(int size, uint32_t v);
	uint32_t getReg(int size, int reg) const;
	void setReg(int size, int reg, uint32_t v);

	uint32_t rd(int seg, uint32_t off, int size);
	void wr(int seg, uint32_t off, int size, uint32_t v);

	// ALU
	uint32_t alu(int op, int size, uint32_t a, uint32_t b);
	uint32_t incdec(bool dec, int size, uint32_t a);
	uint32_t shift(int op, int size, uint32_t a, uint8_t cnt);
	void setSZP(int size, uint32_t res);
	bool cond(int cc) const;
	void grp3(int size);
	void grp5(int size);
	void stringOp(uint8_t op, int size);
	void opcode0F();

	void farJump(uint16_t sel, uint32_t off);
	void farCall(uint16_t sel, uint32_t off, int size);

	int opSize() const { return opsz32 ? 4 : 2; }
	uint32_t ipMask() const { return 0xFFFF; }
};
