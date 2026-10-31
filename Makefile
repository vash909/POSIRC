.POSIX:

CC = cc
CPPFLAGS = -Iinclude
CFLAGS = -O2
LDFLAGS =
LDLIBS =
CURSES_LIBS = -lncursesw
TLS_LIBS = -lssl -lcrypto

CORE_OBJS = src/app.o src/net.o src/irc.o src/buffer.o src/text.o
OBJS = src/main.o src/pipe.o src/ui.o src/config.o $(CORE_OBJS)

all: posirc

posirc: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(CURSES_LIBS) $(TLS_LIBS) $(LDLIBS)

src/pipe.o: src/pipe.c include/pipe.h include/net.h include/irc.h include/text.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c src/pipe.c -o $@

src/config.o: src/config.c include/config.h include/app.h include/irc.h include/text.h include/buffer.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c src/config.c -o $@

src/main.o: src/main.c include/config.h include/pipe.h include/buffer.h include/app.h include/irc.h include/ui.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c src/main.c -o $@

src/app.o: src/app.c include/buffer.h include/text.h include/app.h include/irc.h include/net.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c src/app.c -o $@

src/net.o: src/net.c include/net.h include/irc.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c src/net.c -o $@

src/irc.o: src/irc.c include/irc.h include/text.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c src/irc.c -o $@

src/ui.o: src/ui.c include/buffer.h include/text.h include/app.h include/irc.h include/net.h include/ui.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c src/ui.c -o $@

src/buffer.o: src/buffer.c include/buffer.h include/irc.h include/text.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c src/buffer.c -o $@

src/text.o: src/text.c include/text.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c src/text.c -o $@

tests/test_app: tests/test_app.c $(CORE_OBJS) include/app.h include/buffer.h include/irc.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_app.c $(CORE_OBJS) $(TLS_LIBS) $(LDLIBS)

tests/test_config: tests/test_config.c src/config.o $(CORE_OBJS) include/config.h include/app.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_config.c src/config.o $(CORE_OBJS) $(TLS_LIBS) $(LDLIBS)

tests/test_irc: tests/test_irc.c src/irc.o src/text.o include/irc.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_irc.c src/irc.o src/text.o $(LDLIBS)

tests/test_net: tests/test_net.c tests/tls_test_keys.h $(CORE_OBJS) include/app.h include/buffer.h include/net.h include/irc.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_net.c $(CORE_OBJS) $(TLS_LIBS) $(LDLIBS)

integration-test: posirc tests/test_net FORCE
	./tests/test_net

tests/test_text: tests/test_text.c src/text.o include/text.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_text.c src/text.o $(LDLIBS)

tests/test_buffers: tests/test_buffers.c $(CORE_OBJS) include/app.h include/buffer.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_buffers.c $(CORE_OBJS) $(TLS_LIBS) $(LDLIBS)

tests/test_ui: tests/test_ui.c src/ui.c $(CORE_OBJS) include/ui.h include/text.h include/buffer.h include/app.h include/net.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_ui.c $(CORE_OBJS) $(CURSES_LIBS) $(TLS_LIBS) $(LDLIBS)

ui-test: tests/test_ui FORCE
	./tests/test_ui

test: tests/test_app tests/test_config tests/test_irc tests/test_text tests/test_buffers FORCE
	./tests/test_app
	./tests/test_config
	./tests/test_irc
	./tests/test_text
	./tests/test_buffers

clean: FORCE
	rm -f posirc $(OBJS) tests/test_app tests/test_config tests/test_irc tests/test_net tests/test_text tests/test_buffers tests/test_ui

# POSIX make has no .PHONY; this target intentionally has no corresponding file.
FORCE:
