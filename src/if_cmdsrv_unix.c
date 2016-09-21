
#include "vim.h"
#include "version.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#ifdef HAVE_GETPEERUCRED
# include <ucred.h>
#endif

/* for struct xucred */
#ifdef LOCAL_PEERCRED
# include <sys/ucred.h>
#endif

#define CMDSRV_POLL_INTERVAL 1000

struct cmdsrv_server {
    int handle;
    char_u *path;
};

struct cmdsrv_connection {
    int handle;
};

typedef void *cmdsrv_handle_t;

static char_u *cmdsrv_dir(void);
static char_u *cmdsrv_prefix(void);
static char_u *cmdsrv_make_address(char_u *serverid);
static char_u *cmdsrv_list(int sep);
static int cmdsrv_is_alive(char_u *serverid);
static uid_t cmdsrv_getpeeruid(int fd);
static int cmdsrv_serv_listen(char_u *serverid, cmdsrv_handle_t *pserver);
static int cmdsrv_serv_accept(cmdsrv_handle_t server, cmdsrv_handle_t *pconn);
static int cmdsrv_serv_wait(cmdsrv_handle_t server, int timeoutmsec);
static int cmdsrv_serv_close(cmdsrv_handle_t server);
static int cmdsrv_cli_conn(char_u *serverid, cmdsrv_handle_t *pconn);
static int cmdsrv_conn_close(cmdsrv_handle_t conn);
static int cmdsrv_read_message(cmdsrv_handle_t conn, void **pabuf, size_t *pabufsize);
static int cmdsrv_write_message(cmdsrv_handle_t conn, void *buf, size_t bufsize);
static int cmdsrv_wait(int rfd, int wfd, int timeoutmsec);
static int _ga_concatmemory(garray_T *gap, void *buf, size_t bufsize);
static long_u _GetTickCount(void);

/*
 * @return directory path without last separator
 */
static char_u *
cmdsrv_dir(void)
{
    static char_u buf[256];

    vim_snprintf(buf, sizeof(buf), "/tmp/vim-cmdsrv-%d", (int)getuid());

    return buf;
}

static char_u *
cmdsrv_prefix(void)
{
    static char_u buf[256];

    vim_snprintf(buf, sizeof(buf), "");

    return buf;
}

static char_u *
cmdsrv_make_address(char_u *serverid)
{
    garray_T ga;
    char_u *upname;
    char_u *encoded;

    upname = strup_save(serverid);
    if (upname == NULL)
	return NULL;

    encoded = cmdsrv_urlencode(upname);
    if (encoded == NULL)
    {
	vim_free(upname);
	return NULL;
    }

    vim_free(upname);

    ga_init2(&ga, (int)sizeof(char), 100);

    ga_concat(&ga, cmdsrv_dir());
    ga_concat(&ga, "/");
    ga_concat(&ga, cmdsrv_prefix());
    ga_concat(&ga, encoded);

    vim_free(encoded);

    return (char_u *)ga.ga_data;
}

static char_u *
cmdsrv_list(int sep)
{
    garray_T ga;
    char_u buf[256];
    char_u *pat[1];
    int num_files;
    char_u **files;
    int flags = EW_FILE | EW_ICASE | EW_SILENT;
    char_u *serverid;
    char_u *tail;
    char_u *path;
    char_u *prefix;
    int i;

    prefix = cmdsrv_prefix();

    vim_snprintf(buf, sizeof(buf), "%s/%s*", cmdsrv_dir(), prefix);

    pat[0] = buf;

    if (gen_expand_wildcards(1, pat, &num_files, &files, flags) == FAIL)
    {
	return vim_strsave((char_u *)"");
    }

    ga_init2(&ga, (int)sizeof(char), 100);

    for (i = 0; i < num_files; ++i)
    {
	path = files[i];
	tail = gettail(path);
	if (STRNICMP(tail, prefix, STRLEN(prefix)) == 0)
	{
	    serverid = cmdsrv_urldecode(tail + STRLEN(prefix));
	    if (serverid != NULL)
	    {
#if 0
		if (!cmdsrv_is_alive(serverid)
			&& (errno == ENOENT || errno == ECONNREFUSED))
		{
		    /* server might down with trouble. */
		    mch_remove(path);
		}
		else
#endif
		{
		    ga_concat(&ga, serverid);
		    ga_append(&ga, sep);
		}
		vim_free(serverid);
	    }
	}
    }

    ga_append(&ga, NUL);

    FreeWild(num_files, files);

    return (char_u *)ga.ga_data;
}

static int
cmdsrv_is_alive(char_u *serverid)
{
    cmdsrv_handle_t conn;

    if (cmdsrv_cli_conn(serverid, &conn) == 0)
    {
	cmdsrv_conn_close(conn);
	return 1;
    }

    return 0;
}

/*
 * @return peer uid.  -1 for error.
 * TODO: more variant?
 */
static uid_t
cmdsrv_getpeeruid(int fd)
{
#if defined(HAVE_GETPEERUCRED)
    /* Solaris */
    {
	ucred_t *cred = NULL;
	uid_t uid;

	if (getpeerucred(fd, &cred) != 0)
	    return -1;
	uid = ucred_geteuid(cred);
	ucred_free(cred);
	return uid;
    }
#elif defined(HAVE_GETPEEREID)
    /* FreeBSD */
    {
	uid_t uid;
	gid_t gid;

	if (getpeereid(fd, &uid, &gid) != 0)
	    return -1;
	return uid;
    }
#elif defined(LOCAL_PEERCRED)
    /* FreeBSD */
    {
	struct xucred cred;
	socklen_t len;

	len = sizeof(cred);
	if (getsockopt(fd, 0, LOCAL_PEERCRED, &cred, &len) != 0)
	    return -1;
	return cred.cr_uid;
    }
#elif defined(SO_PEERCRED)
    /* Linux */
    {
	struct ucred cred;
	socklen_t len;

	len = sizeof(cred);
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0)
	    return -1;
	return cred.uid;
    }
#else
    {
	/* No method available.  Rely on directory permission. */
	return getuid();
    }
#endif
}

static int
cmdsrv_serv_listen(char_u *serverid, cmdsrv_handle_t *pserver)
{
    int fd;
    socklen_t len;
    struct sockaddr_un unix_addr;
    char_u *path;
    struct stat st;
    struct cmdsrv_server *server;

    if (!mch_isdir(cmdsrv_dir()))
    {
	if (vim_mkdir(cmdsrv_dir(), 0700) != 0)
	    return -1;
    }

    if (mch_stat((char *)cmdsrv_dir(), &st) != 0
	    || st.st_uid != getuid()
	    || (st.st_mode & 0777) != 0700)
    {
	return -1;
    }

    path = cmdsrv_make_address(serverid);
    if (path == NULL)
	return -1;

    if (STRLEN(path) + 1 > sizeof(unix_addr.sun_path))
    {
	vim_free(path);
	return -1;
    }

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
    {
	vim_free(path);
	return -1;
    }

    memset(&unix_addr, 0, sizeof(unix_addr));
    unix_addr.sun_family = AF_UNIX;
    strcpy(unix_addr.sun_path, path);
    len = SUN_LEN(&unix_addr);

    if (bind(fd, (struct sockaddr*)&unix_addr, len) != 0)
    {
	close(fd);
	vim_free(path);
	return -1;
    }

    if (listen(fd, CMDSRV_INSTANCES) != 0)
    {
	mch_remove(path);
	close(fd);
	vim_free(path);
	return -1;
    }

    server = (struct cmdsrv_server *)lalloc_clear(
	    sizeof(struct cmdsrv_server), TRUE);
    if (server == NULL)
    {
	mch_remove(path);
	close(fd);
	vim_free(path);
	return -1;
    }

    server->handle = fd;
    server->path = path;

    *pserver = (cmdsrv_handle_t)server;

    cmdsrv_listenfd = fd;
#ifdef FEAT_GUI
    cmdsrv_gui_register();
#endif

    return 0;
}

static int
cmdsrv_serv_accept(cmdsrv_handle_t _server, cmdsrv_handle_t *pconn)
{
    struct cmdsrv_server *server = (struct cmdsrv_server *)_server;
    int listenfd;
    int fd;
    socklen_t len;
    struct sockaddr_un unix_addr;
    struct cmdsrv_connection *conn;

    listenfd = server->handle;

    len = sizeof(unix_addr);
    fd = accept(listenfd, (struct sockaddr *)&unix_addr, &len);
    if (fd < 0)
	return -1;

    if (cmdsrv_getpeeruid(fd) != getuid())
    {
	close(fd);
	return -1;
    }

    conn = (struct cmdsrv_connection *)lalloc_clear(
	    sizeof(struct cmdsrv_connection), TRUE);
    if (conn == NULL)
    {
	close(fd);
	return -1;
    }

    conn->handle = fd;

    *pconn = (cmdsrv_handle_t)conn;

    return 0;
}

static int
cmdsrv_serv_wait(cmdsrv_handle_t _server, int timeoutmsec)
{
    struct cmdsrv_server *server = (struct cmdsrv_server *)_server;
    static int lock = 0;
    int ret;

    /* Don't allow accept request while waiting request */
    if (lock != 0)
	return 0;

    /* When watching object is signaled while polling gui event, it is
     * never handled because waiting session is locked.  Then, the
     * signal is not reset and gui event loop never finish.
     * Unregister watching object temporarily. */
#ifdef FEAT_GUI
    if (timeoutmsec != 0)
	cmdsrv_gui_unregister();
#endif

    ++lock;

    ret = cmdsrv_wait(server->handle, -1, timeoutmsec);

    --lock;

#ifdef FEAT_GUI
    if (timeoutmsec != 0)
	cmdsrv_gui_register();
#endif

    return ret;
}

static int
cmdsrv_serv_close(cmdsrv_handle_t _server)
{
    struct cmdsrv_server *server = (struct cmdsrv_server *)_server;

#ifdef FEAT_GUI
    cmdsrv_gui_unregister();
#endif
    cmdsrv_listenfd = -1;

    if (close(server->handle) != 0)
	return -1;

    mch_remove(server->path);

    vim_free(server->path);
    vim_free(server);

    return 0;
}

static int
cmdsrv_cli_conn(char_u *serverid, cmdsrv_handle_t *pconn)
{
    int fd;
    socklen_t len;
    struct sockaddr_un unix_addr;
    int flag;
    char_u *path;
    struct cmdsrv_connection *conn;
    socklen_t errlen;
    int err;
    int ret;

    path = cmdsrv_make_address(serverid);
    if (path == NULL)
	return -1;

    if (STRLEN(path) + 1 > sizeof(unix_addr.sun_path))
    {
	vim_free(path);
	return -1;
    }

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
    {
	vim_free(path);
	return -1;
    }

    /* Set NONBLOCK mode to prevent to block with connect(). */
    flag = fcntl(fd, F_GETFL, 0);
    if (flag == -1)
    {
	close(fd);
	vim_free(path);
	return -1;
    }

    if (fcntl(fd, F_SETFL, flag | O_NONBLOCK) == -1)
    {
	close(fd);
	vim_free(path);
	return -1;
    }

    memset(&unix_addr, 0, sizeof(unix_addr));
    unix_addr.sun_family = AF_UNIX;
    strcpy(unix_addr.sun_path, path);
    len = SUN_LEN(&unix_addr);

    ret = connect(fd, (struct sockaddr *)&unix_addr, len);
    if (ret < 0)
    {
	if (errno == EINPROGRESS)
	{
	    if (cmdsrv_wait(-1, fd, -1) > 0)
	    {
		errlen = sizeof(err);
		if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen) == 0
			&& err == 0)
		    ret = 0;
	    }
	}
    }
    if (ret < 0)
    {
	close(fd);
	vim_free(path);
	return -1;
    }

    conn = (struct cmdsrv_connection *)lalloc_clear(
	    sizeof(struct cmdsrv_connection), TRUE);
    if (conn == NULL)
    {
	close(fd);
	vim_free(path);
	return -1;
    }

    conn->handle = fd;

    *pconn = (cmdsrv_handle_t)conn;

    return 0;
}

static int
cmdsrv_conn_close(cmdsrv_handle_t _conn)
{
    struct cmdsrv_connection *conn = (struct cmdsrv_connection *)_conn;

    if (close(conn->handle) != 0)
	return -1;

    vim_free(conn);

    return 0;
}

static int
cmdsrv_read_message(cmdsrv_handle_t _conn, void **pabuf, size_t *pabufsize)
{
    struct cmdsrv_connection *conn = (struct cmdsrv_connection *)_conn;
    int fd;
    char buf[BUFSIZ];
    ssize_t nread;
    garray_T ga;

    fd = conn->handle;

    ga_init2(&ga, (int)sizeof(char), 100);

    for (;;)
    {
	if (cmdsrv_wait(fd, -1, -1) <= 0)
	{
	    ga_clear(&ga);
	    return -1;
	}

	nread = read(fd, buf, sizeof(buf));
	if (nread < 0)
	{
	    if (errno == EINTR)
		continue;
	    ga_clear(&ga);
	    return -1;
	}
	else if (nread == 0)
	    break;
	if (_ga_concatmemory(&ga, buf, nread) != OK)
	{
	    ga_clear(&ga);
	    return -1;
	}
    }

    /* On MacOSX, shutdown() may fail when the connection is already
     * closed by peer. */
    if (shutdown(fd, SHUT_RD) < 0 && errno != ENOTCONN)
    {
	ga_clear(&ga);
	return -1;
    }

    *pabufsize = ga.ga_len;

    /* ensure NUL terminated to ease to parse */
    ga_append(&ga, NUL);
    ga_append(&ga, NUL);
    ga_append(&ga, NUL);

    *pabuf = ga.ga_data;

    return 0;
}

static int
cmdsrv_write_message(cmdsrv_handle_t _conn, void *buf, size_t bufsize)
{
    struct cmdsrv_connection *conn = (struct cmdsrv_connection *)_conn;
    int fd;
    ssize_t nwrite;
    size_t nwritten;

    fd = conn->handle;

    nwritten = 0;

    while (nwritten < bufsize)
    {
	if (cmdsrv_wait(-1, fd, -1) <= 0)
	{
	    return -1;
	}

	nwrite = write(fd, ((char*)buf) + nwritten, bufsize - nwritten);
	if (nwrite < 0)
	{
	    if (errno == EINTR)
		continue;
	    return -1;
	}
	nwritten += nwrite;
    }

    /* On MacOSX, shutdown() may fail when the connection is already
     * closed by peer. */
    if (shutdown(fd, SHUT_WR) < 0 && errno != ENOTCONN)
	return -1;

    return 0;
}

/*
 * Wait single object, rfd for read, wfd for write.
 * @param timeoutmsec -1 infinit, 0 nowait, >0 millisecond
 * @return -1 error, 0 timeout, 1 received (not handled yet)
 */
#ifdef HAVE_SELECT
static int
cmdsrv_wait(int rfd, int wfd, int timeoutmsec)
{
    int maxfd;
    struct timeval tv;
    fd_set rset;
    fd_set wset;
    int n;
    long_u starttime;
    long_u lefttime;
    long_u waittime;

    starttime = _GetTickCount();

    while (!got_int)
    {
	maxfd = 0;

	FD_ZERO(&rset);
	FD_ZERO(&wset);

	if (rfd != -1)
	{
	    FD_SET(rfd, &rset);
	    if (maxfd < rfd)
		maxfd = rfd;
	}

	if (wfd != -1)
	{
	    FD_SET(wfd, &wset);
	    if (maxfd < wfd)
		maxfd = wfd;
	}

	if (cmdsrv_listenfd != -1
		&& cmdsrv_listenfd != rfd && cmdsrv_listenfd != wfd)
	{
	    FD_SET(cmdsrv_listenfd, &rset);
	    if (maxfd < cmdsrv_listenfd)
		maxfd = cmdsrv_listenfd;
	}

	lefttime = _GetTickCount() - starttime;
	if (timeoutmsec < 0)
	    waittime = CMDSRV_POLL_INTERVAL;
	else if (timeoutmsec == 0)
	    waittime = 0;
	else if ((long_u)timeoutmsec <= lefttime)
	    waittime = 0;
	else if ((long_u)timeoutmsec - lefttime > CMDSRV_POLL_INTERVAL)
	    waittime = CMDSRV_POLL_INTERVAL;
	else
	    waittime = (long_u)timeoutmsec - lefttime;

	tv.tv_sec = waittime / 1000;
	tv.tv_usec = waittime % 1000 * 1000;

	n = select(maxfd + 1, &rset, &wset, NULL, &tv);
	if (n < 0)
	    return -1;

	if (rfd != -1 && FD_ISSET(rfd, &rset))
	    return 1;

	if (wfd != -1 && FD_ISSET(wfd, &wset))
	    return 1;

	lefttime = _GetTickCount() - starttime;
	if (timeoutmsec == 0)
	    return 0;
	else if (timeoutmsec > 0 && (long_u)timeoutmsec <= lefttime)
	    return 0;

	cmdsrv_handle_requests();

	ui_breakcheck();
    }

    return -1;
}
#else
static int
cmdsrv_wait(int rfd, int wfd, int timeoutmsec)
{
    struct pollfd fds[2];
    int nfds;
    int n;
    long_u starttime;
    long_u lefttime;
    long_u waittime;

    starttime = _GetTickCount();

    while (!got_int)
    {
	nfds = 0;

	if (rfd != -1)
	{
	    fds[0].fd = rfd;
	    fds[0].events = POLLIN;
	    nfds = 1;
	}

	if (wfd != -1)
	{
	    fds[0].fd = wfd;
	    fds[0].events = POLLOUT;
	    nfds = 1;
	}

	if (cmdsrv_listenfd != -1
		&& cmdsrv_listenfd != rfd && cmdsrv_listenfd != wfd)
	{
	    fds[nfds].fd = cmdsrv_listenfd;
	    fds[nfds].events = POLLIN;
	    nfds++;
	}

	lefttime = _GetTickCount() - starttime;

	if (timeoutmsec < 0)
	    waittime = CMDSRV_POLL_INTERVAL;
	else if (timeoutmsec == 0)
	    waittime = 0;
	else if ((long_u)timeoutmsec <= lefttime)
	    waittime = 0;
	else if ((long_u)timeoutmsec - lefttime > CMDSRV_POLL_INTERVAL)
	    waittime = CMDSRV_POLL_INTERVAL;
	else
	    waittime = (long_u)timeoutmsec - lefttime;

	n = poll(fds, nfds, (int)waittime);
	if (n < 0)
	    return -1;

	if (rfd != -1)
	{
	    if (fds[0].revents & POLLIN)
		return 1;
	    else if (fds[0].revents & (POLLERR | POLLHUP))
		return -1;
	}

	if (wfd != -1)
	{
	    if (fds[0].revents & POLLOUT)
		return 1;
	    else if (fds[0].revents & (POLLERR | POLLHUP))
		return -1;
	}

	lefttime = _GetTickCount() - starttime;
	if (timeoutmsec == 0)
	    return 0;
	else if (timeoutmsec > 0 && (long_u)timeoutmsec <= lefttime)
	    return 0;

	cmdsrv_handle_requests();

	ui_breakcheck();
    }

    return -1;
}
#endif

static int
_ga_concatmemory(garray_T *gap, void *buf, size_t bufsize)
{
    if (ga_grow(gap, bufsize) != OK)
        return FAIL;
    mch_memmove(((char *)gap->ga_data) + gap->ga_len, buf, bufsize);
    gap->ga_len += bufsize;
    return OK;
}

static long_u
_GetTickCount(void)
{
    struct timeval tv;

    if (gettimeofday(&tv, NULL) != 0)
	return (long_u)-1;

    return (tv.tv_sec * 1000) + (tv.tv_usec / 1000);
}

