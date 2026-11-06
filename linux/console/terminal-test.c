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

	for (unsigned int x = 0; x < TEXT_COLUMNS; x++)
		line[x] = cells[row][x].codepoint < 128 ? cells[row][x].codepoint : '?';
	line[TEXT_COLUMNS] = '\0';
	if (memcmp(line, text, length) ||
	    (length < TEXT_COLUMNS && cells[row][length].codepoint != ' ')) {
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

/* Compare the byte painter with a pixel reference at every byte offset,
 * including inverse video and the keyboard's taller glyphs. */
static void test_glyph_painter(void)
{
	static uint8_t painted[LCD_BYTES];
	const uint8_t *glyphs[] = { console_font['A'], unicode_glyph(0x3bb),
		key_font['W' - KEY_FONT_FIRST] };

	for (unsigned int g = 0; g < 3; g++) {
		unsigned int height = g == 2 ? KEY_FONT_HEIGHT : FONT_HEIGHT;

		for (unsigned int offset = 0; offset < 8; offset++) {
			for (int inverse = 0; inverse < 2; inverse++) {
				memset(framebuffer, 0xa5, sizeof(framebuffer));
				draw_glyph(80 + offset, 8, glyphs[g], height, inverse);
				memcpy(painted, framebuffer, sizeof(painted));
				memset(framebuffer, 0xa5, sizeof(framebuffer));
				for (unsigned int y = 0; y < height; y++)
					for (unsigned int x = 0; x < FONT_WIDTH; x++)
						set_pixel(80 + offset + x, 8 + y,
							  !!(glyphs[g][y] & (0x80 >> x)) ^ inverse);
				expect(!memcmp(painted, framebuffer, sizeof(painted)),
				       "glyph painter matches pixel reference");
			}
		}
	}
	memset(framebuffer, 0, sizeof(framebuffer));
}

static void test_scroll_rows(void)
{
	static struct text_cell before[TEXT_ROWS][TEXT_COLUMNS];
	static unsigned char before_attrs[TEXT_ROWS][TEXT_COLUMNS];
	const unsigned int counts[] = { 1, 2, TEXT_ROWS, TEXT_ROWS + 1 };
	char sequence[64];

	for (unsigned int partial = 0; partial < 2; partial++) {
		unsigned int top = partial ? 3 : 0;
		unsigned int bottom = partial ? 9 : TEXT_ROWS - 1;

		for (unsigned int down = 0; down < 2; down++) {
			for (unsigned int n = 0; n < sizeof(counts) / sizeof(counts[0]); n++) {
				unsigned int count = counts[n];

				reset();
				for (unsigned int row = 0; row < TEXT_ROWS; row++) {
					snprintf(sequence, sizeof(sequence), "\033[%u;1H\033[%um%c",
						 row + 1, row & 1 ? 7 : 0, 'A' + row);
					feed(sequence);
					feed("\xcc\x81\xe6\x97\xa5\xcc\x88");
					memcpy(before[row], cells[row], sizeof(before[row]));
					memcpy(before_attrs[row], attrs[row], TEXT_COLUMNS);
				}
				snprintf(sequence, sizeof(sequence), "\033[%u;%ur\033[%u%c",
					 top + 1, bottom + 1, count, down ? 'T' : 'S');
				feed(sequence);
				if (count > bottom - top + 1)
					count = bottom - top + 1;
				for (unsigned int row = 0; row < TEXT_ROWS; row++) {
					int source = row;

					if (row >= top && row <= bottom)
						source += down ? -(int)count : (int)count;
					if ((source >= (int)top && source <= (int)bottom) ||
					    row < top || row > bottom) {
						expect(!memcmp(cells[row], before[source], sizeof(before[0])) &&
						       !memcmp(attrs[row], before_attrs[source], TEXT_COLUMNS),
						       "scroll keeps rows, marks, wide pairs and attributes together");
					} else {
						for (unsigned int x = 0; x < TEXT_COLUMNS; x++)
							expect(cells[row][x].codepoint == ' ' &&
							       !cells[row][x].marks[0] && !cells[row][x].marks[1] &&
							       !attrs[row][x], "recycled scroll row is blank");
					}
				}
			}
		}
	}
}

static const char unicode_demo[] =
	"\033[?25l\033[2J\033[H"
	"UTF-8: caf\xc3\xa9 \xce\xbb \xd0\x96 \xe2\x82\xac\r\n"
	"Combining: e\xcc\x81 = \xc3\xa9\r\n"
	"Wide: A\xe6\x97\xa5" "B\xf0\x9f\x98\x80" "C\r\n"
	"\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90 \xe2\x94\x82 \xe2\x94\x94\xe2\x94\x80\xe2\x94\x98\r\n"
	"ASCII and keyboard still work.";

int main(int argc, char **argv)
{
	int pipe_fds[2];
	char reply[32];
	char sequence[16];
	ssize_t length;
	unsigned int i;

	if (!setlocale(LC_CTYPE, "C.UTF-8"))
		return 1;
	if (argc == 2 && !strcmp(argv[1], "--demo")) {
		fputs(unicode_demo, stdout);
		return 0;
	}
	if (argc == 2 && !strcmp(argv[1], "--render")) {
		reset();
		feed(unicode_demo);
		return fwrite(framebuffer, 1, TEXT_HEIGHT * LCD_STRIDE, stdout) !=
			TEXT_HEIGHT * LCD_STRIDE;
	}
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
	expect(cells[4][9].codepoint == 'X', "cursor addressing");
	feed("\033[HA\033[3;1HB");
	expect(cells[0][0].codepoint == 'A' && cells[2][0].codepoint == 'B', "home and row;col");
	feed("\033[2AC");
	expect(cells[0][1].codepoint == 'C', "cursor up");
	feed("\033[10D\033[40CZ");
	expect(cells[0][39].codepoint == 'Z', "left and right clamp to the edges");

	/* A full last column does not scroll until the next character. */
	reset();
	snprintf(sequence, sizeof(sequence), "\033[%d;1H", TEXT_ROWS);
	feed(sequence);
	for (i = 0; i < TEXT_COLUMNS; i++)
		feed("x");
	expect_cursor(TEXT_ROWS - 1, 39, "pending wrap");
	expect(cells[TEXT_ROWS - 1][39].codepoint == 'x' &&
	       cells[TEXT_ROWS - 2][0].codepoint == ' ', "no early scroll");
	feed("\r");
	expect_cursor(TEXT_ROWS - 1, 0, "carriage return cancels the wrap");
	snprintf(sequence, sizeof(sequence), "\033[%d;40Hyz", TEXT_ROWS);
	feed(sequence);
	expect(cells[TEXT_ROWS - 2][39].codepoint == 'y' && cells[TEXT_ROWS - 1][0].codepoint == 'z',
	       "wrap scrolls on the next character");

	reset();
	feed("a\tb");
	expect(cells[0][8].codepoint == 'b', "tab stops every 8");

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
	feed("\033]0;title\007ok\033[11m\xc4\xb3\xda\033[10m");
	expect(cells[0][0].codepoint == 'o' && cells[0][1].codepoint == 'k',
	       "title ignored");
	expect(cells[0][2].codepoint == 0x2500 &&
	       cells[0][3].codepoint == 0x2502 &&
	       cells[0][4].codepoint == 0x250c && !cp437_mode,
	       "explicit Linux alternate font keeps narrow curses line drawing");

	reset();
	feed("abc\033[2J");
	expect_row(0, "", "erase display");

	reset();
	feed("caf\xc3");
	expect_cursor(0, 3, "partial UTF-8 waits across output reads");
	feed("\xa9 \xce\xbb \xd0\x96 \xe2\x82\xac");
	expect(cells[0][3].codepoint == 0xe9 && cells[0][5].codepoint == 0x3bb &&
	       cells[0][7].codepoint == 0x416 && cells[0][9].codepoint == 0x20ac,
	       "Latin, Greek, Cyrillic and euro are Unicode scalars");
	expect_cursor(0, 10, "UTF-8 advances by characters, not bytes");
	expect(unicode_glyph(0xe9) && unicode_glyph(0x3bb) && unicode_glyph(0x416) &&
	       unicode_glyph(0x20ac) && unicode_glyph(0xfffd), "Unicode font coverage");
	expect(!memcmp(unicode_glyph(0x2500), console_font[0xc4], FONT_HEIGHT),
	       "Unicode box drawing retains the original joined bitmap");
	feed("\033(0lqk\033(B\033)0\016x\017A");
	expect(cells[0][10].codepoint == 0x250c &&
	       cells[0][11].codepoint == 0x2500 && cells[0][12].codepoint == 0x2510 &&
	       cells[0][13].codepoint == 0x2502 && cells[0][14].codepoint == 'A',
	       "DEC G0 and G1 line drawing and ordinary ASCII");

	reset();
	feed("e\xcc\x81");
	expect_cursor(0, 1, "combining accent does not advance");
	expect(cells[0][0].codepoint == 'e' && cells[0][0].marks[0] == 0x301,
	       "combining mark stays with its base");
	uint8_t composed[LCD_BYTES];
	memcpy(composed, framebuffer, sizeof(composed));
	reset();
	feed("\xc3\xa9");
	expect(!memcmp(composed, framebuffer, sizeof(composed)),
	       "decomposed and precomposed e-acute have the same bitmap");
	reset();
	feed("a\xcc\x81\xcc\x88\xcc\x82");
	expect_cursor(0, 1, "extra combining marks stay bounded and zero width");
	feed("\033[1;1H\033[@");
	expect(cells[0][1].codepoint == 'a' && cells[0][1].marks[1] == 0x308,
	       "insertion carries combining marks");
	feed("\033[P");
	expect(cells[0][0].marks[0] == 0x301, "deletion carries combining marks");
	feed("\033[X");
	expect(!cells[0][0].marks[0], "erase removes combining marks");

	reset();
	feed("\xe6\x97\xa5" "A\xf0\x9f\x98\x80" "B");
	expect(cells[0][0].codepoint == 0x65e5 && !cells[0][1].codepoint &&
	       cells[0][2].codepoint == 'A' && cells[0][3].codepoint == 0x1f600 &&
	       !cells[0][4].codepoint && cells[0][5].codepoint == 'B',
	       "CJK and supplementary emoji retain scalars and two columns");
	expect_cursor(0, 6, "wide characters advance by two");
	feed("\033[1;2HX");
	expect(cells[0][0].codepoint == ' ' && cells[0][1].codepoint == 'X',
	       "overwriting a wide tail clears the old lead");
	feed("\033[1;4HX");
	expect(cells[0][4].codepoint == ' ', "overwriting wide lead clears its tail");
	feed("\033[1;39H\xe6\x97\xa5\xcc\x81");
	expect_cursor(0, 39, "wide character fills the margin without early wrap");
	expect(wrap_pending && cells[0][38].marks[0] == 0x301,
	       "combining mark at the margin attaches before wrap");
	feed("X");
	expect_cursor(1, 1, "next glyph wraps after a wide character");
	feed("\033[2;40HX\033[2;40H\xe6\x97\xa5");
	expect(cells[1][39].codepoint == ' ' && cells[2][0].codepoint == 0x65e5 &&
	       !cells[2][1].codepoint, "wide glyph clears padding and wraps before the last single column");
	feed("\033[3;2H\033[X");
	expect(cells[2][0].codepoint == ' ' && cells[2][1].codepoint == ' ',
	       "erasing a wide tail clears the whole pair");
	reset();
	feed("A\xe6\x97\xa5\xcc\x81Z\033[1;1H\033[@");
	expect(cells[0][2].codepoint == 0x65e5 && !cells[0][3].codepoint &&
	       cells[0][2].marks[0] == 0x301, "insertion moves wide glyph and marks");
	feed("\033[1;4H\033[P");
	expect(cells[0][2].codepoint == ' ' && cells[0][3].codepoint == 'Z',
	       "deleting through a wide glyph repairs the split pair");
	reset();
	feed("\033[15;1H\xe6\x97\xa5\xcc\x81\r\n");
	expect(cells[13][0].codepoint == 0x65e5 && !cells[13][1].codepoint &&
	       cells[13][0].marks[0] == 0x301, "scrolling carries wide cells and marks");

	reset();
	feed("\xc3X\xe2\033[2;1HY");
	expect(cells[0][0].codepoint == 0xfffd && cells[0][1].codepoint == 'X' &&
	       cells[0][2].codepoint == 0xfffd && cells[1][0].codepoint == 'Y',
	       "invalid continuations preserve ASCII and escape sequences");
	reset();
	feed("\xc0\xaf\xed\xa0\x80\xf4\x90\x80\x80");
	expect_cursor(0, 9, "overlong, surrogate and out-of-range encodings rejected");
	for (i = 0; i < 9; i++)
		expect(cells[0][i].codepoint == 0xfffd, "invalid scalar becomes U+FFFD");
	feed("\xf0\x90\x80\x80");
	expect(cells[0][9].codepoint == 0x10000, "lowest supplementary scalar decoded");
	feed("\xc3\033cA");
	expect_row(0, "A", "reset clears partial UTF-8 and charset state");

	test_glyph_painter();
	test_scroll_rows();

	printf("TERMINAL %s: %d failed\n", failures ? "FAIL" : "PASS",
	       failures);
	return failures != 0;
}
