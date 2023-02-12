#pragma once
#include "../playlist.h"
#include <deque>

class ServerPlaylist
{
public:
	ServerPlaylist();
	
	void clear();
	void add(const str &path);
	void add(const plist &pl, int idx);
	void remove(int i, int n);
	void move(int i, int j);
	void remove(const std::set<str> &files);
	void move(const std::set<str> &files, const str &dir);
	void rename(const str &file, const str &new_path);

	const plist &list() const { return playlist; }

	typedef std::pair<bool,int> song; // (dir_plist?, index), index=-1 means null/invalid
	static inline song S(bool b, int i) { return std::make_pair(b,i); }
	
	void play(plist &&p, int i);
	void play(int i) { play(S(false, i), true); }
	void play(const str &path); // creates and goes to dir_plist
	void play(song s, bool restarting = false); // clears the shuffle order if restarting
	void invalidate(const str &path); // sets its type to F_OTHER, it will be ignored after that
	void stop() { play(S(dir,-1), true); } // next(true) is start() now
	bool stopped() const { return i1 == -1; }

	song next(bool force = false) const; // force ignores Repeat and AutoNext
	song prev() const;
	song current() const { return S(dir, i1); }

	str path(song s) const
	{
		auto &p = s.first ? dir_plist : playlist;
		if (s.second < 0 || s.second >= p.size()) return "";
		auto it = *p.items[s.second];
		if (!valid_type(it.type)) return "";
		return it.path;
	}
	const plist_item *current_item() const
	{
		auto &p = dir ? dir_plist : playlist;
		if (i1 < 0 || i1 >= p.size()) return NULL;
		return p.items[i1].get();
	}

private:
	inline static bool valid_type(file_type t) { return t == F_SOUND; }
	static constexpr file_type invalid_type = F_OTHER;

	song random() const;
	song first() const;
	song last() const;
	void reshuffle(int first_item) const;

	plist playlist, dir_plist; // items with type F_OTHER are considered invalid and never returned!
	mutable int  i0; // current song, before shuffling
	int  i1;  // current song, after shuffling
	bool dir; // currently in dir_plist? current() returns (dir,i1)
	str  cwd; // source of dir_plist
	mutable int nv[2]; // number of valid items in the lists
	mutable std::vector<int> order, order_inv; // we play order[0], order[1], ...
	std::deque<int> history; // for going back with shuffle active, play(...) puts items here
};
