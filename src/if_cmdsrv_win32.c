
#include "vim.h"
#include "version.h"

#include <stddef.h>

#define CMDSRV_POLL_INTERVAL 1000

typedef struct {
    OVERLAPPED oOverlap;
    HANDLE hPipeInst;
    BOOL iopending;
} PIPEINST, *LPPIPEINST;

struct cmdsrv_server {
    PIPEINST pipes[CMDSRV_INSTANCES];
    HANDLE hEvents[CMDSRV_INSTANCES];
};

struct cmdsrv_connection {
    PIPEINST Pipe;
    LPPIPEINST Server;
};

typedef void *cmdsrv_handle_t;

static char_u *cmdsrv_dir(void);
static char_u *cmdsrv_prefix(void);
static char_u *cmdsrv_make_address(char_u *serverid);
static char_u *cmdsrv_list(int sep);
static int cmdsrv_serv_listen(char_u *serverid, cmdsrv_handle_t *pserver);
static int cmdsrv_serv_accept(cmdsrv_handle_t server, cmdsrv_handle_t *pconn);
static int cmdsrv_serv_wait(cmdsrv_handle_t server, int timeoutmsec);
static int cmdsrv_serv_close(cmdsrv_handle_t server);
static int cmdsrv_cli_conn(char_u *serverid, cmdsrv_handle_t *pconn);
static int cmdsrv_conn_close(cmdsrv_handle_t conn);
static int cmdsrv_read_message(cmdsrv_handle_t conn, void **pabuf, size_t *pabufsize);
static int cmdsrv_write_message(cmdsrv_handle_t conn, void *buf, size_t bufsize);
static int cmdsrv_wait(int nitems, HANDLE *objects, int *pidx, int timeoutmsec);
static int _ga_concatmemory(garray_T *gap, void *buf, size_t bufsize);
static BOOL DisconnectAndReconnect(LPPIPEINST lpPipe);
static BOOL ConnectToNewClient(LPPIPEINST lpPipe);

/*
 * @return directory path without last separator
 */
static char_u *
cmdsrv_dir(void)
{
    static char_u buf[256];

    vim_snprintf(buf, sizeof(buf), "\\\\.\\pipe");

    return buf;
}

static char_u *
cmdsrv_prefix(void)
{
    static char_u buf[256];

    vim_snprintf(buf, sizeof(buf), "vim-cmdsrv-");

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
    ga_concat(&ga, "\\");
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
    char_u *serverid;
    char_u *tail;
    char_u *prefix;
    WIN32_FIND_DATA finddata;
    HANDLE h;

    prefix = cmdsrv_prefix();

    vim_snprintf(buf, sizeof(buf), "%s\\*", cmdsrv_dir());

    ga_init2(&ga, (int)sizeof(char), 100);

    h = FindFirstFile(buf, &finddata);
    if (h == INVALID_HANDLE_VALUE)
    {
	ga_clear(&ga);
	return NULL;
    }

    do
    {
	tail = gettail((char_u *)finddata.cFileName);
	if (STRNICMP(tail, prefix, STRLEN(prefix)) == 0)
	{
	    serverid = cmdsrv_urldecode(tail + STRLEN(prefix));
	    if (serverid != NULL)
	    {
		ga_concat(&ga, serverid);
		ga_append(&ga, sep);
		vim_free(serverid);
	    }
	}
    } while (FindNextFile(h, &finddata));

    ga_append(&ga, NUL);

    FindClose(h);

    return (char_u *)ga.ga_data;
}

static int
cmdsrv_serv_listen(char_u *serverid, cmdsrv_handle_t *pserver)
{
    char_u *path;
    int i;
    struct cmdsrv_server *server;

    path = cmdsrv_make_address(serverid);
    if (path == NULL)
	return -1;

    server = (struct cmdsrv_server *)lalloc_clear(
	    sizeof(struct cmdsrv_server), TRUE);
    if (server == NULL)
    {
	vim_free(path);
	return -1;
    }

    for (i = 0; i < CMDSRV_INSTANCES; ++i)
    {
	server->pipes[i].iopending = FALSE;
	server->pipes[i].oOverlap.hEvent = NULL;
	server->pipes[i].hPipeInst = INVALID_HANDLE_VALUE;
    }

    for (i = 0; i < CMDSRV_INSTANCES; ++i)
    {
	server->pipes[i].oOverlap.hEvent = CreateEvent(NULL, TRUE, TRUE, NULL);
	if (server->pipes[i].oOverlap.hEvent == NULL)
	    break;

	server->hEvents[i] = server->pipes[i].oOverlap.hEvent;

	server->pipes[i].hPipeInst = CreateNamedPipe(
		path,
		PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
		PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
		CMDSRV_INSTANCES,
		0,
		0,
		0,
		NULL);
	if (server->pipes[i].hPipeInst == INVALID_HANDLE_VALUE)
	    break;

	if (!ConnectToNewClient(&server->pipes[i]))
	    break;
    }

    if (i < CMDSRV_INSTANCES)
    {
	cmdsrv_serv_close(server);
	vim_free(path);
	return -1;
    }

    vim_free(path);

    *pserver = (cmdsrv_handle_t)server;

    cmdsrv_events = server->hEvents;

    return 0;
}

static int
cmdsrv_serv_accept(cmdsrv_handle_t _server, cmdsrv_handle_t *pconn)
{
    struct cmdsrv_server *server = (struct cmdsrv_server *)_server;
    int idx;
    struct cmdsrv_connection *conn;

    if (cmdsrv_wait(CMDSRV_INSTANCES, server->hEvents, &idx, -1) <= 0)
	return -1;

    server->pipes[idx].iopending = FALSE;

    /* Event object will be re-checked while processing connection.
     * Turn off to prevent to be misinterpreted as new request came. */
    if (!ResetEvent(server->hEvents[idx]))
    {
	DisconnectAndReconnect(&server->pipes[idx]);
	return -1;
    }

    conn = (struct cmdsrv_connection *)lalloc_clear(
	    sizeof(struct cmdsrv_connection), TRUE);
    if (conn == NULL)
    {
	DisconnectAndReconnect(&server->pipes[idx]);
	return -1;
    }

    conn->Pipe.oOverlap.hEvent = CreateEvent(NULL, TRUE, TRUE, NULL);
    if (conn->Pipe.oOverlap.hEvent == NULL)
    {
	vim_free(conn);
	DisconnectAndReconnect(&server->pipes[idx]);
	return -1;
    }

    conn->Pipe.hPipeInst = server->pipes[idx].hPipeInst;
    conn->Server = &server->pipes[idx];

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

    ret = cmdsrv_wait(CMDSRV_INSTANCES, server->hEvents, NULL, timeoutmsec);

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
    int i;

    cmdsrv_events = NULL;

    for (i = 0; i < CMDSRV_INSTANCES; ++i)
    {
	if (server->pipes[i].iopending)
	{
	    if (CancelIo(server->pipes[i].hPipeInst))
		WaitForSingleObject(server->pipes[i].oOverlap.hEvent, INFINITE);
	}
	if (server->pipes[i].hPipeInst != INVALID_HANDLE_VALUE)
	{
	    DisconnectNamedPipe(server->pipes[i].hPipeInst);
	    CloseHandle(server->pipes[i].hPipeInst);
	}
	if (server->pipes[i].oOverlap.hEvent != NULL)
	    CloseHandle(server->pipes[i].oOverlap.hEvent);
    }

    vim_free(server);

    return 0;
}

static int
cmdsrv_cli_conn(char_u *serverid, cmdsrv_handle_t *pconn)
{
    char_u *path;
    struct cmdsrv_connection *conn;

    path = cmdsrv_make_address(serverid);
    if (path == NULL)
	return -1;

    conn = (struct cmdsrv_connection *)lalloc_clear(
	    sizeof(struct cmdsrv_connection), TRUE);
    if (conn == NULL)
    {
	vim_free(path);
	return -1;
    }

    conn->Pipe.oOverlap.hEvent = CreateEvent(NULL, TRUE, TRUE, NULL);
    if (conn->Pipe.oOverlap.hEvent == NULL)
    {
	vim_free(conn);
	vim_free(path);
	return -1;
    }

    conn->Pipe.hPipeInst = CreateFile(
	    path,
	    GENERIC_READ | GENERIC_WRITE,
	    0,
	    NULL,
	    OPEN_EXISTING,
	    FILE_FLAG_OVERLAPPED,
	    NULL);
    if (conn->Pipe.hPipeInst == INVALID_HANDLE_VALUE)
    {
	CloseHandle(conn->Pipe.oOverlap.hEvent);
	vim_free(conn);
	vim_free(path);
	return -1;
    }

    conn->Server = NULL;

    *pconn = (cmdsrv_handle_t)conn;

    return 0;
}

static int
cmdsrv_conn_close(cmdsrv_handle_t _conn)
{
    struct cmdsrv_connection *conn = (struct cmdsrv_connection *)_conn;

    if (conn->Server != NULL)
    {
	CloseHandle(conn->Pipe.oOverlap.hEvent);
	DisconnectAndReconnect(conn->Server);
    }
    else
    {
	CloseHandle(conn->Pipe.oOverlap.hEvent);
	CloseHandle(conn->Pipe.hPipeInst);
    }

    vim_free(conn);

    return 0;
}

static int
cmdsrv_read_message(cmdsrv_handle_t _conn, void **pabuf, size_t *pabufsize)
{
    struct cmdsrv_connection *conn = (struct cmdsrv_connection *)_conn;
    char buf[BUFSIZ];
    DWORD nread;
    garray_T ga;

    ga_init2(&ga, (int)sizeof(char), 100);

    for (;;)
    {
	if (ReadFile(conn->Pipe.hPipeInst, buf, sizeof(buf), &nread,
		    &conn->Pipe.oOverlap))
	{
	    if (_ga_concatmemory(&ga, buf, nread) != OK)
	    {
		ga_clear(&ga);
		return -1;
	    }
	    break;
	}
	else if (GetLastError() == ERROR_MORE_DATA)
	{
	    if (_ga_concatmemory(&ga, buf, nread) != OK)
	    {
		ga_clear(&ga);
		return -1;
	    }
	    continue;
	}
	else if (GetLastError() == ERROR_IO_PENDING)
	{
	    if (cmdsrv_wait(1, &conn->Pipe.oOverlap.hEvent, NULL, -1) <= 0)
	    {
		if (CancelIo(conn->Pipe.hPipeInst))
		    WaitForSingleObject(conn->Pipe.oOverlap.hEvent, INFINITE);
		ga_clear(&ga);
		return -1;
	    }
	    if (GetOverlappedResult(conn->Pipe.hPipeInst,
			&conn->Pipe.oOverlap, &nread, FALSE))
	    {
		if (_ga_concatmemory(&ga, buf, nread) != OK)
		{
		    ga_clear(&ga);
		    return -1;
		}
		break;
	    }
	    else if (GetLastError() == ERROR_MORE_DATA)
	    {
		if (_ga_concatmemory(&ga, buf, nread) != OK)
		{
		    ga_clear(&ga);
		    return -1;
		}
		continue;
	    }
	    else
	    {
		ga_clear(&ga);
		return -1;
	    }
	}
	else
	{
	    ga_clear(&ga);
	    return -1;
	}
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
    DWORD nwrite;

    /* message pipe is atomic for each WriteFile() call. */
    if (WriteFile(conn->Pipe.hPipeInst, buf, bufsize, &nwrite,
		&conn->Pipe.oOverlap))
    {
	return 0;
    }
    else if (GetLastError() == ERROR_IO_PENDING)
    {
	if (cmdsrv_wait(1, &conn->Pipe.oOverlap.hEvent, NULL, -1) <= 0)
	{
	    if (CancelIo(conn->Pipe.hPipeInst))
		WaitForSingleObject(conn->Pipe.oOverlap.hEvent, INFINITE);
	    return -1;
	}
	if (GetOverlappedResult(conn->Pipe.hPipeInst,
		    &conn->Pipe.oOverlap, &nwrite, FALSE))
	{
	    return 0;
	}
	else
	{
	    return -1;
	}
    }
    else
    {
	return -1;
    }
}

/*
 * @param timeoutmsec -1 infinit, 0 nowait, >0 millisecond
 * @return -1 error, 0 timeout, 1 received (not handled yet)
 */
static int
cmdsrv_wait(int nitems, HANDLE *objects, int *pidx, int timeoutmsec)
{
    HANDLE wait_objects[1 + CMDSRV_INSTANCES];
    int wait_nitems = 0;
    DWORD dwWait;
    DWORD starttime;
    DWORD lefttime;
    DWORD waittime;

    /* assert nitems == 1 || objects == cmdsrv_events */

    while (wait_nitems < nitems)
    {
	wait_objects[wait_nitems] = objects[wait_nitems];
	wait_nitems++;
    }

    if (objects != cmdsrv_events)
    {
	mch_memmove(wait_objects + wait_nitems, cmdsrv_events,
		sizeof(HANDLE) * CMDSRV_INSTANCES);
	wait_nitems += CMDSRV_INSTANCES;
    }

    starttime = GetTickCount();

    while (!got_int)
    {
	lefttime = GetTickCount() - starttime;

	if (timeoutmsec < 0)
	    waittime = CMDSRV_POLL_INTERVAL;
	else if (timeoutmsec == 0)
	    waittime = 0;
	else if ((DWORD)timeoutmsec <= lefttime)
	    waittime = 0;
	else if ((DWORD)timeoutmsec - lefttime > CMDSRV_POLL_INTERVAL)
	    waittime = CMDSRV_POLL_INTERVAL;
	else
	    waittime = (DWORD)timeoutmsec - lefttime;

	dwWait = WaitForMultipleObjects(wait_nitems, wait_objects, FALSE,
		waittime);
	if (dwWait == WAIT_FAILED)
	{
	    return -1;
	}
	else if (dwWait == WAIT_TIMEOUT)
	{
	    /* continue */
	}
	else if (WAIT_OBJECT_0 <= dwWait && dwWait < WAIT_OBJECT_0 + nitems)
	{
	    if (pidx != NULL)
		*pidx = dwWait - WAIT_OBJECT_0;
	    return 1;
	}
	else if (WAIT_OBJECT_0 + nitems <= dwWait
				    && dwWait < WAIT_OBJECT_0 + wait_nitems)
	{
	    /* request received */
	}
	else
	{
	    return -1;
	}

	lefttime = GetTickCount() - starttime;
	if (timeoutmsec == 0)
	    return 0;
	else if (timeoutmsec > 0 && (DWORD)timeoutmsec <= lefttime)
	    return 0;

	cmdsrv_handle_requests();

	ui_breakcheck();
    }

    return -1;
}

static int
_ga_concatmemory(garray_T *gap, void *buf, size_t bufsize)
{
    if (ga_grow(gap, bufsize) != OK)
        return FAIL;
    mch_memmove(((char *)gap->ga_data) + gap->ga_len, buf, bufsize);
    gap->ga_len += bufsize;
    return OK;
}

static BOOL
DisconnectAndReconnect(LPPIPEINST lpPipe)
{
    if (!DisconnectNamedPipe(lpPipe->hPipeInst))
	return FALSE;
    return ConnectToNewClient(lpPipe);
}

static BOOL
ConnectToNewClient(LPPIPEINST lpPipe)
{
    BOOL fConnected;

    lpPipe->iopending = FALSE;

    fConnected = ConnectNamedPipe(lpPipe->hPipeInst, &lpPipe->oOverlap);
    /* Overlapped ConnectNamedPipe should return zero. */
    if (fConnected)
    {
	return FALSE;
    }

    switch (GetLastError())
    {
    case ERROR_IO_PENDING:
	lpPipe->iopending = TRUE;
	return TRUE;
    case ERROR_PIPE_CONNECTED:
	if (SetEvent(lpPipe->oOverlap.hEvent))
	    return TRUE;
	/* FALLTHROUGH */
    default:
	return FALSE;
    }
}

