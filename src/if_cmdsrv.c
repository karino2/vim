/*
 * TODO:
 *   document
 *
 *   error message
 *
 *   Use inet socket?
 *
 *   test
 *
 *   unix: How to automatically remove socket file which is not listened
 *   (i.e. vim crashed)?  To test with connect() makes many connections
 *   and it may cause EAGAIN error with following connect().  No need to
 *   do it?
 *
 *
 * Changed:
 *
 *  - unix: use unix domain socket.
 *    /tmp/vim-cmdsrv-<uid>/<serverid>
 *
 *  - win32: use named pipe.
 *    \\.\pipe\vim-cmdsrv-<serverid>
 *
 *  - expand('<client>') returns server name instead of number.
 *
 */

/* for struct ucred */
#ifdef __linux__
# define _GNU_SOURCE
#endif

#include "vim.h"
#include "version.h"

#if defined(FEAT_CLIENTSERVER) || defined(PROTO)

#ifdef WIN3264
# include "if_cmdsrv_win32.c"
#else
# include "if_cmdsrv_unix.c"
#endif

typedef struct
{
    char_u *type;
    char_u *sender;
    char_u *script;
    char_u *reply;
    char_u *code;
    char_u *encoding;
    int ncode;
} cmdsrv_message_T;

typedef struct
{
    char_u *serverid;
    garray_T strings;
} cmdsrv_server_reply_T;

static garray_T cmdsrv_reply = GA_EMPTY;

static int cmdsrv_is_serial_name(char_u *name);
static cmdsrv_server_reply_T *cmdsrv_reply_find(char_u *serverid);
static int cmdsrv_reply_add(char_u *serverid, char_u *str);
static int cmdsrv_reply_delete(char_u *serverid);
static int cmdsrv_msg_keys(char_u *keys, char_u **abuf, size_t *abufsize);
static int cmdsrv_msg_expr(char_u *expr, char_u **abuf, size_t *abufsize);
static int cmdsrv_msg_reply(char_u *reply, int code, char_u **abuf, size_t *abufsize);
static int cmdsrv_msg_notification(char_u *reply, char_u **abuf, size_t *abufsize);
static int cmdsrv_parse_message(char_u *msg, cmdsrv_message_T *result);
static int cmdsrv_receive_keys(cmdsrv_handle_t conn, cmdsrv_message_T *pmsg);
static int cmdsrv_receive_expr(cmdsrv_handle_t conn, cmdsrv_message_T *pmsg);
static int cmdsrv_receive_notification(cmdsrv_handle_t conn, cmdsrv_message_T *pmsg);
static char_u *cmdsrv_convert(char_u *client_enc, char_u *data);

static cmdsrv_handle_t cmdsrv_server = NULL;

#ifdef FEAT_GUI
# define CMDSRV_HAS_GUI (gui.in_use || gui.starting)
# if defined(FEAT_GUI_X11)
#  define CMDSRV_OPEN (cmdsrv_listenfd != -1)
static void cmdsrv_message_from_client(XtPointer clientData, int *unused1, XtInputId *unused2);
static XtInputId inputHandler = (XtInputId)NULL;
# elif defined(FEAT_GUI_GTK)
#  define CMDSRV_OPEN (cmdsrv_listenfd != -1)
static void cmdsrv_message_from_client(gpointer clientData, gint unused1, GdkInputCondition unused2);
static gint inputHandler = 0;
# elif defined(FEAT_GUI_W32)
#  define CMDSRV_OPEN (cmdsrv_events != NULL)
# endif
#endif

#define CMDSRV_SEND_MSEC_POLL 50

int
cmdsrv_init(void)
{
    ga_init2(&cmdsrv_reply, sizeof(cmdsrv_server_reply_T), 1);

    return 0;
}

int
cmdsrv_uninit(void)
{
    cmdsrv_server_reply_T *p;
    int i;

    if (cmdsrv_server == NULL)
	return 0;

    vim_free(serverName);
    serverName = NULL;

    vim_free(cmdsrv_clientid);
    cmdsrv_clientid = NULL;

    if (cmdsrv_serv_close(cmdsrv_server) != 0)
	return -1;

    cmdsrv_server = NULL;

    p = (cmdsrv_server_reply_T *)cmdsrv_reply.ga_data;
    for (i = 0; i < cmdsrv_reply.ga_len; ++i)
    {
	vim_free(p[i].serverid);
	ga_clear(&p[i].strings);
    }
    ga_clear(&cmdsrv_reply);

    return 0;
}

#ifdef FEAT_GUI
int
cmdsrv_gui_register(void)
{
    if (!CMDSRV_HAS_GUI || !CMDSRV_OPEN)
	return 0;

#if defined(FEAT_GUI_X11)
    if (inputHandler == (XtInputId)NULL)
    {
	inputHandler = XtAppAddInput(
		(XtAppContext)app_context,
		cmdsrv_listenfd,
		(XtPointer)(XtInputReadMask + XtInputExceptMask),
		cmdsrv_message_from_client,
		NULL);
    }
#elif defined(FEAT_GUI_GTK)
    if (inputHandler == 0)
    {
	inputHandler = gdk_input_add(
		(gint)cmdsrv_listenfd,
		(GdkInputCondition)((int)GDK_INPUT_READ
						+ (int)GDK_INPUT_EXCEPTION),
		cmdsrv_message_from_client,
		NULL);
    }
#endif

    return 0;
}

int
cmdsrv_gui_unregister(void)
{
#if defined(FEAT_GUI_X11)
    if (inputHandler != (XtInputId)NULL)
    {
	XtRemoveInput(inputHandler);
	inputHandler = (XtInputId)NULL;
    }
#elif defined(FEAT_GUI_GTK)
    if (inputHandler != 0)
    {
	gdk_input_remove(inputHandler);
	inputHandler = 0;
    }
#endif

    return 0;
}
#endif

int
cmdsrv_register_name(char_u *name)
{
    cmdsrv_handle_t server = NULL;
    char_u *serverid = NULL;
    int i;

    serverid = alloc(STRLEN(name) + 3 + 1); /* name + 1-999 + NUL */
    if (serverid == NULL)
	return -1;

    STRCPY(serverid, name);

    if (cmdsrv_serv_listen(serverid, &server) != 0)
    {
	/* Specified name is not available.  Try to register with postfix. */
	/* XXX: For backward compatibility, try loosename even if the
	 * specified name is serial name (name[0-999]). */
	for (i = 1; i < 1000; ++i)
	{
	    sprintf((char *)serverid, "%s%d", name, i);
	    if (cmdsrv_serv_listen(serverid, &server) == 0)
		break;
	}
    }

    if (server == NULL)
    {
	MSG_ATTR(_("Unable to register a command server name"),
							hl_attr(HLF_W));
	vim_free(serverid);
	return -1;
    }

    serverName = strup_save(serverid);
    if (serverName == NULL)
    {
	vim_free(serverid);
	cmdsrv_serv_close(server);
	return -1;
    }

#ifdef FEAT_EVAL
    set_vim_var_string(VV_SEND_SERVER, serverName, -1);
#endif

#ifdef FEAT_TITLE
    need_maketitle = TRUE;
#endif

    vim_free(serverid);

    cmdsrv_server = server;

    return 0;
}

int
cmdsrv_server_list(char_u **result)
{
    garray_T ga;
    char_u *list;
    char_u *p;

    list = cmdsrv_list(NUL);
    if (list == NULL)
	return -1;

    ga_init2(&ga, (int)sizeof(char), 100);

    /* filter temporary server name */
    for (p = list; *p != NUL; p += STRLEN(p) + 1)
    {
	if (STRNICMP(p, CMDSRV_TMPNAME, STRLEN(CMDSRV_TMPNAME)) != 0)
	{
	    ga_concat(&ga, p);
	    ga_append(&ga, '\n');
	}
    }

    ga_append(&ga, NUL);

    vim_free(list);

    *result = ga.ga_data;

    return 0;
}

/*
 * @param timeoutmsec -1 infinit, 0 nowait, >0 millisecond
 * @return -1 error, 0 timeout, 1 received (not handled yet)
 */
int
cmdsrv_wait_request(int timeoutmsec)
{
    if (cmdsrv_server == NULL)
	return -1;
    return cmdsrv_serv_wait(cmdsrv_server, timeoutmsec);
}

int
cmdsrv_handle_requests()
{
    cmdsrv_handle_t conn;
    char_u *abuf;
    size_t abufsize;
    cmdsrv_message_T msg;
    int n;

    if (cmdsrv_server == NULL)
	return -1;

    for (;;)
    {
	if (got_int)
	    return -1;

	n = cmdsrv_serv_wait(cmdsrv_server, 0);
	if (n < 0)
	    return -1;
	if (n == 0)
	    return 0;

	if (cmdsrv_serv_accept(cmdsrv_server, &conn) != 0)
	    continue;

	if (cmdsrv_read_message(conn, (void **)&abuf, &abufsize) != 0)
	{
	    cmdsrv_conn_close(conn);
	    continue;
	}

	if (cmdsrv_parse_message(abuf, &msg) != 0)
	{
	    cmdsrv_conn_close(conn);
	    continue;
	}

	if (STRICMP(msg.type, "keys") == 0)
	{
	    cmdsrv_receive_keys(conn, &msg);
	}
	else if (STRICMP(msg.type, "expr") == 0)
	{
	    cmdsrv_receive_expr(conn, &msg);
	}
	else if (STRICMP(msg.type, "notification") == 0)
	{
	    cmdsrv_receive_notification(conn, &msg);
	}

	vim_free(abuf);

	cmdsrv_conn_close(conn);
    }
}

int
cmdsrv_send_keys(char_u *serverid, char_u *keys)
{
    cmdsrv_handle_t conn;
    char_u *abuf;
    size_t abufsize;

    if (serverName != NULL && STRICMP(serverid, serverName) == 0)
    {
	server_to_input_buf(keys);
	return 0;
    }

    if (cmdsrv_cli_conn(serverid, &conn) != 0)
    {
	EMSG(_("E248: Failed to send command to the destination program"));
	return -1;
    }

    if (cmdsrv_msg_keys(keys, &abuf, &abufsize) != 0)
    {
	cmdsrv_conn_close(conn);
	return -1;
    }

    if (cmdsrv_write_message(conn, abuf, abufsize) != 0)
    {
	EMSG(_("E248: Failed to send command to the destination program"));
	vim_free(abuf);
	cmdsrv_conn_close(conn);
	return -1;
    }

    vim_free(abuf);

    if (cmdsrv_conn_close(conn) != 0)
    {
	EMSG(_("Exxx: close error"));
	return -1;
    }

    return 0;
}

int
cmdsrv_send_expr(char_u *serverid, char_u *expr, char_u **result)
{
    cmdsrv_handle_t conn;
    char_u *abuf;
    size_t abufsize;
    cmdsrv_message_T msg;
    char_u *reply;
    char_u *exprbuf;

    if (serverName != NULL && STRICMP(serverid, serverName) == 0)
    {
	/* Use allocated buffer for string literal because eval may
	 * modify expr temporarily. */
	exprbuf = vim_strsave(expr);
	if (exprbuf == NULL)
	    return -1;
	reply = eval_client_expr_to_string(exprbuf);
	vim_free(exprbuf);
	if (result != NULL)
	{
	    if (reply == NULL)
		*result = vim_strsave((char_u *)_(e_invexprmsg));
	    else
		*result = reply;
	}
	else
	    vim_free(reply);
	return reply == NULL ? -1 : 0;
    }

    if (cmdsrv_cli_conn(serverid, &conn) != 0)
    {
	EMSG(_("E248: Failed to send command to the destination program"));
	return -1;
    }

    if (cmdsrv_msg_expr(expr, &abuf, &abufsize) != 0)
    {
	cmdsrv_conn_close(conn);
	return -1;
    }

    if (cmdsrv_write_message(conn, abuf, abufsize) != 0)
    {
	EMSG(_("E248: Failed to send command to the destination program"));
	vim_free(abuf);
	cmdsrv_conn_close(conn);
	return -1;
    }

    vim_free(abuf);

    if (cmdsrv_read_message(conn, (void **)&abuf, &abufsize) != 0)
    {
	EMSG(_("Exxx: Failed to receive result"));
	cmdsrv_conn_close(conn);
	return -1;
    }

    if (cmdsrv_parse_message(abuf, &msg) != 0
	    || STRICMP(msg.type, "reply") != 0)
    {
	EMSG(_("Exxx: Failed to parse result"));
	vim_free(abuf);
	cmdsrv_conn_close(conn);
	return -1;
    }

    if (cmdsrv_conn_close(conn) != 0)
    {
	EMSG(_("Exxx: close error"));
	vim_free(abuf);
	return -1;
    }

    if (result != NULL)
	*result = cmdsrv_convert(msg.encoding, msg.reply);

    vim_free(abuf);

    return msg.ncode == 0 ? 0 : -1;
}

int
cmdsrv_send_notification(char_u *serverid, char_u *reply)
{
    cmdsrv_handle_t conn;
    char_u *abuf;
    size_t abufsize;

    if (cmdsrv_cli_conn(serverid, &conn) != 0)
    {
	EMSG(_("E248: Failed to send command to the destination program"));
	return -1;
    }

    if (cmdsrv_msg_notification(reply, &abuf, &abufsize) != 0)
    {
	cmdsrv_conn_close(conn);
	return -1;
    }

    if (cmdsrv_write_message(conn, abuf, abufsize) != 0)
    {
	EMSG(_("E248: Failed to send command to the destination program"));
	vim_free(abuf);
	cmdsrv_conn_close(conn);
	return -1;
    }

    vim_free(abuf);

    if (cmdsrv_conn_close(conn) != 0)
    {
	EMSG(_("Exxx: close error"));
	return -1;
    }

    return 0;
}

int
cmdsrv_foreground(char_u *serverid)
{
#ifdef WIN3264
    HWND hWnd;
    char_u *windowidstr;
    long windowid;
    char_u *endp;

    if (cmdsrv_send_expr(serverid, "v:windowid", &windowidstr) != 0)
	return -1;

    if (windowidstr == NULL)
	return -1;

    errno = 0;
    windowid = strtol(windowidstr, (char **)&endp, 10);
    if (errno != 0 || *endp != NUL || endp == windowidstr)
    {
	vim_free(windowidstr);
	return -1;
    }

    if (windowid == 0)
	return -1;

    hWnd = (HWND)LongToHandle(windowid);

    SetForegroundWindow(hWnd);

    return 0;
#else
    return cmdsrv_send_expr(serverid, "foreground()", NULL);
#endif
}

int
cmdsrv_peek_reply(char_u *serverid, char_u **pstr)
{
    cmdsrv_server_reply_T *p;

    p = cmdsrv_reply_find(serverid);
    if (p != NULL && p->strings.ga_len > 0)
	*pstr = (char_u *)p->strings.ga_data;
    else
	*pstr = NULL;

    return 0;
}

int
cmdsrv_read_reply(char_u *serverid, char_u **pstr)
{
    cmdsrv_server_reply_T *p;
    char_u *s;
    int len;

    p = cmdsrv_reply_find(serverid);
    if (p != NULL && p->strings.ga_len > 0)
    {
	*pstr = vim_strsave(p->strings.ga_data);
	len = STRLEN(*pstr) + 1;
	if (len < p->strings.ga_len)
	{
	    s = (char_u *)p->strings.ga_data;
	    mch_memmove(s, s + len, p->strings.ga_len - len);
	    p->strings.ga_len -= len;
	}
	else
	{
	    cmdsrv_reply_delete(serverid);
	}
	return 0;
    }

    return -1;
}

int
cmdsrv_wait_reply(char_u *serverid, char_u **pstr)
{
    char_u *exists;

    while (!got_int)
    {
	exists = cmdsrv_find_server(serverid, FALSE);
	if (exists == NULL)
	    return -1;
	vim_free(exists);

	if (cmdsrv_peek_reply(serverid, pstr) != 0)
	    return -1;

	if (*pstr != NULL)
	{
	    if (cmdsrv_read_reply(serverid, pstr) != 0)
		return -1;
	    return 0;
	}

	if (cmdsrv_wait_request(CMDSRV_SEND_MSEC_POLL) < 0)
	    return -1;

	if (cmdsrv_handle_requests() != 0)
	    return -1;
    }

    return -1;
}

char_u *
cmdsrv_find_server(char_u *name, int loose)
{
    char_u *list;
    char_u *res;
    char_u *p;
    char_u *e;
    size_t len;

    list = cmdsrv_list(NUL);
    if (list == NULL)
	return NULL;

    for (p = list; *p != NUL; p += STRLEN(p) + 1)
    {
	if (STRICMP(p, name) == 0)
	{
	    res = vim_strsave(p);
	    vim_free(list);
	    return res;
	}
    }

    if (!loose || cmdsrv_is_serial_name(name))
	return NULL;

    len = STRLEN(name);

    for (p = list; *p != NUL; p += STRLEN(p) + 1)
    {
	if (STRNICMP(p, name, len) == 0)
	{
	    e = skipdigits(p + len);
	    if (*e == NUL)
	    {
		res = vim_strsave(p);
		vim_free(list);
		return res;
	    }
	}
    }

    return NULL;
}

/*
 * @return percent-encoded string in allocated memory, NULL for error.
 */
char_u *
cmdsrv_urlencode(char_u *str)
{
    char_u *buf;
    char_u *s;
    char_u *d;

    buf = (char_u *)lalloc_clear((STRLEN(str) * 3) + 1, TRUE);
    if (buf == NULL)
        return NULL;

    d = buf;
    s = str;
    while (*s != NUL)
    {
	if (ASCII_ISALPHA(*s) || VIM_ISDIGIT(*s)
		|| *s == '-' || *s == '.' || *s == '_' || *s == '~')
	    *d++ = *s++;
	else
	    d += sprintf(d, "%%%02x", *s++);
    }
    *d = NUL;

    return buf;
}


/*
 * @return percent-decoded string in allocated memory, NULL for error.
 */
char_u *
cmdsrv_urldecode(char_u *str)
{
    char_u *buf;
    char_u *s;
    char_u *d;
    int c;

    buf = (char_u *)lalloc_clear(STRLEN(str) + 1, TRUE);
    if (buf == NULL)
	return NULL;

    d = buf;
    s = str;
    while (*s != NUL)
    {
	if (*s == '%')
	{
	    c = hexhex2nr(&s[1]);
	    if (c == -1)
	    {
		vim_free(buf);
		return NULL;
	    }
	    *d++ = c;
	    s += 3;
	}
	else
	{
	    *d++ = *s++;
	}
    }
    *d = NUL;

    return buf;
}

static int
cmdsrv_is_serial_name(char_u *name)
{
    size_t len;

    len = STRLEN(name);
    if (len > 1 && vim_isdigit(name[len - 1]))
	return TRUE;
    return FALSE;
}

static cmdsrv_server_reply_T *
cmdsrv_reply_find(char_u *serverid)
{
    cmdsrv_server_reply_T *p;
    int i;

    p = (cmdsrv_server_reply_T *)cmdsrv_reply.ga_data;
    for (i = 0; i < cmdsrv_reply.ga_len; ++i)
    {
	if (STRICMP(p[i].serverid, serverid) == 0)
	    return &p[i];
    }

    return NULL;
}

static int
cmdsrv_reply_add(char_u *serverid, char_u *str)
{
    cmdsrv_server_reply_T *p;

    p = cmdsrv_reply_find(serverid);

    if (p == NULL)
    {
	if (ga_grow(&cmdsrv_reply, 1) != OK)
	    return -1;
	p = ((cmdsrv_server_reply_T *)cmdsrv_reply.ga_data)
	    + cmdsrv_reply.ga_len;
	p->serverid = vim_strsave(serverid);
	if (p->serverid == NULL)
	    return -1;
	ga_init2(&p->strings, 1, 100);
	cmdsrv_reply.ga_len++;
    }

    ga_concat(&p->strings, str);
    ga_append(&p->strings, NUL);

    return 0;
}

static int
cmdsrv_reply_delete(char_u *serverid)
{
    cmdsrv_server_reply_T *p;
    cmdsrv_server_reply_T *e;

    p = cmdsrv_reply_find(serverid);

    if (p != NULL)
    {
	e = ((cmdsrv_server_reply_T *)cmdsrv_reply.ga_data)
	    + cmdsrv_reply.ga_len;
	ga_clear(&p->strings);
	mch_memmove(p, p + 1, (e - (p + 1)) * sizeof(*p));
	cmdsrv_reply.ga_len--;
    }

    return 0;
}

static int
cmdsrv_msg_keys(char_u *keys, char_u **abuf, size_t *abufsize)
{
    garray_T ga;
    char_u *sender;

    sender = serverName;
    if (sender == NULL)
	sender = (char_u *)"";

    ga_init2(&ga, (int)sizeof(char), 100);

    ga_concat(&ga, "type");
    ga_append(&ga, NUL);
    ga_concat(&ga, "keys");
    ga_append(&ga, NUL);

    ga_concat(&ga, "sender");
    ga_append(&ga, NUL);
    ga_concat(&ga, sender);
    ga_append(&ga, NUL);

    ga_concat(&ga, "script");
    ga_append(&ga, NUL);
    ga_concat(&ga, keys);
    ga_append(&ga, NUL);

#ifdef FEAT_MBYTE
    ga_concat(&ga, "encoding");
    ga_append(&ga, NUL);
    ga_concat(&ga, p_enc);
    ga_append(&ga, NUL);
#endif

    ga_append(&ga, NUL);

    *abuf = ga.ga_data;
    *abufsize = ga.ga_len;

    return 0;
}

static int
cmdsrv_msg_expr(char_u *expr, char_u **abuf, size_t *abufsize)
{
    garray_T ga;
    char_u *sender;

    sender = serverName;
    if (sender == NULL)
	sender = (char_u *)"";

    ga_init2(&ga, (int)sizeof(char), 100);

    ga_concat(&ga, "type");
    ga_append(&ga, NUL);
    ga_concat(&ga, "expr");
    ga_append(&ga, NUL);

    ga_concat(&ga, "sender");
    ga_append(&ga, NUL);
    ga_concat(&ga, sender);
    ga_append(&ga, NUL);

    ga_concat(&ga, "script");
    ga_append(&ga, NUL);
    ga_concat(&ga, expr);
    ga_append(&ga, NUL);

#ifdef FEAT_MBYTE
    ga_concat(&ga, "encoding");
    ga_append(&ga, NUL);
    ga_concat(&ga, p_enc);
    ga_append(&ga, NUL);
#endif

    ga_append(&ga, NUL);

    *abuf = ga.ga_data;
    *abufsize = ga.ga_len;

    return 0;
}

static int
cmdsrv_msg_reply(char_u *reply, int code, char_u **abuf, size_t *abufsize)
{
    garray_T ga;
    char_u *sender;
    char codestr[NUMBUFLEN];

    sender = serverName;
    if (sender == NULL)
	sender = (char_u *)"";

    vim_snprintf(codestr, sizeof(codestr), "%d", code);

    ga_init2(&ga, (int)sizeof(char), 100);

    ga_concat(&ga, "type");
    ga_append(&ga, NUL);
    ga_concat(&ga, "reply");
    ga_append(&ga, NUL);

    ga_concat(&ga, "sender");
    ga_append(&ga, NUL);
    ga_concat(&ga, sender);
    ga_append(&ga, NUL);

    ga_concat(&ga, "reply");
    ga_append(&ga, NUL);
    ga_concat(&ga, reply);
    ga_append(&ga, NUL);

    ga_concat(&ga, "code");
    ga_append(&ga, NUL);
    ga_concat(&ga, codestr);
    ga_append(&ga, NUL);

#ifdef FEAT_MBYTE
    ga_concat(&ga, "encoding");
    ga_append(&ga, NUL);
    ga_concat(&ga, p_enc);
    ga_append(&ga, NUL);
#endif

    ga_append(&ga, NUL);

    *abuf = ga.ga_data;
    *abufsize = ga.ga_len;

    return 0;
}

static int
cmdsrv_msg_notification(char_u *reply, char_u **abuf, size_t *abufsize)
{
    char_u *sender;
    garray_T ga;

    sender = serverName;
    if (sender == NULL)
	sender = (char_u *)"";

    ga_init2(&ga, (int)sizeof(char), 100);

    ga_concat(&ga, "type");
    ga_append(&ga, NUL);
    ga_concat(&ga, "notification");
    ga_append(&ga, NUL);

    ga_concat(&ga, "sender");
    ga_append(&ga, NUL);
    ga_concat(&ga, sender);
    ga_append(&ga, NUL);

    ga_concat(&ga, "reply");
    ga_append(&ga, NUL);
    ga_concat(&ga, reply);
    ga_append(&ga, NUL);

#ifdef FEAT_MBYTE
    ga_concat(&ga, "encoding");
    ga_append(&ga, NUL);
    ga_concat(&ga, p_enc);
    ga_append(&ga, NUL);
#endif

    ga_append(&ga, NUL);

    *abuf = ga.ga_data;
    *abufsize = ga.ga_len;

    return 0;
}

static int
cmdsrv_parse_message(char_u *msg, cmdsrv_message_T *result)
{
    char_u *key;
    char_u *value;
    char_u *p;
    char_u *endp;

    vim_memset(result, 0, sizeof(*result));

    p = msg;
    while (*p != NUL)
    {
	key = p;
	p += STRLEN(p) + 1;
	value = p;
	p += STRLEN(p) + 1;
	if (STRICMP(key, "type") == 0)
	    result->type = value;
	else if (STRICMP(key, "sender") == 0)
	    result->sender = value;
	else if (STRICMP(key, "script") == 0)
	    result->script = value;
	else if (STRICMP(key, "reply") == 0)
	    result->reply = value;
	else if (STRICMP(key, "code") == 0)
	    result->code = value;
	else if (STRICMP(key, "encoding") == 0)
	    result->encoding = value;
	else
	    return -1;
    }

    if (result->code != NULL)
    {
	errno = 0;
	result->ncode = strtol(result->code, (char **)&endp, 10);
	if (errno != 0 || *endp != NUL || endp == result->code)
	    return -1;
    }

    if (result->type == NULL)
    {
	return -1;
    }
    else if (STRICMP(result->type, "keys") == 0)
    {
	if (result->sender == NULL || result->script == NULL)
	    return -1;
    }
    else if (STRICMP(result->type, "expr") == 0)
    {
	if (result->sender == NULL || result->script == NULL)
	    return -1;
    }
    else if (STRICMP(result->type, "reply") == 0)
    {
	if (result->sender == NULL || result->reply == NULL
		|| result->code == NULL)
	    return -1;
    }
    else if (STRICMP(result->type, "notification") == 0)
    {
	if (result->sender == NULL || result->reply == NULL)
	    return -1;
    }
    else
    {
	return -1;
    }

    return 0;
}

static int
cmdsrv_receive_keys(cmdsrv_handle_t conn, cmdsrv_message_T *pmsg)
{
    char_u *script;

    /* Remember in global */
    vim_free(cmdsrv_clientid);
    cmdsrv_clientid = vim_strsave(pmsg->sender);

    script = cmdsrv_convert(pmsg->encoding, pmsg->script);
    if (script == NULL)
	return -1;

    server_to_input_buf(script);

    vim_free(script);

    return 0;
}

static int
cmdsrv_receive_expr(cmdsrv_handle_t conn, cmdsrv_message_T *pmsg)
{
    char_u *script;
    char_u *result;
    char_u *reply;
    char_u *abuf;
    size_t abufsize;
    int code;

    /* Remember in global */
    vim_free(cmdsrv_clientid);
    cmdsrv_clientid = vim_strsave(pmsg->sender);

    script = cmdsrv_convert(pmsg->encoding, pmsg->script);
    if (script == NULL)
	return -1;

    result = eval_client_expr_to_string(script);

    vim_free(script);

    if (result != NULL)
    {
	reply = result;
	code = 0;
    }
    else
    {
	reply = (char_u *)_(e_invexprmsg);
	code = 1;
    }

    if (cmdsrv_msg_reply(reply, code, &abuf, &abufsize) != 0)
    {
	vim_free(result);
	return -1;
    }

    vim_free(result);

    if (cmdsrv_write_message(conn, abuf, abufsize) != 0)
    {
	vim_free(abuf);
	return -1;
    }

    vim_free(abuf);

    return 0;
}

static int
cmdsrv_receive_notification(cmdsrv_handle_t conn, cmdsrv_message_T *pmsg)
{
    char_u *reply;

    reply = cmdsrv_convert(pmsg->encoding, pmsg->reply);
    if (reply == NULL)
	return -1;

    if (cmdsrv_reply_add(pmsg->sender, reply) != 0)
    {
	vim_free(reply);
	return -1;
    }

#ifdef FEAT_AUTOCMD
    apply_autocmds(EVENT_REMOTEREPLY, pmsg->sender, reply, TRUE, curbuf);
#endif

    vim_free(reply);

    return 0;
}

static char_u *
cmdsrv_convert(char_u *client_enc, char_u *data)
{
#ifdef FEAT_MBYTE
    if (client_enc != NULL && p_enc != NULL)
    {
	vimconv_T	vimconv;
	char_u *res;

	vimconv.vc_type = CONV_NONE;
	if (convert_setup(&vimconv, client_enc, p_enc) != FAIL
					      && vimconv.vc_type != CONV_NONE)
	    res = string_convert(&vimconv, data, NULL);
	else
	    res = vim_strsave(data);
	convert_setup(&vimconv, NULL, NULL);

	return res;
    }
#endif
    return vim_strsave(data);
}

#if defined(FEAT_GUI_X11)
static void
cmdsrv_message_from_client(XtPointer clientData UNUSED,
			   int *unused1 UNUSED,
			   XtInputId *unused2 UNUSED)
{
    cmdsrv_handle_requests();
}
#elif defined(FEAT_GUI_GTK)
static void
cmdsrv_message_from_client(gpointer clientData UNUSED,
			   gint unused1 UNUSED,
			   GdkInputCondition unused2 UNUSED)
{
    cmdsrv_handle_requests();
}
#endif

#endif /* FEAT_CLIENTSERVER */
