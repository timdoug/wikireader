/*
 * Decode a ZIM's WebP thumbnail at display resolution, composite alpha onto
 * white, and convert it to the WikiReader's packed black-pixel bitmap.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zim_image.h"

#include <stdlib.h>
#include <string.h>

#include "src/webp/decode.h"

#ifdef ZIM_APP
#include <grifo.h>
#endif

/* Incremental decoding is also the cooperative scheduling boundary for the
 * single-core firmware.  Large chunks make a drag wait behind tens of
 * milliseconds of WebP work; 128 bytes keeps touch handling close to a video
 * frame without changing the decoder state or restarting the image. */
#define ZIM_WEBP_INPUT_CHUNK 128

struct zim_image_decoder {
	const unsigned char *webp;
	size_t webp_size;
	size_t offset;
	WebPDecoderConfig config;
	WebPIDecoder *decoder;
	unsigned int width;
	unsigned int height;
	unsigned char *bitmap;
	size_t capacity;
	uint8_t *width_out;
	uint16_t *height_out;
	size_t *bitmap_size;
	ZIM_IMAGE_PROGRESS progress;
	void *progress_opaque;
};

static void keep_alive(void)
{
#ifdef ZIM_APP
	watchdog(WATCHDOG_KEY);
#endif
}

static void fit_dimensions(unsigned int source_width,
			   unsigned int source_height,
			   unsigned int requested_width,
			   unsigned int requested_height,
			   unsigned int *width,
			   unsigned int *height)
{
	uint64_t w = requested_width ? requested_width : source_width;
	uint64_t h = requested_height ? requested_height : source_height;

	if (!requested_width && requested_height)
		w = (uint64_t)source_width * h / source_height;
	else if (requested_width && !requested_height)
		h = (uint64_t)source_height * w / source_width;
	if (!w) w = 1;
	if (!h) h = 1;
	if (w > ZIM_IMAGE_MAX_WIDTH) {
		h = h * ZIM_IMAGE_MAX_WIDTH / w;
		w = ZIM_IMAGE_MAX_WIDTH;
		if (!h) h = 1;
	}
	if (h > ZIM_IMAGE_MAX_HEIGHT) {
		w = w * ZIM_IMAGE_MAX_HEIGHT / h;
		h = ZIM_IMAGE_MAX_HEIGHT;
		if (!w) w = 1;
	}
	*width = (unsigned int)w;
	*height = (unsigned int)h;
}

int zim_image_fit_dimensions(unsigned int source_width,
			     unsigned int source_height,
			     unsigned int requested_width,
			     unsigned int requested_height,
			     uint8_t *width_out, uint16_t *height_out,
			     size_t *bitmap_size)
{
	unsigned int width;
	unsigned int height;

	if (!source_width || !source_height || !width_out || !height_out ||
	    !bitmap_size)
		return -1;
	fit_dimensions(source_width, source_height, requested_width,
		       requested_height, &width, &height);
	*width_out = (uint8_t)width;
	*height_out = (uint16_t)height;
	*bitmap_size = (size_t)((width + 7) / 8) * height;
	return 0;
}

static int dither_atkinson(const unsigned char *pixels, unsigned int width,
			   unsigned int height, int stride, int rgba,
			   const unsigned char *alpha, int alpha_stride,
			   unsigned char *bitmap, size_t capacity,
			   size_t *bitmap_size, ZIM_IMAGE_PROGRESS progress,
			   void *progress_opaque)
{
	size_t row_bytes = (width + 7) / 8;
	size_t required = row_bytes * height;
	int *errors;
	int *row0;
	int *row1;
	int *row2;
	unsigned int y;

	if (required > capacity)
		return -1;
	errors = calloc((size_t)(width + 4) * 3, sizeof(*errors));
	if (!errors)
		return -1;
	memset(bitmap, 0, required);
	row0 = errors;
	row1 = errors + width + 4;
	row2 = errors + (width + 4) * 2;
	for (y = 0; y < height; y++) {
		const unsigned char *pixel = pixels + (size_t)y * stride;
		const unsigned char *alpha_pixel = alpha ?
			alpha + (size_t)y * alpha_stride : NULL;
		unsigned int x;
		for (x = 0; x < width; x++, pixel += rgba ? 4 : 1) {
			unsigned int opacity = rgba ? pixel[3] :
				(alpha_pixel ? alpha_pixel[x] : 255);
			int gray;
			int value;
			int quantized;
			int error;

			if (rgba) {
				gray = (77 * pixel[0] + 150 * pixel[1] +
					29 * pixel[2] + 128) >> 8;
			} else {
				/* WebP's Y plane is studio-range BT.601. The RGB
				 * grayscale weights reduce to this luma expansion,
				 * avoiding chroma upsampling and RGB conversion. */
				if (pixel[0] <= 16)
					gray = 0;
				else if (pixel[0] >= 235)
					gray = 255;
				else
					gray = ((pixel[0] - 16) * 149 + 64) >> 7;
			}
			if (!opacity)
				gray = 255;
			else if (opacity != 255)
				gray = (gray * (int)opacity +
					255 * (255 - (int)opacity) + 127) / 255;
			value = gray + row0[x + 1];
			if (value < 0) value = 0;
			if (value > 255) value = 255;
			quantized = value < 128 ? 0 : 255;
			if (!quantized)
				bitmap[(size_t)y * row_bytes + x / 8] |=
					(unsigned char)(0x80U >> (x & 7));
			error = (value - quantized) / 8;
			row0[x + 2] += error;
			row0[x + 3] += error;
			row1[x] += error;
			row1[x + 1] += error;
			row1[x + 2] += error;
			row2[x + 1] += error;
		}
		{
			int *old = row0;
			row0 = row1;
			row1 = row2;
			row2 = old;
			memset(row2, 0, (size_t)(width + 4) * sizeof(*row2));
		}
		if (!(y & 15)) {
			keep_alive();
			if (progress)
				progress(progress_opaque, 80 + (size_t)y * 20 /
					 height, 100);
		}
	}
	free(errors);
	*bitmap_size = required;
	if (progress)
		progress(progress_opaque, 100, 100);
	return 0;
}

ZIM_IMAGE_DECODER *zim_image_decoder_create(const unsigned char *webp,
					     size_t webp_size,
					     unsigned int requested_width,
					     unsigned int requested_height,
					     unsigned char *bitmap,
					     size_t capacity,
					     uint8_t *width_out,
					     uint16_t *height_out,
					     size_t *bitmap_size,
					     ZIM_IMAGE_PROGRESS progress,
					     void *progress_opaque)
{
	ZIM_IMAGE_DECODER *state;
	unsigned int width;
	unsigned int height;

	if (!webp || !webp_size || !bitmap || !width_out || !height_out ||
	    !bitmap_size)
		return NULL;
	state = calloc(1, sizeof(*state));
	if (!state || !WebPInitDecoderConfig(&state->config))
		goto error;
	if (WebPGetFeatures(webp, webp_size, &state->config.input) !=
		VP8_STATUS_OK || state->config.input.width <= 0 ||
	    state->config.input.height <= 0 || state->config.input.has_animation)
		goto error;
	fit_dimensions((unsigned int)state->config.input.width,
		       (unsigned int)state->config.input.height,
		       requested_width, requested_height, &width, &height);
	if ((size_t)((width + 7) / 8) * height > capacity)
		goto error;
	state->config.output.colorspace = MODE_RGBA;
	state->config.options.use_scaling =
		width != (unsigned int)state->config.input.width ||
		height != (unsigned int)state->config.input.height;
	state->config.options.scaled_width = (int)width;
	state->config.options.scaled_height = (int)height;
	state->config.options.no_fancy_upsampling = 1;
	state->config.options.use_threads = 0;
	/* Lossy WebP is already YUV. Dither its luma plane directly instead of
	 * spending target cycles and memory upsampling chroma into RGBA. The
	 * lossless decoder requires an RGB output mode. */
	if (state->config.input.format == 1)
		state->config.output.colorspace = state->config.input.has_alpha ?
			MODE_YUVA : MODE_YUV;
	state->decoder = WebPIDecode(NULL, 0, &state->config);
	if (!state->decoder)
		goto error;
	state->webp = webp;
	state->webp_size = webp_size;
	state->width = width;
	state->height = height;
	state->bitmap = bitmap;
	state->capacity = capacity;
	state->width_out = width_out;
	state->height_out = height_out;
	state->bitmap_size = bitmap_size;
	state->progress = progress;
	state->progress_opaque = progress_opaque;
	return state;

error:
	if (state) {
		WebPIDelete(state->decoder);
		WebPFreeDecBuffer(&state->config.output);
		free(state);
	}
	return NULL;
}

int zim_image_decoder_step(ZIM_IMAGE_DECODER *state)
{
	VP8StatusCode decode_status;
	int result;

	if (!state || state->offset >= state->webp_size)
		return -1;
	{
		size_t amount = state->webp_size - state->offset;
		if (amount > ZIM_WEBP_INPUT_CHUNK)
			amount = ZIM_WEBP_INPUT_CHUNK;
		decode_status = WebPIAppend(state->decoder,
			state->webp + state->offset, amount);
		state->offset += amount;
		keep_alive();
		if (state->progress)
			state->progress(state->progress_opaque,
				state->offset * 80 / state->webp_size, 100);
		if (decode_status != VP8_STATUS_SUSPENDED &&
		    !(decode_status == VP8_STATUS_OK &&
		      state->offset == state->webp_size))
			return -1;
	}
	if (decode_status == VP8_STATUS_SUSPENDED)
		return 1;
	if (WebPIsRGBMode(state->config.output.colorspace)) {
		result = dither_atkinson(state->config.output.u.RGBA.rgba,
				    state->width, state->height,
				    state->config.output.u.RGBA.stride, 1,
				    NULL, 0, state->bitmap, state->capacity,
				    state->bitmap_size, state->progress,
				    state->progress_opaque);
		if (result)
			return -1;
	} else {
		result = dither_atkinson(state->config.output.u.YUVA.y,
				   state->width, state->height,
				   state->config.output.u.YUVA.y_stride, 0,
				   state->config.output.u.YUVA.a,
				   state->config.output.u.YUVA.a_stride,
				   state->bitmap, state->capacity,
				   state->bitmap_size, state->progress,
				   state->progress_opaque);
		if (result)
			return -1;
	}
	*state->width_out = (uint8_t)state->width;
	*state->height_out = (uint16_t)state->height;
	return 0;
}

void zim_image_decoder_destroy(ZIM_IMAGE_DECODER *state)
{
	if (!state)
		return;
	WebPIDelete(state->decoder);
	WebPFreeDecBuffer(&state->config.output);
	free(state);
}

int zim_webp_to_bitmap_progress(const unsigned char *webp, size_t webp_size,
				unsigned int requested_width,
				unsigned int requested_height,
				unsigned char *bitmap, size_t capacity,
				uint8_t *width_out, uint16_t *height_out,
				size_t *bitmap_size,
				ZIM_IMAGE_PROGRESS progress,
				void *progress_opaque)
{
	ZIM_IMAGE_DECODER *decoder;
	int result;

	decoder = zim_image_decoder_create(webp, webp_size, requested_width,
		requested_height, bitmap, capacity, width_out, height_out,
		bitmap_size, progress, progress_opaque);
	if (!decoder)
		return -1;
	do {
		result = zim_image_decoder_step(decoder);
	} while (result > 0);
	zim_image_decoder_destroy(decoder);
	return result;
}

int zim_webp_to_bitmap(const unsigned char *webp, size_t webp_size,
		       unsigned int requested_width,
		       unsigned int requested_height,
		       unsigned char *bitmap, size_t capacity,
		       uint8_t *width_out, uint16_t *height_out,
		       size_t *bitmap_size)
{
	return zim_webp_to_bitmap_progress(webp, webp_size, requested_width,
		requested_height, bitmap, capacity, width_out, height_out,
		bitmap_size, NULL, NULL);
}
