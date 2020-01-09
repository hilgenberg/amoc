#pragma once

decoder *mp3_plugin();
decoder *wav_plugin();
decoder *aac_plugin();
decoder *mod_plugin();
decoder *sid_plugin();
decoder *muse_plugin();
decoder *flac_plugin();
decoder *speex_plugin();
decoder *ffmpeg_plugin();
decoder *timidity_plugin();
decoder *sndfile_plugin();
decoder *vorbis_plugin();

bool vorbis_has_tremor();

#define ALL_INPUTS \
	H(mp3);\
	H(wav);\
	H(aac);\
	H(mod);\
	H(sid);\
	H(muse);\
	H(flac);\
	H(speex);\
	H(ffmpeg);\
	H(timidity);\
	H(sndfile);\
	H(vorbis);

