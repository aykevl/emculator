package main

import (
	"bufio"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"strings"
)

// This file implements the GDB Remote Serial Protocol (RSP).
// Some documentation:
// https://sourceware.org/gdb/onlinedocs/gdb/Remote-Protocol.html
// https://sourceware.org/gdb/onlinedocs/gdb/Packets.html
// https://www.embecosm.com/appnotes/ean4/embecosm-howto-rsp-server-ean4-issue-2.html

// #include "machine.h"
import "C"

// GDB will request this file (named target.xml) to know the register map of the
// target.
var gdbAnnexTarget = `<?xml version="1.0"?>
<!DOCTYPE target SYSTEM "gdb-target.dtd">
<target version="1.0">
<feature name="org.gnu.gdb.arm.m-profile">
<reg name="r0" bitsize="32" regnum="0" save-restore="yes" type="int" group="general"/>
<reg name="r1" bitsize="32" regnum="1" save-restore="yes" type="int" group="general"/>
<reg name="r2" bitsize="32" regnum="2" save-restore="yes" type="int" group="general"/>
<reg name="r3" bitsize="32" regnum="3" save-restore="yes" type="int" group="general"/>
<reg name="r4" bitsize="32" regnum="4" save-restore="yes" type="int" group="general"/>
<reg name="r5" bitsize="32" regnum="5" save-restore="yes" type="int" group="general"/>
<reg name="r6" bitsize="32" regnum="6" save-restore="yes" type="int" group="general"/>
<reg name="r7" bitsize="32" regnum="7" save-restore="yes" type="int" group="general"/>
<reg name="r8" bitsize="32" regnum="8" save-restore="yes" type="int" group="general"/>
<reg name="r9" bitsize="32" regnum="9" save-restore="yes" type="int" group="general"/>
<reg name="r10" bitsize="32" regnum="10" save-restore="yes" type="int" group="general"/>
<reg name="r11" bitsize="32" regnum="11" save-restore="yes" type="int" group="general"/>
<reg name="r12" bitsize="32" regnum="12" save-restore="yes" type="int" group="general"/>
<reg name="sp" bitsize="32" regnum="13" save-restore="yes" type="data_ptr" group="general"/>
<reg name="lr" bitsize="32" regnum="14" save-restore="yes" type="int" group="general"/>
<reg name="pc" bitsize="32" regnum="15" save-restore="yes" type="code_ptr" group="general"/>
<reg name="xPSR" bitsize="32" regnum="16" save-restore="yes" type="int" group="general"/>
</feature>
</target>
`

// GDB will request this to know the memory map of the device.
var gdbAnnexMemoryMap = `<memory-map>
<memory type="flash" start="0x0" length="0x%x">
<property name="blocksize">0x%x</property>
</memory>
<memory type="ram" start="0x20000000" length="0x%x"/>
</memory-map>
`

// Wait for GDB to connect and handle each connection.
func gdbServer(machine *C.machine_t, port string, runChan chan struct{}) error {
	sock, err := net.Listen("tcp", port)
	if err != nil {
		return err
	}

	m := &Machine{
		machine: machine,
		halted:  false,
		runChan: runChan,
	}

	for {
		conn, err := sock.Accept()
		if err != nil {
			return err
		}

		// Note that we intentionally don't handle the connection in a
		// goroutine, as in general only one GDB connection is supported.
		// Otherwise the two GDB instances will trample all over each other.
		err = gdbHandle(conn, m)
		if err != nil {
			fmt.Fprintln(os.Stderr, "gdb handler error:", err)
		}
	}
}

// Handles a single GDB connection, receiving and handling commands.
func gdbHandle(sock net.Conn, machine *Machine) error {
	conn := bufio.NewReadWriter(bufio.NewReader(sock), bufio.NewWriter(sock))
	acks := true
	packetChan := make(chan string)
	go gdbRecvPackets(conn, packetChan)
	for packet := range packetChan {
		if packet == "" {
			continue
		}

		// This is required before QStartNoAckMode has been negotiated.
		// It has no use over TCP.
		if acks {
			conn.WriteByte('+')
		}

		if strings.HasPrefix(packet, "qSupported:") {
			// Copied from OpenOCD.
			gdbSendPacket(conn, "PacketSize=3fff;qXfer:memory-map:read+;qXfer:features:read+;QStartNoAckMode+")
		} else if packet == "QStartNoAckMode" {
			gdbSendPacket(conn, "OK")
			acks = false
		} else if packet == "Hg0" {
			gdbSendPacket(conn, "OK") // set thread mode
		} else if strings.HasPrefix(packet, "qXfer:") {
			parts := strings.Split(packet[len("qXfer:"):], ":")
			if len(parts) != 4 {
				gdbSendPacket(conn, "")
				continue
			}
			var offset, length int
			_, err := fmt.Sscanf(parts[3], "%x,%x", &offset, &length)
			if err != nil || offset != 0 {
				gdbSendPacket(conn, "")
				continue
			}
			data := ""
			if strings.HasPrefix(packet, "qXfer:features:read:target.xml:") {
				data = gdbAnnexTarget
			} else if strings.HasPrefix(packet, "qXfer:memory-map:read::") {
				data = fmt.Sprintf(gdbAnnexMemoryMap, flagFlashSize*1024, flagFlashPageSize, flagRAMSize*1024)
			} else {
				gdbSendPacket(conn, "")
				continue
			}
			gdbSendPacket(conn, "l"+data)
		} else if strings.HasPrefix(packet, "qSymbol") {
			gdbSendPacket(conn, "OK")
		} else if packet == "qfThreadInfo" {
			// Not sure what this means...
			gdbSendPacket(conn, "l")
		} else if packet == "Hc-1" || packet == "Hc0" {
			// Something about threads.
			// Microcontrollers usually don't have threads.
			gdbSendPacket(conn, "OK")
		} else if packet == "?" {
			// TODO: send error if the program crashed.
			gdbSendPacket(conn, "S00")
		} else if packet[0] == 'p' {
			// Read a specific register.
			var reg int
			_, err := fmt.Sscanf(packet[1:], "%x", &reg)
			if err != nil {
				gdbSendPacket(conn, "")
				continue
			}
			regval := machine.ReadRegister(reg)
			// encode in little-endian format
			reghex := ""
			for i := 0; i < 4; i++ {
				reghex += fmt.Sprintf("%02x", regval&0xff)
				regval /= 256
			}
			gdbSendPacket(conn, reghex)
		} else if packet == "g" {
			// Read all registers.
			regs := machine.ReadRegisters(17)
			gdbSendPacket(conn, hex.EncodeToString(regs))
		} else if packet[0] == 'm' {
			// Read memory in the given range.
			var addr, length int
			_, err := fmt.Sscanf(packet[1:], "%x,%x", &addr, &length)
			if err != nil {
				gdbSendPacket(conn, "")
				continue
			}
			mem := machine.ReadMemory(addr, length)
			out := hex.EncodeToString(mem)
			gdbSendPacket(conn, out)
		} else if packet == "c" {
			// Continue running.
			if machine.Halted() {
				// The target was halted (this is not always the case). Start it
				// again.
				machine.Continue()
			}
			for machine.Running() {
				// TODO: also continue on breakpoints.
				select {
				case packet := <-packetChan:
					if packet == "\x03" {
						machine.Halt()
					} else {
						fmt.Fprintln(os.Stderr, "gdb: unexpected packet during continue:", packet)
					}
				case <-machine.runChan:
					machine.halted = true
				}
			}
			// Send a response only after the target has halted again.
			gdbSendPacket(conn, "S00")
		} else if packet == "s" {
			// Single-step.
			if !machine.Halted() {
				// target not halted
				gdbSendPacket(conn, "E00")
				continue
			}
			result := machine.Step()
			gdbSendPacket(conn, fmt.Sprintf("S%02x", result))
		} else if packet[0] == 'Z' || packet[0] == 'z' {
			// Set or remove a breakpoint.
			num := packet[1] - '0'
			if num >= 4 {
				gdbSendPacket(conn, "E00")
				continue
			}
			var address uint32
			_, err := fmt.Sscanf(packet[2:], ",%x", &address)
			if err != nil {
				gdbSendPacket(conn, "E00")
				continue
			}
			if packet[0] == 'z' {
				// remove breakpoint
				address = 0
			}
			if !machine.SetBreakpoint(int(num), address) {
				gdbSendPacket(conn, "E00")
				continue
			}
			gdbSendPacket(conn, "OK")
		} else {
			// Unknown command, send an empty response.
			gdbSendPacket(conn, "")
		}

		// Make sure the packet is sent, before we deadlock because GDB is still
		// waiting on our packet while we're waiting on GDB's next packet.
		conn.Flush()
	}

	return nil
}

func gdbRecvPackets(conn *bufio.ReadWriter, packetChan chan string) {
	defer close(packetChan)
	for {
		packet, err := gdbRecvPacket(conn)
		if err != nil {
			if err != io.EOF {
				fmt.Fprintln(os.Stderr, "gdb connection error:", err)
			}
			return
		}
		if packet == "" {
			continue
		}
		packetChan <- packet
	}
}

func gdbRecvPacket(conn *bufio.ReadWriter) (string, error) {
	// Packet format: "#payload$cs" where cs is the checksum (two hex bytes).
	// https://www.embecosm.com/appnotes/ean4/embecosm-howto-rsp-server-ean4-issue-2.html#sec_presentation_layer
	c, err := conn.ReadByte()
	if err != nil {
		return "", err
	}
	for c != '$' {
		if c == 3 {
			// Ctrl-C from GDB
			return "\x03", nil
		}
		c, err = conn.ReadByte()
		if err != nil {
			return "", err
		}
	}
	packet, err := conn.ReadString('#')

	// Read the checksum which follows the hash sign
	c1, err := conn.ReadByte()
	if err != nil {
		return "", err
	}
	c2, err := conn.ReadByte()
	if err != nil {
		return "", err
	}
	checksum := string([]byte{c1, c2})

	// parse packet
	// TODO: escaping
	packet = packet[:len(packet)-1] // drop starting '#'
	if len(packet) == 0 {
		return "", nil
	}

	if checksum != gdbPacketChecksum(packet) {
		return "", errors.New("checksum mismatch")
	}

	return packet, nil
}

func gdbSendPacket(conn *bufio.ReadWriter, msg string) error {
	// See gdbRecvPacket for format.
	// TODO: escaping
	packet := fmt.Sprintf("$%s#%s", msg, gdbPacketChecksum(msg))
	_, err := conn.WriteString(packet)
	if err != nil {
		return err
	}
	return nil
}

// Calculate the checksum over the payload of an RSP packet.
func gdbPacketChecksum(msg string) string {
	// https://www.embecosm.com/appnotes/ean4/embecosm-howto-rsp-server-ean4-issue-2.html#sec_presentation_layer
	// From there:
	//   The checksum is the unsigned sum of all the characters in the packet
	//   data modulo 256.
	checksum := uint8(0)
	for _, c := range []byte(msg) {
		checksum += c
	}
	return fmt.Sprintf("%02x", checksum)
}
