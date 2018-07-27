
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

typedef struct {
	uint32_t :24;
	uint32_t t:1; // Thumb mode
	uint32_t :3;
	uint32_t v:1; // overflow
	uint32_t c:1; // carry
	uint32_t z:1; // zero
	uint32_t n:1; // negative
} flags_t;

#define MACHINE_BACKTRACE_LEN (100)

typedef struct {
	// Regular registers (r0 .. r15)
	union {
		struct {
			uint32_t r0;
			uint32_t r1;
			uint32_t r2;
			uint32_t r3;
			uint32_t r4;
			uint32_t r5;
			uint32_t r6;
			uint32_t r7;
			uint32_t r8;
			uint32_t r9;
			uint32_t r10;
			uint32_t r11;
			uint32_t r12;
			union {
				uint32_t r13;
				uint32_t sp;
			};
			union {
				uint32_t r14;
				uint32_t lr;
			};
			union {
				uint32_t r15;
				uint32_t pc;
			};
			flags_t psr; // CPU flags
		};
		uint32_t regs[17];
	};

	// ROM/flash area
	union {
		uint32_t *image32;
		uint16_t *image16;
		uint8_t  *image8;
		void     *image;
	};
	size_t image_size;
	bool image_writable;
	size_t pagesize;

	// RAM area
	union {
		uint32_t *mem32;
		uint16_t *mem16;
		uint8_t  *mem8;
		void     *mem;
	};
	size_t mem_size;

	// The NVIC peripheral
	struct {
		uint32_t ip[8]; // interrupt priority
	} nvic;

	// Statistics and backtrace depth.
	// Warning: call_depth may not fit in the backtrace! So check before
	// indexing.
	int call_depth;
	uint32_t backtrace[MACHINE_BACKTRACE_LEN];
	uint32_t last_sp;

	volatile uint32_t hwbreak[4];

	// misc
	int loglevel;
	volatile bool halt;
} machine_t;

typedef enum {
	WIDTH_8,
	WIDTH_16,
	WIDTH_32,
} width_t;

typedef enum {
	LOAD,
	STORE,
} transfer_type_t;

enum {
	ERR_OK,        // no error
	ERR_EXIT,      // program has exited (should not normally happen on a MCU)
	ERR_OTHER,     // other (already handled) error
	ERR_BREAK,     // hit a breakpoint
	ERR_UNDEFINED, // undefined instruction
};

enum {
	LOG_ERROR,    // only log critical errors
	LOG_WARN,     // log warnings
	LOG_CALLS,    // log all branch/stack related instructions
	LOG_CALLS_SP, // log calls and registers at stack moves
	LOG_INSTRS,   // log everything
};

machine_t * machine_create(size_t image_size, size_t pagesize, size_t ram_size, int loglevel);
void machine_load(machine_t *machine, uint8_t *image, size_t image_size);
void machine_readmem(machine_t *machine, void *buf, size_t offset, size_t length);
void machine_readregs(machine_t *machine, uint32_t *regs, size_t num);
uint32_t machine_readreg(machine_t *machine, size_t reg);
void machine_reset(machine_t *machine);
int machine_step(machine_t *machine);
void machine_run(machine_t *machine);
void machine_halt(machine_t *machine);
bool machine_break(machine_t *machine, size_t num, uint32_t addr);
void machine_free(machine_t *machine);
