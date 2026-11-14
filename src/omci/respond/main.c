/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/* omcid -- the OMCI stack: startup and the main loop.
 *
 * Takes redirect type 1 and answers the OLT (omcimsg.c), keeps the MIB
 * (mibstore.c), applies it to the switch driver (apply*.c, drv.c), and serves
 * the omcli and omcicli queues (clisrv.c, vqsrv.c). OLT_QUIET_MS after the
 * last OMCI frame, the loop rebuilds the upstream QoS and bridge connections.
 * See ../README.md for the design; omcid.h for what the modules share.
 */
#include "omcid.h"
#include "../redirect_guard.h"

/* wdt_ping() -- the odi_wdt watchdog rule this daemon owns (docs/SETTINGS.md,
 * "Watchdog rules"): a cheap write to /proc/odi_wdt/ping every few seconds
 * from this OWN main loop, not a separate probing process. rcS registers
 * "omcid" with its deadline at boot (/proc/odi_wdt/register), except in a
 * safe-mode boot; the kernel arms it at that registration and resets the
 * board if the pings here do not arrive in time. A missing /proc/odi_wdt (host, qemu, a stock kernel) makes the
 * open fail and the write a silent no-op -- exactly as intended off real
 * hardware.
 */
#define WDT_PING_PATH   "/proc/odi_wdt/ping"
#define WDT_PING_NAME   "omcid"

static void wdt_ping(void)
{
	long fd = sys_open(WDT_PING_PATH, O_WRONLY);

	if (fd < 0)
		return;
	sys_write((int)fd, WDT_PING_NAME, sizeof(WDT_PING_NAME) - 1);
	sys_close((int)fd);
}

/* SIGHUP does not stop the daemon: it asks it to reread the config store
 * (reload.c). The handler returns, which needs the kernel-provided signal
 * return (the vdso trampoline on MIPS); it stores one flag and does nothing
 * else. */

/* A redirect type dropped without deregistering keeps the kernel delivering
 * to a dead netlink port, with an unthrottled printk per frame; the OLT
 * retries hard, and the console storm can starve the kernel thread that kicks
 * the watchdog. So deregister on a signal too. Without a libc there is no
 * sigreturn trampoline: this handler must exit, never return. */
static void on_signal(int sig)
{
	(void)sig;
	/* Reset the sink FIRST: mid-command it is cli_sink, and flushing through
	 * it would spend the whole retry budget on a queue about to be deleted. */
	out_set_sink(0);
	if (nl_fd >= 0)
		nl_redirect(nl_fd, nl_tid, REDIRECT_TYPE, NL_ACT_DEREG, 0);
	/* Queues outlive the process, and this kernel has 53 in total. Only
	 * ours: reply_q belongs to a client, which removes it. */
	if (cliq >= 0)
		mq_remove(cliq);
	if (vq >= 0 && vq_is_ours)
		mq_remove(vq);
	out("\nderegistered on signal\n");
	out_flush();
	sys_exit(0);
}

static void catch_signals(int fd, uint32_t tid)
{
	nl_fd = fd;
	nl_tid = tid;
	/* SIGHUP is installed at the top of main() and only sets a flag. */
	out_fmt("signal handlers: int %d term %d\n",
		sys_signal(2, on_signal),
		sys_signal(15, on_signal));      /* what kill sends */
}

static void usage(void)
{
	out("usage: omcid [-a] [-r] [-d] [-f] [-w units] [-c caps-hex] [-s state]\n"
	    "             [-g file] [-P file] [-m file] [-n file] [-t file] [-i file] [-j seconds] [-k pid] [-h]\n"
	    "\n"
	    "The OMCI responder: registers for redirect type 1 and answers the\n"
	    "OLT, and serves the omcli and omcicli queues.\n"
	    "\n"
	    "  -a          program the switch (apply mode); without it, dry run\n"
	    "  -r          restart: first remove every bridge connection a\n"
	    "              previous omcid left in the switch (needs -a)\n"
	    "  -d          daemon: no frame or idle limit\n"
	    "  -w units    exit after this many idle 5 s units (default 24)\n"
	    "  -c hex      use this capability blob instead of the driver one\n"
	    "  -s state    use this ONU state for the resume decision instead\n"
	    "              of asking the driver (test only, see docs/BOOT.md)\n"
	    "  -g file     read the Alloc-IDs and the serial from this file\n"
	    "              instead of /proc/odi_gpon/status (test only)\n"
	    "  -P file     read the PLOAM type counts from this file instead\n"
	    "              of /proc/odi_gpon/ploam (test only)\n"
	    "  -l file     read the UNI link bit from this file instead of\n"
	    "              the odi_switch host_link parameter (test only)\n"
	    "  -m file     read the bridge MAC address of class 46 from this\n"
	    "              file instead of the sysfs one of eth0 (test only)\n"
	    "  -n file     read the FEC counters from this file instead of\n"
	    "              /proc/odi_gpon/counters (test only)\n"
	    "  -t file     take the PM interval clock, in seconds, from this\n"
	    "              file instead of the monotonic clock (test only)\n"
	    "  -i file     append the driver verbs of a reload to this file\n"
	    "              instead of /proc/odi_init (test only)\n"
	    "  -j seconds  how long a reload waits for O5 (test only)\n"
	    "  -k pid      the process an accepted OLT Reboot signals instead of\n"
	    "              init (test only)\n"
	    "  -f          start even if a live process holds redirect type 1\n"
	    "  -h          this text; starts nothing\n");
}

int main(int argc, char **argv)
{
	uint32_t tid = 0, plen;
	long fd, rc;
	int idle = 0, maxidle = 24, n = 0, want = 400, force = 0, restart = 0;
	int quiet = 0;
	unsigned long frames_seen = 0;
	/* CLOCK_MONOTONIC second at which the oldest pending rebuild was first
	 * marked dirty; -1 when none is pending. See the gate below. */
	long dirty_since_s = -1;
	long now_s = -1;
	int rebuild_now = 0;
	/* -s: an ONU state to use instead of asking the driver, the same
	 * reason -c takes a capability blob -- a test harness with no line
	 * side has no driver to answer command 13 either. */
	int state_from_arg = 0;
	uint32_t state_arg = 0;

	/* First thing: an apply.sh SIGHUP that lands while omcid is still
	 * starting must set the flag, not take the default action and kill it. */
	sys_signal(1, reload_on_hup);

	/* Every argument is matched whole, and anything unrecognised stops the
	 * program before it opens a socket: a mistyped flag must not start a
	 * second daemon that takes redirect type 1 from the running one. */
	for (int i = 1; i < argc; i++) {
		if (str_eq(argv[i], "-h") || str_eq(argv[i], "--help")) {
			usage();
			out_flush();
			return 0;
		} else if (str_eq(argv[i], "-f") || str_eq(argv[i], "--force")) {
			force = 1;
		} else if (str_eq(argv[i], "-a"))
			apply_hw = 1;
		/* -r: replacing a running omcid, whose bridge connections are
		 * still in the switch; without clearing them the first rebuild
		 * adds a second set. /etc/scripts/apply passes it, rcS does not. */
		else if (str_eq(argv[i], "-r"))
			restart = 1;
		/* -d: the daemon, stopped only by a signal. Without it the
		 * 400-frame and 120 s idle limits end a live session. */
		else if (str_eq(argv[i], "-d")) {
			want = 0x7fffffff;
			maxidle = 0x7fffffff / (5000000 / NL_POLL_US) - 1;
		}
		/* -c <120 bytes of hex>: a capability blob (port map, flow and
		 * T-CONT counts) captured from a real device, so data-path
		 * construction can run off the device. It does not imply -a. */
		else if (str_eq(argv[i], "-c") && i + 1 < argc) {
			const char *h = argv[++i];
			unsigned n = 0;

			while (*h && n < sizeof caps) {
				int hi = hex_nib(*h);
				int lo = h[1] ? hex_nib(h[1]) : -1;

				if (hi < 0 || lo < 0)
					break;
				caps[n++] = (uint8_t)((hi << 4) | lo);
				h += 2;
			}
			if (n == sizeof caps) {
				caps_ok = 1;
				caps_from_arg = 1;
			} else {
				out_fmt("%% -c needs %d bytes of hex, got %d\n",
					(long)sizeof caps, (long)n);
			}
		}
		else if (str_eq(argv[i], "-w") && i + 1 < argc) {
			maxidle = 0;
			for (const char *s = argv[++i]; *s >= '0' && *s <= '9'; s++)
				maxidle = maxidle * 10 + (*s - '0');
		}
		/* -s state: use this ONU state (5 == O5) for the resume
		 * decision instead of asking the driver. Test-only, like -c. */
		else if (str_eq(argv[i], "-s") && i + 1 < argc) {
			state_arg = 0;
			for (const char *s = argv[++i]; *s >= '0' && *s <= '9'; s++)
				state_arg = state_arg * 10 + (uint32_t)(*s - '0');
			state_from_arg = 1;
		}
		/* -g file: the /proc/odi_gpon/status shape (alloc_ids, sn) from a
		 * file, the same reason as -s: qemu has no kernel of ours. */
		else if (str_eq(argv[i], "-g") && i + 1 < argc) {
			gpon_proc_path = argv[++i];
		}
		/* -l file: the UNI link bit (0 or 1), for the same reason. */
		else if (str_eq(argv[i], "-l") && i + 1 < argc) {
			uni_link_path = argv[++i];
		}
		/* -m file: the stick MAC address, "aa:bb:cc:dd:ee:ff", for class
		 * 46 instead of /sys/class/net/eth0/address; under qemu the
		 * sysfs file would be the test host's. */
		/* -n file / -t file: the FEC counters (the /proc/odi_gpon/counters
		 * shape) and the clock of the PM intervals, in seconds, from files:
		 * a test moves 15 minutes by writing a number. */
		else if (str_eq(argv[i], "-n") && i + 1 < argc) {
			pm_counters_path = argv[++i];
		}
		else if (str_eq(argv[i], "-t") && i + 1 < argc) {
			pm_clock_path = argv[++i];
		}
		else if (str_eq(argv[i], "-m") && i + 1 < argc) {
			bridge_mac_path = argv[++i];
		}
		/* -i file: where the reload writes the driver verbs instead of
		 * /proc/odi_init, and -j seconds: how long it waits for O5 and
		 * services; both test only. */
		/* -k: the process an accepted OLT Reboot signals, init (1)
		 * unless a test harness names a stand-in. */
		else if (str_eq(argv[i], "-k") && i + 1 < argc) {
			reboot_pid = 0;
			for (const char *s = argv[++i]; *s >= '0' && *s <= '9'; s++)
				reboot_pid = reboot_pid * 10 + (*s - '0');
		}
		/* -P file: the /proc/odi_gpon/ploam shape (the Deactivate_ONU-ID
		 * count) from a file, test only, like -g. */
		else if (str_eq(argv[i], "-P") && i + 1 < argc) {
			ploam_proc_path = argv[++i];
		}
		else if (str_eq(argv[i], "-i") && i + 1 < argc) {
			odi_init_path = argv[++i];
		} else if (str_eq(argv[i], "-j") && i + 1 < argc) {
			reload_o5_wait_s = 0;
			for (const char *s = argv[++i]; *s >= '0' && *s <= '9'; s++)
				reload_o5_wait_s = reload_o5_wait_s * 10 + (*s - '0');
		} else {
			out_fmt("omcid: unknown argument %s\n", argv[i]);
			usage();
			out_flush();
			return 2;
		}
	}

	/* Opened, and nl_fd/nl_tid set, before the capability, serial and
	 * device-id reads: omci_drv_call() needs them for the ODI_OMCI_OP_CMD
	 * path, and the sockopt fallback does not exist on this image.
	 * catch_signals() sets the same two globals again later. */
	fd = nl_open(&tid, NL_POLL_US);
	if (fd < 0) {
		out_fmt("%% netlink: %d -- no line side. The message queues are "
			"still served, so `omcli --inject` works and replies "
			"are logged rather than sent.\n", (long)fd);
		tid = (uint32_t)sys_gettid();
	}
	/* Before any driver read or queue: a second omcid must not register
	 * over the first. Only with a line side; qemu has nothing to take. */
	if (fd >= 0 && redirect_guard("omcid", REDIRECT_TYPE, force, tid)) {
		sys_close((int)fd);
		out_flush();
		return 1;
	}
	nl_fd = fd;
	nl_tid = tid;

	/* Not gated on -a: the read changes nothing, and without it a dry run
	 * maps no UNI, so every TP pointer reports "in no capability slot". */
	if (!caps_from_arg) {
		caps_ok = omci_drv_call(OMCI_CAPS_CMD, caps, sizeof caps) == 0;
		if (!caps_ok)
			out("% could not read the device capabilities; "
			    "flow ids fall back to a 64-entry table and no "
			    "UNI is mapped\n");
		else {
			out_fmt("capabilities: %d gem flows, %d priority queues\n",
				(long)caps_flows(),
				(long)caps_u32(OMCI_CAPS_OFF_PRIQ));
			for (unsigned i = 0; i < OMCI_CAPS_UNI_SLOTS; i++)
				if (caps[i * 2 + 1] != 0xff)
					out_fmt("  uni slot %d: type %d, "
						"index %d -> switch port %d\n",
						(long)i, (long)caps[i * 2],
						(long)caps[i * 2 + 1], (long)i);
		}
	}

	if (serial_refresh(1) != 0)
		out("% no serial number yet; asking again once a second\n");
	if (omci_drv_call(DRV_GET_DEVID, devid, sizeof devid) != 0)
		out("% could not read the device id from the driver\n");
	devid[8] = 0;                            /* "RTL9602C" is NUL-padded */

	/* The config store, read once at startup, so the CTC LOID entity
	 * answers from it from the first frame. */
	cfg_load_identity();
	cfg_load_vlan();
	cfg_load_report();
	out_fmt("store: loid %s, manual vlan %s, identity report %s\n",
		ident.loid[0] ? "set" : "none",
		vlanCfg.manual ? "on" : "off (transparent)",
		report.on ? "on" : "off");
	cfg_load_unknown_me();
	out_fmt("unknown entities: %s\n", unknown_me_ok
		? "answered ok (OMCI_UNKNOWN_ME_OK=1)"
		: "answered unknown entity");
	cfg_load_veip_slot();
	cfg_load_private_me();
	cfg_load_veip_on();
	cfg_load_custom_me();
	cfg_load_custom_mcast();
	if (custom_mcast & CUSTOM_MCAST_DS_UNTAGGED)
		out("multicast: downstream tag stripped whatever the OLT sets (OMCI_CUSTOM_MCAST bit 1)\n");
	cfg_load_pots_ports();
	aut_rebuild();
	if (!veip_on)
		out("VEIP: none (OMCI_VEIP=0), the ONU presents its PPTP Ethernet UNI only\n");
	else
	out_fmt("VEIP in slot %d%s, vendor-private entities %s\n", (long)veip_slot,
		veip_slot_key >= 0 && veip_slot == VEIP_SLOT_DEFAULT && veip_slot_key != veip_slot
		? " (OMCI_VEIP_SLOT_ID is set but OMCI_CUSTOM_ME bit 256 is not)" : "",
		private_me ? "created" : "left out (OMCI_PRIVATE_ME=0)");
	if (pots_ports)
		out_fmt("phone: %d POTS port(s) in slot %d, the VoIP entities accepted (OMCI_POTS_PORTS)\n",
			(long)pots_ports, (long)pots_slot);
	cfg_load_fixed_me();
	if (fixed_me.n)
		out_fmt("fixed entities: %d entr(ies) answered from OMCI_FIXED_ME\n",
			(long)fixed_me.n);
	reload_init();

	/* Resume without re-registration (docs/BOOT.md): a respawned omcid
	 * whose PON is still O5, with a snapshot on disk for this exact
	 * device, loads it and answers from it -- no re-registration, no
	 * switch reprogramming, because the datapath is already in hardware
	 * (see kb rtl9601-omci-reapply-without-reboot); only a rebuild that
	 * was pending when the last omcid died is run. Everything else (no
	 * snapshot, a mismatched device, not O5, or a MIB the OLT has since
	 * reset -- mib_reset_all() deletes the snapshot itself) falls back to
	 * the ordinary path below unchanged.
	 *
	 * The decision file is written before anything else here can fail or
	 * block, so omci-respawn-reprovision.sh -- backgrounded by
	 * svc-omcid.sh at the same moment this process starts -- only has to
	 * wait on it briefly, not for full registration. svc-omcid.sh moves an
	 * earlier run's file to RESUME_DECISION_PREV before this process
	 * starts, so the main file read by that script is only ever this
	 * run's; both are removed here before the new decision is written. */
	{
		uint32_t st = 0;
		int resumed, restart;
		const char *why = "no_snapshot";
		long prev = sys_open(RESUME_DECISION_PATH, O_RDONLY);
		long prevp = sys_open(RESUME_DECISION_PREV, O_RDONLY);

		/* The decision file is on tmpfs and every omcid writes one, so
		 * finding one (or the .prev svc-omcid.sh moved it to) says an
		 * omcid already ran this boot: this is a respawn or a restart,
		 * not the first start. */
		restart = prev >= 0 || prevp >= 0;
		if (prev >= 0)
			sys_close((int)prev);
		if (prevp >= 0)
			sys_close((int)prevp);
		sys_unlink(RESUME_DECISION_PATH);
		sys_unlink(RESUME_DECISION_PREV);
		if (state_from_arg)
			st = state_arg;
		else if (fd < 0 || omci_getOnuState(&st) != 0)
			st = 0;
		resumed = snapshot_try_resume(st, &why);
		snapshot_write_decision(resumed);
		if (!resumed)
			uni_start();
		out_fmt("resume: onu state %d, %s\n", (long)st,
			resumed ? "snapshot loaded, resuming without re-registration"
				: "no valid snapshot for this state -- falling back to re-registration");
		/* A Create or Set the last omcid answered and did not get to
		 * program: its snapshot carried the flags (snapshot.c), and
		 * the loop below runs the rebuild after the quiet second. */
		if (resumed && (conn_dirty || qos_dirty))
			out_fmt("resume: a rebuild was pending (%s%s%s)\n",
				conn_dirty ? "bridge connections" : "",
				conn_dirty && qos_dirty ? ", " : "",
				!qos_dirty ? "" : qos_full ? "upstream QoS, in full"
							   : "upstream QoS");
		ev_start(restart, resumed, why, st);
	}

	if (restart && apply_hw && fd >= 0) {
		/* Deactivating an id the driver does not hold is not an error
		 * (odi_switch_cmd.c cmd 50), so every id is sent. */
		for (int id = 0; id < SERV_MAX; id++)
			omci_deactiveBdgConn((uint32_t)id);
		out_fmt("restart: %d bridge connection ids cleared\n", (long)SERV_MAX);
		/* And the T-CONTs, queues and flows (upstream and downstream)
		 * the last omcid programmed: this one starts with none of them
		 * in its tables, and would otherwise meet their queue blocks
		 * and keep their flows valid (odi_switch_cmd.c, command 0). */
		{
			uint8_t none[4] = { 0, 0, 0, 0 };

			out_fmt("restart: flow reset -> %d\n",
				(long)omci_drv_call(OMCI_MIB_RESET_CMD, none, sizeof none));
		}
	}

	/* The netlink poll is NL_POLL_US (2 ms): the queues are served between
	 * netlink timeouts, and the stock omcicli gives up after 12 ms. `-w`
	 * keeps its five-second units; the idle count is scaled instead.
	 *
	 * No netlink is not fatal: frames injected on the message queue
	 * (msgType 0) need none, and qemu-user answers EPROTONOSUPPORT (-120 on
	 * MIPS) for our own protocol number (NETLINK_ODI; the stock firmware
	 * used NETLINK_USERSOCK). The MIB store, both CLI protocols, the
	 * renderers and injection all run there. Answers are logged, not sent. */
	/* A queue of our own, not 0x800: a client detects the omcli protocol by
	 * whether this key exists, not by probing a daemon that may be stock. */
	cliq = mq_open_fresh(OMCLI_KEY);
	if (cliq < 0)
		out_fmt("%% no cli queue (%d); omcli will not find this daemon\n",
			(long)cliq);

	/* The 0x800 queue (boa and the omcicli scripts) is taken in the loop:
	 * omci_app usually still holds it here. See vq_ensure. */
	vq_ensure();
	out_fmt("cli queue %d (omcli); the omcicli queue is %s\n", (long)cliq,
		vq >= 0 ? "ours" : "still omci_app's");

	catch_signals((int)fd, tid);
	rc = fd < 0 ? -1
		    : nl_redirect((int)fd, tid, REDIRECT_TYPE, NL_ACT_REG,
				  REDIRECT_MTU);
	out_fmt("answering as %s, tid %d (register %d)%s\n", (const char *)devid,
		tid, rc, apply_hw ? ", programming the switch" : "");
	out_flush();

	while (n < want && idle < maxidle * (5000000 / NL_POLL_US)) {
		/* SIGHUP: reread the store (reload.c). Cheap when idle, and
		 * never blocks, so the ping below keeps going. */
		reload_poll();
		/* Serving a queue is not idling; with no line side it is the
		 * only thing that resets the -w count. */
		if (cli_poll() || vq_poll())
			idle = 0;
		/* A rebuild of the upstream QoS, the multicast policy and the
		 * bridge connections (us_qos_rebuild(), mcast_policy_export(),
		 * bdgconn_rebuild()) runs from the MIB as it stands when either
		 * holds: one quiet second has passed after the last OMCI frame,
		 * from the line or injected, or REBUILD_MAX_S seconds have
		 * passed since the first change was marked dirty, however busy
		 * the channel is. Every frame resets the quiet count, Gets and
		 * PM polls included, so an OLT that sends one more often than
		 * once a second would otherwise keep the rules from ever being
		 * programmed. Quiet counts OMCI frames only: CLI and queue
		 * requests reset `idle` (the -w exit) but not this, or a client
		 * polling omcicli every few seconds, a metrics scrape, would
		 * hold the rebuild off indefinitely and leave the ONU in O5
		 * with no service. The cap is timed from the first change to
		 * the QoS or the bridge connections, not from mcast_dirty,
		 * which starts out set: from the start of omcid, a burst that
		 * began inside the first REBUILD_MAX_S seconds would be rebuilt
		 * half-way (bdgconn_rebuild() removes every service first). A
		 * change that sets only mcast_dirty (a Set of class 309 or 310
		 * that leaves out the tag, the access lists and the profile
		 * pointer) waits for the quiet second, as it always did. */
		if (omci_frames_handled != frames_seen) {
			frames_seen = omci_frames_handled;
			quiet = 0;
		} else if (quiet < 1000000 / NL_POLL_US) {
			quiet++;
		}
		now_s = -1;
		{
			/* CLOCK_MONOTONIC-gated, not loop-iteration-counted: this
			 * used to count NL_POLL_US-spaced ticks and call a
			 * counted 1000000/NL_POLL_US of them "one second", which
			 * only holds if every iteration takes exactly NL_POLL_US.
			 * One slow iteration -- a full apply on an OMCI frame, a
			 * write that blocks while /tmp is nearly full -- stretches
			 * a counted second past its real length, and 15 counted
			 * seconds can cover well more than 15 real ones: seen on
			 * hardware as a 27 s last_ping_age against the 15 s this
			 * comment used to promise. Wall-clock elapsed time keeps
			 * the real cadence regardless of how long any one
			 * iteration takes. */
			static long last_sample_s = -1;
			static long last_ping_s = -1;
			long now_ns;

			if (sys_clock_gettime(CLOCK_MONOTONIC, &now_s, &now_ns) == 0) {
				if (last_sample_s < 0)
					last_sample_s = now_s;
				if (last_ping_s < 0)
					last_ping_s = now_s;
				if (now_s - last_sample_s >= 1) {
					last_sample_s = now_s;
					onu_state_sample();
					serial_refresh(1);
					/* The T-CONTs follow the kernel
					 * list: a new assignment or a release
					 * reprograms the upstream graph a
					 * quiet second later, like any Set.
					 * Every change, not only one under a
					 * bound T-CONT: a T-CONT the driver
					 * refused because its Alloc-ID was not
					 * assigned yet is bound by this. */
					if (alloc_ids_refresh()) {
						qos_dirty = 1;
						qos_full = 1;
					}
					ev_tick();
					alarm_tick();
					pm_tick();
				}
				/* Every 15 s: a quarter of the 60 s deadline
				 * odi_wdt.h/rcS register omcid with, so an
				 * occasional missed beat (a slow OLT frame, a
				 * stalled CLI client) never costs a reset. */
				if (now_s - last_ping_s >= 15) {
					last_ping_s = now_s;
					wdt_ping();
				}
			}
		}
		if (!(qos_dirty || conn_dirty))
			dirty_since_s = -1;
		else if (dirty_since_s < 0)
			dirty_since_s = now_s;
		rebuild_now = quiet >= 1000000 / NL_POLL_US ||
			      (dirty_since_s >= 0 && now_s >= 0 &&
			       now_s - dirty_since_s >= REBUILD_MAX_S);
		if (qos_dirty && rebuild_now) {
			qos_dirty = 0;
			us_qos_rebuild();
			/* The flow ids and T-CONT map us_qos_rebuild() just
			 * settled are exactly the "maps a managed entity to
			 * switch programming" half of a resume snapshot
			 * (docs/BOOT.md) -- the Create/Set handler that set
			 * qos_dirty already saved one, but the rebuild that
			 * actually fills these tables runs here, a quiet
			 * second later. Without this a snapshot taken between
			 * that message and this rebuild describes a MIB whose
			 * bookkeeping has not caught up with it yet. */
			snapshot_save();
		}
		if (mcast_dirty && rebuild_now) {
			mcast_dirty = 0;
			mcast_policy_export();
		}
		if (conn_dirty && rebuild_now) {
			conn_dirty = 0;
			bdgconn_rebuild();
			snapshot_save();       /* see the qos_dirty arm above */
		}
		/* Whatever a rebuild marked dirty again is timed from the next
		 * pass, not from the change that started this one. */
		if (rebuild_now)
			dirty_since_s = -1;
		if (fd < 0) {
			/* No line side: pace the loop as the netlink
			 * receive timeout would. */
			sys_nanosleep(0, NL_POLL_US * 1000);
			idle++;
			continue;
		}
		rc = nl_recv((int)fd, rxbuf, sizeof rxbuf, &plen);
		if (rc <= 0) {
			idle++;
			continue;
		}
		idle = 0;
		if (plen >= NL_FRAME_OFF &&
		    rx_accept(rxbuf + NLMSG_HDR + NL_FRAME_OFF,
			      (uint32_t)plen - NL_FRAME_OFF)) {
			n++;
			handle((int)fd, tid, rxbuf + NLMSG_HDR + NL_FRAME_OFF);
		}
		out_flush();
	}
	if (fd >= 0) {
		nl_redirect((int)fd, tid, REDIRECT_TYPE, NL_ACT_DEREG, 0);
		sys_close((int)fd);
	}
	if (cliq >= 0)
		mq_remove(cliq);
	if (vq >= 0 && vq_is_ours)
		mq_remove(vq);
	out_fmt("done after %d frames\n", (long)n);
	out_flush();
	return 0;
}
