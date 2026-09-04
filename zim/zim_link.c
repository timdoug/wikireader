#include "zim_link.h"

#include <string.h>

static int hex_digit(unsigned char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

int zim_link_normalize(const char *base_path, const unsigned char *href,
		       size_t href_length, char *path, size_t capacity)
{
	unsigned char joined[1024];
	size_t base_length = 0;
	size_t joined_length;
	size_t input;
	size_t output = 0;

	if (!base_path || !href || !path || capacity < 2)
		return -1;
	while (href_length && href[href_length - 1] == ' ')
		href_length--;
	for (input = 0; input < href_length; input++)
		if (href[input] == '?' || href[input] == '#') {
			href_length = input;
			break;
		}
	if (!href_length || href[0] == '/' ||
	    (href_length >= 2 && href[0] == '/' && href[1] == '/'))
		return -1;
	for (input = 0; base_path[input]; input++)
		if (base_path[input] == '/')
			base_length = input + 1;
	if (base_length + href_length >= sizeof(joined))
		return -1;
	memcpy(joined, base_path, base_length);
	joined_length = base_length;
	for (input = 0; input < href_length; input++) {
		int high;
		int low;

		if (href[input] == '%' && input + 2 < href_length &&
		    (high = hex_digit(href[input + 1])) >= 0 &&
		    (low = hex_digit(href[input + 2])) >= 0) {
			unsigned char decoded = (unsigned char)(high * 16 + low);
			if (!decoded || decoded < 0x20)
				return -1;
			joined[joined_length++] = decoded;
			input += 2;
		} else if (href_length - input >= 5 &&
			   !memcmp(href + input, "&amp;", 5)) {
			joined[joined_length++] = '&';
			input += 4;
		} else {
			joined[joined_length++] = href[input];
		}
	}

	input = 0;
	while (input < joined_length) {
		size_t start;
		size_t length;

		while (input < joined_length && joined[input] == '/')
			input++;
		start = input;
		while (input < joined_length && joined[input] != '/')
			input++;
		length = input - start;
		if (!length || (length == 1 && joined[start] == '.'))
			continue;
		if (length == 2 && joined[start] == '.' && joined[start + 1] == '.') {
			while (output && path[output - 1] == '/')
				output--;
			while (output && path[output - 1] != '/')
				output--;
			continue;
		}
		if (output && path[output - 1] != '/') {
			if (output >= capacity - 1)
				return -1;
			path[output++] = '/';
		}
		if (length >= capacity - output)
			return -1;
		memcpy(path + output, joined + start, length);
		output += length;
	}
	if (!output)
		return -1;
	path[output] = '\0';
	return 0;
}
