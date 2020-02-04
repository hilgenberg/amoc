#pragma once
#include <optional>

struct file_tags
{
	file_tags() : track(-1), time(-1), rating(-1), usage(0) {}
	file_tags(const file_tags &t)
	: title(t.title), artist(t.artist), album(t.album)
	, track(t.track), rating(t.rating), time(t.time)
	, usage(0)
	{
	}
	file_tags(const file_tags &&t)
	: title(std::move(t.title)), artist(std::move(t.artist)), album(std::move(t.album))
	, track(t.track), rating(t.rating), time(t.time)
	, usage(0)
	{
		assert(t.usage == 0);
	}

	str title, artist, album;
	int track, rating;
	int time; // in seconds or -1 for streams
	mutable int usage; // reference count

	file_tags& operator= (file_tags &&t)
	{
		artist = std::move(t.artist);
		album = std::move(t.album);
		title = std::move(t.title);
		track = t.track;
		rating = t.rating;
		time = t.time;
		// leave usage as is
		return *this;
	}
	file_tags& operator= (const file_tags &t)
	{
		artist = t.artist;
		album = t.album;
		title = t.title;
		track = t.track;
		rating = t.rating;
		time = t.time;
		// leave usage as is
		return *this;
	}
};

struct tag_changes
{
	std::optional<str> title, artist, album;
	std::optional<int> track;

	bool empty() const
	{
		return !track && !title && !artist && !album;
	}
};
