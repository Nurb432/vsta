/*
 * select.c
 *	Implement select() operation across servers
 *
 * This library module works in conjunction with the "selfs" server, which
 * handles the unification of I/O events across all the various servers.
 *
 * This implementation assumes that only one thread will be using select()
 * services.
 */
#include <sys/types.h>
#include <time.h>
#include <sys/fs.h>
#define _SELFS_INTERNAL
#include <select.h>
#include <std.h>
#include <fdl.h>
#include <limits.h>

/*
 * This lets us know when we're a new process with a need for new handles
 */
extern uint fork_tally;
static uint old_fork_tally = (uint)-1;
static pid_t old_pid;

/*
 * Our connection to the select server
 */
static port_t selfs_port = -1;
static long selfs_clid;
static ulong selfs_key;

/*
 * Our cache of connections set up for select() operation
 */
static uint cache_nfd;
static struct cache {
	uint c_mask;		/* Events we last looked for */
	struct select_complete
		c_event;	/* Events we last got */
} *cache;

/*
 * just_sleep()
 *	Timed sleep based on timeval value
 */
static int
just_sleep(struct timeval *t)
{
	if (t == 0) {
		return(0);
	}
	return(__msleep(t->tv_sec*1000 + t->tv_usec/1000));
}

/*
 * connect_selfs()
 *	Create connection to select server, get clid and key values
 */
static int
connect_selfs(void)
{
	/*
	 * Connect to the select server
	 */
	selfs_port = path_open("fs/select:client", ACC_NOCLONE);
	if (selfs_port < 0) {
		return(-1);
	}

	/*
	 * Extract our client ID and key for our server
	 */
	selfs_clid = atoi(rstat(selfs_port, "clid"));
	selfs_key = atoi(rstat(selfs_port, "key"));
	return(0);
}

/*
 * clear_entry()
 *	Tell a server to not bother with this descriptor any more
 *
 * Updates our cache mask at the same time.
 */
static void
clear_entry(int fd)
{
	struct msg m;
	static char msg[] = "unselect\n";

	m.m_op = FS_WSTAT;
	m.m_buf = msg;
	m.m_arg = m.m_buflen = sizeof(msg);
	m.m_nseg = 1;
	(void)msg_send(__fd_port(fd), &m);
	bzero(&cache[fd], sizeof(struct cache));
}

/*
 * set_entry()
 *	Set up for select() support on an FD
 *
 * Returns 0 on success, 1 on failure
 */
static int
set_entry(int fd, uint mask)
{
	struct msg m;
	char buf[80];
	int len;
	static char *hostname;

	/*
	 * Get this first time only
	 */
	if (hostname == 0) {
		/* TBD: get from networking */
		hostname = "localhost";
	}

	/*
	 * Connect to selfs one time only
	 */
	if (selfs_port == -1) {
		if (connect_selfs()) {
			return(1);
		}
	}

	/*
	 * Build setup message
	 */
retry:	sprintf(buf, "select=%u,%ld,%d,%lu,%s\n",
		mask, selfs_clid, fd, selfs_key, hostname);
	m.m_op = FS_WSTAT;
	m.m_buf = buf;
	m.m_buflen = m.m_arg = len = strlen(buf);
	m.m_nseg = 1;
	m.m_arg1 = 0;

	/*
	 * Send it
	 */
	if (msg_send(__fd_port(fd), &m) < 0) {
		/*
		 * If we find ourselves conflicting with some other
		 * process or thread under this client connection,
		 * spawn a new one instead.
		 */
		if (!strcmp(strerror(), EBUSY)) {
			if (__fd_clone(fd) < 0) {
				return(-1);
			}
			goto retry;
		}
		return(1);
	}

	/*
	 * Mark our mask on success
	 */
	cache[fd].c_mask = mask;
	__fd_set_iocount(fd, 0);
	return(0);
}

/*
 * clear_cache()
 *	Clear out our record of masks which are set up, and start over
 */
static void
clear_cache(void)
{
	if (cache_nfd) {
		bzero(cache, sizeof(struct cache) * cache_nfd);
	}
}

/*
 * zero()
 *	Clear out the indicated range of an fd_set
 */
static void
zero(fd_set *s, uint nfd)
{
	bzero(s, roundup(nfd, 8*sizeof(uint)) / 8);
}

/*
 * within_range()
 *	Tell if an iocount value is equal or greater than ours
 *
 * Takes into account the free-running nature of I/O counts
 */
static int
within_range(ulong ours, ulong new)
{
	if (new >= ours) {
		return ((new-ours) < ULONG_MAX/4);
	}
	return((new + (ULONG_MAX-ours)) < ULONG_MAX/4);
}

/*
 * select()
 *	Block on zero or more pending I/O events, or timeout
 */
int
select(uint nfd, fd_set *rfds, fd_set *wfds, fd_set *efds, struct timeval *t)
{
	int x, unsupp, count;
	ulong iocount;
	uint newmask;
	struct cache *c;
	struct msg m;
	struct select_complete *sc, events[20];

	/*
	 * If this is a timed sleep only, skip all the hard work
	 */
	if ((nfd == 0) || ((rfds == 0) && (wfds == 0) && (efds == 0))) {
		return(just_sleep(t));
	}

	/*
	 * Clear context if we're a new process
	 */
	if (old_fork_tally != fork_tally) {
		/*
		 * No problem if we're the parent of the fork()
		 */
		if (getpid() != old_pid) {
			clear_cache();
			old_pid = getpid();
		}
		old_fork_tally = fork_tally;
	}

	/*
	 * Resize the cache
	 */
	if (nfd != cache_nfd) {
		/*
		 * Free up entries at the top of it's shrinking
		 */
		if (nfd < cache_nfd) {
			for (x = nfd; x < cache_nfd; ++x) {
				clear_entry(x);
			}
		} else {
			/*
			 * Otherwise create and zero the new entries
			 */
			cache = realloc(cache, nfd * sizeof(struct cache));
			bzero(cache + cache_nfd,
				(nfd - cache_nfd) * sizeof(struct cache));
		}
		cache_nfd = nfd;
	}

	/*
	 * Walk the file descriptors, clearing unused ones and creating
	 * selfs associations for new ones.
	 */
	count = 0;
	for (unsupp = x = 0, c = cache; x < nfd; ++x, ++c) {
		newmask = ((rfds && FD_ISSET(x, rfds)) ? ACC_READ : 0) |
			((wfds && FD_ISSET(x, wfds)) ? ACC_WRITE : 0) |
			((efds && FD_ISSET(x, efds)) ? ACC_EXCEP : 0);
		if (c->c_mask != newmask) {
			/*
			 * No more select()'ing on this FD
			 */
			if (newmask == 0) {
				clear_entry(x);
			} else {
				/*
				 * Turn on select()'ing, and notice if
				 * the fd doesn't support it.
				 */
				if (set_entry(x, newmask)) {
					unsupp += 1;
					c->c_mask |= ACC_UNSUPP;
				}
			}

		/*
		 * If this one's set up, and they're select()'ing on
		 * an FD which previously had already select()'ed
		 * true (without having done I/O in between), then
		 * arrange to return the same answer as last time.
		 */
		} else if ((c->c_event.sc_mask) &&
				(c->c_event.sc_iocount == __fd_iocount(x))) {
			bcopy(&c->c_event, &events[count++],
				sizeof(struct select_complete));
		} else if (c->c_mask & ACC_READ) {
			/*
			 * If the data is buffered at the FDL layer,
			 * make the descriptor select readable.  This
			 * will most likely only occur for TTY's.
			 */
			if (__fd_readcount(x)) {
				events[count].sc_index = x;
				events[count].sc_mask = ACC_READ;
				events[count].sc_iocount = __fd_iocount(x);
				count += 1;
			}
		}
	}

	/*
	 * If we bump into an entry which doesn't support select(), return
	 * now with that entry always selecting "true".
	 */
	if (unsupp > 0) {
		/*
		 * Clear all except the fd(s) which don't support
		 * select().
		 */
		for (x = 0, c= cache; x < nfd; ++x, ++c) {
			if (!(c->c_mask & ACC_UNSUPP)) {
				if (rfds) {
					FD_CLR(x, rfds);
				}
				if (wfds) {
					FD_CLR(x, wfds);
				}
				if (efds) {
					FD_CLR(x, efds);
				}
			}
		}
		return(unsupp);
	}

	/*
	 * If we have previously available events, we'll just
	 * run with those.
	 */
	if (count > 0) {
		x = count * sizeof(struct select_complete);
	} else {
		/*
		 * We're finally ready to rumble.
		 * Post a read for wakeup events.
		 */
retry:		m.m_op = FS_READ | M_READ;
		m.m_buf = events;
		m.m_arg = m.m_buflen = sizeof(events);

		/*
		 * If there's a timeout struct, calculate the delay.
		 * For the special case of a time of 0, we send across
		 * -1, which means simply poll for existing events.
		 * If there's no timeout struct, we send 0, which means
		 * sleep until something's available.
		 */
		if (t) {
			m.m_arg1 = (t->tv_sec * 1000) + (t->tv_usec / 1000);
			if (m.m_arg1 == 0) {
				m.m_arg1 = -1;
			}
		} else {
			m.m_arg1 = 0;
		}
		m.m_nseg = 1;
		x = msg_send(selfs_port, &m);
		if (x < 0) {
			return(-1);
		}

		/*
		 * A return with length 0 means a timeout.  Clear all the
		 * descriptor masks.
		 */
		if (x == 0) {
			if (rfds) {
				zero(rfds, nfd);
			}
			if (wfds) {
				zero(wfds, nfd);
			}
			if (efds) {
				zero(efds, nfd);
			}
			return(0);
		}
	}

	/*
	 * Walk the result, tallying results
	 */
	for (count = 0, sc = events; x >= sizeof(struct select_complete);
			x -= sizeof(struct select_complete)) {
		int fd, event;

		/*
		 * Ignore anything which is outside our current range
		 */
		fd = sc->sc_index;
		if (fd >= nfd) {
			continue;
		}
		c = &cache[fd];

		/*
		 * If the reported events don't match what currently
		 * interests us, also ignore it.
		 */
		if ((sc->sc_mask & c->c_mask) == 0) {
			continue;
		}

		/*
		 * If we've done I/O since this select event was generated,
		 * also ignore it (a new event, if any, will arrive
		 * later).
		 */
		iocount = __fd_iocount(fd);
		if (sc->sc_iocount != iocount) {
			if (within_range(iocount, sc->sc_iocount)) {
				__fd_set_iocount(fd, sc->sc_iocount);
			} else {
				continue;
			}
		}

		/*
		 * Record the event, in case they just re-select
		 * without doing any I/O.
		 */
		c->c_event = *sc;

		/*
		 * Ok, we're really going to wake up now.  The first time
		 * we decide this, clear the select arguments.
		 */
		if (count++ == 0) {
			if (rfds) {
				zero(rfds, nfd);
			}
			if (wfds) {
				zero(wfds, nfd);
			}
			if (efds) {
				zero(efds, nfd);
			}
		}

		/*
		 * Post the requested events
		 */
		if (rfds && (sc->sc_mask & c->c_mask & ACC_READ)) {
			FD_SET(fd, rfds);
		}
		if (wfds && (sc->sc_mask & c->c_mask & ACC_WRITE)) {
			FD_SET(fd, wfds);
		}
		if (efds && (sc->sc_mask & c->c_mask & ACC_EXCEP)) {
			FD_SET(fd, efds);
		}
	}

	/*
	 * If we found any of interest, return now
	 */
	if (count > 0) {
		return(count);
	}

	/*
	 * Nope, none matched.  Re-sleep.
	 */
	goto retry;
}
