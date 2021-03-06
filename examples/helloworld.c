#include <assert.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hadoofus/lowlevel.h>
#include <hadoofus/objects.h>

int
main(int argc, char **argv)
{
	const char
	      *host = "localhost",
	      *port = "8020";

	struct hdfs_namenode namenode;
	struct hdfs_error err;

	struct hdfs_object *rpc;
	struct hdfs_object *object;
	int64_t msgno_invoke, msgno_recv;

	if (argc > 1) {
		if (strcmp(argv[1], "-h") == 0) {
			printf("Usage: ./helloworld [host [port]]\n");
			exit(0);
		}
		host = argv[1];
		if (argc > 2)
			port = argv[2];
	}

	// Initialize the connection object and connect to the local namenode
	hdfs_namenode_init(&namenode, HDFS_NO_KERB);
	err = hdfs_namenode_connect(&namenode, host, port);
	if (hdfs_is_error(err))
		goto out;

	// Pretend to be the user "mapred"
	err = hdfs_namenode_authenticate(&namenode, "mapred");
	if (hdfs_is_error(err))
		goto out;

	// Call getProtocolVersion(61)
	rpc = hdfs_rpc_invocation_new(
	    "getProtocolVersion",
	    hdfs_string_new(HADOOFUS_CLIENT_PROTOCOL_STR),
	    hdfs_long_new(61),
	    NULL);
	err = hdfs_namenode_invoke(&namenode, rpc, &msgno_invoke, NULL);
	hdfs_object_free(rpc);
	while (hdfs_is_again(err)) {
		err = hdfs_namenode_invoke_continue(&namenode);
	}
	if (hdfs_is_error(err))
		goto out;

	// Get the response (should be long(61))
	do {
		err = hdfs_namenode_recv(&namenode, &object, &msgno_recv, NULL);
		if (hdfs_is_again(err)) {
			struct hdfs_error ret;
			struct pollfd pfd;

			ret = hdfs_namenode_get_eventfd(&namenode, &pfd.fd, &pfd.events);
			if (hdfs_is_error(ret)) {
				err = ret;
				goto out;
			}
			/* RC */poll(&pfd, 1, -1);
		}
	} while (hdfs_is_again(err));
	if (hdfs_is_error(err))
		goto out;
	assert(msgno_recv == msgno_invoke);

	if (object->ob_type == H_LONG &&
	    object->ob_val._long._val == 61L)
		printf("success\n");
	else
		printf("bad result\n");

	hdfs_object_free(object);

out:
	if (hdfs_is_error(err))
		fprintf(stderr, "hdfs error (%s): %s\n",
		    hdfs_error_str_kind(err), hdfs_error_str(err));

	// Destroy any resources used by the connection
	hdfs_namenode_destroy(&namenode);

	return hdfs_is_error(err);
}
