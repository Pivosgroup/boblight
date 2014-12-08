/*
 * boblight
 * Copyright (C) Bob  2009 
 * 
 * boblight is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * boblight is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define BOBLIGHT_DLOPEN
#include "lib/boblight.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/poll.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/ioctl.h>

#include "config.h"
#include "util/misc.h"
#include "util/timeutils.h"
#include "flagmanager-aml.h"

using namespace std;

//#include <linux/pivos/aml_snapshot.h>

#define AMSNAPSHOT_IOC_MAGIC 'T'
#define AMSNAPSHOT_IOC_GET_FRAME   _IOW(AMSNAPSHOT_IOC_MAGIC, 0x04, unsigned long)

#define AMSNAPSHOT_FOURCC(a, b, c, d)\
	((__u32)(a) | ((__u32)(b) << 8) | ((__u32)(c) << 16) | ((__u32)(d) << 24))

#define AMSNAPSHOT_FMT_S24_BGR   AMSNAPSHOT_FOURCC('B', 'G', 'R', '3') /* 24  BGR-8-8-8     */
#define AMSNAPSHOT_FMT_S24_RGB   AMSNAPSHOT_FOURCC('R', 'G', 'B', '3') /* 24  RGB-8-8-8     */
#define AMSNAPSHOT_FMT_S32_RGBA  AMSNAPSHOT_FOURCC('R', 'G', 'B', 'A') /* 32  BGR-8-8-8-8   */
#define AMSNAPSHOT_FMT_S32_BGRA  AMSNAPSHOT_FOURCC('B', 'G', 'R', 'A') /* 32  BGR-8-8-8-8   */
#define AMSNAPSHOT_FMT_S32_ABGR  AMSNAPSHOT_FOURCC('A', 'B', 'G', 'R') /* 32  BGR-8-8-8-8   */
#define AMSNAPSHOT_FMT_S32_ARGB  AMSNAPSHOT_FOURCC('A', 'R', 'G', 'B') /* 32  BGR-8-8-8-8   */

struct aml_snapshot_t {
  unsigned int  src_x;
  unsigned int  src_y;
  unsigned int  src_width;
  unsigned int  src_height;
  unsigned int  dst_width;
  unsigned int  dst_height;
  unsigned int  dst_stride;
  unsigned int  dst_format;
  unsigned int  dst_size;
  unsigned long dst_vaddr;
};

volatile bool g_stop = false;
CFlagManagerAML g_flagmanager;
/*********************************************************
 *********************************************************/
static void SignalHandler(int signum)
{
  if (signum == SIGTERM)
  {
    fprintf(stderr, "caught SIGTERM\n");
    g_stop = true;
  }
  else if (signum == SIGINT)
  {
    fprintf(stderr, "caught SIGTERM\n");
    g_stop = true;
  }
}

#define VIDEO_PATH       "/dev/amvideo"
#define AMSTREAM_IOC_MAGIC  'S'
#define AMSTREAM_IOC_GET_VIDEO_DISABLE  _IOR(AMSTREAM_IOC_MAGIC, 0x48, unsigned long)
#define AMSTREAM_IOC_GET_VIDEO_AXIS     _IOR(AMSTREAM_IOC_MAGIC, 0x4b, unsigned long)
static int amvideo_utils_get_position(aml_snapshot_t &snapshot)
{
  int video_fd;
  int axis[4], video_disable;

  video_fd = open(VIDEO_PATH, O_RDWR);
  if (video_fd < 0) {
    return -1;
  }

  ioctl(video_fd, AMSTREAM_IOC_GET_VIDEO_DISABLE, &video_disable);
  if (video_disable)
  {
    close(video_fd);
    return 1;
  }

  ioctl(video_fd, AMSTREAM_IOC_GET_VIDEO_AXIS, &axis[0]);
  close(video_fd);

  snapshot.src_x = axis[0];
  snapshot.src_y = axis[1];
  snapshot.src_width  = axis[2] - axis[0] + 1;
  snapshot.src_height = axis[3] - axis[1] + 1;

  return 0;
}

static void frameToboblight(void *boblight, uint8_t* outputptr, int w, int h, int stride)
{
  if (!boblight)
    return;

  if (!outputptr)
    return;

  //read out pixels and hand them to libboblight
  uint8_t* buffptr;
  for (int y = 0; y < h; y++) {
    buffptr = outputptr + stride * y;
    for (int x = 0; x < w; x++) {
      int rgb[3];
      rgb[0] = *(buffptr++);
      rgb[1] = *(buffptr++);
      rgb[2] = *(buffptr++);

      //fprintf(stdout, "frameToboblight: x(%d), y(%d)\n", x, y);

      boblight_addpixelxy(boblight, x, y, rgb);
    }
  }
}

static int Run(void* boblight)
{
  int snapshot_fd = -1;
  aml_snapshot_t aml_snapshot = {0};
  int lastPriority = 255;

  aml_snapshot.dst_width  = 160;
  aml_snapshot.dst_height = 160;
/*
  // min width/height clamps (ge2d)
  if (aml_snapshot.dst_width < 160)
    aml_snapshot.dst_width = 160;
  if (aml_snapshot.dst_height < 80)
    aml_snapshot.dst_height = 80;
*/
  // calc stride, size and alloc mem
  aml_snapshot.dst_stride = aml_snapshot.dst_width  * 3;
  aml_snapshot.dst_size   = aml_snapshot.dst_stride * aml_snapshot.dst_height;
  aml_snapshot.dst_vaddr  = (unsigned long)calloc(aml_snapshot.dst_size, 1);

  fprintf(stdout, "Connection to boblightd config: width(%d), height(%d)\n",
    aml_snapshot.dst_width, aml_snapshot.dst_height);
  //tell libboblight how big our image is
  boblight_setscanrange(boblight, (int)aml_snapshot.dst_width, (int)aml_snapshot.dst_height);

  while(!g_stop)
  {
    int64_t bgn = GetTimeUs();

    if (snapshot_fd == -1) {
      snapshot_fd = open("/dev/aml_snapshot", O_RDWR | O_NONBLOCK, 0);
      if (snapshot_fd == -1) {
        sleep(1);
        continue;
      } else {
        fprintf(stdout, "snapshot_fd(%d) \n", snapshot_fd);
      }
    }

    // match source ratio if possible
    if (amvideo_utils_get_position(aml_snapshot) != 0) {
      if ( lastPriority != 255)
      {
        boblight_setpriority(boblight, 255);
        lastPriority = 255;
      }
      sleep(1);
      continue;
    }

    if (ioctl(snapshot_fd, AMSNAPSHOT_IOC_GET_FRAME, &aml_snapshot) == 0) {
      // image to boblight convert.
      frameToboblight(boblight, (uint8_t*)aml_snapshot.dst_vaddr,
        aml_snapshot.dst_width, aml_snapshot.dst_height, aml_snapshot.dst_stride);
      if (lastPriority != g_flagmanager.m_priority)
      {
        boblight_setpriority(boblight, g_flagmanager.m_priority);
        lastPriority = g_flagmanager.m_priority;
      }
      if (!boblight_sendrgb(boblight, 1, NULL))
      {
        // some error happened, probably connection broken, so bitch and try again
        PrintError(boblight_geterror(boblight));
        boblight_destroy(boblight);
        continue;
      }
    }
    else
    {
      //fprintf(stdout, "nap time\n");
      sleep(1);
    }

    int64_t end = GetTimeUs();
    float calc_time_ms = (float)(end - bgn) / 1000.0;
    // throttle to 100ms max cycle rate
    calc_time_ms -= 100.0;
    if ((int)calc_time_ms < 0)
      usleep((int)(-calc_time_ms * 1000));
  }

  // last image is black
  boblight_setpriority(boblight, 255);
  boblight_destroy(boblight);
  close(snapshot_fd);
  return 0;
}

/*********************************************************
 *********************************************************/
int main(int argc, char *argv[])
{
  //load the boblight lib, if it fails we get a char* from dlerror()
  const char* boblight_error = boblight_loadlibrary(NULL);
  if (boblight_error)
  {
    PrintError(boblight_error);
    return 1;
  }

  //try to parse the flags and bitch to stderr if there's an error
  try {
    g_flagmanager.ParseFlags(argc, argv);
  }
  catch (string error) {
    PrintError(error);
    g_flagmanager.PrintHelpMessage();
    return 1;
  }
  
  if (g_flagmanager.m_printhelp) {
    g_flagmanager.PrintHelpMessage();
    return 1;
  }

  if (g_flagmanager.m_printboblightoptions) {
    g_flagmanager.PrintBoblightOptions();
    return 1;
  }

  //set up signal handlers
  signal(SIGINT,  SignalHandler);
  signal(SIGTERM, SignalHandler);

  //keep running until we want to quit
  while(!g_stop) {
    //init boblight
    void* boblight = boblight_init();

    fprintf(stdout, "Connecting to boblightd(%p)\n", boblight);
    
    //try to connect, if we can't then bitch to stderr and destroy boblight
    if (!boblight_connect(boblight, g_flagmanager.m_address, g_flagmanager.m_port, 5000000) ||
        !boblight_setpriority(boblight, 255)) {
      PrintError(boblight_geterror(boblight));
      fprintf(stdout, "Waiting 10 seconds before trying again\n");
      boblight_destroy(boblight);
      sleep(10);
      continue;
    }

    fprintf(stdout, "Connection to boblightd opened\n");

    //try to parse the boblight flags and bitch to stderr if we can't
    try {
      g_flagmanager.ParseBoblightOptions(boblight);
    }
    catch (string error) {
      PrintError(error);
      return 1;
    }

    try {
      Run(boblight);
    }
    catch (string error) {
      PrintError(error);
      boblight_destroy(boblight);
      return 1;
    }
  }
  fprintf(stdout, "Exiting\n");
}
