#include "fifo_buf.h"

fifo_buf::fifo_buf(size_t n)
	: size(n)
	, fill(0)
	, pos(0)
	, buf(new char[n])
{
}

fifo_buf::~fifo_buf()
{
	delete[] buf;
}

size_t fifo_buf::put(const char *data, size_t N)
{
	size_t n = 0;

	while (fill < size && n < N)
	{
		size_t i = (pos + fill) % size; // next free byte
		size_t k = std::min(N-n, (i >= pos ? size : pos) - i);

		memcpy(buf + i, data + n, k);
		fill += k;
		n    += k;
	}

	return n;
}

size_t fifo_buf::get(char *data, size_t N)
{
	size_t n = 0;

	while (fill && n < N)
	{
		size_t k = pos + fill <= size ? fill : size - pos;
		if (k > N-n) k = N-n;

		memcpy(data, buf + pos, k);
		data += k;
		n += k;

		fill -= k;
		pos  += k;
		pos  %= size;
	}

	return n;
}

size_t fifo_buf::peek(char *data, size_t N)
{
	size_t fill = this->fill, pos = this->pos;

	// same as get() now
	size_t n = 0;

	while (fill && n < N)
	{
		size_t k = pos + fill <= size ? fill : size - pos;
		if (k > N-n) k = N-n;

		memcpy(data, buf + pos, k);
		data += k;
		n += k;

		fill -= k;
		pos  += k;
		pos  %= size;
	}

	return n;
}

