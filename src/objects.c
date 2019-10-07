#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Expose struct timespec atime, mtime in struct stat as st_[am]timespec. On
 * BSD and OS X, this is the native name. On Glibc, alias.
 */
#if defined(__GLIBC__)
# define st_mtimespec st_mtim
# define st_atimespec st_atim
#endif

#include <hadoofus/highlevel.h>

#include "heapbuf.h"
#include "heapbufobjs.h"
#include "objects-internal.h"
#include "rpc2-internal.h"
#include "util.h"

#include "hadoop_rpc2_0.pb-c.h"
#include "IpcConnectionContext.pb-c.h"
#include "ProtobufRpcEngine.pb-c.h"
#include "RpcHeader.pb-c.h"
#include "RpcPayloadHeader2_0.pb-c.h"

_Static_assert(_H_INVALID < H_PROTOCOL_EXCEPTION, "overlapping type");

static struct {
	const char *type;
	bool objtype;
	struct hdfs_object *(*slurper)(struct hdfs_heap_buf *);
} object_types[] = {
	[H_VOID - _H_START] = { .type = VOID_TYPE, .objtype = false,
		.slurper = _oslurp_null, },
	[H_NULL - _H_START] = { .type = NULL_TYPE1, .objtype = false,
		.slurper = _oslurp_null, },
	[H_BOOLEAN - _H_START] = { .type = BOOLEAN_TYPE, .objtype = false,
		.slurper = _oslurp_boolean, },
	[H_INT - _H_START] = { .type = INT_TYPE, .objtype = false,
		.slurper = _oslurp_int, },
	[H_LONG - _H_START] = { .type = LONG_TYPE, .objtype = false,
		.slurper = _oslurp_long, },
	[H_ARRAY_LONG - _H_START] = { .type = ARRAYLONG_TYPE, .objtype = false,
		.slurper = _oslurp_array_long, },
	[H_LOCATED_BLOCK - _H_START] = { .type = LOCATEDBLOCK_TYPE, .objtype = true,
		.slurper = _oslurp_located_block, },
	[H_LOCATED_BLOCKS - _H_START] = { .type = LOCATEDBLOCKS_TYPE, .objtype = true,
		.slurper = _oslurp_located_blocks, },
	[H_DIRECTORY_LISTING - _H_START] = { .type = DIRECTORYLISTING_TYPE, .objtype = true,
		.slurper = _oslurp_directory_listing, },
	[H_DATANODE_INFO - _H_START] = { .type = DATANODEINFO_TYPE, .objtype = true,
		.slurper = _oslurp_datanode_info, },
	[H_ARRAY_DATANODE_INFO - _H_START] = { .type = ARRAYDATANODEINFO_TYPE, .objtype = false,
		.slurper = _oslurp_array_datanode_info, },
	[H_FILE_STATUS - _H_START] = { .type = FILESTATUS_TYPE, .objtype = true,
		.slurper = _oslurp_file_status, },
	[H_CONTENT_SUMMARY - _H_START] = { .type = CONTENTSUMMARY_TYPE, .objtype = true,
		.slurper = _oslurp_content_summary, },
	[H_LOCATED_DIRECTORY_LISTING - _H_START] = { .type = LOCATEDDIRECTORYLISTING_TYPE,
		.objtype = true, .slurper = /*_oslurp_located_directory_listing*/NULL, },
	[H_UPGRADE_STATUS_REPORT - _H_START] = { .type = UPGRADESTATUSREPORT_TYPE, .objtype = true,
		.slurper = _oslurp_upgrade_status_report, },
	[H_BLOCK - _H_START] = { .type = BLOCK_TYPE, .objtype = true,
		.slurper = _oslurp_block, },
	[H_ARRAY_BYTE - _H_START] = { .type = ARRAYBYTE_TYPE, .objtype = false,
		.slurper = /*_oslurp_array_byte*/NULL, },
	[H_TOKEN - _H_START] = { .type = TOKEN_TYPE, .objtype = true,
		.slurper = /*_oslurp_token*/NULL, },
	[H_STRING - _H_START] = { .type = STRING_TYPE, .objtype = false },
	[H_FSPERMS - _H_START] = { .type = FSPERMS_TYPE, .objtype = true,
		.slurper = _oslurp_fsperms, },
	[H_SHORT - _H_START] = { .type = SHORT_TYPE, .objtype = false,
		.slurper = _oslurp_short, },
	[H_ARRAY_STRING - _H_START] = { .type = ARRAYSTRING_TYPE, .objtype = false,
		.slurper = /*_oslurp_array_string*/NULL, },
	[H_TEXT - _H_START] = { .type = TEXT_TYPE, .objtype = true,
		.slurper = /*_oslurp_text*/NULL, },
	[H_SAFEMODEACTION - _H_START] = { .type = SAFEMODEACTION_TYPE, .objtype = false,
		.slurper = /*_oslurp_safemodeaction*/NULL, },
	[H_DNREPORTTYPE - _H_START] = { .type = DNREPORTTYPE_TYPE, .objtype = false,
		.slurper = /*_oslurp_dnreporttype*/NULL, },
	[H_ARRAY_LOCATEDBLOCK - _H_START] = { .type = ARRAYLOCATEDBLOCK_TYPE, .objtype = false,
		.slurper = /*_oslurp_array_locatedblock*/NULL, },
	[H_UPGRADE_ACTION - _H_START] = { .type = UPGRADEACTION_TYPE, .objtype = false,
		.slurper = /*_oslurp_upgrade_action*/NULL, },
};

static struct {
	const char *type;
} exception_types[] = {
	[0] = { .type = NULL, },
	[H_ACCESS_CONTROL_EXCEPTION - _H_EXCEPTION_START] = {
		.type = ACCESS_EXCEPTION_STR, },
	[H_ALREADY_BEING_CREATED_EXCEPTION - _H_EXCEPTION_START] = {
		.type = ALREADY_BEING_EXCEPTION_STR, },
	[H_FILE_NOT_FOUND_EXCEPTION - _H_EXCEPTION_START] = {
		.type = NOT_FOUND_EXCEPTION_STR, },
	[H_IO_EXCEPTION - _H_EXCEPTION_START] = {
		.type = IO_EXCEPTION_STR, },
	[H_LEASE_EXPIRED_EXCEPTION - _H_EXCEPTION_START] = {
		.type = LEASE_EXCEPTION_STR, },
	[H_SECURITY_EXCEPTION - _H_EXCEPTION_START] = {
		.type = SECURITY_EXCEPTION_STR, },
	[H_QUOTA_EXCEPTION - _H_EXCEPTION_START] = {
		.type = QUOTA_EXCEPTION_STR, },
	[H_ILLEGAL_ARGUMENT_EXCEPTION - _H_EXCEPTION_START] = {
		.type = ILLARG_EXCEPTION_STR, },
	[H_INVALID_TOKEN_EXCEPTION - _H_EXCEPTION_START] = {
		.type = INVTOK_EXCEPTION_STR, },
	[H_INVALID_PATH_EXCEPTION - _H_EXCEPTION_START] = {
		.type = INVPATH_EXCEPTION_STR, },
	[H_FILE_ALREADY_EXISTS_EXCEPTION - _H_EXCEPTION_START] = {
		.type = EEXIST_EXCEPTION_STR, },
	[H_IPC_EXCEPTION - _H_EXCEPTION_START] = {
		.type = IPC_EXCEPTION_STR, },
	[H_SASL_EXCEPTION - _H_EXCEPTION_START] = {
		.type = SASL_EXCEPTION_STR, },
	[H_RPC_EXCEPTION - _H_EXCEPTION_START] = {
		.type = RPC_EXCEPTION_STR, },
	[H_RPC_NO_SUCH_METHOD_EXCEPTION - _H_EXCEPTION_START] = {
		.type = RPC_ENOENT_EXCEPTION_STR, },
};

enum hdfs_object_type
_string_to_type(const char *otype)
{
	for (unsigned i = 0; i < nelem(object_types); i++)
		if (streq(otype, object_types[i].type))
			return _H_START + i;

	return _H_INVALID;
}

static enum hdfs_exception_type
_string_to_etype(const char *etype)
{
	for (unsigned i = 1/*skip proto exception; never matches*/;
	    i < nelem(exception_types); i++)
		if (streq(etype, exception_types[i].type))
			return _H_EXCEPTION_START + i;

	return H_BASE_PROTOCOL_EXCEPTION;
}

EXPORT_SYM const char *
hdfs_etype_to_string(enum hdfs_exception_type e)
{
	const char *res;

	ASSERT(e >= _H_EXCEPTION_START && e < _H_EXCEPTION_END);
	res = exception_types[e - _H_EXCEPTION_START].type;
	if (!res)
		return "ProtocolException";
	return res;
}

static struct hdfs_object *
_object_exception(const char *etype, const char *emsg)
{
	enum hdfs_exception_type realtype;

	realtype = _string_to_etype(etype);
	return hdfs_protocol_exception_new(realtype, emsg);
}

static inline int64_t
_timespec_to_ms(struct timespec ts)
{
	return ((int64_t)ts.tv_sec * 1000UL) +
	    (((int64_t)ts.tv_nsec / 1000000ULL) % 1000UL);
}

static void *
_objmalloc(void)
{
	void *r = malloc(sizeof(struct hdfs_object));
	ASSERT(r);
	memset(r, 0, sizeof(struct hdfs_object));
	return r;
}

EXPORT_SYM struct hdfs_object *
hdfs_void_new()
{
	struct hdfs_object *r = _objmalloc();
	r->ob_type = H_VOID;
	return r;
}

EXPORT_SYM struct hdfs_object *
hdfs_null_new(enum hdfs_object_type type)
{
	struct hdfs_object *r = _objmalloc();
	r->ob_type = H_NULL;
	r->ob_val._null._type = type;
	return r;
}

EXPORT_SYM struct hdfs_object *
hdfs_boolean_new(bool val)
{
	struct hdfs_object *r = _objmalloc();
	r->ob_type = H_BOOLEAN;
	r->ob_val._boolean._val = val;
	return r;
}

struct hdfs_object *
_hdfs_boolean_new_proto(protobuf_c_boolean val)
{

	return hdfs_boolean_new(val);
}

EXPORT_SYM struct hdfs_object *
hdfs_short_new(int16_t val)
{
	struct hdfs_object *r = _objmalloc();
	r->ob_type = H_SHORT;
	r->ob_val._short._val = val;
	return r;
}

EXPORT_SYM struct hdfs_object *
hdfs_int_new(int32_t val)
{
	struct hdfs_object *r = _objmalloc();
	r->ob_type = H_INT;
	r->ob_val._int._val = val;
	return r;
}

EXPORT_SYM struct hdfs_object *
hdfs_long_new(int64_t val)
{
	struct hdfs_object *r = _objmalloc();
	r->ob_type = H_LONG;
	r->ob_val._long._val = val;
	return r;
}

EXPORT_SYM struct hdfs_object *
hdfs_token_new_nulsafe(const uint8_t *id, size_t idlen, const uint8_t *pw,
    size_t pwlen, const char *kind, const char *service)
{
	struct hdfs_object *r = _objmalloc();
	uint8_t *id_copy, *pw_copy;
	char *kind_copy, *service_copy;

	id_copy = malloc(idlen + 1);
	pw_copy = malloc(pwlen + 1);
	kind_copy = strdup(kind);
	service_copy = strdup(service);

	ASSERT(id_copy);
	ASSERT(pw_copy);
	ASSERT(kind_copy);
	ASSERT(service_copy);

	memcpy(id_copy, id, idlen);
	id_copy[idlen] = '\0'; // null-terminate for safety
	memcpy(pw_copy, pw, pwlen);
	pw_copy[pwlen] = '\0';

	r->ob_type = H_TOKEN;
	r->ob_val._token = (struct hdfs_token) {
		._id = id_copy,
		._id_len = idlen,
		._pw = pw_copy,
		._pw_len = pwlen,
		._kind = kind_copy,
		._service = service_copy
	};
	return r;
}

EXPORT_SYM struct hdfs_object *
hdfs_token_new(const char *s1, const char *s2, const char *s3, const char *s4)
{

	return hdfs_token_new_nulsafe((const uint8_t *)s1, strlen(s1),
	    (const uint8_t *)s2, strlen(s2), s3, s4);
}

EXPORT_SYM struct hdfs_object *
hdfs_token_new_empty()
{
	struct hdfs_object *r = _objmalloc();
	uint8_t *id_empty, *pw_empty;
	char *kind_empty, *service_empty;

	id_empty = (uint8_t *)strdup("");
	pw_empty = (uint8_t *)strdup("");
	kind_empty = strdup("");
	service_empty = strdup("");

	ASSERT(id_empty);
	ASSERT(pw_empty);
	ASSERT(kind_empty);
	ASSERT(service_empty);

	r->ob_type = H_TOKEN;
	r->ob_val._token = (struct hdfs_token) {
		._id = id_empty, // XXX consider NULL
		._id_len = 0,
		._pw = pw_empty, // XXX consider NULL
		._pw_len = 0,
		._kind = kind_empty,
		._service = service_empty,
	};
	return r;
}

struct hdfs_object *
_hdfs_token_new_proto(Hadoop__Common__TokenProto *pr)
{

	return hdfs_token_new_nulsafe((void *)pr->identifier.data,
	    pr->identifier.len, (void *)pr->password.data, pr->password.len,
	    pr->kind, pr->service);
}

EXPORT_SYM struct hdfs_object *
hdfs_token_copy(struct hdfs_object *src)
{

	ASSERT(src);
	ASSERT(src->ob_type == H_TOKEN);

	return hdfs_token_new_nulsafe(src->ob_val._token._id,
	    src->ob_val._token._id_len,
	    src->ob_val._token._pw,
	    src->ob_val._token._pw_len,
	    src->ob_val._token._kind,
	    src->ob_val._token._service);
}

EXPORT_SYM struct hdfs_object *
hdfs_array_long_new(int len, const int64_t *values)
{
	struct hdfs_object *r = _objmalloc();
	int64_t *values_copied = malloc(len * sizeof(int64_t));
	ASSERT(values_copied);
	memcpy(values_copied, values, len * sizeof(int64_t));

	r->ob_type = H_ARRAY_LONG;
	r->ob_val._array_long = (struct hdfs_array_long) {
		._len = len,
		._vals = values_copied,
	};
	return r;
}

EXPORT_SYM struct hdfs_object *
hdfs_located_block_new(int64_t blkid, int64_t len, int64_t generation, int64_t offset)
{
	struct hdfs_object *r = _objmalloc();
	r->ob_type = H_LOCATED_BLOCK;
	r->ob_val._located_block = (struct hdfs_located_block) {
		._blockid = blkid,
		._len = len,
		._generation = generation,
		._offset = offset,
		._token = hdfs_token_new_empty(),
	};
	return r;
}

struct hdfs_object *
_hdfs_located_block_new_proto(Hadoop__Hdfs__LocatedBlockProto *lb)
{
	struct hdfs_object *res;
	size_t i;

	ASSERT(lb);
	ASSERT(lb->b);

	res = hdfs_located_block_new(lb->b->blockid, 0, lb->b->generationstamp,
	    lb->offset);

	if (lb->b->has_numbytes)
		res->ob_val._located_block._len = lb->b->numbytes;

	res->ob_val._located_block._pool_id = strdup(lb->b->poolid);
	ASSERT(res->ob_val._located_block._pool_id);

	res->ob_val._located_block._corrupt = lb->corrupt;
	// XXX TODO The above hdfs_located_block_new() allocates _token with
	// hdfs_token_new_empty(), so we need to free that here before the
	// _hdfs_token_new_proto() call, or we'll leak memory. This allocation just to
	// immediately free is wasteful, so TODO change this to something smarter
	hdfs_object_free(res->ob_val._located_block._token);
	res->ob_val._located_block._token =
	    _hdfs_token_new_proto(lb->blocktoken);

	ASSERT(lb->n_locs <= INT_MAX);
	for (i = 0; i < lb->n_locs; i++) {
		struct hdfs_object *dni;

		dni = _hdfs_datanode_info_new_proto(lb->locs[i]);
		hdfs_located_block_append_datanode_info(res, dni);
	}
	return res;
}

EXPORT_SYM struct hdfs_object *
hdfs_located_block_copy(struct hdfs_object *src)
{
	int nlocs;
	struct hdfs_object *r = _objmalloc(),
			   **arr_locs = NULL;
	char *pool_id;

	ASSERT(src);
	ASSERT(src->ob_type == H_LOCATED_BLOCK);

	if (src->ob_val._located_block._pool_id) {
		pool_id = strdup(src->ob_val._located_block._pool_id);
		ASSERT(pool_id);
	} else
		pool_id = NULL;

	nlocs = src->ob_val._located_block._num_locs;

	if (nlocs > 0) {
		arr_locs = malloc(nlocs * sizeof *arr_locs);
		ASSERT(arr_locs);


		for (int i = 0; i < nlocs; i++)
			arr_locs[i] = hdfs_datanode_info_copy(
			    src->ob_val._located_block._locs[i]);
	}

	r->ob_type = H_LOCATED_BLOCK;
	r->ob_val._located_block = (struct hdfs_located_block) {
		._blockid = src->ob_val._located_block._blockid,
		._len = src->ob_val._located_block._len,
		._generation = src->ob_val._located_block._generation,
		._num_locs = nlocs,
		._locs = arr_locs,
		._offset = src->ob_val._located_block._offset,
		._token = hdfs_token_copy(src->ob_val._located_block._token),
		._pool_id = pool_id,
	};
	return r;
}

EXPORT_SYM struct hdfs_object *
hdfs_located_blocks_new(bool beingcreated, int64_t size)
{
	struct hdfs_object *r = _objmalloc();
	r->ob_type = H_LOCATED_BLOCKS;
	r->ob_val._located_blocks = (struct hdfs_located_blocks) {
		._being_written = beingcreated,
		._size = size,
	};
	return r;
}

struct hdfs_object *
_hdfs_located_blocks_new_proto(Hadoop__Hdfs__LocatedBlocksProto *lb)
{
	struct hdfs_object *res;
	size_t i;

	ASSERT(lb);

	res = hdfs_located_blocks_new(lb->underconstruction, lb->filelength);
	if (lb->lastblock)
		res->ob_val._located_blocks._last_block =
		    _hdfs_located_block_new_proto(lb->lastblock);
	res->ob_val._located_blocks._last_block_complete =
	    lb->islastblockcomplete;

	for (i = 0; i < lb->n_blocks; i++) {
		struct hdfs_object *b;

		b = _hdfs_located_block_new_proto(lb->blocks[i]);
		hdfs_located_blocks_append_located_block(res, b);
	}

	return res;
}

static struct hdfs_object *
_hdfs_directory_listing_new(bool has_locations)
{
	struct hdfs_object *r = _objmalloc();
	r->ob_type = H_DIRECTORY_LISTING;
	r->ob_val._directory_listing._has_locations = has_locations;
	return r;
}

EXPORT_SYM struct hdfs_object *
hdfs_located_directory_listing_new()
{
	return _hdfs_directory_listing_new(true/*locations*/);
}

EXPORT_SYM struct hdfs_object *
hdfs_directory_listing_new()
{
	return _hdfs_directory_listing_new(false/*locations*/);
}

struct hdfs_object *
_hdfs_directory_listing_new_proto(Hadoop__Hdfs__DirectoryListingProto *list)
{
	struct hdfs_object *res;
	bool has_locations;
	size_t i;

	ASSERT(list);

	has_locations = false;
	for (i = 0; i < list->n_partiallisting; i++) {
		if (list->partiallisting[i]->locations != NULL) {
			has_locations = true;
			break;
		}
	}

	res = _hdfs_directory_listing_new(has_locations);
	for (i = 0; i < list->n_partiallisting; i++) {
		Hadoop__Hdfs__HdfsFileStatusProto *fs;
		struct hdfs_object *fso, *lbo;

		fs = list->partiallisting[i];
		fso = _hdfs_file_status_new_proto(fs);

		lbo = NULL;
		if (has_locations) {
			ASSERT(fs->locations);
			lbo = _hdfs_located_blocks_new_proto(fs->locations);
		}

		hdfs_directory_listing_append_file_status(res, fso, lbo);
	}

	res->ob_val._directory_listing._remaining_entries =
	    list->remainingentries;
	return res;
}

EXPORT_SYM struct hdfs_object *
hdfs_datanode_info_new(const char *ipaddr, const char *host, const char *port, const char *rack,
	uint16_t namenodeport) // "/default-rack"
{
	char *rack_copy = strdup(rack),
	     *ipaddr_copy = strdup(ipaddr),
	     *host_copy = strdup(host),
	     *port_copy = strdup(port);
	struct hdfs_object *r = _objmalloc();

	ASSERT(rack_copy);
	ASSERT(ipaddr_copy);
	ASSERT(host_copy);
	ASSERT(port_copy);

	r->ob_type = H_DATANODE_INFO;
	r->ob_val._datanode_info = (struct hdfs_datanode_info) {
		._location = rack_copy,
		._ipaddr = ipaddr_copy,
		._hostname = host_copy,
		._port = port_copy,
		._namenodeport = namenodeport,
	};
	return r;
}

struct hdfs_object *
_hdfs_datanode_info_new_proto(Hadoop__Hdfs__DatanodeInfoProto *pr)
{
	char dn_port_str[14];

	sprintf(dn_port_str, "%u", pr->id->xferport);

	return hdfs_datanode_info_new(pr->id->ipaddr, pr->id->hostname,
	    dn_port_str, pr->location ? pr->location : "",
	    pr->id->ipcport);
}

EXPORT_SYM struct hdfs_object *
hdfs_datanode_info_copy(struct hdfs_object *src)
{
	struct hdfs_object *r = _objmalloc();
	char *rack_copy,
	     *ipaddr_copy,
	     *host_copy,
	     *port_copy;
	uint16_t namenodeport;

	ASSERT(src);
	ASSERT(src->ob_type == H_DATANODE_INFO);

	rack_copy = strdup(src->ob_val._datanode_info._location);
	ipaddr_copy = strdup(src->ob_val._datanode_info._ipaddr);
	host_copy = strdup(src->ob_val._datanode_info._hostname);
	port_copy = strdup(src->ob_val._datanode_info._port);
	namenodeport = src->ob_val._datanode_info._namenodeport;

	ASSERT(rack_copy);
	ASSERT(ipaddr_copy);
	ASSERT(host_copy);
	ASSERT(port_copy);

	r->ob_type = H_DATANODE_INFO;
	r->ob_val._datanode_info = (struct hdfs_datanode_info) {
		._location = rack_copy,
		._ipaddr = ipaddr_copy,
		._hostname = host_copy,
		._port = port_copy,
		._namenodeport = namenodeport,
	};
	return r;
}

EXPORT_SYM struct hdfs_object *
hdfs_array_datanode_info_new()
{
	struct hdfs_object *r = _objmalloc();
	r->ob_type = H_ARRAY_DATANODE_INFO;
	return r;
}

EXPORT_SYM struct hdfs_object *
hdfs_array_datanode_info_copy(struct hdfs_object *src)
{
	struct hdfs_object *r;
	int n;

	if (!src)
		return hdfs_null_new(H_ARRAY_DATANODE_INFO);
	if (src->ob_type == H_NULL) {
		ASSERT(src->ob_val._null._type == H_ARRAY_DATANODE_INFO);
		return hdfs_null_new(H_ARRAY_DATANODE_INFO);
	}

	r = _objmalloc();

	ASSERT(src->ob_type == H_ARRAY_DATANODE_INFO);
	n = src->ob_val._array_datanode_info._len;

	r->ob_type = H_ARRAY_DATANODE_INFO;
	r->ob_val._array_datanode_info._len = n;
	if (n > 0) {
		r->ob_val._array_datanode_info._values =
		    malloc(n * sizeof(struct hdfs_object *));
		ASSERT(r->ob_val._array_datanode_info._values);

		for (int i = 0; i < n; i++)
			r->ob_val._array_datanode_info._values[i] =
			    hdfs_datanode_info_copy(src->ob_val._array_datanode_info._values[i]);
	}

	return r;
}

EXPORT_SYM struct hdfs_object *
hdfs_file_status_new(const char *logical_name, const struct stat *sb,
    const char *owner, const char *group)
{
	struct hdfs_object *r = _objmalloc();
	char *name_copy = strdup(logical_name),
	     *owner_copy = strdup(owner),
	     *group_copy = strdup(group);
	int mode = (S_ISDIR(sb->st_mode))? (sb->st_mode & 0777) :
	    (sb->st_mode & 0666);

	ASSERT(name_copy);
	ASSERT(owner_copy);
	ASSERT(group_copy);

	r->ob_type = H_FILE_STATUS;
	r->ob_val._file_status = (struct hdfs_file_status) {
		._file = name_copy,
		._size = sb->st_size,
		._directory = S_ISDIR(sb->st_mode),
		._replication = !S_ISDIR(sb->st_mode), // 1 for files, 0 for dir
		._block_size = 64*1024*1024,
		._mtime = _timespec_to_ms(sb->st_mtimespec),
		._atime = _timespec_to_ms(sb->st_atimespec),
		._permissions = mode,
		._owner = owner_copy,
		._group = group_copy,
	};

	return r;
}

EXPORT_SYM struct hdfs_object *
hdfs_file_status_new_ex(const char *logical_name, int64_t size, bool directory,
	int replication, int64_t block_size, int64_t mtime_ms, int64_t atime_ms,
	int perms, const char *owner, const char *group)
{
	struct hdfs_object *r = _objmalloc();
	char *name_copy = strdup(logical_name),
	     *owner_copy = strdup(owner),
	     *group_copy = strdup(group);

	ASSERT(name_copy);
	ASSERT(owner_copy);
	ASSERT(group_copy);

	r->ob_type = H_FILE_STATUS;
	r->ob_val._file_status = (struct hdfs_file_status) {
		._file = name_copy,
		._size = size,
		._directory = directory,
		._replication = replication,
		._block_size = block_size,
		._mtime = mtime_ms,
		._atime = atime_ms,
		._permissions = perms,
		._owner = owner_copy,
		._group = group_copy,
	};
	return r;
}

enum hdfs_file_type
_hdfs_file_type_from_proto(Hadoop__Hdfs__HdfsFileStatusProto__FileType pr)
{

	ASSERT((unsigned)HADOOP__HDFS__HDFS_FILE_STATUS_PROTO__FILE_TYPE__IS_DIR == HDFS_FT_DIR);
	ASSERT((unsigned)HADOOP__HDFS__HDFS_FILE_STATUS_PROTO__FILE_TYPE__IS_FILE == HDFS_FT_FILE);
	ASSERT((unsigned)HADOOP__HDFS__HDFS_FILE_STATUS_PROTO__FILE_TYPE__IS_SYMLINK == HDFS_FT_SYMLINK);

	ASSERT(HDFS_FT_DIR <= (unsigned)pr && (unsigned)pr <= HDFS_FT_SYMLINK);
	return (unsigned)pr;
}

struct hdfs_object *
_hdfs_file_status_new_proto(Hadoop__Hdfs__HdfsFileStatusProto *fs)
{
	struct hdfs_object *r = _objmalloc();
	enum hdfs_file_type ft;
	char *path_copy, *owner_copy, *group_copy;

	ASSERT(fs);

	ft = _hdfs_file_type_from_proto(fs->filetype);
	path_copy = _proto_str(fs->path);
	owner_copy = strdup(fs->owner);
	group_copy = strdup(fs->group);

	ASSERT(path_copy);
	ASSERT(owner_copy);
	ASSERT(group_copy);

	r->ob_type = H_FILE_STATUS;
	r->ob_val._file_status = (struct hdfs_file_status) {
		._file = path_copy,
		._size = fs->length,
		._type = ft,
		._directory = (ft == HDFS_FT_DIR)? true : false,
		._mtime = (int64_t)fs->modification_time,
		._atime = (int64_t)fs->access_time,
		._permissions = (int16_t)fs->permission->perm,
		._owner = owner_copy,
		._group = group_copy,

		/* Default values: */
		._replication = 0,
		._block_size = 0,
		._symlink_target = NULL,
		._fileid = 0,
		._num_children = -1,
	};

	if (fs->has_block_replication)
		r->ob_val._file_status._replication = fs->block_replication;
	if (fs->has_blocksize)
		r->ob_val._file_status._block_size = fs->blocksize;
	if (fs->has_symlink)
		r->ob_val._file_status._symlink_target = _proto_str(fs->symlink);
	if (fs->has_fileid)
		r->ob_val._file_status._fileid = fs->fileid;
	if (fs->has_childrennum)
		r->ob_val._file_status._num_children = fs->childrennum;

	return r;
}

EXPORT_SYM struct hdfs_object *
hdfs_content_summary_new(int64_t length, int64_t files, int64_t dirs, int64_t quota)
{
	struct hdfs_object *r = _objmalloc();
	r->ob_type = H_CONTENT_SUMMARY;
	r->ob_val._content_summary = (struct hdfs_content_summary) {
		._length = length,
		._files = files,
		._dirs = dirs,
		._quota = quota,
	};
	return r;
}

struct hdfs_object *
_hdfs_content_summary_new_proto(Hadoop__Hdfs__ContentSummaryProto *pr)
{

	return hdfs_content_summary_new(pr->length, pr->filecount,
	    pr->directorycount, pr->spacequota);
}

EXPORT_SYM struct hdfs_object *
hdfs_block_new(int64_t blkid, int64_t len, int64_t generation)
{
	struct hdfs_object *r = _objmalloc();
	r->ob_type = H_BLOCK;
	r->ob_val._block = (struct hdfs_block) {
		._blkid = blkid,
		._length = len,
		._generation = generation,
	};
	return r;
}

EXPORT_SYM struct hdfs_object *
hdfs_block_copy(struct hdfs_object *src)
{
	struct hdfs_object *r;
	char *pool_id;

	if (!src)
		return hdfs_null_new(H_BLOCK);

	if (src->ob_val._block._pool_id) {
		pool_id = strdup(src->ob_val._block._pool_id);
		ASSERT(pool_id);
	} else
		pool_id = NULL;

	r = _objmalloc();

	ASSERT(src->ob_type == H_BLOCK);

	r->ob_type = H_BLOCK;
	r->ob_val._block = src->ob_val._block;
	r->ob_val._block._pool_id = pool_id;
	return r;
}

EXPORT_SYM struct hdfs_object *
hdfs_block_from_located_block(struct hdfs_object *src)
{
	struct hdfs_object *r = _objmalloc();
	char *pool_id;

	ASSERT(src);
	ASSERT(src->ob_type == H_LOCATED_BLOCK);

	if (src->ob_val._located_block._pool_id) {
		pool_id = strdup(src->ob_val._located_block._pool_id);
		ASSERT(pool_id);
	} else
		pool_id = NULL;

	r->ob_type = H_BLOCK;
	r->ob_val._block = (struct hdfs_block) {
		._blkid = src->ob_val._located_block._blockid,
		._length = src->ob_val._located_block._len,
		._generation = src->ob_val._located_block._generation,
		._pool_id = pool_id,
	};
	return r;
}

EXPORT_SYM struct hdfs_object *
hdfs_array_byte_new(int len, int8_t *bytes)
{
	int8_t *bytes_copy = NULL;
	struct hdfs_object *r = _objmalloc();

	if (len) {
		bytes_copy = malloc(len);
		ASSERT(bytes_copy);
		memcpy(bytes_copy, bytes, len);
	}

	r->ob_type = H_ARRAY_BYTE;
	r->ob_val._array_byte = (struct hdfs_array_byte) {
		._len = len,
		._bytes = bytes_copy,
	};
	return r;
}

EXPORT_SYM struct hdfs_object *
hdfs_array_byte_copy(struct hdfs_object *src)
{
	struct hdfs_object *r;
	int32_t len;

	if (!src)
		return hdfs_null_new(H_ARRAY_BYTE);

	ASSERT(src->ob_type == H_ARRAY_BYTE);

	r = _objmalloc();
	len = src->ob_val._array_byte._len;

	r->ob_type = H_ARRAY_BYTE;
	r->ob_val._array_byte._len = len;
	if (len) {
		int8_t *bytes_copy = malloc(len);
		ASSERT(bytes_copy);
		memcpy(bytes_copy, src->ob_val._array_byte._bytes, len);
		r->ob_val._array_byte._bytes = bytes_copy;
	}
	return r;
}

EXPORT_SYM struct hdfs_rpc_invocation *
hdfs_rpc_invocation_new(const char *name, ...)
{
	unsigned i;
	char *meth_copy = strdup(name);
	struct hdfs_rpc_invocation *r = malloc(sizeof(*r));
	va_list ap;
	struct hdfs_object *arg;

	ASSERT(meth_copy);
	ASSERT(r);

	memset(r, 0, sizeof(*r));

	r->_method = meth_copy;

	va_start(ap, name);
	i = 0;
	while (true) {
		arg = va_arg(ap, struct hdfs_object *);
		if (!arg)
			break;
		r->_args[i] = arg;
		i++;

		ASSERT(i < nelem(r->_args));
	}
	r->_nargs = i;
	va_end(ap);

	return r;
}

EXPORT_SYM void
hdfs_rpc_invocation_free(struct hdfs_rpc_invocation *r)
{
	ASSERT(r);

	free(r->_method);
	for (int i = 0; i < r->_nargs; i++) {
		hdfs_object_free(r->_args[i]);
	}

	free(r);
}

// XXX These authheader functions don't really need to be user-accessible.
EXPORT_SYM struct hdfs_authheader *
hdfs_authheader_new_ext(const char *user, const char *real_user)
{
	char *user_copy, *real_user_copy;
	struct hdfs_authheader *auth = malloc(sizeof*auth);

	ASSERT(auth);
	memset(auth, 0, sizeof(*auth));

	user_copy = strdup(user);
	ASSERT(user_copy);

	ASSERT(real_user == NULL /* TODO Issue #27 */);
	real_user_copy = real_user ? strdup(real_user) : NULL;
	if (real_user)
		ASSERT(real_user_copy);

	auth->_username = user_copy;
	auth->_real_username = real_user_copy;

	return auth;
}

EXPORT_SYM struct hdfs_authheader *
hdfs_authheader_new(const char *user)
{
	return hdfs_authheader_new_ext(user, NULL);
}

EXPORT_SYM void
hdfs_authheader_free(struct hdfs_authheader *auth)
{
	ASSERT(auth);

	free(auth->_username);
	free(auth->_real_username);

	free(auth);
}

EXPORT_SYM struct hdfs_object *
hdfs_string_new(const char *s)
{
	char *str_copy;
	struct hdfs_object *r;

	if (!s)
		return hdfs_null_new(H_STRING);

	str_copy = strdup(s);
	r = _objmalloc();

	ASSERT(str_copy);

	r->ob_type = H_STRING;
	r->ob_val._string = (struct hdfs_string) {
		._val = str_copy,
	};
	return r;
}

EXPORT_SYM struct hdfs_object *
hdfs_text_new(const char *s)
{
	char *str_copy;
	struct hdfs_object *r;

	if (!s)
		return hdfs_null_new(H_TEXT);

	str_copy = strdup(s);
	r = _objmalloc();

	ASSERT(str_copy);

	r->ob_type = H_TEXT;
	r->ob_val._string = (struct hdfs_string) {
		._val = str_copy,
	};
	return r;
}

EXPORT_SYM struct hdfs_object *
hdfs_fsperms_new(int16_t perms)
{
	struct hdfs_object *r = _objmalloc();
	r->ob_type = H_FSPERMS;
	r->ob_val._fsperms._perms = perms;
	return r;
}

EXPORT_SYM struct hdfs_object *
hdfs_protocol_exception_new(enum hdfs_exception_type etype, const char *msg)
{
	char *msg_copy = strdup(msg);
	struct hdfs_object *r = _objmalloc();
	ASSERT(msg_copy);
	r->ob_type = H_PROTOCOL_EXCEPTION;
	r->ob_val._exception = (struct hdfs_exception) {
		._etype = etype,
		._msg = msg_copy,
	};
	return r;
}

EXPORT_SYM struct hdfs_object *
hdfs_array_string_new(int32_t len, const char **strings)
{
	struct hdfs_object *r = _objmalloc();

	r->ob_type = H_ARRAY_STRING;

	for (int32_t i = 0; i < len; i++)
		hdfs_array_string_add(r, strings[i]);

	return r;
}

EXPORT_SYM struct hdfs_object *
hdfs_safemodeaction_new(const char *mode)
{
	struct hdfs_object *r;

	ASSERT(mode);
	ASSERT(streq(mode, HDFS_SAFEMODE_ENTER) ||
	    streq(mode, HDFS_SAFEMODE_LEAVE) ||
	    streq(mode, HDFS_SAFEMODE_GET));

	r = hdfs_string_new(mode);
	r->ob_type = H_SAFEMODEACTION;
	return r;
}

EXPORT_SYM struct hdfs_object *
hdfs_dnreporttype_new(const char *mode)
{
	struct hdfs_object *r;

	ASSERT(mode);
	ASSERT(streq(mode, HDFS_DNREPORT_ALL) ||
	    streq(mode, HDFS_DNREPORT_LIVE) ||
	    streq(mode, HDFS_DNREPORT_DEAD));

	r = hdfs_string_new(mode);
	r->ob_type = H_DNREPORTTYPE;
	return r;
}

EXPORT_SYM struct hdfs_object *
hdfs_upgradeaction_new(const char *mode)
{
	struct hdfs_object *r;

	ASSERT(mode);
	ASSERT(streq(mode, HDFS_UPGRADEACTION_STATUS) ||
	    streq(mode, HDFS_UPGRADEACTION_DETAILED) ||
	    streq(mode, HDFS_UPGRADEACTION_FORCE_PROCEED));

	r = hdfs_string_new(mode);
	r->ob_type = H_UPGRADE_ACTION;
	return r;
}

EXPORT_SYM struct hdfs_object *
hdfs_upgrade_status_report_new(int32_t version, int16_t status)
{
	struct hdfs_object *r;

	r = _objmalloc();
	r->ob_type = H_UPGRADE_STATUS_REPORT;
	r->ob_val._upgrade_status._version = version;
	r->ob_val._upgrade_status._status = status;
	return r;
}

enum hdfs_checksum_type
_hdfs_csum_from_proto(Hadoop__Hdfs__ChecksumTypeProto pr)
{

	ASSERT((unsigned)HADOOP__HDFS__CHECKSUM_TYPE_PROTO__CHECKSUM_NULL == HDFS_CSUM_NULL);
	ASSERT((unsigned)HADOOP__HDFS__CHECKSUM_TYPE_PROTO__CHECKSUM_CRC32 == HDFS_CSUM_CRC32);
	ASSERT((unsigned)HADOOP__HDFS__CHECKSUM_TYPE_PROTO__CHECKSUM_CRC32C == HDFS_CSUM_CRC32C);

	ASSERT(HDFS_CSUM_NULL <= (unsigned)pr && (unsigned)pr <= HDFS_CSUM_CRC32C);
	return (unsigned)pr;
}

struct hdfs_object *
_hdfs_fsserverdefaults_new_proto(Hadoop__Hdfs__FsServerDefaultsProto *pr)
{
	struct hdfs_object *r;

	ASSERT(pr);

	r = _objmalloc();
	r->ob_type = H_FS_SERVER_DEFAULTS;
	r->ob_val._server_defaults = (struct hdfs_fsserverdefaults) {
		._blocksize = pr->blocksize,
		._bytes_per_checksum = pr->bytesperchecksum,
		._write_packet_size = pr->writepacketsize,
		._replication = pr->replication,
		._filebuffersize = pr->filebuffersize,
		._encrypt_data_transfer = false,
		._trashinterval = 0,
		._checksumtype = HDFS_CSUM_CRC32,
	};

	if (pr->has_encryptdatatransfer)
		r->ob_val._server_defaults._encrypt_data_transfer =
		    pr->encryptdatatransfer;
	if (pr->has_trashinterval)
		r->ob_val._server_defaults._trashinterval =
		    pr->trashinterval;
	if (pr->has_checksumtype)
		r->ob_val._server_defaults._checksumtype =
		    _hdfs_csum_from_proto(pr->checksumtype);
	return r;
}

EXPORT_SYM struct hdfs_object *
hdfs_array_locatedblock_new(void)
{
	struct hdfs_object *r;

	r = hdfs_located_blocks_new(false, 0);
	r->ob_type = H_ARRAY_LOCATEDBLOCK;
	return r;
}

EXPORT_SYM struct hdfs_object *
hdfs_array_locatedblock_copy(struct hdfs_object *src)
{
	struct hdfs_object *r;
	int32_t len;

	ASSERT(src && src->ob_type == H_ARRAY_LOCATEDBLOCK);

	len = src->ob_val._located_blocks._num_blocks;

	r = _objmalloc();
	r->ob_type = H_ARRAY_LOCATEDBLOCK;
	r->ob_val._located_blocks._num_blocks = len;

	if (len <= 0)
		goto out;

	r->ob_val._located_blocks._blocks = malloc(len * sizeof(struct hdfs_object *));
	ASSERT(r->ob_val._located_blocks._blocks);

	for (int32_t i = 0; i < len; i++) {
		r->ob_val._located_blocks._blocks[i] = hdfs_located_block_copy(
		    src->ob_val._located_blocks._blocks[i]);
	}

out:
	return r;
}

EXPORT_SYM struct hdfs_object *
hdfs_array_string_copy(struct hdfs_object *src)
{
	struct hdfs_object *r = _objmalloc();
	char **strings;
	int32_t len, i;

	ASSERT(src->ob_type == H_ARRAY_STRING);

	len = src->ob_val._array_string._len;
	strings = src->ob_val._array_string._val;

	r->ob_type = H_ARRAY_STRING;
	r->ob_val._array_string._len = len;
	r->ob_val._array_string._val = malloc(len * sizeof(*strings));
	ASSERT(r->ob_val._array_string._val);

	for (i = 0; i < len; i++) {
		r->ob_val._array_string._val[i] =
		    strdup(src->ob_val._array_string._val[i]);
		ASSERT(r->ob_val._array_string._val[i] != NULL);
	}
	return r;
}

// Caller loses references to objects that are being appended into other
// objects.
#define H_ARRAY_RESIZE 8
#define H_ARRAY_APPEND(array, array_len, obj) do { \
	if (array_len % H_ARRAY_RESIZE == 0) { \
		array = realloc(array, (array_len+H_ARRAY_RESIZE) * sizeof(struct hdfs_object *)); \
		ASSERT(array); \
	} \
	array[array_len] = obj; \
	array_len += 1; \
} while (0)

EXPORT_SYM void
hdfs_located_block_append_datanode_info(struct hdfs_object *located_block,
	struct hdfs_object *datanode_info)
{
	ASSERT(located_block->ob_type == H_LOCATED_BLOCK);
	ASSERT(datanode_info->ob_type == H_DATANODE_INFO);

	H_ARRAY_APPEND(located_block->ob_val._located_block._locs,
	    located_block->ob_val._located_block._num_locs,
	    datanode_info);
}

EXPORT_SYM void
hdfs_located_blocks_append_located_block(struct hdfs_object *located_blocks,
	struct hdfs_object *located_block)
{
	ASSERT(located_blocks->ob_type == H_LOCATED_BLOCKS);
	ASSERT(located_block->ob_type == H_LOCATED_BLOCK);

	H_ARRAY_APPEND(located_blocks->ob_val._located_blocks._blocks,
	    located_blocks->ob_val._located_blocks._num_blocks, located_block);
}

EXPORT_SYM void
hdfs_array_locatedblock_append_located_block(struct hdfs_object *arr_located_block,
	struct hdfs_object *located_block)
{
	ASSERT(arr_located_block->ob_type == H_ARRAY_LOCATEDBLOCK);
	ASSERT(located_block->ob_type == H_LOCATED_BLOCK);

	H_ARRAY_APPEND(arr_located_block->ob_val._located_blocks._blocks,
	    arr_located_block->ob_val._located_blocks._num_blocks, located_block);
}

EXPORT_SYM void
hdfs_directory_listing_append_file_status(struct hdfs_object *directory_listing,
	struct hdfs_object *file_status, struct hdfs_object *located_blocks)
{
	ASSERT(directory_listing);
	ASSERT(directory_listing->ob_type == H_DIRECTORY_LISTING);
	ASSERT(file_status);
	ASSERT(file_status->ob_type == H_FILE_STATUS);

	H_ARRAY_APPEND(directory_listing->ob_val._directory_listing._files,
	    directory_listing->ob_val._directory_listing._num_files, file_status);

	if (directory_listing->ob_val._directory_listing._has_locations) {
		ASSERT(located_blocks);
		ASSERT(located_blocks->ob_type == H_LOCATED_BLOCKS);

		directory_listing->ob_val._directory_listing._num_files -= 1;
		H_ARRAY_APPEND(directory_listing->ob_val._directory_listing._located_blocks,
		    directory_listing->ob_val._directory_listing._num_files, located_blocks);
	} else {
		ASSERT(located_blocks == NULL);
	}
}

EXPORT_SYM void
hdfs_array_datanode_info_append_datanode_info(struct hdfs_object *array, struct hdfs_object *datanode_info)
{
	ASSERT(array->ob_type == H_ARRAY_DATANODE_INFO);
	ASSERT(datanode_info->ob_type == H_DATANODE_INFO);

	H_ARRAY_APPEND(array->ob_val._array_datanode_info._values,
	    array->ob_val._array_datanode_info._len,
	    datanode_info);
}

EXPORT_SYM void
hdfs_array_string_add(struct hdfs_object *o, const char *s)
{
	char *copy;

	ASSERT(s);
	ASSERT(o);
	ASSERT(o->ob_type == H_ARRAY_STRING);

	copy = strdup(s);
	ASSERT(copy);

	H_ARRAY_APPEND(o->ob_val._array_string._val, o->ob_val._array_string._len, copy);
}

#define FREE_H_ARRAY_ELMS(array, array_len) do { \
	for (int32_t i = 0; i < array_len; i++) { \
		hdfs_object_free(array[i]); \
	} \
} while (0)

#define FREE_H_ARRAY(array, array_len) do { \
	FREE_H_ARRAY_ELMS(array, array_len); \
	free(array); \
} while (0)

// Recursively frees an object:
EXPORT_SYM void
hdfs_object_free(struct hdfs_object *obj)
{
	switch (obj->ob_type) {
	case _H_INVALID:
		/* UNREACHABLE */
		ASSERT(false);
		break;
	case H_VOID: break; // NOOP
	case H_NULL: break;
	case H_BOOLEAN: break;
	case H_SHORT: break;
	case H_FSPERMS: break;
	case H_INT: break;
	case H_LONG: break;
	case H_UPGRADE_STATUS_REPORT: break;
	case H_FS_SERVER_DEFAULTS: break;
	case H_ARRAY_LONG:
		free(obj->ob_val._array_long._vals);
		break;
	case H_LOCATED_BLOCK:
		FREE_H_ARRAY(obj->ob_val._located_block._locs,
		    obj->ob_val._located_block._num_locs);
		hdfs_object_free(obj->ob_val._located_block._token);
		free(obj->ob_val._located_block._pool_id);
		break;
	case H_ARRAY_LOCATEDBLOCK:
		/* FALLTHROUGH */
	case H_LOCATED_BLOCKS:
		FREE_H_ARRAY(obj->ob_val._located_blocks._blocks,
		    obj->ob_val._located_blocks._num_blocks);
		if (obj->ob_val._located_blocks._last_block)
			hdfs_object_free(obj->ob_val._located_blocks._last_block);
		break;
	case H_LOCATED_DIRECTORY_LISTING:
		/* FALLTHROUGH */
	case H_DIRECTORY_LISTING:
		FREE_H_ARRAY(obj->ob_val._directory_listing._files,
		    obj->ob_val._directory_listing._num_files);
		if (obj->ob_val._directory_listing._has_locations) {
			FREE_H_ARRAY(obj->ob_val._directory_listing._located_blocks,
			    obj->ob_val._directory_listing._num_files);
		}
		break;
	case H_DATANODE_INFO:
		free(obj->ob_val._datanode_info._location);
		free(obj->ob_val._datanode_info._ipaddr);
		free(obj->ob_val._datanode_info._hostname);
		free(obj->ob_val._datanode_info._port);
		break;
	case H_ARRAY_DATANODE_INFO:
		FREE_H_ARRAY(obj->ob_val._array_datanode_info._values,
		    obj->ob_val._array_datanode_info._len);
		break;
	case H_FILE_STATUS:
		free(obj->ob_val._file_status._file);
		free(obj->ob_val._file_status._owner);
		free(obj->ob_val._file_status._group);
		free(obj->ob_val._file_status._symlink_target);
		break;
	case H_CONTENT_SUMMARY: break;
	case H_BLOCK:
		free(obj->ob_val._block._pool_id);
		break;
	case H_ARRAY_BYTE:
		if (obj->ob_val._array_byte._bytes)
			free(obj->ob_val._array_byte._bytes);
		break;
	case H_TOKEN:
		free(obj->ob_val._token._id);
		free(obj->ob_val._token._pw);
		free(obj->ob_val._token._kind);
		free(obj->ob_val._token._service);
		break;
	case H_PROTOCOL_EXCEPTION:
		free(obj->ob_val._exception._msg);
		break;
	case H_UPGRADE_ACTION:
		/* FALLTHROUGH */
	case H_DNREPORTTYPE:
		/* FALLTHROUGH */
	case H_SAFEMODEACTION:
		/* FALLTHROUGH */
	case H_STRING:
		/* FALLTHROUGH */
	case H_TEXT:
		free(obj->ob_val._string._val);
		break;
	case H_ARRAY_STRING:
		for (int32_t i = 0; i < obj->ob_val._array_string._len; i++)
			free(obj->ob_val._array_string._val[i]);
		if (obj->ob_val._array_string._val)
			free(obj->ob_val._array_string._val);
		break;
	default:
		ASSERT(false);
	}
	free(obj);
}

static const char *
_rawtypestring(enum hdfs_object_type t)
{
	const char *res;

	ASSERT(t >= _H_START);
	res = object_types[t - _H_START].type;
	ASSERT(res);
	return res;
}

static const char *
_typestring(struct hdfs_object *obj)
{
	const char *res;

	ASSERT(obj->ob_type >= _H_START);
	res = object_types[obj->ob_type - _H_START].type;
	ASSERT(res);
	return res;
}

static bool
_is_object_objtype(struct hdfs_object *obj)
{
	return object_types[obj->ob_type - _H_START].objtype;
}

static bool
_is_type_objtype(enum hdfs_object_type t)
{
	return object_types[t - _H_START].objtype;
}

static void
_serialize_rpc_v1(struct hdfs_heap_buf *dest, struct hdfs_rpc_invocation *rpc,
	int64_t msgno)
{
	struct hdfs_heap_buf rbuf = { 0 };

	// XXX TODO try to refactor to avoid using local heapbufs

	_bappend_s32(&rbuf, msgno);
	_bappend_string(&rbuf, rpc->_method);
	_bappend_s32(&rbuf, rpc->_nargs);
	for (int i = 0; i < rpc->_nargs; i++) {
		struct hdfs_object *aobj = rpc->_args[i];

		_bappend_string(&rbuf, _typestring(aobj));
		if (_is_object_objtype(aobj))
			_bappend_string(&rbuf, _typestring(aobj));
		if (aobj->ob_type == H_NULL || aobj->ob_type == H_VOID)
			_bappend_string(&rbuf, NULL_TYPE2);
		hdfs_object_serialize(&rbuf, aobj);
	}

	_bappend_s32(dest, rbuf.used);
	_bappend_mem(dest, rbuf.used, rbuf.buf);
	free(rbuf.buf);
}

static void
_serialize_rpc_v2(struct hdfs_heap_buf *dest, struct hdfs_rpc_invocation *rpc,
	int64_t msgno)
{
	/*
	 * v2 is (stupid) complicated. RPCs look like:
	 *
	 * +-----------------------------------------------------------------+
	 * | int32 size of vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv    |
	 * +-----------------------------------------------------------------+
	 * |   "header_buf"       |   "rpcwrapper_buf"                       |
	 * |  vlint size of vvv   |  vlint size of vvvvvvvvvvvvvvvvvvvvv     |
	 * +----------------------|------------------------------------------+
	 * |RpcPayloadHeaderProto |HadoopRpcRequestProto |hdfs:*RequestProto |
	 * |    "header"          |   "rpcwrapper"       |   "method"        |
	 * |                      |                      `-------------------|
	 * |______________________|__________________________________________|
	 *
	 * The vast majority of this crap doesn't change at all from call to
	 * call. Hurray.
	 */

	struct hdfs_heap_buf method_buf = { 0 },
			     rpcwrapper_buf = { 0 },
			     header_buf = { 0 };
	HadoopRpcRequestProto rpcwrapper = HADOOP_RPC_REQUEST_PROTO__INIT;
	RpcPayloadHeaderProto header = RPC_PAYLOAD_HEADER_PROTO__INIT;
	size_t rpcwrapper_sz, header_sz;

	// XXX TODO try to refactor to avoid using local heapbufs

	_rpc2_request_serialize(&method_buf, rpc);

	rpcwrapper.methodname = rpc->_method;
	rpcwrapper.has_request = true;
	rpcwrapper.request.len = method_buf.used;
	rpcwrapper.request.data = (void *)method_buf.buf;
	rpcwrapper.declaringclassprotocolname =
	    __DECONST(char *, CLIENT_PROTOCOL);
	rpcwrapper.clientprotocolversion = 1;
	rpcwrapper_sz = hadoop_rpc_request_proto__get_packed_size(&rpcwrapper);

	_bappend_vlint(&rpcwrapper_buf, rpcwrapper_sz);
	_hbuf_reserve(&rpcwrapper_buf, rpcwrapper_sz);
	hadoop_rpc_request_proto__pack(&rpcwrapper,
	    (void *)&rpcwrapper_buf.buf[rpcwrapper_buf.used]);
	rpcwrapper_buf.used += rpcwrapper_sz;

	header.has_rpckind = true;
	header.rpckind = RPC_KIND_PROTO__RPC_PROTOCOL_BUFFER;
	header.has_rpcop = true;
	header.rpcop = RPC_PAYLOAD_OPERATION_PROTO__RPC_FINAL_PAYLOAD;
	header.callid = msgno;
	header_sz = rpc_payload_header_proto__get_packed_size(&header);

	_bappend_vlint(&header_buf, header_sz);
	_hbuf_reserve(&header_buf, header_sz);
	rpc_payload_header_proto__pack(&header,
	    (void *)&header_buf.buf[header_buf.used]);
	header_buf.used += header_sz;

	_bappend_s32(dest, header_buf.used + rpcwrapper_buf.used);
	_bappend_mem(dest, header_buf.used, header_buf.buf);
	_bappend_mem(dest, rpcwrapper_buf.used, rpcwrapper_buf.buf);

	free(header_buf.buf);
	free(method_buf.buf);
	free(rpcwrapper_buf.buf);
}

static void
_serialize_rpc_v2_2(struct hdfs_heap_buf *dest, struct hdfs_rpc_invocation *rpc,
	int64_t msgno, const uint8_t *client_id)
{
	/*
	 * v2.2 is similar to v2, but uses different .proto classes. Unlike v2,
	 * the hdfs:*RequestProto is concatenated at the end (with varint size),
	 * rather than being embedded in the class that has the method name.
	 *
	 * Again, the rest of this is prefixed with the int32 total size:
	 *
	 *               +----------------+
	 *               | total size:i32 |
	 * +-----------------------------------------------------------------+
	 * | sizeof:varint | RpcRequestHeaderProto "header"                  |
	 * |-----------------------------------------------------------------|
	 * | sizeof:varint | RequestHeaderProto    "rpcwrapper"              |
	 * |-----------------------------------------------------------------|
	 * | sizeof:varint | hdfs:*RequestProto    "method"                  |
	 * +-----------------------------------------------------------------+
	 */

	Hadoop__Common__RequestHeaderProto rpcwrapper =
	    HADOOP__COMMON__REQUEST_HEADER_PROTO__INIT;
	Hadoop__Common__RpcRequestHeaderProto header =
	    HADOOP__COMMON__RPC_REQUEST_HEADER_PROTO__INIT;
	size_t method_sz, method_vlint_sz, rpcwrapper_sz,
	    rpcwrapper_vlint_sz, header_sz, header_vlint_sz, totsz;

	method_sz = _rpc2_request_get_size(rpc);
	method_vlint_sz = _get_vlint_encoding_size(method_sz);

	rpcwrapper.methodname = rpc->_method;
	rpcwrapper.declaringclassprotocolname =
	    __DECONST(char *, CLIENT_PROTOCOL);
	rpcwrapper.clientprotocolversion = 1;

	rpcwrapper_sz =
	    hadoop__common__request_header_proto__get_packed_size(&rpcwrapper);
	rpcwrapper_vlint_sz = _get_vlint_encoding_size(rpcwrapper_sz);

	header.has_rpckind = true;
	header.rpckind = HADOOP__COMMON__RPC_KIND_PROTO__RPC_PROTOCOL_BUFFER;
	header.has_rpcop = true;
	header.rpcop =
	    HADOOP__COMMON__RPC_REQUEST_HEADER_PROTO__OPERATION_PROTO__RPC_FINAL_PACKET;
	header.callid = msgno;
	header.clientid.len = _HDFS_CLIENT_ID_LEN;
	header.clientid.data = __DECONST(uint8_t *, client_id);
	header.has_retrycount = true;
	header.retrycount = 0;

	header_sz =
	    hadoop__common__rpc_request_header_proto__get_packed_size(&header);
	header_vlint_sz = _get_vlint_encoding_size(header_sz);

	totsz = header_vlint_sz + header_sz +
	    rpcwrapper_vlint_sz + rpcwrapper_sz +
	    method_vlint_sz + method_sz;
	_hbuf_reserve(dest, totsz + 4);

	// total size
	_bappend_s32(dest, totsz);
	// header
	_bappend_vlint(dest, header_sz);
	hadoop__common__rpc_request_header_proto__pack(&header,
	    (void *)_hbuf_writeptr(dest));
	_hbuf_append(dest, header_sz);
	// rpcwrapper
	_bappend_vlint(dest, rpcwrapper_sz);
	hadoop__common__request_header_proto__pack(&rpcwrapper,
	    (void *)_hbuf_writeptr(dest));
	_hbuf_append(dest, rpcwrapper_sz);
	// method
	_bappend_vlint(dest, method_sz);
	_rpc2_request_serialize(dest, rpc);
}

void
_hdfs_serialize_rpc(struct hdfs_heap_buf *dest, struct hdfs_rpc_invocation *rpc,
	 enum hdfs_namenode_proto proto, int64_t msgno, const uint8_t *client_id)
{
	switch (proto) {
	case HDFS_NN_v1:
		return _serialize_rpc_v1(dest, rpc, msgno);
	case HDFS_NN_v2:
		return _serialize_rpc_v2(dest, rpc, msgno);
	case HDFS_NN_v2_2:
		return _serialize_rpc_v2_2(dest, rpc, msgno, client_id);
	default:
		ASSERT(false);
	}
}

void
_hdfs_serialize_authheader(struct hdfs_heap_buf *dest, struct hdfs_authheader *auth,
	enum hdfs_namenode_proto pr, enum hdfs_kerb kerb, const uint8_t *client_id)
{
	struct hdfs_heap_buf abuf = { 0 };
	size_t cc_sz;

	// XXX TODO refactor this into separate functions by namenode proto, with this
	// function purely being a proto switch
	// XXX TODO consider moving this to namenode.c and making it static
	// XXX TODO try to refactor this to avoid using any local heapbufs
	// XXX TODO ensure that this appends to dest and doesn't just write to the beginning

	// XXX should this assertion be removed since issue #27 has been closed for a while?
	ASSERT(auth->_real_username == NULL);	// Issue #27

	if (pr == HDFS_NN_v1) {
		_bappend_text(&abuf, CLIENT_PROTOCOL);
		_bappend_s8(&abuf, 1);
		_bappend_string(&abuf, auth->_username);
		if (auth->_real_username == NULL)
			_bappend_s8(&abuf, 0);
		else {
			_bappend_s8(&abuf, 1);
			_bappend_string(&abuf, auth->_real_username);
		}

		_bappend_s32(dest, abuf.used);
		_bappend_mem(dest, abuf.used, abuf.buf);
		goto out;
	}

	// XXX TODO double check this. Also consider the interaction between
	// this and HDFS_TRY_KERB if we fall back to simple auth (does v2.2 even
	// allow for fallback to simple auth?)
	/* 2.2 doesn't send any header for SASL connections */
	if (pr == HDFS_NN_v2_2 && kerb != HDFS_NO_KERB)
		goto out;

	Hadoop__Common__UserInformationProto ui = HADOOP__COMMON__USER_INFORMATION_PROTO__INIT;
	Hadoop__Common__IpcConnectionContextProto context =
	    HADOOP__COMMON__IPC_CONNECTION_CONTEXT_PROTO__INIT;

	ui.effectiveuser = auth->_username;
	ui.realuser = auth->_real_username;
	context.userinfo = &ui;
	context.protocol = __DECONST(char *, CLIENT_PROTOCOL);

	cc_sz = hadoop__common__ipc_connection_context_proto__get_packed_size(&context);
	_hbuf_reserve(&abuf, cc_sz);
	hadoop__common__ipc_connection_context_proto__pack(&context,
	    (void *)&abuf.buf[abuf.used]);
	abuf.used += cc_sz;

	if (pr == HDFS_NN_v2) {
		_bappend_s32(dest, abuf.used);
		_bappend_mem(dest, abuf.used, abuf.buf);
	} else {
		/*
		 * HDFSv2.2+:
		 *
		 * i32 total size of:
		 *   varint size of:           \
		 *     RpcRequestHeaderProto    } encoded in 'hbuf' below
		 *   varint size of:           /
		 *     IpcConnectionContextProto
		 */
		struct hdfs_heap_buf hbuf = { 0 };
		Hadoop__Common__RpcRequestHeaderProto header =
		    HADOOP__COMMON__RPC_REQUEST_HEADER_PROTO__INIT;
		size_t hd_sz;

		ASSERT(pr == HDFS_NN_v2_2);

		header.has_rpckind = true;
		header.rpckind =
		    HADOOP__COMMON__RPC_KIND_PROTO__RPC_PROTOCOL_BUFFER;
		header.has_rpcop = true;
		header.rpcop =
		    HADOOP__COMMON__RPC_REQUEST_HEADER_PROTO__OPERATION_PROTO__RPC_FINAL_PACKET;
		header.callid = -3 /* Magic */;
		header.clientid.len = _HDFS_CLIENT_ID_LEN;
		ASSERT(client_id);
		header.clientid.data = __DECONST(uint8_t *, client_id);
		header.has_retrycount = true;
		header.retrycount = -1 /* Magic */;

		hd_sz = hadoop__common__rpc_request_header_proto__get_packed_size(&header);
		_bappend_vlint(&hbuf, hd_sz);
		_hbuf_reserve(&hbuf, hd_sz);
		hadoop__common__rpc_request_header_proto__pack(&header,
		    (void *)&hbuf.buf[hbuf.used]);
		hbuf.used += hd_sz;

		_bappend_vlint(&hbuf, cc_sz);

		_bappend_s32(dest, hbuf.used + abuf.used);
		_bappend_mem(dest, hbuf.used, hbuf.buf);
		_bappend_mem(dest, abuf.used, abuf.buf);

		free(hbuf.buf);
	}

out:
	free(abuf.buf);
}

// Serializes an hdfs_object into a buffer. dest must be non-NULL and
// initialized to zero.
//
// (For legacy v1 protocol objects only.)
EXPORT_SYM void
hdfs_object_serialize(struct hdfs_heap_buf *dest, struct hdfs_object *obj)
{
	switch (obj->ob_type) {
	case _H_INVALID:
		/* UNREACHABLE */
		ASSERT(false);
		break;
	// v2+ types are invalid input
	case H_FS_SERVER_DEFAULTS:
	// The client library doesn't ever need to serialize an exception
	case H_PROTOCOL_EXCEPTION:
		/* Invalid input */
		ASSERT(false);
		break;
	case H_VOID:
		_bappend_string(dest, "void");
		break;
	case H_NULL:
		_bappend_string(dest, _rawtypestring(obj->ob_val._null._type));
		break;
	case H_BOOLEAN:
		_bappend_s8(dest, obj->ob_val._boolean._val);
		break;
	case H_SHORT:
		_bappend_s16(dest, obj->ob_val._short._val);
		break;
	case H_INT:
		_bappend_s32(dest, obj->ob_val._int._val);
		break;
	case H_LONG:
		_bappend_s64(dest, obj->ob_val._long._val);
		break;

	case H_ARRAY_LONG:
		{
		int len = obj->ob_val._array_long._len;
		_bappend_s32(dest, len);
		for (int i = 0; i < len; i++) {
			_bappend_string(dest, _rawtypestring(H_LONG));
			_bappend_s64(dest, obj->ob_val._array_long._vals[i]);
		}
		}
		break;
	case H_ARRAY_DATANODE_INFO:
		{
		int len = obj->ob_val._array_datanode_info._len;
		_bappend_s32(dest, len);
		for (int i = 0; i < len; i++) {
			_bappend_string(dest, _rawtypestring(H_DATANODE_INFO));
			_bappend_string(dest, _rawtypestring(H_DATANODE_INFO));
			hdfs_object_serialize(dest,
			    obj->ob_val._array_datanode_info._values[i]);
		}
		}
		break;
	case H_ARRAY_BYTE:
		{
		int len = obj->ob_val._array_byte._len;
		_bappend_s32(dest, len);
		_bappend_mem(dest, len, obj->ob_val._array_byte._bytes);
		}
		break;

	case H_LOCATED_BLOCK:
		{
		int len;

		hdfs_object_serialize(dest, obj->ob_val._located_block._token);
		_bappend_s8(dest, 0);
		_bappend_s64(dest, obj->ob_val._located_block._offset);
		_bappend_s64(dest, obj->ob_val._located_block._blockid);
		_bappend_s64(dest, obj->ob_val._located_block._len);
		_bappend_s64(dest, obj->ob_val._located_block._generation);
		len = obj->ob_val._located_block._num_locs;
		_bappend_s32(dest, len);
		for (int i = 0; i < len; i++)
			hdfs_object_serialize(dest,
			    obj->ob_val._located_block._locs[i]);
		}
		break;
	case H_LOCATED_BLOCKS:
		{
		int len;
		_bappend_s64(dest, obj->ob_val._located_blocks._size);
		_bappend_s8(dest, obj->ob_val._located_blocks._being_written);
		len = obj->ob_val._located_blocks._num_blocks;
		_bappend_s32(dest, len);
		for (int i = 0; i < len; i++)
			hdfs_object_serialize(dest,
			    obj->ob_val._located_blocks._blocks[i]);
		}
		break;
	case H_ARRAY_LOCATEDBLOCK:
		{
		int32_t len;
		len = obj->ob_val._located_blocks._num_blocks;
		_bappend_s32(dest, len);
		for (int32_t i = 0; i < len; i++) {
			_bappend_string(dest, _rawtypestring(H_LOCATED_BLOCK));
			_bappend_string(dest, _rawtypestring(H_LOCATED_BLOCK));
			hdfs_object_serialize(dest,
			    obj->ob_val._located_blocks._blocks[i]);
		}
		}
		break;
	case H_LOCATED_DIRECTORY_LISTING:
		/* FALLTHROUGH */
	case H_DIRECTORY_LISTING:
		{
		int len = obj->ob_val._directory_listing._num_files;
		_bappend_s32(dest, len);
		for (int i = 0; i < len; i++)
			hdfs_object_serialize(dest,
			    obj->ob_val._directory_listing._files[i]);
		if (obj->ob_val._directory_listing._has_locations)
			for (int i = 0; i < len; i++)
				hdfs_object_serialize(dest,
				    obj->ob_val._directory_listing._located_blocks[i]);
		_bappend_s32(dest, 0/*remaining*/);
		}
		break;
	case H_DATANODE_INFO:
		{
		char *hostport = malloc(strlen(obj->ob_val._datanode_info._hostname) +
		    1 + strlen(obj->ob_val._datanode_info._port) + 1);
		ASSERT(hostport);
		strcpy(hostport, obj->ob_val._datanode_info._hostname);
		strcat(hostport, ":");
		strcat(hostport, obj->ob_val._datanode_info._port);

		_bappend_string(dest, hostport);
		_bappend_string(dest, hostport);
		_bappend_u16(dest, 31337/*arbitrary*/);
		_bappend_u16(dest, obj->ob_val._datanode_info._namenodeport/*ipc port*/);
		_bappend_s64(dest, 0/*capacity*/);
		_bappend_s64(dest, 0/*space used*/);
		_bappend_s64(dest, 0/*remaining*/);
		_bappend_s64(dest, 0/*last update*/);
		_bappend_s32(dest, 0/*xceivercount*/);
		_bappend_text(dest, obj->ob_val._datanode_info._location);
		_bappend_text(dest, obj->ob_val._datanode_info._hostname);
		_bappend_text(dest, "NORMAL");

		free(hostport);
		}
		break;
	case H_FILE_STATUS:
		{
		int len = strlen(obj->ob_val._file_status._file);
		mode_t perms = obj->ob_val._file_status._permissions;
		_bappend_s32(dest, len);
		_bappend_mem(dest, len, obj->ob_val._file_status._file);
		_bappend_s64(dest, obj->ob_val._file_status._size);
		_bappend_s8(dest, obj->ob_val._file_status._directory);
		_bappend_u16(dest, obj->ob_val._file_status._replication);
		_bappend_s64(dest, obj->ob_val._file_status._block_size);
		_bappend_s64(dest, obj->ob_val._file_status._mtime);
		_bappend_s64(dest, obj->ob_val._file_status._atime);
		if (obj->ob_val._file_status._directory)
			perms &= 0777;
		else
			perms &= 0666;
		_bappend_u16(dest, perms);
		_bappend_text(dest, obj->ob_val._file_status._owner);
		_bappend_text(dest, obj->ob_val._file_status._group);
		}
		break;
	case H_CONTENT_SUMMARY:
		_bappend_s64(dest, obj->ob_val._content_summary._length);
		_bappend_s64(dest, obj->ob_val._content_summary._files);
		_bappend_s64(dest, obj->ob_val._content_summary._dirs);
		_bappend_s64(dest, -1);
		_bappend_s64(dest, obj->ob_val._content_summary._length);
		_bappend_s64(dest, obj->ob_val._content_summary._quota);
		break;
	case H_UPGRADE_ACTION:
		/* FALLTHROUGH */
	case H_DNREPORTTYPE:
		/* FALLTHROUGH */
	case H_SAFEMODEACTION:
		/* FALLTHROUGH */
	case H_STRING:
		_bappend_string(dest, obj->ob_val._string._val);
		break;
	case H_FSPERMS:
		_bappend_s16(dest, obj->ob_val._fsperms._perms);
		break;
	case H_BLOCK:
		_bappend_s64(dest, obj->ob_val._block._blkid);
		_bappend_s64(dest, obj->ob_val._block._length);
		_bappend_s64(dest, obj->ob_val._block._generation);
		break;
	case H_TOKEN:
		_bappend_vlint(dest, obj->ob_val._token._id_len);
		_bappend_mem(dest, obj->ob_val._token._id_len,
		    obj->ob_val._token._id);
		_bappend_vlint(dest, obj->ob_val._token._pw_len);
		_bappend_mem(dest, obj->ob_val._token._pw_len,
		    obj->ob_val._token._pw);
		_bappend_text(dest, obj->ob_val._token._kind);
		_bappend_text(dest, obj->ob_val._token._service);
		break;
	case H_ARRAY_STRING:
		{
		int32_t len = obj->ob_val._array_string._len;
		_bappend_s32(dest, len);
		for (int32_t i = 0; i < len; i++) {
			_bappend_string(dest, _rawtypestring(H_STRING));
			_bappend_string(dest, obj->ob_val._array_string._val[i]);
		}
		}
		break;
	case H_TEXT:
		_bappend_text(dest, obj->ob_val._string._val);
		break;
	case H_UPGRADE_STATUS_REPORT:
		_bappend_s32(dest, obj->ob_val._upgrade_status._version);
		_bappend_s16(dest, obj->ob_val._upgrade_status._status);
		break;
	default:
		/* Invalid input */
		ASSERT(false);
	}
}

struct hdfs_error
_hdfs_result_deserialize(char *buf, int buflen, struct _hdfs_result *res)
{
	struct hdfs_error error = HDFS_SUCCESS;
	struct hdfs_heap_buf rbuf = {
		.buf = buf,
		.used = 0,
		.size = buflen,
	};

	int32_t msgno, status;
	char *etype = NULL, *emsg = NULL;
	char *otype = NULL, *ttype = NULL;
	enum hdfs_object_type realtype;

	ASSERT(res);
	res->rs_obj = NULL;

	msgno = _bslurp_s32(&rbuf);
	if (rbuf.used < 0)
		goto out;
	status = _bslurp_s32(&rbuf);
	if (rbuf.used < 0)
		goto out;

	// Parse exceptions
	if (status != 0) {
		etype = _bslurp_string32(&rbuf);
		if (rbuf.used < 0)
			goto out;
		emsg = _bslurp_string32(&rbuf);
		if (rbuf.used < 0)
			goto out;

		res->rs_msgno = msgno;
		res->rs_obj = _object_exception(etype, emsg);
		res->rs_size = rbuf.used;
		goto out; // HDFS_SUCCESS
	}

	// If we got this far we're reading a normal object
	otype = _bslurp_string(&rbuf);
	if (rbuf.used < 0)
		goto out;

	realtype = _string_to_type(otype);
	if (realtype == _H_INVALID) {
		error = error_from_hdfs(HDFS_ERR_NAMENODE_PROTOCOL);
		goto out;
	}

	// "object" types have their type twice (I think the idea is that the
	// first type is that of the API and the second could be a child class
	// or implementing class of an interface, but in practice it's always
	// the same)
	if (_is_type_objtype(realtype)) {
		ttype = _bslurp_string(&rbuf);
		if (rbuf.used < 0)
			goto out;
		ASSERT(streq(ttype, otype));
	} else if (realtype == H_VOID) {
		realtype = H_NULL;
	}

	if (realtype == H_NULL) {
		// ttype for null values is NOT the same as its otype; it's another string.
		ttype = _bslurp_string(&rbuf);
		if (rbuf.used < 0)
			goto out;
		ASSERT(streq(ttype, NULL_TYPE2));
	}

	res->rs_obj = hdfs_object_slurp(&rbuf, realtype);
	if (rbuf.used < 0)
		goto out;

	res->rs_msgno = msgno;
	res->rs_size = rbuf.used;

out:
	if (otype)
		free(otype);
	if (ttype)
		free(ttype);
	if (etype)
		free(etype);
	if (emsg)
		free(emsg);

	if (rbuf.used == _H_PARSE_EOF) {
		error = HDFS_AGAIN;
	} else if (rbuf.used == _H_PARSE_ERROR) {
		error = error_from_hdfs(HDFS_ERR_NAMENODE_PROTOCOL);
	}

	if (!hdfs_is_error(error)) {
		ASSERT(rbuf.used >= 0);
		ASSERT(res->rs_obj);
		ASSERT(res->rs_size == rbuf.used);
	}

	return error;
}

struct hdfs_error
_hdfs_result_deserialize_v2(char *buf, int buflen, struct _hdfs_result *res,
	struct _hdfs_pending *pend, int npend)
{
	struct hdfs_error error = HDFS_SUCCESS;
	struct hdfs_heap_buf rbuf = {
		.buf = buf,
		.used = 0,
		.size = buflen,
	};
	RpcResponseHeaderProto *resphd = NULL;
	int64_t resphdsz;
	char *etype = NULL, *emsg = NULL;
	int32_t respsz;
	int i;

	ASSERT(res);
	res->rs_obj = NULL;

	resphdsz = _bslurp_vlint(&rbuf);
	if (rbuf.used < 0)
		goto out;
	if (resphdsz > (rbuf.size - rbuf.used)) {
		error = HDFS_AGAIN;
		goto out;
	}

	resphd = rpc_response_header_proto__unpack(NULL, resphdsz,
	    (void *)&rbuf.buf[rbuf.used]);
	rbuf.used += resphdsz;
	if (resphd == NULL) {
		error = error_from_hdfs(HDFS_ERR_NAMENODE_PROTOCOL);
		goto out;
	}

	for (i = 0; i < npend; i++)
		if (pend[i].pd_msgno == (int64_t)resphd->callid)
			break;

	// Got a response to an unexpected msgno
	if (i == npend) {
		error = error_from_hdfs(HDFS_ERR_NAMENODE_BAD_MSGNO);
		goto out;
	}

	ASSERT(pend[i].pd_slurper);

	if (resphd->status == RPC_STATUS_PROTO__ERROR) {
		etype = _bslurp_string32(&rbuf);
		if (rbuf.used < 0)
			goto out;
		emsg = _bslurp_string32(&rbuf);
		if (rbuf.used < 0)
			goto out;

		res->rs_msgno = (int64_t)resphd->callid;
		res->rs_obj = _object_exception(etype, emsg);
		res->rs_size = rbuf.used;
		goto out; // HDFS_SUCCESS
	} else if (resphd->status != RPC_STATUS_PROTO__SUCCESS) {
		/* This shouldn't happen. */
		error = error_from_hdfs(HDFS_ERR_NAMENODE_PROTOCOL);
		goto out;
	}

	respsz = _bslurp_s32(&rbuf);
	if (rbuf.used < 0)
		goto out;
	if (respsz > (rbuf.size - rbuf.used)) {
		error = HDFS_AGAIN;
		goto out;
	}

	rbuf.size = rbuf.used + respsz;
	res->rs_obj = pend[i].pd_slurper(&rbuf);
	if (res->rs_obj == NULL) {
		error = error_from_hdfs(HDFS_ERR_NAMENODE_PROTOCOL);
		goto out;
	}

	res->rs_msgno = (int64_t)resphd->callid;
	res->rs_size = rbuf.used;

out:
	if (resphd)
		rpc_response_header_proto__free_unpacked(resphd, NULL);

	free(emsg);
	free(etype);

	if (rbuf.used == _H_PARSE_EOF) {
		error = HDFS_AGAIN;
	} else if (rbuf.used == _H_PARSE_ERROR) {
		error = error_from_hdfs(HDFS_ERR_NAMENODE_PROTOCOL);
	}

	if (!hdfs_is_error(error)) {
		ASSERT(rbuf.used >= 0);
		ASSERT(res->rs_obj);
		ASSERT(res->rs_size == rbuf.used);
	}

	return error;
}

struct hdfs_error
_hdfs_result_deserialize_v2_2(char *buf, int buflen, struct _hdfs_result *res,
	struct _hdfs_pending *pend, int npend)
{
	struct hdfs_error error = HDFS_SUCCESS;
	struct hdfs_heap_buf rbuf = {
		.buf = buf,
		.used = 0,
		.size = buflen,
	};
	Hadoop__Common__RpcResponseHeaderProto *resphd = NULL;
	int64_t resphdsz, totalsz, respsz;
	int i;

	ASSERT(res);
	res->rs_obj = NULL;

	totalsz = _bslurp_s32(&rbuf);
	if (rbuf.used < 0)
		goto out;
	if (totalsz > (rbuf.size - rbuf.used)) {
		error = HDFS_AGAIN;
		goto out;
	}

	resphdsz = _bslurp_vlint(&rbuf);
	if (rbuf.used < 0)
		goto out;
	if (resphdsz > (rbuf.size - rbuf.used)) {
		error = HDFS_AGAIN;
		goto out;
	}

	resphd = hadoop__common__rpc_response_header_proto__unpack(NULL,
	    resphdsz, (void *)&rbuf.buf[rbuf.used]);
	rbuf.used += resphdsz;
	if (resphd == NULL) {
		error = error_from_hdfs(HDFS_ERR_NAMENODE_PROTOCOL);
		goto out;
	}

	for (i = 0; i < npend; i++)
		if (pend[i].pd_msgno == (int64_t)resphd->callid)
			break;

	// Got a response to an unexpected msgno
	if (i == npend) {
		error = error_from_hdfs(HDFS_ERR_NAMENODE_BAD_MSGNO);
		goto out;
	}

	ASSERT(pend[i].pd_slurper);

	if (resphd->status ==
	    HADOOP__COMMON__RPC_RESPONSE_HEADER_PROTO__RPC_STATUS_PROTO__ERROR) {
		res->rs_msgno = (int64_t)resphd->callid;
		/* XXX: errordetail also potentially interesting */
		res->rs_obj = _object_exception(resphd->exceptionclassname,
		    resphd->errormsg);
		res->rs_size = rbuf.used;
		goto out; // HDFS_SUCCESS
	} else if (resphd->status !=
	    HADOOP__COMMON__RPC_RESPONSE_HEADER_PROTO__RPC_STATUS_PROTO__SUCCESS) {
		/* This shouldn't happen. */
		error = error_from_hdfs(HDFS_ERR_NAMENODE_PROTOCOL);
		goto out;
	}

	respsz = _bslurp_vlint(&rbuf);
	if (rbuf.used < 0)
		goto out;
	if (respsz > (rbuf.size - rbuf.used)) {
		error = HDFS_AGAIN;
		goto out;
	}

	rbuf.size = rbuf.used + respsz;
	res->rs_obj = pend[i].pd_slurper(&rbuf);
	if (res->rs_obj == NULL) {
		error = error_from_hdfs(HDFS_ERR_NAMENODE_PROTOCOL);
		goto out;
	}

	res->rs_msgno = (int64_t)resphd->callid;
	res->rs_size = rbuf.used;

out:
	if (resphd)
		hadoop__common__rpc_response_header_proto__free_unpacked(resphd, NULL);

	if (rbuf.used == _H_PARSE_EOF) {
		error = HDFS_AGAIN;
	} else if (rbuf.used == _H_PARSE_ERROR) {
		error = error_from_hdfs(HDFS_ERR_NAMENODE_PROTOCOL);
	}

	if (!hdfs_is_error(error)) {
		ASSERT(rbuf.used >= 0);
		ASSERT(res->rs_obj);
		ASSERT(res->rs_size == rbuf.used);
		if (rbuf.used != totalsz + 4) {
			error = error_from_hdfs(HDFS_ERR_NAMENODE_PROTOCOL);
			hdfs_object_free(res->rs_obj);
			res->rs_obj = NULL;
		}
	}

	return error;
}

EXPORT_SYM struct hdfs_object *
hdfs_object_slurp(struct hdfs_heap_buf *rbuf, enum hdfs_object_type realtype)
{
	struct hdfs_object *(*slurper)(struct hdfs_heap_buf *);

	slurper = object_types[realtype - _H_START].slurper;
	ASSERT(slurper);

	return slurper(rbuf);
}
