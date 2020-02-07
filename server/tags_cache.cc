#include "tags_cache.h"
#include "server.h"
#include "../playlist.h"
#include "audio.h"
#include "input/decoder.h"
#include "ratings.h"
#include <pthread.h>
#include <sys/stat.h>

/* Read the selected tags for this file and add it to the cache.
 * If client_id != -1, the server is notified using tags_response().
 * If client_id == -1, copy of file_tags is returned. */
file_tags tags_cache::read_add (const str &file, int client_id)
{
	debug ("Getting tags for %s", file);

	auto lock = db->lock(file);
	auto rec = db->get(file);

	/* If this entry is already present in the cache, we have 3 options:
	 * we must read different tags (TAGS_*) or the tags are outdated
	 * or this is an immediate tags read (client_id == -1) */
	time_t current_mtime = get_mtime (file);
	if (rec)
	{
		if (rec.mod_time == current_mtime)
		{
			debug ("Cache hit.");
			return std::move(rec.tags);
		}
		debug ("Tags in the cache are outdated");
	}

	auto *df = get_decoder (file);
	if (df && df->info) df->info(file.c_str(), &rec.tags);
	rec.tags.rating = ratings_read(file);
	rec.mod_time = current_mtime;

	db->add(file, rec);

	if (client_id != -1) tags_response (client_id, file, &rec.tags);

	return std::move(rec.tags);
}


/* Read the selected tags for this file and add it to the cache.
 * If client_id != -1, the server is notified using tags_response().
 * If client_id == -1, copy of file_tags is returned. */
void tags_cache::write_add (const str &file, tag_changes *tags, int client_id)
{
	assert(tags);
	if (tags->empty()) return;
	
	#ifndef NDEBUG
	debug ("Setting tags for %s", file);
	if (tags->artist) debug ("*** artist: %s", tags->artist->c_str());
	if (tags->album)  debug ("***  album: %s", tags->album->c_str());
	if (tags->title)  debug ("***  title: %s", tags->title->c_str());
	if (tags->track)  debug ("***  track: %d", *tags->track);
	#endif

	auto *df = get_decoder (file);
	if (!df || !df->write_info)
	{
		status_msg(format("Can not write tags for %s", file.c_str()));
		delete tags;
		return;
	}
	bool ok = df->write_info(file, *tags);
	delete tags;
	if (!ok)
	{
		status_msg(format("Failed writing tags for %s", file.c_str()));
		return;
	}

	assert (client_id != -1);
	read_add(file, client_id);
}

void tags_cache::ratings_changed(const str &file, int rating)
{
	assert (!file.empty());
	debug ("Updating tags for %s", file.c_str());

	auto lock = db->lock(file);
	auto rec = db->get(file);

	if (!rec) return; // nothing to do

	time_t current_mtime = get_mtime (file);
	if (rec.mod_time != current_mtime)
	{
		debug ("Ignoring outdated entry");
		return;
	}
	if (rec.tags.rating == rating)
	{
		debug ("Ignoring dummy update");
		return;
	}

	rec.tags.rating = rating;
	db->add(file, rec);
}


void *tags_cache::reader_thread(void *cache_ptr)
{
	logit ("Tags reader thread started");

	tags_cache *c = (tags_cache *)cache_ptr;
	LOCK (c->mutex);

	for (int client = 0, last_client = 0; !c->stop_reader_thread; )
	{
		++client; client %= CLIENTS_MAX;
		auto &q = c->queues[client];
		if (!q.empty())
		{
			last_client = client;
			auto &rq = q.front();
			str file = rq.path;
			tag_changes *tags = rq.tags.release();
			q.pop();
			UNLOCK (c->mutex);
			if (!tags)
				c->read_add (file, client);
			else
				c->write_add(file, tags, client);
			LOCK (c->mutex);
		}
		else if (client == last_client)
		{
			debug ("All queues empty, waiting");
			pthread_cond_wait (&c->request_cond, &c->mutex);
			continue;
		}
	}

	UNLOCK (c->mutex);
	logit ("Exiting tags reader thread");

	return NULL;
}

tags_cache::tags_cache()
: stop_reader_thread(false)
, db(NULL)
{
	pthread_mutex_init (&mutex, NULL);
	int rc = pthread_cond_init (&request_cond, NULL);
	if (rc != 0) fatal ("Can't create request_cond: %s", xstrerror (rc));
	rc = pthread_create (&reader_thread_id, NULL, reader_thread, this);
	if (rc != 0) fatal ("Can't create tags cache thread: %s", xstrerror (rc));

	try
	{
		db = new tags_db;
	}
	catch(std::exception &e)
	{
		// TODO: run with db == NULL?

		db = NULL;
		fatal("Can't create tags_db: %s", e.what());
	}
}

tags_cache::~tags_cache()
{
	LOCK (mutex);
	stop_reader_thread = true;
	pthread_cond_signal (&request_cond);
	UNLOCK (mutex);

	delete db;

	int rc = pthread_join (reader_thread_id, NULL);
	if (rc != 0) fatal ("pthread_join() on cache reader thread failed: %s", xstrerror (rc));

	rc = pthread_mutex_destroy (&mutex);
	if (rc != 0) log_errno ("Can't destroy mutex", rc);
	rc = pthread_cond_destroy (&request_cond);
	if (rc != 0) log_errno ("Can't destroy request_cond", rc);
}

void tags_cache::add_request (const str &file, int client_id, tag_changes *tags)
{
	assert (LIMIT(client_id, CLIENTS_MAX));

	if (!tags)
	{
		debug ("Request for tags for '%s' from client %d", file.c_str(), client_id);

		auto rec = db->get(file);
		if (rec) {
			if (rec.mod_time == get_mtime(file)) {
				tags_response (client_id, file, &rec.tags);
				debug ("Tags are present in the cache");
				return;
			}
			debug ("Found outdated tags in the cache");
		}
	}

	LOCK (mutex);
	queues[client_id].emplace(file, tags);
	pthread_cond_signal (&request_cond);
	UNLOCK (mutex);
}

void tags_cache::clear_queue (int client_id)
{
	assert (LIMIT(client_id, CLIENTS_MAX));
	LOCK (mutex);
	request_queue().swap(queues[client_id]);
	debug ("Cleared requests queue for client %d", client_id);
	UNLOCK (mutex);
}

/* Immediately read tags for a file bypassing the request queue. */
file_tags tags_cache::get_immediate (const str &file)
{
	debug ("Immediate tags read for %s", file.c_str());
	return !is_url(file) ? read_add(file, -1) : file_tags();
}

void tags_cache::files_rm(std::set<str> &src)
{
	std::set<str> fails;
	for (auto &f : src)
	{
		if (unlink(f.c_str()) != 0)
			fails.insert(f);
		else
		{
			db->remove(f);
			ratings_remove(f);
		}
	}
	src.erase(fails.begin(), fails.end());
}

void tags_cache::files_mv(std::set<str> &src, const str &dst)
{
	if (dst.empty() || dst[0] != '/') { src.clear(); return; }
	std::set<str> fails;
	for (auto &f : src)
	{
		if (f.empty() || f[0] != '/' || !is_regular_file(f))
		{
			logit ("rename(%s,%s) failed: error in first argument", f.c_str(), dst.c_str());
			fails.insert(f);
			continue;
		}
		str p = add_path(dst, file_name(f));
		if (file_exists(p))
		{
			logit ("rename(%s,%s) failed: file exists", f.c_str(), p.c_str());
			fails.insert(f);
			continue;
		}
		if (rename(f.c_str(), p.c_str()) != 0)
		{
			char *err = xstrerror (errno);
			logit ("rename(%s,%s) failed: %s", f.c_str(), p.c_str(), err);
			free(err);

			fails.insert(f);
			continue;
		}
		db->remove(f);
		ratings_move(f, p);
	}
	src.erase(fails.begin(), fails.end());
}

bool tags_cache::files_mv(const str &src, str &dst)
{
	if (src.empty() || src[0] != '/') return false;
	if (!is_regular_file(src)) return false;

	// dst must either be a filename only or an absolute
	// directory or filepath
	if (dst.empty()) return false;
	if (dst[0] != '/')
	{
		if (dst.find('/') != str::npos) return false;
		dst = add_path(containing_directory(src), dst);
	}
	else if (is_dir(dst))
	{
		dst = add_path(dst, file_name(src));
	}
	if (file_exists(dst)) return false;
	if (rename(src.c_str(), dst.c_str()) != 0) return false;
	ratings_move(src, dst);
	return true;
}
