#pragma once
#include "../../playlist.h"

struct TagChanges
{
	std::map<str, file_tags> changes;

	bool empty() const { return changes.empty(); }

	void set_artist(const plist_item &it, const str &val)
	{
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
		if (val <= 0 || (it.tags && it.tags->track == val))
		{
			auto i = changes.find(it.path); if (i == changes.end()) return;
			i->second.title.clear();
			if (empty(i->second)) changes.erase(i);
			return;
		}
		changes[it.path].track = val;
	}

	str get_title(const plist_item &it) const
	{
		auto ch = changes.find(it.path);
		if (ch != changes.end() && !ch->second.title.empty()) return ch->second.title;
		return it.tags ? it.tags->title : str();
	}
	str get_artist(const plist_item &it) const
	{
		auto ch = changes.find(it.path);
		if (ch != changes.end() && !ch->second.artist.empty()) return ch->second.artist;
		return it.tags ? it.tags->artist : str();
	}
	str get_album(const plist_item &it) const
	{
		auto ch = changes.find(it.path);
		if (ch != changes.end() && !ch->second.album.empty()) return ch->second.album;
		return it.tags ? it.tags->album : str();
	}
	int get_track(const plist_item &it) const
	{
		auto ch = changes.find(it.path);
		if (ch != changes.end() && ch->second.track > 0) return ch->second.track;
		return it.tags ? it.tags->track : -1;
	}

	static bool empty(const file_tags &it)
	{
		return it.track <= 0 && it.title.empty() && it.artist.empty() && it.album.empty();
	}
};
