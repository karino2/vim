/* if_cmdsrv.c */
int cmdsrv_init __ARGS((void));
int cmdsrv_uninit __ARGS((void));
int cmdsrv_gui_register __ARGS((void));
int cmdsrv_gui_unregister __ARGS((void));
int cmdsrv_register_name __ARGS((char_u *name));
int cmdsrv_server_list __ARGS((char_u **result));
int cmdsrv_wait_request __ARGS((int timeoutmsec));
int cmdsrv_handle_requests __ARGS((void));
int cmdsrv_send_keys __ARGS((char_u *serverid, char_u *keys));
int cmdsrv_send_expr __ARGS((char_u *serverid, char_u *expr, char_u **result));
int cmdsrv_send_notification __ARGS((char_u *serverid, char_u *reply));
int cmdsrv_foreground __ARGS((char_u *serverid));
int cmdsrv_peek_reply __ARGS((char_u *serverid, char_u **pstr));
int cmdsrv_read_reply __ARGS((char_u *serverid, char_u **pstr));
int cmdsrv_wait_reply __ARGS((char_u *serverid, char_u **pstr));
char_u *cmdsrv_find_server __ARGS((char_u *name, int loose));
char_u *cmdsrv_urlencode(char_u *str);
char_u *cmdsrv_urldecode(char_u *str);
/* vim: set ft=c : */
