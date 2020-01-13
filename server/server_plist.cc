#include "server_plist.h"

#define VALID(i) valid_type(p.items[i]->type)
#define NIL      S(dir,-1)
#define IT       S(dir, i)

ServerPlaylist::ServerPlaylist()
: i0(-1), i1(-1), dir(false), nv{0,0}
{}

void ServerPlaylist::play(plist &&p, int i)
{
	logit("Playlist: playing item %d on new playlist with %d items", i, p.size());
	playlist.swap(p);
	nv[0] = 0; for (auto &it : playlist.items) if (valid_type(it->type)) ++nv[0];
	play(S(false, i), true);
}
void ServerPlaylist::play(const str &path)
{
	logit("Playlist: playing file %s", path.c_str());
	str cd = containing_directory(path);
	if (cd != cwd)
	{
		cwd = cd;
		dir_plist.clear();
		dir_plist.add_directory(cwd, false);
		nv[1] = 0; for (auto &it : dir_plist.items) if (valid_type(it->type)) ++nv[1];
	}
	play(S(true, dir_plist.find(path)), true);
}

void ServerPlaylist::play(song s, bool restarting)
{
	dir = s.first;
	i1 = s.second;
	logit("Playlist: playing song (%s,%d)%s", dir ? "dir" : "lst", i1, restarting ? " with restart" : "");
	if (restarting)
	{
		order.clear(); order_inv.clear();
		history.clear();
	}
}
void ServerPlaylist::invalidate(const str &path)
{
	auto &p = dir ? dir_plist : playlist;
	auto &n = nv[dir];
	// check current song first
	if (i1 >= 0 || i1 < p.size())
	{
		auto &it = *p.items[i1];
		if (it.path == path && valid_type(it.type))
		{
			--n;
			it.type = invalid_type;
			return;
			// there could be more entries for this path, but they get fixed
			// when this gets called again
		}
	}
	// check next (because either play or precache should find invalid songs)
	song nxt = next(true);
	if (nxt.second != -1)
	{
		auto &it = *p.items[nxt.second];
		if (it.path == path && valid_type(it.type))
		{
			--n;
			it.type = invalid_type;
			return;
		}
	}

	// if it was neither of the above, then check the entire list
	bool found = false;
	for (auto &it : playlist.items)
	{
		if (valid_type(it->type) && it->path == path)
		{
			--nv[0];
			it->type = invalid_type;
			found = true;
		}
	}
	for (auto &it : dir_plist.items)
	{
		if (valid_type(it->type) && it->path == path)
		{
			--nv[1];
			it->type = invalid_type;
			found = true;
		}
	}
	if (!found) logit("Trying to invalide missing item \"%s\"", path.c_str());
}

ServerPlaylist::song ServerPlaylist::random() const
{
	if (!nv[dir]) return NIL;
	auto &p = dir ? dir_plist : playlist;
	const int n = p.size();
	while (true)
	{
		int i = random_int(n-1);
		if (VALID(i)) return IT;
	}
}

ServerPlaylist::song ServerPlaylist::first() const
{
	if (!nv[dir]) return NIL;
	auto &p = dir ? dir_plist : playlist;
	const int n = p.size();
	for (int i = 0; i < n; ++i) if (VALID(i)) return IT;
	assert(false); nv[dir] = 0;
	return NIL;
}
ServerPlaylist::song ServerPlaylist::last() const
{
	if (!nv[dir]) return NIL;
	auto &p = dir ? dir_plist : playlist;
	int n = p.size();
	for (int i = n-1; i >= 0; --i) if (VALID(i)) return IT;
	assert(false); nv[dir] = 0;
	return NIL;
}

void ServerPlaylist::reshuffle(int first_item) const
{
	auto &p = dir ? dir_plist : playlist;
	const int n = p.size();
	order.resize(n);
	order_inv.resize(n);
	if (n == 0) return;
	assert(first_item >= 0 && first_item < n);
	order[0] = first_item;
	for (int i = 1; i < n; ++i) order[i] = (i > first_item ? i : i-1);
	for (int i = 1; i < n; ++i)
	{
		int j = random_int(i, n);
		std::swap(order[i], order[j]);
	}
	for (int i = 0; i < n; ++i) order_inv[order[i]] = i;
}

ServerPlaylist::song ServerPlaylist::next(bool force) const
{
	if (!nv[dir]) return NIL;
	auto &p = dir ? dir_plist : playlist;
	int n = p.size();
	bool have_valid_item = (i1 >= 0 && i1 < n && VALID(i1));
	
	if (!options::AutoNext)
	{
		if (options::Repeat)
		{
			if (have_valid_item) return current();
			if (options::Shuffle) return random();
		}
		else
		{
			if (!force) return NIL;
			if (options::Shuffle) return random();
			if (have_valid_item) return current();
		}
		return first();
	}

	if (!options::Shuffle)
	{
		// next line works for i1==-1 too
		for (int i = i1+1; i < n; ++i)
			if (VALID(i)) return IT;
		assert(i1 != -1);
		if (!options::Repeat && !force) return NIL;
		return first();
	}

	if (!have_valid_item) return random();

	if (order.size() != p.size()) reshuffle(i1);

	for (int i0 = order_inv[i1]+1; i0 < n; ++i0)
		if (VALID(order[i0])) return S(dir, order[i0]);

	if (!options::Repeat && !force) return NIL;
	
	reshuffle(i1);
	for (int i0 = 1; i0 < n; ++i0)
		if (VALID(order[i0])) return S(dir, order[i0]);

	assert(false); nv[dir] = 0; return NIL;
}

ServerPlaylist::song ServerPlaylist::prev() const
{
	if (!nv[dir]) return NIL;
	auto &p = dir ? dir_plist : playlist;
	int n = p.size();
	bool have_valid_item = (i1 >= 0 && i1 < n && VALID(i1));
	
	if (!options::Shuffle)
	{
		for (int i = i1-1; i >= 0; --i)
			if (VALID(i)) return IT;
		return last();
	}

	// rest is not really working after reshuffling, but ok for now...
	if (!have_valid_item) return random();
	if (order.size() != p.size()) return random();
	for (int i0 = order_inv[i1]-1; i0 >= 0; --i0)
		if (VALID(order[i0])) return S(dir, order[i0]);

	return random();
}

void ServerPlaylist::clear()
{
	playlist.clear();
	if (!dir) i1 = -1;
	nv[0] = 0;
}
void ServerPlaylist::add(const str &path)
{
	playlist += path;
	if (valid_type(playlist.items.back()->type)) ++nv[0];

}
void ServerPlaylist::add(const plist &pl, int idx)
{
	if (idx < 0)
		playlist += pl;
	else
	{
		playlist.insert(pl, idx);
		if (i1 >= idx) i1 += pl.size();
	}

	for (auto &it : pl.items) if (valid_type(it->type)) ++nv[0];
}
void ServerPlaylist::remove(int i, int n)
{
	playlist.remove(i, n);
	if (!dir)
	{
		if (i1 >= i) i1 -= std::min(n, i1-i);
	}
	// TODO: shuffle and make sure i1 != i
}
void ServerPlaylist::move(int i, int j)
{
	playlist.move(i, j); if (!dir && i == i1 && i != -1) i1 = j; 
	// TODO: shuffle...
}
