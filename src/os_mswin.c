/* vi:set ts=8 sts=4 sw=4 noet:
 *
 * VIM - Vi IMproved	by Bram Moolenaar
 *
 * Do ":help uganda"  in Vim to read copying and usage conditions.
 * Do ":help credits" in Vim to see a list of people who contributed.
 * See README.txt for an overview of the Vim source code.
 */

/*
 * os_mswin.c
 *
 * Routines for Win32.
 */

#include "vim.h"

#include <sys/types.h>
#include <signal.h>
#include <limits.h>
#ifndef PROTO
# include <process.h>
#endif

#undef chdir
#ifdef __GNUC__
# ifndef __MINGW32__
#  include <dirent.h>
# endif
#else
# include <direct.h>
#endif

#ifndef PROTO
# if defined(FEAT_TITLE) && !defined(FEAT_GUI_W32)
#  include <shellapi.h>
# endif

# if defined(FEAT_PRINTER) && !defined(FEAT_POSTSCRIPT)
#  include <dlgs.h>
#  ifdef WIN3264
#   include <winspool.h>
#  else
#   include <print.h>
#  endif
#  include <commdlg.h>
#endif

#endif /* PROTO */

#ifdef __MINGW32__
# ifndef FROM_LEFT_1ST_BUTTON_PRESSED
#  define FROM_LEFT_1ST_BUTTON_PRESSED    0x0001
# endif
# ifndef RIGHTMOST_BUTTON_PRESSED
#  define RIGHTMOST_BUTTON_PRESSED	  0x0002
# endif
# ifndef FROM_LEFT_2ND_BUTTON_PRESSED
#  define FROM_LEFT_2ND_BUTTON_PRESSED    0x0004
# endif
# ifndef FROM_LEFT_3RD_BUTTON_PRESSED
#  define FROM_LEFT_3RD_BUTTON_PRESSED    0x0008
# endif
# ifndef FROM_LEFT_4TH_BUTTON_PRESSED
#  define FROM_LEFT_4TH_BUTTON_PRESSED    0x0010
# endif

/*
 * EventFlags
 */
# ifndef MOUSE_MOVED
#  define MOUSE_MOVED   0x0001
# endif
# ifndef DOUBLE_CLICK
#  define DOUBLE_CLICK  0x0002
# endif
#endif

/*
 * When generating prototypes for Win32 on Unix, these lines make the syntax
 * errors disappear.  They do not need to be correct.
 */
#ifdef PROTO
#define WINAPI
#define WINBASEAPI
typedef int BOOL;
typedef int CALLBACK;
typedef int COLORREF;
typedef int CONSOLE_CURSOR_INFO;
typedef int COORD;
typedef int DWORD;
typedef int ENUMLOGFONT;
typedef int HANDLE;
typedef int HDC;
typedef int HFONT;
typedef int HICON;
typedef int HWND;
typedef int INPUT_RECORD;
typedef int KEY_EVENT_RECORD;
typedef int LOGFONT;
typedef int LPARAM;
typedef int LPBOOL;
typedef int LPCSTR;
typedef int LPCWSTR;
typedef int LPSTR;
typedef int LPTSTR;
typedef int LPWSTR;
typedef int LRESULT;
typedef int MOUSE_EVENT_RECORD;
typedef int NEWTEXTMETRIC;
typedef int PACL;
typedef int PRINTDLG;
typedef int PSECURITY_DESCRIPTOR;
typedef int PSID;
typedef int SECURITY_INFORMATION;
typedef int SHORT;
typedef int SMALL_RECT;
typedef int TEXTMETRIC;
typedef int UINT;
typedef int WCHAR;
typedef int WORD;
typedef int WPARAM;
typedef void VOID;
#endif

/* Record all output and all keyboard & mouse input */
/* #define MCH_WRITE_DUMP */

#ifdef MCH_WRITE_DUMP
FILE* fdDump = NULL;
#endif

#ifdef WIN3264
extern DWORD g_PlatformId;
#endif

#ifndef FEAT_GUI_MSWIN
extern char g_szOrigTitle[];
#endif

#ifdef FEAT_GUI
extern HWND s_hwnd;
#else
static HWND s_hwnd = 0;	    /* console window handle, set by GetConsoleHwnd() */
#endif

#ifdef FEAT_JOB_CHANNEL
int WSInitialized = FALSE; /* WinSock is initialized */
#endif

/* Don't generate prototypes here, because some systems do have these
 * functions. */
#if defined(__GNUC__) && !defined(PROTO)
# ifndef __MINGW32__
int _stricoll(char *a, char *b)
{
    // the ANSI-ish correct way is to use strxfrm():
    char a_buff[512], b_buff[512];  // file names, so this is enough on Win32
    strxfrm(a_buff, a, 512);
    strxfrm(b_buff, b, 512);
    return strcoll(a_buff, b_buff);
}

char * _fullpath(char *buf, char *fname, int len)
{
    LPTSTR toss;

    return (char *)GetFullPathName(fname, len, buf, &toss);
}
# endif

# if !defined(__MINGW32__) || (__GNUC__ < 4)
int _chdrive(int drive)
{
    char temp [3] = "-:";
    temp[0] = drive + 'A' - 1;
    return !SetCurrentDirectory(temp);
}
# endif
#else
# ifdef __BORLANDC__
/* being a more ANSI compliant compiler, BorlandC doesn't define _stricoll:
 * but it does in BC 5.02! */
#  if __BORLANDC__ < 0x502
int _stricoll(char *a, char *b)
{
#   if 1
    // this is fast but not correct:
    return stricmp(a, b);
#   else
    // the ANSI-ish correct way is to use strxfrm():
    char a_buff[512], b_buff[512];  // file names, so this is enough on Win32
    strxfrm(a_buff, a, 512);
    strxfrm(b_buff, b, 512);
    return strcoll(a_buff, b_buff);
#   endif
}
#  endif
# endif
#endif


#if defined(FEAT_GUI_MSWIN) || defined(PROTO)
/*
 * GUI version of mch_exit().
 * Shut down and exit with status `r'
 * Careful: mch_exit() may be called before mch_init()!
 */
    void
mch_exit(int r)
{
    display_errors();

    ml_close_all(TRUE);		/* remove all memfiles */

# ifdef FEAT_CLIENTSERVER
    cmdsrv_uninit();
# endif
# ifdef FEAT_OLE
    UninitOLE();
# endif
# ifdef FEAT_JOB_CHANNEL
    if (WSInitialized)
    {
	WSInitialized = FALSE;
	WSACleanup();
    }
# endif
#ifdef DYNAMIC_GETTEXT
    dyn_libintl_end();
#endif

    if (gui.in_use)
	gui_exit(r);

#ifdef EXITFREE
    free_all_mem();
#endif

    exit(r);
}

#endif /* FEAT_GUI_MSWIN */


/*
 * Init the tables for toupper() and tolower().
 */
    void
mch_early_init(void)
{
    int		i;

#ifdef WIN3264
    PlatformId();
#endif

    /* Init the tables for toupper() and tolower() */
    for (i = 0; i < 256; ++i)
	toupper_tab[i] = tolower_tab[i] = i;
#ifdef WIN3264
    CharUpperBuff((LPSTR)toupper_tab, 256);
    CharLowerBuff((LPSTR)tolower_tab, 256);
#else
    AnsiUpperBuff((LPSTR)toupper_tab, 256);
    AnsiLowerBuff((LPSTR)tolower_tab, 256);
#endif
}


/*
 * Return TRUE if the input comes from a terminal, FALSE otherwise.
 */
    int
mch_input_isatty(void)
{
#ifdef FEAT_GUI_MSWIN
    return OK;	    /* GUI always has a tty */
#else
    if (isatty(read_cmd_fd))
	return TRUE;
    return FALSE;
#endif
}

#ifdef FEAT_TITLE
/*
 * mch_settitle(): set titlebar of our window
 */
    void
mch_settitle(
    char_u *title,
    char_u *icon)
{
# ifdef FEAT_GUI_MSWIN
    gui_mch_settitle(title, icon);
# else
    if (title != NULL)
    {
#  ifdef FEAT_MBYTE
	if (enc_codepage >= 0 && (int)GetACP() != enc_codepage)
	{
	    /* Convert the title from 'encoding' to the active codepage. */
	    WCHAR	*wp = enc_to_utf16(title, NULL);
	    int	n;

	    if (wp != NULL)
	    {
		n = SetConsoleTitleW(wp);
		vim_free(wp);
		if (n != 0 || GetLastError() != ERROR_CALL_NOT_IMPLEMENTED)
		    return;
	    }
	}
#  endif
	SetConsoleTitle((LPCSTR)title);
    }
# endif
}


/*
 * Restore the window/icon title.
 * which is one of:
 *  1: Just restore title
 *  2: Just restore icon (which we don't have)
 *  3: Restore title and icon (which we don't have)
 */
/*ARGSUSED*/
    void
mch_restore_title(
    int which)
{
#ifndef FEAT_GUI_MSWIN
    SetConsoleTitle(g_szOrigTitle);
#endif
}


/*
 * Return TRUE if we can restore the title (we can)
 */
    int
mch_can_restore_title(void)
{
    return TRUE;
}


/*
 * Return TRUE if we can restore the icon title (we can't)
 */
    int
mch_can_restore_icon(void)
{
    return FALSE;
}
#endif /* FEAT_TITLE */


/*
 * Get absolute file name into buffer "buf" of length "len" bytes,
 * turning all '/'s into '\\'s and getting the correct case of each component
 * of the file name.  Append a (back)slash to a directory name.
 * When 'shellslash' set do it the other way around.
 * Return OK or FAIL.
 */
/*ARGSUSED*/
    int
mch_FullName(
    char_u	*fname,
    char_u	*buf,
    int		len,
    int		force)
{
    int		nResult = FAIL;

#ifdef __BORLANDC__
    if (*fname == NUL) /* Borland behaves badly here - make it consistent */
	nResult = mch_dirname(buf, len);
    else
#endif
    {
#ifdef FEAT_MBYTE
	if (enc_codepage >= 0 && (int)GetACP() != enc_codepage
# ifdef __BORLANDC__
		/* Wide functions of Borland C 5.5 do not work on Windows 98. */
		&& g_PlatformId == VER_PLATFORM_WIN32_NT
# endif
	   )
	{
	    WCHAR	*wname;
	    WCHAR	wbuf[MAX_PATH];
	    char_u	*cname = NULL;

	    /* Use the wide function:
	     * - convert the fname from 'encoding' to UCS2.
	     * - invoke _wfullpath()
	     * - convert the result from UCS2 to 'encoding'.
	     */
	    wname = enc_to_utf16(fname, NULL);
	    if (wname != NULL && _wfullpath(wbuf, wname, MAX_PATH) != NULL)
	    {
		cname = utf16_to_enc((short_u *)wbuf, NULL);
		if (cname != NULL)
		{
		    vim_strncpy(buf, cname, len - 1);
		    nResult = OK;
		}
	    }
	    vim_free(wname);
	    vim_free(cname);
	}
	if (nResult == FAIL)	    /* fall back to non-wide function */
#endif
	{
	    if (_fullpath((char *)buf, (const char *)fname, len - 1) == NULL)
	    {
		/* failed, use relative path name */
		vim_strncpy(buf, fname, len - 1);
	    }
	    else
		nResult = OK;
	}
    }

#ifdef USE_FNAME_CASE
    fname_case(buf, len);
#else
    slash_adjust(buf);
#endif

    return nResult;
}


/*
 * Return TRUE if "fname" does not depend on the current directory.
 */
    int
mch_isFullName(char_u *fname)
{
#ifdef FEAT_MBYTE
    /* WinNT and later can use _MAX_PATH wide characters for a pathname, which
     * means that the maximum pathname is _MAX_PATH * 3 bytes when 'enc' is
     * UTF-8. */
    char szName[_MAX_PATH * 3 + 1];
#else
    char szName[_MAX_PATH + 1];
#endif

    /* A name like "d:/foo" and "//server/share" is absolute */
    if ((fname[0] && fname[1] == ':' && (fname[2] == '/' || fname[2] == '\\'))
	    || (fname[0] == fname[1] && (fname[0] == '/' || fname[0] == '\\')))
	return TRUE;

    /* A name that can't be made absolute probably isn't absolute. */
    if (mch_FullName(fname, (char_u *)szName, sizeof(szName) - 1, FALSE) == FAIL)
	return FALSE;

    return pathcmp((const char *)fname, (const char *)szName, -1) == 0;
}

/*
 * Replace all slashes by backslashes.
 * This used to be the other way around, but MS-DOS sometimes has problems
 * with slashes (e.g. in a command name).  We can't have mixed slashes and
 * backslashes, because comparing file names will not work correctly.  The
 * commands that use a file name should try to avoid the need to type a
 * backslash twice.
 * When 'shellslash' set do it the other way around.
 * When the path looks like a URL leave it unmodified.
 */
    void
slash_adjust(char_u *p)
{
    if (path_with_url(p))
	return;
    while (*p)
    {
	if (*p == psepcN)
	    *p = psepc;
	mb_ptr_adv(p);
    }
}

/* Use 64-bit stat functions if available. */
#ifdef HAVE_STAT64
# undef stat
# undef _stat
# undef _wstat
# undef _fstat
# define stat _stat64
# define _stat _stat64
# define _wstat _wstat64
# define _fstat _fstat64
#endif

#if (defined(_MSC_VER) && (_MSC_VER >= 1300)) || defined(__MINGW32__)
# define OPEN_OH_ARGTYPE intptr_t
#else
# define OPEN_OH_ARGTYPE long
#endif

    static int
stat_symlink_aware(const char *name, stat_T *stp)
{
#if (defined(_MSC_VER) && (_MSC_VER < 1900)) || defined(__MINGW32__)
    /* Work around for VC12 or earlier (and MinGW). stat() can't handle
     * symlinks properly.
     * VC9 or earlier: stat() doesn't support a symlink at all. It retrieves
     * status of a symlink itself.
     * VC10: stat() supports a symlink to a normal file, but it doesn't support
     * a symlink to a directory (always returns an error).
     * VC11 and VC12: stat() doesn't return an error for a symlink to a
     * directory, but it doesn't set S_IFDIR flag.
     * MinGW: Same as VC9. */
    WIN32_FIND_DATA	findData;
    HANDLE		hFind, h;
    DWORD		attr = 0;
    BOOL		is_symlink = FALSE;

    hFind = FindFirstFile(name, &findData);
    if (hFind != INVALID_HANDLE_VALUE)
    {
	attr = findData.dwFileAttributes;
	if ((attr & FILE_ATTRIBUTE_REPARSE_POINT)
		&& (findData.dwReserved0 == IO_REPARSE_TAG_SYMLINK))
	    is_symlink = TRUE;
	FindClose(hFind);
    }
    if (is_symlink)
    {
	h = CreateFile(name, FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
		OPEN_EXISTING,
		(attr & FILE_ATTRIBUTE_DIRECTORY)
					    ? FILE_FLAG_BACKUP_SEMANTICS : 0,
		NULL);
	if (h != INVALID_HANDLE_VALUE)
	{
	    int	    fd, n;

	    fd = _open_osfhandle((OPEN_OH_ARGTYPE)h, _O_RDONLY);
	    n = _fstat(fd, (struct _stat *)stp);
	    if ((n == 0) && (attr & FILE_ATTRIBUTE_DIRECTORY))
		stp->st_mode = (stp->st_mode & ~S_IFREG) | S_IFDIR;
	    _close(fd);
	    return n;
	}
    }
#endif
    return stat(name, stp);
}

#ifdef FEAT_MBYTE
    static int
wstat_symlink_aware(const WCHAR *name, stat_T *stp)
{
# if (defined(_MSC_VER) && (_MSC_VER < 1900)) || defined(__MINGW32__)
    /* Work around for VC12 or earlier (and MinGW). _wstat() can't handle
     * symlinks properly.
     * VC9 or earlier: _wstat() doesn't support a symlink at all. It retrieves
     * status of a symlink itself.
     * VC10: _wstat() supports a symlink to a normal file, but it doesn't
     * support a symlink to a directory (always returns an error).
     * VC11 and VC12: _wstat() doesn't return an error for a symlink to a
     * directory, but it doesn't set S_IFDIR flag.
     * MinGW: Same as VC9. */
    int			n;
    BOOL		is_symlink = FALSE;
    HANDLE		hFind, h;
    DWORD		attr = 0;
    WIN32_FIND_DATAW	findDataW;

    hFind = FindFirstFileW(name, &findDataW);
    if (hFind != INVALID_HANDLE_VALUE)
    {
	attr = findDataW.dwFileAttributes;
	if ((attr & FILE_ATTRIBUTE_REPARSE_POINT)
		&& (findDataW.dwReserved0 == IO_REPARSE_TAG_SYMLINK))
	    is_symlink = TRUE;
	FindClose(hFind);
    }
    if (is_symlink)
    {
	h = CreateFileW(name, FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
		OPEN_EXISTING,
		(attr & FILE_ATTRIBUTE_DIRECTORY)
					    ? FILE_FLAG_BACKUP_SEMANTICS : 0,
		NULL);
	if (h != INVALID_HANDLE_VALUE)
	{
	    int	    fd;

	    fd = _open_osfhandle((OPEN_OH_ARGTYPE)h, _O_RDONLY);
	    n = _fstat(fd, (struct _stat *)stp);
	    if ((n == 0) && (attr & FILE_ATTRIBUTE_DIRECTORY))
		stp->st_mode = (stp->st_mode & ~S_IFREG) | S_IFDIR;
	    _close(fd);
	    return n;
	}
    }
# endif
    return _wstat(name, (struct _stat *)stp);
}
#endif

/*
 * stat() can't handle a trailing '/' or '\', remove it first.
 */
    int
vim_stat(const char *name, stat_T *stp)
{
#ifdef FEAT_MBYTE
    /* WinNT and later can use _MAX_PATH wide characters for a pathname, which
     * means that the maximum pathname is _MAX_PATH * 3 bytes when 'enc' is
     * UTF-8. */
    char_u	buf[_MAX_PATH * 3 + 1];
#else
    char_u	buf[_MAX_PATH + 1];
#endif
    char_u	*p;

    vim_strncpy((char_u *)buf, (char_u *)name, sizeof(buf) - 1);
    p = buf + STRLEN(buf);
    if (p > buf)
	mb_ptr_back(buf, p);

    /* Remove trailing '\\' except root path. */
    if (p > buf && (*p == '\\' || *p == '/') && p[-1] != ':')
	*p = NUL;

    if ((buf[0] == '\\' && buf[1] == '\\') || (buf[0] == '/' && buf[1] == '/'))
    {
	/* UNC root path must be followed by '\\'. */
	p = vim_strpbrk(buf + 2, (char_u *)"\\/");
	if (p != NULL)
	{
	    p = vim_strpbrk(p + 1, (char_u *)"\\/");
	    if (p == NULL)
		STRCAT(buf, "\\");
	}
    }
#ifdef FEAT_MBYTE
    if (enc_codepage >= 0 && (int)GetACP() != enc_codepage
# ifdef __BORLANDC__
	    /* Wide functions of Borland C 5.5 do not work on Windows 98. */
	    && g_PlatformId == VER_PLATFORM_WIN32_NT
# endif
       )
    {
	WCHAR	*wp = enc_to_utf16(buf, NULL);
	int	n;

	if (wp != NULL)
	{
	    n = wstat_symlink_aware(wp, stp);
	    vim_free(wp);
	    if (n >= 0 || g_PlatformId == VER_PLATFORM_WIN32_NT)
		return n;
	    /* Retry with non-wide function (for Windows 98). Can't use
	     * GetLastError() here and it's unclear what errno gets set to if
	     * the _wstat() fails for missing wide functions. */
	}
    }
#endif
    return stat_symlink_aware((char *)buf, stp);
}

#if defined(FEAT_GUI_MSWIN) || defined(PROTO)
/*ARGSUSED*/
    void
mch_settmode(int tmode)
{
    /* nothing to do */
}

    int
mch_get_shellsize(void)
{
    /* never used */
    return OK;
}

    void
mch_set_shellsize(void)
{
    /* never used */
}

/*
 * Rows and/or Columns has changed.
 */
    void
mch_new_shellsize(void)
{
    /* never used */
}

#endif

/*
 * We have no job control, so fake it by starting a new shell.
 */
    void
mch_suspend(void)
{
    suspend_shell();
}

#if defined(USE_MCH_ERRMSG) || defined(PROTO)

#ifdef display_errors
# undef display_errors
#endif

/*
 * Display the saved error message(s).
 */
    void
display_errors(void)
{
    char *p;

    if (error_ga.ga_data != NULL)
    {
	/* avoid putting up a message box with blanks only */
	for (p = (char *)error_ga.ga_data; *p; ++p)
	    if (!isspace(*p))
	    {
		(void)gui_mch_dialog(
#ifdef FEAT_GUI
				     gui.starting ? VIM_INFO :
#endif
					     VIM_ERROR,
#ifdef FEAT_GUI
				     gui.starting ? (char_u *)_("Message") :
#endif
					     (char_u *)_("Error"),
				     (char_u *)p, (char_u *)_("&Ok"),
					1, NULL, FALSE);
		break;
	    }
	ga_clear(&error_ga);
    }
}
#endif


/*
 * Return TRUE if "p" contain a wildcard that can be expanded by
 * dos_expandpath().
 */
    int
mch_has_exp_wildcard(char_u *p)
{
    for ( ; *p; mb_ptr_adv(p))
    {
	if (vim_strchr((char_u *)"?*[", *p) != NULL
		|| (*p == '~' && p[1] != NUL))
	    return TRUE;
    }
    return FALSE;
}

/*
 * Return TRUE if "p" contain a wildcard or a "~1" kind of thing (could be a
 * shortened file name).
 */
    int
mch_has_wildcard(char_u *p)
{
    for ( ; *p; mb_ptr_adv(p))
    {
	if (vim_strchr((char_u *)
#  ifdef VIM_BACKTICK
				    "?*$[`"
#  else
				    "?*$["
#  endif
						, *p) != NULL
		|| (*p == '~' && p[1] != NUL))
	    return TRUE;
    }
    return FALSE;
}


/*
 * The normal _chdir() does not change the default drive.  This one does.
 * Returning 0 implies success; -1 implies failure.
 */
    int
mch_chdir(char *path)
{
    if (path[0] == NUL)		/* just checking... */
	return -1;

    if (p_verbose >= 5)
    {
	verbose_enter();
	smsg((char_u *)"chdir(%s)", path);
	verbose_leave();
    }
    if (isalpha(path[0]) && path[1] == ':')	/* has a drive name */
    {
	/* If we can change to the drive, skip that part of the path.  If we
	 * can't then the current directory may be invalid, try using chdir()
	 * with the whole path. */
	if (_chdrive(TOLOWER_ASC(path[0]) - 'a' + 1) == 0)
	    path += 2;
    }

    if (*path == NUL)		/* drive name only */
	return 0;

#ifdef FEAT_MBYTE
    if (enc_codepage >= 0 && (int)GetACP() != enc_codepage)
    {
	WCHAR	*p = enc_to_utf16((char_u *)path, NULL);
	int	n;

	if (p != NULL)
	{
	    n = _wchdir(p);
	    vim_free(p);
	    if (n == 0 || g_PlatformId == VER_PLATFORM_WIN32_NT)
		return n;
	    /* Retry with non-wide function (for Windows 98). */
	}
    }
#endif

    return chdir(path);	       /* let the normal chdir() do the rest */
}


/*
 * Switching off termcap mode is only allowed when Columns is 80, otherwise a
 * crash may result.  It's always allowed on NT or when running the GUI.
 */
/*ARGSUSED*/
    int
can_end_termcap_mode(
    int give_msg)
{
#ifdef FEAT_GUI_MSWIN
    return TRUE;	/* GUI starts a new console anyway */
#else
    if (g_PlatformId == VER_PLATFORM_WIN32_NT || Columns == 80)
	return TRUE;
    if (give_msg)
	msg((char_u *)
		_("'columns' is not 80, cannot execute external commands"));
    return FALSE;
#endif
}

#ifdef FEAT_GUI_MSWIN
/*
 * return non-zero if a character is available
 */
    int
mch_char_avail(void)
{
    /* never used */
    return TRUE;
}
#endif


/*
 * set screen mode, always fails.
 */
/*ARGSUSED*/
    int
mch_screenmode(
    char_u *arg)
{
    EMSG(_(e_screenmode));
    return FAIL;
}


#if defined(FEAT_LIBCALL) || defined(PROTO)
/*
 * Call a DLL routine which takes either a string or int param
 * and returns an allocated string.
 * Return OK if it worked, FAIL if not.
 */
# ifdef WIN3264
typedef LPTSTR (*MYSTRPROCSTR)(LPTSTR);
typedef LPTSTR (*MYINTPROCSTR)(int);
typedef int (*MYSTRPROCINT)(LPTSTR);
typedef int (*MYINTPROCINT)(int);
# else
typedef LPSTR (*MYSTRPROCSTR)(LPSTR);
typedef LPSTR (*MYINTPROCSTR)(int);
typedef int (*MYSTRPROCINT)(LPSTR);
typedef int (*MYINTPROCINT)(int);
# endif

/*
 * Check if a pointer points to a valid NUL terminated string.
 * Return the length of the string, including terminating NUL.
 * Returns 0 for an invalid pointer, 1 for an empty string.
 */
    static size_t
check_str_len(char_u *str)
{
    SYSTEM_INFO			si;
    MEMORY_BASIC_INFORMATION	mbi;
    size_t			length = 0;
    size_t			i;
    const char_u		*p;

    /* get page size */
    GetSystemInfo(&si);

    /* get memory information */
    if (VirtualQuery(str, &mbi, sizeof(mbi)))
    {
	/* pre cast these (typing savers) */
	long_u dwStr = (long_u)str;
	long_u dwBaseAddress = (long_u)mbi.BaseAddress;

	/* get start address of page that str is on */
	long_u strPage = dwStr - (dwStr - dwBaseAddress) % si.dwPageSize;

	/* get length from str to end of page */
	long_u pageLength = si.dwPageSize - (dwStr - strPage);

	for (p = str; !IsBadReadPtr(p, (UINT)pageLength);
				  p += pageLength, pageLength = si.dwPageSize)
	    for (i = 0; i < pageLength; ++i, ++length)
		if (p[i] == NUL)
		    return length + 1;
    }

    return 0;
}

/*
 * Passed to do_in_runtimepath() to load a vim.ico file.
 */
    static void
mch_icon_load_cb(char_u *fname, void *cookie)
{
    HANDLE *h = (HANDLE *)cookie;

    *h = LoadImage(NULL,
		   (LPSTR)fname,
		   IMAGE_ICON,
		   64,
		   64,
		   LR_LOADFROMFILE | LR_LOADMAP3DCOLORS);
}

/*
 * Try loading an icon file from 'runtimepath'.
 */
    int
mch_icon_load(HANDLE *iconp)
{
    return do_in_runtimepath((char_u *)"bitmaps/vim.ico",
						  0, mch_icon_load_cb, iconp);
}

    int
mch_libcall(
    char_u	*libname,
    char_u	*funcname,
    char_u	*argstring,	/* NULL when using a argint */
    int		argint,
    char_u	**string_result,/* NULL when using number_result */
    int		*number_result)
{
    HINSTANCE		hinstLib;
    MYSTRPROCSTR	ProcAdd;
    MYINTPROCSTR	ProcAddI;
    char_u		*retval_str = NULL;
    int			retval_int = 0;
    size_t		len;

    BOOL fRunTimeLinkSuccess = FALSE;

    // Get a handle to the DLL module.
    hinstLib = vimLoadLib((char *)libname);

    // If the handle is valid, try to get the function address.
    if (hinstLib != NULL)
    {
#ifdef HAVE_TRY_EXCEPT
	__try
	{
#endif
	if (argstring != NULL)
	{
	    /* Call with string argument */
	    ProcAdd = (MYSTRPROCSTR)GetProcAddress(hinstLib, (LPCSTR)funcname);
	    if ((fRunTimeLinkSuccess = (ProcAdd != NULL)) != 0)
	    {
		if (string_result == NULL)
		    retval_int = ((MYSTRPROCINT)ProcAdd)((LPSTR)argstring);
		else
		    retval_str = (char_u *)(ProcAdd)((LPSTR)argstring);
	    }
	}
	else
	{
	    /* Call with number argument */
	    ProcAddI = (MYINTPROCSTR) GetProcAddress(hinstLib, (LPCSTR)funcname);
	    if ((fRunTimeLinkSuccess = (ProcAddI != NULL)) != 0)
	    {
		if (string_result == NULL)
		    retval_int = ((MYINTPROCINT)ProcAddI)(argint);
		else
		    retval_str = (char_u *)(ProcAddI)(argint);
	    }
	}

	// Save the string before we free the library.
	// Assume that a "1" result is an illegal pointer.
	if (string_result == NULL)
	    *number_result = retval_int;
	else if (retval_str != NULL
		&& (len = check_str_len(retval_str)) > 0)
	{
	    *string_result = lalloc((long_u)len, TRUE);
	    if (*string_result != NULL)
		mch_memmove(*string_result, retval_str, len);
	}

#ifdef HAVE_TRY_EXCEPT
	}
	__except(EXCEPTION_EXECUTE_HANDLER)
	{
	    if (GetExceptionCode() == EXCEPTION_STACK_OVERFLOW)
		RESETSTKOFLW();
	    fRunTimeLinkSuccess = 0;
	}
#endif

	// Free the DLL module.
	(void)FreeLibrary(hinstLib);
    }

    if (!fRunTimeLinkSuccess)
    {
	EMSG2(_(e_libcall), funcname);
	return FAIL;
    }

    return OK;
}
#endif

/*
 * Debugging helper: expose the MCH_WRITE_DUMP stuff to other modules
 */
/*ARGSUSED*/
    void
DumpPutS(
    const char *psz)
{
# ifdef MCH_WRITE_DUMP
    if (fdDump)
    {
	fputs(psz, fdDump);
	if (psz[strlen(psz) - 1] != '\n')
	    fputc('\n', fdDump);
	fflush(fdDump);
    }
# endif
}

#ifdef _DEBUG

void __cdecl
Trace(
    char *pszFormat,
    ...)
{
    CHAR szBuff[2048];
    va_list args;

    va_start(args, pszFormat);
    vsprintf(szBuff, pszFormat, args);
    va_end(args);

    OutputDebugString(szBuff);
}

#endif //_DEBUG

#if !defined(FEAT_GUI) || defined(PROTO)
# if defined(FEAT_TITLE) && defined(WIN3264)
extern HWND g_hWnd;	/* This is in os_win32.c. */
# endif

/*
 * Showing the printer dialog is tricky since we have no GUI
 * window to parent it. The following routines are needed to
 * get the window parenting and Z-order to work properly.
 */
    static void
GetConsoleHwnd(void)
{
# define MY_BUFSIZE 1024 // Buffer size for console window titles.

    char pszNewWindowTitle[MY_BUFSIZE]; // Contains fabricated WindowTitle.
    char pszOldWindowTitle[MY_BUFSIZE]; // Contains original WindowTitle.

    /* Skip if it's already set. */
    if (s_hwnd != 0)
	return;

# if defined(FEAT_TITLE) && defined(WIN3264)
    /* Window handle may have been found by init code (Windows NT only) */
    if (g_hWnd != 0)
    {
	s_hwnd = g_hWnd;
	return;
    }
# endif

    GetConsoleTitle(pszOldWindowTitle, MY_BUFSIZE);

    wsprintf(pszNewWindowTitle, "%s/%d/%d",
	    pszOldWindowTitle,
	    GetTickCount(),
	    GetCurrentProcessId());
    SetConsoleTitle(pszNewWindowTitle);
    Sleep(40);
    s_hwnd = FindWindow(NULL, pszNewWindowTitle);

    SetConsoleTitle(pszOldWindowTitle);
}

/*
 * Console implementation of ":winpos".
 */
    int
mch_get_winpos(int *x, int *y)
{
    RECT  rect;

    GetConsoleHwnd();
    GetWindowRect(s_hwnd, &rect);
    *x = rect.left;
    *y = rect.top;
    return OK;
}

/*
 * Console implementation of ":winpos x y".
 */
    void
mch_set_winpos(int x, int y)
{
    GetConsoleHwnd();
    SetWindowPos(s_hwnd, NULL, x, y, 0, 0,
		 SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
}
#endif

#if (defined(FEAT_PRINTER) && !defined(FEAT_POSTSCRIPT)) || defined(PROTO)

/*=================================================================
 * Win32 printer stuff
 */

static HFONT		prt_font_handles[2][2][2];
static PRINTDLG		prt_dlg;
static const int	boldface[2] = {FW_REGULAR, FW_BOLD};
static TEXTMETRIC	prt_tm;
static int		prt_line_height;
static int		prt_number_width;
static int		prt_left_margin;
static int		prt_right_margin;
static int		prt_top_margin;
static char_u		szAppName[] = TEXT("VIM");
static HWND		hDlgPrint;
static int		*bUserAbort = NULL;
static char_u		*prt_name = NULL;

/* Defines which are also in vim.rc. */
#define IDC_BOX1		400
#define IDC_PRINTTEXT1		401
#define IDC_PRINTTEXT2		402
#define IDC_PROGRESS		403

#if !defined(FEAT_MBYTE)
# define vimSetDlgItemText(h, i, s) SetDlgItemText(h, i, s)
#else
    static BOOL
vimSetDlgItemText(HWND hDlg, int nIDDlgItem, char_u *s)
{
    WCHAR   *wp = NULL;
    BOOL    ret;

    if (enc_codepage >= 0 && (int)GetACP() != enc_codepage)
    {
	wp = enc_to_utf16(s, NULL);
    }
    if (wp != NULL)
    {
	ret = SetDlgItemTextW(hDlg, nIDDlgItem, wp);
	vim_free(wp);
	return ret;
    }
    return SetDlgItemText(hDlg, nIDDlgItem, (LPCSTR)s);
}
#endif

/*
 * Convert BGR to RGB for Windows GDI calls
 */
    static COLORREF
swap_me(COLORREF colorref)
{
    int  temp;
    char *ptr = (char *)&colorref;

    temp = *(ptr);
    *(ptr ) = *(ptr + 2);
    *(ptr + 2) = temp;
    return colorref;
}

/* Attempt to make this work for old and new compilers */
#if !defined(_WIN64) && (!defined(_MSC_VER) || _MSC_VER < 1300)
# define PDP_RETVAL BOOL
#else
# define PDP_RETVAL INT_PTR
#endif

/*ARGSUSED*/
    static PDP_RETVAL CALLBACK
PrintDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
#ifdef FEAT_GETTEXT
    NONCLIENTMETRICS nm;
    static HFONT hfont;
#endif

    switch (message)
    {
	case WM_INITDIALOG:
#ifdef FEAT_GETTEXT
	    nm.cbSize = sizeof(NONCLIENTMETRICS);
	    if (SystemParametersInfo(
			SPI_GETNONCLIENTMETRICS,
			sizeof(NONCLIENTMETRICS),
			&nm,
			0))
	    {
		char buff[MAX_PATH];
		int i;

		/* Translate the dialog texts */
		hfont = CreateFontIndirect(&nm.lfMessageFont);
		for (i = IDC_PRINTTEXT1; i <= IDC_PROGRESS; i++)
		{
		    SendDlgItemMessage(hDlg, i, WM_SETFONT, (WPARAM)hfont, 1);
		    if (GetDlgItemText(hDlg,i, buff, sizeof(buff)))
			vimSetDlgItemText(hDlg,i, (char_u *)_(buff));
		}
		SendDlgItemMessage(hDlg, IDCANCEL,
						WM_SETFONT, (WPARAM)hfont, 1);
		if (GetDlgItemText(hDlg,IDCANCEL, buff, sizeof(buff)))
		    vimSetDlgItemText(hDlg,IDCANCEL, (char_u *)_(buff));
	    }
#endif
	    SetWindowText(hDlg, (LPCSTR)szAppName);
	    if (prt_name != NULL)
	    {
		vimSetDlgItemText(hDlg, IDC_PRINTTEXT2, (char_u *)prt_name);
		vim_free(prt_name);
		prt_name = NULL;
	    }
	    EnableMenuItem(GetSystemMenu(hDlg, FALSE), SC_CLOSE, MF_GRAYED);
#ifndef FEAT_GUI
	    BringWindowToTop(s_hwnd);
#endif
	    return TRUE;

	case WM_COMMAND:
	    *bUserAbort = TRUE;
	    EnableWindow(GetParent(hDlg), TRUE);
	    DestroyWindow(hDlg);
	    hDlgPrint = NULL;
#ifdef FEAT_GETTEXT
	    DeleteObject(hfont);
#endif
	    return TRUE;
    }
    return FALSE;
}

/*ARGSUSED*/
    static BOOL CALLBACK
AbortProc(HDC hdcPrn, int iCode)
{
    MSG msg;

    while (!*bUserAbort && pPeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
    {
	if (!hDlgPrint || !pIsDialogMessage(hDlgPrint, &msg))
	{
	    TranslateMessage(&msg);
	    pDispatchMessage(&msg);
	}
    }
    return !*bUserAbort;
}

#ifndef FEAT_GUI

    static UINT_PTR CALLBACK
PrintHookProc(
	HWND hDlg,	// handle to dialog box
	UINT uiMsg,	// message identifier
	WPARAM wParam,	// message parameter
	LPARAM lParam	// message parameter
	)
{
    HWND	hwndOwner;
    RECT	rc, rcDlg, rcOwner;
    PRINTDLG	*pPD;

    if (uiMsg == WM_INITDIALOG)
    {
	// Get the owner window and dialog box rectangles.
	if ((hwndOwner = GetParent(hDlg)) == NULL)
	    hwndOwner = GetDesktopWindow();

	GetWindowRect(hwndOwner, &rcOwner);
	GetWindowRect(hDlg, &rcDlg);
	CopyRect(&rc, &rcOwner);

	// Offset the owner and dialog box rectangles so that
	// right and bottom values represent the width and
	// height, and then offset the owner again to discard
	// space taken up by the dialog box.

	OffsetRect(&rcDlg, -rcDlg.left, -rcDlg.top);
	OffsetRect(&rc, -rc.left, -rc.top);
	OffsetRect(&rc, -rcDlg.right, -rcDlg.bottom);

	// The new position is the sum of half the remaining
	// space and the owner's original position.

	SetWindowPos(hDlg,
		HWND_TOP,
		rcOwner.left + (rc.right / 2),
		rcOwner.top + (rc.bottom / 2),
		0, 0,		// ignores size arguments
		SWP_NOSIZE);

	/*  tackle the printdlg copiesctrl problem */
	pPD = (PRINTDLG *)lParam;
	pPD->nCopies = (WORD)pPD->lCustData;
	SetDlgItemInt( hDlg, edt3, pPD->nCopies, FALSE );
	/*  Bring the window to top */
	BringWindowToTop(GetParent(hDlg));
	SetForegroundWindow(hDlg);
    }

    return FALSE;
}
#endif

    void
mch_print_cleanup(void)
{
    int pifItalic;
    int pifBold;
    int pifUnderline;

    for (pifBold = 0; pifBold <= 1; pifBold++)
	for (pifItalic = 0; pifItalic <= 1; pifItalic++)
	    for (pifUnderline = 0; pifUnderline <= 1; pifUnderline++)
		DeleteObject(prt_font_handles[pifBold][pifItalic][pifUnderline]);

    if (prt_dlg.hDC != NULL)
	DeleteDC(prt_dlg.hDC);
    if (!*bUserAbort)
	SendMessage(hDlgPrint, WM_COMMAND, 0, 0);
}

    static int
to_device_units(int idx, int dpi, int physsize, int offset, int def_number)
{
    int		ret = 0;
    int		u;
    int		nr;

    u = prt_get_unit(idx);
    if (u == PRT_UNIT_NONE)
    {
	u = PRT_UNIT_PERC;
	nr = def_number;
    }
    else
	nr = printer_opts[idx].number;

    switch (u)
    {
	case PRT_UNIT_PERC:
	    ret = (physsize * nr) / 100;
	    break;
	case PRT_UNIT_INCH:
	    ret = (nr * dpi);
	    break;
	case PRT_UNIT_MM:
	    ret = (nr * 10 * dpi) / 254;
	    break;
	case PRT_UNIT_POINT:
	    ret = (nr * 10 * dpi) / 720;
	    break;
    }

    if (ret < offset)
	return 0;
    else
	return ret - offset;
}

    static int
prt_get_cpl(void)
{
    int		hr;
    int		phyw;
    int		dvoff;
    int		rev_offset;
    int		dpi;

    GetTextMetrics(prt_dlg.hDC, &prt_tm);
    prt_line_height = prt_tm.tmHeight + prt_tm.tmExternalLeading;

    hr	    = GetDeviceCaps(prt_dlg.hDC, HORZRES);
    phyw    = GetDeviceCaps(prt_dlg.hDC, PHYSICALWIDTH);
    dvoff   = GetDeviceCaps(prt_dlg.hDC, PHYSICALOFFSETX);
    dpi	    = GetDeviceCaps(prt_dlg.hDC, LOGPIXELSX);

    rev_offset = phyw - (dvoff + hr);

    prt_left_margin = to_device_units(OPT_PRINT_LEFT, dpi, phyw, dvoff, 10);
    if (prt_use_number())
    {
	prt_number_width = PRINT_NUMBER_WIDTH * prt_tm.tmAveCharWidth;
	prt_left_margin += prt_number_width;
    }
    else
	prt_number_width = 0;

    prt_right_margin = hr - to_device_units(OPT_PRINT_RIGHT, dpi, phyw,
							       rev_offset, 5);

    return (prt_right_margin - prt_left_margin) / prt_tm.tmAveCharWidth;
}

    static int
prt_get_lpp(void)
{
    int vr;
    int phyw;
    int dvoff;
    int rev_offset;
    int	bottom_margin;
    int	dpi;

    vr	    = GetDeviceCaps(prt_dlg.hDC, VERTRES);
    phyw    = GetDeviceCaps(prt_dlg.hDC, PHYSICALHEIGHT);
    dvoff   = GetDeviceCaps(prt_dlg.hDC, PHYSICALOFFSETY);
    dpi	    = GetDeviceCaps(prt_dlg.hDC, LOGPIXELSY);

    rev_offset = phyw - (dvoff + vr);

    prt_top_margin = to_device_units(OPT_PRINT_TOP, dpi, phyw, dvoff, 5);

    /* adjust top margin if there is a header */
    prt_top_margin += prt_line_height * prt_header_height();

    bottom_margin = vr - to_device_units(OPT_PRINT_BOT, dpi, phyw,
							       rev_offset, 5);

    return (bottom_margin - prt_top_margin) / prt_line_height;
}

    int
mch_print_init(prt_settings_T *psettings, char_u *jobname, int forceit)
{
    static HGLOBAL	stored_dm = NULL;
    static HGLOBAL	stored_devn = NULL;
    static int		stored_nCopies = 1;
    static int		stored_nFlags = 0;

    LOGFONT		fLogFont;
    int			pifItalic;
    int			pifBold;
    int			pifUnderline;

    DEVMODE		*mem;
    DEVNAMES		*devname;
    int			i;

    bUserAbort = &(psettings->user_abort);
    vim_memset(&prt_dlg, 0, sizeof(PRINTDLG));
    prt_dlg.lStructSize = sizeof(PRINTDLG);
#ifndef FEAT_GUI
    GetConsoleHwnd();	    /* get value of s_hwnd */
#endif
    prt_dlg.hwndOwner = s_hwnd;
    prt_dlg.Flags = PD_NOPAGENUMS | PD_NOSELECTION | PD_RETURNDC;
    if (!forceit)
    {
	prt_dlg.hDevMode = stored_dm;
	prt_dlg.hDevNames = stored_devn;
	prt_dlg.lCustData = stored_nCopies; // work around bug in print dialog
#ifndef FEAT_GUI
	/*
	 * Use hook to prevent console window being sent to back
	 */
	prt_dlg.lpfnPrintHook = PrintHookProc;
	prt_dlg.Flags |= PD_ENABLEPRINTHOOK;
#endif
	prt_dlg.Flags |= stored_nFlags;
    }

    /*
     * If bang present, return default printer setup with no dialog
     * never show dialog if we are running over telnet
     */
    if (forceit
#ifndef FEAT_GUI
	    || !term_console
#endif
	    )
    {
	prt_dlg.Flags |= PD_RETURNDEFAULT;
#ifdef WIN3264
	/*
	 * MSDN suggests setting the first parameter to WINSPOOL for
	 * NT, but NULL appears to work just as well.
	 */
	if (*p_pdev != NUL)
	    prt_dlg.hDC = CreateDC(NULL, (LPCSTR)p_pdev, NULL, NULL);
	else
#endif
	{
	    prt_dlg.Flags |= PD_RETURNDEFAULT;
	    if (PrintDlg(&prt_dlg) == 0)
		goto init_fail_dlg;
	}
    }
    else if (PrintDlg(&prt_dlg) == 0)
	goto init_fail_dlg;
    else
    {
	/*
	 * keep the previous driver context
	 */
	stored_dm = prt_dlg.hDevMode;
	stored_devn = prt_dlg.hDevNames;
	stored_nFlags = prt_dlg.Flags;
	stored_nCopies = prt_dlg.nCopies;
    }

    if (prt_dlg.hDC == NULL)
    {
	EMSG(_("E237: Printer selection failed"));
	mch_print_cleanup();
	return FALSE;
    }

    /* Not all printer drivers report the support of color (or grey) in the
     * same way.  Let's set has_color if there appears to be some way to print
     * more than B&W. */
    i = GetDeviceCaps(prt_dlg.hDC, NUMCOLORS);
    psettings->has_color = (GetDeviceCaps(prt_dlg.hDC, BITSPIXEL) > 1
				   || GetDeviceCaps(prt_dlg.hDC, PLANES) > 1
				   || i > 2 || i == -1);

    /* Ensure all font styles are baseline aligned */
    SetTextAlign(prt_dlg.hDC, TA_BASELINE|TA_LEFT);

    /*
     * On some windows systems the nCopies parameter is not
     * passed back correctly. It must be retrieved from the
     * hDevMode struct.
     */
    mem = (DEVMODE *)GlobalLock(prt_dlg.hDevMode);
    if (mem != NULL)
    {
#ifdef WIN3264
	if (mem->dmCopies != 1)
	    stored_nCopies = mem->dmCopies;
#endif
	if ((mem->dmFields & DM_DUPLEX) && (mem->dmDuplex & ~DMDUP_SIMPLEX))
	    psettings->duplex = TRUE;
	if ((mem->dmFields & DM_COLOR) && (mem->dmColor & DMCOLOR_COLOR))
	    psettings->has_color = TRUE;
    }
    GlobalUnlock(prt_dlg.hDevMode);

    devname = (DEVNAMES *)GlobalLock(prt_dlg.hDevNames);
    if (devname != 0)
    {
	char_u	*printer_name = (char_u *)devname + devname->wDeviceOffset;
	char_u	*port_name = (char_u *)devname +devname->wOutputOffset;
	char_u	*text = (char_u *)_("to %s on %s");
#ifdef FEAT_MBYTE
	char_u  *printer_name_orig = printer_name;
	char_u	*port_name_orig = port_name;

	if (enc_codepage >= 0 && (int)GetACP() != enc_codepage)
	{
	    char_u  *to_free = NULL;
	    int     maxlen;

	    acp_to_enc(printer_name, (int)STRLEN(printer_name), &to_free,
								    &maxlen);
	    if (to_free != NULL)
		printer_name = to_free;
	    acp_to_enc(port_name, (int)STRLEN(port_name), &to_free, &maxlen);
	    if (to_free != NULL)
		port_name = to_free;
	}
#endif
	prt_name = alloc((unsigned)(STRLEN(printer_name) + STRLEN(port_name)
							     + STRLEN(text)));
	if (prt_name != NULL)
	    wsprintf((char *)prt_name, (const char *)text,
		    printer_name, port_name);
#ifdef FEAT_MBYTE
	if (printer_name != printer_name_orig)
	    vim_free(printer_name);
	if (port_name != port_name_orig)
	    vim_free(port_name);
#endif
    }
    GlobalUnlock(prt_dlg.hDevNames);

    /*
     * Initialise the font according to 'printfont'
     */
    vim_memset(&fLogFont, 0, sizeof(fLogFont));
    if (get_logfont(&fLogFont, p_pfn, prt_dlg.hDC, TRUE) == FAIL)
    {
	EMSG2(_("E613: Unknown printer font: %s"), p_pfn);
	mch_print_cleanup();
	return FALSE;
    }

    for (pifBold = 0; pifBold <= 1; pifBold++)
	for (pifItalic = 0; pifItalic <= 1; pifItalic++)
	    for (pifUnderline = 0; pifUnderline <= 1; pifUnderline++)
	    {
		fLogFont.lfWeight =  boldface[pifBold];
		fLogFont.lfItalic = pifItalic;
		fLogFont.lfUnderline = pifUnderline;
		prt_font_handles[pifBold][pifItalic][pifUnderline]
					      = CreateFontIndirect(&fLogFont);
	    }

    SetBkMode(prt_dlg.hDC, OPAQUE);
    SelectObject(prt_dlg.hDC, prt_font_handles[0][0][0]);

    /*
     * Fill in the settings struct
     */
    psettings->chars_per_line = prt_get_cpl();
    psettings->lines_per_page = prt_get_lpp();
    if (prt_dlg.Flags & PD_USEDEVMODECOPIESANDCOLLATE)
    {
	psettings->n_collated_copies = (prt_dlg.Flags & PD_COLLATE)
						    ? prt_dlg.nCopies : 1;
	psettings->n_uncollated_copies = (prt_dlg.Flags & PD_COLLATE)
						    ? 1 : prt_dlg.nCopies;

	if (psettings->n_collated_copies == 0)
	    psettings->n_collated_copies = 1;

	if (psettings->n_uncollated_copies == 0)
	    psettings->n_uncollated_copies = 1;
    } else {
	psettings->n_collated_copies = 1;
	psettings->n_uncollated_copies = 1;
    }

    psettings->jobname = jobname;

    return TRUE;

init_fail_dlg:
    {
	DWORD err = CommDlgExtendedError();

	if (err)
	{
	    char_u *buf;

	    /* I suspect FormatMessage() doesn't work for values returned by
	     * CommDlgExtendedError().  What does? */
	    FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER |
			  FORMAT_MESSAGE_FROM_SYSTEM |
			  FORMAT_MESSAGE_IGNORE_INSERTS,
			  NULL, err, 0, (LPTSTR)(&buf), 0, NULL);
	    EMSG2(_("E238: Print error: %s"),
				  buf == NULL ? (char_u *)_("Unknown") : buf);
	    LocalFree((LPVOID)(buf));
	}
	else
	    msg_clr_eos(); /* Maybe canceled */

	mch_print_cleanup();
	return FALSE;
    }
}


    int
mch_print_begin(prt_settings_T *psettings)
{
    int			ret;
    static DOCINFO	di;
    char		szBuffer[300];

    hDlgPrint = CreateDialog(GetModuleHandle(NULL), TEXT("PrintDlgBox"),
					     prt_dlg.hwndOwner, PrintDlgProc);
    SetAbortProc(prt_dlg.hDC, AbortProc);
    wsprintf(szBuffer, _("Printing '%s'"), gettail(psettings->jobname));
    vimSetDlgItemText(hDlgPrint, IDC_PRINTTEXT1, (char_u *)szBuffer);

    vim_memset(&di, 0, sizeof(DOCINFO));
    di.cbSize = sizeof(DOCINFO);
    di.lpszDocName = (LPCSTR)psettings->jobname;
    ret = StartDoc(prt_dlg.hDC, &di);

#ifdef FEAT_GUI
    /* Give focus back to main window (when using MDI). */
    SetFocus(s_hwnd);
#endif

    return (ret > 0);
}

/*ARGSUSED*/
    void
mch_print_end(prt_settings_T *psettings)
{
    EndDoc(prt_dlg.hDC);
    if (!*bUserAbort)
	SendMessage(hDlgPrint, WM_COMMAND, 0, 0);
}

    int
mch_print_end_page(void)
{
    return (EndPage(prt_dlg.hDC) > 0);
}

    int
mch_print_begin_page(char_u *msg)
{
    if (msg != NULL)
	vimSetDlgItemText(hDlgPrint, IDC_PROGRESS, msg);
    return (StartPage(prt_dlg.hDC) > 0);
}

    int
mch_print_blank_page(void)
{
    return (mch_print_begin_page(NULL) ? (mch_print_end_page()) : FALSE);
}

static int prt_pos_x = 0;
static int prt_pos_y = 0;

    void
mch_print_start_line(int margin, int page_line)
{
    if (margin)
	prt_pos_x = -prt_number_width;
    else
	prt_pos_x = 0;
    prt_pos_y = page_line * prt_line_height
				 + prt_tm.tmAscent + prt_tm.tmExternalLeading;
}

    int
mch_print_text_out(char_u *p, int len)
{
#if defined(FEAT_PROPORTIONAL_FONTS) || defined(FEAT_MBYTE)
    SIZE	sz;
#endif
#if defined(FEAT_MBYTE)
    WCHAR	*wp = NULL;
    int		wlen = len;

    if (enc_codepage >= 0 && (int)GetACP() != enc_codepage)
    {
	wp = enc_to_utf16(p, &wlen);
    }
    if (wp != NULL)
    {
	int ret = FALSE;

	TextOutW(prt_dlg.hDC, prt_pos_x + prt_left_margin,
					 prt_pos_y + prt_top_margin, wp, wlen);
	GetTextExtentPoint32W(prt_dlg.hDC, wp, wlen, &sz);
	vim_free(wp);
	prt_pos_x += (sz.cx - prt_tm.tmOverhang);
	/* This is wrong when printing spaces for a TAB. */
	if (p[len] != NUL)
	{
	    wlen = MB_PTR2LEN(p + len);
	    wp = enc_to_utf16(p + len, &wlen);
	    if (wp != NULL)
	    {
		GetTextExtentPoint32W(prt_dlg.hDC, wp, 1, &sz);
		ret = (prt_pos_x + prt_left_margin + sz.cx > prt_right_margin);
		vim_free(wp);
	    }
	}
	return ret;
    }
#endif
    TextOut(prt_dlg.hDC, prt_pos_x + prt_left_margin,
					  prt_pos_y + prt_top_margin,
					  (LPCSTR)p, len);
#ifndef FEAT_PROPORTIONAL_FONTS
    prt_pos_x += len * prt_tm.tmAveCharWidth;
    return (prt_pos_x + prt_left_margin + prt_tm.tmAveCharWidth
				     + prt_tm.tmOverhang > prt_right_margin);
#else
    GetTextExtentPoint32(prt_dlg.hDC, (LPCSTR)p, len, &sz);
    prt_pos_x += (sz.cx - prt_tm.tmOverhang);
    /* This is wrong when printing spaces for a TAB. */
    if (p[len] == NUL)
	return FALSE;
    GetTextExtentPoint32(prt_dlg.hDC, p + len, 1, &sz);
    return (prt_pos_x + prt_left_margin + sz.cx > prt_right_margin);
#endif
}

    void
mch_print_set_font(int iBold, int iItalic, int iUnderline)
{
    SelectObject(prt_dlg.hDC, prt_font_handles[iBold][iItalic][iUnderline]);
}

    void
mch_print_set_bg(long_u bgcol)
{
    SetBkColor(prt_dlg.hDC, GetNearestColor(prt_dlg.hDC,
						   swap_me((COLORREF)bgcol)));
    /*
     * With a white background we can draw characters transparent, which is
     * good for italic characters that overlap to the next char cell.
     */
    if (bgcol == 0xffffffUL)
	SetBkMode(prt_dlg.hDC, TRANSPARENT);
    else
	SetBkMode(prt_dlg.hDC, OPAQUE);
}

    void
mch_print_set_fg(long_u fgcol)
{
    SetTextColor(prt_dlg.hDC, GetNearestColor(prt_dlg.hDC,
						   swap_me((COLORREF)fgcol)));
}

#endif /*FEAT_PRINTER && !FEAT_POSTSCRIPT*/



#if defined(FEAT_SHORTCUT) || defined(PROTO)
# ifndef PROTO
#  include <shlobj.h>
# endif

/*
 * When "fname" is the name of a shortcut (*.lnk) resolve the file it points
 * to and return that name in allocated memory.
 * Otherwise NULL is returned.
 */
    char_u *
mch_resolve_shortcut(char_u *fname)
{
    HRESULT		hr;
    IShellLink		*psl = NULL;
    IPersistFile	*ppf = NULL;
    OLECHAR		wsz[MAX_PATH];
    WIN32_FIND_DATA	ffd; // we get those free of charge
    CHAR		buf[MAX_PATH]; // could have simply reused 'wsz'...
    char_u		*rfname = NULL;
    int			len;
# ifdef FEAT_MBYTE
    IShellLinkW		*pslw = NULL;
    WIN32_FIND_DATAW	ffdw; // we get those free of charge
# endif

    /* Check if the file name ends in ".lnk". Avoid calling
     * CoCreateInstance(), it's quite slow. */
    if (fname == NULL)
	return rfname;
    len = (int)STRLEN(fname);
    if (len <= 4 || STRNICMP(fname + len - 4, ".lnk", 4) != 0)
	return rfname;

    CoInitialize(NULL);

# ifdef FEAT_MBYTE
    if (enc_codepage >= 0 && (int)GetACP() != enc_codepage)
    {
	// create a link manager object and request its interface
	hr = CoCreateInstance(
		&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
		&IID_IShellLinkW, (void**)&pslw);
	if (hr == S_OK)
	{
	    WCHAR	*p = enc_to_utf16(fname, NULL);

	    if (p != NULL)
	    {
		// Get a pointer to the IPersistFile interface.
		hr = pslw->lpVtbl->QueryInterface(
			pslw, &IID_IPersistFile, (void**)&ppf);
		if (hr != S_OK)
		    goto shortcut_errorw;

		// "load" the name and resolve the link
		hr = ppf->lpVtbl->Load(ppf, p, STGM_READ);
		if (hr != S_OK)
		    goto shortcut_errorw;
#  if 0  // This makes Vim wait a long time if the target does not exist.
		hr = pslw->lpVtbl->Resolve(pslw, NULL, SLR_NO_UI);
		if (hr != S_OK)
		    goto shortcut_errorw;
#  endif

		// Get the path to the link target.
		ZeroMemory(wsz, MAX_PATH * sizeof(WCHAR));
		hr = pslw->lpVtbl->GetPath(pslw, wsz, MAX_PATH, &ffdw, 0);
		if (hr == S_OK && wsz[0] != NUL)
		    rfname = utf16_to_enc(wsz, NULL);

shortcut_errorw:
		vim_free(p);
		goto shortcut_end;
	    }
	}
	/* Retry with non-wide function (for Windows 98). */
    }
# endif
    // create a link manager object and request its interface
    hr = CoCreateInstance(
	    &CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
	    &IID_IShellLink, (void**)&psl);
    if (hr != S_OK)
	goto shortcut_end;

    // Get a pointer to the IPersistFile interface.
    hr = psl->lpVtbl->QueryInterface(
	    psl, &IID_IPersistFile, (void**)&ppf);
    if (hr != S_OK)
	goto shortcut_end;

    // full path string must be in Unicode.
    MultiByteToWideChar(CP_ACP, 0, (LPCSTR)fname, -1, wsz, MAX_PATH);

    // "load" the name and resolve the link
    hr = ppf->lpVtbl->Load(ppf, wsz, STGM_READ);
    if (hr != S_OK)
	goto shortcut_end;
# if 0  // This makes Vim wait a long time if the target doesn't exist.
    hr = psl->lpVtbl->Resolve(psl, NULL, SLR_NO_UI);
    if (hr != S_OK)
	goto shortcut_end;
# endif

    // Get the path to the link target.
    ZeroMemory(buf, MAX_PATH);
    hr = psl->lpVtbl->GetPath(psl, buf, MAX_PATH, &ffd, 0);
    if (hr == S_OK && buf[0] != NUL)
	rfname = vim_strsave((char_u *)buf);

shortcut_end:
    // Release all interface pointers (both belong to the same object)
    if (ppf != NULL)
	ppf->lpVtbl->Release(ppf);
    if (psl != NULL)
	psl->lpVtbl->Release(psl);
# ifdef FEAT_MBYTE
    if (pslw != NULL)
	pslw->lpVtbl->Release(pslw);
# endif

    CoUninitialize();
    return rfname;
}
#endif

#if (defined(FEAT_EVAL) && !defined(FEAT_GUI)) || defined(PROTO)
/*
 * Bring ourselves to the foreground.  Does work if the OS doesn't allow it.
 */
    void
win32_set_foreground(void)
{
# ifndef FEAT_GUI
    GetConsoleHwnd();	    /* get value of s_hwnd */
# endif
    if (s_hwnd != 0)
	SetForegroundWindow(s_hwnd);
}
#endif

#if defined(FEAT_GUI) || (defined(FEAT_PRINTER) && !defined(FEAT_POSTSCRIPT)) \
	|| defined(PROTO)

struct charset_pair
{
    char	*name;
    BYTE	charset;
};

static struct charset_pair
charset_pairs[] =
{
    {"ANSI",		ANSI_CHARSET},
    {"CHINESEBIG5",	CHINESEBIG5_CHARSET},
    {"DEFAULT",		DEFAULT_CHARSET},
    {"HANGEUL",		HANGEUL_CHARSET},
    {"OEM",		OEM_CHARSET},
    {"SHIFTJIS",	SHIFTJIS_CHARSET},
    {"SYMBOL",		SYMBOL_CHARSET},
#ifdef WIN3264
    {"ARABIC",		ARABIC_CHARSET},
    {"BALTIC",		BALTIC_CHARSET},
    {"EASTEUROPE",	EASTEUROPE_CHARSET},
    {"GB2312",		GB2312_CHARSET},
    {"GREEK",		GREEK_CHARSET},
    {"HEBREW",		HEBREW_CHARSET},
    {"JOHAB",		JOHAB_CHARSET},
    {"MAC",		MAC_CHARSET},
    {"RUSSIAN",		RUSSIAN_CHARSET},
    {"THAI",		THAI_CHARSET},
    {"TURKISH",		TURKISH_CHARSET},
# if (!defined(_MSC_VER) || (_MSC_VER > 1010)) \
	&& (!defined(__BORLANDC__) || (__BORLANDC__ > 0x0500))
    {"VIETNAMESE",	VIETNAMESE_CHARSET},
# endif
#endif
    {NULL,		0}
};

struct quality_pair
{
    char	*name;
    DWORD	quality;
};

static struct quality_pair
quality_pairs[] = {
#ifdef CLEARTYPE_QUALITY
    {"CLEARTYPE",	CLEARTYPE_QUALITY},
#endif
#ifdef ANTIALIASED_QUALITY
    {"ANTIALIASED",	ANTIALIASED_QUALITY},
#endif
#ifdef NONANTIALIASED_QUALITY
    {"NONANTIALIASED",	NONANTIALIASED_QUALITY},
#endif
#ifdef PROOF_QUALITY
    {"PROOF",		PROOF_QUALITY},
#endif
#ifdef DRAFT_QUALITY
    {"DRAFT",		DRAFT_QUALITY},
#endif
    {"DEFAULT",		DEFAULT_QUALITY},
    {NULL,		0}
};

/*
 * Convert a charset ID to a name.
 * Return NULL when not recognized.
 */
    char *
charset_id2name(int id)
{
    struct charset_pair *cp;

    for (cp = charset_pairs; cp->name != NULL; ++cp)
	if ((BYTE)id == cp->charset)
	    break;
    return cp->name;
}

/*
 * Convert a quality ID to a name.
 * Return NULL when not recognized.
 */
    char *
quality_id2name(DWORD id)
{
    struct quality_pair *qp;

    for (qp = quality_pairs; qp->name != NULL; ++qp)
	if (id == qp->quality)
	    break;
    return qp->name;
}

static const LOGFONT s_lfDefault =
{
    -12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
    PROOF_QUALITY, FIXED_PITCH | FF_DONTCARE,
    "Fixedsys"	/* see _ReadVimIni */
};

/* Initialise the "current height" to -12 (same as s_lfDefault) just
 * in case the user specifies a font in "guifont" with no size before a font
 * with an explicit size has been set. This defaults the size to this value
 * (-12 equates to roughly 9pt).
 */
int current_font_height = -12;		/* also used in gui_w48.c */

/* Convert a string representing a point size into pixels. The string should
 * be a positive decimal number, with an optional decimal point (eg, "12", or
 * "10.5"). The pixel value is returned, and a pointer to the next unconverted
 * character is stored in *end. The flag "vertical" says whether this
 * calculation is for a vertical (height) size or a horizontal (width) one.
 */
    static int
points_to_pixels(char_u *str, char_u **end, int vertical, long_i pprinter_dc)
{
    int		pixels;
    int		points = 0;
    int		divisor = 0;
    HWND	hwnd = (HWND)0;
    HDC		hdc;
    HDC		printer_dc = (HDC)pprinter_dc;

    while (*str != NUL)
    {
	if (*str == '.' && divisor == 0)
	{
	    /* Start keeping a divisor, for later */
	    divisor = 1;
	}
	else
	{
	    if (!VIM_ISDIGIT(*str))
		break;

	    points *= 10;
	    points += *str - '0';
	    divisor *= 10;
	}
	++str;
    }

    if (divisor == 0)
	divisor = 1;

    if (printer_dc == NULL)
    {
	hwnd = GetDesktopWindow();
	hdc = GetWindowDC(hwnd);
    }
    else
	hdc = printer_dc;

    pixels = MulDiv(points,
		    GetDeviceCaps(hdc, vertical ? LOGPIXELSY : LOGPIXELSX),
		    72 * divisor);

    if (printer_dc == NULL)
	ReleaseDC(hwnd, hdc);

    *end = str;
    return pixels;
}

/*ARGSUSED*/
    static int CALLBACK
font_enumproc(
    ENUMLOGFONT	    *elf,
    NEWTEXTMETRIC   *ntm,
    int		    type,
    LPARAM	    lparam)
{
    /* Return value:
     *	  0 = terminate now (monospace & ANSI)
     *	  1 = continue, still no luck...
     *	  2 = continue, but we have an acceptable LOGFONT
     *	      (monospace, not ANSI)
     * We use these values, as EnumFontFamilies returns 1 if the
     * callback function is never called. So, we check the return as
     * 0 = perfect, 2 = OK, 1 = no good...
     * It's not pretty, but it works!
     */

    LOGFONT *lf = (LOGFONT *)(lparam);

#ifndef FEAT_PROPORTIONAL_FONTS
    /* Ignore non-monospace fonts without further ado */
    if ((ntm->tmPitchAndFamily & 1) != 0)
	return 1;
#endif

    /* Remember this LOGFONT as a "possible" */
    *lf = elf->elfLogFont;

    /* Terminate the scan as soon as we find an ANSI font */
    if (lf->lfCharSet == ANSI_CHARSET
	    || lf->lfCharSet == OEM_CHARSET
	    || lf->lfCharSet == DEFAULT_CHARSET)
	return 0;

    /* Continue the scan - we have a non-ANSI font */
    return 2;
}

    static int
init_logfont(LOGFONT *lf)
{
    int		n;
    HWND	hwnd = GetDesktopWindow();
    HDC		hdc = GetWindowDC(hwnd);

    n = EnumFontFamilies(hdc,
			 (LPCSTR)lf->lfFaceName,
			 (FONTENUMPROC)font_enumproc,
			 (LPARAM)lf);

    ReleaseDC(hwnd, hdc);

    /* If we couldn't find a usable font, return failure */
    if (n == 1)
	return FAIL;

    /* Tidy up the rest of the LOGFONT structure. We set to a basic
     * font - get_logfont() sets bold, italic, etc based on the user's
     * input.
     */
    lf->lfHeight = current_font_height;
    lf->lfWidth = 0;
    lf->lfItalic = FALSE;
    lf->lfUnderline = FALSE;
    lf->lfStrikeOut = FALSE;
    lf->lfWeight = FW_NORMAL;

    /* Return success */
    return OK;
}

/*
 * Get font info from "name" into logfont "lf".
 * Return OK for a valid name, FAIL otherwise.
 */
    int
get_logfont(
    LOGFONT	*lf,
    char_u	*name,
    HDC		printer_dc,
    int		verbose)
{
    char_u	*p;
    int		i;
    int		ret = FAIL;
    static LOGFONT *lastlf = NULL;
#ifdef FEAT_MBYTE
    char_u	*acpname = NULL;
#endif

    *lf = s_lfDefault;
    if (name == NULL)
	return OK;

#ifdef FEAT_MBYTE
    /* Convert 'name' from 'encoding' to the current codepage, because
     * lf->lfFaceName uses the current codepage.
     * TODO: Use Wide APIs instead of ANSI APIs. */
    if (enc_codepage >= 0 && (int)GetACP() != enc_codepage)
    {
	int	len;
	enc_to_acp(name, (int)STRLEN(name), &acpname, &len);
	name = acpname;
    }
#endif
    if (STRCMP(name, "*") == 0)
    {
#if defined(FEAT_GUI_W32)
	CHOOSEFONT	cf;
	/* if name is "*", bring up std font dialog: */
	vim_memset(&cf, 0, sizeof(cf));
	cf.lStructSize = sizeof(cf);
	cf.hwndOwner = s_hwnd;
	cf.Flags = CF_SCREENFONTS | CF_FIXEDPITCHONLY | CF_INITTOLOGFONTSTRUCT;
	if (lastlf != NULL)
	    *lf = *lastlf;
	cf.lpLogFont = lf;
	cf.nFontType = 0 ; //REGULAR_FONTTYPE;
	if (ChooseFont(&cf))
	    ret = OK;
#endif
	goto theend;
    }

    /*
     * Split name up, it could be <name>:h<height>:w<width> etc.
     */
    for (p = name; *p && *p != ':'; p++)
    {
	if (p - name + 1 > LF_FACESIZE)
	    goto theend;			/* Name too long */
	lf->lfFaceName[p - name] = *p;
    }
    if (p != name)
	lf->lfFaceName[p - name] = NUL;

    /* First set defaults */
    lf->lfHeight = -12;
    lf->lfWidth = 0;
    lf->lfWeight = FW_NORMAL;
    lf->lfItalic = FALSE;
    lf->lfUnderline = FALSE;
    lf->lfStrikeOut = FALSE;

    /*
     * If the font can't be found, try replacing '_' by ' '.
     */
    if (init_logfont(lf) == FAIL)
    {
	int	did_replace = FALSE;

	for (i = 0; lf->lfFaceName[i]; ++i)
	    if (lf->lfFaceName[i] == '_')
	    {
		lf->lfFaceName[i] = ' ';
		did_replace = TRUE;
	    }
	if (!did_replace || init_logfont(lf) == FAIL)
	    goto theend;
    }

    while (*p == ':')
	p++;

    /* Set the values found after ':' */
    while (*p)
    {
	switch (*p++)
	{
	    case 'h':
		lf->lfHeight = - points_to_pixels(p, &p, TRUE, (long_i)printer_dc);
		break;
	    case 'w':
		lf->lfWidth = points_to_pixels(p, &p, FALSE, (long_i)printer_dc);
		break;
	    case 'b':
		lf->lfWeight = FW_BOLD;
		break;
	    case 'i':
		lf->lfItalic = TRUE;
		break;
	    case 'u':
		lf->lfUnderline = TRUE;
		break;
	    case 's':
		lf->lfStrikeOut = TRUE;
		break;
	    case 'c':
		{
		    struct charset_pair *cp;

		    for (cp = charset_pairs; cp->name != NULL; ++cp)
			if (STRNCMP(p, cp->name, strlen(cp->name)) == 0)
			{
			    lf->lfCharSet = cp->charset;
			    p += strlen(cp->name);
			    break;
			}
		    if (cp->name == NULL && verbose)
		    {
			vim_snprintf((char *)IObuff, IOSIZE,
				_("E244: Illegal charset name \"%s\" in font name \"%s\""), p, name);
			EMSG(IObuff);
			break;
		    }
		    break;
		}
	    case 'q':
		{
		    struct quality_pair *qp;

		    for (qp = quality_pairs; qp->name != NULL; ++qp)
			if (STRNCMP(p, qp->name, strlen(qp->name)) == 0)
			{
			    lf->lfQuality = qp->quality;
			    p += strlen(qp->name);
			    break;
			}
		    if (qp->name == NULL && verbose)
		    {
			vim_snprintf((char *)IObuff, IOSIZE,
				_("E244: Illegal quality name \"%s\" in font name \"%s\""), p, name);
			EMSG(IObuff);
			break;
		    }
		    break;
		}
	    default:
		if (verbose)
		{
		    vim_snprintf((char *)IObuff, IOSIZE,
			    _("E245: Illegal char '%c' in font name \"%s\""),
			    p[-1], name);
		    EMSG(IObuff);
		}
		goto theend;
	}
	while (*p == ':')
	    p++;
    }
    ret = OK;

theend:
    /* ron: init lastlf */
    if (ret == OK && printer_dc == NULL)
    {
	vim_free(lastlf);
	lastlf = (LOGFONT *)alloc(sizeof(LOGFONT));
	if (lastlf != NULL)
	    mch_memmove(lastlf, lf, sizeof(LOGFONT));
    }
#ifdef FEAT_MBYTE
    vim_free(acpname);
#endif

    return ret;
}

#endif /* defined(FEAT_GUI) || defined(FEAT_PRINTER) */

#if defined(FEAT_JOB_CHANNEL) || defined(PROTO)
/*
 * Initialize the Winsock dll.
 */
    void
channel_init_winsock(void)
{
    WSADATA wsaData;
    int wsaerr;

    if (WSInitialized)
	return;

    wsaerr = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (wsaerr == 0)
	WSInitialized = TRUE;
}
#endif
