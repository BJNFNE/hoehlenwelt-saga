// Port I/O: PIC, PIT, keyboard controller, VGA DAC, DMA, Sound Blaster DSP,
// MPU-401 (UART mode) and the AdLib/OPL ports.
#include <algorithm>

#include "host.h"
#include "machine.h"
#include "midi.h"

uint8_t Machine::in8(uint16_t port) {
	switch (port) {
	case 0x20:
	case 0xA0: {
		int p = port == 0xA0;
		return picReadIsr[p] ? picIsr[p] : picIrr[p];
	}
	case 0x21: return picMask[0];
	case 0xA1: return picMask[1];
	case 0x40:
	case 0x41:
	case 0x42: {
		int ch = port - 0x40;
		uint16_t v;
		if (pitLatched[ch]) {
			v = pitLatch[ch];
		} else {
			// free-running down counter derived from real time
			uint32_t div = pitDivisor[ch] ? pitDivisor[ch] : 65536;
			uint64_t ticks = (nowUs() - startUs) * 1193182ull / 1000000ull;
			v = (uint16_t)(div - (ticks % div));
		}
		uint8_t out;
		if (pitReadLow[ch]) {
			out = (uint8_t)v;
		} else {
			out = (uint8_t)(v >> 8);
			pitLatched[ch] = false;
		}
		pitReadLow[ch] = !pitReadLow[ch];
		return out;
	}
	case 0x60:
		kbdPending = false;
		return kbdData;
	case 0x61:
		return (uint8_t)(((nowUs() / 15) & 1) << 4);
	case 0x64:
		return kbdPending ? 0x1D : 0x1C;
	case 0x3C7: return 3;
	case 0x3C8: return (uint8_t)dacWriteIdx;
	case 0x3C9: {
		uint8_t v = dac[dacReadIdx][dacReadComp];
		if (++dacReadComp == 3) {
			dacReadComp = 0;
			dacReadIdx = (dacReadIdx + 1) & 0xFF;
		}
		return v;
	}
	case 0x3C6: return dacMask;
	case 0x3DA:
	case 0x3BA: {
		// 70 Hz refresh, vertical retrace during the first 1.2 ms of a frame
		uint64_t t = (nowUs() - startUs) % 14286;
		uint8_t v = 0;
		if (t < 1200)
			v |= 0x08 | 0x01;
		else if ((t / 32) & 1)
			v |= 0x01;
		idleSpins++;
		if ((idleSpins & 0x3FF) == 0)
			service();
		return v;
	}
	case 0x388:
	case 0x389:
		return 0x06; // no OPL timers -> AdLib not detected
	case 0x330:
		if (!mpuIn.empty()) {
			uint8_t v = mpuIn.front();
			mpuIn.pop_front();
			return v;
		}
		return 0xFE;
	case 0x331:
		return mpuIn.empty() ? 0x80 : 0x00;
	default:
		break;
	}
	if (port >= sbBase && port < sbBase + 0x10) {
		switch (port - sbBase) {
		case 0x0A:
			if (!sbOut.empty()) {
				uint8_t v = sbOut.front();
				sbOut.pop_front();
				return v;
			}
			return 0xFF;
		case 0x0C:
			return 0x7F; // ready for commands
		case 0x0E:
			return sbOut.empty() ? 0x7F : 0xFF;
		case 0x0F:
			return 0xFF;
		case 0x08:
			return 0x06;
		default:
			return 0xFF;
		}
	}
	if (port <= 0x0F) {
		int ch = (port >> 1) & 3;
		uint16_t v = (port & 1) ? dmaCount[ch] : dmaAddr[ch];
		uint8_t out = dmaFlip ? (uint8_t)(v >> 8) : (uint8_t)v;
		dmaFlip = !dmaFlip;
		return out;
	}
	return 0xFF;
}

void Machine::out8(uint16_t port, uint8_t v) {
	switch (port) {
	case 0x20:
	case 0xA0: {
		int p = port == 0xA0;
		if ((v & 0x18) == 0x08) { // OCW3
			if (v & 2)
				picReadIsr[p] = v & 1;
		} else if (v & 0x20) {
			picEoi(p);
		}
		return;
	}
	case 0x21: picMask[0] = v; return;
	case 0xA1: picMask[1] = v; return;
	case 0x40:
	case 0x41:
	case 0x42: {
		int ch = port - 0x40;
		if (pitLowByte[ch]) {
			pitDivisor[ch] = (pitDivisor[ch] & 0xFF00) | v;
		} else {
			pitDivisor[ch] = (pitDivisor[ch] & 0x00FF) | (v << 8);
		}
		pitLowByte[ch] = !pitLowByte[ch];
		return;
	}
	case 0x43: {
		int ch = v >> 6;
		if (ch == 3)
			return;
		if ((v & 0x30) == 0) {
			uint32_t div = pitDivisor[ch] ? pitDivisor[ch] : 65536;
			uint64_t ticks = (nowUs() - startUs) * 1193182ull / 1000000ull;
			pitLatch[ch] = (uint16_t)(div - (ticks % div));
			pitLatched[ch] = true;
			pitReadLow[ch] = true;
			return;
		}
		pitMode[ch] = v;
		pitLowByte[ch] = true;
		pitReadLow[ch] = true;
		return;
	}
	case 0x60:
	case 0x61:
	case 0x64:
		return;
	case 0x3C6: dacMask = v; return;
	case 0x3C7: dacReadIdx = v; dacReadComp = 0; return;
	case 0x3C8: dacWriteIdx = v; dacComp = 0; return;
	case 0x3C9:
		dac[dacWriteIdx][dacComp] = v & 63;
		if (++dacComp == 3) {
			dacComp = 0;
			dacWriteIdx = (dacWriteIdx + 1) & 0xFF;
		}
		return;
	case 0x388: oplIndex = v; return;
	case 0x389: midi.oplWrite(oplIndex, v); return;
	case 0x330:
		midi.sendByte(v);
		return;
	case 0x331:
		if (v == 0xFF) {
			mpuUart = false;
			mpuIn.clear();
			mpuIn.push_back(0xFE);
		} else if (v == 0x3F) {
			mpuUart = true;
			mpuIn.clear();
			mpuIn.push_back(0xFE);
		} else {
			mpuIn.push_back(0xFE);
		}
		return;
	case 0x0A: return; // DMA mask
	case 0x0B: dmaModeReg[v & 3] = v; return;
	case 0x0C: dmaFlip = false; return;
	case 0x87: dmaPage[0] = v; return;
	case 0x83: dmaPage[1] = v; return;
	case 0x81: dmaPage[2] = v; return;
	case 0x82: dmaPage[3] = v; return;
	default:
		break;
	}
	if (port <= 0x07) {
		int ch = (port >> 1) & 3;
		uint16_t &reg = (port & 1) ? dmaCount[ch] : dmaAddr[ch];
		if (!dmaFlip)
			reg = (reg & 0xFF00) | v;
		else
			reg = (reg & 0x00FF) | (v << 8);
		dmaFlip = !dmaFlip;
		return;
	}
	if (port >= sbBase && port < sbBase + 0x10) {
		switch (port - sbBase) {
		case 0x06:
			if (v & 1) {
				sbResetting = true;
			} else if (sbResetting) {
				sbResetting = false;
				sbOut.clear();
				sbOut.push_back(0xAA);
				sbCmdLen = 0;
				sbCmd.clear();
				host.stopPcm();
			}
			return;
		case 0x0C:
			sbWrite(v);
			return;
		case 0x08: oplIndex = v; return;
		case 0x09: midi.oplWrite(oplIndex, v); return;
		case 0x00:
		case 0x02: oplIndex = v; return;
		case 0x01:
		case 0x03: midi.oplWrite(oplIndex, v); return;
		default:
			return;
		}
	}
}

uint16_t Machine::in16(uint16_t port) {
	return (uint16_t)(in8(port) | (in8((uint16_t)(port + 1)) << 8));
}

void Machine::out16(uint16_t port, uint16_t v) {
	out8(port, (uint8_t)v);
	out8((uint16_t)(port + 1), (uint8_t)(v >> 8));
}

// ------------------------------------------------------------------ Sound Blaster DSP

void Machine::sbWrite(uint8_t v) {
	if (sbCmdLen > 0) {
		sbCmd.push_back(v);
		if ((int)sbCmd.size() < sbCmdLen)
			return;
		sbCmdLen = 0;
		switch (sbCurCmd) {
		case 0x40:
			sbTimeConst = sbCmd[0];
			sbRate = 1000000 / (256 - sbTimeConst);
			break;
		case 0x41:
			sbRate = (sbCmd[0] << 8) | sbCmd[1];
			break;
		case 0x14:
		case 0x91:
			sbStartDma((sbCmd[0] | (sbCmd[1] << 8)) + 1);
			break;
		case 0x48:
			sbDmaLen = (sbCmd[0] | (sbCmd[1] << 8)) + 1;
			break;
		case 0xE0:
			sbOut.push_back((uint8_t)~sbCmd[0]);
			break;
		case 0xE4:
			break;
		case 0x10:
			break;
		default:
			if (sbCurCmd >= 0xC0 && sbCurCmd <= 0xCF)
				sbStartDma((sbCmd[1] | (sbCmd[2] << 8)) + 1);
			break;
		}
		sbCmd.clear();
		return;
	}
	sbCurCmd = v;
	sbCmd.clear();
	switch (v) {
	case 0x10: sbCmdLen = 1; break;
	case 0x14:
	case 0x91: sbCmdLen = 2; break;
	case 0x1C:
	case 0x90:
		sbStartDma(sbDmaLen ? sbDmaLen : 1);
		break;
	case 0x40: sbCmdLen = 1; break;
	case 0x41:
	case 0x42: sbCmdLen = 2; break;
	case 0x48: sbCmdLen = 2; break;
	case 0xD0: host.pausePcm(true); break;
	case 0xD4: host.pausePcm(false); break;
	case 0xD1: sbSpeaker = true; break;
	case 0xD3: sbSpeaker = false; break;
	case 0xD8: sbOut.push_back(sbSpeaker ? 0xFF : 0x00); break;
	case 0xDA: break;
	case 0xE0: sbCmdLen = 1; break;
	case 0xE1:
		sbOut.push_back(4);
		sbOut.push_back(5);
		break;
	case 0xE4: sbCmdLen = 1; break;
	case 0xE8: sbOut.push_back(0); break;
	case 0xF2:
		sbIrqAt = nowUs() + 10;
		sbIrqPending = true;
		break;
	default:
		if (v >= 0xC0 && v <= 0xCF)
			sbCmdLen = 3;
		break;
	}
}

void Machine::sbStartDma(int len) {
	int ch = sbDma & 3;
	uint32_t addr = ((uint32_t)dmaPage[ch] << 16) | dmaAddr[ch];
	int dmaLen = dmaCount[ch] + 1;
	int n = std::min(len, dmaLen);
	if (addr + n > MEM_SIZE)
		n = (int)(MEM_SIZE - addr);
	if (trace)
		log("SB DMA %d bytes at %06X rate %u", n, addr, sbRate);
	host.playPcm(&mem[addr], n, sbRate);
	uint64_t durUs = (uint64_t)n * 1000000ull / std::max<uint32_t>(sbRate, 1000);
	sbIrqAt = nowUs() + durUs;
	sbIrqPending = true;
}
