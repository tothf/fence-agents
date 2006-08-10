/******************************************************************************
*******************************************************************************
**
**  Copyright (C) 2004-2006 Red Hat, Inc.  All rights reserved.
**
**  This copyrighted material is made available to anyone wishing to use,
**  modify, copy, or redistribute it subject to the terms and conditions
**  of the GNU General Public License v.2.
**
*******************************************************************************
******************************************************************************/

#include <sys/poll.h>
#include <sys/types.h>
#include <sys/errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <syslog.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <signal.h>

/* openais headers */
#include <openais/totem/totemip.h>
#include <openais/totem/totempg.h>
#include <openais/totem/aispoll.h>
#include <openais/service/service.h>
#include <openais/service/config.h>
#include <openais/lcr/lcr_comp.h>
#include <openais/service/swab.h>

#include "cnxman-socket.h"
#include "commands.h"
#include "logging.h"

#include "ais.h"
#include "cmanccs.h"
#include "daemon.h"

extern int our_nodeid();
extern char cluster_name[MAX_CLUSTER_NAME_LEN+1];
extern char *key_filename;
extern unsigned int quorumdev_poll;
extern unsigned int ccsd_poll_interval;
extern unsigned int shutdown_timeout;
extern int init_config(struct objdb_iface_ver0 *objdb);

struct totem_ip_address mcast_addr[MAX_INTERFACES];
struct totem_ip_address ifaddrs[MAX_INTERFACES];
int num_interfaces;
uint64_t incarnation;

static int config_run;
static char errorstring[512];
static unsigned int debug_mask;
static struct objdb_iface_ver0 *global_objdb;
static totempg_groups_handle group_handle;
static struct totempg_group cman_group[1] = {
        { .group          = "CMAN", .group_len      = 4},
};

/* This structure is tacked onto the start of a cluster message packet for our
 * own nefarious purposes. */
struct cl_protheader {
	unsigned char  tgtport; /* Target port number */
	unsigned char  srcport; /* Source (originating) port number */
	unsigned short pad;
	unsigned int   flags;
	int            srcid;	/* Node ID of the sender */
	int            tgtid;	/* Node ID of the target */
};

static int comms_init_ais(struct objdb_iface_ver0 *objdb);
static void cman_deliver_fn(unsigned int nodeid, struct iovec *iovec, int iov_len,
			    int endian_conversion_required);
static void cman_confchg_fn(enum totem_configuration_type configuration_type,
			    unsigned int *member_list, int member_list_entries,
			    unsigned int *left_list, int left_list_entries,
			    unsigned int *joined_list, int joined_list_entries,
			    struct memb_ring_id *ring_id);
static int cman_readconfig(struct objdb_iface_ver0 *objdb, char **error_string);

/* Plugin-specific code */
/* Need some better way of determining these.... */
#define CMAN_SERVICE 9

static int cman_exit_fn(void *conn_info);
static int cman_exec_init_fn(struct objdb_iface_ver0 *objdb);

/* These just makes the code below a little neater */
static inline int objdb_get_string(struct objdb_iface_ver0 *objdb, unsigned int object_service_handle,
				   char *key, char **value)
{
	int res;

	*value = NULL;
	if ( !(res = objdb->object_key_get(object_service_handle,
					   key,
					   strlen(key),
					   (void *)value,
					   NULL))) {
		if (*value)
			return 0;
	}
	return -1;
}

static inline void objdb_get_int(struct objdb_iface_ver0 *objdb, unsigned int object_service_handle,
				   char *key, unsigned int *intvalue)
{
	char *value = NULL;

	if (!objdb->object_key_get(object_service_handle, key, strlen(key),
				   (void *)&value, NULL)) {
		if (value) {
			*intvalue = atoi(value);
		}
	}
}


/*
 * Exports the interface for the service
 */
static struct openais_service_handler cman_service_handler = {
	.name		    		= (unsigned char *)"openais CMAN membership service 2.01",
	.id			        = CMAN_SERVICE,
	.lib_exit_fn		       	= cman_exit_fn,
	.exec_init_fn		       	= cman_exec_init_fn,
	.config_init_fn                 = NULL,
};

static struct openais_service_handler *cman_get_handler_ver0(void)
{
	return (&cman_service_handler);
}

static struct openais_service_handler_iface_ver0 cman_service_handler_iface = {
	.openais_get_service_handler_ver0 = cman_get_handler_ver0
};



static struct config_iface_ver0 cmanconfig_iface_ver0 = {
	.config_readconfig        = cman_readconfig
};

static struct lcr_iface ifaces_ver0[2] = {
	{
		.name		       	= "cmanconfig",
		.version	       	= 0,
		.versions_replace      	= 0,
		.versions_replace_count	= 0,
		.dependencies	       	= 0,
		.dependency_count      	= 0,
		.constructor	       	= NULL,
		.destructor	       	= NULL,
		.interfaces	       	= NULL,
	},
	{
		.name		        = "openais_cman",
		.version	        = 0,
		.versions_replace     	= 0,
		.versions_replace_count = 0,
		.dependencies	      	= 0,
		.dependency_count      	= 0,
		.constructor	       	= NULL,
		.destructor		= NULL,
		.interfaces		= NULL,
	}
};

static struct lcr_comp cman_comp_ver0 = {
	.iface_count				= 2,
	.ifaces					= ifaces_ver0,
};



__attribute__ ((constructor)) static void cman_comp_register(void) {
	lcr_interfaces_set(&ifaces_ver0[0], &cmanconfig_iface_ver0);
	lcr_interfaces_set(&ifaces_ver0[1], &cman_service_handler_iface);
	lcr_component_register(&cman_comp_ver0);
}

/* ------------------------------- */

static int cman_readconfig(struct objdb_iface_ver0 *objdb, char **error_string)
{
	int error;

	/* Initialise early logging */
	if (getenv("CMAN_DEBUGLOG"))
	{
		debug_mask = atoi(getenv("CMAN_DEBUGLOG"));
	}

	init_debug(debug_mask);

	/* We need to set this up to internal defaults too early */
	openlog("openais", LOG_CONS|LOG_PID, LOG_LOCAL4);

	global_objdb = objdb;

	/* Read low-level totem/aisexec etc config from CCS */
	init_config(objdb);

	/* Read cman-specific config from CCS */
	error = read_ccs_config();
	if (error)
	{
		sprintf(errorstring, "Error reading config from CCS");
		return -1;
	}

	/* Do config overrides */
	comms_init_ais(objdb);

	config_run = 1;

	return 0;
}

static int cman_exec_init_fn(struct objdb_iface_ver0 *objdb)
{
	unsigned int object_handle;

	/* We can only work if our config inerface was run first */
	if (!config_run)
		return 0;

	/* Get our config variable */
	objdb->object_find_reset(OBJECT_PARENT_HANDLE);
	if (objdb->object_find(OBJECT_PARENT_HANDLE, "cman", strlen("cman"), &object_handle) == 0)
	{
		objdb_get_int(objdb, object_handle, "quorum_dev_poll", &quorumdev_poll);
		objdb_get_int(objdb, object_handle, "shutdown_timeout", &shutdown_timeout);
		objdb_get_int(objdb, object_handle, "ccsd_poll", &ccsd_poll_interval);

		/* Only use the CCS version of this if it was not overridden on the command-line */
		if (!getenv("CMAN_DEBUGLOG"))
		{
			objdb_get_int(objdb, object_handle, "debug_mask", &debug_mask);
			init_debug(debug_mask);
		}
	}

	/* Open local sockets and initialise I/O queues */
	cman_init();

	/* Start totem */
	totempg_groups_initialize(&group_handle, cman_deliver_fn, cman_confchg_fn);
	totempg_groups_join(group_handle, cman_group, 1);

	return 0;
}


int cman_exit_fn(void *conn_info)
{
	cman_finish();
	return 0;
}

/* END Plugin-specific code */

int ais_add_ifaddr(char *mcast, char *ifaddr, int portnum)
{
	unsigned int totem_object_handle;
	unsigned int interface_object_handle;
	char tmp[132];
	int ret = -1;

	P_AIS("Adding local address %s\n", ifaddr);

	/* This will already exist as early config creates it */
	global_objdb->object_find_reset(OBJECT_PARENT_HANDLE);
	if (global_objdb->object_find(OBJECT_PARENT_HANDLE,
				      "totem", strlen("totem"),&totem_object_handle) == 0) {

		if (global_objdb->object_create(totem_object_handle, &interface_object_handle,
						"interface", strlen("interface")) == 0) {

			P_AIS("Setting if %d, name: %s,  mcast: %s,  port=%d, \n",
			      num_interfaces, ifaddr, mcast, portnum);
			sprintf(tmp, "%d", num_interfaces);
			global_objdb->object_key_create(interface_object_handle, "ringnumber", strlen("ringnumber"),
							tmp, strlen(tmp)+1);

			global_objdb->object_key_create(interface_object_handle, "bindnetaddr", strlen("bindnetaddr"),
							ifaddr, strlen(ifaddr)+1);

			global_objdb->object_key_create(interface_object_handle, "mcastaddr", strlen("mcastaddr"),
							mcast, strlen(mcast)+1);

			sprintf(tmp, "%d", portnum);
			global_objdb->object_key_create(interface_object_handle, "mcastport", strlen("mcastport"),
							tmp, strlen(tmp)+1);

			/* Save a local copy */
			ret = totemip_parse(&mcast_addr[num_interfaces], mcast, 0);
			if (!ret)
				ret = totemip_parse(&ifaddrs[num_interfaces], ifaddr,
						    mcast_addr[num_interfaces].family);
			if (!ret)
				num_interfaces++;
			else
				return ret;
		}
	}

	return ret;
}


int comms_send_message(void *buf, int len,
		       unsigned char toport, unsigned char fromport,
		       int nodeid,
		       unsigned int flags)
{
	struct iovec iov[2];
	struct cl_protheader header;
	int totem_flags = TOTEMPG_AGREED;

	P_AIS("comms send message %p len = %d\n", buf,len);
	header.tgtport = toport;
	header.srcport = fromport;
	header.flags   = flags;
	header.srcid   = our_nodeid();
	header.tgtid   = nodeid;

	iov[0].iov_base = &header;
	iov[0].iov_len  = sizeof(header);
	iov[1].iov_base = buf;
	iov[1].iov_len  = len;

	if (flags & MSG_TOTEM_SAFE)
		totem_flags = TOTEMPG_SAFE;

	return totempg_groups_mcast_joined(group_handle, iov, 2, totem_flags);
}

// This assumes the iovec has only one element ... is it true ??
static void cman_deliver_fn(unsigned int nodeid, struct iovec *iovec, int iov_len,
			    int endian_conversion_required)
{
	struct cl_protheader *header = iovec->iov_base;
	char *buf = iovec->iov_base;

	P_AIS("deliver_fn called, iov_len = %d, iov[0].len = %d, source nodeid = %d, conversion reqd=%d\n",
	      iov_len, iovec->iov_len, nodeid, endian_conversion_required);

	if (endian_conversion_required) {
		header->srcid = swab32(header->srcid);
		header->tgtid = swab32(header->tgtid);
		header->flags = swab32(header->flags);
	}

	/* Only pass on messages for us or everyone */
	if (header->tgtid == our_nodeid() ||
	    header->tgtid == 0) {
		send_to_userport(header->srcport, header->tgtport,
				 header->srcid, header->tgtid,
				 buf + sizeof(struct cl_protheader),
				 iovec->iov_len - sizeof(struct cl_protheader),
				 endian_conversion_required);
	}
}

static void cman_confchg_fn(enum totem_configuration_type configuration_type,
			    unsigned int *member_list, int member_list_entries,
			    unsigned int *left_list, int left_list_entries,
			    unsigned int *joined_list, int joined_list_entries,
			    struct memb_ring_id *ring_id)
{
	int i;
	static int last_memb_count = 0;

	P_AIS("confchg_fn called type = %d, seq=%lld\n", configuration_type, ring_id->seq);

	incarnation = ring_id->seq;

	/* Tell the cman membership layer */
	for (i=0; i<left_list_entries; i++)
		del_ais_node(left_list[i]);
	for (i=0; i<joined_list_entries; i++)
		add_ais_node(joined_list[i], incarnation, member_list_entries);

	if (configuration_type == TOTEM_CONFIGURATION_REGULAR) {
		P_AIS("last memb_count = %d, current = %d\n", last_memb_count, member_list_entries);
		send_transition_msg(last_memb_count);
		last_memb_count = member_list_entries;
	}
}

/* These are basically our overrides to the totem config bits */
static int comms_init_ais(struct objdb_iface_ver0 *objdb)
{
	unsigned int object_handle;
	char tmp[256];

	P_AIS("comms_init_ais()\n");

	objdb->object_find_reset(OBJECT_PARENT_HANDLE);

	if (objdb->object_find(OBJECT_PARENT_HANDLE,
			       "totem", strlen("totem"),
			       &object_handle) == 0)
	{
		objdb->object_key_create(object_handle, "version", strlen("version"),
					 "2", 2);

		sprintf(tmp, "%d", our_nodeid());
		objdb->object_key_create(object_handle, "nodeid", strlen("nodeid"),
					 tmp, strlen(tmp)+1);

		objdb->object_key_create(object_handle, "vsftype", strlen("vsftype"),
					 "none", strlen("none")+1);

		/* Set RRP mode appropriately */
		if (num_interfaces > 1) {
			global_objdb->object_key_create(object_handle, "rrp_mode", strlen("rrp_mode"),
							"active", strlen("active")+1);
		}
		else {
			global_objdb->object_key_create(object_handle, "rrp_mode", strlen("rrp_mode"),
							"none", strlen("none")+1);
		}

		sprintf(tmp, "%d", 1);
		objdb->object_key_create(object_handle, "secauth", strlen("secauth"),
					 tmp, strlen(tmp)+1);

		if (key_filename)
		{
			objdb->object_key_create(object_handle, "keyfile", strlen("keyfile"),
						 key_filename, strlen(key_filename)+1);
		}
		else /* Use the cluster name as key,
		      * This isn't a good isolation strategey but it does make sure that
		      * clusters on the same port/multicast by mistake don't actually interfere
		      * and that we have some form of encryption going.
		      */
		{
			int keylen;
			memset(tmp, 0, sizeof(tmp));

			strcpy(tmp, cluster_name);

			/* Key length must be a multiple of 4 */
			keylen = (strlen(cluster_name)+4) & 0xFC;
			objdb->object_key_create(object_handle, "key", strlen("key"),
						 tmp, keylen);
		}
	}

	/* Make sure mainconfig doesn't stomp on our logging options */
	objdb->object_find_reset(OBJECT_PARENT_HANDLE);
	if (objdb->object_find(OBJECT_PARENT_HANDLE,
			       "logging", strlen("logging"),
			       &object_handle) == 0)
	{
		unsigned int logger_object_handle;
		char *logstr;

		/* Default logging facility is "local4" unless overridden by the user */
		if (!objdb_get_string(objdb, object_handle, "syslog_facility", &logstr)) {
			objdb->object_key_create(object_handle, "syslog_facility", strlen("syslog_facility"),
						 "local4", strlen("local4")+1);
		}

		objdb->object_create(object_handle, &logger_object_handle,
				      "logger", strlen("logger"));
		objdb->object_key_create(logger_object_handle, "ident", strlen("ident"),
					 "CMAN", strlen("CMAN")+1);

		if (debug_mask)
		{
			objdb->object_key_create(object_handle, "to_stderr", strlen("to_stderr"),
						 "yes", strlen("yes")+1);
			objdb->object_key_create(logger_object_handle, "debug", strlen("debug"),
						 "on", strlen("on")+1);
		}
		else
		{
			objdb->object_key_create(object_handle, "to_syslog", strlen("to_syslog"),
						 "yes", strlen("yes")+1);
		}
	}

	/* Don't run under user "ais" */
	objdb->object_find_reset(OBJECT_PARENT_HANDLE);
	if (objdb->object_find(OBJECT_PARENT_HANDLE, "aisexec", strlen("aisexec"), &object_handle) == 0)
	{
		objdb->object_key_create(object_handle, "user", strlen("user"),
				 "root", strlen("root") + 1);
		objdb->object_key_create(object_handle, "group", strlen("group"),
				 "root", strlen("root") + 1);
	}

	/* Make sure we load our alter-ego */
	objdb->object_create(OBJECT_PARENT_HANDLE, &object_handle,
			     "service", strlen("service"));
	objdb->object_key_create(object_handle, "name", strlen("name"),
				 "openais_cman", strlen("openais_cman") + 1);
	objdb->object_key_create(object_handle, "ver", strlen("ver"),
				 "0", 2);

	return 0;
}
