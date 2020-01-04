#pragma once

#include <cstring>
#include <sys/types.h>
#include <map>
#include <memory>

#define TAGS_COMMENTS 1
#define TAGS_TIME     2

struct file_tags
{
	file_tags() : track(-1), time(-1), rating(-1), filled(0) {}
	void read_file_tags (const char *file);

	int time; // in seconds or -1 for streams
	str title, artist, album;
	int track, rating;
	std::map<str, str> extra;
	int filled;
};

enum file_type
{
	F_OTHER = 0,
	F_DIR,
	F_SOUND,
	F_URL,
	F_PLAYLIST
};

class plist_item
{
public:
	static file_type ftype(const str &path);

	explicit plist_item(str &&p) : path(p), type(ftype(path)) {}
	explicit plist_item(const str &p) : path(p), type(ftype(path)) {}
	plist_item(const str &p, file_type t) : path(p), type(t) {}
	plist_item(const plist_item &i)
		: path(i.path), type(i.type)
		, tags(i.tags ? new file_tags(*i.tags) : NULL)
	{}

	bool read_file_tags();

	const str path; // absolute path or URL
	file_type type;
	std::unique_ptr<file_tags> tags;
};
bool operator< (const plist_item &a, const plist_item &b);

struct plist
{
public:
	plist() : serial(-1), is_dir(false) {}
	plist(const plist &) = delete;
	plist(plist &&p) : serial(p.serial), is_dir(p.is_dir), cwd(p.cwd) { items.swap(p.items); }

	void clear() { items.clear(); }
	void remove(int i)
	{
		if (i < 0 || (size_t)i >= items.size()) return;
		items.erase(items.begin() + i);
	}

	bool load_directory(const char *directory);
	bool add_directory(const char *directory, bool recursive = true);
	bool load_m3u(const char *path);
	bool save (const char *m3u_path) const;
	plist & operator+= (const plist &other);
	plist & operator+= (plist &&other);
	plist & operator+= (const plist_item &i);
	plist & operator+= (str &&f) { items.emplace_back(new plist_item(f)); return *this; }
	plist & operator+= (const str &f) { items.emplace_back(new plist_item(f)); return *this; }
	plist & operator+= (const char *f) { items.emplace_back(new plist_item(f)); return *this; }

	size_t size() const { return items.size(); }

	int total_time() const
	{
		int sum = 0;
		for (auto &i : items)
		{
			if (!i->tags) continue;
			sum += std::max(0, i->tags->time);
		}
		return sum;
	}
	int find(const str &file) const // TODO: remove this!
	{
		for (int i = 0, n = (int)items.size(); i < n; ++i)
			if (items[i]->path == file) return i;
		return -1;
	}
	int find(const char *file) const { return find(str(file)); }

	void shuffle();
	void sort()
	{
		std::sort(items.begin(), items.end(), 
		[](const std::unique_ptr<plist_item>&a, const std::unique_ptr<plist_item>&b)
		{ return *a < *b; });
	}
	bool move_to_front(const char *item)
	{
		for (auto &i : items)
		{
			if (i->path != item) continue;
			std::swap(i, items[0]);
			return true;
		}
		return false;
	}

	void swap(plist &other)
	{
		std::swap(items, other.items);
		std::swap(serial, other.serial);
		std::swap(is_dir, other.is_dir);
		std::swap(cwd, other.cwd);
	}

	std::vector<std::unique_ptr<plist_item> > items;
	int  serial; /* Optional serial number of this playlist */
	bool is_dir; // otherwise it's a playlist
	str  cwd; // set for directories and on-disk playlists
};

inline void swap(plist &a, plist &b) { a.swap(b); }
