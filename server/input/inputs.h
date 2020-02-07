#pragma once

Decoder *mp3_plugin();
Decoder *wav_plugin();
Decoder *aac_plugin();
Decoder *mod_plugin();
Decoder *muse_plugin();
Decoder *flac_plugin();
Decoder *speex_plugin();
Decoder *ffmpeg_plugin();
Decoder *timidity_plugin();
Decoder *sndfile_plugin();
Decoder *vorbis_plugin();

#define ALL_INPUTS \
	H(mp3);\
	H(wav);\
	H(aac);\
	H(mod);\
	H(muse);\
	H(flac);\
	H(speex);\
	H(ffmpeg);\
	H(timidity);\
	H(sndfile);\
	H(vorbis);

