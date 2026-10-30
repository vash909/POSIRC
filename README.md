# POSIRC

POSIRC is a small terminal IRC client written in C99, using POSIX APIs, curses
and OpenSSL. It connects to one IRC server over verified TLS, supports channels
and private messages, and provides separate conversation buffers, channel user
lists, UTF-8 editing and IRC colors.

The priorities are portable source, predictable memory use, few dependencies
and readable code. Networking, IRC parsing, application state and terminal
rendering are separate modules.

## Requirements and build

- A C99 compiler, POSIX-compatible make and a shell.
- The system C library and a wide-character X/Open Curses implementation, such as ncursesw.
- OpenSSL headers and libraries exposing the 1.1.1 API or later. Use a maintained
  OpenSSL release; the tested version is 3.5.7.
- A terminal with a valid `TERM`, terminal descriptions and a UTF-8 locale.
- A system CA store, or an explicit PEM CA file supplied with `-c`.

On Debian/Ubuntu, the usual build packages are `build-essential`,
`libncurses-dev` and `libssl-dev`; on Arch Linux, `base-devel`, `ncurses`
and `openssl`. A `ca-certificates` package normally supplies public trust roots.
There is no dependency on CMake, Autotools, pkg-config, Python or an application
thread library. The client and test programs are C.

```sh
make
make test
make ui-test
make integration-test
./posirc
```

`make test` runs unit tests without a terminal or network access.
`make ui-test` checks the real curses cells using `xterm-256color` and `vt100`
terminal descriptions and temporary streams; it needs a UTF-8 locale but no
interactive terminal.
`make integration-test` creates temporary certificates and local TLS servers on
loopback using ephemeral ports. It needs permission to create sockets and fork
processes; it never contacts an external IRC server.

The default build links `-lncursesw -lssl -lcrypto`. On systems with different
library names or installation paths, override make variables:

```sh
make clean
make CC=cc CPPFLAGS='-Iinclude -I/opt/openssl/include' \
    LDFLAGS='-L/opt/openssl/lib' CURSES_LIBS='-lncursesw' \
    TLS_LIBS='-lssl -lcrypto'
```

Some systems expose wide curses through `-lcurses` instead. On macOS with
Homebrew, install `openssl@3` and `ncurses`, then supply their include/library
paths using `CPPFLAGS` and `LDFLAGS`.

Preserve `-Iinclude` when replacing `CPPFLAGS`. Some older Unix systems need
`LDLIBS=-lrt` for `clock_gettime`. The Makefile uses POSIX syntax. Clean before
changing compilers or flags, because make does not track variable values.
For GCC or Clang development builds:

```sh
make clean
make all test tests/test_net \
    CFLAGS='-std=c99 -O0 -g -Wall -Wextra -Wpedantic -Werror'
make integration-test
```

## Connect

Replace `irc.example.net` with your server:

```sh
./posirc -s irc.example.net -p 6697 -n yournick -j '#channel'
```

Or start `./posirc` and enter:

```text
/nick yournick
/connect irc.example.net
/join #channel
hello everyone
```

Wait for registration before `/join`. The target switches to the channel after
the server confirms your JOIN. Ordinary text goes to the target shown in the
status line. A local `[queued]` echo means the message entered the send queue;
it does not confirm delivery. Server errors remain visible in the transcript.

`-s` connects on startup; without it, POSIRC stays offline until `/connect`.
`-p` sets the startup port (default 6697); `/connect` takes its own optional port.
`-n` sets the nickname (default `posirc`), and `-j` joins one channel after each
successful registration. Quote channel names in shell commands.
`--help` and `--version` work without a terminal.

For a private CA:

```sh
./posirc -s irc.example.net -n yournick -c /path/to/trusted-ca.pem
```

The CA file replaces the default OpenSSL trust paths for every connection in
that process. The server must still present a valid certificate for the exact
hostname or IP address used to connect.

## Commands and interface

Commands are case-sensitive. There is one server connection, a server buffer,
and a separate history and input draft for every channel or query. Incoming
private messages create a query without stealing focus. An asterisk in the
conversation list indicates unread activity.

| Command | Action |
| --- | --- |
| `/connect host [port]` | Resolve and connect using verified TLS; default port 6697 |
| `/nick nickname` | Set the offline nickname or request a nickname change |
| `/join #channel [key]` | Join a channel, selecting it on server confirmation |
| `/part [#channel]` | Leave a channel; defaults to the current target |
| `/query nickname` | Select a private-message target |
| `/target name` | Select a channel or nickname; does not join a channel |
| `/msg target text` | Send to a target without changing the selected destination |
| `/disconnect` | Send QUIT and disconnect, keeping POSIRC open |
| `/quit` | Send QUIT and exit |
| `/buffer number\|name` | Switch to an existing buffer (server is number 0) |
| `/buffers` | List buffers and unread counts |
| `/users` | Print the current channel user list |
| `/close` | Close the current query or a channel already left |
| `/clear` | Clear only the current history |
| `/help` | Show commands and keys |

Channels appear after your JOIN is confirmed. PART or KICK marks a channel as
`[left]`, clears its users and disables ordinary sending there, while retaining
its history until `/close`. Reconnecting to the same host and port retains
histories; connecting to a different endpoint clears conversation buffers so
messages from different servers cannot be mixed. Server status has its own
buffer. Nickname collisions remain visible; use `/nick anothername` to retry.

The left sidebar lists channels and queries, and the right sidebar lists the
current channel's users with their highest known privilege prefix. User lists
follow NAMES, JOIN, PART, QUIT, NICK, KICK and MODE. PREFIX, CHANMODES,
CHANTYPES, STATUSMSG and CASEMAPPING are read from ISUPPORT. NAMES fragments are
merged with concurrent membership changes so stale names do not resurrect users
who have left. Topics appear above the conversation. PRIVMSG, NOTICE and CTCP
ACTION messages are routed to their channel/query.

Sidebars hide on narrow terminals (buffers below 70 columns, users below 100).
The commands `/buffers`, `/buffer` and `/users` remain available. Long transcript
lines are clipped, not wrapped. A short terminal shows a size notice; ncurses
resize handling and terminal restoration are tested.

| Key | Action |
| --- | --- |
| Tab / Shift-Tab, F3 / F2, Ctrl-N / Ctrl-P | Next / previous buffer |
| PageUp / PageDown | Scroll the current history |
| F7 / F8 | Scroll the channel user list |
| Left / Right, Home / End | Move within the current draft |
| Backspace / Delete | Delete the preceding / following Unicode scalar |
| Enter | Submit; return to live history |
| Ctrl-U | Clear the draft |
| Ctrl-L | Redraw |
| Ctrl-K | Insert IRC color control, followed by `04`, `04,02`, etc. |
| Ctrl-B / Ctrl-T / Ctrl-_ | Toggle bold / italic / underline |
| Ctrl-R / Ctrl-O | Toggle reverse / reset formatting |

Draft formatting controls are shown as caret notation; they are sent as IRC
control bytes. For example, Ctrl-K then `04hello` sends red text. Received colors
00-98, default color 99, foreground/background combinations and hexadecimal RGB
colors are interpreted. Colors map to the closest available terminal color;
monochrome terminals keep readable text and supported attributes. The renderer
uses at most 255 stable color pairs (or the terminal's lower limit); additional
combinations fall back to the default pair. Italic depends on terminal/curses
support. Monospace has no additional effect, and strikeout is ignored.

## Unicode

Set a UTF-8 locale, for example `LANG=C.UTF-8` on Linux or `LANG=en_US.UTF-8` on
macOS. Input is decoded incrementally, preserving characters even if UTF-8 bytes
arrive in separate reads over SSH. Rendering uses wide curses and `wcwidth()`;
CJK characters, individual emoji and combining accents are supported. Editing
and clipping never split a UTF-8 sequence. Names and messages remain UTF-8 on
the wire; server identifier rules and IRC byte limits still apply.

Backspace and cursor movement operate on Unicode scalars, not full grapheme
clusters. Complex joined emoji, flags, shaping and long combining sequences
depend on the terminal, libc and curses cell limits; no full Unicode grapheme
segmentation library is included. Non-ASCII identifiers are compared bytewise,
while ASCII uses the server's IRC case mapping; no Unicode normalization or
case folding is applied.

Invalid incoming UTF-8 becomes a replacement character. Unrecognized controls,
ANSI escapes and bidirectional override controls are sanitized; only recognized
IRC formatting is interpreted. Server text is never emitted as raw terminal
escapes. TLS certificate verification is unaffected by display formatting.

SIGINT, SIGTERM and SIGHUP request terminal cleanup and close the transport.
Use `/quit` for a graceful IRC QUIT; signal termination closes immediately.
SIGPIPE is ignored so a broken connection becomes a visible I/O error.

## TLS and transport

Every connection starts TLS immediately. TLS 1.2 is the minimum; newer versions
are negotiated when available. Certificate-chain, validity and hostname/IP
checks are mandatory. DNS names use SNI; IP literals are checked against IP
subject alternative names. Registration is queued only after verification.
There is no plaintext mode, STARTTLS fallback or certificate-verification bypass.

Sockets use `O_NONBLOCK`, and `poll()` waits for the transport and terminal.
OpenSSL retry directions are respected, including writes that need to read.
A queued write's buffer and length remain stable across retries. Received TLS
bytes pass through an incremental CRLF framer, so split or combined records do
not affect IRC parsing.

`getaddrinfo()` runs in a short-lived child process because POSIX provides no
portable asynchronous equivalent. Its result is read over a nonblocking pipe;
connection cancellation stops and reaps the child. IPv4 and IPv6 addresses are
tried in resolver order, up to 16 addresses. This is sequential fallback, not
parallel connection racing. DNS, TCP setup and the TLS handshake share a
30-second deadline, with at most 10 seconds per pending TCP connection. IRC
registration has a separate 60-second timeout.

Memory and work are bounded: 64 outgoing IRC frames, 64 buffer slots including
the server, 256 history lines of up to 1023 bytes per buffer, and 4096 user
records per channel. Histories and user arrays allocate storage only as needed;
closed buffers release it. User-list overflow is marked with `+`; buffer limits
are reported in the server buffer. Input drafts hold up to 1023 UTF-8 bytes.
There is a fixed network receive buffer. Work per network tick is limited so
server traffic and pasted input do not starve each other. Oversized outgoing
messages and full queues are reported, not truncated or silently dropped.
Incoming frames must fit the classic 512-byte IRC limit including CRLF;
malformed or oversized frames disconnect the session. PING receives an automatic
PONG even before registration completes.

`/quit` and `/disconnect` drain the send queue for up to two seconds, then close.
TLS close-notify is best effort, without waiting for the peer's reply. No
application data is silently retried after a lost connection.

## Source layout

| File | Responsibility |
| --- | --- |
| `src/main.c` | CLI options, terminal preconditions and signal handling |
| `src/net.c`, `include/net.h` | DNS worker, TCP/TLS state machine, bounded queue and poll |
| `src/irc.c`, `include/irc.h` | Incremental framing, message parsing and identifier checks |
| `src/app.c`, `include/app.h` | IRC registration/events, commands and conversation routing |
| `src/buffer.c`, `include/buffer.h` | Histories, drafts, unread counts and membership state |
| `src/text.c`, `include/text.h` | UTF-8 validation/editing and IRC formatting parser |
| `src/ui.c`, `include/ui.h` | Curses lifecycle, rendering, input and history view |
| `tests/test_irc.c`, `tests/test_app.c` | Protocol boundaries and application unit tests |
| `tests/test_buffers.c`, `tests/test_text.c` | Conversation/membership races, Unicode and color parsing |
| `tests/test_ui.c` | Wide curses cells, colors, combining marks and responsive layout |
| `tests/test_net.c` | Local TLS server, temporary CA/certificates and integration scenarios |
| `Makefile` | Portable build, tests and cleanup |

## Verification and current limits

Verified on Linux x86_64 with GCC 14.2.0, GNU make 4.4.1, ncurses 6.5 and
OpenSSL 3.5.7:

- C99 compilation with strict warnings treated as errors.
- Separate buffers, unread markers, drafts, user membership and privilege updates.
- UTF-8 boundaries, malformed sequences, color parsing, CJK/emoji and combining
  characters in actual curses cells, including monochrome fallback.
- UTF-8 terminal input split across reads and buffer navigation.
- IRC framing at every split position, 512-byte boundaries, malformed input,
  parameter parsing, case mapping, local commands and transcript wraparound.
- TLS 1.2/1.3, IRC registration, fragmented PING, JOIN, channel/private messages,
  NICK, PART and QUIT against a loopback server.
- Rejection of untrusted, expired and wrong-hostname/IP certificates.
- IP certificate verification, DNS cancellation, queue overflow/wraparound,
  CRLF injection rejection, malformed server frames and abrupt TLS disconnection.
- Pseudoterminal checks using `xterm` and `vt100`: input, resize and terminal
  restoration on exit and termination signals.
- Unit, integration and terminal checks with AddressSanitizer and
  UndefinedBehaviorSanitizer. Leak detection is disabled in this environment
  because LeakSanitizer cannot run under its ptrace supervision.

The source targets POSIX.1-2001 with X/Open Curses and OpenSSL. BSD, macOS,
other Unix platforms and other curses implementations have not been tested.
Only IPv4 loopback transport has been exercised by the current integration
suite; IPv6 address handling is implemented but still needs an IPv6 test host.

Not implemented yet: SASL, server passwords, IRCv3 capability negotiation,
extended tagged-message limits, automatic reconnect, persistent configuration,
scrollback storage on disk. Servers requiring SASL or a password cannot be used yet. The UI does
not split oversized messages automatically or implement outbound flood pacing.

Protocol and library references:
[IRC formatting](https://modern.ircdocs.horse/formatting.html),
[Modern IRC protocol](https://modern.ircdocs.horse/),
[RFC 2812](https://www.rfc-editor.org/rfc/rfc2812),
[OpenSSL identity verification](https://docs.openssl.org/3.5/man3/SSL_set1_host/),
[OpenSSL write retries](https://docs.openssl.org/3.5/man3/SSL_write/),
[OpenSSL reads](https://docs.openssl.org/3.5/man3/SSL_read/).
