#include "dlm_daemon.h"
#include <corosync/corotypes.h>
#include <corosync/cfg.h>
#include <corosync/quorum.h>
#include "libfenced.h"

static corosync_cfg_handle_t	ch;
static quorum_handle_t		qh;
static uint32_t			old_nodes[MAX_NODES];
static int			old_node_count;
static uint32_t			quorum_nodes[MAX_NODES];
static int			quorum_node_count;

static int is_member(uint32_t *node_list, int count, uint32_t nodeid)
{
	int i;

	for (i = 0; i < count; i++) {
		if (node_list[i] == nodeid)
			return 1;
	}
	return 0;
}

static int is_old_member(uint32_t nodeid)
{
	return is_member(old_nodes, old_node_count, nodeid);
}

int is_cluster_member(uint32_t nodeid)
{
	return is_member(quorum_nodes, quorum_node_count, nodeid);
}

static void quorum_callback(quorum_handle_t h, uint32_t quorate,
			    uint64_t ring_seq, uint32_t node_list_entries,
			    uint32_t *node_list)
{
	corosync_cfg_node_address_t addrs[MAX_NODE_ADDRESSES];
	corosync_cfg_node_address_t *addrptr = addrs;
	cs_error_t err;
	int i, j, num_addrs;


	cluster_quorate = quorate;
	cluster_ringid_seq = (uint32_t)ring_seq;

	log_debug("cluster quorum %u seq %u nodes %u",
		  cluster_quorate, cluster_ringid_seq, node_list_entries);

	old_node_count = quorum_node_count;
	memcpy(&old_nodes, &quorum_nodes, sizeof(old_nodes));

	quorum_node_count = 0;
	memset(&quorum_nodes, 0, sizeof(quorum_nodes));

	for (i = 0; i < node_list_entries; i++)
		quorum_nodes[quorum_node_count++] = node_list[i];

	for (i = 0; i < old_node_count; i++) {
		if (!is_cluster_member(old_nodes[i])) {
			log_debug("cluster node %u removed seq %u",
				  old_nodes[i], cluster_ringid_seq);
			node_history_cluster_remove(old_nodes[i]);
			del_configfs_node(old_nodes[i]);
		}
	}

	for (i = 0; i < quorum_node_count; i++) {
		if (!is_old_member(quorum_nodes[i])) {
			log_debug("cluster node %u added seq %u",
				  quorum_nodes[i], cluster_ringid_seq);
			node_history_cluster_add(quorum_nodes[i]);

			err = corosync_cfg_get_node_addrs(ch, quorum_nodes[i],
							  MAX_NODE_ADDRESSES,
							  &num_addrs, addrs);
			if (err != CS_OK) {
				log_error("corosync_cfg_get_node_addrs failed "
					  "nodeid %u", quorum_nodes[i]);
				continue;
			}

			for (j = 0; j < num_addrs; j++) {
				add_configfs_node(quorum_nodes[i],
						  addrptr[j].address,
						  addrptr[j].address_length,
						  (quorum_nodes[i] ==
						   our_nodeid));
			}
		}
	}
}

static quorum_callbacks_t quorum_callbacks =
{
	.quorum_notify_fn = quorum_callback,
};

void process_cluster(int ci)
{
	cs_error_t err;

	err = quorum_dispatch(qh, CS_DISPATCH_ALL);
	if (err != CS_OK)
		cluster_dead(0);
}

/* Force re-read of quorum nodes */
void update_cluster(void)
{
	cs_error_t err;

	err = quorum_dispatch(qh, CS_DISPATCH_ONE);
	if (err != CS_OK)
		cluster_dead(0);
}

int setup_cluster(void)
{
	cs_error_t err;
	int fd;

	err = quorum_initialize(&qh, &quorum_callbacks);
	if (err != CS_OK) {
		log_error("quorum init error %d", err);
		return -1;
	}

	err = quorum_fd_get(qh, &fd);
	if (err != CS_OK) {
		log_error("quorum fd_get error %d", err);
		goto fail;
	}

	err = quorum_trackstart(qh, CS_TRACK_CHANGES);
	if (err != CS_OK) {
		log_error("quorum trackstart error %d", err);
		goto fail;
	}

	old_node_count = 0;
	memset(&old_nodes, 0, sizeof(old_nodes));
	quorum_node_count = 0;
	memset(&quorum_nodes, 0, sizeof(quorum_nodes));

	return fd;
 fail:
	quorum_finalize(qh);
	return -1;
}

void close_cluster(void)
{
	quorum_trackstop(qh);
	quorum_finalize(qh);
}

void kick_node_from_cluster(int nodeid)
{
	if (!nodeid) {
		log_error("telling corosync to shut down cluster locally");
		corosync_cfg_try_shutdown(ch,
				COROSYNC_CFG_SHUTDOWN_FLAG_IMMEDIATE);
	} else {
		log_error("telling corosync to remove nodeid %d from cluster",
			  nodeid);
		corosync_cfg_kill_node(ch, nodeid, "dlm_controld");
	}
}

static void shutdown_callback(corosync_cfg_handle_t h,
			      corosync_cfg_shutdown_flags_t flags)
{
	if (flags & COROSYNC_CFG_SHUTDOWN_FLAG_REQUEST) {
		if (list_empty(&lockspaces))
			corosync_cfg_replyto_shutdown(ch,
					COROSYNC_CFG_SHUTDOWN_FLAG_YES);
		else {
			log_debug("no to corosync shutdown");
			corosync_cfg_replyto_shutdown(ch,
					COROSYNC_CFG_SHUTDOWN_FLAG_NO);
		}
	}
}

static corosync_cfg_callbacks_t cfg_callbacks =
{
	.corosync_cfg_shutdown_callback = shutdown_callback,
	.corosync_cfg_state_track_callback = NULL,
};

void process_cluster_cfg(int ci)
{
	cs_error_t err;

	err = corosync_cfg_dispatch(ch, CS_DISPATCH_ALL);
	if (err != CS_OK)
		cluster_dead(0);
}

int setup_cluster_cfg(void)
{
	cs_error_t err;
	unsigned int nodeid;
	int fd;

	err = corosync_cfg_initialize(&ch, &cfg_callbacks);
	if (err != CS_OK) {
		log_error("corosync cfg init error %d", err);
		return -1;
	}

	err = corosync_cfg_fd_get(ch, &fd);
	if (err != CS_OK) {
		log_error("corosync cfg fd_get error %d", err);
		corosync_cfg_finalize(ch);
		return -1;
	}

	err = corosync_cfg_local_get(ch, &nodeid);
	if (err != CS_OK) {
		log_error("corosync cfg local_get error %d", err);
		corosync_cfg_finalize(ch);
		return -1;
	}
	our_nodeid = nodeid;
	log_debug("our_nodeid %d", our_nodeid);

	return fd;
}

void close_cluster_cfg(void)
{
	corosync_cfg_finalize(ch);
}

int fence_node_time(int nodeid, uint64_t *last_fenced_time)
{
	struct fenced_node nodeinfo;
	int rv;

	memset(&nodeinfo, 0, sizeof(nodeinfo));

	rv = fenced_node_info(nodeid, &nodeinfo);
	if (rv < 0)
		return rv;

	*last_fenced_time = nodeinfo.last_fenced_time;
	return 0;
}

int fence_in_progress(int *count)
{
	struct fenced_domain domain;
	int rv;

	memset(&domain, 0, sizeof(domain));

	rv = fenced_domain_info(&domain);
	if (rv < 0)
		return rv;

	*count = domain.victim_count;
	return 0;
}

