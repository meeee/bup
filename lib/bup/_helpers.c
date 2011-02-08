#include "bupsplit.h"
#include <Python.h>
#include <assert.h>
#include <stdint.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

static int istty = 0;

static PyObject *selftest(PyObject *self, PyObject *args)
{
    if (!PyArg_ParseTuple(args, ""))
	return NULL;
    
    return Py_BuildValue("i", !bupsplit_selftest());
}


static PyObject *blobbits(PyObject *self, PyObject *args)
{
    if (!PyArg_ParseTuple(args, ""))
	return NULL;
    return Py_BuildValue("i", BUP_BLOBBITS);
}


static PyObject *splitbuf(PyObject *self, PyObject *args)
{
    unsigned char *buf = NULL;
    int len = 0, out = 0, bits = -1;

    if (!PyArg_ParseTuple(args, "t#", &buf, &len))
	return NULL;
    out = bupsplit_find_ofs(buf, len, &bits);
    return Py_BuildValue("ii", out, bits);
}


static PyObject *bitmatch(PyObject *self, PyObject *args)
{
    unsigned char *buf1 = NULL, *buf2 = NULL;
    int len1 = 0, len2 = 0;
    int byte, bit;

    if (!PyArg_ParseTuple(args, "t#t#", &buf1, &len1, &buf2, &len2))
	return NULL;
    
    bit = 0;
    for (byte = 0; byte < len1 && byte < len2; byte++)
    {
	int b1 = buf1[byte], b2 = buf2[byte];
	if (b1 != b2)
	{
	    for (bit = 0; bit < 8; bit++)
		if ( (b1 & (0x80 >> bit)) != (b2 & (0x80 >> bit)) )
		    break;
	    break;
	}
    }
    
    return Py_BuildValue("i", byte*8 + bit);
}


static PyObject *firstword(PyObject *self, PyObject *args)
{
    unsigned char *buf = NULL;
    int len = 0;
    uint32_t v;

    if (!PyArg_ParseTuple(args, "t#", &buf, &len))
	return NULL;
    
    if (len < 4)
	return NULL;
    
    v = ntohl(*(uint32_t *)buf);
    return PyLong_FromUnsignedLong(v);
}


typedef struct {
    uint32_t high;
    unsigned char low;
} bits40_t;


static void to_bloom_address_bitmask4(const bits40_t *buf,
	const int nbits, uint64_t *v, unsigned char *bitmask)
{
    int bit;
    uint64_t raw, mask;

    mask = (1<<nbits) - 1;
    raw = (((uint64_t)ntohl(buf->high)) << 8) | buf->low;
    bit = (raw >> (37-nbits)) & 0x7;
    *v = (raw >> (40-nbits)) & mask;
    *bitmask = 1 << bit;
}

static void to_bloom_address_bitmask5(const uint32_t *buf,
	const int nbits, uint32_t *v, unsigned char *bitmask)
{
    int bit;
    uint32_t raw, mask;

    mask = (1<<nbits) - 1;
    raw = ntohl(*buf);
    bit = (raw >> (29-nbits)) & 0x7;
    *v = (raw >> (32-nbits)) & mask;
    *bitmask = 1 << bit;
}


#define BLOOM_SET_BIT(name, address, itype, otype) \
static void name(unsigned char *bloom, const void *buf, const int nbits)\
{\
    unsigned char bitmask;\
    otype v;\
    address((itype *)buf, nbits, &v, &bitmask);\
    bloom[16+v] |= bitmask;\
}
BLOOM_SET_BIT(bloom_set_bit4, to_bloom_address_bitmask4, bits40_t, uint64_t)
BLOOM_SET_BIT(bloom_set_bit5, to_bloom_address_bitmask5, uint32_t, uint32_t)


#define BLOOM_GET_BIT(name, address, itype, otype) \
static int name(const unsigned char *bloom, const void *buf, const int nbits)\
{\
    unsigned char bitmask;\
    otype v;\
    address((itype *)buf, nbits, &v, &bitmask);\
    return bloom[16+v] & bitmask;\
}
BLOOM_GET_BIT(bloom_get_bit4, to_bloom_address_bitmask4, bits40_t, uint64_t)
BLOOM_GET_BIT(bloom_get_bit5, to_bloom_address_bitmask5, uint32_t, uint32_t)


static PyObject *bloom_add(PyObject *self, PyObject *args)
{
    unsigned char *sha = NULL, *bloom = NULL;
    unsigned char *end;
    int len = 0, blen = 0, nbits = 0, k = 0;

    if (!PyArg_ParseTuple(args, "w#s#ii", &bloom, &blen, &sha, &len, &nbits, &k))
	return NULL;

    if (blen < 16+(1<<nbits) || len % 20 != 0)
	return NULL;

    if (k == 5)
    {
	if (nbits > 29)
	    return NULL;
	for (end = sha + len; sha < end; sha += 20/k)
	    bloom_set_bit5(bloom, sha, nbits);
    }
    else if (k == 4)
    {
	if (nbits > 37)
	    return NULL;
	for (end = sha + len; sha < end; sha += 20/k)
	    bloom_set_bit4(bloom, sha, nbits);
    }
    else
	return NULL;


    return Py_BuildValue("i", len/20);
}

static PyObject *bloom_contains(PyObject *self, PyObject *args)
{
    unsigned char *sha = NULL, *bloom = NULL;
    int len = 0, blen = 0, nbits = 0, k = 0;
    unsigned char *end;
    int steps;

    if (!PyArg_ParseTuple(args, "t#s#ii", &bloom, &blen, &sha, &len, &nbits, &k))
	return NULL;

    if (len != 20)
	return NULL;

    if (k == 5)
    {
	if (nbits > 29)
	    return NULL;
	for (steps = 1, end = sha + 20; sha < end; sha += 20/k, steps++)
	    if (!bloom_get_bit5(bloom, sha, nbits))
		return Py_BuildValue("Oi", Py_None, steps);
    }
    else if (k == 4)
    {
	if (nbits > 37)
	    return NULL;
	for (steps = 1, end = sha + 20; sha < end; sha += 20/k, steps++)
	    if (!bloom_get_bit4(bloom, sha, nbits))
		return Py_BuildValue("Oi", Py_None, steps);
    }
    else
	return NULL;

    return Py_BuildValue("Oi", Py_True, k);
}


static uint32_t _extract_bits(unsigned char *buf, int nbits)
{
    uint32_t v, mask;

    mask = (1<<nbits) - 1;
    v = ntohl(*(uint32_t *)buf);
    v = (v >> (32-nbits)) & mask;
    return v;
}
static PyObject *extract_bits(PyObject *self, PyObject *args)
{
    unsigned char *buf = NULL;
    int len = 0, nbits = 0;

    if (!PyArg_ParseTuple(args, "t#i", &buf, &len, &nbits))
	return NULL;
    
    if (len < 4)
	return NULL;
    
    return PyLong_FromUnsignedLong(_extract_bits(buf, nbits));
}


struct sha {
    unsigned char bytes[20];
};
struct idx {
    unsigned char *map;
    struct sha *cur;
    struct sha *end;
    uint32_t *cur_name;
    long bytes;
    int name_base;
};


static int _cmp_sha(const struct sha *sha1, const struct sha *sha2)
{
    int i;
    for (i = 0; i < 20; i++)
	if (sha1->bytes[i] != sha2->bytes[i])
	    return sha1->bytes[i] - sha2->bytes[i];
    return 0;
}


static void _fix_idx_order(struct idx **idxs, int *last_i)
{
    struct idx *idx;
    int low, mid, high, c = 0;

    idx = idxs[*last_i];
    if (idxs[*last_i]->cur >= idxs[*last_i]->end)
    {
	idxs[*last_i] = NULL;
	PyMem_Free(idx);
	--*last_i;
	return;
    }
    if (*last_i == 0)
	return;

    low = *last_i-1;
    mid = *last_i;
    high = 0;
    while (low >= high)
    {
	mid = (low + high) / 2;
	c = _cmp_sha(idx->cur, idxs[mid]->cur);
	if (c < 0)
	    high = mid + 1;
	else if (c > 0)
	    low = mid - 1;
	else
	    break;
    }
    if (c < 0)
	++mid;
    if (mid == *last_i)
	return;
    memmove(&idxs[mid+1], &idxs[mid], (*last_i-mid)*sizeof(struct idx *));
    idxs[mid] = idx;
}


static uint32_t _get_idx_i(struct idx *idx)
{
    if (idx->cur_name == NULL)
	return idx->name_base;
    return ntohl(*idx->cur_name) + idx->name_base;
}


static PyObject *merge_into(PyObject *self, PyObject *args)
{
    PyObject *ilist = NULL;
    unsigned char *fmap = NULL;
    struct sha *sha_ptr, *last = NULL;
    uint32_t *table_ptr, *name_ptr;
    struct idx **idxs = NULL;
    int flen = 0, bits = 0, i;
    uint32_t total, count, prefix;
    int num_i;
    int last_i;

    if (!PyArg_ParseTuple(args, "w#iIO", &fmap, &flen, &bits, &total, &ilist))
	return NULL;

    num_i = PyList_Size(ilist);
    idxs = (struct idx **)PyMem_Malloc(num_i * sizeof(struct idx *));

    for (i = 0; i < num_i; i++)
    {
	long len, sha_ofs, name_map_ofs;
	idxs[i] = (struct idx *)PyMem_Malloc(sizeof(struct idx));
	PyObject *itup = PyList_GetItem(ilist, i);
	if (!PyArg_ParseTuple(itup, "t#llli", &idxs[i]->map, &idxs[i]->bytes,
		    &len, &sha_ofs, &name_map_ofs, &idxs[i]->name_base))
	    return NULL;
	idxs[i]->cur = (struct sha *)&idxs[i]->map[sha_ofs];
	idxs[i]->end = &idxs[i]->cur[len];
	idxs[i]->cur_name = (uint32_t *)&idxs[i]->map[name_map_ofs];
    }
    table_ptr = (uint32_t *)&fmap[12];
    sha_ptr = (struct sha *)&table_ptr[1<<bits];
    name_ptr = (uint32_t *)&sha_ptr[total];

    last_i = num_i-1;
    count = 0;
    prefix = 0;
    while (last_i >= 0)
    {
	struct idx *idx;
	uint32_t new_prefix;
	if (count % 102424 == 0 && istty)
	    fprintf(stderr, "midx: writing %.2f%% (%d/%d)\r",
		    count*100.0/total, count, total);
	idx = idxs[last_i];
	new_prefix = _extract_bits((unsigned char *)idx->cur, bits);
	while (prefix < new_prefix)
	    table_ptr[prefix++] = htonl(count);
	if (last == NULL || _cmp_sha(last, idx->cur) != 0)
	{
	    memcpy(sha_ptr++, idx->cur, 20);
	    *name_ptr++ = htonl(_get_idx_i(idx));
	    last = idx->cur;
	}
	++idx->cur;
	if (idx->cur_name != NULL)
	    ++idx->cur_name;
	_fix_idx_order(idxs, &last_i);
	++count;
    }
    table_ptr[prefix] = htonl(count);

    PyMem_Free(idxs);
    return PyLong_FromUnsignedLong(count);
}


// I would have made this a lower-level function that just fills in a buffer
// with random values, and then written those values from python.  But that's
// about 20% slower in my tests, and since we typically generate random
// numbers for benchmarking other parts of bup, any slowness in generating
// random bytes will make our benchmarks inaccurate.  Plus nobody wants
// pseudorandom bytes much except for this anyway.
static PyObject *write_random(PyObject *self, PyObject *args)
{
    uint32_t buf[1024/4];
    int fd = -1, seed = 0, verbose = 0;
    ssize_t ret;
    long long len = 0, kbytes = 0, written = 0;

    if (!PyArg_ParseTuple(args, "iLii", &fd, &len, &seed, &verbose))
	return NULL;
    
    srandom(seed);
    
    for (kbytes = 0; kbytes < len/1024; kbytes++)
    {
	unsigned i;
	for (i = 0; i < sizeof(buf)/sizeof(buf[0]); i++)
	    buf[i] = random();
	ret = write(fd, buf, sizeof(buf));
	if (ret < 0)
	    ret = 0;
	written += ret;
	if (ret < (int)sizeof(buf))
	    break;
	if (verbose && kbytes/1024 > 0 && !(kbytes%1024))
	    fprintf(stderr, "Random: %lld Mbytes\r", kbytes/1024);
    }
    
    // handle non-multiples of 1024
    if (len % 1024)
    {
	unsigned i;
	for (i = 0; i < sizeof(buf)/sizeof(buf[0]); i++)
	    buf[i] = random();
	ret = write(fd, buf, len % 1024);
	if (ret < 0)
	    ret = 0;
	written += ret;
    }
    
    if (kbytes/1024 > 0)
	fprintf(stderr, "Random: %lld Mbytes, done.\n", kbytes/1024);
    return Py_BuildValue("L", written);
}


static PyObject *random_sha(PyObject *self, PyObject *args)
{
    static int seeded = 0;
    uint32_t shabuf[20/4];
    int i;
    
    if (!seeded)
    {
	assert(sizeof(shabuf) == 20);
	srandom(time(NULL));
	seeded = 1;
    }
    
    if (!PyArg_ParseTuple(args, ""))
	return NULL;
    
    memset(shabuf, 0, sizeof(shabuf));
    for (i=0; i < 20/4; i++)
	shabuf[i] = random();
    return Py_BuildValue("s#", shabuf, 20);
}


static PyObject *open_noatime(PyObject *self, PyObject *args)
{
    char *filename = NULL;
    int attrs, attrs_noatime, fd;
    if (!PyArg_ParseTuple(args, "s", &filename))
	return NULL;
    attrs = O_RDONLY;
#ifdef O_NOFOLLOW
    attrs |= O_NOFOLLOW;
#endif
#ifdef O_LARGEFILE
    attrs |= O_LARGEFILE;
#endif
    attrs_noatime = attrs;
#ifdef O_NOATIME
    attrs_noatime |= O_NOATIME;
#endif
    fd = open(filename, attrs_noatime);
    if (fd < 0 && errno == EPERM)
    {
	// older Linux kernels would return EPERM if you used O_NOATIME
	// and weren't the file's owner.  This pointless restriction was
	// relaxed eventually, but we have to handle it anyway.
	// (VERY old kernels didn't recognized O_NOATIME, but they would
	// just harmlessly ignore it, so this branch won't trigger)
	fd = open(filename, attrs);
    }
    if (fd < 0)
	return PyErr_SetFromErrnoWithFilename(PyExc_IOError, filename);
    return Py_BuildValue("i", fd);
}


static PyObject *fadvise_done(PyObject *self, PyObject *args)
{
    int fd = -1;
    long long ofs = 0;
    if (!PyArg_ParseTuple(args, "iL", &fd, &ofs))
	return NULL;
#ifdef POSIX_FADV_DONTNEED
    posix_fadvise(fd, 0, ofs, POSIX_FADV_DONTNEED);
#endif    
    return Py_BuildValue("");
}


static PyMethodDef faster_methods[] = {
    { "selftest", selftest, METH_VARARGS,
	"Check that the rolling checksum rolls correctly (for unit tests)." },
    { "blobbits", blobbits, METH_VARARGS,
	"Return the number of bits in the rolling checksum." },
    { "splitbuf", splitbuf, METH_VARARGS,
	"Split a list of strings based on a rolling checksum." },
    { "bitmatch", bitmatch, METH_VARARGS,
	"Count the number of matching prefix bits between two strings." },
    { "firstword", firstword, METH_VARARGS,
        "Return an int corresponding to the first 32 bits of buf." },
    { "bloom_contains", bloom_contains, METH_VARARGS,
	"Check if a bloom filter of 2^nbits bytes contains an object" },
    { "bloom_add", bloom_add, METH_VARARGS,
	"Add an object to a bloom filter of 2^nbits bytes" },
    { "extract_bits", extract_bits, METH_VARARGS,
	"Take the first 'nbits' bits from 'buf' and return them as an int." },
    { "merge_into", merge_into, METH_VARARGS,
	"Merges a bunch of idx and midx files into a single midx." },
    { "write_random", write_random, METH_VARARGS,
	"Write random bytes to the given file descriptor" },
    { "random_sha", random_sha, METH_VARARGS,
        "Return a random 20-byte string" },
    { "open_noatime", open_noatime, METH_VARARGS,
	"open() the given filename for read with O_NOATIME if possible" },
    { "fadvise_done", fadvise_done, METH_VARARGS,
	"Inform the kernel that we're finished with earlier parts of a file" },
    { NULL, NULL, 0, NULL },  // sentinel
};

PyMODINIT_FUNC init_helpers(void)
{
    Py_InitModule("_helpers", faster_methods);
    istty = isatty(2) || getenv("BUP_FORCE_TTY");
}
