/*

   nsjail - cmdline parsing

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

#include "cmdline.h"

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <strings.h>
#include <sys/personality.h>
#include <sys/time.h>
#include <unistd.h>

#include "common.h"
#include "log.h"

struct custom_option {
	struct option opt;
	const char *descr;
};

static const char *logYesNo(bool yes)
{
	return (yes ? "true" : "false");
}

static void cmdlineHelp(const char *pname, struct custom_option *opts)
{
	LOG_HELP_BOLD("Usage: %s [options] -- path_to_command [args]", pname);
	LOG_HELP_BOLD("Options:");
	for (int i = 0; opts[i].opt.name; i++) {
		if (isprint(opts[i].opt.val)) {
			LOG_HELP_BOLD(" --%s%s%c %s", opts[i].opt.name,
				      "|-", opts[i].opt.val, opts[i].opt.has_arg == required_argument ? "[val]" : "");
		} else {
			LOG_HELP_BOLD(" --%s %s", opts[i].opt.name,
				      opts[i].opt.has_arg == required_argument ? "[val]" : "");
		}
		LOG_HELP("\t%s", opts[i].descr);
	}
}

void cmdlineLogParams(struct nsjconf_t *nsjconf)
{
	switch (nsjconf->mode) {
	case MODE_LISTEN_TCP:
		LOG_I("Mode: LISTEN_TCP");
		break;
	case MODE_STANDALONE_ONCE:
		LOG_I("Mode: STANDALONE_ONCE");
		break;
	case MODE_STANDALONE_RERUN:
		LOG_I("Mode: STANDALONE_RERUN");
		break;
	default:
		LOG_F("Mode: UNKNOWN");
		break;
	}

	LOG_I
	    ("Jail parameters: hostname:'%s', chroot:'%s', process:'%s', port:%d, "
	     "max_conns_per_ip:%u, uid:%u, gid:%u, time_limit:%ld, personality:%#lx, daemonize:%s, "
	     "clone_newnet:%s, clone_newuser:%s, clone_newns:%s, clone_newpid:%s, "
	     "clone_newipc:%s, clonew_newuts:%s, apply_sandbox:%s, keep_caps:%s",
	     nsjconf->hostname, nsjconf->chroot, nsjconf->argv[0], nsjconf->port,
	     nsjconf->max_conns_per_ip, nsjconf->uid, nsjconf->gid, nsjconf->tlimit,
	     nsjconf->personality, logYesNo(nsjconf->daemonize), logYesNo(nsjconf->clone_newnet),
	     logYesNo(nsjconf->clone_newuser), logYesNo(nsjconf->clone_newns),
	     logYesNo(nsjconf->clone_newpid), logYesNo(nsjconf->clone_newipc),
	     logYesNo(nsjconf->clone_newuts), logYesNo(nsjconf->apply_sandbox), logYesNo(nsjconf->keep_caps));
	for (size_t i = 0; i < nsjconf->bindmountpts->fs_count; i++) {
		LOG_I("Additional bind mount point: '%s'", nsjconf->bindmountpts->mountpt[i]);
	}
	for (size_t i = 0; i < nsjconf->tmpfsmountpts->fs_count; i++) {
		LOG_I("Additional tmpfs mount point: '%s'", nsjconf->tmpfsmountpts->mountpt[i]);
	}
}

static void cmdlineUsage(const char *pname, struct custom_option *opts)
{
	cmdlineHelp(pname, opts);
	exit(0);
}

static bool cmdlineIsANumber(const char *s)
{
	for (int i = 0; s[i]; s++) {
		if (!isdigit(s[i]) && s[i] != 'x') {
			return false;
		}
	}
	return true;
}

rlim_t cmdlineParseRLimit(int res, const char *optarg, unsigned long mul)
{
	struct rlimit cur;
	if (getrlimit(res, &cur) == -1) {
		PLOG_F("getrlimit(%d)", res);
	}
	if (strcasecmp(optarg, "max") == 0) {
		return cur.rlim_max;
	}
	if (strcasecmp(optarg, "def") == 0) {
		return cur.rlim_cur;
	}
	if (cmdlineIsANumber(optarg) == false) {
		LOG_F("RLIMIT %d needs a numeric or 'max'/'def' value ('%s' provided)", res, optarg);
	}
	rlim_t val = strtoul(optarg, NULL, 0) * mul;
	if (val == ULONG_MAX && errno != 0) {
		PLOG_F("strtoul('%s', 0)", optarg);
	}
	return val;
}

bool cmdlineParse(int argc, char *argv[], struct nsjconf_t * nsjconf)
{
	LIST_INIT(&nsjconf->pids);
	const char *user = "nobody";
	const char *group = "nobody";
	const char *logfile = NULL;
	nsjconf->bindmountpts = malloc(sizeof(*(nsjconf->bindmountpts)));
	if (nsjconf->bindmountpts == NULL) {
		LOG_F("malloc");
	}
	nsjconf->bindmountpts->fs_count = 0;
	nsjconf->tmpfsmountpts = malloc(sizeof(*(nsjconf->bindmountpts)));
	if (nsjconf->tmpfsmountpts == NULL) {
		LOG_F("malloc");
	}
	nsjconf->tmpfsmountpts->fs_count = 0;

        /*  *INDENT-OFF* */
	struct custom_option custom_opts[] = {
		{{"help", no_argument, NULL, 'h'}, "Help plz.."},
		{{"mode", required_argument, NULL, 'M'}, "Execution mode (default: l [MODE_LISTEN_TCP]):\n"
			"\tl: Listen to connections on a TCP port (specified with --port) [MODE_LISTEN_TCP]\n"
			"\to: Immediately launch a single process on a console [MODE_STANDALONE_ONCE]\n"
			"\tr: Immediately launch a single process on a console, keep doing it forever [MODE_STANDALONE_RERUN]"},
		{{"chroot", required_argument, NULL, 'c'}, "Directory containing / of the jail (default: '/chroot')"},
		{{"user", required_argument, NULL, 'u'}, "Username/uid of processess inside the jail (default: 'nobody')"},
		{{"group", required_argument, NULL, 'g'}, "Groupname/gid of processess inside the jail (default: 'nogroup')"},
		{{"hostname", required_argument, NULL, 'H'}, "UTS name (hostname) of the jail (default: 'NSJAIL')"},
		{{"port", required_argument, NULL, 'p'}, "TCP port to bind to (only in [MODE_LISTEN_TCP]) (default: 31337)"},
		{{"max_conns_per_ip", required_argument, NULL, 'i'}, "Maximum number of connections per one IP (default: 0 (unlimited))"},
		{{"log", required_argument, NULL, 'l'}, "Log file (default: stderr)"},
		{{"time_limit", required_argument, NULL, 't'}, "Maximum time that a jail can exist, in seconds (default: 600)"},
		{{"daemon", no_argument, NULL, 'd'}, "Daemonize after start? (default: false)"},
		{{"verbose", no_argument, NULL, 'v'}, "Verbose output (default: false)"},
		{{"keep_env", no_argument, NULL, 'e'}, "Should all environment variables be passed to the child? (default: false)"},
		{{"keep_caps", no_argument, NULL, 0x0502}, "Don't drop capabilities (DANGEROUS) (default: false)"},
		{{"rlimit_as", required_argument, NULL, 0x0201}, "RLIMIT_AS in MB, 'max' for RLIM_INFINITY, 'def' for the current value (default: 512)"},
		{{"rlimit_core", required_argument, NULL, 0x0202}, "RLIMIT_CORE in MB, 'max' for RLIM_INFINITY, 'def' for the current value (default: 0)"},
		{{"rlimit_cpu", required_argument, NULL, 0x0203}, "RLIMIT_CPU, 'max' for RLIM_INFINITY, 'def' for the current value (default: 600)"},
		{{"rlimit_fsize", required_argument, NULL, 0x0204}, "RLIMIT_FSIZE in MB, 'max' for RLIM_INFINITY, 'def' for the current value (default: 1)"},
		{{"rlimit_nofile", required_argument, NULL, 0x0205}, "RLIMIT_NOFILE, 'max' for RLIM_INFINITY, 'def' for the current value (default: 32)"},
		{{"rlimit_nproc", required_argument, NULL, 0x0206}, "RLIMIT_NPROC, 'max' for RLIM_INFINITY, 'def' for the current value (default: 'def')"},
		{{"rlimit_stack", required_argument, NULL, 0x0207}, "RLIMIT_STACK in MB, 'max' for RLIM_INFINITY, 'def' for the current value (default: 'def')"},
		{{"persona_addr_compat_layout", no_argument, NULL, 0x0301}, "personality(ADDR_COMPAT_LAYOUT) (default: false)"},
		{{"persona_mmap_page_zero", no_argument, NULL, 0x0302}, "personality(MMAP_PAGE_ZERO) (default: false)"},
		{{"persona_read_implies_exec", no_argument, NULL, 0x0303}, "personality(READ_IMPLIES_EXEC) (default: false)"},
		{{"persona_addr_limit_3gb", no_argument, NULL, 0x0304}, "personality(ADDR_LIMIT_3GB) (default: false)"},
		{{"persona_addr_no_randomize", no_argument, NULL, 0x0305}, "personality(ADDR_NO_RANDOMIZE) (default: false)"},
		{{"disable_clone_newnet", no_argument, NULL, 'N'}, "Enable networking inside the jail (default: false)"},
		{{"disable_clone_newuser", no_argument, NULL, 0x0402}, "Don't use CLONE_NEWUSER (default: false)"},
		{{"disable_clone_newns", no_argument, NULL, 0x0403}, "Don't use CLONE_NEWNS (default: false)"},
		{{"disable_clone_newpid", no_argument, NULL, 0x0404}, "Don't use CLONE_NEWPID (default: false)"},
		{{"disable_clone_newipc", no_argument, NULL, 0x0405}, "Don't use CLONE_NEWIPC (default: false)"},
		{{"disable_clone_newuts", no_argument, NULL, 0x0406}, "Don't use CLONE_NEWUTS (default: false)"},
		{{"disable_sandbox", no_argument, NULL, 0x0501}, "Don't enable the seccomp-bpf sandboxing (default: false)"},
		{{"rw", no_argument, NULL, 0x0503}, "Mount / as RW (default: RO)"},
		{{"silent", no_argument, NULL, 0x0504}, "Redirect child's fd:0/1/2 to /dev/null (default: false)"},
		{{"bindmount", required_argument, NULL, 'B'}, "List of mountpoints to be mounted --bind inside the container. Can be specified multiple times (default: none)"},
		{{"tmpfsmount", required_argument, NULL, 'T'}, "List of mountpoints to be mounted as RW/tmpfs inside the container. Can be specified multiple times (default: none)"},
		{{"net_macvtap", required_argument, NULL, 0x601}, "Interface which will be cloned (MACVTAP) and put inside the namespace as vt0"},
		{{"net_macvlan", required_argument, NULL, 0x602}, "Interface which will be cloned (MACVLAN) and put inside the namespace as vl0"},
		{{0, 0, 0, 0}, NULL},
	};
        /*  *INDENT-ON* */

	struct option opts[ARRAYSIZE(custom_opts)];
	for (unsigned i = 0; i < ARRAYSIZE(custom_opts); i++) {
		opts[i] = custom_opts[i].opt;
	}

	int opt_index = 0;
	for (;;) {
		int c = getopt_long(argc, argv, "H:c:p:i:u:g:l:t:M:Ndveh?B:T:I:", opts, &opt_index);
		if (c == -1) {
			break;
		}
		switch (c) {
		case 'H':
			nsjconf->hostname = optarg;
			break;
		case 'c':
			nsjconf->chroot = optarg;
			break;
		case 'p':
			nsjconf->port = strtoul(optarg, NULL, 0);
			break;
		case 'i':
			nsjconf->max_conns_per_ip = strtoul(optarg, NULL, 0);
			break;
		case 'u':
			user = optarg;
			break;
		case 'g':
			group = optarg;
			break;
		case 'l':
			logfile = optarg;
			break;
		case 'd':
			nsjconf->daemonize = true;
			break;
		case 'v':
			nsjconf->verbose = true;
			break;
		case 'e':
			nsjconf->keep_env = true;
			break;
		case 't':
			nsjconf->tlimit = strtol(optarg, NULL, 0);
			break;
		case 'h':	/* help */
		case '?':	/* help */
			cmdlineUsage(argv[0], custom_opts);
			break;
		case 0x0201:
			nsjconf->rl_as = cmdlineParseRLimit(RLIMIT_AS, optarg, (1024 * 1024));
			break;
		case 0x0202:
			nsjconf->rl_core = cmdlineParseRLimit(RLIMIT_CORE, optarg, (1024 * 1024));
			break;
		case 0x0203:
			nsjconf->rl_cpu = cmdlineParseRLimit(RLIMIT_CPU, optarg, 1);
			break;
		case 0x0204:
			nsjconf->rl_fsize = cmdlineParseRLimit(RLIMIT_FSIZE, optarg, (1024 * 1024));
			break;
		case 0x0205:
			nsjconf->rl_nofile = cmdlineParseRLimit(RLIMIT_NOFILE, optarg, 1);
			break;
		case 0x0206:
			nsjconf->rl_nproc = cmdlineParseRLimit(RLIMIT_NPROC, optarg, 1);
			break;
		case 0x0207:
			nsjconf->rl_stack = cmdlineParseRLimit(RLIMIT_STACK, optarg, (1024 * 1024));
			break;
		case 0x0301:
			nsjconf->personality |= ADDR_COMPAT_LAYOUT;
			break;
		case 0x0302:
			nsjconf->personality |= MMAP_PAGE_ZERO;
			break;
		case 0x0303:
			nsjconf->personality |= READ_IMPLIES_EXEC;
			break;
		case 0x0304:
			nsjconf->personality |= ADDR_LIMIT_3GB;
			break;
		case 0x0305:
			nsjconf->personality |= ADDR_NO_RANDOMIZE;
			break;
		case 'N':
			nsjconf->clone_newnet = false;
			break;
		case 0x0402:
			nsjconf->clone_newuser = false;
			break;
		case 0x0403:
			nsjconf->clone_newns = false;
			break;
		case 0x0404:
			nsjconf->clone_newpid = false;
			break;
		case 0x0405:
			nsjconf->clone_newipc = false;
			break;
		case 0x0406:
			nsjconf->clone_newuts = false;
			break;
		case 0x0501:
			nsjconf->apply_sandbox = false;
			break;
		case 0x0502:
			nsjconf->keep_caps = true;
			break;
		case 0x0503:
			nsjconf->is_root_rw = true;
			break;
		case 0x0504:
			nsjconf->is_silent = true;
			break;
		case 'B':
			nsjconf->bindmountpts->fs_count++;
			size_t sz =
			    sizeof(*(nsjconf->bindmountpts)) +
			    (sizeof(nsjconf->bindmountpts->mountpt[0]) * nsjconf->bindmountpts->fs_count);
			if (realloc(nsjconf->bindmountpts, sz) == NULL) {
				LOG_F("realloc(%zu)", sz);
			}
			nsjconf->bindmountpts->mountpt[nsjconf->bindmountpts->fs_count - 1] = optarg;
			break;
		case 'T':
			nsjconf->tmpfsmountpts->fs_count++;
			sz = sizeof(*(nsjconf->tmpfsmountpts)) +
			    (sizeof(nsjconf->tmpfsmountpts->mountpt[0]) * nsjconf->tmpfsmountpts->fs_count);
			if (realloc(nsjconf->tmpfsmountpts, sz) == NULL) {
				LOG_F("realloc(%zu)", sz);
			}
			nsjconf->tmpfsmountpts->mountpt[nsjconf->tmpfsmountpts->fs_count - 1] = optarg;
			break;
		case 'M':
			switch (optarg[0]) {
			case 'l':
				nsjconf->mode = MODE_LISTEN_TCP;
				break;
			case 'o':
				nsjconf->mode = MODE_STANDALONE_ONCE;
				break;
			case 'r':
				nsjconf->mode = MODE_STANDALONE_RERUN;
				break;
			default:
				LOG_E("Modes supported: -M l - MODE_LISTEN_TCP (default)");
				LOG_E("                 -M o - MODE_STANDALONE_ONCE");
				LOG_E("                 -M r - MODE_STANDALONE_RERUN");
				cmdlineUsage(argv[0], custom_opts);
				return false;
				break;
			}
			break;
		case 0x601:
			nsjconf->iface_macvtap = optarg;
			break;
		case 0x602:
			nsjconf->iface_macvlan = optarg;
			break;
		default:
			cmdlineUsage(argv[0], custom_opts);
			return false;
			break;
		}
	}

	if (logInitLogFile(nsjconf, logfile, nsjconf->verbose) == false) {
		return false;
	}

	nsjconf->argv = &argv[optind];
	if (nsjconf->argv[0] == NULL) {
		LOG_E("No command provided");
		cmdlineUsage(argv[0], custom_opts);
		return false;
	}

	struct passwd *pw = getpwnam(user);
	if (pw != NULL) {
		nsjconf->uid = pw->pw_uid;
	} else if (cmdlineIsANumber(user)) {
		nsjconf->uid = (uid_t) strtoull(user, NULL, 0);
	} else {
		LOG_E("No such user '%s'", user);
		return false;
	}
	struct group *gr = getgrnam(group);
	if (gr != NULL) {
		nsjconf->gid = gr->gr_gid;
	} else if (cmdlineIsANumber(group)) {
		nsjconf->gid = (gid_t) strtoull(group, NULL, 0);
	} else {
		LOG_E("No such group '%s'", group);
		return false;
	}

	return true;
}
