/*
    DABlin - capital DAB experience
    Copyright (C) 2015 Stefan Pöschel

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "dabplus_decoder.h"


// --- SuperframeFilter -----------------------------------------------------------------
SuperframeFilter::SuperframeFilter(SubchannelSinkObserver* observer) : SubchannelSink(observer) {
	aac_dec = NULL;

	frame_len = 0;
	frame_count = 0;
	sync_frames = 0;

	sf_raw = NULL;
	sf = NULL;
	sf_len = 0;

	sf_format_set = false;
	sf_format_raw = 0;

	num_aus = 0;
}

SuperframeFilter::~SuperframeFilter() {
	delete[] sf_raw;
	delete[] sf;
	delete aac_dec;
}

void SuperframeFilter::Feed(const uint8_t *data, size_t len) {
	// check frame len
	if(frame_len) {
		if(frame_len != len) {
			fprintf(stderr, "SuperframeFilter: different frame len %zu (should be: %zu) - frame ignored!\n", len, frame_len);
			return;
		}
	} else {
		if(len < 10) {
			fprintf(stderr, "SuperframeFilter: frame len %zu too short - frame ignored!\n", len);
			return;
		}
		if((5 * len) % 120) {
			fprintf(stderr, "SuperframeFilter: resulting Superframe len of len %zu not divisible by 120 - frame ignored!\n", len);
			return;
		}

		frame_len = len;
		sf_len = 5 * frame_len;

		sf_raw = new uint8_t[sf_len];
		sf = new uint8_t[sf_len];
	}

	if(frame_count == 5) {
		// shift previous frames
		for(int i = 0; i < 4; i++)
			memcpy(sf_raw + i * frame_len, sf_raw + (i + 1) * frame_len, frame_len);
	} else {
		frame_count++;
	}

	// copy frame
	memcpy(sf_raw + (frame_count - 1) * frame_len, data, frame_len);

	if(frame_count < 5)
		return;


	// append RS coding on copy
	memcpy(sf, sf_raw, sf_len);
	rs_dec.DecodeSuperframe(sf, sf_len);

	if(!CheckSync()) {
		if(sync_frames == 0)
			fprintf(stderr, "SuperframeFilter: Superframe sync started...\n");
		sync_frames++;
		return;
	}

	if(sync_frames) {
		fprintf(stderr, "SuperframeFilter: Superframe sync succeeded after %d frame(s)\n", sync_frames);
		sync_frames = 0;
	}


	// check announced format
	if(!sf_format_set || sf_format_raw != sf[2]) {
		sf_format_raw = sf[2];
		sf_format_set = true;

		ProcessFormat();
	}

	// decode frames
	for(int i = 0; i < num_aus; i++) {
		uint8_t *au_data = sf + au_start[i];
		size_t au_len = au_start[i+1] - au_start[i];

		uint16_t au_crc_stored = au_data[au_len-2] << 8 | au_data[au_len-1];
		uint16_t au_crc_calced = CalcCRC::CalcCRC_CRC16_CCITT.Calc(au_data, au_len - 2);
		if(au_crc_stored != au_crc_calced) {
			fprintf(stderr, "\x1B[31m" "(AU #%d)" "\x1B[0m" " ", i);
			continue;
		}

		au_len -= 2;
		aac_dec->DecodeFrame(au_data, au_len);
		CheckForPAD(au_data, au_len);
	}

	// ensure getting a complete new Superframe
	frame_count = 0;
}


void SuperframeFilter::CheckForPAD(const uint8_t *data, size_t len) {
	bool present = false;

	// check for PAD (embedded into Data Stream Element)
	if(len >= 3 && (data[0] >> 5) == 4) {
		size_t pad_start = 2;
		size_t pad_len = data[1];
		if(pad_len == 255) {
			pad_len += data[2];
			pad_start++;
		}

		if(pad_len >= 2 && len >= pad_start + pad_len) {
			observer->ProcessPAD(data + pad_start, pad_len - FPAD_LEN, data + pad_start + pad_len - FPAD_LEN);
			present = true;
		}
	}

	if(!present) {
		// required to reset internal state of PAD parser (in case of omitted CI list)
		uint8_t zero_fpad[FPAD_LEN] = {0x00};
		observer->ProcessPAD(NULL, 0, zero_fpad);
	}
}


bool SuperframeFilter::CheckSync() {
	// abort, if au_start is kind of zero (prevent sync on complete zero array)
	if(sf[3] == 0x00 && sf[4] == 0x00)
		return false;

	// TODO: use fire code for error correction

	// try to sync on fire code
	uint16_t crc_stored = sf[0] << 8 | sf[1];
	uint16_t crc_calced = CalcCRC::CalcCRC_FIRE_CODE.Calc(sf + 2, 9);
	if(crc_stored != crc_calced)
		return false;


	// handle format
	sf_format.dac_rate             = sf[2] & 0x40;
	sf_format.sbr_flag             = sf[2] & 0x20;
	sf_format.aac_channel_mode     = sf[2] & 0x10;
	sf_format.ps_flag              = sf[2] & 0x08;
	sf_format.mpeg_surround_config = sf[2] & 0x07;


	// determine number/start of AUs
	num_aus = sf_format.dac_rate ? (sf_format.sbr_flag ? 3 : 6) : (sf_format.sbr_flag ? 2 : 4);

	au_start[0] = sf_format.dac_rate ? (sf_format.sbr_flag ? 6 : 11) : (sf_format.sbr_flag ? 5 : 8);
	au_start[num_aus] = sf_len / 120 * 110;	// pseudo-next AU (w/o RS coding)

	au_start[1] = sf[3] << 4 | sf[4] >> 4;
	if(num_aus >= 3)
		au_start[2] = (sf[4] & 0x0F) << 8 | sf[5];
	if(num_aus >= 4)
		au_start[3] = sf[6] << 4 | sf[7] >> 4;
	if(num_aus == 6) {
		au_start[4] = (sf[7] & 0x0F) << 8 | sf[8];
		au_start[5] = sf[9] << 4 | sf[10] >> 4;
	}

	// simple plausi check for correct order of start offsets
	for(int i = 0; i < num_aus; i++)
		if(au_start[i] >= au_start[i+1])
			return false;

	return true;
}


void SuperframeFilter::ProcessFormat() {
	// output format
	const char *mode;
	switch(sf_format.mpeg_surround_config) {
	case 0:
		mode = (sf_format.aac_channel_mode || sf_format.ps_flag) ? "Stereo" : "Mono";
		break;
	case 1:
		mode = "Surround 5.1";
		break;
	case 2:
		mode = "Surround 7.1";
		break;
	default:
		mode = "Surround (unknown)";
		break;
	}

	int bitrate = sf_len / 120 * 8;

	std::stringstream ss;
	ss << (sf_format.sbr_flag ? (sf_format.ps_flag ? "HE-AAC v2" : "HE-AAC") : "AAC-LC") << ", ";
	ss << (sf_format.dac_rate ? 48 : 32) << " kHz ";
	ss << mode << " ";
	ss << "@ " << bitrate << " kBit/s";
	observer->FormatChange(ss.str());

	if(aac_dec)
		delete aac_dec;
#ifdef DABLIN_AAC_FAAD2
	aac_dec = new AACDecoderFAAD2(observer, sf_format);
#endif
#ifdef DABLIN_AAC_FDKAAC
	aac_dec = new AACDecoderFDKAAC(observer, sf_format);
#endif
}


// --- RSDecoder -----------------------------------------------------------------
RSDecoder::RSDecoder() {
	rs_handle = init_rs_char(8, 0x11D, 0, 1, 10, 135);
	if(!rs_handle)
		throw std::runtime_error("RSDecoder: error while init_rs_char");
}

RSDecoder::~RSDecoder() {
	free_rs_char(rs_handle);
}

void RSDecoder::DecodeSuperframe(uint8_t *sf, size_t sf_len) {
//	// insert errors for test
//	sf_raw[0] ^= 0xFF;
//	sf_raw[10] ^= 0xFF;
//	sf_raw[20] ^= 0xFF;

	int subch_index = sf_len / 120;
	int total_corr_count = 0;
	bool uncorr_errors = false;

	// process all RS packets
	for(int i = 0; i < subch_index; i++) {
		for(int pos = 0; pos < 120; pos++)
			rs_packet[pos] = sf[pos * subch_index + i];

		// detect errors
		int corr_count = decode_rs_char(rs_handle, rs_packet, corr_pos, 0);
		if(corr_count == -1)
			uncorr_errors = true;
		else
			total_corr_count += corr_count;

		// correct errors
		for(int j = 0; j < corr_count; j++) {

			int pos = corr_pos[j] - 135;
			if(pos < 0)
				continue;

//			fprintf(stderr, "j: %d, pos: %d, sf-index: %d\n", j, pos, pos * subch_index + i);
			sf[pos * subch_index + i] = rs_packet[pos];
		}
	}

	// output statistics if errors present (using ANSI coloring)
	if(total_corr_count || uncorr_errors)
		fprintf(stderr, "\x1B[36m" "(%d%s)" "\x1B[0m" " ", total_corr_count, uncorr_errors ? "+" : "");
}



// --- AACDecoder -----------------------------------------------------------------
AACDecoder::AACDecoder(std::string decoder_name, SubchannelSinkObserver* observer, SuperframeFormat sf_format) {
	fprintf(stderr, "AACDecoder: using decoder '%s'\n", decoder_name.c_str());

	this->observer = observer;

	/* AudioSpecificConfig structure (the only way to select 960 transform here!)
	 *
	 *  00010 = AudioObjectType 2 (AAC LC)
	 *  xxxx  = (core) sample rate index
	 *  xxxx  = (core) channel config
	 *  100   = GASpecificConfig with 960 transform
	 *
	 * SBR: implicit signaling sufficient - libfaad2 automatically assumes SBR on sample rates <= 24 kHz
	 * => explicit signaling works, too, but is not necessary here
	 *
	 * PS:  implicit signaling sufficient - libfaad2 therefore always uses stereo output (if PS support was enabled)
	 * => explicit signaling not possible, as libfaad2 does not support AudioObjectType 29 (PS)
	 */

	int core_sr_index = sf_format.dac_rate ? (sf_format.sbr_flag ? 6 : 3) : (sf_format.sbr_flag ? 8 : 5);	// 24/48/16/32 kHz
	int core_ch_config = GetAACChannelConfiguration(sf_format);

	asc[0] = 0b00010 << 3 | core_sr_index >> 1;
	asc[1] = (core_sr_index & 0x01) << 7 | core_ch_config << 3 | 0b100;
}

int AACDecoder::GetAACChannelConfiguration(SuperframeFormat sf_format) {
	switch(sf_format.mpeg_surround_config) {
	case 0:		// no surround
	default:
		return sf_format.aac_channel_mode ? 2 : 1;
	case 1:		// 5.1
		return 6;
	case 2:		// 7.1
		return 7;
	}
}


#ifdef DABLIN_AAC_FAAD2
// --- AACDecoderFAAD2 -----------------------------------------------------------------
AACDecoderFAAD2::AACDecoderFAAD2(SubchannelSinkObserver* observer, SuperframeFormat sf_format) : AACDecoder("FAAD2", observer, sf_format) {
	// ensure features
	unsigned long cap = NeAACDecGetCapabilities();
	if(!(cap & LC_DEC_CAP))
		throw std::runtime_error("AACDecoderFAAD2: no LC decoding support!");

	handle = NeAACDecOpen();
	if(!handle)
		throw std::runtime_error("AACDecoderFAAD2: error while NeAACDecOpen");

	// set general config
	NeAACDecConfigurationPtr config = NeAACDecGetCurrentConfiguration(handle);
	if(!config)
		throw std::runtime_error("AACDecoderFAAD2: error while NeAACDecGetCurrentConfiguration");

	config->outputFormat = FAAD_FMT_FLOAT;
	config->downMatrix = 1;
	config->dontUpSampleImplicitSBR = 0;

	if(NeAACDecSetConfiguration(handle, config) != 1)
		throw std::runtime_error("AACDecoderFAAD2: error while NeAACDecSetConfiguration");

	// init decoder
	unsigned long output_sr;
	unsigned char output_ch;
	long int init_result = NeAACDecInit2(handle, asc, sizeof(asc), &output_sr, &output_ch);
	if(init_result != 0) {
		std::stringstream ss;
		ss << "AACDecoderFAAD2: error while NeAACDecInit2: " << NeAACDecGetErrorMessage(-init_result);
		throw std::runtime_error(ss.str());
	}

	observer->StartAudio(output_sr, output_ch, true);
}

AACDecoderFAAD2::~AACDecoderFAAD2() {
	NeAACDecClose(handle);
}

void AACDecoderFAAD2::DecodeFrame(uint8_t *data, size_t len) {
	// decode audio
	uint8_t* output_frame = (uint8_t*) NeAACDecDecode(handle, &dec_frameinfo, data, len);
	if(dec_frameinfo.bytesconsumed != len)
		throw std::runtime_error("AACDecoderFAAD2: NeAACDecDecode did not consume all bytes");

	size_t output_frame_len = dec_frameinfo.samples * 4;
	if(dec_frameinfo.error)
		fprintf(stderr, "AACDecoderFAAD2: error while NeAACDecDecode: bytes %zu, samplerate %ld, sbr %d, ps %d => %d = %s\n", output_frame_len, dec_frameinfo.samplerate, dec_frameinfo.sbr, dec_frameinfo.ps, dec_frameinfo.error, NeAACDecGetErrorMessage(dec_frameinfo.error));

	observer->PutAudio(output_frame, output_frame_len);
}
#endif



#ifdef DABLIN_AAC_FDKAAC
// --- AACDecoderFDKAAC -----------------------------------------------------------------
AACDecoderFDKAAC::AACDecoderFDKAAC(SubchannelSinkObserver* observer, SuperframeFormat sf_format) : AACDecoder("FDK-AAC", observer, sf_format) {
	handle = aacDecoder_Open(TT_MP4_RAW, 1);
	if(!handle)
		throw std::runtime_error("AACDecoderFDKAAC: error while aacDecoder_Open");

//	// down/upmix to stereo
//	AAC_DECODER_ERROR init_result = aacDecoder_SetParam(handle, AAC_PCM_OUTPUT_CHANNELS, 2);
//	if(init_result != AAC_DEC_OK) {
//		std::stringstream ss;
//		ss << "AACDecoderFDKAAC: error while setting parameter AAC_PCM_OUTPUT_CHANNELS: " << init_result;
//		throw std::runtime_error(ss.str());
//	}


	uint8_t* asc_array[1] {asc};
	const unsigned int asc_sizeof_array[1] {sizeof(asc)};
	AAC_DECODER_ERROR init_result = aacDecoder_ConfigRaw(handle, asc_array, asc_sizeof_array);
	if(init_result != AAC_DEC_OK) {
		std::stringstream ss;
		ss << "AACDecoderFDKAAC: error while aacDecoder_ConfigRaw: " << init_result;
		throw std::runtime_error(ss.str());
	}

	int channels = sf_format.aac_channel_mode || sf_format.ps_flag ? 2 : 1;
	output_frame_len = 960 * 2 * channels * (sf_format.sbr_flag ? 2 : 1);
	output_frame = new uint8_t[output_frame_len];

	observer->StartAudio(sf_format.dac_rate ? 48000 : 32000, channels, false);
}

AACDecoderFDKAAC::~AACDecoderFDKAAC() {
	aacDecoder_Close(handle);
	delete[] output_frame;
}

void AACDecoderFDKAAC::DecodeFrame(uint8_t *data, size_t len) {
	uint8_t* input_buffer[1] {data};
	const unsigned int input_buffer_size[1] {(unsigned int) len};
	unsigned int bytes_valid = len;

	// fill internal input buffer
	AAC_DECODER_ERROR result = aacDecoder_Fill(handle, input_buffer, input_buffer_size, &bytes_valid);
	if(result != AAC_DEC_OK) {
		std::stringstream ss;
		ss << "AACDecoderFDKAAC: error while aacDecoder_Fill: " << result;
		throw std::runtime_error(ss.str());
	}
	if(bytes_valid)
		throw std::runtime_error("AACDecoderFDKAAC: aacDecoder_Fill did not consume all bytes");


	// decode audio
	result = aacDecoder_DecodeFrame(handle, (short int*) output_frame, output_frame_len, 0);
	if(result != AAC_DEC_OK) {
		std::stringstream ss;
		ss << "AACDecoderFDKAAC: error while aacDecoder_DecodeFrame: " << result;
		throw std::runtime_error(ss.str());
	}

	observer->PutAudio(output_frame, output_frame_len);
}
#endif
