#include "tags_db.h"
#include <dirent.h>
#include <sys/stat.h>

#define TAGS_DB_FILE "tags.db"
#define TAGS_INFO_FILE "tags.version"

/* Number used to create cache version tag to detect incompatibilities
 * between cache version stored on the disk and MOC/BerkeleyDB environment.
 * If you modify the DB structure, increase this number. */
#define CACHE_DB_FORMAT_VERSION	4

/* How frequently to flush the tags database to disk.  A value of zero
 * disables flushing. */
#define DB_SYNC_COUNT 5

#undef  STRERROR_FN
#define STRERROR_FN bdb_strerror
static inline char *bdb_strerror (int errnum)
{
	return errnum > 0 ? xstrerror(errnum) : xstrdup(db_strerror (errnum));
}
#ifndef NDEBUG
static void db_err_cb (const DB_ENV *, const char *errpfx, const char *msg)
{
	assert (msg);
	if (errpfx && errpfx[0])
		logit ("BDB said: %s: %s", errpfx, msg);
	else
		logit ("BDB said: %s", msg);
}
static void db_msg_cb (const DB_ENV *, const char *msg)
{
	assert (msg);
	logit ("BDB said: %s", msg);
}
static void db_panic_cb (DB_ENV *, int errval)
{
	log_errno ("BDB said", errval);
}
#endif

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

tags_db::Lock::Lock(tags_db &db, const str &k)
: db_env(db.db_env)
{
	DBT key; memset(&key, 0, sizeof(key));
	key.data = (void *) k.c_str();
	key.size = k.length();

	int rc = db_env->lock_get (db.db_env, db.locker, 0, &key, DB_LOCK_WRITE, &lock);
	if (rc) fatal ("Can't get DB lock: %s", db_strerror (rc));
}
tags_db::Lock::~Lock()
{
	int rc = db_env->lock_put (db_env, &lock);
	if (rc) fatal ("Can't release DB lock: %s", db_strerror (rc));
}

tags_db::Lock tags_db::lock(const str &key)
{
	return Lock(*this, key);
}

cache_record tags_db::get(const str &k)
{
	debug ("Getting tags for %s", k.c_str());

	DBT key; memset(&key, 0, sizeof(key));
	key.data = (void *) k.c_str();
	key.size = k.length();

	DBT val; memset (&val, 0, sizeof(val));
	val.flags = DB_DBT_MALLOC;

	cache_record rec;
	int ret = db->get(db, NULL, &key, &val, 0);
	if (ret == DB_NOTFOUND)
	{
		debug("Tags not found");
		rec.mod_time = -1;
		return rec;
	}
	if (ret)
	{
		log_errno ("Cache DB get error", ret);
		throw std::runtime_error("Cache DB get error");
	}

	bool ok = cache_record_deserialize(rec, (const char*)val.data, val.size);
	free(val.data);
	if (!ok) throw std::runtime_error("Cache DB deserialization error");
	return rec;
}

void tags_db::add(const str &k, const cache_record &rec)
{
	debug ("Adding/updating cache object");

	DBT key; memset (&key, 0, sizeof (key));
	key.data = (void *) k.c_str();
	key.size = k.length();

	DBT val; memset (&val, 0, sizeof(val));
	auto buf = cache_record_serialize (rec);
	val.data = buf.data();
	val.size = buf.size();

	int ret = db->put (db, NULL, &key, &val, 0);
	if (ret) error_errno ("DB put error", ret);

	sync();
}

void tags_db::remove(const str &k)
{
	debug ("Removing %s from the cache...", k.c_str());

	DBT key;
	memset (&key, 0, sizeof(key));
	key.data = (void*)k.c_str();
	key.size = k.length();

	int ret = db->del(db, NULL, &key, 0);
	if (ret) logit ("Can't remove item for %s from the cache: %s", k.c_str(), db_strerror (ret));
	sync();
}

/* Synchronize cache every DB_SYNC_COUNT updates. */
void tags_db::sync ()
{
	static int sync_count = 0;
	if (DB_SYNC_COUNT == 0) return;
	if (++sync_count >= DB_SYNC_COUNT) {
		sync_count = 0;
		db->sync (db, 0);
	}
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
static bool cache_version_matches ()
{
	str fname = options::run_file_path(TAGS_INFO_FILE);
	FILE *f = fopen(fname.c_str(), "r");
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

tags_db::tags_db()
: db(NULL), db_env(NULL), locker(0)
{
	int ret;

	if (!cache_version_matches()) {
		logit ("Preparing new tags cache....");
		if (!file_delete(options::run_file_path(TAGS_INFO_FILE)) ||
		    !file_delete(options::run_file_path(TAGS_DB_FILE)))
		{
			error ("Deleting old files failed!");
			goto err;
		}

		str p = options::run_file_path(TAGS_INFO_FILE);
		FILE *f = fopen (p.c_str(), "w");
		if (!f) {
			log_errno ("Error writing cache info file", errno);
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
	//db_env->set_msgcall (db_env, db_msg_cb);
	ret = db_env->set_paniccall (db_env, db_panic_cb);
	if (ret) logit ("Could not set DB panic callback");
	#endif

	ret = db_env->open (db_env, options::RunDir.c_str(),
	      DB_CREATE | DB_PRIVATE | DB_INIT_MPOOL | DB_THREAD | DB_INIT_LOCK, 0);
	if (ret) {
		error ("Can't open DB environment (%s): %s", options::RunDir.c_str(), db_strerror (ret));
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
	//db->set_msgcall (db, db_msg_cb);
	ret = db->set_paniccall (db, db_panic_cb);
	if (ret) logit ("Could not set DB panic callback");
	#endif

	ret = db->open (db, NULL, options::run_file_path(TAGS_DB_FILE).c_str(), NULL, DB_BTREE, DB_CREATE | DB_THREAD, 0);
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
	throw std::runtime_error("Failed to initialise tags cache");
}

tags_db::~tags_db()
{
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
}