/*
 * Copyright (c) 2026 the IOS-26-Hot raphael port authors. MIT License.
 *
 * bluetool: a stand-in for /usr/sbin/BlueTool on the K20 Pro.
 *
 * bluetoothd asks BlueTool over XPC (Mach service com.apple.BlueTool) to run its chip scripts ("boot",
 * "init", "deepsleep") before it uses the controller. The stock scripts are written for Broadcom chips and
 * fail on the WCN3998, so bluetoothd gives up ("Power on failed"). Here the controller is already up: btup
 * loads its firmware before bluetoothd starts. This program answers every request with result 0 and logs
 * what was asked (stderr: the job's StandardErrorPath), so later versions can answer more precisely.
 *
 * Facts about the protocol (kMsgId RunBuiltinScript / RunExternalScript / RunCommand, kMsgArgs, reply key
 * "result" = 0 for success) come from the public analysis in the beryllium port's notes; no code taken.
 *
 * The XPC functions are declared here: the iOS SDK marks the listener API unavailable for apps.
 */
#include <dispatch/dispatch.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef void *xpc_object_t;
typedef void *xpc_connection_t;
typedef const struct _xpc_type_s *xpc_type_t;
typedef void (^xpc_handler_t)(xpc_object_t);

extern const struct _xpc_type_s _xpc_type_connection, _xpc_type_dictionary, _xpc_type_error;
extern xpc_connection_t xpc_connection_create_mach_service(const char *name, dispatch_queue_t q, uint64_t flags);
extern void xpc_connection_set_event_handler(xpc_connection_t c, xpc_handler_t h);
extern void xpc_connection_resume(xpc_connection_t c);
extern void xpc_connection_send_message(xpc_connection_t c, xpc_object_t m);
extern xpc_type_t xpc_get_type(xpc_object_t o);
extern xpc_object_t xpc_dictionary_create_reply(xpc_object_t original);
extern void xpc_dictionary_set_int64(xpc_object_t d, const char *k, int64_t v);
extern void xpc_dictionary_set_string(xpc_object_t d, const char *k, const char *v);
extern char *xpc_copy_description(xpc_object_t o);
extern xpc_object_t xpc_retain(xpc_object_t o);
extern void xpc_release(xpc_object_t o);

#define LISTENER (1ull << 0)

static void
logf_(const char *fmt, const char *s)
{
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	fprintf(stderr, "%02d:%02d:%02d bluetool[%d]: ", tm.tm_hour, tm.tm_min, tm.tm_sec, getpid());
	fprintf(stderr, fmt, s);
	fputc('\n', stderr);
}

static void
handle(xpc_connection_t peer, xpc_object_t msg)
{
	if (xpc_get_type(msg) != &_xpc_type_dictionary) {
		if (xpc_get_type(msg) == &_xpc_type_error) {
			xpc_release(peer);      /* the client went away */
		}
		return;
	}
	char *d = xpc_copy_description(msg);
	logf_("request %s", d ? d : "?");
	free(d);
	xpc_object_t reply = xpc_dictionary_create_reply(msg);
	if (reply == NULL) {
		return;
	}
	xpc_dictionary_set_int64(reply, "result", 0);
	xpc_dictionary_set_string(reply, "output", "");
	xpc_connection_send_message(peer, reply);
	xpc_release(reply);
	logf_("answered result 0%s", "");
}

int
main(int argc, char **argv)
{
	setvbuf(stderr, NULL, _IOLBF, 0);
	if (argc > 1 && strcmp(argv[1], "-R") != 0) {
		/* run by hand ("BlueTool -c ...") : nothing to do, the kernel and btup own the chip */
		logf_("command line call (%s): nothing to do", argv[1]);
		return 0;
	}
	xpc_connection_t l = xpc_connection_create_mach_service("com.apple.BlueTool", dispatch_get_main_queue(), LISTENER);
	if (l == NULL) {
		logf_("cannot listen on %s", "com.apple.BlueTool");
		return 1;
	}
	xpc_connection_set_event_handler(l, ^(xpc_object_t peer) {
		if (xpc_get_type(peer) != &_xpc_type_connection) {
			char *d = xpc_copy_description(peer);
			logf_("listener event %s", d ? d : "?");
			free(d);
			return;
		}
		xpc_retain(peer);
		xpc_connection_set_event_handler(peer, ^(xpc_object_t m) {
			handle(peer, m);
		});
		xpc_connection_resume(peer);
	});
	xpc_connection_resume(l);
	logf_("listening on %s", "com.apple.BlueTool");
	dispatch_main();
}
