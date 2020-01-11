#pragma once
#include "server.h"
# include <db.h>

// TODO: handle load() failing!

struct cache_record
{
	time_t mod_time; // last modification time of the file
	file_tags tags;
};

struct file_tags;
class tags_cache
{
public:
	tags_cache();
	~tags_cache();

	/* Request queue manipulation functions: */
	void clear_queue (int client_id);
	void clear_up_to (const char *file, int client_id);

	/* Cache DB manipulation functions: */
	void load (const str &cache_dir);
	void add_request (const char *file, int client_id);
	file_tags get_immediate (const char *file);
	void ratings_changed(const char *file, int rating);

private:
	DB_ENV *db_env;
	DB *db;
	u_int32_t locker;

	struct Lock
	{
		Lock(tags_cache &c, DBT &key);
		Lock(const Lock &) = delete;
		~Lock();
		DB_LOCK lock;
		DB_ENV *db_env;
	};
	friend struct Lock;

	void remove_rec(const char *fname);
	void sync();
	void add(DBT &key, const cache_record &rec);
	file_tags read_add(const char *file, int client_id);
	static void *reader_thread (void *cache_ptr);

	typedef std::queue<str> request_queue;
	request_queue queues[CLIENTS_MAX]; /* requests queues for each client */
	bool stop_reader_thread; /* request for stopping read thread (if non-zero) */
	pthread_cond_t request_cond; /* condition for signalizing new requests */
	pthread_mutex_t mutex; /* mutex for all above data (except db because it's thread-safe) */
	pthread_t reader_thread_id; /* tid of the reading thread */
};
