#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <dirent.h>
#include "server.h"
#include "../playlist.h"
#include "tags_cache.h"
#include "audio.h"
#include "input/decoder.h"
#include "ratings.h"

#undef STRERROR_FN
#define STRERROR_FN bdb_strerror
/* BerkleyDB-provided error code to description function wrapper. */
static inline char *bdb_strerror (int errnum)
{
	return errnum > 0 ? xstrerror(errnum) : xstrdup(db_strerror (errnum));
}
#ifndef NDEBUG
static void db_err_cb (const DB_ENV *unused, const char *errpfx, const char *msg)
{
	assert (msg);
	if (errpfx && errpfx[0])
		logit ("BDB said: %s: %s", errpfx, msg);
	else
		logit ("BDB said: %s", msg);
}
static void db_msg_cb (const DB_ENV *unused, const char *msg)
{
	assert (msg);
	logit ("BDB said: %s", msg);
}
static void db_panic_cb (DB_ENV *unused, int errval)
{
	log_errno ("BDB said", errval);
}
#endif

/* The name of the tags database in the cache directory. */
#define TAGS_DB "tags.db"

/* The name of the version tag file in the cache directory. */
#define MOC_VERSION_TAG "moc_version_tag"

/* Number used to create cache version tag to detect incompatibilities
 * between cache version stored on the disk and MOC/BerkeleyDB environment.
 *
 * If you modify the DB structure, increase this number.  You can also
 * temporarily set it to zero to disable cache activity during structural
 * changes which require multiple commits.
 */
#define CACHE_DB_FORMAT_VERSION	4

/* How frequently to flush the tags database to disk.  A value of zero
 * disables flushing. */
#define DB_SYNC_COUNT 5

static std::vector<char> cache_record_serialize (const cache_record &rec)
{
	const auto &tags = rec.tags;
	size_t artist_len = tags.artist.length()+1;
	size_t album_len = tags.album.length()+1;
	size_t title_len = tags.title.length()+1;

	size_t len = sizeof(rec.mod_time)
		+ artist_len + album_len + title_len
		+ sizeof(tags.track)
		+ 1 /* rating */
		+ sizeof(tags.time);
	std::vector<char> buf(len);
	char *p = (char*)buf.data();

	memcpy (p, &rec.mod_time, sizeof(rec.mod_time)); p += sizeof(rec.mod_time);
	memcpy (p, tags.artist.c_str(), artist_len); p += artist_len;
	memcpy (p, tags.album.c_str(), album_len); p += album_len;
	memcpy (p, tags.title.c_str(), title_len); p += title_len;
	memcpy (p, &tags.track, sizeof(tags.track)); p += sizeof(tags.track);
	memcpy (p, &tags.time, sizeof(tags.time)); p += sizeof(tags.time);
	*p++ = (char)tags.rating;
	return buf;
}
static bool cache_record_deserialize (cache_record &rec, const char *buf, size_t bytes_left)
{
	auto &tags = rec.tags;
	const char *p = buf;

	#define extract_num(var) \
	do { \
		if (bytes_left < sizeof(var)) goto err; \
		memcpy (&var, p, sizeof(var)); \
		bytes_left -= sizeof(var); \
		p += sizeof(var); \
	} while (0)

	#define extract_str(var) \
	do { \
		size_t len = strlen(p) + 1; \
		if (len > bytes_left) goto err; \
		var = p; p += len; bytes_left -= len; \
		assert(var.length() == len-1); \
	} while (0)

	extract_num (rec.mod_time);
	extract_str (tags.artist);
	extract_str (tags.album);
	extract_str (tags.title);
	extract_num (tags.track);
	extract_num (tags.time);

	if (!bytes_left) goto err;
	tags.rating = *p++;
	--bytes_left;

	return true;

err:
	logit ("Cache record deserialization error at %tdB", p - buf);
	return false;
}

/* This function ensures that a DB function takes place while holding a
 * database record lock.  It also provides an initialised database thang
 * for the key and record. */
tags_cache::Lock::Lock(tags_cache &c, DBT &key)
: db_env(c.db_env)
{
	int rc = db_env->lock_get (db_env, c.locker, 0, &key, DB_LOCK_WRITE, &lock);
	if (rc) fatal ("Can't get DB lock: %s", db_strerror (rc));
}

tags_cache::Lock::~Lock()
{
	int rc = db_env->lock_put (db_env, &lock);
	if (rc) fatal ("Can't release DB lock: %s", db_strerror (rc));
}

void tags_cache::remove_rec (const str &fname)
{
	debug ("Removing %s from the cache...", fname.c_str());

	DBT key;
	memset (&key, 0, sizeof(key));
	key.data = (void*)fname.c_str();
	key.size = fname.length();

	int ret = db->del (db, NULL, &key, 0);
	if (ret)
		logit ("Can't remove item for %s from the cache: %s",
				fname, db_strerror (ret));
}

/* Synchronize cache every DB_SYNC_COUNT updates. */
void tags_cache::sync ()
{
	static int sync_count = 0;
	if (DB_SYNC_COUNT == 0) return;
	if (++sync_count >= DB_SYNC_COUNT) {
		sync_count = 0;
		db->sync (db, 0);
	}
}

/* Add this tags object for the file to the cache. */
void tags_cache::add (DBT &key, const cache_record &rec)
{
	DBT data;

	debug ("Adding/updating cache object");

	auto buf = cache_record_serialize (rec);
	if (buf.empty()) return;

	memset (&data, 0, sizeof(data));
	data.data = buf.data();
	data.size = buf.size();

	int ret = db->put (db, NULL, &key, &data, 0);
	if (ret) error_errno ("DB put error", ret);

	sync();
}

/* Read the selected tags for this file and add it to the cache.
 * If client_id != -1, the server is notified using tags_response().
 * If client_id == -1, copy of file_tags is returned. */
file_tags tags_cache::read_add (const str &file, int client_id)
{
	debug ("Getting tags for %s", file);

	cache_record rec;
	DBT key, record;
	memset (&key, 0, sizeof (key));
	memset (&record, 0, sizeof (record));
	key.data = (void *) file.c_str();
	key.size = file.length();
	record.flags = DB_DBT_MALLOC;

	Lock lock(*this, key);

	int ret = db->get (db, NULL, &key, &record, 0);
	if (ret && ret != DB_NOTFOUND) log_errno ("Cache DB get error", ret);

	/* If this entry is already present in the cache, we have 3 options:
	 * we must read different tags (TAGS_*) or the tags are outdated
	 * or this is an immediate tags read (client_id == -1) */
	bool ok = (ret == 0 && cache_record_deserialize(rec, (const char*)record.data, record.size));
	free (record.data);
	time_t current_mtime = get_mtime (file);
	if (ok)
	{
		if (rec.mod_time == current_mtime)
		{
			debug ("Cache hit.");
			return std::move(rec.tags);
		}
		else 
		{
			debug ("Tags in the cache are outdated");
		}
	}

	auto *df = get_decoder (file);
	if (df && df->info) df->info (file.c_str(), &rec.tags);
	rec.tags.rating = ratings_read(file);

	rec.mod_time = current_mtime;
	add (key, rec);

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

	cache_record rec;
	DBT key, record;
	memset (&key, 0, sizeof (key));
	memset (&record, 0, sizeof (record));
	key.data = (void *) file.c_str();
	key.size = file.length();
	record.flags = DB_DBT_MALLOC;

	Lock lock(*this, key);

	int ret = db->get (db, NULL, &key, &record, 0);
	if (ret && ret != DB_NOTFOUND) log_errno ("Cache DB get error", ret);
	if (ret == DB_NOTFOUND) return; // nothing to do

	/* If this entry is already present in the cache, we have 3 options:
	 * we must read different tags (TAGS_*) or the tags are outdated
	 * or this is an immediate tags read (client_id == -1) */
	bool ok = cache_record_deserialize(rec, (const char*)record.data, record.size);
	free (record.data);
	if (!ok)
	{
		debug ("Ignoring garbage entry.");
		return;
	}

	time_t current_mtime = get_mtime (file);
	if (rec.mod_time != current_mtime)
	{
		debug ("Ignoring outdated entry");
		return;
	}

	debug ("Cache hit.");
	if (rec.tags.rating == rating)
	{
		debug ("Ignoring dummy update");
		return;
	}

	rec.tags.rating = rating;
	add (key, rec);
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
, db(NULL), db_env(NULL)
{
	pthread_mutex_init (&mutex, NULL);
	int rc = pthread_cond_init (&request_cond, NULL);
	if (rc != 0) fatal ("Can't create request_cond: %s", xstrerror (rc));
	rc = pthread_create (&reader_thread_id, NULL, reader_thread, this);
	if (rc != 0) fatal ("Can't create tags cache thread: %s", xstrerror (rc));
}

tags_cache::~tags_cache()
{
	LOCK (mutex);
	stop_reader_thread = true;
	pthread_cond_signal (&request_cond);
	UNLOCK (mutex);

	if (db) {
		#ifndef NDEBUG
		db->set_errcall (db, NULL);
		db->set_msgcall (db, NULL);
		db->set_paniccall (db, NULL);
		#endif
		db->close (db, 0);
		db = NULL;
	}
	if (db_env) {
		db_env->lock_id_free (db_env, locker);
		#ifndef NDEBUG
		db_env->set_errcall (db_env, NULL);
		db_env->set_msgcall (db_env, NULL);
		db_env->set_paniccall (db_env, NULL);
		#endif
		db_env->close (db_env, 0);
		db_env = NULL;
	}

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

		DBT key, record;
		memset (&key, 0, sizeof (key));
		key.data = (void *) file.c_str();
		key.size = file.length();
		memset (&record, 0, sizeof (record));
		record.flags = DB_DBT_MALLOC;

		Lock lock(*this, key);

		struct cache_record rec;

		int db_ret = db->get (db, NULL, &key, &record, 0);

		if (db_ret && db_ret != DB_NOTFOUND) {
			error_errno ("Cache DB search error", db_ret);
		}

		bool ok = (db_ret == 0 && cache_record_deserialize(rec, (const char*)record.data, record.size));
		free(record.data);
		if (ok) {
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

/* Create a MOC/db version string. */
static str create_version_tag()
{
	int db_major, db_minor;
	db_version (&db_major, &db_minor, NULL);
	return format("%d %d %d", CACHE_DB_FORMAT_VERSION, db_major, db_minor);
}

/* Check version of the cache directory.  If it was created
 * using format not handled by this version of MOC, return 0. */
static bool cache_version_matches (const str &cache_dir)
{
	str fname = add_path(cache_dir, MOC_VERSION_TAG);
	FILE *f = fopen (fname.c_str(), "r");
	if (!f) return false;

	str vt0 = create_version_tag(), vt;
	vt.resize(vt0.length()+1);
	ssize_t rres = fread ((char*)vt.c_str(), 1, vt0.length()+1, f);
	fclose(f); f = NULL;
	if (rres != vt0.length()) return false;
	vt.resize(rres);
	logit("Cache version %s", vt.c_str());
	return vt == vt0;
}

void tags_cache::load()
{
	str cache_dir = options::run_file_path("cache");

	bool wcv = false;
	int ret;

	if (mkdir(cache_dir.c_str(), 0700) == 0) {
		wcv = true;
	}
	else if (errno != EEXIST) {
		error_errno ("Failed to create directory for tags cache", errno);
		error ("Can't prepare cache directory!");
		goto err;
	}
	else if (!cache_version_matches (cache_dir)) {
		logit ("Tags cache directory is the wrong version, purging....");

		if (!purge_directory (cache_dir))
		{
			error ("Can't prepare cache directory!");
			goto err;
		}
		wcv = true;
	}
	if (wcv)
	{
		str p = add_path(cache_dir, MOC_VERSION_TAG);
		FILE *f = fopen (p.c_str(), "w");
		if (!f) {
			log_errno ("Error opening cache", errno);
			goto err;
		}

		str vt = create_version_tag();
		if (fwrite (vt.c_str(), vt.length(), 1, f) != 1)
			logit ("Error writing cache version tag");

		fclose (f);
	}

	ret = db_env_create (&db_env, 0);
	if (ret) {
		error_errno ("Can't create DB environment", ret);
		goto err;
	}

#ifndef NDEBUG
	db_env->set_errcall (db_env, db_err_cb);
	db_env->set_msgcall (db_env, db_msg_cb);
	ret = db_env->set_paniccall (db_env, db_panic_cb);
	if (ret) logit ("Could not set DB panic callback");
#endif

	ret = db_env->open (db_env, cache_dir.c_str(),
	                       DB_CREATE | DB_PRIVATE | DB_INIT_MPOOL |
	                       DB_THREAD | DB_INIT_LOCK, 0);
	if (ret) {
		error ("Can't open DB environment (%s): %s", cache_dir.c_str(), db_strerror (ret));
		goto err;
	}

	ret = db_env->lock_id (db_env, &locker);
	if (ret) {
		error_errno ("Failed to get DB locker", ret);
		goto err;
	}

	ret = db_create (&db, db_env, 0);
	if (ret) {
		error_errno ("Failed to create cache db", ret);
		goto err;
	}

#ifndef NDEBUG
	db->set_errcall (db, db_err_cb);
	db->set_msgcall (db, db_msg_cb);
	ret = db->set_paniccall (db, db_panic_cb);
	if (ret) logit ("Could not set DB panic callback");
#endif

	ret = db->open (db, NULL, TAGS_DB, NULL, DB_BTREE, DB_CREATE | DB_THREAD, 0);
	if (ret) {
		error_errno ("Failed to open (or create) tags cache db", ret);
		goto err;
	}

	return;

err:
	if (db) {
#ifndef NDEBUG
		db->set_errcall (db, NULL);
		db->set_msgcall (db, NULL);
		db->set_paniccall (db, NULL);
#endif
		db->close (db, 0);
		db = NULL;
	}
	if (db_env) {
#ifndef NDEBUG
		db_env->set_errcall (db_env, NULL);
		db_env->set_msgcall (db_env, NULL);
		db_env->set_paniccall (db_env, NULL);
#endif
		db_env->close (db_env, 0);
		db_env = NULL;
	}
	error ("Failed to initialise tags cache: caching disabled");
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
			remove_rec(f);
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
		remove_rec(f);
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
