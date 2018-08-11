
#include "machine.h"
#include "terminal.h"

#include <string.h>

// This file implements the CPU core and memory subsystem.
// For more information on the instruction set, see:
// https://ece.uwaterloo.ca/~ece222/ARM/ARM7-TDMI-manual-pt3.pdf
// http://hermes.wings.cs.wisc.edu/files/Thumb-2SupplementReferenceManual.pdf
// https://www.heyrick.co.uk/armwiki/The_Status_register

#ifdef __EMSCRIPTEN__

#include <emscripten.h>

// Implemented in JavaScript.
void *wasm_malloc(size_t size);
#define malloc wasm_malloc
#define calloc(size, nmemb) wasm_malloc(size * nmemb)

#define machine_loglevel(machine) (0)
#define machine_log(machine, level, ...) ((false) ? fprintf(stderr, __VA_ARGS__) : 0)

#define KEEPALIVE EMSCRIPTEN_KEEPALIVE

#else // all other compilers

#include <stdio.h>

// TODO: make this configurable
#define machine_loglevel(machine) (machine->loglevel)
#define machine_log(machine, level, ...) ((machine->loglevel >= level) ? fprintf(stderr, __VA_ARGS__) : 0)

#define KEEPALIVE

#endif

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
		if (transfer_type == STORE) {
			if ((address & 3) != 0 || width != WIDTH_32) {
				machine_log(machine, LOG_ERROR, "ERROR: unaligned write to read-only memory (PC: %x)\n", machine->pc - 3);
				return ERR_MEM;
			}
			if (!machine->image_writable) {
				machine_log(machine, LOG_ERROR, "ERROR: write to read-only memory (PC: %x)\n", machine->pc - 3);
				return ERR_MEM;
			}

			// Emulate NOR memory where bits can only be cleared.
			*(uint32_t*)ptr &= *reg;
			return 0;
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
		if ((address & 3) != 0 || width != WIDTH_32) {
			machine_log(machine, LOG_ERROR, "\nERROR: invalid %s peripheral address: 0x%08x (PC: %x)\n", transfer_type == LOAD ? "load" : "store", address, machine->pc - 3);
			return ERR_MEM;
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
		} else if (transfer_type == LOAD && address == 0x4001e400) { // NVMC.READY
			value = 1; // always ready
		} else if (transfer_type == STORE && address == 0x4001e504) { // NVMC.CONFIG
			machine->image_writable = *reg != 0;
		} else if (transfer_type == STORE && address == 0x4001e508) { // NVMC.ERASEPAGE
			if ((*reg & (machine->pagesize-1)) != 0 || *reg >= machine->image_size) {
				machine_log(machine, LOG_ERROR, "ERROR: invalid page address: %x (PC: %x)\n", *reg, machine->pc - 3);
				return ERR_MEM;
			}
			// Emulate erasing NOR flash.
			memset(machine->image8 + *reg, 0xff, machine->pagesize);
		} else {
			machine_log(machine, LOG_WARN, "unknown %s peripheral address: 0x%08x (value: 0x%x, PC: %x)\n", transfer_type == LOAD ? "load" : "store", address, *reg, machine->pc - 3);
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
		// Private peripheral bus + Device: 0xe0000000 .. 0xffffffff
		if ((address & 3) != 0) {
			machine_log(machine, LOG_ERROR, "\nERROR: invalid device/private address: 0x%08x\n", address);
			return ERR_MEM;
		}
		if (address == 0xe000e100 && transfer_type == STORE) {
			// NVIC Interrupt Set-enable Register
			machine_log(machine, LOG_WARN, "set interrupts: %08x\n", *reg);
			return 0;
		}
		if (address == 0xe000e180 && transfer_type == STORE) {
			// NVIC Interrupt Clear-enable Register
			machine_log(machine, LOG_WARN, "clear interrupts: %08x\n", *reg);
			return 0;
		}
		if ((address & 0xfffffff0) == 0xe000e400) {
			ptr = &machine->nvic.ip[(address / 4) % 8];
		}
		if ((address & 0xfffffff0) == 0xf0000fe0 && transfer_type == LOAD) {
			machine_log(machine, LOG_WARN, "private address: %x\n", address);
			// 0xf0000fe0, 0xf0000fe4, 0xf0000fe8, 0xf0000fec
			// Not sure what this is all about but this is what the nrf
			// seems to expect...
			*reg = 0;
			return 0;
		}
	}

	if (ptr == 0) {
		if (transfer_type == LOAD) {
			machine_log(machine, LOG_ERROR, "\nERROR: invalid load address: 0x%08x (PC: %x)\n", address, machine->pc - 3);
		} else {
			machine_log(machine, LOG_ERROR, "\nERROR: invalid store address: 0x%08x (PC: %x, value: %x)\n", address, machine->pc - 3, *reg);
		}
		return ERR_MEM;
	} else if ((width == WIDTH_16 && (address & 1) != 0) || (width == WIDTH_32 && (address & 3) != 0)) {
		machine_log(machine, LOG_ERROR, "\nERROR: unaligned %s address: 0x%08x (PC: %x)\n", transfer_type == LOAD ? "load" : "store", address, machine->pc - 3);
		return ERR_MEM;
	}

	if (transfer_type == LOAD) {
		if (width == WIDTH_8) {
			uint8_t value = *(uint8_t*)ptr;
			if (signextend) {
				*reg = (int8_t)value;
			} else {
				*reg = value;
			}
		} else if (width == WIDTH_16) {
			uint16_t value = *(uint16_t*)ptr;
			if (signextend) {
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

KEEPALIVE
void machine_reset(machine_t *machine) {
	// Do a reset
	machine->sp = machine->image32[0]; // initial stack pointer
	//machine->lr = 0xffffffff; // exit address
	machine->lr = 0xdeadbeef; // exit address
	machine->pc = machine->image32[1]; // Reset_Vector address
	machine->backtrace[1] = machine->pc - 1;
	machine_log(machine, LOG_CALLS, "RESET %5x (sp: %x)\n", machine->pc - 1, machine->sp);
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

static int machine_instr_stmdb(machine_t *machine, uint32_t *reg, uint32_t reg_list, bool wback) {
	uint32_t address = *reg;
	for (int i = 14; i >= 0; i--) {
		if (reg_list & (1 << i)) {
			address -= 4;
			if (reg == &machine->sp) {
				if (i == 14) {
					machine_log(machine, LOG_CALLS, "%*spush lr      (sp: %x) (lr: %x)\n", machine->call_depth * 2, "", machine->sp, machine->lr + 2);
					machine_add_backtrace(machine, machine->pc - 3);
				} else {
					machine_log(machine, LOG_CALLS, "%*spush r%d      (sp: %x)\n", machine->call_depth * 2, "", i, machine->sp);
				}
			}
			if (machine_transfer(machine, address, STORE, &machine->regs[i], WIDTH_32, false)) {
				return ERR_MEM;
			}
		}
	}
	if (wback) {
		*reg = address;
	}
	return 0;
}

static int machine_instr_stmia(machine_t *machine, uint32_t *reg, uint32_t reg_list, bool wback) {
	uint32_t address = *reg;
	for (size_t i = 0; i <= 15; i++) {
		if (((reg_list >> i) & 1) == 1) {
			uint32_t *reg = &machine->regs[i];
			if (machine_transfer(machine, address, STORE, reg, WIDTH_32, false)) {
				return ERR_MEM;
			}
			address += 4;
		}
	}
	if (wback) {
		*reg = address;
	}
	return 0;
}

static int machine_instr_ldmia(machine_t *machine, uint32_t *reg, uint32_t reg_list, bool wback) {
	uint32_t address = *reg;
	for (int i = 0; i <= 15; i++) {
		if (reg_list & (1 << i)) {
			if (reg == &machine->sp) {
				if (i == 15) {
					machine_log(machine, LOG_CALLS, "%*sPOP pc %5x (sp: %x)\n", machine->call_depth * 2, "", machine->pc - 1, machine->sp);
					machine_sub_backtrace(machine);
				} else {
					machine_log(machine, LOG_CALLS, "%*spop r%d       (sp: %x)\n", machine->call_depth * 2, "", i, machine->sp);
				}
			}
			if (machine_transfer(machine, address, LOAD, &machine->regs[i], WIDTH_32, false)) {
				return ERR_MEM;
			}
			address += 4;
		}
	}
	if (wback) {
		*reg = address;
	}
	return 0;
}

static uint32_t machine_instr_lsl(machine_t *machine, uint32_t src, uint32_t shift, bool setflags) {
	if (setflags && shift != 0) { // range 0..31, setflags only when shifting non-zero amount
		machine->psr.c = src >> (32 - shift) & 1;
	}
	if (shift >= 32) {
		return 0;
	}
	return src << shift;
}

static uint32_t machine_instr_lsr(machine_t *machine, uint32_t src, uint32_t shift, bool setflags) {
	if (shift >= 32) {
		if (setflags) {
			machine->psr.c = (src >> 31) & 1;
		}
		return 0;
	}
	if (shift != 0 && setflags) {
		machine->psr.c = src >> (shift - 1) & 1;
	}
	return src >> shift;
}

static uint32_t machine_instr_asr(machine_t *machine, uint32_t src, uint32_t shift, bool setflags) {
	if (shift >= 32) {
		if (setflags) {
			machine->psr.c = (((int32_t)src) >> 31) & 1;
		}
		// shift twice to avoid undefined behavior in the C compiler
		return (((int32_t)src) >> 16) >> 16;
	} else if (shift >= 0) {
		if (setflags) {
			machine->psr.c = ((int32_t)src) >> (shift - 1) & 1;
		}
		return ((int32_t)src) >> shift;
	} else {
		return src;
	}
}

static uint32_t machine_instr_add(machine_t *machine, uint32_t a, uint32_t b, bool setflags) {
	uint32_t result = a + b;
	if (setflags) {
		int64_t result64s = (int64_t)(int32_t)a + (int64_t)(int32_t)b;
		uint64_t result64u = (uint64_t)a + (uint64_t)b;
		machine->psr.n = (int32_t)result < 0;
		machine->psr.z = result == 0;
		machine->psr.c = result64u >= ((uint64_t)1 << 32); // true if 32-bit overflow
		machine->psr.v = ((int32_t)result < 0) != (result64s < 0);
	}
	return result;
}

static uint32_t machine_instr_adc(machine_t *machine, uint32_t a, uint32_t b, bool setflags) {
	uint32_t result = a + b + machine->psr.c;
	if (setflags) {
		int64_t result64s = (int64_t)(int32_t)a + (int64_t)(int32_t)b + machine->psr.c;
		uint64_t result64u = (uint64_t)a + (uint64_t)b + machine->psr.c;
		machine->psr.n = (int32_t)result < 0;
		machine->psr.z = result == 0;
		machine->psr.c = result64u >= ((uint64_t)1 << 32); // true if 32-bit overflow
		machine->psr.v = ((int32_t)result < 0) != (result64s < 0);
	}
	return result;
}

static uint32_t machine_instr_sub(machine_t *machine, uint32_t a, uint32_t b, bool setflags) {
	uint32_t result = a - b;
	if (setflags) {
		int64_t result64s = (int64_t)(int32_t)a - (int64_t)(int32_t)b;
		uint64_t result64u = (uint64_t)a - (uint64_t)b;
		machine->psr.n = (int32_t)result < 0;
		machine->psr.z = result == 0;
		machine->psr.c = (int64_t)result64u >= 0;
		machine->psr.v = ((int32_t)result < 0) != (result64s < 0);
	}
	return result;
}

static uint32_t machine_instr_sbc(machine_t *machine, uint32_t a, uint32_t b, bool setflags) {
	uint32_t result = a - b - !machine->psr.c;
	if (setflags) {
		int64_t result64s = (int64_t)(int32_t)a - (int64_t)(int32_t)b - !machine->psr.c;
		int64_t result64u = (uint64_t)a - (uint64_t)b - !machine->psr.c;
		machine->psr.n = (int32_t)result < 0;
		machine->psr.z = result == 0;
		machine->psr.c = (int64_t)result64u >= 0;
		machine->psr.v = ((int32_t)result < 0) != (result64s < 0);
	}
	return result;
}

// Return 1 if true, 0 if false, and -1 if invalid.
static int machine_condition(machine_t *machine, uint32_t condition) {
	if (condition == 0b0000) { // BEQ: Z == 1
		return machine->psr.z == true;
	} else if (condition == 0b0001) { // BNE: Z == 0
		return machine->psr.z == false;
	} else if (condition == 0b0010) { // BCS: C == 1
		return machine->psr.c == true;
	} else if (condition == 0b0011) { // BCC: C == 0
		return machine->psr.c == false;
	} else if (condition == 0b0100) { // BMI: N == 1
		return machine->psr.n == true;
	} else if (condition == 0b0101) { // BPL: N == 0
		return machine->psr.n == false;
	} else if (condition == 0b1000) { // BHI: C == 1 && Z == 0
		return machine->psr.c == true && machine->psr.z == false;
	} else if (condition == 0b1001) { // BLS: C == 0 || Z == 1
		return machine->psr.c == false || machine->psr.z == true;
	} else if (condition == 0b1010) { // BGE: N == V
		return machine->psr.n == machine->psr.v;
	} else if (condition == 0b1011) { // BLT: N != V
		return machine->psr.n != machine->psr.v;
	} else if (condition == 0b1100) { // BGT: Z == 0 && N == V
		return machine->psr.z == false && machine->psr.n == machine->psr.v;
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
		return machine->psr.z == true || machine->psr.n != machine->psr.v;
	} else {
		return -1;
	}
}

int machine_step(machine_t *machine) {
	// Some handy aliases
	uint32_t *pc = &machine->pc; // r15
	uint32_t *lr = &machine->lr; // r14
	uint32_t *sp = &machine->sp; // r13

	if (*pc - 1 == machine->hwbreak[0] ||
		*pc - 1 == machine->hwbreak[1] ||
		*pc - 1 == machine->hwbreak[2] ||
		*pc - 1 == machine->hwbreak[3]) {
		return ERR_BREAK;
	}

	if (*pc == 0xdeadbeef) {
		return ERR_EXIT;
	}
	if (*pc > machine->image_size - 2) {
		return ERR_PC;
	}
	if ((*pc & 1) != 1) {
		return ERR_PC;
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
				*reg_dst = machine_instr_lsl(machine, *reg_src, offset5, true);
			} else if (op == 1) { // LSRS
				*reg_dst = machine_instr_lsr(machine, *reg_src, offset5 == 0 ? 32 : offset5 , true);
			} else if (op == 2) { // ASRS
				*reg_dst = machine_instr_asr(machine, *reg_src, offset5 == 0 ? 32 : offset5 , true);
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
				*reg_dst = machine_instr_add(machine, *reg_src, value, true);
			} else { // SUBS
				*reg_dst = machine_instr_sub(machine, *reg_src, value, true);
			}
		}
		machine->psr.n = (int32_t)*reg_dst < 0;
		machine->psr.z = *reg_dst == 0;

	} else if ((instruction >> 13) == 0b001) {
		// Format 3: move/compare/add/subtract immediate
		uint32_t  imm  = instruction & 0xff;
		uint32_t *reg = &machine->regs[(instruction >> 8)  & 0b111];
		size_t   op   = (instruction >> 11) & 0b11;
		bool setflags = true;
		if (op == 0) { // MOVS
			*reg = imm;
		} else if (op == 1) { // CMP
			// Update flags as if doing *reg - imm
			machine_instr_sub(machine, *reg, imm, true);
			setflags = false;
			// Don't update *reg
		} else if (op == 2) { // ADDS
			*reg = machine_instr_add(machine, *reg, imm, true);
		} else if (op == 3) { // SUBS
			*reg = machine_instr_sub(machine, *reg, imm, true);
		}
		if (setflags) {
			machine->psr.n = (int32_t)*reg < 0;
			machine->psr.z = *reg == 0;
		}

	} else if ((instruction >> 10) == 0b010000) {
		// Format 4: ALU operations
		uint32_t *reg_dst = &machine->regs[(instruction >> 0) & 0b111];
		uint32_t *reg_src = &machine->regs[(instruction >> 3) & 0b111];
		uint32_t op       = (instruction >> 6) & 0b1111;
		bool setflags = true;
		if (op == 0b0000) { // ANDS
			*reg_dst &= *reg_src;
		} else if (op == 0b0001) { // EORS
			*reg_dst ^= *reg_src;
		} else if (op == 0b0010) { // LSLS
			*reg_dst = machine_instr_lsl(machine, *reg_dst, *reg_src & 0xff, true);
		} else if (op == 0b0011) { // LSRS
			*reg_dst = machine_instr_lsr(machine, *reg_dst, *reg_src & 0xff, true);
		} else if (op == 0b0100) { // ASRS
			*reg_dst = machine_instr_asr(machine, *reg_dst, *reg_src & 0xff, true);
		} else if (op == 0b0101) { // ADCS
			*reg_dst = machine_instr_adc(machine, *reg_dst, *reg_src, true);
		} else if (op == 0b0110) { // SBCS
			*reg_dst = machine_instr_sbc(machine, *reg_dst, *reg_src, true);
		} else if (op == 0b1000) { // TST
			// set CC on Rd AND Rs
			machine->psr.n = (int32_t)(*reg_src & *reg_dst) < 0;
			machine->psr.z = (int32_t)(*reg_src & *reg_dst) == 0;
			setflags = false;
		} else if (op == 0b1001) { // NEG / RSBS
			*reg_dst = machine_instr_sub(machine, 0, *reg_src, true);
		} else if (op == 0b1010) { // CMP
			// set CC on Rd - Rs
			machine_instr_sub(machine, *reg_dst, *reg_src, true);
			setflags = false;
		} else if (op == 0b1011) { // CMN
			// set cc on Rd + Rs
			machine_instr_add(machine, *reg_dst, *reg_src, true);
			setflags = false;
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
			// The only missing ALU op is ROR.
			return ERR_UNDEFINED;
		}
		if (setflags) {
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
				return ERR_UNDEFINED; // unimplemented
			}
			if (h1) {
				machine_log(machine, LOG_CALLS, "%*sBLX r%ld %6x (sp: %x) -> %x\n", machine->call_depth * 2, "", reg_src - machine->regs, *pc - 3, *sp, *reg_src - 1);
				machine_add_backtrace(machine, *pc - 3);
			} else if (reg_src == lr) {
				machine_log(machine, LOG_CALLS, "%*sBX lr %6x (sp: %x) <- %x\n", machine->call_depth * 2, "", *pc - 3, *sp, *reg_src - 1);
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
				// set CC on Rd - Rs
				machine_instr_sub(machine, *reg_dst, *reg_src, true);
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
			return ERR_MEM;
		}

	} else if ((instruction >> 12) == 0b0101) {
		uint32_t *reg_change = &machine->regs[(instruction >> 0) & 0b111];
		uint32_t *reg_base   = &machine->regs[(instruction >> 3) & 0b111];
		uint32_t *reg_offset = &machine->regs[(instruction >> 6) & 0b111];
		if (((instruction >> 9) & 0b1) == 0) {
			// Format 7: Load/store with register offset (LDR)
			bool flag_byte = (instruction >> 10) & 0b1;
			bool flag_load = (instruction >> 11) & 0b1;
			transfer_type_t transfer_type = flag_load ? LOAD : STORE;
			width_t width = flag_byte ? WIDTH_8 : WIDTH_32;
			if (machine_transfer(machine, *reg_base + *reg_offset, transfer_type, reg_change, width, false)) {
				return ERR_MEM;
			}
		} else {
			// Format 8: Load/store sign-extended byte/halfword
			bool flag_sign_extend = (instruction >> 10) & 0b1;
			bool flag_H           = (instruction >> 11) & 0b1;
			uint32_t address = *reg_base + *reg_offset;
			if (flag_sign_extend) {
				if (flag_H) {
					if (machine_transfer(machine, address, LOAD, reg_change, WIDTH_16, true)) { // LDSH
						return ERR_MEM;
					}
				} else {
					if (machine_transfer(machine, address, LOAD, reg_change, WIDTH_8, true)) { // LDSB
						return ERR_MEM;
					}
				}
			} else {
				transfer_type_t transfer_type = flag_H ? LOAD : STORE;
				if (machine_transfer(machine, address, transfer_type, reg_change, WIDTH_16, false)) { // STRH/LDRH
					return ERR_MEM;
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
				return ERR_MEM;
			}
		} else {
			uint32_t address = *reg_base + offset5 * 4;
			if (machine_transfer(machine, address, transfer_type, reg_change, WIDTH_32, false)) {
				return ERR_MEM;
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
			return ERR_MEM;
		}

	} else if ((instruction >> 12) == 0b1001) {
		// Format 11: SP-relative load/store
		uint32_t word8     = (instruction >> 0) & 0xff; // 8 bits
		uint32_t *reg      = &machine->regs[(instruction >> 8) & 0b111];
		bool     flag_load = (instruction >> 11) & 0b1;
		transfer_type_t transfer_type = flag_load ? LOAD : STORE;
		if (machine_transfer(machine, *sp + word8 * 4, transfer_type, reg, WIDTH_32, false)) {
			return ERR_MEM;
		}

	} else if ((instruction >> 12) == 0b1010) {
		// Format 12: load address
		uint32_t word8    = (instruction >> 0) & 0xff; // 8 bits
		uint32_t *reg_dst = &machine->regs[(instruction >> 8) & 0b111];
		bool     flag_sp  = (instruction >> 11) & 0b1;
		uint32_t value = flag_sp ? *sp : *pc;
		if (!flag_sp) {
			// for PC: force bit 1 to 0
			value &= ~0b11UL;
		}
		*reg_dst = value + (word8 << 2);

	} else if ((instruction >> 8) == 0b10110000) {
		// Format 13: add offset to Stack Pointer
		uint32_t offset6  = (instruction >> 0) & 0x3f; // 6 bits
		bool     flag_neg = (instruction >> 7) & 0b1;
		if (flag_neg) { // SUB SP, #imm
			machine_log(machine, LOG_CALLS, "%*ssub     0x%02x (sp: %x)\n", machine->call_depth * 2, "", offset6 * 4, *sp);
			*sp -= offset6 * 4;
		} else { // ADD SP, #imm
			machine_log(machine, LOG_CALLS, "%*sadd    %2x (sp: %x)\n", machine->call_depth * 2, "", offset6 * 4, *sp);
			*sp += offset6 * 4;
		}

	} else if ((instruction >> 8) == 0b10110010) {
		// Sign or zero extend
		uint32_t *reg_dst = &machine->regs[(instruction >> 0) & 0b111];
		uint32_t *reg_src = &machine->regs[(instruction >> 3) & 0b111];
		uint32_t opcode   = (instruction >> 6) & 0b11;
		if (opcode == 0b00) {
			// T1: SXTH (signed extend halfword)
			*reg_dst = (int32_t)(*reg_src << 16) >> 16;
		} else if (opcode == 0b01) {
			// T1: SXTB (signed extend byte)
			*reg_dst = (int32_t)(*reg_src << 24) >> 24;
		} else if (opcode == 0b10) {
			// T1: UXTH (unsigned extend halfword)
			*reg_dst = *reg_src & 0xffff;
		} else if (opcode == 0b11) {
			// T1: UXTB (unsigned extend byte)
			*reg_dst = *reg_src & 0xff;
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
		// This emulator handles some breakpoints in a special way.
		if (imm8 == 0x81) {
			machine->loglevel = LOG_INSTRS;
		} else {
			return ERR_BREAK;
		}

	} else if ((instruction >> 12) == 0b1011 && ((instruction >> 9) & 0b11) == 0b10) { // 1011x10
		// Format 14: push/pop registers
		uint32_t reg_list   = (instruction >> 0) & 0xff;
		bool     flag_load  = (instruction >> 11) & 0b1;
		bool     flag_pc_lr = (instruction >> 8) & 0b1; // store LR / load PC
		if (flag_load) { // POP
			if (flag_pc_lr) {
				reg_list |= (1 << 15); // PC
			}
			int err = machine_instr_ldmia(machine, sp, reg_list, true);
			if (err != 0) {
				return err;
			}
		} else { // PUSH
			if (flag_pc_lr) {
				reg_list |= (1 << 14); // LR
			}
			int err = machine_instr_stmdb(machine, sp, reg_list, true);
			if (err != 0) {
				return err;
			}
		}

	} else if ((instruction >> 12) == 0b1100) {
		// Format 15: multiple load/store (LDMIA and STMIA)
		uint32_t  reg_list = (instruction >> 0) & 0xff;
		uint32_t  reg_base_num = (instruction >> 8) & 0b111;
		uint32_t *reg_base = &machine->regs[reg_base_num];
		bool     flag_load = (instruction >> 11) & 0b1;
		if (reg_list == 0) {
			machine_log(machine, LOG_ERROR, "\nERROR: LDMIA/STMIA does not allow zero registers (%04x)\n", instruction);
			return ERR_UNDEFINED;
		}
		if (flag_load) {
			// LDMIA!
			bool wback = (reg_list & (1 << reg_base_num)) == 0;
			machine_instr_ldmia(machine, reg_base, reg_list, wback);
		} else {
			// STMIA!
			machine_instr_stmia(machine, reg_base, reg_list, true);
		}

	} else if ((instruction >> 12) == 0b1101) {
		// Format 16: conditional branch
		// http://infocenter.arm.com/help/topic/com.arm.doc.dui0497a/BABEHFEF.html
		uint32_t offset8   = (instruction >> 0) & 0xff;
		uint32_t condition = (instruction >> 8) & 0b1111;
		int32_t offset = ((int32_t)(offset8 << 24) >> 23);
		offset += 2;
		uint32_t old_pc = *pc;
		int result = machine_condition(machine, condition);
		if (result < 0) {
			return ERR_UNDEFINED;
		}
		if (result) {
			*pc += offset;
		}
		if (old_pc != *pc) {
			machine_log(machine, LOG_CALLS, "%*sBcond %6x (sp: %x) -> %x\n", machine->call_depth * 2, "", old_pc - 3, *sp, *pc - 1);
		} else {
			machine_log(machine, LOG_CALLS, "%*sBcond %6x (sp: %x) -> !%x\n", machine->call_depth * 2, "", old_pc - 3, *sp, *pc - 1);
		}

	} else if ((instruction >> 11) == 0b11100) {
		// Format 18: unconditional branch
		uint32_t offset11 = (instruction >> 0) & 0x7ff;
		int32_t offset = ((int32_t)(offset11 << 21) >> 20);
		uint32_t old_pc = *pc;
		*pc += offset + 2;
		machine_log(machine, LOG_CALLS, "%*sB    %7x (sp: %x) -> %x\n", machine->call_depth * 2, "", old_pc - 3, *sp, *pc - 1);

	} else if ((instruction >> 12) == 0b1111) {
		// 32-bit instruction

		uint16_t hw1 = instruction;
		uint16_t hw2 = machine->image16[*pc/2];
		*pc += 2;
		if ((hw1 >> 11) == 0b11110 && (hw2 >> 11) == 0b11111) {
			// BL
			uint32_t imm10 = hw1 & 0x3ff;
			uint32_t imm11 = hw2 & 0x7ff;
			int32_t pc_offset = (int32_t)(((uint32_t)imm10 << 12) | ((uint32_t)imm11 << 1)); // >> 11;
			pc_offset <<= 10;
			pc_offset >>= 10;
			uint32_t new_pc = (int32_t)*pc + pc_offset;
			machine_log(machine, LOG_CALLS, "%*sBL   %7x (sp: %x) -> %x\n", machine->call_depth * 2, "", *pc, *sp, new_pc - 1);
			machine_add_backtrace(machine, *pc - 5);
			*lr = *pc;
			*pc = new_pc;

		} else if ((hw1 >> 4) == 0b111100111011 || (hw2 >> 14) == 0b10) {
			// Special control operations, ignore.

		} else {
			*pc -= 2; // undo 32-bit change
			return ERR_UNDEFINED;
		}

	} else {
		return ERR_UNDEFINED;
	}

	return 0;
}

void machine_print_registers(machine_t *machine) {
	machine_log(machine, LOG_ERROR, "\n[ ");
	for (size_t i=0; i<8; i++) {
		machine_log(machine, LOG_ERROR, "%8x ", machine->regs[i]);
	}
	machine_log(machine, LOG_ERROR, ".. %8x ", machine->sp);     // sp
	machine_log(machine, LOG_ERROR, "%8x ",    machine->lr - 1); // lr, not entirely correct
	machine_log(machine, LOG_ERROR, "%8x ",    machine->pc - 1); // pc, not entirely correct
	machine_log(machine, LOG_ERROR, "%c%c%c%c ", machine->psr.n ? 'N' : '_', machine->psr.z ? 'Z' : '_', machine->psr.c ? 'C' : '_', machine->psr.v ? 'V' : '_');
	machine_log(machine, LOG_ERROR, "]\n");
}

KEEPALIVE
machine_t * machine_create(size_t image_size, size_t pagesize, size_t ram_size, int loglevel) {
	if (image_size < 16 * 4) {
#if !defined(__EMSCRIPTEN__)
		if (loglevel >= LOG_ERROR) {
			fprintf(stderr, "\nERROR: image is too small to contain an executable\n");
		}
#endif
		return NULL;
	}

	machine_t *machine = calloc(sizeof(machine_t), 1);
	machine->pagesize = pagesize;
	machine->call_depth = 1;
	machine->loglevel = loglevel;
	machine->image_size = image_size;
	machine->mem_size = ram_size;
	machine->psr.t = 1; // Thumb mode

	uint32_t *image = malloc(image_size);
	memset(image, 0xff, image_size); // erase flash
	machine->image32 = image;

	// TODO: put random data in here to make a better simulation
	uint32_t *ram = calloc(ram_size, 1);
	machine->mem32 = ram;

	return machine;
}

void machine_load(machine_t *machine, uint8_t *image, size_t image_size) {
	// caller should check this, but at least fail in a reasonable way
	if (image_size > machine->image_size) {
		image_size = machine->image_size;
	}
	memcpy(machine->image8, image, image_size);
}

KEEPALIVE
uint8_t * machine_get_image(machine_t *machine) {
	return machine->image8;
}

void machine_free(machine_t *machine) {
	free(machine->image);
	machine->image = NULL;
	free(machine->mem);
	machine->mem = NULL;
	free(machine);
}

KEEPALIVE
int machine_run(machine_t *machine) {
	while (1) {
		if (machine->halt) {
			machine->halt = false;
			return ERR_HALT;
		}

		// Print registers
		if (machine_loglevel(machine) >= LOG_INSTRS || (machine_loglevel(machine) >= LOG_CALLS_SP && machine->sp != machine->last_sp)) {
			machine->last_sp = machine->sp;
			machine_print_registers(machine);
		}

		// Execute a single instruction
		int err = machine_step(machine);
		switch (err) {
			case ERR_OK:
				break; // no error
			case ERR_HALT:
				// expected
				break;
			case ERR_EXIT:
				machine_log(machine, LOG_ERROR, "exited.\n");
				break;
			case ERR_BREAK:
				machine_log(machine, LOG_ERROR, "\nhit breakpoint at address %x\n", machine->pc - 3);
				break;
			case ERR_MEM:
				// already printed
				break;
			case ERR_PC:
				machine_log(machine, LOG_ERROR, "\nERROR: invalid PC address: 0x%08x\n", machine->pc);
				break;
			case ERR_UNDEFINED:
				machine_log(machine, LOG_ERROR, "\nERROR: unknown instruction %04x at address %x\n", machine->image16[machine->pc/2 - 1], machine->pc - 3);
				break;
			default:
				machine_log(machine, LOG_ERROR, "\nERROR: unknown error: %d\n", err);
				break;
		}
		if (err != 0 && err != ERR_BREAK) {
			if (machine_loglevel(machine) < LOG_INSTRS) { // don't double-log
				machine_print_registers(machine);
			}
			machine_add_backtrace(machine, machine->pc);
			machine_log(machine, LOG_ERROR, "Backtrace:\n");
			for (int i = 1; i < machine->call_depth; i++) {
				if (i >= MACHINE_BACKTRACE_LEN) {
					machine_log(machine, LOG_ERROR, " %3d. (too much recursion)\n", i);
					break;
				}
				machine_log(machine, LOG_ERROR, " %3d. %8x\n", i, machine->backtrace[i]);
			}
			return err;
		}
	}
}

void machine_readmem(machine_t *machine, void *buf, size_t address, size_t length) {
	if (address % 4 == 0 && length % 4 == 0) {
		for (size_t i=0; i<length; i += 4) {
			uint32_t reg;
			machine_transfer(machine, address + i, LOAD, &reg, WIDTH_32, false);
			((uint32_t*)buf)[i / 4] = reg;
		}
	} else {
		for (size_t i=0; i<length; i++) {
			uint32_t reg;
			machine_transfer(machine, address + i, LOAD, &reg, WIDTH_8, false);
			((uint8_t*)buf)[i] = reg;
		}
	}
}

void machine_readregs(machine_t *machine, uint32_t *regs, size_t num) {
	if (num < sizeof(machine->regs) / sizeof(machine->regs[0])) {
		num = sizeof(machine->regs) / sizeof(machine->regs[0]);
	}
	for (size_t i=0; i<num; i++) {
		regs[i] = machine->regs[i];
	}
}

KEEPALIVE
uint32_t machine_readreg(machine_t *machine, size_t reg) {
	if (reg >= sizeof(machine->regs) / sizeof(machine->regs[0])) {
		return 0;
	}
	return machine->regs[reg];
}

void machine_halt(machine_t *machine) {
	machine->halt = true;
}

bool machine_break(machine_t *machine, size_t num, uint32_t addr) {
	if (num >= sizeof(machine->hwbreak) / sizeof(machine->hwbreak[0])) {
		return false;
	}
	machine->hwbreak[num] = addr;
	return true;
}
