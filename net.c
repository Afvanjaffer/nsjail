/*

   nsjail - networking routines
   -----------------------------------------

   Copyright 2014 Google Inc. All Rights Reserved.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

*/
#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <netinet/ip6.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <libnl3/netlink/route/link.h>
#include <libnl3/netlink/route/link/macvlan.h>

#include "log.h"

static bool netCloneMacV(const char *type, const char *name, char *iface, int pid)
{
	struct nl_sock *sk;
	struct nl_cache *link_cache;
	int err, master_index;
	bool ret = false;

	sk = nl_socket_alloc();
	if ((err = nl_connect(sk, NETLINK_ROUTE)) < 0) {
		LOG_E("Unable to connect socket: %s", nl_geterror(err));
		goto out_sock;
	}

	struct rtnl_link *rmv = rtnl_link_macvlan_alloc();
	if (rmv == NULL) {
		LOG_E("rtnl_link_macvlan_alloc(): %s", nl_geterror(err));
		goto out_sock;
	}

	if ((err = rtnl_link_alloc_cache(sk, AF_UNSPEC, &link_cache)) < 0) {
		LOG_E("rtnl_link_alloc_cache(): %s", nl_geterror(err));
		goto out_link;
	}

	if (!(master_index = rtnl_link_name2i(link_cache, iface))) {
		LOG_E("rtnl_link_name2i(): %s", nl_geterror(master_index));
		goto out_cache;
	}

	rtnl_link_set_name(rmv, name);
	rtnl_link_set_link(rmv, master_index);
	rtnl_link_set_type(rmv, type);
	rtnl_link_set_ns_pid(rmv, pid);

	if ((err = rtnl_link_add(sk, rmv, NLM_F_CREATE)) < 0) {
		LOG_E("rtnl_link_add(): %s", nl_geterror(err));
		goto out_cache;
	}

	ret = true;
 out_cache:
	nl_cache_free(link_cache);
 out_link:
	rtnl_link_put(rmv);
 out_sock:
	nl_socket_free(sk);
	return ret;
}

bool netCloneNetIfaces(struct nsjconf_t * nsjconf, int pid)
{
	if (nsjconf->iface_macvtap != NULL) {
		if (netCloneMacV("macvtap", "vt0", nsjconf->iface_macvtap, pid) == false) {
			LOG_E("Couldn't setup 'macvtap' interface");
			return false;
		}
	}
	if (nsjconf->iface_macvlan != NULL) {
		if (netCloneMacV("macvlan", "vl0", nsjconf->iface_macvlan, pid) == false) {
			LOG_E("Couldn't setup 'macvtap' interface");
			return false;
		}
	}

	return true;
}

static bool netIsSocket(int fd)
{
	int optval;
	socklen_t optlen = sizeof(optval);
	int ret = getsockopt(fd, SOL_SOCKET, SO_TYPE, &optval, &optlen);
	if (ret == -1) {
		return false;
	}
	return true;
}

bool netLimitConns(struct nsjconf_t * nsjconf, int connsock)
{
	/* 0 means 'unlimited' */
	if (nsjconf->max_conns_per_ip == 0) {
		return true;
	}

	struct sockaddr_in6 addr;
	char cs_addr[64];
	netConnToText(connsock, true /* remote */ , cs_addr, sizeof(cs_addr), &addr);

	unsigned int cnt = 0;
	struct pids_t *p;
	LIST_FOREACH(p, &nsjconf->pids, pointers) {
		if (memcmp
		    (addr.sin6_addr.s6_addr, p->remote_addr.sin6_addr.s6_addr,
		     sizeof(*p->remote_addr.sin6_addr.s6_addr)) == 0) {
			cnt++;
		}
	}

	if (cnt >= nsjconf->max_conns_per_ip) {
		LOG_W("Rejecting connection from '%s', max_conns_per_ip limit reached: %u", cs_addr,
		      nsjconf->max_conns_per_ip);
		return false;
	}

	return true;
}

int netGetRecvSocket(int port)
{
	if (port < 1 || port > 65535) {
		LOG_F("TCP port %d out of bounds (0 <= port <= 65535)", port);
	}

	int sockfd = socket(AF_INET6, SOCK_STREAM, 0);
	if (sockfd == -1) {
		PLOG_E("socket(AF_INET6)");
		return -1;
	}
	int so = 1;
	if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &so, sizeof(so)) == -1) {
		PLOG_E("setsockopt(%d, SO_REUSEADDR)", sockfd);
		return -1;
	}
	struct sockaddr_in6 addr = {
		.sin6_family = AF_INET6,
		.sin6_port = htons(port),
		.sin6_flowinfo = 0,
		.sin6_addr = in6addr_any,
		.sin6_scope_id = 0,
	};
	if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
		PLOG_E("bind(port:%d)", port);
		return -1;
	}
	if (listen(sockfd, SOMAXCONN) == -1) {
		PLOG_E("listen(%d)", SOMAXCONN);
		return -1;
	}

	char ss_addr[64];
	netConnToText(sockfd, false /* remote */ , ss_addr, sizeof(ss_addr), NULL);
	LOG_I("Listening on %s", ss_addr);

	return sockfd;
}

int netAcceptConn(int listenfd)
{
	struct sockaddr_in6 cli_addr;
	socklen_t socklen = sizeof(cli_addr);
	int connfd = accept(listenfd, (struct sockaddr *)&cli_addr, &socklen);
	if (connfd == -1) {
		if (errno != EINTR) {
			PLOG_E("accept(%d)", listenfd);
		}
		return -1;
	}

	char cs_addr[64], ss_addr[64];
	netConnToText(connfd, true /* remote */ , cs_addr, sizeof(cs_addr), NULL);
	netConnToText(connfd, false /* remote */ , ss_addr, sizeof(ss_addr), NULL);
	LOG_I("New connection from: %s on: %s", cs_addr, ss_addr);

	int so = 1;
	if (setsockopt(connfd, SOL_TCP, TCP_CORK, &so, sizeof(so)) == -1) {
		PLOG_W("setsockopt(%d, TCP_CORK)", connfd);
	}
	return connfd;
}

void netConnToText(int fd, bool remote, char *buf, size_t s, struct sockaddr_in6 *addr_or_null)
{
	if (netIsSocket(fd) == false) {
		snprintf(buf, s, "[STANDALONE_MODE]");
		return;
	}

	struct sockaddr_in6 addr;
	socklen_t addrlen = sizeof(addr);
	if (remote) {
		if (getpeername(fd, (struct sockaddr *)&addr, &addrlen) == -1) {
			PLOG_W("getpeername(%d)", fd);
			snprintf(buf, s, "[unknown]");
			return;
		}
	} else {
		if (getsockname(fd, (struct sockaddr *)&addr, &addrlen) == -1) {
			PLOG_W("getsockname(%d)", fd);
			snprintf(buf, s, "[unknown]");
			return;
		}
	}

	if (addr_or_null) {
		memcpy(addr_or_null, &addr, sizeof(*addr_or_null));
	}

	char tmp[s];
	if (inet_ntop(AF_INET6, addr.sin6_addr.s6_addr, tmp, s) == NULL) {
		PLOG_W("inet_ntop()");
		snprintf(buf, s, "[unknown]:%hu", ntohs(addr.sin6_port));
		return;
	}
	snprintf(buf, s, "%s:%hu", tmp, ntohs(addr.sin6_port));
	return;
}
