/* GStreamer unit tests for the curlhttpsrc element
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdlib.h>

#include <gio/gio.h>
#include <glib.h>
#include <glib/gprintf.h>

#include <gst/check/gstcheck.h>

gboolean redirect = TRUE;

static const char **cookies = NULL;

typedef struct _GioHttpServer
{
  guint16 port;
  char *root;
  GSocketService *service;
  guint64 delay;
} GioHttpServer;

typedef struct _HttpRequest
{
  gchar *method;
  gchar *version;
  gchar *path;
  gchar *query;
} HttpRequest;

static GioHttpServer *run_server (void);
static void stop_server (GioHttpServer * server);
static guint16 get_port_from_server (GioHttpServer * server);

static const gchar *STATUS_OK = "200 OK";
static const gchar *STATUS_MOVED_PERMANENTLY = "301 Moved Permanently";
static const gchar *STATUS_MOVED_TEMPORARILY = "302 Moved Temporarily";
static const gchar *STATUS_TEMPORARY_REDIRECT = "307 Temporary Redirect";
static const gchar *STATUS_FORBIDDEN = "403 Forbidden";
static const gchar *STATUS_NOT_FOUND = "404 Not Found";

static void
do_get (GioHttpServer * server, const HttpRequest * req, GOutputStream * out)
{
  gboolean send_error_doc = FALSE;
  int buflen = 1024;
  const gchar *status = STATUS_OK;
  const gchar *content_type = "application/octet-stream";
  GString *s;
  char *buf = NULL;
  gsize written = 0;

  GST_DEBUG ("%s request: \"%s\"", req->method, req->path);

  if (!strcmp (req->path, "/301"))
    status = STATUS_MOVED_PERMANENTLY;
  else if (!strcmp (req->path, "/302"))
    status = STATUS_MOVED_TEMPORARILY;
  else if (!strcmp (req->path, "/307"))
    status = STATUS_TEMPORARY_REDIRECT;
  else if (!strcmp (req->path, "/403"))
    status = STATUS_FORBIDDEN;
  else if (!strcmp (req->path, "/404"))
    status = STATUS_NOT_FOUND;
  else if (!strcmp (req->path, "/404-with-data")) {
    status = STATUS_NOT_FOUND;
    send_error_doc = TRUE;
  }
  s = g_string_new ("HTTP/");
  g_string_append_printf (s, "%s %s\r\n", req->version, status);

  if (g_str_has_prefix (status, "30")) {
    g_string_append_printf (s, "Location: %s-redirected\r\n", req->path);
  }

  if (status == STATUS_OK || send_error_doc) {
    g_string_append_printf (s, "Content-Type: %s\r\n", content_type);
    g_string_append_printf (s, "Content-Length: %lu\r\n", (gulong) buflen);
    if (!g_strcmp0 (req->method, "GET")) {
      buf = g_malloc (buflen);
      memset (buf, 0, buflen);
    }
  }

  g_string_append (s, "\r\n");
  GST_DEBUG ("Response headers: %lu\n%s\n********\n", s->len, s->str);
  g_output_stream_write_all (out, s->str, s->len, &written, NULL, NULL);
  fail_if (written != s->len);
  g_string_free (s, TRUE);
  if (buf) {
    g_output_stream_write_all (out, buf, buflen, &written, NULL, NULL);
    fail_if (written != buflen);
    g_free (buf);
  }
}

static void
send_error (GOutputStream * out, int error_code, const gchar * reason)
{
  gchar *res;

  res = g_strdup_printf ("HTTP/1.0 %d %s\r\n\r\n"
      "<html><head><title>%d %s</title></head>"
      "<body>%s</body></html>", error_code, reason, error_code, reason, reason);
  g_output_stream_write_all (out, res, strlen (res), NULL, NULL, NULL);
  g_free (res);
}

static gboolean
server_callback (GThreadedSocketService * service,
    GSocketConnection * connection,
    GSocketListener * listener, gpointer user_data)
{
  GioHttpServer *server = (GioHttpServer *) user_data;
  GOutputStream *out;
  GInputStream *in;
  GDataInputStream *data = NULL;
  char *line = NULL, *escaped, *tmp;
  HttpRequest req;

  in = g_io_stream_get_input_stream (G_IO_STREAM (connection));
  out = g_io_stream_get_output_stream (G_IO_STREAM (connection));

  data = g_data_input_stream_new (in);

  g_data_input_stream_set_newline_type (data, G_DATA_STREAM_NEWLINE_TYPE_ANY);

  line = g_data_input_stream_read_line (data, NULL, NULL, NULL);

  if (line == NULL) {
    send_error (out, 400, "Invalid request");
    goto out;
  }

  tmp = strchr (line, ' ');
  if (!tmp) {
    send_error (out, 400, "Invalid request");
    goto out;
  }
  req.method = line;
  *tmp = '\0';
  escaped = tmp + 1;

  req.version = NULL;
  tmp = strchr (escaped, ' ');
  if (tmp != NULL) {
    *tmp = 0;
    req.version = tmp + 6;      /* skip "HTTP/" from version field */
  }

  req.query = strchr (escaped, '?');
  if (req.query != NULL) {
    *req.query = '\0';
    req.query++;
  }

  req.path = g_uri_unescape_string (escaped, NULL);

  GST_DEBUG ("%s %s HTTP/%s", req.method, req.path, req.version);

  if (server->delay) {
    g_usleep (server->delay);
  }
  do_get (server, &req, out);

  g_free (req.path);
out:
  g_free (line);
  if (data)
    g_object_unref (data);

  return TRUE;
}

static guint16
get_port_from_server (GioHttpServer * server)
{
  fail_if (server == NULL);
  return server->port;
}

static GioHttpServer *
run_server (void)
{
  GioHttpServer *server;
  GError *error = NULL;

  server = g_slice_new0 (GioHttpServer);
  server->service = g_threaded_socket_service_new (10);
  server->port =
      g_socket_listener_add_any_inet_port (G_SOCKET_LISTENER (server->service),
      NULL, &error);
  fail_if (server->port == 0);
  g_signal_connect (server->service, "run", G_CALLBACK (server_callback),
      server);

  GST_DEBUG ("HTTP server listening on port %u", server->port);

  /* check if we can connect to our local http server */
  {
    GSocketConnection *conn;
    GSocketClient *client;

    client = g_socket_client_new ();
    g_socket_client_set_timeout (client, 2);
    conn =
        g_socket_client_connect_to_host (client, "127.0.0.1", server->port,
        NULL, NULL);
    if (conn == NULL) {
      GST_INFO ("Couldn't connect to 127.0.0.1:%u", server->port);
      g_object_unref (client);
      g_slice_free (GioHttpServer, server);
      return NULL;
    }

    g_object_unref (conn);
    g_object_unref (client);
  }

  return server;
}

static void
stop_server (GioHttpServer * server)
{
  fail_if (server == NULL);
  GST_DEBUG ("Stopping server...");
  g_socket_service_stop (server->service);
  g_socket_listener_close (G_SOCKET_LISTENER (server->service));
  g_object_unref (server->service);
  g_slice_free (GioHttpServer, server);
  GST_DEBUG ("Server stopped");
}

static void
handoff_cb (GstElement * fakesink, GstBuffer * buf, GstPad * pad,
    GstBuffer ** p_outbuf)
{
  GST_LOG ("handoff, buf = %p", buf);
  if (*p_outbuf == NULL)
    *p_outbuf = gst_buffer_ref (buf);
}

static gboolean
run_test (const gchar * path, gint expected_status_code,
    gboolean has_body, gboolean has_error)
{
  GstStateChangeReturn ret;
  GstElement *pipe, *src, *sink;
  GstBuffer *buf = NULL;
  GstMessage *msg;
  gchar *url;
  gboolean res = FALSE;
  GioHttpServer *server;
  guint port;
  gboolean done = FALSE;

  server = run_server ();
  fail_if (server == NULL, "Failed to start up HTTP server");

  pipe = gst_pipeline_new (NULL);
  fail_unless (pipe != NULL);

  src = gst_element_factory_make ("curlhttpsrc", NULL);
  fail_unless (src != NULL);

  sink = gst_element_factory_make ("fakesink", NULL);
  fail_unless (sink != NULL);

  gst_bin_add (GST_BIN (pipe), src);
  gst_bin_add (GST_BIN (pipe), sink);
  fail_unless (gst_element_link (src, sink));

  port = get_port_from_server (server);
  url = g_strdup_printf ("http://127.0.0.1:%u%s", port, path);
  fail_unless (url != NULL);
  g_object_set (src, "location", url, NULL);
  g_free (url);

  g_object_set (src, "automatic-redirect", redirect, NULL);
  if (cookies != NULL)
    g_object_set (src, "cookies", cookies, NULL);
  g_object_set (sink, "signal-handoffs", TRUE, NULL);
  /*g_object_set (sink, "dump", TRUE, NULL); */
  g_signal_connect (sink, "preroll-handoff", G_CALLBACK (handoff_cb), &buf);

  ret = gst_element_set_state (pipe, GST_STATE_PAUSED);
  if (ret != GST_STATE_CHANGE_ASYNC) {
    GST_DEBUG ("failed to start up curl http src, ret = %d", ret);
    goto done;
  }

  gst_element_set_state (pipe, GST_STATE_PLAYING);
  while (!done) {
    msg = gst_bus_poll (GST_ELEMENT_BUS (pipe),
        GST_MESSAGE_EOS | GST_MESSAGE_ERROR, -1);
    GST_DEBUG ("Message: %" GST_PTR_FORMAT, msg);
    switch (GST_MESSAGE_TYPE (msg)) {
      case GST_MESSAGE_ERROR:{
        gchar *debug = NULL;
        GError *err = NULL;
        gint rc = -1;
        const GstStructure *details = NULL;

        fail_unless (has_error);
        gst_message_parse_error (msg, &err, &debug);
        gst_message_parse_error_details (msg, &details);
        GST_DEBUG ("debug object: %s", debug);
        GST_DEBUG ("err->message: \"%s\"", err->message);
        GST_DEBUG ("err->details: %" GST_PTR_FORMAT, details);
        if (g_str_has_suffix (err->message, "Not Found"))
          rc = 404;
        else if (g_str_has_suffix (err->message, "Forbidden"))
          rc = 403;
        else if (g_str_has_suffix (err->message, "Unauthorized"))
          rc = 401;
        else if (g_str_has_suffix (err->message, "Found"))
          rc = 302;
        if (details) {
          if (gst_structure_has_field_typed (details, "http-status-code",
                  G_TYPE_UINT)) {
            guint code = 0;
            gst_structure_get_uint (details, "http-status-code", &code);
            rc = code;
          }
        }
        g_error_free (err);
        g_free (debug);
        GST_DEBUG ("Got HTTP error %d, expected_status_code %d", rc,
            expected_status_code);
        res = (rc == expected_status_code);
        done = TRUE;
      }
        break;
      case GST_MESSAGE_EOS:
        if (!has_error)
          done = TRUE;
        break;
      default:
        fail_if (TRUE, "Unexpected GstMessage");
        break;
    }
    gst_message_unref (msg);
  }

  /* don't wait for more than 10 seconds */
  ret = gst_element_get_state (pipe, NULL, NULL, 10 * GST_SECOND);
  GST_LOG ("ret = %u", ret);

  if (buf != NULL) {
    fail_unless (has_body);
    /* we want to test the buffer offset, nothing else; if there's a failure
     * it might be for lots of reasons (no network connection, whatever), we're
     * not interested in those */
    GST_DEBUG ("buffer offset = %" G_GUINT64_FORMAT, GST_BUFFER_OFFSET (buf));

    /* first buffer should have a 0 offset */
    fail_unless (GST_BUFFER_OFFSET (buf) == 0);
    gst_buffer_unref (buf);
  }
  res = TRUE;

done:

  gst_element_set_state (pipe, GST_STATE_NULL);
  gst_object_unref (pipe);
  stop_server (server);
  return res;
}

GST_START_TEST (test_first_buffer_has_offset)
{
  fail_unless (run_test ("/", 200, TRUE, FALSE));
}

GST_END_TEST;

GST_START_TEST (test_not_found)
{
  fail_unless (run_test ("/404", 404, FALSE, TRUE));
}

GST_END_TEST;

GST_START_TEST (test_not_found_with_data)
{
  fail_unless (run_test ("/404-with-data", 404, TRUE, TRUE));
}

GST_END_TEST;

GST_START_TEST (test_forbidden)
{
  fail_unless (run_test ("/403", 403, FALSE, TRUE));
}

GST_END_TEST;

GST_START_TEST (test_redirect_no)
{
  redirect = FALSE;
  fail_unless (run_test ("/302", 302, FALSE, FALSE));
}

GST_END_TEST;

GST_START_TEST (test_redirect_yes)
{
  redirect = TRUE;
  fail_unless (run_test ("/302", 200, TRUE, FALSE));
}

GST_END_TEST;

GST_START_TEST (test_cookies)
{
  static const char *biscotti[] = { "delacre=yummie", "koekje=lu", NULL };
  gboolean res;

  cookies = biscotti;
  res = run_test ("/", 200, TRUE, FALSE);
  cookies = NULL;
  fail_unless (res);
}

GST_END_TEST;

typedef struct _HttpSrcTestDownloader
{
  GstElement *bin;
  GstElement *src;
  GstElement *sink;
  GioHttpServer *server;
  guint count;
} HttpSrcTestDownloader;

static gboolean
move_element_to_ready (gpointer user_data)
{
  HttpSrcTestDownloader *tp = (HttpSrcTestDownloader *) user_data;

  GST_TRACE_OBJECT (tp->bin, "Move bin to READY state");
  gst_element_set_state (tp->bin, GST_STATE_READY);
  return G_SOURCE_REMOVE;
}

static GstPadProbeReturn
src_event_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  HttpSrcTestDownloader *tp = (HttpSrcTestDownloader *) user_data;
  GstEvent *event;

  event = gst_pad_probe_info_get_event (info);

  if (GST_EVENT_TYPE (event) == GST_EVENT_EOS) {
    GST_DEBUG_OBJECT (tp->bin, "finished last request");
    g_idle_add (move_element_to_ready, tp);
  }
  return GST_PAD_PROBE_OK;
}

static void
start_next_download (HttpSrcTestDownloader * tp)
{
  gchar *url;

  url = g_strdup_printf ("http://127.0.0.1:%u/multi/%s-%u",
      tp->server->port, GST_ELEMENT_NAME (tp->bin), tp->count);
  fail_unless (url != NULL);
  GST_DEBUG_OBJECT (tp->bin, "Start next request for: %s", url);
  g_object_set (tp->src, "location", url, NULL);
  g_free (url);
  fail_unless (gst_element_sync_state_with_parent (tp->bin));
}

static HttpSrcTestDownloader *
test_curl_http_src_create_downloader (const gchar * name, guint64 delay)
{
  HttpSrcTestDownloader *tp;
  gchar *url;
  GstPad *src_pad;

  tp = g_slice_new0 (HttpSrcTestDownloader);
  tp->server = run_server ();
  fail_if (tp->server == NULL, "Failed to start up HTTP server");
  tp->server->delay = delay;

  tp->src = gst_element_factory_make ("curlhttpsrc", NULL);
  fail_unless (tp->src != NULL);

  url = g_strdup_printf ("http://127.0.0.1:%u/multi/%s-0", tp->server->port,
      name);
  fail_unless (url != NULL);
  g_object_set (tp->src, "location", url, NULL);
  g_free (url);

  src_pad = gst_element_get_static_pad (tp->src, "src");
  fail_unless (src_pad != NULL);
  gst_pad_add_probe (src_pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
      src_event_probe, tp, NULL);
  gst_object_unref (src_pad);

  tp->sink = gst_element_factory_make ("fakesink", NULL);
  fail_unless (tp->sink != NULL);

  tp->bin = gst_bin_new (name);
  fail_unless (tp->bin != NULL);

  gst_bin_add (GST_BIN (tp->bin), tp->src);
  gst_bin_add (GST_BIN (tp->bin), tp->sink);
  fail_unless (gst_element_link (tp->src, tp->sink));
  gst_element_set_locked_state (GST_ELEMENT (tp->bin), TRUE);

  return tp;
}

typedef struct _MultipleHttpRequestsContext
{
  GMainLoop *loop;
  GstElement *pipe;
  HttpSrcTestDownloader *downloader1;
  HttpSrcTestDownloader *downloader2;
  gboolean failed;
} MultipleHttpRequestsContext;

static gboolean
bus_message (GstBus * bus, GstMessage * msg, gpointer user_data)
{
  MultipleHttpRequestsContext *context =
      (MultipleHttpRequestsContext *) user_data;
  gchar *debug;
  GError *err;
  GstState newstate;
  GstState pending;
  const GstStructure *details;

  GST_TRACE ("Message: %" GST_PTR_FORMAT, msg);
  switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_STATE_CHANGED:
      gst_message_parse_state_changed (msg, NULL, &newstate, &pending);
      if (newstate == GST_STATE_PLAYING && pending == GST_STATE_VOID_PENDING &&
          GST_MESSAGE_SRC (msg) == GST_OBJECT (context->pipe)) {
        GST_DEBUG ("Test ready to start");
        start_next_download (context->downloader1);
        start_next_download (context->downloader2);
      } else if (newstate == GST_STATE_READY
          && pending == GST_STATE_VOID_PENDING) {
        if (GST_MESSAGE_SRC (msg) == GST_OBJECT (context->downloader1->bin)) {
          if (++context->downloader1->count < 20) {
            start_next_download (context->downloader1);
          } else {
            gst_element_set_state (context->downloader1->bin, GST_STATE_NULL);
            if (context->downloader2->count == 20) {
              g_main_loop_quit (context->loop);
            }
          }
        } else if (GST_MESSAGE_SRC (msg) ==
            GST_OBJECT (context->downloader2->bin)) {
          if (++context->downloader2->count < 20) {
            start_next_download (context->downloader2);
          } else {
            gst_element_set_state (context->downloader2->bin, GST_STATE_NULL);
            if (context->downloader1->count == 20) {
              g_main_loop_quit (context->loop);
            }
          }
        }
      }
      break;
    case GST_MESSAGE_ERROR:
      debug = NULL;
      err = NULL;
      details = NULL;
      gst_message_parse_error (msg, &err, &debug);
      gst_message_parse_error_details (msg, &details);
      GST_DEBUG ("err->debug: %s", debug);
      GST_DEBUG ("err->message: \"%s\"", err->message);
      GST_DEBUG ("err->details: %" GST_PTR_FORMAT, details);
      g_error_free (err);
      g_free (debug);
      context->failed = TRUE;
      g_main_loop_quit (context->loop);
      break;
    case GST_MESSAGE_EOS:
      if (context->downloader1->count == 20
          && context->downloader2->count == 20) {
        g_main_loop_quit (context->loop);
      }
      break;
    default:
      break;
  }
  return TRUE;
}

/* test_multiple_http_requests tries to reproduce the way in which
 * GstAdaptiveDemux makes use of URI source elements. GstAdaptiveDemux
 * creates a bin with the httpsrc element and a queue element and sets the
 * locked state of that bin to TRUE, so that it does not follow the state
 * transitions of its parent. It then moves this bin to the PLAYING state
 * to start each download and back to READY when the download completes.
 */
GST_START_TEST (test_multiple_http_requests)
{
  GstStateChangeReturn ret;
  MultipleHttpRequestsContext context;
  guint watch_id;
  GstBus *bus;

  context.loop = g_main_loop_new (NULL, FALSE);
  context.downloader1 =
      test_curl_http_src_create_downloader ("bin1", 5 * G_USEC_PER_SEC / 1000);
  fail_unless (context.downloader1 != NULL);
  context.downloader2 =
      test_curl_http_src_create_downloader ("bin2", 7 * G_USEC_PER_SEC / 1000);
  fail_unless (context.downloader2 != NULL);

  context.pipe = gst_pipeline_new (NULL);
  fail_unless (context.pipe != NULL);

  gst_bin_add (GST_BIN_CAST (context.pipe), context.downloader1->bin);
  gst_bin_add (GST_BIN_CAST (context.pipe), context.downloader2->bin);

  bus = gst_pipeline_get_bus (GST_PIPELINE (context.pipe));
  watch_id = gst_bus_add_watch (bus, bus_message, &context);
  gst_object_unref (bus);

  GST_DEBUG ("Start pipeline playing");
  ret = gst_element_set_state (context.pipe, GST_STATE_PLAYING);
  fail_unless (ret == GST_STATE_CHANGE_ASYNC
      || ret == GST_STATE_CHANGE_SUCCESS);

  g_main_loop_run (context.loop);
  g_source_remove (watch_id);
  gst_element_set_state (context.pipe, GST_STATE_NULL);
  gst_object_unref (context.pipe);
  stop_server (context.downloader1->server);
  stop_server (context.downloader2->server);
  g_slice_free (HttpSrcTestDownloader, context.downloader1);
  g_slice_free (HttpSrcTestDownloader, context.downloader2);
  g_main_loop_unref (context.loop);
}

GST_END_TEST;

static Suite *
curlhttpsrc_suite (void)
{
  TCase *tc_chain;
  Suite *s;

  /* we don't support exceptions from the proxy, so just unset the environment
   * variable - in case it's set in the test environment it would otherwise
   * prevent us from connecting to localhost (like jenkins.qa.ubuntu.com) */
  g_unsetenv ("http_proxy");

  s = suite_create ("curlhttpsrc");
  tc_chain = tcase_create ("general");

  suite_add_tcase (s, tc_chain);
  tcase_add_test (tc_chain, test_first_buffer_has_offset);
  tcase_add_test (tc_chain, test_redirect_yes);
  tcase_add_test (tc_chain, test_redirect_no);
  tcase_add_test (tc_chain, test_not_found);
  tcase_add_test (tc_chain, test_not_found_with_data);
  tcase_add_test (tc_chain, test_forbidden);
  tcase_add_test (tc_chain, test_cookies);
  tcase_add_test (tc_chain, test_multiple_http_requests);

  return s;
}

GST_CHECK_MAIN (curlhttpsrc);
