
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <stdbool.h>

static int terminal_buf = -1;
static struct termios terminal_termios_state;

void terminal_disable_raw() {
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &terminal_termios_state);
}

void terminal_enable_raw() {
	static bool enabled_raw = false;
	if (enabled_raw) {
		return; // make enabling idempotent
	}
	atexit(terminal_disable_raw);
	tcgetattr(STDIN_FILENO, &terminal_termios_state);
	terminal_termios_state.c_lflag &= ~(ECHO | ICANON);
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &terminal_termios_state);
}

static int terminal_getchar_raw() {
	terminal_enable_raw(); // idempotent

	if (terminal_buf >= 0) {
		int c = terminal_buf;
		terminal_buf = -1;
		return c;
	}
	int c = getchar(); // TODO: this blocks
	if (c == '\n') {
		terminal_buf = '\n';
		return '\r';
	}
	return c;
}

int terminal_getchar() {
	int c = terminal_getchar_raw();
	return c;
}

void terminal_putchar(int c) {
	putchar(c);
}
