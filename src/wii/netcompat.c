#define _BSD_SOURCE 1

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <poll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <network.h>
#include <sys/iosupport.h>
#include <tuxedo/ppc/intrinsics.h>

u64 gettime(void)
{
	u32 hi, lo, hi2;

	do {
		hi = PPCMftbu();
		lo = PPCMftb();
		hi2 = PPCMftbu();
	} while (hi != hi2);

	return ((u64)hi << 32) | lo;
}

// socket() hands back a *public* handle index; the net_*() layer (net_select,
// net_ioctl, net_recvfrom, ...) only understands the *internal* socket index
// stored in the handle's fileStruct. libogc's own wrappers (recvfrom, fcntl,
// ...) convert via soc_get_fd() before calling net_*(). Our shims must do the
// same, otherwise get_socket() returns NULL and the call silently no-ops
// (this is why fcntl(F_SETFL, O_NONBLOCK) never made ENet's socket nonblocking).
static int to_net_fd(int fd)
{
	__handle *h = __get_handle(fd);
	if (h == NULL)
		return -1;
	if (strcmp(devoptab_list[h->device]->name, "soc") != 0)
		return -1;
	return *(s32 *)h->fileStruct;
}

// Public-handle -> internal net_* index. Exposed so the ENet backend (which
// drives the net_* API directly, like libogc's udptest) can translate the
// socket() handle it was handed into the index net_send/net_recv/net_connect
// expect.
int netcompat_netfd(int fd)
{
	return to_net_fd(fd);
}

// The real-HW IOS rejects net_sendto() when the destination address is not the
// 8-byte wii_sockaddr_in layout: it reads destaddr[0] as sin_len and returns
// EINVAL (22) for the 16-byte standard struct sockaddr_in that libogc's
// sendto() passes straight through. (Dolphin's emulated IOS is lenient, so the
// video/audio pings work there but fail on real HW, and the server never learns
// the client's ports -> "no video/audio traffic".) Normalize to the 8-byte
// layout before calling net_sendto().
int netcompat_sendto(int sockfd, const void *buf, int len, int flags,
                     const struct sockaddr *dest, int addrlen)
{
	int nfd = to_net_fd(sockfd);
	if (nfd < 0) {
		errno = ENOTSOCK;
		return -1;
	}

	if (dest == NULL)
		return net_send(nfd, buf, len, flags);

	// IPv4 only. The first 8 bytes of a standard sockaddr_in
	// (sin_len, sin_family, sin_port, sin_addr) are layout-compatible with the
	// 8-byte wii_sockaddr_in the IOS expects.
	unsigned char waddr[8];
	if (addrlen < 8) {
		errno = EINVAL;
		return -1;
	}
	memcpy(waddr, dest, 8);
	waddr[0] = 8;        // sin_len
	waddr[1] = AF_INET;  // sin_family

	int ret = net_sendto(nfd, buf, len, flags, (struct sockaddr *)waddr, 8);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return ret;
}

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
	   struct timeval *timeout)
{
	fd_set r, w, e, r0, w0, e0;
	int maxfd = 0;
	int fd;
	int ret;

	FD_ZERO(&r);
	FD_ZERO(&w);
	FD_ZERO(&e);
	FD_ZERO(&r0);
	FD_ZERO(&w0);
	FD_ZERO(&e0);
	if (readfds)
		r0 = *readfds;
	if (writefds)
		w0 = *writefds;
	if (exceptfds)
		e0 = *exceptfds;

	for (fd = 0; fd < nfds; fd++) {
		int nfd = to_net_fd(fd);
		if (nfd < 0)
			continue;
		if (nfd > maxfd)
			maxfd = nfd;
		if (FD_ISSET(fd, &r0))
			FD_SET(nfd, &r);
		if (FD_ISSET(fd, &w0))
			FD_SET(nfd, &w);
		if (FD_ISSET(fd, &e0))
			FD_SET(nfd, &e);
	}

	ret = net_select(maxfd + 1, &r, &w, &e, timeout);
	if (ret < 0) {
		// A net_select error is a transient IOS hiccup (e.g. the Wi-Fi link
		// flapping), not a reason to tear down the session. Treat it as a
		// timeout: honor the requested wait so we don't spin, then report "no
		// events". Real disconnects are caught by the higher-level no-traffic
		// and ENet reliable-ACK timeouts.
		if (timeout) {
			int ms = timeout->tv_sec * 1000 + timeout->tv_usec / 1000;
			if (ms > 0)
				usleep((useconds_t)(ms * 1000));
		}
		FD_ZERO(&r);
		FD_ZERO(&w);
		FD_ZERO(&e);
		ret = 0;
	}

	if (readfds) {
		*readfds = r0;
		for (fd = 0; fd < nfds; fd++) {
			int nfd = to_net_fd(fd);
			if (nfd >= 0 && FD_ISSET(nfd, &r))
				FD_SET(fd, readfds);
			else
				FD_CLR(fd, readfds);
		}
	}
	if (writefds) {
		*writefds = w0;
		for (fd = 0; fd < nfds; fd++) {
			int nfd = to_net_fd(fd);
			if (nfd >= 0 && FD_ISSET(nfd, &w))
				FD_SET(fd, writefds);
			else
				FD_CLR(fd, writefds);
		}
	}
	if (exceptfds) {
		*exceptfds = e0;
		for (fd = 0; fd < nfds; fd++) {
			int nfd = to_net_fd(fd);
			if (nfd >= 0 && FD_ISSET(nfd, &e))
				FD_SET(fd, exceptfds);
			else
				FD_CLR(fd, exceptfds);
		}
	}

	return ret;
}

// libogc's HW_DOL poll() passes the pollfd *count* as net_select()'s first
// argument instead of highest-fd+1, so it never watches sockets with fd >= 1.
// Provide a correct implementation on top of net_select(). The pollfd values
// are *public* handle indices, so each is converted to its internal net index
// (to_net_fd) before being placed in the fd_set that net_select() inspects.
int poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
	fd_set readfds, writefds, exceptfds;
	struct timeval tv;
	struct timeval *tvp = NULL;
	int maxfd = 0;
	int i;

	FD_ZERO(&readfds);
	FD_ZERO(&writefds);
	FD_ZERO(&exceptfds);

	if (timeout >= 0) {
		tv.tv_sec = timeout / 1000;
		tv.tv_usec = (timeout % 1000) * 1000;
		tvp = &tv;
	}

	for (i = 0; i < (int) nfds; i++) {
		int nfd = (fds[i].fd < 0) ? -1 : to_net_fd(fds[i].fd);
		fds[i].revents = 0;
		if (nfd < 0)
			continue;
		if (nfd > maxfd)
			maxfd = nfd;
		if (fds[i].events & POLLIN)
			FD_SET(nfd, &readfds);
		if (fds[i].events & POLLOUT)
			FD_SET(nfd, &writefds);
		if (fds[i].events & (POLLERR | POLLHUP))
			FD_SET(nfd, &exceptfds);
	}

	int ret = net_select(maxfd + 1, &readfds, &writefds, &exceptfds, tvp);
	if (ret < 0) {
		// A net_select error is a transient IOS hiccup (e.g. the Wi-Fi link
		// flapping), not a reason to tear down the session. Treat it as a
		// timeout: honor the requested wait so we don't spin, then report "no
		// events". Real disconnects are caught by the higher-level no-traffic
		// and ENet reliable-ACK timeouts.
		static int errCount = 0;
		if (++errCount % 256 == 1)
			printf("[poll] net_select error ret=%d (treated as timeout, %d so far)\n", ret, errCount);
		if (timeout >= 0)
			usleep((useconds_t)(timeout * 1000));
		FD_ZERO(&readfds);
		FD_ZERO(&writefds);
		FD_ZERO(&exceptfds);
		ret = 0;
	}

	for (i = 0; i < (int) nfds; i++) {
		int nfd = (fds[i].fd < 0) ? -1 : to_net_fd(fds[i].fd);
		if (nfd < 0)
			continue;
		if (FD_ISSET(nfd, &readfds))
			fds[i].revents |= POLLIN;
		if (FD_ISSET(nfd, &writefds))
			fds[i].revents |= POLLOUT;
		if (FD_ISSET(nfd, &exceptfds))
			fds[i].revents |= POLLERR;
	}

	return ret;
}

int ioctl(int fd, int request, ...)
{
	void *argp;
	va_list ap;

	va_start(ap, request);
	argp = va_arg(ap, void *);
	va_end(ap);

	if (request == FIONBIO) {
		// libogc's net_fcntl() is an unimplemented stub; only net_ioctl()
		// actually toggles the O_NONBLOCK flag on the socket. net_ioctl()
		// needs the internal fd, so convert the public handle first.
		int nfd = to_net_fd(fd);
		if (nfd < 0) {
			errno = ENOTSOCK;
			return -1;
		}
		return net_ioctl(nfd, FIONBIO, argp);
	}

	errno = ENOSYS;
	return -1;
}

// libogc's fcntl() routes to the stubbed net_fcntl(), so fcntl(F_SETFL,
// O_NONBLOCK) silently no-ops and sockets stay blocking. ENet relies on this
// to make its UDP socket non-blocking, so provide a working implementation
// on top of net_ioctl(FIONBIO).
int fcntl(int fd, int cmd, ...)
{
	int arg = 0;
	va_list ap;

	va_start(ap, cmd);
	arg = va_arg(ap, int);
	va_end(ap);

	if (cmd == F_SETFL) {
		int on = (arg & O_NONBLOCK) ? 1 : 0;
		int nfd = to_net_fd(fd);
		if (nfd < 0) {
			errno = ENOTSOCK;
			return -1;
		}
		return net_ioctl(nfd, FIONBIO, &on);
	}

	if (cmd == F_GETFL) {
		return 0;
	}

	errno = ENOSYS;
	return -1;
}

int getaddrinfo(const char *node, const char *service,
	       const struct addrinfo *hints, struct addrinfo **res)
{
	struct in_addr inaddr;
	struct hostent *he;
	struct addrinfo *ai;
	struct sockaddr_in *sin;

	if (node == NULL)
		return EAI_NONAME;

	if (hints != NULL && hints->ai_family != AF_UNSPEC &&
	    hints->ai_family != AF_INET)
		return EAI_ADDRFAMILY;

	// libogc's gethostbyname only does DNS (no dotted-quad), so resolve an
	// IP literal with inet_aton first; fall back to DNS for hostnames.
	he = NULL;
	if (inet_aton(node, &inaddr) == 0) {
		he = gethostbyname(node);
		if (he == NULL)
			return EAI_NONAME;
	}

	ai = calloc(1, sizeof(*ai));
	sin = calloc(1, sizeof(*sin));
	if (ai == NULL || sin == NULL) {
		free(ai);
		free(sin);
		return EAI_MEMORY;
	}

	sin->sin_family = AF_INET;
	if (he != NULL)
		sin->sin_addr = *((struct in_addr *)he->h_addr_list[0]);
	else
		sin->sin_addr = inaddr;
	if (service != NULL)
		sin->sin_port = htons((unsigned short)atoi(service));

	ai->ai_family = AF_INET;
	ai->ai_socktype = (hints != NULL && hints->ai_socktype != 0)
			   ? hints->ai_socktype : SOCK_STREAM;
	ai->ai_protocol = 0;
	ai->ai_addrlen = sizeof(*sin);
	ai->ai_addr = (struct sockaddr *)sin;
	ai->ai_next = NULL;

	*res = ai;
	return 0;
}

void freeaddrinfo(struct addrinfo *res)
{
	if (res != NULL) {
		free(res->ai_addr);
		free(res);
	}
}

int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact)
{
	(void)signum;
	(void)act;
	(void)oldact;
	return 0;
}
