package main

import (
	"flag"
	"fmt"
	"io"
	"os"
	"unsafe"
)

// #include "machine.h"
// #include "terminal.h"
import "C"

var (
	flagRAMSize       int
	flagFlashSize     int
	flagFlashPageSize int
	flagLoglevel      string
	flagGdbServer     string
)

var loglevels = map[string]int{
	"none":    C.LOG_NONE,
	"error":   C.LOG_ERROR,
	"err":     C.LOG_ERROR,
	"warning": C.LOG_WARN,
	"warn":    C.LOG_WARN,
	"calls":   C.LOG_CALLS,
	"instrs":  C.LOG_INSTRS,
}

func isPowerOfTwo(n int) bool {
	// https://stackoverflow.com/a/600306/559350
	return n >= 0 && (n&(n-1)) == 0
}

func main() {
	flag.IntVar(&flagRAMSize, "ram", 32, "RAM size in kB")
	flag.IntVar(&flagFlashSize, "flash", 256, "flash size in kB")
	flag.IntVar(&flagFlashPageSize, "pagesize", 1024, "flash page size in bytes")
	flag.StringVar(&flagLoglevel, "loglevel", "error", "error, warning, calls, instrs")
	flag.StringVar(&flagGdbServer, "gdb", "localhost:7333", "GDB target port")
	flag.Parse()

	if flag.NArg() != 1 {
		fmt.Fprintln(os.Stderr, "error: provide a firmware image")
		flag.PrintDefaults()
		os.Exit(1)
	}

	if !isPowerOfTwo(flagFlashPageSize) {
		fmt.Fprintln(os.Stderr, "error: pagesize must be a power of two")
		flag.PrintDefaults()
		os.Exit(1)
	}

	if _, ok := loglevels[flagLoglevel]; !ok {
		fmt.Fprintln(os.Stderr, "error: loglevel must be one of: error, warning, calls, instrs")
		flag.PrintDefaults()
		os.Exit(1)
	}

	f, err := os.Open(flag.Arg(0))
	if err != nil {
		fmt.Fprintln(os.Stderr, "cannot open firmware image:", err)
		os.Exit(1)
	}
	defer f.Close()
	st, err := f.Stat()
	if err != nil {
		fmt.Fprintln(os.Stderr, "cannot stat firmware image:", err)
		os.Exit(1)
	}
	if st.Size() > int64(flagFlashSize*1024) {
		fmt.Fprintln(os.Stderr, "firmware does not fit in flash")
		os.Exit(1)
	}
	cfirmware := C.malloc(C.size_t(st.Size()))
	defer C.free(cfirmware)
	firmware := (*[1 << 30]byte)(unsafe.Pointer(cfirmware))[:st.Size():st.Size()]
	_, err = io.ReadFull(f, firmware)
	if err != nil {
		fmt.Fprintln(os.Stderr, "cannot read firmware image:", err)
		os.Exit(1)
	}

	// This is where the MCU is actually started.
	machine := C.machine_create(C.size_t(flagFlashSize*1024), C.size_t(flagFlashPageSize), C.size_t(flagRAMSize*1024), C.int(loglevels[flagLoglevel]))
	C.machine_load(machine, (*C.uint8_t)(cfirmware), C.size_t(len(firmware)))

	runChan := make(chan struct{})
	if flagGdbServer != "" {
		go func() {
			err := gdbServer(machine, flagGdbServer, runChan)
			if err != nil {
				fmt.Fprintln(os.Stderr, "gdb server error:", err)
			}
		}()
	}

	C.machine_reset(machine)
	for {
		if C.machine_run(machine) == 0 {
			return
		}
		C.terminal_disable_raw()

		// send "machine has stopped"
		runChan <- struct{}{}

		// wait until we may resume again
		<-runChan
	}
	C.machine_free(machine)
}
