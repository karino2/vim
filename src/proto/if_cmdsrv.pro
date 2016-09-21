/* if_cmdsrv.c */
int cmdsrv_init(void);
int cmdsrv_uninit(void);
int cmdsrv_gui_register(void);
int cmdsrv_gui_unregister(void);
int cmdsrv_register_name(char_u *name);
int cmdsrv_server_list(char_u **result);
int cmdsrv_wait_request(int timeoutmsec);
int cmdsrv_handle_requests(void);
int cmdsrv_send_keys(char_u *serverid, char_u *keys);
int cmdsrv_send_expr(char_u *serverid, char_u *expr, char_u **result);
int cmdsrv_send_notification(char_u *serverid, char_u *reply);
int cmdsrv_foreground(char_u *serverid);
int cmdsrv_peek_reply(char_u *serverid, char_u **pstr);
int cmdsrv_read_reply(char_u *serverid, char_u **pstr);
int cmdsrv_wait_reply(char_u *serverid, char_u **pstr);
char_u *cmdsrv_find_server(char_u *name, int loose);
char_u *cmdsrv_urlencode(char_u *str);
char_u *cmdsrv_urldecode(char_u *str);
/* vim: set ft=c : */
