package main

import (
	"unsafe"
)

// #include "machine.h"
import "C"

type Machine struct {
	machine *C.machine_t
	halted  bool
	runChan chan struct{}
}

func (m *Machine) Halted() bool {
	return m.halted
}

func (m *Machine) Running() bool {
	return !m.Halted()
}

func (m *Machine) Halt() {
	if m.halted {
		panic("machine is already halted")
	}
	C.machine_halt(m.machine)
	<-m.runChan
	m.halted = true
}

func (m *Machine) Step() int {
	return int(C.machine_step(m.machine))
}

func (m *Machine) Continue() {
	if !m.halted {
		panic("machine is already running")
	}
	m.halted = false
	m.runChan <- struct{}{}
}

func (m *Machine) SetBreakpoint(num int, address uint32) bool {
	return bool(C.machine_break(m.machine, C.size_t(num), C.uint32_t(address)))
}

func (m *Machine) ReadRegister(register int) uint32 {
	return uint32(C.machine_readreg(m.machine, C.size_t(register)))
}

func (m *Machine) ReadRegisters(num int) []byte {
	length := num * 4
	cregs := C.malloc(C.size_t(length))
	regs := (*[1 << 30]byte)(unsafe.Pointer(cregs))[:length:length]
	C.machine_readregs(m.machine, (*C.uint32_t)(cregs), C.size_t(num))
	buf := make([]byte, length)
	copy(buf, regs)
	C.free(cregs)
	return buf
}

func (m *Machine) ReadMemory(addr, length int) []byte {
	cmem := C.malloc(C.size_t(length))
	mem := (*[1 << 30]byte)(unsafe.Pointer(cmem))[:length:length]
	C.machine_readmem(m.machine, cmem, C.size_t(addr), C.size_t(length))
	buf := make([]byte, length)
	copy(buf, mem)
	C.free(cmem)
	return buf
}
