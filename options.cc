#include "options.h"
#include <pwd.h>
#include <regex.h>

static void parse(bool &o, const char *v)
{
	if      (!strcasecmp (v, "yes")) o = true;
	else if (!strcasecmp (v, "no")) o = false;
	else throw std::runtime_error(format("Invalid boolean: '%s'", v));
}
static void parse(int &o, const char *v)
{
	if (!is_int(v, o)) throw std::runtime_error(format("Invalid integer: '%s'", v));
}
static void parse(std::string &o, const char *v)
{
	o = v;
}
static void eparse(int &o, const char *v, ...)
{
	va_list ap; va_start(ap, v);
	for (int i = 0; true; ++i)
	{
		const char *w = va_arg(ap, const char *);
		if (!w) break;
		if (!strcasecmp (v, w))
		{
			va_end(ap);
			o = i;
			return;
		}
	}
	va_end(ap);
	throw std::runtime_error(format("Invalid value: '%s'", v));
}

static void build_rating_strings(const char *s0 = " ", const char *s1 = "*")
{
	const char** &S = options::rating_strings;
	const size_t l0 = strlen (s0), l1 = strlen (s1);
	free(S); S = (const char**)malloc(6 * sizeof(char*) + (l0+l1)*15 + 6);
	char *s = (char*)(S+6);
	for (int i = 0; i <= 5; ++i)
	{
		int j = 0; S[i] = s;
		for (; j < i; ++j) { memcpy (s, s1, l1); s += l1; }
		for (; j < 5; ++j) { memcpy (s, s0, l0); s += l0; }
		*s++ = 0;
	}
}

namespace options {

str config_file_path(const char *file) { return add_path(ConfigDir, file); }
str run_file_path(const char *file) { return add_path(RunDir, file); }

// read "Key = Value" pairs from file, adding them to items
// returns true if the file could be opened
static bool read_config_file(const str &path, std::map<str,str> &items)
{
	FILE *file = fopen(path.c_str(), "r");
	if (!file)
	{
		char *err = STRERROR_FN (errno);
		logit ("Can't open file \"%s\": %s", path.c_str(), err);
		free (err);

		return false;
	}

	int line = 0;
	while (true)
	{
		int c = fgetc(file); if (c == EOF) break;
		++line;
		
		// skip initial whitespace and comment lines
		while (isspace(c)) c = fgetc(file); if (c == EOF) break;
		if (c == '#')
		{
			do c = fgetc(file); while (c != EOF && c != '\n');
			continue;
		}

		// read this option's key
		std::string key, value;
		while (!isspace(c) && c != EOF && c != '=') { key += c; c = fgetc(file); }

		// skip space, equal sign, space
		while (isspace(c)) c = fgetc(file); if (c == EOF) break;
		if (c != '=')
		{
			logit("Syntax error on line %d", line);
			do c = fgetc(file); while (c != EOF && c != '\n');
			continue;
		}
		c = fgetc(file);
		while (isspace(c) && c != EOF) c = fgetc(file);

		// read this option's value
		if (c == '\'' || c == '"')
		{
			int q = c; c = fgetc(file);
			while (c != EOF && c != '\n' && c != q)
			{
				value += c;
				c = fgetc(file);
			}
			if (c != q)
			{
				value.insert(0, 1, (char)q);
			}
			else
			{
				while (c != '\n' && c != EOF) c = fgetc(file);
			}
		}
		else
		{
			while (c != '#' && c != '\n' && c != EOF) { value += c; c = fgetc(file); }
			while (!value.empty() && isspace(value.back())) value.pop_back();
			if (c == '#') while (c != '\n' && c != EOF) c = fgetc(file);
		}

		// finally add it to the map, replacing any prior value
		items[key] = value;
	}
	fclose (file);
	return true;
}


static bool write_config_file(const str &path, const std::map<str,str> &items, const char *header=NULL)
{
	FILE *file = fopen(path.c_str(), "w");
	if (!file)
	{
		char *err = STRERROR_FN (errno);
		logit ("Can't write file \"%s\": %s", path.c_str(), err);
		free (err);
		return false;
	}

	if (header) fprintf(file, "%s\n", header);

	for (auto &it : items)
		fprintf(file, "%s=%s\n", it.first.c_str(), it.second.c_str());
	fclose (file);
	return true;
}

void load(Component who)
{
	const char *home = getenv ("HOME");
	if (!home) {
		errno = 0;
		struct passwd *passwd = getpwuid (geteuid ());
		if (passwd)
			home = passwd->pw_dir;
		else if (errno != 0) {
			char *err = xstrerror (errno);
			logit ("getpwuid(%d): %s", geteuid (), err);
			free (err);
		}
	}
	if (!home) fatal("Can't get home directory!");
	Home = home; normalize_path(Home);

	str cfg1 = add_path(Home, ".config/amoc"), cfg2 = add_path(Home, ".amoc");
	ConfigDir = is_dir(cfg1) ? cfg1 : is_dir(cfg2) ? cfg2 : 
		is_dir(add_path(Home, ".config")) ? cfg1 : cfg2;

	std::map<str,str> items;
	read_config_file(add_path(ConfigDir, "config"), items);

	switch (who)
	{
		case CLI:    break;
		case GUI:    read_config_file(add_path(ConfigDir, "config_gui"), items); break;
		case SERVER: read_config_file(add_path(ConfigDir, "config_srv"), items); break;
	}

	std::string RatingSpace(" "), RatingStar("*");
	RunDir = ConfigDir;

	int hsplit_plist = 1, hsplit_dirlist = 1, vsplit_plist = 1, vsplit_dirlist = 1;

	#define OPT(x) if (items.count(#x)) ::parse(x, items[#x].c_str())
	#define EOPT(x,...) if (items.count(#x)) { int tmp; ::eparse(tmp, items[#x].c_str(), __VA_ARGS__, NULL); x = (decltype(x))tmp; }

	EOPT(layout, "hsplit", "vsplit", "single");
	OPT(hsplit_plist); OPT(hsplit_dirlist);
	OPT(hsplit_plist); OPT(hsplit_dirlist);

	OPT(LastDir);
	OPT(RatingSpace);
	OPT(RatingStar);
	OPT(ReadTags);
	OPT(MusicDir);
	OPT(StartInMusicDir);
	OPT(Repeat);
	OPT(Shuffle);
	OPT(AutoNext);
	OPT(ASCIILines); OPT(HideBorder);
	OPT(InputBuffer);
	OPT(OutputBuffer);
	OPT(Prebuffering);
	OPT(HTTPProxy);
	EOPT(SoundDriver, "SNDIO", "JACK", "ALSA", "OSS", "NULL");
	OPT(JackClientName);
	OPT(JackStartServer);
	OPT(JackOutLeft);
	OPT(JackOutRight);
	OPT(OSSDevice);
	OPT(OSSMixerDevice);
	OPT(OSSMixerChannel1);
	OPT(OSSMixerChannel2);
	OPT(ALSADevice);
	OPT(ALSAMixer1);
	OPT(ALSAMixer2);
	OPT(ALSAStutterDefeat);
	OPT(Softmixer_SaveState);
	OPT(Equalizer_SaveState);
	OPT(ShowMixer);
	OPT(ShowHiddenFiles);
	OPT(HideFileExtension);
	OPT(RunDir);
	OPT(UseMMap);
	OPT(UseMimeMagic);
	OPT(UseRCCForFilesystem);
	OPT(FileNamesIconv);
	OPT(TiMidity_Config);
	OPT(TERM);
	OPT(RatingFile);
	OPT(TimeBarLine);
	OPT(TimeBarSpace);
	OPT(SeekTime);
	OPT(SilentSeekTime);
	EOPT(ResampleMethod, "SincBestQuality", "SincMediumQuality", "SincFastest", "ZeroOrderHold", "Linear");
	OPT(ForceSampleRate);
	OPT(Allow24bitOutput);
	OPT(UseRealtimePriority);
	OPT(PlaylistFullPaths);
	OPT(MessageLingerTime);

	build_rating_strings(RatingSpace.c_str(), RatingStar.c_str());
	if (Prebuffering > InputBuffer) InputBuffer = Prebuffering;

	if (RunDir.empty()) RunDir = ConfigDir;
	normalize_path(RunDir);
	if (!MusicDir.empty()) normalize_path(MusicDir);
	SocketPath = add_path(RunDir, "socket");
	if (!LastDir.empty()) normalize_path(LastDir);

	#define UNIX_PATH_MAX	108
	if (SocketPath.length() > UNIX_PATH_MAX) fatal ("Can't create socket name!");

	hsplit.first  = std::max(0, hsplit_plist);
	hsplit.second = std::max(0, hsplit_dirlist);
	vsplit.first  = std::max(0, vsplit_plist);
	vsplit.second = std::max(0, vsplit_dirlist);
}

void save(Component who)
{
	if (who == CLI) return;

	std::map<str,str> items;
	const char *header = "# This file is auto-generated and overrides the regular config file";

	if (who == GUI)
	{
		items["LastDir"]        = LastDir;
		items["hsplit_plist"]   = format("%d", hsplit.first);
		items["hsplit_dirlist"] = format("%d", hsplit.second);
		items["vsplit_plist"]   = format("%d", vsplit.first);
		items["vsplit_dirlist"] = format("%d", vsplit.second);
		items["layout"] = (layout == HSPLIT ? "hsplit" : layout == VSPLIT ? "vsplit" : "single");
		write_config_file(add_path(ConfigDir, "config_gui"), items, header);
	}
	else if (who == SERVER)
	{
		items["Shuffle"]  = Shuffle  ? "yes" : "no";
		items["Repeat"]   = Repeat   ? "yes" : "no";
		items["AutoNext"] = AutoNext ? "yes" : "no";
		write_config_file(add_path(ConfigDir, "config_srv"), items, header);
	}
}

str Home, ConfigDir, MusicDir, RunDir, SocketPath;

const char **rating_strings = NULL;

Ratio hsplit = std::make_pair(1,1), vsplit = std::make_pair(1,1);
Layout layout = HSPLIT;

bool ReadTags = true;
bool StartInMusicDir = false;
str  LastDir = "";
bool Repeat = false;
bool Shuffle = false;
bool AutoNext = true;
bool ASCIILines = false, HideBorder = false;
//str  ("FormatString", "%(n:%n :)%(a:%a - :)%(t:%t:)%(A: \\(%A\\):)", CHECK_NONE);
int InputBuffer = 512;
int OutputBuffer = 512;
int Prebuffering = 64;
str HTTPProxy = "";

SoundDriver_t SoundDriver = SoundDriver_t::AUTO;

str  JackClientName = "moc";
bool JackStartServer = false;
str  JackOutLeft  = "system:playback_1";
str  JackOutRight = "system:playback_2";

str OSSDevice = "/dev/dsp";
str OSSMixerDevice ="/dev/mixer";
str OSSMixerChannel1 = "pcm";
str OSSMixerChannel2 = "master";

str  ALSADevice = "default";
str  ALSAMixer1 = "PCM";
str  ALSAMixer2 = "Master";
bool ALSAStutterDefeat = false;

bool Softmixer_SaveState = true;
bool Equalizer_SaveState = true;
bool ShowMixer = true;

bool ShowHiddenFiles = false;
bool HideFileExtension = false;

bool UseMMap = false;
bool UseMimeMagic = false;
bool UseRCCForFilesystem = false;
bool FileNamesIconv = false;
str  TiMidity_Config = "no";
str  TERM = "";

str  RatingFile = "ratings";

str  TimeBarLine =  "";
str  TimeBarSpace = "";

int  SeekTime = 1;
int  SilentSeekTime = 5;

ResampleMethod_t ResampleMethod = ResampleMethod_t::Linear;

int  ForceSampleRate = 0;
bool Allow24bitOutput = false;
bool UseRealtimePriority = false;

bool PlaylistFullPaths = true;

int  MessageLingerTime = 2;

} // end of namespace
