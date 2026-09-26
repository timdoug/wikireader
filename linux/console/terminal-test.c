// SPDX-License-Identifier: GPL-2.0-only
/*
 * Host test of wr-console's terminal: feeds it the sequences curses sends
 * for TERM=linux and checks the screen model.
 *
 *   cc -o terminal-test terminal-test.c && ./terminal-test
 */
#define main wr_console_main
#include "wr-console.c"
#undef main

static int failures;

static void feed(const char *text)
{
	while (*text)
		terminal_byte((unsigned char)*text++);
}

static void reset(void)
{
	terminal_reset();
	escape_state = ESCAPE_NONE;
}

static void expect_row(unsigned int row, const char *text, const char *what)
{
	char line[TEXT_COLUMNS + 1];
	size_t length = strlen(text);

	memcpy(line, cells[row], TEXT_COLUMNS);
	line[TEXT_COLUMNS] = '\0';
	if (memcmp(cells[row], text, length) ||
	    (length < TEXT_COLUMNS && cells[row][length] != ' ')) {
		printf("FAIL %s: row %u is \"%s\"\n", what, row, line);
		failures++;
	}
}

static void expect_cursor(unsigned int row, unsigned int column,
			  const char *what)
{
	if (cursor_y != row || cursor_x != column) {
		printf("FAIL %s: cursor at %u,%u, expected %u,%u\n", what,
		       cursor_y, cursor_x, row, column);
		failures++;
	}
}

static void expect(int ok, const char *what)
{
	if (!ok) {
		printf("FAIL %s\n", what);
		failures++;
	}
}

int main(void)
{
	int pipe_fds[2];
	char reply[32];
	ssize_t length;
	unsigned int i;

	index_glyphs();
	if (pipe(pipe_fds))
		return 1;
	terminal_fd = pipe_fds[1];

	reset();
	feed("hello\r\nworld");
	expect_row(0, "hello", "text");
	expect_row(1, "world", "second line");
	expect_cursor(1, 5, "after text");

	reset();
	feed("\033[5;10HX");
	expect(cells[4][9] == 'X', "cursor addressing");
	feed("\033[HA\033[3;1HB");
	expect(cells[0][0] == 'A' && cells[2][0] == 'B', "home and row;col");
	feed("\033[2AC");
	expect(cells[0][1] == 'C', "cursor up");
	feed("\033[10D\033[40CZ");
	expect(cells[0][39] == 'Z', "left and right clamp to the edges");

	/* A full last column does not scroll until the next character. */
	reset();
	feed("\033[14;1H");
	for (i = 0; i < TEXT_COLUMNS; i++)
		feed("x");
	expect_cursor(13, 39, "pending wrap");
	expect(cells[13][39] == 'x' && cells[12][0] == ' ', "no early scroll");
	feed("\r");
	expect_cursor(13, 0, "carriage return cancels the wrap");
	feed("\033[14;40Hyz");
	expect(cells[12][39] == 'y' && cells[13][0] == 'z',
	       "wrap scrolls on the next character");

	reset();
	feed("a\tb");
	expect(cells[0][8] == 'b', "tab stops every 8");

	reset();
	feed("abcdef\033[1;3H\033[2@");
	expect_row(0, "ab  cdef", "insert characters");
	feed("\033[3P");
	expect_row(0, "abdef", "delete characters");
	feed("\033[1;2H\033[2X");
	expect_row(0, "a  ef", "erase characters");
	feed("\033[1;3H\033[K");
	expect_row(0, "a", "erase to end of line");

	/* Scrolling region: only rows 2..4 move. */
	reset();
	for (i = 0; i < 6; i++) {
		char line[8];

		snprintf(line, sizeof(line), "\033[%u;1H%u", i + 1, i);
		feed(line);
	}
	feed("\033[2;4r\033[4;1H\n");
	expect_row(0, "0", "row above the region stays");
	expect_row(1, "2", "region scrolled up");
	expect_row(3, "", "new line at the region's bottom");
	expect_row(4, "4", "row below the region stays");
	feed("\033[2;1H\033M");
	expect_row(1, "", "reverse index scrolls the region down");
	expect_row(2, "2", "reverse index moved the line");
	feed("\033[r");

	reset();
	feed("\033[1;1H1\033[2;1H2\033[3;1H3\033[2;1H\033[L");
	expect_row(1, "", "insert line");
	expect_row(2, "2", "insert line pushes down");
	feed("\033[M");
	expect_row(1, "2", "delete line pulls up");

	reset();
	feed("a\033[7mb\033[27mc\033[7md\033[0;10me");
	expect(!attrs[0][0] && attrs[0][1] == ATTR_REVERSE && !attrs[0][2] &&
	       attrs[0][3] == ATTR_REVERSE && !attrs[0][4], "reverse video");

	reset();
	feed("\033[?25l\033[?1c");
	expect(!cursor_visible, "cursor hidden, and ?1c is not a query");
	feed("\033[?25h\033[?0c");
	expect(cursor_visible, "cursor shown");

	reset();
	feed("\0337\033[5;6H\0338");
	expect_cursor(0, 0, "save and restore cursor");
	feed("\033[3;7H\033[6n");
	length = read(pipe_fds[0], reply, sizeof(reply) - 1);
	reply[length > 0 ? length : 0] = '\0';
	expect(!strcmp(reply, "\033[3;7R"), "cursor position report");

	reset();
	feed("\033]0;title\007ok\xc4\xb3\xda");
	expect_row(0, "ok-|+", "title ignored, line drawing as ASCII");

	reset();
	feed("abc\033[2J");
	expect_row(0, "", "erase display");

	printf("TERMINAL %s: %d failed\n", failures ? "FAIL" : "PASS",
	       failures);
	return failures != 0;
}
