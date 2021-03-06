
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <stdbool.h>

// This file handles raw terminal input and output.

static int terminal_buf = -1;
static struct termios terminal_termios_state;
static bool terminal_enabled_raw = false;

void terminal_disable_raw() {
	if (!terminal_enabled_raw) {
		return;
	}
	terminal_enabled_raw = false;
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &terminal_termios_state);
}

void terminal_enable_raw() {
	// https://viewsourcecode.org/snaptoken/kilo/02.enteringRawMode.html
	if (terminal_enabled_raw) {
		return; // make enabling idempotent
	}
	terminal_enabled_raw = true;
	atexit(terminal_disable_raw);
	tcgetattr(STDIN_FILENO, &terminal_termios_state);
	struct termios state = terminal_termios_state;
	state.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	state.c_lflag &= ~(ECHO | ICANON | ISIG);
	state.c_lflag &= ~(ECHO);
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &state);
}

static int terminal_getchar_raw() {
	terminal_enable_raw(); // idempotent

	if (terminal_buf >= 0) {
		int c = terminal_buf;
		terminal_buf = -1;
		return c;
	}
	int c = getchar(); // TODO: this blocks
	if (c == 24) { // Ctrl-X
		exit(0);
	}
	return c;
}

int terminal_getchar() {
	int c = terminal_getchar_raw();
	return c;
}

void terminal_putchar(int c) {
	// bypass all buffering etc.
	write(STDOUT_FILENO, &c, 1);
}
