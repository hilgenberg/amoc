#pragma once
#include "../audio.h"
#include "../../playlist.h"
#include "io.h"

enum decoder_error_type
{
	ERROR_OK, /*!< There was no error. */
	ERROR_STREAM, /*!< Recoverable error in the stream. */
	ERROR_FATAL /*!< Fatal error in the stream - further decoding can't be performed. */
};
struct decoder_error
{
	decoder_error() : type(ERROR_OK) {}
	void clear() { type = ERROR_OK; desc.clear(); }
	void warn(const char *format, ...);
	void fatal(const char *format, ...);
	operator bool() const { return type != ERROR_OK; }

	decoder_error_type type;
	str  desc;
};

class Codec;

class Decoder
{
public:
	virtual ~Decoder() {}

	/* Open the given resource (file).
	 *
	 * \param uri URL to the resource that can be used as the file parameter
	 * and return pointer to io_open().
	 *
	 * \return Codec or NULL on error.
	 */
	virtual Codec* open(const str &uri);

	/* Open the resource for an already opened stream.
	 *
	 * Handle the stream that was already opened, but no data were read.
	 * You must operate on the stream using io_*() functions. This is used
	 * for internet streams, so seeking is not possible. This function is
	 * optional.
	 *
	 * \param stream Opened stream from which the decoder must read.
	 *
	 * \return Codec or NULL on error.
	 */
	virtual Codec* open(io_stream &stream) { return NULL; }

	/* Check if the decoder is able to decode from this stream.
	 *
	 * Used to check if the decoder is able to read from an already opened
	 * stream. This is used to find the proper decoder for an internet
	 * stream when searching by the MIME type failed. The decoder must not
	 * read from this stream (io_read()), but can peek data (io_peek()).
	 * The decoder is expected to peek a few bytes to recognize its format.
	 * Optional.
	 *
	 * \param stream Opened stream.
	 */
	virtual bool can_decode(io_stream &stream) { return false; }

	virtual void read_tags(const str &file, file_tags &tags);
	virtual bool write_tags(const str &file, const tag_changes &tags);
	virtual int  get_duration(const str &file) { return -1; }
	virtual bool can_write_tags(const str &file_name);

	virtual bool matches_ext(const char *ext) const = 0;
	virtual bool matches_mime(const str &mime_type) const { return false; }
};

class Codec
{
public:
	decoder_error error; // must be set in c'tor and decode()

	virtual ~Codec() {}

	/* Decode a piece of input and write it to the buffer. The buffer size
	 * is at least 32KB, but don't make any assumptions that it is always
	 * true. It is preferred that as few bytes as possible be decoded
	 * without loss of performance to minimise delays.
	 *
	 * \param buf Buffer to put data in.
	 * \param buf_len Size of the buffer in bytes.
	 * \param sound_params Parameters of the decoded sound. This must be
	 * always filled.
	 *
	 * \return Number of bytes written or 0 on EOF.
	 */
	virtual int decode(char *buf, int buf_len, sound_params &sound_params) = 0;

	/* Seek to the given position.
	 *
	 * \param sec Where to seek in seconds (never less than zero).
	 *
	 * \return The position that we actually seek to or -1 on error.
	 * -1 is not a fatal error and further decoding will be performed.
	 */
	virtual int seek(int sec) { return -1; }

	/* Get the bitrate of the last decoded piece of sound.
	 *
	 * \return Current bitrate in kbps or -1 if not available.
	 */
	virtual int get_bitrate() const { return -1; }

	/* Get duration of the stream. It is used as a faster alternative
	 * for getting duration than using info() if the file is opened.
	 *
	 * \return Duration in seconds or -1 on error. -1 is not a fatal
	 * error, further decoding will be performed.
	 */
	virtual int get_duration() const { return -1; }

	/** Get current tags for the stream.
	 *
	 * Fill the tags structure with the current tags for the stream. This
	 * is intended for internet streams and used when the source of the
	 * stream doesn't provide tags while broadcasting. This function is
	 * optional.
	 *
	 * \return 1 if the tags were changed from the last call of this
	 * function or 0 if not.
	 */
	virtual bool current_tags(file_tags &tags) { return false; }

	/* Get the bitrate of the whole file.
	 *
	 * \return Average bitrate in kbps or -1 if not available.
	 */
	virtual int get_avg_bitrate() const { return -1; }
};

bool   is_sound_file (const str &file);
Decoder *get_decoder (const str &file);
Decoder *get_decoder_by_content(io_stream &stream);
void decoder_init ();
void decoder_cleanup ();
