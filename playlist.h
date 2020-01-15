#pragma once
#include <memory>

struct file_tags
{
	file_tags() : track(-1), time(-1), rating(-1) {}
	void read_file_tags (const char *file);

	int time; // in seconds or -1 for streams
	str title, artist, album;
	int track, rating;
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

	explicit plist_item(str &&p) : path(p), type(ftype(path)) { assert(!path.empty() && (type == F_URL || path[0] == '/')); }
	explicit plist_item(const str &p) : path(p), type(ftype(path)) { assert(!path.empty() && (type == F_URL || path[0] == '/')); }
	plist_item(const str &p, file_type t) : path(p), type(t) { assert(!path.empty() && (type == F_URL || path[0] == '/')); }
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
	plist() : is_dir(false) {}
	plist(const plist &) = delete;
	plist(plist &&p) : is_dir(p.is_dir) { items.swap(p.items); }

	bool empty() const { return items.empty(); }
	void clear() { items.clear(); }
	void remove(int i, int n = 1)
	{
		if (i < 0 || n <= 0 || (size_t)(i+n) > items.size()) return;
		items.erase(items.begin() + i, items.begin() + i + n);
	}
	void move(int i, int j)
	{
		int n = (int)items.size();
		if (i == j || i < 0 || j < 0 || i >= n || j >= n) return;
		while (i < j) { std::swap(items[i], items[i+1]); ++i; }
		while (i > j) { std::swap(items[i], items[i-1]); --i; }
	}

	bool load_directory(const str &directory, bool include_updir=true);
	bool add_directory(const str &directory, bool recursive = true);
	bool load_m3u(const str &path);
	bool save (const str &m3u_path) const;
	plist & operator+= (const plist &other);
	plist & operator+= (plist &&other);
	plist & operator+= (const plist_item &i);
	plist & operator+= (str &&f) { items.emplace_back(new plist_item(f)); return *this; }
	plist & operator+= (const str &f) { items.emplace_back(new plist_item(f)); return *this; }
	plist & operator+= (const char *f) { items.emplace_back(new plist_item(f)); return *this; }

	void insert(const plist &other, int pos); // pos = -1 to add
	void insert(plist &&other, int pos); // pos = -1 to add
	void insert(const str &f, int pos); // pos = -1 to add

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
	void set_tags(const str &file, file_tags *tags)
	{
		for (auto &i : items)
		{
			if (i->path != file) continue;
			if (i->tags) *i->tags = *tags; else i->tags.reset(new file_tags(*tags));
		}
	}
	void set_rating(const str &file, int rating)
	{
		for (auto &i : items)
		{
			if (i->path != file) continue;
			if (i->tags) i->tags->rating = rating;
		}
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
		std::swap(is_dir, other.is_dir);
	}

	std::vector<std::unique_ptr<plist_item> > items;
	bool is_dir; // otherwise it's a playlist
};

inline void swap(plist &a, plist &b) { a.swap(b); }
