#pragma once
#include "../file_tags.h"
#include <db.h>

struct cache_record
{
	operator bool() const { return mod_time != -1; }

	time_t mod_time; // last modification time of the file
	file_tags tags;
};

class tags_db
{
public:
	tags_db();
	~tags_db();

	void add(const str &key, const cache_record &rec);
	cache_record get(const str &key); // mod_time -1 if not found
	void remove(const str &key);
	void sync();

	struct Lock
	{
		Lock(Lock &&) = default;
		Lock(const Lock &) = delete;
		Lock(tags_db &db, const str &k);
		~Lock();
		DB_LOCK lock;
		DB_ENV *db_env;
	};
	friend struct Lock;

	Lock lock(const str &key);

private:
	DB_ENV *db_env;
	DB     *db;
	u_int32_t locker;
};
