#ifndef _T_MAIN_H
#define _T_MAIN_H

#ifndef nelem
# define nelem(ARR) (sizeof(ARR) / sizeof(ARR[0]))
#endif

#define HDFS_T_ENV "HDFS_TEST_NODE_ADDRESS"
#define HDFS_T_USER "HDFS_TEST_USER"

Suite *		t_hl_rpc_basics_suite(void);

extern const char *H_ADDR, *H_USER;

#endif