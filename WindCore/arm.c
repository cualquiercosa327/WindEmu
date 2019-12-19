/* Copyright (c) 2013-2014 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "arm.h"
#include "isa-arm.h"
#include "isa-inlines.h"

static inline enum RegisterBank _ARMSelectBank(enum PrivilegeMode);

void ARMSetPrivilegeMode(struct ARMCore* cpu, enum PrivilegeMode mode) {
	if (mode == cpu->privilegeMode) {
		// Not switching modes after all
		return;
	}

	enum RegisterBank newBank = _ARMSelectBank(mode);
	enum RegisterBank oldBank = _ARMSelectBank(cpu->privilegeMode);
	if (newBank != oldBank) {
		// Switch banked registers
		if (mode == MODE_FIQ || cpu->privilegeMode == MODE_FIQ) {
			int oldFIQBank = oldBank == BANK_FIQ;
			int newFIQBank = newBank == BANK_FIQ;
			cpu->bankedRegisters[oldFIQBank][2] = cpu->gprs[8];
			cpu->bankedRegisters[oldFIQBank][3] = cpu->gprs[9];
			cpu->bankedRegisters[oldFIQBank][4] = cpu->gprs[10];
			cpu->bankedRegisters[oldFIQBank][5] = cpu->gprs[11];
			cpu->bankedRegisters[oldFIQBank][6] = cpu->gprs[12];
			cpu->gprs[8] = cpu->bankedRegisters[newFIQBank][2];
			cpu->gprs[9] = cpu->bankedRegisters[newFIQBank][3];
			cpu->gprs[10] = cpu->bankedRegisters[newFIQBank][4];
			cpu->gprs[11] = cpu->bankedRegisters[newFIQBank][5];
			cpu->gprs[12] = cpu->bankedRegisters[newFIQBank][6];
		}
		cpu->bankedRegisters[oldBank][0] = cpu->gprs[ARM_SP];
		cpu->bankedRegisters[oldBank][1] = cpu->gprs[ARM_LR];
		cpu->gprs[ARM_SP] = cpu->bankedRegisters[newBank][0];
		cpu->gprs[ARM_LR] = cpu->bankedRegisters[newBank][1];

		cpu->bankedSPSRs[oldBank] = cpu->spsr.packed;
		cpu->spsr.packed = cpu->bankedSPSRs[newBank];
	}
	cpu->privilegeMode = mode;
}

static inline enum RegisterBank _ARMSelectBank(enum PrivilegeMode mode) {
	switch (mode) {
	case MODE_USER:
	case MODE_SYSTEM:
		// No banked registers
		return BANK_NONE;
	case MODE_FIQ:
		return BANK_FIQ;
	case MODE_IRQ:
		return BANK_IRQ;
	case MODE_SUPERVISOR:
		return BANK_SUPERVISOR;
	case MODE_ABORT:
		return BANK_ABORT;
	case MODE_UNDEFINED:
		return BANK_UNDEFINED;
	default:
		// This should be unreached
		return BANK_NONE;
	}
}

void ARMReset(struct ARMCore* cpu) {
	int i;
	for (i = 0; i < 16; ++i) {
		cpu->gprs[i] = 0;
	}
	for (i = 0; i < 6; ++i) {
		cpu->bankedRegisters[i][0] = 0;
		cpu->bankedRegisters[i][1] = 0;
		cpu->bankedRegisters[i][2] = 0;
		cpu->bankedRegisters[i][3] = 0;
		cpu->bankedRegisters[i][4] = 0;
		cpu->bankedRegisters[i][5] = 0;
		cpu->bankedRegisters[i][6] = 0;
		cpu->bankedSPSRs[i] = 0;
	}

	cpu->privilegeMode = MODE_SYSTEM;
	cpu->cpsr.packed = MODE_SYSTEM;
	cpu->spsr.packed = 0;

	cpu->shifterOperand = 0;
	cpu->shifterCarryOut = 0;

	ARMWritePC(cpu);

	cpu->cycles = 0;
	cpu->nextEvent = 0;
	cpu->halted = 0;

	cpu->irqh.reset(cpu);
}

void ARMRaiseFIQ(struct ARMCore* cpu) {
	if (cpu->cpsr.f) {
		return;
	}
	union PSR cpsr = cpu->cpsr;
	ARMSetPrivilegeMode(cpu, MODE_FIQ);
	cpu->cpsr.priv = MODE_FIQ;
	cpu->gprs[ARM_LR] = cpu->gprs[ARM_PC];
	cpu->gprs[ARM_PC] = BASE_FIQ;
	cpu->cycles += ARMWritePC(cpu);
	cpu->spsr = cpsr;
	cpu->cpsr.f = 1;
	cpu->cpsr.i = 1;
	cpu->halted = 0;
}

void ARMRaiseIRQ(struct ARMCore* cpu) {
	if (cpu->cpsr.i) {
		return;
	}
	union PSR cpsr = cpu->cpsr;
	ARMSetPrivilegeMode(cpu, MODE_IRQ);
	cpu->cpsr.priv = MODE_IRQ;
	cpu->gprs[ARM_LR] = cpu->gprs[ARM_PC];
	cpu->gprs[ARM_PC] = BASE_IRQ;
	cpu->cycles += ARMWritePC(cpu);
	cpu->spsr = cpsr;
	cpu->cpsr.i = 1;
	cpu->halted = 0;
}

void ARMRaiseSWI(struct ARMCore* cpu) {
	union PSR cpsr = cpu->cpsr;
	ARMSetPrivilegeMode(cpu, MODE_SUPERVISOR);
	cpu->cpsr.priv = MODE_SUPERVISOR;
	cpu->gprs[ARM_LR] = cpu->gprs[ARM_PC] - 4;
	cpu->gprs[ARM_PC] = BASE_SWI;
	cpu->cycles += ARMWritePC(cpu);
	cpu->spsr = cpsr;
	cpu->cpsr.i = 1;
}

void ARMRaiseUndefined(struct ARMCore* cpu) {
	union PSR cpsr = cpu->cpsr;
	ARMSetPrivilegeMode(cpu, MODE_UNDEFINED);
	cpu->cpsr.priv = MODE_UNDEFINED;
	cpu->gprs[ARM_LR] = cpu->gprs[ARM_PC] - 4;
	cpu->gprs[ARM_PC] = BASE_UNDEF;
	cpu->cycles += ARMWritePC(cpu);
	cpu->spsr = cpsr;
	cpu->cpsr.i = 1;
}

static inline void ARMStep(struct ARMCore* cpu) {
	uint32_t opcode = cpu->prefetch[0];
	cpu->prefetch[0] = cpu->prefetch[1];
	cpu->gprs[ARM_PC] += 4;
	cpu->prefetch[1] = cpu->memory.load32(cpu, cpu->gprs[ARM_PC], NULL);

	unsigned condition = opcode >> 28;
	if (condition != 0xE) {
		bool conditionMet = false;
		switch (condition) {
		case 0x0:
			conditionMet = ARM_COND_EQ;
			break;
		case 0x1:
			conditionMet = ARM_COND_NE;
			break;
		case 0x2:
			conditionMet = ARM_COND_CS;
			break;
		case 0x3:
			conditionMet = ARM_COND_CC;
			break;
		case 0x4:
			conditionMet = ARM_COND_MI;
			break;
		case 0x5:
			conditionMet = ARM_COND_PL;
			break;
		case 0x6:
			conditionMet = ARM_COND_VS;
			break;
		case 0x7:
			conditionMet = ARM_COND_VC;
			break;
		case 0x8:
			conditionMet = ARM_COND_HI;
			break;
		case 0x9:
			conditionMet = ARM_COND_LS;
			break;
		case 0xA:
			conditionMet = ARM_COND_GE;
			break;
		case 0xB:
			conditionMet = ARM_COND_LT;
			break;
		case 0xC:
			conditionMet = ARM_COND_GT;
			break;
		case 0xD:
			conditionMet = ARM_COND_LE;
			break;
		default:
			break;
		}
		if (!conditionMet) {
			cpu->cycles += ARM_PREFETCH_CYCLES;
			return;
		}
	}
	ARMInstruction instruction = _armTable[((opcode >> 16) & 0xFF0) | ((opcode >> 4) & 0x00F)];
	instruction(cpu, opcode);
}

void ARMRun(struct ARMCore* cpu) {
	ARMStep(cpu);
	if (cpu->cycles >= cpu->nextEvent) {
		cpu->irqh.processEvents(cpu);
	}
}

void ARMRunLoop(struct ARMCore* cpu) {
	while (cpu->cycles < cpu->nextEvent) {
		ARMStep(cpu);
	}
	cpu->irqh.processEvents(cpu);
}

void ARMRunFake(struct ARMCore* cpu, uint32_t opcode) {
	cpu->gprs[ARM_PC] -= 4;
	cpu->prefetch[1] = cpu->prefetch[0];
	cpu->prefetch[0] = opcode;
}