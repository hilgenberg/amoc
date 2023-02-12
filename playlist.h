#pragma once
#include <memory>
#include "file_tags.h"

enum file_type
{
	F_OTHER = 0,
	F_DIR,
	F_SOUND,
	F_PLAYLIST
};

class plist_item
{
public:
	static file_type ftype(const str &path);

	explicit plist_item(str &&p) : path(p), type(ftype(path)), tags(NULL) { assert(!path.empty() && path[0] == '/'); }
	explicit plist_item(const str &p) : path(p), type(ftype(path)), tags(NULL) { assert(!path.empty() && path[0] == '/'); }
	plist_item(const str &p, file_type t) : path(p), type(t), tags(NULL) { assert(!path.empty() && path[0] == '/'); }
	plist_item(const plist_item &i) : path(i.path), type(i.type), tags(i.tags) { if (tags) ++tags->usage; }

	bool can_tag() const; // can we write tags for this?

	str       path; // absolute path
	file_type type;
	mutable file_tags *tags; // not owned, not deleted!
};
bool operator< (const plist_item &a, const plist_item &b);

class Tags;

struct plist
{
public:
	plist(Tags *tags = NULL) : is_dir(false), tags(tags) {}
	plist(const plist &) = delete;
	plist(plist &&p) : is_dir(p.is_dir), tags(p.tags) { items.swap(p.items); }
	~plist();

	bool empty() const { return items.empty(); }
	size_t size() const { return items.size(); }

	plist_item& operator[] (int i) { assert(i >= 0 && i < (int)items.size()); return *items[i]; }
	const plist_item& operator[] (int i) const { assert(i >= 0 && i < (int)items.size()); return *items[i]; }

	void clear();
	void swap(plist &other);
	void remove(int i, int n = 1);
	void remove(const std::set<int> &idx);
	int  remove(const std::set<str> &files, int i0 = -1); // returns where the item at i0 went
	void replace(const std::map<str, str> &mod);
	void move(int i, int j);

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

	int find(const str &file) const
	{
		for (int i = 0, n = (int)items.size(); i < n; ++i)
			if (items[i]->path == file) return i;
		return -1;
	}

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

	std::vector<std::unique_ptr<plist_item> > items;
	Tags *tags; // can be NULL
	bool is_dir; // otherwise it's a playlist
};

inline void swap(plist &a, plist &b) { a.swap(b); }
