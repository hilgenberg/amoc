#include <sys/stat.h>
#include <dirent.h>
#include <stack>
#include <fcntl.h>
#include <sys/file.h>

#include "playlist.h"
#include "client/Util/Tags.h"
#include "client/client.h" // user_wants_interrupt()
#include "server/input/decoder.h"

file_type plist_item::ftype (const str &file)
{
	struct stat file_stat;
	const char *f = file.c_str();

	if (stat(f, &file_stat) == -1) return F_OTHER;
	if (S_ISDIR(file_stat.st_mode)) return F_DIR;
	if (is_sound_file(f)) return F_SOUND;
	if (is_plist_file(f)) return F_PLAYLIST;
	return F_OTHER;
}

bool plist_item::can_tag() const
{
	if (type != F_SOUND) return false;
	auto *df = get_decoder (path.c_str());
	return df && df->can_write_tags(path);
}

plist::~plist()
{
	if (tags) for (auto &it : items) tags->release(*it);
}
void plist::swap(plist &other)
{
	if (tags && !other.tags)
	{
		for (auto &it : items) tags->release(*it);
		for (auto &it : other.items) { delete it->tags; it->tags = NULL; }
	}
	else if (!tags && other.tags)
	{
		for (auto &it : other.items) other.tags->release(*it);
		for (auto &it : items) { delete it->tags; it->tags = NULL; }
	}
	else
	{
		assert(tags == other.tags); // we only ever use one Tags instance
	}
	std::swap(items, other.items);
	std::swap(is_dir, other.is_dir);
}

void plist::clear()
{
	if (tags) for (auto &it : items) tags->release(*it);
	items.clear();
}
void plist::remove(int i, int n)
{
	if (i < 0 || n <= 0 || (size_t)(i+n) > items.size()) return;
	if (tags) for (int j = i, k = i+n; j < k; ++j) tags->release(*items[j]);
	items.erase(items.begin() + i, items.begin() + i + n);
}
void plist::remove(const std::set<int> &idx)
{
	int j = 0;
	for (int i = 0, n = (int)items.size(); i < n; ++i)
	{
		if (idx.count(i)) continue;
		std::swap(items[i], items[j++]);
	}
	items.resize(j);
}
int plist::remove(const std::set<str> &files, int i0)
{
	int j = 0;
	for (int i = 0, n = (int)items.size(); i < n; ++i)
	{
		if (files.count(items[i]->path)) continue;
		std::swap(items[i], items[j++]);
		if (i0 > i) --i0;
	}
	items.resize(j);
	return i0;
}
void plist::replace(const std::map<str, str> &mod)
{
	for (int i = 0, n = (int)items.size(); i < n; ++i)
	{
		auto it = mod.find(items[i]->path);
		if (it == mod.end()) continue;
		items[i]->path = it->second;
	}
}

void plist::move(int i, int j)
{
	int n = (int)items.size();
	if (i == j || i < 0 || j < 0 || i >= n || j >= n) return;
	while (i < j) { std::swap(items[i], items[i+1]); ++i; }
	while (i > j) { std::swap(items[i], items[i-1]); --i; }
}

plist & plist::operator+= (const plist &other)
{
	for (auto &i : other.items)
		items.emplace_back(new plist_item(*i));
	return *this;
}
plist & plist::operator+= (const plist_item &i)
{
	items.emplace_back(new plist_item(i));
	return *this;
}
plist & plist::operator+= (plist &&other)
{
	for (auto &i : other.items)
		items.emplace_back(std::move(i));
	return *this;
}

void plist::insert(const plist &other, int pos)
{
	if (pos < 0 || pos >= items.size())
	{
		*this += other;
		return;
	}

	items.reserve(size() + other.size());
	auto j = items.begin() + pos;
	for (auto &i : other.items)
		items.emplace(j++, new plist_item(*i));
}
void plist::insert(plist &&other, int pos)
{
	if (pos < 0 || pos >= items.size())
	{
		*this += other;
		return;
	}

 	items.insert(items.begin() + pos,
		std::make_move_iterator(std::begin(other.items)),
		std::make_move_iterator(std::end(other.items)));
}
void plist::insert(const str &f, int pos)
{
	if (pos < 0 || pos >= items.size())
	{
		*this += f;
		return;
	}

	auto j = items.begin() + pos;
	items.emplace(j++, new plist_item(f));
}

void plist::shuffle ()
{
	for (int i = 0, n = (int)items.size(); i < n; ++i)
	{
		int j = random_int(i, n);
		std::swap(items[i], items[j]);
	}
}

bool operator< (const plist_item &a, const plist_item &b)
{
	if (a.type != b.type)
	{
		// dirs first
		if (a.type == F_DIR) return true;
		if (b.type == F_DIR) return false;
		// then playlists
		if (a.type == F_PLAYLIST) return true;
		if (b.type == F_PLAYLIST) return false;
		// then all other files
	}

	if (a.type == F_DIR)
	{
		// '../' first
		if (a.path == "../") return true;
		if (b.path == "../") return false;
	}

	const char *sa = a.path.c_str(), *sb = b.path.c_str();
	if (isdigit(*sa) && isdigit(*sb))
	{
		while (*sa == '0') ++sa;
		while (*sb == '0') ++sb;
		// if both start with 0, 00, ... -- sort lex
		if (isdigit(*sa) || isdigit(*sb))
		{
			int na = 0, nb = 0;
			while (isdigit(sa[na])) ++na;
			while (isdigit(sb[nb])) ++nb;
			if (na < nb) return true; // "9" < "123"
			if (na > nb) return false;
			// same length -- easy
			for (; na; ++sa, ++sb, --na) {
				if (*sa < *sb) return true;
				if (*sa > *sb) return false;
			}
			return strcoll(sa, sb) < 0;
		}
	}
	return strcoll(a.path.c_str(), b.path.c_str()) < 0;
}

bool plist::load_directory(const str &directory_, bool include_updir)
{
	str directory = normalized_path(directory_);
	
	DIR *dir = opendir(directory.empty() ? "." : directory.c_str());
	if (!dir) {
		error_errno ("Can't read directory", errno);
		return false;
	}

	items.clear();
	is_dir = true;

	const bool root = (directory == "/");
	const char *prefix = (root ? "" : directory.c_str());
	dirent *entry;
	while ((entry = readdir(dir)))
	{
		if (user_wants_interrupt()) {
			error ("Interrupted! Not all files read!");
			break;
		}

		if (!strcmp(entry->d_name, ".")) continue;
		bool up = !strcmp(entry->d_name, "..");
		if (up && (root || !include_updir)) continue;
		if (!up && !options::ShowHiddenFiles && *entry->d_name == '.') continue;

		str p = format("%s/%s", prefix, entry->d_name);
		if (up) normalize_path(p);
		items.emplace_back(new plist_item(p));
	}

	closedir (dir);

	sort();
	return true;
}

bool plist::add_directory (const str &directory, bool recursive)
{
	is_dir = false;

	std::stack<str> todo;
	std::set<ino_t> done;
	todo.push(directory);
	plist added;
	while (!todo.empty())
	{
		if (user_wants_interrupt()) {
			error ("Interrupted! Not all files read!");
			break;
		}

		str d = todo.top(); todo.pop();
		const char *dir = d.c_str();
		
		struct stat st;
		if (stat (dir, &st)) {
			char *err = xstrerror (errno);
			error ("Can't stat \"%s\" (Error: \"%s\")", dir, err);
			free (err);
			continue;
		}
		if (done.count(st.st_ino))
		{
			logit ("Detected symlink loop on %s", dir);
			continue;
		}

		plist p; p.load_directory(d, false);
		for (int i = 0, n = (int)p.items.size(); i < n; ++i)
		{
			auto &it = p.items[i];
			switch (it->type)
			{
				case F_DIR:
				{
					if (recursive) todo.push(it->path);
					// fallthrough
				}
				case F_PLAYLIST:
				case F_OTHER:
					p.items.erase(p.items.begin() + i);
					--i; --n;
					break;
				default: break;
			}
		}

		p += std::move(added);
		added.swap(p);
	}
	*this += std::move(added);
	return true;
}

bool plist::load_m3u (const str &fname)
{
	struct flock read_lock = {.l_type = F_RDLCK, .l_whence = SEEK_SET, .l_start=0, .l_len=0, .l_pid=-1};

	FILE *file = fopen (fname.c_str(), "r");
	if (!file) {
		error_errno ("Can't open playlist file", errno);
		return false;
	}
	/* Lock gets released by fclose(). */
	if (fcntl (fileno (file), F_SETLKW, &read_lock) == -1)
		log_errno ("Can't lock the playlist file", errno);
	
	items.clear();
	is_dir = false;

	str base(fname);
	size_t i = base.rfind('/');
	if (i != str::npos) base.erase(i+1); else base = "";

	for (char *line = NULL; (line = read_line(file)) != NULL; free(line))
	{
		char *s = line;
		while (*s && isblank(*s)) ++s;
		if (*s == '#') continue;

		// remove trailing whitespace
		for (char *c = line + strlen(line) - 1; c >= line && isblank(*c); --c) *c = 0;

		if (!*line) continue;

		str p = (line[0] == '/' ? str(line) : base + line);
		normalize_path(p);
		items.emplace_back(new plist_item(p));
	}

	fclose (file);
	return true;
}

/* Save the playlist into the file in m3u format. */
bool plist::save (const str &fname) const
{
	struct flock write_lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start=0, .l_len=0, .l_pid=-1};

	debug ("Saving playlist to '%s'", fname.c_str());

	FILE *file = fopen (fname.c_str(), "w");
	if (!file) {
		error_errno ("Can't save playlist", errno);
		return false;
	}
	/* Lock gets released by fclose(). */
	if (fcntl (fileno (file), F_SETLKW, &write_lock) == -1)
		log_errno ("Can't lock the playlist file", errno);

	if (fprintf (file, "#EXTM3U\r\n") < 0) {
		error_errno ("Error writing playlist", errno);
		fclose (file);
		return false;
	}

	for (auto &i : items)
	{
		int ret = 0;
		if (i->tags && !i->tags->title.empty())
			ret = fprintf (file, "#EXTINF:%d,%s\r\n", i->tags->time, i->tags->title.c_str());
		if (ret >= 0) ret = fprintf (file, "%s\r\n", i->path.c_str());
		if (ret < 0) {
			error_errno ("Error writing playlist", errno);
			fclose (file);
			return false;
		}
	}

	if (fclose (file) != 0)
	{
		error_errno ("Error writing playlist", errno);
		return false;
	}
	return true;
}
