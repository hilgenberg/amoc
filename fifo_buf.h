#pragma once

class fifo_buf
{
public:
	fifo_buf(size_t size);
	fifo_buf(const fifo_buf &) = delete;	
	~fifo_buf();

	void clear() { fill = pos = 0; }

	// these return number of bytes actually put/got
	size_t put (const char *data, size_t size);
	size_t peek(char *data, size_t size);
	size_t get (char *data, size_t size);

	size_t get_space() const { return size - fill; }
	size_t get_fill()  const { return fill; }
	size_t get_size()  const { return size; }

private:
	size_t size, fill, pos;
	char  *buf;
};

