#include <stdlib.h>

#include "alac_wrapper.h"
#include "ALACDecoder.h"
#include "ALACBitUtilities.h"

struct alac_codec_s {
	ALACDecoder *decoder;
	ALACSpecificConfig config;
};

struct alac_codec_s *alac_create_decoder(int magic_cookie_size, unsigned char *magic_cookie,
								unsigned char *sample_size, unsigned *sample_rate,
								unsigned char *channels, unsigned int *block_size) {
	if (!magic_cookie || magic_cookie_size <= 0) {
		return NULL;
	}

	alac_codec_s *codec = (alac_codec_s *)calloc(1, sizeof(alac_codec_s));
	if (!codec) {
		return NULL;
	}

	codec->decoder = new ALACDecoder();
	if (!codec->decoder) {
		free(codec);
		return NULL;
	}

	if (codec->decoder->Init(magic_cookie, (uint32_t)magic_cookie_size) != ALAC_noErr) {
		delete codec->decoder;
		free(codec);
		return NULL;
	}

	codec->config = codec->decoder->mConfig;

	if (sample_size) *sample_size = codec->config.bitDepth;
	if (sample_rate) *sample_rate = codec->config.sampleRate;
	if (channels) *channels = codec->config.numChannels;
	if (block_size) *block_size = codec->config.frameLength;

	return codec;
}

void alac_delete_decoder(struct alac_codec_s *codec) {
	if (!codec) return;
	delete codec->decoder;
	free(codec);
}

bool alac_to_pcm(struct alac_codec_s *codec, unsigned char *input, size_t input_len,
				 unsigned char *output, char channels, unsigned *out_frames) {
	if (!codec || !codec->decoder || !input || !output) {
		return false;
	}

	uint32_t decoded_samples = 0;
	BitBuffer bits;
	uint32_t byteSize = (uint32_t)input_len;

	if (byteSize == 0) {
		byteSize = codec->config.maxFrameBytes;
	}

	BitBufferInit(&bits, input, byteSize);

	int32_t status = codec->decoder->Decode(&bits, output, codec->config.frameLength,
											(uint32_t)channels, &decoded_samples);
	if (status != ALAC_noErr) {
		return false;
	}

	if (out_frames) {
		*out_frames = decoded_samples;
	}

	return true;
}
