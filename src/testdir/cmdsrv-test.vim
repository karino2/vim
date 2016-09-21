#!vim -u
" Test for cmdsrv-nox.
" Run this script in src directory as ./vim -u testdir/cmdsrv-test.vim.
" This script works under GNOME desktop or Windows desktop.

if has('vim_starting')
  set nocompatible
  set loadplugins
  call feedkeys(":source " . expand('<sfile>') . "\<CR>")
  finish
endif

set nocompatible
set nomore

function! Sleep(n)
  execute 'sleep' a:n
endfunction

function! WaitResponse(serverid)
  if has('win32')
    " WORKAROUND: update event loop to receive response
    sleep 1
    call remote_expr(a:serverid, 'remote_expr("' . v:servername . '", "Sleep(1)")')
  else
    sleep 1
  endif
endfunction

function! s:serverlist()
  return split(serverlist(), '\n')
endfunction

function! s:cui_start_server(name)
  let nservers = len(s:serverlist())
  if has('win32')
    let cmd = printf('start .\vim.exe -u NORC --servername %s', shellescape(a:name, 1))
    silent! execute '!' . cmd
  else
    let cmd = printf("gnome-terminal -e \"./vim -u NORC --servername %s\"", escape(shellescape(a:name), '\"'))
    call system(cmd)
  endif
  if v:shell_error
    throw 'cui_start_server: failed to start server'
  endif
  " wait server ready
  for i in range(100)
    if nservers != len(s:serverlist())
      return
    endif
    sleep 100m
  endfor
  throw 'cui_start_server: failed to start server'
endfunction

function! s:gui_start_server(name)
  let nservers = len(s:serverlist())
  if has('win32')
    let cmd = printf('start .\gvim.exe -u NORC --servername %s', shellescape(a:name, 1))
    silent! execute '!' . cmd
  else
    let cmd = printf('./vim -g -u NORC --servername %s', shellescape(a:name))
    call system(cmd)
  endif
  if v:shell_error
    throw 'gui_start_server: failed to start server'
  endif
  " wait server ready
  for i in range(100)
    if nservers != len(s:serverlist())
      return
    endif
    sleep 100m
  endfor
  throw 'gui_start_server: failed to start server'
endfunction

let s:servermode = 'cui'

function! s:start_server(name)
  if s:servermode == 'cui'
    call s:cui_start_server(a:name)
  else
    call s:gui_start_server(a:name)
  endif
endfunction

function! s:close_server(name)
  let nservers = len(s:serverlist())
  call remote_send(a:name, '<C-\><C-N>:qall!<CR>')
  " wait server closed
  for i in range(100)
    if nservers != len(s:serverlist())
      return
    endif
    sleep 100m
  endfor
  throw 'close_server: failed to close server'
endfunction

function! s:close_all_servers()
  for name in s:serverlist()
    if name =~? '^\(ONE\|TWO\|THREE\)\d*$'
      call s:close_server(name)
    endif
  endfor
endfunction

function! s:test_loosename1()
  echomsg 'test_loosename1: Test for loosename'
  call s:start_server('one')
  call s:start_server('one')
  call s:start_server('one')
  let servers = s:serverlist()
  if index(servers, 'one', 0, 1) == -1 || index(servers, 'one1', 0, 1) == -1
        \ || index(servers, 'one2', 0, 1) == -1
    throw 'test_loosename1: failed'
  endif
  call s:close_all_servers()
endfunction

function! s:test_loosename2()
  echomsg 'test_loosename2: Test for loosename.'
  " one1 already used, second server will be named as one11.
  call s:start_server('one1')
  call s:start_server('one1')
  let servers = s:serverlist()
  if index(servers, 'one1', 0, 1) == -1 || index(servers, 'one11', 0, 1) == -1
    throw 'test_loosename2: failed'
  endif
  call s:close_all_servers()
endfunction

function! s:test_loosename3()
  echomsg 'test_loosename3: Test for many instances.'
  let n = 20
  for i in range(n)
    let name = printf('one%d', i)
    call s:start_server(name)
  endfor
  for i in range(n)
    let name = printf('one%d', i)
    if remote_expr(name, 'v:servername') !=? name
      throw 'test_loosename3: failed'
    endif
  endfor
  call s:close_all_servers()
endfunction

function! s:test_loosename4()
  echomsg 'test_loosename4: Test for special character which is not safe in file name'
  let servername = '/\*"[]:;|=?'
  call s:start_server(servername)
  if index(s:serverlist(), servername) == -1
    throw 'test_loosename4: failed'
  endif
  call s:close_server(servername)
endfunction

function! s:test_remote_send1()
  echomsg 'test_remote_send1: Test for remote_send().'
  call s:start_server('one')
  call remote_send('one', 'ihello<Esc>')
  call WaitResponse('one')
  let result = remote_expr('one', 'getline(1)')
  if result !=# 'hello'
    throw 'test_remote_send1: failed'
  endif
  call s:close_all_servers()
endfunction

function! s:test_remote_expr1()
  echomsg 'test_remote_expr1: Test for remote_expr().'
  call s:start_server('one')
  let result = remote_expr('one', 'v:servername')
  if result !=# 'ONE'
    throw 'test_remote_expr1: failed'
  endif
  call s:close_all_servers()
endfunction

function! s:test_remote_expr2()
  echomsg 'test_remote_expr2: Test for remote_expr().'
  call s:start_server('one')
  for i in range(100)
    let result = remote_expr('one', i)
    if result != i
      throw 'test_remote_expr2: failed'
    endif
  endfor
  call s:close_all_servers()
endfunction

let s:test_remote_expr3_count = 0

function! _test_remote_expr3_recursive(serverid, n)
  if a:n == 0
    return v:servername
  endif
  let s:test_remote_expr3_count += 1
  return v:servername . remote_expr(a:serverid, 'v:servername . remote_expr("' . v:servername . '", "_test_remote_expr3_recursive(\"" . v:servername . "\", ' . (a:n - 1) . ')")')
endfunction

function! s:test_remote_expr3()
  echomsg 'test_remote_expr3: Test for recursive call.'
  call s:start_server('one')
  let s:test_remote_expr3_count = 0
  let result = _test_remote_expr3_recursive('one', 3)
  let expect = join(repeat([v:servername], 4), 'ONE')
  if s:test_remote_expr3_count != 3 || result != expect
    throw 'test_remote_expr3: failed'
  endif
  call s:close_all_servers()
endfunction

function! s:test_reply1()
  echomsg 'test_reply1: Test for remote_peek().'
  call s:start_server('one')
  let cmd = '<C-\><C-N>:call server2client(expand("<client>"), "hello")<CR>'
  call remote_send('one', cmd, 'serverid')
  call WaitResponse('one')
  let available = remote_peek(serverid, 'result')
  if !available || result !=# 'hello'
    throw 'test_reply1: failed'
  endif
  " clear
  while remote_peek(serverid)
    call remote_read(serverid)
  endwhile
  call s:close_all_servers()
endfunction

function! s:test_reply2()
  echomsg 'test_reply2: Test for remote_read().'
  call s:start_server('one')
  let cmd = '<C-\><C-N>:call server2client(expand("<client>"), "hello")<CR>'
  call remote_send('one', cmd, 'serverid')
  call WaitResponse('one')
  let result = remote_read(serverid)
  if result !=# 'hello'
    throw 'test_reply2: failed'
  endif
  " clear
  while remote_peek(serverid)
    call remote_read(serverid)
  endwhile
  call s:close_all_servers()
endfunction

function! s:test_reply3()
  echomsg 'test_reply3: Test for remote_read() with remote_expr.'
  call s:start_server('one')
  let cmd = 'server2client(expand("<client>"), "hello")'
  call remote_expr('one', cmd, 'serverid')
  call WaitResponse('one')
  let result = remote_read(serverid)
  if result !=# 'hello'
    throw 'test_reply3: failed'
  endif
  " clear
  while remote_peek(serverid)
    call remote_read(serverid)
  endwhile
  call s:close_all_servers()
endfunction

function! s:test_reply4()
  echomsg 'test_reply4: Test for RemoteReply autocommand.'
  let s:test_reply4_result = 'deadbeaf'
  call s:start_server('one')
  let cmd = '<C-\><C-N>:call server2client(expand("<client>"), "hello")<CR>'
  call remote_send('one', cmd, 'serverid')
  augroup TestReply4
    au!
    execute printf("autocmd RemoteReply %s let s:test_reply4_result = expand('<afile>')", serverid)
  augroup END
  call WaitResponse('one')
  if s:test_reply4_result !=# 'hello'
    throw 'test_reply4: failed'
  endif
  " clear
  while remote_peek(serverid)
    call remote_read(serverid)
  endwhile
  call s:close_all_servers()
  augroup TestReply4
    au!
  augroup END
endfunction

function! s:test_reply5()
  echomsg 'test_reply5: Test for remote_read with sleep'
  call s:start_server('one')
  let cmd = '<C-\><C-N>:sleep 2 | call server2client(expand("<client>"), "hello")<CR>'
  call remote_send('one', cmd, 'serverid')
  let result = remote_read(serverid)
  if result !=# 'hello'
    throw 'test_reply5: failed'
  endif
  " clear
  while remote_peek(serverid)
    call remote_read(serverid)
  endwhile
  call s:close_all_servers()
endfunction

function! s:test_error1()
  echomsg 'test_error1: Test for non-existence serverid'
  let ok = 0
  try
    call remote_expr('nonexistence', '0')
  catch /^Vim\%((\a\+)\)\=:E241:/
    " E241: Unable to send to %s
    let ok = 1
  endtry
  if !ok
    throw 'test_error1: failed'
  endif
endfunction

function! s:test_error2()
  echomsg 'test_error2: Test for invalid expression'
  call s:start_server('one')
  let ok = 0
  try
    call remote_expr('one', 'invalid expression')
  catch /^Vim\%((\a\+)\)\=:E449:/
    " E449: Invalid expression received
    let ok = 1
  endtry
  if !ok
    throw 'test_error2: failed'
  endif
  call s:close_all_servers()
endfunction

function! s:test_error3()
  echomsg 'test_error3: Test for exception'
  call s:start_server('one')
  call remote_send('one', '<C-\><C-N>:function Raise()<CR>throw "Raise()"<CR>endfunction<CR><CR>')
  sleep 1
  let result = remote_expr('one', 'Raise()')
  if result !=# '0'
    throw 'test_error3: failed'
  endif
  call s:close_all_servers()
endfunction

function! s:test_error4()
  echomsg 'test_error4: Test for remote_read() with non-existence serverid'
  let ok = 0
  try
    call remote_read('0xffffffff')
  catch /^Vim\%((\a\+)\)\=:E277:/
    " E277: Unable to read a server reply
    let ok = 1
  endtry
  if !ok
    throw 'test_error4: failed'
  endif
endfunction

function! s:main()
  for servermode in ['cui', 'gui']
    let s:servermode = servermode
    call s:test_loosename1()
    call s:test_loosename2()
    call s:test_loosename3()
    call s:test_loosename4()
    call s:test_remote_send1()
    call s:test_remote_expr1()
    call s:test_remote_expr2()
    call s:test_remote_expr3()
    call s:test_reply1()
    call s:test_reply2()
    call s:test_reply3()
    call s:test_reply4()
    call s:test_reply5()
    call s:test_error1()
    call s:test_error2()
    call s:test_error3()
    call s:test_error4()
  endfor
  echomsg "all test done"
endfunction

try
  call s:main()
endtry

