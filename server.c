/* GStreamer
 *
 * Copyright (C) 2016 William Manley <will@williammanley.net>
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

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <gst/gst.h>

static gboolean bus_call (GstBus *bus, GstMessage *msg, gpointer data);
static gchar* get_current_mode (void);

int main(int argc, char* argv[])
{
  GMainLoop *loop;
  GstBus *bus;
  GstElement * epipeline;
  GstElement * mmalvideosink;
  GstPipeline * pipeline;
  GError * err = NULL;
  gchar * sink_pipeline, *pipeline_description;
  struct timespec ts;
  int res;
  GstClock *clock;

  gst_init(&argc, &argv);

  loop = g_main_loop_new (NULL, FALSE);

  if (argc > 1)
    sink_pipeline = argv[1];
  else
    sink_pipeline = "videoconvert ! mmalvideosink name=mmalsink";

  pipeline_description = g_strdup_printf (
      "videotestsrc is-live=true pattern=white "
      "! %s "
      "! timestampoverlay "
      "! queue "
      "! %s", get_current_mode(), sink_pipeline);
  g_printerr ("Using pipeline %s\n", pipeline_description);
  epipeline = gst_parse_launch (pipeline_description, &err);

  if (err) {
    fprintf(stderr, "Error creating pipeline: %s\n", err->message);
    return 1;
  }
  g_return_val_if_fail (epipeline != NULL, 1);
  pipeline = GST_PIPELINE(epipeline);

  mmalvideosink = gst_bin_get_by_name (GST_BIN(pipeline), "mmalsink");
  if (mmalvideosink) {
    /* By default mmalvideosink scales the video to fullscreen.  We want it
     * original resolution but centered */
    GstStructure * destinationwindow = gst_structure_new ("destinationwindow",
        "x", G_TYPE_DOUBLE, (double) (1280. - 640.) / 2. / 1280.,
        "y", G_TYPE_DOUBLE, (double) (720. - 240.) / 2. / 720.,
        "width", G_TYPE_DOUBLE, (double) 640./1280.,
        "height", G_TYPE_DOUBLE, (double) 240./720.,
        NULL);
    g_object_set (mmalvideosink, "destinationwindow", destinationwindow, NULL);
    gst_object_unref (mmalvideosink);
    mmalvideosink = NULL;
  }

  /* we add a message handler */
  bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
  gst_bus_add_watch (bus, bus_call, loop);
  gst_object_unref (bus);

  gst_pipeline_set_latency (pipeline, 100 * GST_MSECOND);

  gst_element_set_state(epipeline, GST_STATE_READY);

  res = clock_gettime(CLOCK_REALTIME, &ts);
  g_return_val_if_fail (res == 0, 1);

  gst_element_set_state(epipeline, GST_STATE_PLAYING);

  clock = gst_pipeline_get_clock (pipeline);
  GST_INFO("Pipeline clock is %" GST_PTR_FORMAT, clock);
  g_clear_object (&clock);

  g_main_loop_run (loop);

  return 0;
}


static gboolean
bus_call (GstBus *bus, GstMessage *msg, gpointer data)
{
  GMainLoop *loop = (GMainLoop *) data;

  switch (GST_MESSAGE_TYPE (msg)) {

    case GST_MESSAGE_EOS:
      g_print ("End of stream\n");
      g_main_loop_quit (loop);
      break;

    case GST_MESSAGE_ERROR: {
      gchar  *debug;
      GError *error;

      gst_message_parse_error (msg, &error, &debug);
      g_free (debug);

      g_printerr ("Error: %s\n", error->message);
      g_error_free (error);

      exit (1);
      break;
    }
    default:
      break;
  }

  return TRUE;
}

struct frac {
    int n, d;
};

struct frac fps_to_frac(double fps) {
  struct frac out;
  if (fabs(fps * 1001. / 1000. - round(fps)) > fabs(round(fps) - fps))
    out.d = 1;
  else
    out.d = 1001;

  out.n = round(fps * out.d);
  return out;
}

static gchar*
get_current_mode (void)
{
  gchar *tv_stderr = NULL, *tv_stdout = NULL;
  gint tv_exit_status = -1;
  GError * err = NULL;
  GRegex * regex;
  GMatchInfo * match_info = NULL;
  struct frac fps;
  gchar* argv[] = {"tvservice", "-s", NULL};

  /* On a Raspberry Pi we can get the current mode from tvservice so we can set
   * the caps appropriately */
  g_spawn_sync (NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &tv_stdout,
      &tv_stderr, &tv_exit_status, &err);

  if (err) {
    g_printerr ("Failed to run tvservice, falling back to defaults: %s\n",
        err->message);
    goto error;
  }
  if (tv_exit_status != 0) {
    g_printerr ("tvservice failed, falling back to defaults: %s\n", tv_stderr);
    goto error;
  }

  regex = g_regex_new (" (\\d+)x(\\d+) @ (\\d+\\.\\d+)Hz", 0, 0, NULL);
  if (!g_regex_match (regex, tv_stdout, 0, &match_info)) {
    g_printerr("Failed to parse tvservice output, falling back to defaults.  "
        "Output was %s\n", tv_stdout);
    goto error;
  };
  fps = fps_to_frac (g_strtod (g_match_info_fetch (match_info, 3), NULL));
  return g_strdup_printf ("video/x-raw,width=640,height=240,framerate=%i/%i",
      fps.n, fps.d);
error:
  return "video/x-raw,width=640,height=360,framerate=30/1";
}
