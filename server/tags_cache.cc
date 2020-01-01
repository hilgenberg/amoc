#include <pthread.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include "server.h"
#include "../playlist.h"
#include "../files.h"
#include "tags_cache.h"
#include "../output/audio.h"

#ifdef HAVE_DB_H
#undef STRERROR_FN
#define STRERROR_FN bdb_strerror
/* BerkleyDB-provided error code to description function wrapper. */
static inline char *bdb_strerror (int errnum)
{
	return errnum > 0 ? xstrerror(errnum) : xstrdup(db_strerror (errnum));
}
#ifndef NDEBUG
static void db_err_cb (const DB_ENV *unused ATTR_UNUSED, const char *errpfx, const char *msg)
{
	assert (msg);
	if (errpfx && errpfx[0])
		logit ("BDB said: %s: %s", errpfx, msg);
	else
		logit ("BDB said: %s", msg);
}
static void db_msg_cb (const DB_ENV *unused ATTR_UNUSED, const char *msg)
{
	assert (msg);
	logit ("BDB said: %s", msg);
}
static void db_panic_cb (DB_ENV *unused ATTR_UNUSED, int errval)
{
	log_errno ("BDB said", errval);
}
#endif

#define DB_ONLY
#else
#define DB_ONLY ATTR_UNUSED
#endif

/* The name of the tags database in the cache directory. */
#define TAGS_DB "tags.db"

/* The name of the version tag file in the cache directory. */
#define MOC_VERSION_TAG "moc_version_tag"

/* The maximum length of the version tag (including trailing NULL). */
#define VERSION_TAG_MAX 64

/* Number used to create cache version tag to detect incompatibilities
 * between cache version stored on the disk and MOC/BerkeleyDB environment.
 *
 * If you modify the DB structure, increase this number.  You can also
 * temporarily set it to zero to disable cache activity during structural
 * changes which require multiple commits.
 */
#define CACHE_DB_FORMAT_VERSION	3

/* How frequently to flush the tags database to disk.  A value of zero
 * disables flushing. */
#define DB_SYNC_COUNT 5

static std::vector<char> cache_record_serialize (const cache_record &rec)
{
	const auto &tags = rec.tags;
	size_t artist_len = tags.artist.length();
	size_t album_len = tags.album.length();
	size_t title_len = tags.title.length();

	size_t len = sizeof(rec.mod_time)
		+ artist_len + album_len + title_len + 3 /* null bytes */
		+ sizeof(tags.track)
		+ 1 /* rating */
		+ sizeof(tags.time);
	std::vector<char> buf(len);
	char *p = (char*)buf.data();

	memcpy (p, &rec.mod_time, sizeof(rec.mod_time)); p += sizeof(rec.mod_time);
	strcpy (p, tags.artist.c_str()); p += artist_len;
	strcpy (p, tags.album.c_str()); p += album_len;
	strcpy (p, tags.title.c_str()); p += title_len;
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
#ifdef HAVE_DB_H
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

void tags_cache::remove_rec (const char *fname)
{
	DBT key;
	int ret;

	assert (fname != NULL);

	debug ("Removing %s from the cache...", fname);

	memset (&key, 0, sizeof(key));
	key.data = (void *)fname;
	key.size = strlen (fname);

	ret = db->del (db, NULL, &key, 0);
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

#endif

/* Read the selected tags for this file and add it to the cache.
 * If client_id != -1, the server is notified using tags_response().
 * If client_id == -1, copy of file_tags is returned. */
file_tags tags_cache::read_add (const char *file, int client_id)
{
	assert (file != NULL);
	debug ("Getting tags for %s", file);

	cache_record rec;
	#ifdef HAVE_DB_H
	{
	DBT key, record;
	memset (&key, 0, sizeof (key));
	memset (&record, 0, sizeof (record));
	key.data = (void *) file;
	key.size = strlen (file);
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
			return rec.tags;
		}
		else 
		{
			debug ("Tags in the cache are outdated");
		}
	}

	rec.tags.read_file_tags(file);
	rec.mod_time = current_mtime;
	add (key, rec);
	}
	#else
	rec.tags = read_missing_tags (file, tags, tags_sel);
	#endif

	if (client_id != -1) tags_response (client_id, file, &rec.tags);

	return rec.tags;
}

void tags_cache::ratings_changed(const char *file, int rating)
{
	#ifdef HAVE_DB_H
	assert (file != NULL);
	debug ("Updating tags for %s", file);

	cache_record rec;
	DBT key, record;
	memset (&key, 0, sizeof (key));
	memset (&record, 0, sizeof (record));
	key.data = (void *) file;
	key.size = strlen (file);
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

	#endif
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
			str file = q.front(); q.pop();
			UNLOCK (c->mutex);
			c->read_add (file.c_str(), client);
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
#ifdef HAVE_DB_H
	, db_env(NULL)
	, db(NULL)
#endif
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

	#ifdef HAVE_DB_H
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
	#endif

	int rc = pthread_join (reader_thread_id, NULL);
	if (rc != 0) fatal ("pthread_join() on cache reader thread failed: %s", xstrerror (rc));

	rc = pthread_mutex_destroy (&mutex);
	if (rc != 0) log_errno ("Can't destroy mutex", rc);
	rc = pthread_cond_destroy (&request_cond);
	if (rc != 0) log_errno ("Can't destroy request_cond", rc);
}

void tags_cache::add_request (const char *file, int client_id)
{
	assert (file != NULL);
	assert (LIMIT(client_id, CLIENTS_MAX));

	debug ("Request for tags for '%s' from client %d", file, client_id);

	#ifdef HAVE_DB_H
	{
	DBT key, record;
	memset (&key, 0, sizeof (key));
	key.data = (void *) file;
	key.size = strlen (file);
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
	#endif

	LOCK (mutex);
	queues[client_id].push(file);
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

/* Remove all pending requests from the queue for the given client up to
 * the request associated with the given file. */
void tags_cache::clear_up_to (const char *file, int client_id)
{
	assert (LIMIT(client_id, CLIENTS_MAX));
	assert (file != NULL);

	LOCK (mutex);
	debug ("Removing requests for client %d up to file %s", client_id, file);
	auto &q = queues[client_id];
	while (!q.empty()) {
		if (q.front() == file) break;
		q.pop();
	}
	UNLOCK (mutex);
}

/* Create a MOC/db version string.
 *
 * @param buf Output buffer (at least VERSION_TAG_MAX chars long)
 */
#ifdef HAVE_DB_H
static const char *create_version_tag (char *buf)
{
	int db_major, db_minor;

	db_version (&db_major, &db_minor, NULL);

	snprintf (buf, VERSION_TAG_MAX, "%d %d %d",
	          CACHE_DB_FORMAT_VERSION, db_major, db_minor);

	return buf;
}

/* Check version of the cache directory.  If it was created
 * using format not handled by this version of MOC, return 0. */
static int cache_version_matches (const char *cache_dir)
{
	char *fname = NULL;
	char disk_version_tag[VERSION_TAG_MAX];
	ssize_t rres;
	FILE *f;
	int compare_result = 0;

	fname = (char *)xmalloc (strlen (cache_dir) + sizeof (MOC_VERSION_TAG) + 1);
	sprintf (fname, "%s/%s", cache_dir, MOC_VERSION_TAG);

	f = fopen (fname, "r");
	if (!f) {
		logit ("No %s in cache directory", MOC_VERSION_TAG);
		free (fname);
		return 0;
	}

	rres = fread (disk_version_tag, 1, sizeof (disk_version_tag) - 1, f);
	if (rres == sizeof (disk_version_tag) - 1) {
		logit ("On-disk version tag too long");
	}
	else {
		char *ptr, cur_version_tag[VERSION_TAG_MAX];

		disk_version_tag[rres] = '\0';
		ptr = strrchr (disk_version_tag, '\n');
		if (ptr)
			*ptr = '\0';
		ptr = strrchr (disk_version_tag, ' ');
		if (ptr && ptr[1] == 'r')
			*ptr = '\0';

		create_version_tag (cur_version_tag);
		ptr = strrchr (cur_version_tag, '\n');
		if (ptr)
			*ptr = '\0';
		ptr = strrchr (cur_version_tag, ' ');
		if (ptr && ptr[1] == 'r')
			*ptr = '\0';

		compare_result = !strcmp (disk_version_tag, cur_version_tag);
	}

	fclose (f);
	free (fname);

	return compare_result;
}

static void write_cache_version (const char *cache_dir)
{
	char cur_version_tag[VERSION_TAG_MAX];
	char *fname = NULL;
	FILE *f;
	int rc;

	fname = (char *)xmalloc (strlen (cache_dir) + sizeof (MOC_VERSION_TAG) + 1);
	sprintf (fname, "%s/%s", cache_dir, MOC_VERSION_TAG);

	f = fopen (fname, "w");
	if (!f) {
		log_errno ("Error opening cache", errno);
		free (fname);
		return;
	}

	create_version_tag (cur_version_tag);
	rc = fwrite (cur_version_tag, strlen (cur_version_tag), 1, f);
	if (rc != 1)
		logit ("Error writing cache version tag: %d", rc);

	free (fname);
	fclose (f);
}

/* Make sure that the cache directory exists and clear it if necessary. */
static int prepare_cache_dir (const char *cache_dir)
{
	if (mkdir (cache_dir, 0700) == 0) {
		write_cache_version (cache_dir);
		return 1;
	}

	if (errno != EEXIST) {
		error_errno ("Failed to create directory for tags cache", errno);
		return 0;
	}

	if (!cache_version_matches (cache_dir)) {
		logit ("Tags cache directory is the wrong version, purging....");

		if (!purge_directory (cache_dir))
			return 0;
		write_cache_version (cache_dir);
	}

	return 1;
}
#endif

void tags_cache::load(const char *cache_dir)
{
	assert (cache_dir != NULL);

#ifdef HAVE_DB_H
	int ret;

	if (!prepare_cache_dir (cache_dir)) {
		error ("Can't prepare cache directory!");
		goto err;
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

	ret = db_env->open (db_env, cache_dir,
	                       DB_CREATE | DB_PRIVATE | DB_INIT_MPOOL |
	                       DB_THREAD | DB_INIT_LOCK, 0);
	if (ret) {
		error ("Can't open DB environment (%s): %s", cache_dir, db_strerror (ret));
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
#endif
}

/* Immediately read tags for a file bypassing the request queue. */
file_tags tags_cache::get_immediate (const char *file)
{
	assert (file != NULL);
	debug ("Immediate tags read for %s", file);

	if (!is_url (file))
		return read_add (file, -1);

	return file_tags();
}
