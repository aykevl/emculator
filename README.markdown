# eMCUlator - Emulate a MCU (microcontroller)

This program is able to interpret most ARM Cortex-M0 instructions. It takes a
binary as input and executes it. It currently only supports the NRF51822 (flash,
memory map, UART peripheral).

While most of it works, some emulation does not. This is a work-in-progress, and
I won't promise I'll ever finish it.

It is able to run [MicroPython](https://micropython.org/) (or more specifically,
the [nRF port](https://github.com/micropython/micropython/tree/master/ports/nrf))
with all tests passing for the PCA10028 board so it is probably able to run most
applications that don't use any peripherals or interrupts.

Currently supported:

  * Most of the Cortex-M0 instruction set.
  * A basic implementation of the UART0 and NVIC peripheral for Nordic devices.
  * GDB remote support (connect `gdb` with `target remote :7333`).

Not supported:

  * Interrupt handling (except `Reset_Handler`, of course).
  * The `WFI` and `WFE` instructions.

This emulator has two variants of the CLI tool:

  * **C**: A (hopefully) portable implementation of the emulator, with bare
    bones image loading support and nothing else.
    
    Usage:
    
        make
        ./emculator <imagepath>
  * **Go**: An emulator that uses the library written in C and provides some
    extra tooling around it, like GDB remote support.
    
    Usage:
    
        go install github.com/aykevl/emculator
        emculator <imagepath>

Note that you must provide raw image files (.bin), not .hex or .elf files. Those
are not (yet) supported.
