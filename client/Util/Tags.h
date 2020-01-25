#pragma once
#include "../../playlist.h"
#include "../../Socket.h"

class Tags
{
public:
	std::map<str, file_tags> tags, changes;
	std::set<str> requests; // to drop some duplicate requests. not exact, but that's ok

	inline void connect(const plist_item &it) const
	{
		if (it.tags) return;
		auto i = tags.find(it.path); if (i == tags.end()) return;
		++i->second.usage;
		it.tags = const_cast<file_tags*>(&i->second);
	}

	void release(plist_item &it)
	{
		requests.erase(it.path);
		if (!it.tags) return;
		if (!it.tags->usage) { assert(false); return; }// would be ok, but we're not using it like that yet
		if (!--it.tags->usage)
		{
			assert(tags.find(it.path) != tags.end() && &tags[it.path] == it.tags);
			tags.erase(it.path);
		}
		it.tags = NULL;
	}
	void request(const str &path, Socket &srv)
	{
		if (requests.count(path) || tags.count(path)) return;
		if (is_url(path)) return;

		requests.insert(path);

		srv.send(CMD_GET_FILE_TAGS);
		srv.send(path);
	}
	void request(const plist &plist, Socket &srv)
	{
		for (auto &i : plist.items)
		{
			connect(*i);
			if (i->tags || i->type != F_SOUND) continue;
			request(i->path, srv);
		}
	}

	void update(const str &path, std::unique_ptr<file_tags> &&tag)
	{
		// handle server response
		assert(tag->usage == 0);
		tags[path] = std::move(*tag);
		requests.erase(path);
	}

	void remove_unused()
	{
		for (auto it = tags.begin(); it != tags.end(); )
		{
			if (it->second.usage) ++it; else it = tags.erase(it);
		}
	}

	void set_artist(const plist_item &it, const str &val)
	{
		connect(it);
		if (val.empty() || (it.tags && it.tags->artist == val))
		{
			auto i = changes.find(it.path); if (i == changes.end()) return;
			i->second.artist.clear();
			if (empty(i->second)) changes.erase(i);
			return;
		}
		changes[it.path].artist = val;
	}
	void set_album(const plist_item &it, const str &val)
	{
		connect(it);
		if (val.empty() || (it.tags && it.tags->album == val))
		{
			auto i = changes.find(it.path); if (i == changes.end()) return;
			i->second.album.clear();
			if (empty(i->second)) changes.erase(i);
			return;
		}
		changes[it.path].album = val;
	}
	void set_title(const plist_item &it, const str &val)
	{
		connect(it);
		if (val.empty() || (it.tags && it.tags->title == val))
		{
			auto i = changes.find(it.path); if (i == changes.end()) return;
			i->second.title.clear();
			if (empty(i->second)) changes.erase(i);
			return;
		}
		changes[it.path].title = val;
	}
	void set_track(const plist_item &it, int val)
	{
		connect(it);
		// Note: track -1 keeps it as is, track 0 clears it
		#define N(t) ((t) <= 0 ? 0 : (t))
		if (val <= 0 || (it.tags && N(it.tags->track) == N(val)))
		{
			auto i = changes.find(it.path); if (i == changes.end()) return;
			i->second.track = -1;
			if (empty(i->second)) changes.erase(i);
			return;
		}
		changes[it.path].track = N(val);
		#undef N
	}

	void set_rating(const str &path, int val)
	{
		auto it = tags.find(path);
		if (it == tags.end()) return;
		it->second.rating = val;
	}

	str get_title(const plist_item &it) const
	{
		connect(it);
		auto ch = changes.find(it.path);
		if (ch != changes.end() && !ch->second.title.empty()) return ch->second.title;
		return it.tags ? it.tags->title : str();
	}
	str get_artist(const plist_item &it) const
	{
		connect(it);
		auto ch = changes.find(it.path);
		if (ch != changes.end() && !ch->second.artist.empty()) return ch->second.artist;
		return it.tags ? it.tags->artist : str();
	}
	str get_album(const plist_item &it) const
	{
		connect(it);
		auto ch = changes.find(it.path);
		if (ch != changes.end() && !ch->second.album.empty()) return ch->second.album;
		return it.tags ? it.tags->album : str();
	}
	int get_track(const plist_item &it) const
	{
		connect(it);
		auto ch = changes.find(it.path);
		if (ch != changes.end() && ch->second.track > 0) return ch->second.track;
		return it.tags ? it.tags->track : -1;
	}

	int get_time(const str &path) const
	{
		auto it = tags.find(path);
		return it == tags.end() ? 0 : it->second.time;
	}

private:
	static bool empty(const file_tags &it)
	{
		return it.track < 0 && it.title.empty() && it.artist.empty() && it.album.empty();
	}
};
