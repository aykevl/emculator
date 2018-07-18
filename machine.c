
#include "machine.h"
#include "terminal.h"

#include <stdio.h>
#include <string.h>

static int machine_transfer(machine_t *machine, uint32_t address, transfer_type_t transfer_type, uint32_t *reg, width_t width, bool signextend) {
	// Select memory region
	uint32_t region = address >> 29; // 3 bits for the region
	uint32_t region_address = address & (0xffffffff >> 3);

	void *ptr = 0;
	if (region == 0) {
		// code: 0x00000000 .. 0x1fffffff
		if (region_address < machine->image_size) {
			ptr = &machine->image8[region_address];
		}
	} else if (region == 1) {
		// SRAM: 0x20000000 .. 0x3ffffff
		if (region_address < machine->mem_size) {
			ptr = &machine->mem8[region_address];
		}
	} else if (region == 2) {
		// Peripherals: 0x40000000 .. 0x5fffffff
		// Make this a special case
		uint32_t value = 0;
		if ((address & 3) != 0) {
			fprintf(stderr, "\nERROR: invalid peripheral address: 0x%08x (PC: %x)\n", address, machine->pc - 3);
			return ERR_OTHER;
		}
		if (transfer_type == STORE && address == 0x40002000) { // STARTRX
		} else if (transfer_type == STORE && address == 0x40002004) { // STOPRX
		} else if (transfer_type == STORE && address == 0x40002008) { // STARTTX
		} else if (transfer_type == STORE && address == 0x4000200c) { // STOPTX
		} else if (address == 0x40002108) { // RXDRDY
			value = 1;
		} else if (address == 0x4000211c) { // TXDRDY
			value = 1;
		} else if (address == 0x40002124) { // ERROR
		} else if (address == 0x40002144) { // RXTO
		} else if (transfer_type == LOAD && address == 0x40002518) { // RXD
			value = terminal_getchar();
		} else if (transfer_type == STORE && address == 0x4000251c) { // TXD
			terminal_putchar(*reg);
		} else if (transfer_type == LOAD && address == 0x4000d100) { // RNG.VALRDY
			value = 1;
		} else if (transfer_type == LOAD && address == 0x4000d508) { // RNG.VALUE
			value = rand() & 0xff;
		} else {
			if (machine->loglevel >= LOG_WARN) {
				fprintf(stderr, "unknown %s peripheral address: 0x%08x (value: 0x%x, PC: %x)\n", transfer_type == LOAD ? "load" : "store", address, *reg, machine->pc - 3);
			}
		}
		if (transfer_type == LOAD) {
			if (width == WIDTH_8) {
				value &= 0xff;
			} else if (width == WIDTH_16) {
				value &= 0xffff;
			}
			*reg = value;
		}
		return 0;
	} else if (region == 7) {
		if ((address & 3) != 0) {
			fprintf(stderr, "\nERROR: invalid device/private address: 0x%08x\n", address);
			return ERR_OTHER;
		}
		// Private peripheral bus + Device: 0xe0000000 .. 0xffffffff
		if ((address == 0xe000e100 && transfer_type == STORE)) {
			// NVIC Interrupt Set-enable Register
			if (machine->loglevel >= LOG_WARN) {
				fprintf(stderr, "set interrupts: %08x\n", *reg);
			}
			return 0;
		}
		if ((address == 0xe000e180 && transfer_type == STORE)) {
			// NVIC Interrupt Clear-enable Register
			if (machine->loglevel >= LOG_WARN) {
				fprintf(stderr, "clear interrupts: %08x\n", *reg);
			}
			return 0;
		}
		if (((address & 0xfffffff0) == 0xe000e400)) {
			ptr = &machine->nvic.ip[(address / 4) % 8];
		}
		if ((address & 0xfffffff0) == 0xf0000fe0 && transfer_type == LOAD) {
			if (machine->loglevel >= LOG_WARN) {
				fprintf(stderr, "private address: %x\n", address);
			}
			// 0xf0000fe0, 0xf0000fe4, 0xf0000fe8, 0xf0000fec
			// Not sure what this is all about but this is what the nrf
			// seems to expect...
			*reg = 0;
			return 0;
		}
	}

	if (ptr == 0) {
		fprintf(stderr, "\nERROR: invalid %s address: 0x%08x (PC: %x)\n", transfer_type == LOAD ? "load" : "store", address, machine->pc - 3);
		return ERR_OTHER;
	} else if ((width == WIDTH_16 && (address & 1) != 0) || (width == WIDTH_32 && (address & 3) != 0)) {
		fprintf(stderr, "\nERROR: unaligned %s address: 0x%08x (PC: %x)\n", transfer_type == LOAD ? "load" : "store", address, machine->pc - 3);
		return ERR_OTHER;
	}

	if (transfer_type == LOAD) {
		if (width == WIDTH_8) {
			uint8_t value = *(uint8_t*)ptr;
			if (signextend) {
				// TODO: test!
				*reg = (int8_t)value;
			} else {
				*reg = value;
			}
		} else if (width == WIDTH_16) {
			uint16_t value = *(uint16_t*)ptr;
			if (signextend) {
				// TODO: test!
				*reg = (int16_t)value;
			} else {
				*reg = value;
			}
		} else if (width == WIDTH_32) {
			*reg = *(uint32_t*)ptr;
		} else {
			// should be unreachable
		}
	} else { // STORE
		if (width == WIDTH_8) {
			*(uint8_t*)ptr = *reg;
		} else if (width == WIDTH_16) {
			*(uint16_t*)ptr = *reg;
		} else if (width == WIDTH_32) {
			*(uint32_t*)ptr = *reg;
		} else {
			// should be unreachable
		}
	}
	return 0;
}

void machine_init(machine_t *machine, uint32_t *image, size_t image_size, uint32_t *ram, size_t ram_size) {
	// TODO: put random data in here to make a better simulation
	memset(machine, 0, sizeof(*machine));
	machine->image32 = image;
	machine->image_size = image_size;
	machine->mem32 = ram;
	machine->mem_size = ram_size;
	machine->call_depth = 1;
}

void machine_reset(machine_t *machine) {
	// Do a reset
	machine->sp = machine->image32[0]; // initial stack pointer
	//machine->lr = 0xffffffff; // exit address
	machine->lr = 0xdeadbeef; // exit address
	machine->pc = machine->image32[1]; // Reset_Vector address
	machine->backtrace[1] = machine->pc - 1;
	if (machine->loglevel >= LOG_CALLS) {
		fprintf(stderr, "RESET %5x (sp: %x)\n", machine->pc - 1, machine->sp);
	}
}

static void machine_add_backtrace(machine_t *machine, uint32_t pc) {
	machine->call_depth++;
	if (machine->call_depth >= 0 && machine->call_depth < MACHINE_BACKTRACE_LEN) {
		machine->backtrace[machine->call_depth] = pc;
	}
}

static void machine_sub_backtrace(machine_t *machine) {
	machine->call_depth--;
}

int machine_step(machine_t *machine) {
	// Some handy aliases
	uint32_t *pc = &machine->pc; // r15
	uint32_t *lr = &machine->lr; // r14
	uint32_t *sp = &machine->sp; // r13

	// Run instruction
	// https://ece.uwaterloo.ca/~ece222/ARM/ARM7-TDMI-manual-pt3.pdf
	// http://hermes.wings.cs.wisc.edu/files/Thumb-2SupplementReferenceManual.pdf
	if (*pc == 0xdeadbeef) {
		fprintf(stderr, "exited.\n");
		return ERR_OTHER;
	}
	if (*pc > machine->image_size - 2) {
		fprintf(stderr, "\nERROR: PC address is out of range: 0x%08x\n", *pc);
		return ERR_OTHER;
	}
	if ((*pc & 1) != 1) {
		fprintf(stderr, "\nERROR: PC address must have the high bit set: 0x%08x\n", *pc);
		return ERR_OTHER;
	}
	uint16_t instruction = machine->image16[*pc/2];

	// Increment PC to point to the next instruction.
	*pc += 2;

	// Decode/execute instruction

	if ((instruction >> 13) == 0b000) {
		uint32_t *reg_dst = &machine->regs[(instruction >> 0) & 0b111];
		uint32_t *reg_src = &machine->regs[(instruction >> 3) & 0b111];
		uint32_t op       = (instruction >> 11)  & 0b11;
		if (op != 3) {
			// Format 1: move shifted register
			uint32_t offset5 = (instruction >> 6) & 0x1f;
			if (op == 0) { // LSLS
				// TODO: LSLS #0 does not affect carry flag
				machine->psr.c = *reg_src >> (32 - offset5) & 1;
				*reg_dst = *reg_src << offset5;
			} else if (op == 1) { // LSRS
				machine->psr.c = *reg_src >> (offset5 - 1) & 1;
				*reg_dst = *reg_src >> offset5;
			} else if (op == 2) { // ASRS
				machine->psr.c = ((int32_t)*reg_src) >> (offset5 - 1) & 1;
				*reg_dst = ((int32_t)*reg_src) >> offset5;
			}
		} else { // op == 3
			// Format 2: add/subtract
			uint32_t value    = (instruction >> 6)  & 0b111;
			uint32_t op       = (instruction >> 9)  & 0b1;
			bool     flag_imm = (instruction >> 10) & 0b1;
			if (!flag_imm) {
				value = machine->regs[value];
			}
			if (op == 0) { // ADDS
				machine->psr.v = (int32_t)*reg_src >= 0 && (int32_t)(*reg_src + value) < 0;
				*reg_dst = *reg_src + value;
				machine->psr.c = (uint64_t)*reg_src + (uint64_t)value >= (1UL << 32); // true if 32-bit overflow
			} else { // SUBS
				machine->psr.c = (int64_t)((uint64_t)*reg_src - (uint64_t)value) >= 0;
				machine->psr.v = (int32_t)*reg_src < 0 && (int32_t)(*reg_src - value) >= 0;
				*reg_dst = *reg_src - value;
			}
		}
		machine->psr.n = (int32_t)*reg_dst < 0;
		machine->psr.z = *reg_dst == 0;

	} else if ((instruction >> 13) == 0b001) {
		// Format 3: move/compare/add/subtract immediate
		uint32_t  imm  = instruction & 0xff;
		uint32_t *reg = &machine->regs[(instruction >> 8)  & 0b111];
		size_t   op   = (instruction >> 11) & 0b11;
		bool set_cc = true;
		if (op == 0) { // MOVS
			*reg = imm;
		} else if (op == 1) { // CMP
			// Update flags as if doing *reg - imm
			int32_t value = *reg - imm;
			int64_t value64 = (uint64_t)*reg - (uint64_t)imm;
			machine->psr.c = value64 >= 0;
			machine->psr.n = value < 0;
			machine->psr.z = value == 0;
			machine->psr.v = value != (uint32_t)value64;
			if (machine->loglevel >= LOG_INSTRS) {
				fprintf(stderr, "CMP %x - %x -> V=%d (PC: %x)\n", *reg, imm, (int)machine->psr.v, *pc - 3);
			}
			set_cc = false;
			// Don't update *reg
		} else if (op == 2) { // ADDS
			machine->psr.c = (uint64_t)*reg + (uint64_t)imm >= (1UL << 32); // true if 32-bit overflow
			machine->psr.v = (int32_t)*reg >= 0 && (int32_t)(*reg + imm) < 0;
			*reg += imm;
		} else if (op == 3) { // SUBS
			machine->psr.c = (int64_t)((uint64_t)*reg - (uint64_t)imm) >= 0;
			machine->psr.v = (int32_t)*reg < 0 && (int32_t)(*reg - imm) >= 0;
			*reg -= imm;
		}
		if (set_cc) {
			machine->psr.n = (int32_t)*reg < 0;
			machine->psr.z = *reg == 0;
		}

	} else if ((instruction >> 10) == 0b010000) {
		// Format 4: ALU operations
		uint32_t *reg_dst = &machine->regs[(instruction >> 0) & 0b111];
		uint32_t *reg_src = &machine->regs[(instruction >> 3) & 0b111];
		uint32_t op       = (instruction >> 6) & 0b1111;
		flags_t old_flags = machine->psr;
		bool set_cc = true;
		if (op == 0b0000) { // ANDS
			*reg_dst &= *reg_src;
		} else if (op == 0b0001) { // EORS
			*reg_dst ^= *reg_src;
		} else if (op == 0b0010) { // LSLS
			machine->psr.c = *reg_dst >> (32 - *reg_src) & 1;
			*reg_dst <<= *reg_src;
		} else if (op == 0b0011) { // LSRS
			machine->psr.c = *reg_dst >> (*reg_src - 1) & 1;
			*reg_dst >>= *reg_src;
		} else if (op == 0b0100) { // ASRS
			machine->psr.c = ((int32_t)*reg_dst) >> (*reg_src - 1) & 1;
			*(int32_t*)reg_dst >>= *reg_src;
		} else if (op == 0b0101) { // ADCS
			machine->psr.c = ((uint64_t)*reg_dst + (uint64_t)*reg_src + (uint64_t)old_flags.c) >= (1UL << 32); // true if 32-bit overflow
			*reg_dst = *reg_dst + *reg_src + old_flags.c;
			int64_t value64 = (uint64_t)*reg_dst + (uint64_t)*reg_src + (uint64_t)old_flags.c;
			machine->psr.v = *reg_dst != (uint32_t)value64;
		} else if (op == 0b0110) { // SBCS
			*reg_dst = *reg_dst - *reg_src - !old_flags.c;
			int64_t value64 = (uint64_t)*reg_dst - (uint64_t)*reg_src - (uint64_t)!old_flags.c;
			machine->psr.c = value64 >= 0;
			machine->psr.v = *reg_dst != (uint32_t)value64;
		} else if (op == 0b1000) { // TST
			// set CC on Rd AND Rs
			machine->psr.n = (int32_t)(*reg_src & *reg_dst) < 0;
			machine->psr.z = (int32_t)(*reg_src & *reg_dst) == 0;
			set_cc = false;
		} else if (op == 0b1001) { // NEG / RSBS
			*reg_dst = 0 - *reg_src;
			int64_t value64 = (uint64_t)0 - (uint64_t)*reg_src;
			machine->psr.c = value64 >= 0;
			machine->psr.v = *reg_dst != (uint32_t)value64;
		} else if (op == 0b1010) { // CMP
			// set CC on Rd - Rs
			int32_t value = *reg_dst - *reg_src;
			int64_t value64 = (uint64_t)*reg_dst - (uint64_t)*reg_src;
			machine->psr.c = value64 >= 0;
			machine->psr.n = value < 0;
			machine->psr.z = value == 0;
			machine->psr.v = value != (uint32_t)value64;
			if (machine->loglevel >= LOG_INSTRS) {
				fprintf(stderr, "CMP %x - %x -> V=%d (PC: %x)\n", *reg_dst, *reg_src, (int)machine->psr.v, *pc - 3);
			}
			set_cc = false;
		} else if (op == 0b1100) { // ORRS
			// does not update C or V
			*reg_dst |= *reg_src;
		} else if (op == 0b1101) { // MULS
			// does not update C or V
			*reg_dst *= *reg_src;
		} else if (op == 0b1110) { // BICS
			// does not update C or V
			*reg_dst &= ~*reg_src;
		} else if (op == 0b1111) { // MVNS
			// does not update C or V
			*reg_dst = ~*reg_src;
		} else {
			fprintf(stderr, "\nERROR: unimplemented: ALU operation %u (%04x, PC: %x)\n", op, instruction, *pc - 3);
			return ERR_OTHER;
		}
		if (set_cc) {
			machine->psr.n = (int32_t)*reg_dst < 0;
			machine->psr.z = *reg_dst == 0;
		}

	} else if ((instruction >> 10) == 0b010001) {
		// Format 5: Hi register operations/branch exchange
		uint32_t *reg_dst = &machine->regs[(instruction >> 0) & 0b111];
		uint32_t *reg_src = &machine->regs[(instruction >> 3) & 0b111];
		bool     h2       = (instruction >> 6) & 0b1;
		bool     h1       = (instruction >> 7) & 0b1;
		uint32_t op       = (instruction >> 8) & 0b11;
		reg_src += h2 * 8; // make high register (if h2 is 1)
		if (op == 3) { // BX/BLX
			if (reg_dst != &machine->r0) {
				fprintf(stderr, "\nERROR: invalid instruction: BX/BLX lower bits != 0 (%04x)\n", instruction);
				return ERR_OTHER;
			}
			if (h1) {
				if (machine->loglevel >= LOG_CALLS) {
					fprintf(stderr, "%*sBLX r%ld %6x (sp: %x) -> %x\n", machine->call_depth * 2, "", reg_src - machine->regs, *pc - 3, *sp, *reg_src - 1);
				}
				machine_add_backtrace(machine, *pc - 3);
			} else if (reg_src == lr) {
				if (machine->loglevel >= LOG_CALLS) {
					fprintf(stderr, "%*sBX lr %6x (sp: %x) <- %x\n", machine->call_depth * 2, "", *pc - 3, *sp, *reg_src - 1);
				}
				machine_sub_backtrace(machine);
			}
			uint32_t next_lr = *pc;
			*pc = *reg_src;
			if (h1) { // BLX
				*lr = next_lr;
			}
		} else { // ALU operation with high registers
			reg_dst += h1 * 8; // make high register (if h1 is 1)
			if (op == 0) { // ADD
				*reg_dst += *reg_src;
			} else if (op == 1) { // CMP
				int32_t value = *reg_dst - *reg_src;
				int64_t value64 = (uint64_t)*reg_dst - (uint64_t)*reg_src;
				machine->psr.c = value64 >= 0;
				machine->psr.n = value < 0;
				machine->psr.z = value == 0;
				machine->psr.v = value != (uint32_t)value64;
				if (machine->loglevel >= LOG_INSTRS) {
					fprintf(stderr, "CMP %x - %x -> V=%d (PC: %x)\n", *reg_dst, *reg_src, (int)machine->psr.v, *pc - 3);
				}
			} else if (op == 2) { // MOV
				*reg_dst = *reg_src;
				if (reg_dst == pc) {
					*reg_dst |= 1; // force T-bit to 1
				}
			}
		}

	} else if ((instruction >> 11) == 0b01001) {
		// Format 6: PC-relative load
		uint32_t imm = instruction & 0xff; // 8 bits
		uint32_t *reg = &machine->regs[(instruction >> 8)  & 0b111];
		uint32_t address = ((*pc + 2) >> 2U << 2U) + imm * 4;
		if (machine_transfer(machine, address, LOAD, reg, WIDTH_32, false)) {
			return ERR_OTHER;
		}

	} else if ((instruction >> 12) == 0b0101) {
		uint32_t *reg_change = &machine->regs[(instruction >> 0) & 0b111];
		uint32_t *reg_base   = &machine->regs[(instruction >> 3) & 0b111];
		uint32_t *reg_offset = &machine->regs[(instruction >> 6) & 0b111];
		if (((instruction >> 9) & 0b1) == 0) {
			// Format 7: Load/store with register offset
			bool flag_byte = (instruction >> 10) & 0b1;
			bool flag_load = (instruction >> 11) & 0b1;
			transfer_type_t transfer_type = flag_load ? LOAD : STORE;
			width_t width = flag_byte ? WIDTH_8 : WIDTH_32;
			if (machine_transfer(machine, *reg_base + *reg_offset, transfer_type, reg_change, width, false)) {
				return ERR_OTHER;
			}
		} else {
			// Format 8: Load/store sign-extended byte/halfword
			bool flag_sign_extend = (instruction >> 10) & 0b1;
			bool flag_H           = (instruction >> 11) & 0b1;
			uint32_t address = *reg_base + *reg_offset;
			if (flag_sign_extend) {
				if (flag_H) {
					if (machine_transfer(machine, address, LOAD, reg_change, WIDTH_16, true)) { // LDSH
						return ERR_OTHER;
					}
				} else {
					if (machine_transfer(machine, address, LOAD, reg_change, WIDTH_8, true)) { // LDSB
						return ERR_OTHER;
					}
				}
			} else {
				transfer_type_t transfer_type = flag_H ? LOAD : STORE;
				if (machine_transfer(machine, address, transfer_type, reg_change, WIDTH_16, false)) { // STRH/LDRH
					return ERR_OTHER;
				}
			}
		}

	} else if ((instruction >> 13) == 0b011) {
		// Format 9: load/store with immediate offset
		uint32_t *reg_change = &machine->regs[(instruction >> 0)  & 0b111];
		uint32_t *reg_base   = &machine->regs[(instruction >> 3)  & 0b111];
		uint32_t offset5     = (instruction >> 6) & 0x1f;
		bool     flag_load   = (instruction >> 11) & 0b1;
		bool     flag_byte   = (instruction >> 12) & 0b1;
		transfer_type_t transfer_type = flag_load ? LOAD : STORE;
		if (flag_byte) {
			uint32_t address = *reg_base + offset5;
			if (machine_transfer(machine, address, transfer_type, reg_change, WIDTH_8, false)) {
				return ERR_OTHER;
			}
		} else {
			uint32_t address = *reg_base + offset5 * 4;
			if (machine_transfer(machine, address, transfer_type, reg_change, WIDTH_32, false)) {
				return ERR_OTHER;
			}
		}

	} else if ((instruction >> 12) == 0b1000) {
		// Format 10: load/store halfword
		uint32_t *reg_change = &machine->regs[(instruction >> 0)  & 0b111];
		uint32_t *reg_base   = &machine->regs[(instruction >> 3)  & 0b111];
		uint32_t offset5     = (instruction >> 6) & 0x1f;
		bool     flag_load   = (instruction >> 11) & 0b1;
		transfer_type_t transfer_type = flag_load ? LOAD : STORE;
		uint32_t address = *reg_base + (offset5 << 1);
		if (machine_transfer(machine, address, transfer_type, reg_change, WIDTH_16, false)) {
			return ERR_OTHER;
		}

	} else if ((instruction >> 12) == 0b1001) {
		// Format 11: SP-relative load/store
		uint32_t word8     = (instruction >> 0) & 0xff; // 8 bits
		uint32_t *reg      = &machine->regs[(instruction >> 8) & 0b111];
		bool     flag_load = (instruction >> 11) & 0b1;
		transfer_type_t transfer_type = flag_load ? LOAD : STORE;
		if (machine_transfer(machine, *sp + word8 * 4, transfer_type, reg, WIDTH_32, false)) {
			return ERR_OTHER;
		}

	} else if ((instruction >> 12) == 0b1010) {
		// Format 12: load address
		uint32_t word8    = (instruction >> 0) & 0xff; // 8 bits
		uint32_t *reg_dst = &machine->regs[(instruction >> 8) & 0b111];
		bool     flag_sp  = (instruction >> 11) & 0b1;
		uint32_t *reg_src = flag_sp ? sp : pc;
		uint32_t value = *reg_src;
		if (!flag_sp) {
			// for PC: force bit 1 to 0, add PC offset
			value &= ~0b10UL + 4;
		}
		*reg_dst = value + (word8 << 2);

	} else if ((instruction >> 8) == 0b10110000) {
		// Format 13: add offset to Stack Pointer
		uint32_t offset6  = (instruction >> 0) & 0x3f; // 6 bits
		bool     flag_neg = (instruction >> 7) & 0b1;
		if (flag_neg) { // SUB SP, #imm
			if (machine->loglevel >= LOG_CALLS) {
				fprintf(stderr, "%*ssub     0x%02x (sp: %x)\n", machine->call_depth * 2, "", offset6 * 4, *sp);
			}
			*sp -= offset6 * 4;
		} else { // ADD SP, #imm
			if (machine->loglevel >= LOG_CALLS) {
				fprintf(stderr, "%*sadd    %2x (sp: %x)\n", machine->call_depth * 2, "", offset6 * 4, *sp);
			}
			*sp += offset6 * 4;
		}
	} else if ((instruction >> 8) == 0b10110010) {
		// Sign or zero extend
		uint32_t *reg_dst = &machine->regs[(instruction >> 0) & 0b111];
		uint32_t *reg_src = &machine->regs[(instruction >> 3) & 0b111];
		uint32_t opcode   = (instruction >> 6) & 0b11;
		if (opcode == 0b01) {
			// T1: SXTB (signed extend byte)
			*reg_dst = (int32_t)(*reg_src << 24) >> 24;
		} else if (opcode == 0b10) {
			// T1: UXTH (unsigned extend halfword)
			*reg_dst = (*reg_src << 16) >> 16;
		} else if (opcode == 0b11) {
			// T1: UXTB (unsigned extend byte)
			*reg_dst = (*reg_src << 24) >> 24;
		} else {
			return ERR_UNDEFINED;
		}

	} else if ((instruction >> 8) == 0b10111010) {
		// T1: Reverse bytes
		uint32_t *reg_dst = &machine->regs[(instruction >> 0) & 0b111];
		uint32_t *reg_src = &machine->regs[(instruction >> 3) & 0b111];
		uint32_t opcode   = (instruction >> 6) & 0b11;
		if (opcode == 0b00) { // REV: reverse bytes
			*reg_dst =
				(*reg_src >> 0  & 0xff) << 24 |
				(*reg_src >> 8  & 0xff) << 16 |
				(*reg_src >> 16 & 0xff) << 8 |
				(*reg_src >> 24 & 0xff) << 0;
		//} else if (opcode == 0b01) { // REV16
		//} else if (opcode == 0b11) { // REVSH
		} else {
			return ERR_UNDEFINED;
		}

	} else if ((instruction >> 8) == 0b10111110) {
		// T1: BKPT (software breakpoint)
		uint32_t imm8 = (instruction >> 0) & 0b11111111;
		fprintf(stderr, "Execution has hit breakpoint %d at PC=%x.\n", imm8, machine->pc - 3);
		return ERR_OTHER;

	} else if ((instruction >> 12) == 0b1011 && ((instruction >> 9) & 0b11) == 0b10) { // 1011x10
		// Format 14: push/pop registers
		uint8_t  reg_list   = (instruction >> 0) & 0xff;
		bool     flag_load  = (instruction >> 11) & 0b1;
		bool     flag_pc_lr = (instruction >> 8) & 0b1; // store LR / load PC
		if (flag_load) { // POP
			for (int i = 0; i < 8; i++) {
				if (reg_list & (1 << i)) {
					if (machine->loglevel >= LOG_CALLS) {
						fprintf(stderr, "%*spop r%d       (sp: %x)\n", machine->call_depth * 2, "", i, *sp);
					}
					if (machine_transfer(machine, *sp, LOAD, &machine->regs[i], WIDTH_32, false)) {
						return ERR_OTHER;
					}
					*sp += 4;
				}
			}
			if (flag_pc_lr) {
				uint32_t old_pc = *pc;
				if (machine_transfer(machine, *sp, LOAD, pc, WIDTH_32, false)) {
					return ERR_OTHER;
				}
				if (machine->loglevel >= LOG_CALLS) {
					fprintf(stderr, "%*sPOP pc %5x (sp: %x) <- %5x\n", machine->call_depth * 2, "", old_pc - 3, *sp, *pc - 1);
				}
				machine_sub_backtrace(machine);
				*sp += 4;
			}
		} else { // PUSH
			if (flag_pc_lr) {
				*sp -= 4;
				if (machine->loglevel >= LOG_CALLS) {
					fprintf(stderr, "%*spush lr      (sp: %x) (lr: %x)\n", machine->call_depth * 2, "", *sp, *lr + 2);
				}
				if (machine_transfer(machine, *sp, STORE, lr, WIDTH_32, false)) {
					return ERR_OTHER;
				}
			}
			for (int i = 7; i >= 0; i--) {
				if (reg_list & (1 << i)) {
					*sp -= 4;
					if (machine->loglevel >= LOG_CALLS) {
						fprintf(stderr, "%*spush r%d      (sp: %x)\n", machine->call_depth * 2, "", i, *sp);
					}
					if (machine_transfer(machine, *sp, STORE, &machine->regs[i], WIDTH_32, false)) {
						return ERR_OTHER;
					}
				}
			}
		}

	} else if ((instruction >> 12) == 0b1100) {
		// Format 15: multiple load/store (LDMIA and STMIA)
		uint8_t  reg_list = (instruction >> 0) & 0xff;
		uint32_t *reg_base = &machine->regs[(instruction >> 8) & 0b111];
		bool     flag_load = (instruction >> 11) & 0b1;
		if (reg_list == 0) {
			fprintf(stderr, "\nERROR: LDMIA/STMIA does not allow zero registers (%04x)\n", instruction);
			return ERR_OTHER;
		}
		uint32_t address = *reg_base;
		size_t offset = 0;
		for (size_t i = 0; i < 8; i++) {
			if (((reg_list >> i) & 1) == 1) {
				uint32_t *reg = &machine->regs[i];
				transfer_type_t transfer_type = flag_load ? LOAD : STORE;
				if (machine_transfer(machine, address + offset, transfer_type, reg, WIDTH_32, false)) {
					return ERR_OTHER;
				}
				offset += 4;
			}
		}
		*reg_base = address + offset;

	} else if ((instruction >> 12) == 0b1101) {
		// Format 16: conditional branch
		// http://infocenter.arm.com/help/topic/com.arm.doc.dui0497a/BABEHFEF.html
		uint32_t offset8   = (instruction >> 0) & 0xff;
		uint32_t condition = (instruction >> 8) & 0b1111;
		int32_t offset = ((int32_t)(offset8 << 24) >> 23);
		offset += 2;
		uint32_t old_pc = *pc;
		if (condition == 0b0000) { // BEQ: Z == 1
			if (machine->psr.z == true)
				*pc += offset;
		} else if (condition == 0b0001) { // BNE: Z == 0
			if (machine->psr.z == false)
				*pc += offset;
		} else if (condition == 0b0010) { // BCS: C == 1
			if (machine->psr.c == true)
				*pc += offset;
		} else if (condition == 0b0011) { // BCC: C == 0
			if (machine->psr.c == false)
				*pc += offset;
		} else if (condition == 0b0100) { // BMI: N == 1
			if (machine->psr.n == true)
				*pc += offset;
		} else if (condition == 0b0101) { // BPL: N == 0
			if (machine->psr.n == false)
				*pc += offset;
		} else if (condition == 0b1000) { // BHI: C == 1 && Z == 0
			if (machine->psr.c == true && machine->psr.z == false)
				*pc += offset;
		} else if (condition == 0b1001) { // BLS: C == 0 || Z == 1
			if (machine->psr.c == false || machine->psr.z == true)
				*pc += offset;
		} else if (condition == 0b1010) { // BGE: N == V
			if (machine->psr.n == machine->psr.v)
				*pc += offset;
		} else if (condition == 0b1011) { // BLT: N != V
			if (machine->psr.n != machine->psr.v)
				*pc += offset;
		} else if (condition == 0b1100) { // BGT: Z == 0 && N == V
			if (machine->psr.z == false && machine->psr.n == machine->psr.v)
				*pc += offset;
		} else if (condition == 0b1101) { // BLE: Z == 1 || N != V
			// For this instruction, different manuals say different
			// things.
			// One option:
			//   Z == 1 || N != V
			// Another option:
			//   Z == 1 && N != V
			// I've tested compiler-generated code and it appears the
			// former is correct. Also, it is more consistent with e.g.
			// BHI/BLS. An example of a page that says otherwise:
			//   http://infocenter.arm.com/help/index.jsp?topic=/com.arm.doc.dui0497a/BABEHFEF.html
			if (machine->psr.z == true || machine->psr.n != machine->psr.v)
				*pc += offset;
		} else {
			return ERR_UNDEFINED;
		}
		if (machine->loglevel >= LOG_CALLS) {
			if (old_pc != *pc) {
				fprintf(stderr, "%*sBcond %6x (sp: %x) -> %x\n", machine->call_depth * 2, "", old_pc - 3, *sp, *pc - 1);
			} else {
				fprintf(stderr, "%*sBcond %6x (sp: %x) -> !%x\n", machine->call_depth * 2, "", old_pc - 3, *sp, *pc - 1);
			}
		}

	} else if ((instruction >> 11) == 0b11100) {
		// Format 18: unconditional branch
		uint32_t offset11 = (instruction >> 0) & 0x7ff;
		int32_t offset = ((int32_t)(offset11 << 21) >> 20);
		uint32_t old_pc = *pc;
		*pc += offset + 2;
		if (machine->loglevel >= LOG_CALLS) {
			fprintf(stderr, "%*sB    %7x (sp: %x) -> %x\n", machine->call_depth * 2, "", old_pc - 3, *sp, *pc - 1);
		}

	} else if ((instruction >> 11) == 0b11110) {
		// Part 1 of 32-bit Thumb-2 instruction: BL
		// Set LR to PC + SignExtend(imm11 : '000000000000')
		uint32_t imm11 = (instruction >> 0) & 0x7ff;
		*lr = ((int32_t)(imm11 << 21)) >> 9; // leave the lowest 12 bits zero
		if (machine->loglevel >= LOG_CALLS) {
			fprintf(stderr, "%*sbl prep\n", machine->call_depth * 2, "");
		}

	} else if ((instruction >> 11) == 0b11111) {
		// Part 2 of 32-bit Thumb-2 instruction: BL
		// Complete operation.
		uint32_t imm11 = (instruction >> 0) & 0x7ff;
		uint32_t old_pc = *pc;
		*pc += *lr + (imm11 << 1);
		*lr = old_pc;
		if (machine->loglevel >= LOG_CALLS) {
			fprintf(stderr, "%*sBL   %7x (sp: %x) -> %x\n", machine->call_depth * 2, "", old_pc - 5, *sp, *pc - 1);
		}
		machine_add_backtrace(machine, old_pc - 5);

	} else {
		return ERR_UNDEFINED;
	}

	return 0;
}

void run_emulator(uint32_t *image, size_t image_size, uint32_t *ram, size_t ram_size, int loglevel) {
	if (image_size < 16 * 4) {
		fprintf(stderr, "\nERROR: image is too small to contain an executable\n");
		return;
	}

	machine_t machine;
	machine.loglevel = loglevel;
	machine_init(&machine, image, image_size, ram, ram_size);
	machine_reset(&machine);

	uint32_t last_sp = 0;
	while (1) {
		// Print registers
		if (machine.loglevel >= LOG_INSTRS || (machine.loglevel >= LOG_CALLS_SP && machine.sp != last_sp)) {
			last_sp = machine.sp;
			fprintf(stderr, "\n[ ");
			for (size_t i=0; i<8; i++) {
				fprintf(stderr, "%8x ", machine.regs[i]);
			}
			fprintf(stderr, ".. %8x ", machine.sp);     // sp
			fprintf(stderr, "%8x ",    machine.lr - 1); // lr, not entirely correct
			fprintf(stderr, "%8x ",    machine.pc - 1); // pc, not entirely correct
			fprintf(stderr, "%c%c%c%c ", machine.psr.n ? 'N' : '_', machine.psr.z ? 'Z' : '_', machine.psr.c ? 'C' : '_', machine.psr.v ? 'V' : '_');
			fprintf(stderr, "]\n");
		}

		// Execute a single instruction
		int err = machine_step(&machine);
		switch (err) {
			case 0:
				break; // no error
			case ERR_UNDEFINED:
				fprintf(stderr, "\nERROR: unknown instruction %04x at address %x\n", machine.image16[machine.pc/2 - 1], machine.pc - 3);
				break;
			case ERR_OTHER:
				break; // already printed
			default:
				fprintf(stderr, "\nERROR: unknown error: %d\n", err);
				break;
		}
		if (err != 0) {
			machine_add_backtrace(&machine, machine.pc);
			fprintf(stderr, "Backtrace:\n");
			for (int i = 1; i < machine.call_depth; i++) {
				if (i >= MACHINE_BACKTRACE_LEN) {
					fprintf(stderr, " %3d. (too much recursion)\n", i);
					break;
				}
				fprintf(stderr, " %3d. %8x\n", i, machine.backtrace[i]);
			}
			return;
		}
	}
}
