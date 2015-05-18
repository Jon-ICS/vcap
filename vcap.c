/*
 * vcap - yet another video (v4l) capture program
 *
 * Author: Jon Trulson <jtrulson@ics.com>
 *
 * This program simply captures a frame from a video device that
 * supports video capture, and saves the output to a JPEG file.  This
 * program currently only supports YUYV frames obtained via mmapped
 * buffers.
 *
 * To build on the edison, you will need to install the 'jpeg-dev'
 * package, and be running a new enough firmware that includes the
 * uvcvideo kernel driver.
 *
 * The MIT license
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */


#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>

#include <sys/ioctl.h>
#include <sys/mman.h>

#include <jpeglib.h>

#include <linux/videodev2.h>

#define CLAMP(_val, _min, _max) \
  (((_val) < (_min)) ? (_min) : (((_val) > (_max)) ? (_max) : (_val)))

#define DEFAULT_VIDEODEV "/dev/video0"
#define DEFAULT_OUTPUTFILE "vcap.jpg"
#define DEFAULT_WIDTH 640
#define DEFAULT_HEIGHT 480
#define DEFAULT_JPEG_QUALITY 95

// holds all of our useful information
typedef struct {
  int width, height;

  // /dev/video* file descriptor
  int fd;

  // capablities struct
  struct v4l2_capability caps;
  // format
  struct v4l2_format fmt;

  // information related to our mmap()'d buffer.
  uint8_t *buffer;
  size_t buffer_len;

  bool verbose;
} videoInfo_t;


void printUsage()
{
  printf("Usage: vcap [-d path ] [-o path ] [ -w width ]\n");
  printf("            [ -h height ] [ -v ]\n\n");
  printf("    -d <path>        video device, default: %s\n", DEFAULT_VIDEODEV);
  printf("    -o <path>        image output file, default: %s\n",
         DEFAULT_OUTPUTFILE);
  printf("    -w width         image width, default: 640\n");
  printf("    -h height        image height, default: 480\n");
  printf("    -v               be verbose.\n");
  return;
}

// This seems... odd, but appears to be necessary.
// Ignore error and retry if the ioctl fails due to EINTR
int xioctl(int fd, int request, void* argp)
{
  int r;

  do r = ioctl(fd, request, argp);
  while(-1 == r && EINTR == errno);

  return r;
}


bool checkCapabilities(videoInfo_t *vi)
{
  if (xioctl(vi->fd, VIDIOC_QUERYCAP, &(vi->caps)) < 0)
    {
      perror("ioctl(VIDIOC_QUERYCAP)");
      return false;
    }
  
  if (vi->verbose)
    {
      printf("Driver: %s\n", vi->caps.driver);
      printf("Device: %s\n", vi->caps.card);
      printf("Caps  : %08x\n", vi->caps.capabilities);
    }

  // see if capturing is supported
  if (!(vi->caps.capabilities & V4L2_CAP_VIDEO_CAPTURE))
    {
      fprintf(stderr, "Device does not support video capture\n");
      return false;
    }

  if (!(vi->caps.capabilities & V4L2_CAP_STREAMING))
    {
      fprintf(stderr, "Device does not support streaming i/o\n");
      return false;
    }

  return true;
}

bool setFormat(videoInfo_t *vi)
{
  vi->fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  vi->fmt.fmt.pix.width = vi->width;
  vi->fmt.fmt.pix.height = vi->height;
  vi->fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
  vi->fmt.fmt.pix.field = V4L2_FIELD_ANY;
        
  if (xioctl(vi->fd, VIDIOC_S_FMT, &(vi->fmt)) < 0)
    {
      perror("ioctl(VIDIOC_S_FMT)");
      return false;
    }

  // Now retrieve the driver's selected format check it -
  // specifically, the width and height might change, causing
  // coredumps if we don't adjust them accordingly.

  if (xioctl(vi->fd, VIDIOC_G_FMT, &(vi->fmt)) < 0)
    {
      perror("ioctl(VIDIOC_C_FMT)");
      return false;
    }

  if (vi->fmt.fmt.pix.width != vi->width)
    {
      fprintf(stderr, 
              "Warning: Selected width (%d) adjusted by driver to %d\n",
              vi->width, vi->fmt.fmt.pix.width);
      vi->width = vi->fmt.fmt.pix.width;
    }
  
  if (vi->fmt.fmt.pix.height != vi->height)
    {
      fprintf(stderr, 
              "Warning: Selected height (%d) adjusted by driver to %d\n",
              vi->height, vi->fmt.fmt.pix.height);
      vi->height = vi->fmt.fmt.pix.height;
    }

  return true;
}
 
bool allocBuffer(videoInfo_t *vi)
{
  struct v4l2_requestbuffers rb;
  memset(&rb, 0, sizeof(rb));

  // we just want one buffer, and we only support mmap().
  rb.count = 1;
  rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  rb.memory = V4L2_MEMORY_MMAP;
 
  if (xioctl(vi->fd, VIDIOC_REQBUFS, &rb) < 0)
    {
      if (errno == EINVAL)
        {
          fprintf(stderr, "Capture device does not support mmapped buffers\n");
        }
      perror("ioctl(VIDIOC_REQBUFS)");
      return false;
    }
 
  // get the buffer and mmap it
  struct v4l2_buffer mbuf;
  memset(&mbuf, 0, sizeof(mbuf));

  mbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  mbuf.memory = V4L2_MEMORY_MMAP;
  mbuf.index = 0;

  if(xioctl(vi->fd, VIDIOC_QUERYBUF, &mbuf) < 0)
    {
      perror("ioctl(VIDIOC_QUERYBUF)");
      return false;
    }
 
  // map it
  vi->buffer = mmap(NULL, mbuf.length, PROT_READ | PROT_WRITE, MAP_SHARED, 
                    vi->fd, mbuf.m.offset);

  if (vi->buffer == MAP_FAILED)
    {
      perror("mmap()");
      return false;
    }

  // we'll need this when unmapping
  vi->buffer_len = mbuf.length;
  
  return true;
}
  
void releaseBuffer(videoInfo_t *vi)
{
  munmap(vi->buffer, vi->buffer_len);
}

bool YUYV2JPEG(FILE *file, videoInfo_t *vi)
{
  struct jpeg_compress_struct jpgInfo;
  struct jpeg_error_mgr jerr;
  JSAMPROW row_pointer[1];
  unsigned char *row_buffer = NULL;
  unsigned char *yuyv = NULL;
  int z;

  row_buffer = calloc(vi->width * 3, 1);
  if (!row_buffer)
    {
      fprintf(stderr, "Line buffer allocation failed\n");
      return false;
    }

  yuyv = vi->buffer;

  jpgInfo.err = jpeg_std_error(&jerr);
  jpeg_create_compress(&jpgInfo);
  jpeg_stdio_dest(&jpgInfo, file);

  jpgInfo.image_width = vi->width;
  jpgInfo.image_height = vi->height;
  // R, G, B
  jpgInfo.input_components = 3;
  jpgInfo.in_color_space = JCS_RGB;

  jpeg_set_defaults(&jpgInfo);
  jpeg_set_quality(&jpgInfo, DEFAULT_JPEG_QUALITY, TRUE);

  jpeg_start_compress(&jpgInfo, TRUE);

  z = 0;
  while (jpgInfo.next_scanline < jpgInfo.image_height) {
    int x;
    unsigned char *ptr = row_buffer;

    for (x = 0; x < vi->width; x++) {
      int r, g, b;
      int y, u, v;

      if (!z)
	y = yuyv[0] << 8;
      else
	y = yuyv[2] << 8;
      u = yuyv[1] - 128;
      v = yuyv[3] - 128;

      r = (y + (359 * v)) >> 8;
      g = (y - (88 * u) - (183 * v)) >> 8;
      b = (y + (454 * u)) >> 8;

      *(ptr++) = CLAMP(r, 0, 255);
      *(ptr++) = CLAMP(g, 0, 255);
      *(ptr++) = CLAMP(b, 0, 255);

      if (z++) {
	z = 0;
	yuyv += 4;
      }
    }

    row_pointer[0] = row_buffer;
    jpeg_write_scanlines(&jpgInfo, row_pointer, 1);
  }

  jpeg_finish_compress(&jpgInfo);
  jpeg_destroy_compress(&jpgInfo);

  free(row_buffer);

  return true;
}

bool saveImage(char *fname, videoInfo_t *vi)
{
  FILE *file;

  if ( (file = fopen(fname, "wb")) == NULL)
    {
      perror("fopen()");
      return false;
    }
  
  YUYV2JPEG(file, vi);
  fclose(file);

  printf("Saved image to %s\n", fname);

  return true;
}
 
bool captureImage(videoInfo_t *vi)
{
  struct v4l2_buffer buf = {0};
  buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf.memory = V4L2_MEMORY_MMAP;
  buf.index = 0;

  // queue our buffer
  if (xioctl(vi->fd, VIDIOC_QBUF, &buf) < 0)
    {
      perror("ioctl(VIDIOC_QBUF)");
      return false;
    }
  
  // enable streaming
  if (xioctl(vi->fd, VIDIOC_STREAMON, &buf.type) < 0)
    {
      perror("ioctl(VIDIOC_STREAMON)");
      return false;
    }

  // use select to wait for a complete frame.
  fd_set fds;

  FD_ZERO(&fds);
  FD_SET(vi->fd, &fds);

  struct timeval tv;
  memset(&tv, 0, sizeof(tv));

  // 5 seconds should be more than enough
  tv.tv_sec = 5;

  int rv;
  if ( (rv = select((vi->fd) + 1, &fds, NULL, NULL, &tv)) < 0)
    {
      perror("select");
      return false;
    }

  if (!rv)
    {
      // timed out
      fprintf(stderr, "Timed out waiting for frame\n");
      return false;
    }

  // de-queue the buffer
  if(xioctl(vi->fd, VIDIOC_DQBUF, &buf) < 0)
    {
      perror("ioctl(VIDIOC_DQBUF)");
      return false;
    }

  // turn off streaming
  if(xioctl(vi->fd, VIDIOC_STREAMOFF, &buf.type) < 0)
    {
      perror("ioctl(VIDIOC_STREAMOFF)");
      return false;
    }
    
  return true;
}

bool initVideoDevice(videoInfo_t *vi, char *vdev)
{
  if (!vdev)
    return false;

  if ( (vi->fd = open(vdev, O_RDWR)) < 0)
    {
      perror("open()");
      return false;
    }
  
  if (!checkCapabilities(vi))
    {
      close(vi->fd);
      vi->fd = -1;
      return false;
    }

  return true;
}

// main().  The final frontier. 
int main(int argc, char *argv[])
{
  videoInfo_t VI;

  memset(&VI, 0, sizeof(VI));
  VI.fd = -1;
  VI.verbose = false;
  VI.width = DEFAULT_WIDTH;
  VI.height = DEFAULT_HEIGHT;

  char *videoDevice = DEFAULT_VIDEODEV;
  char *outputFile = DEFAULT_OUTPUTFILE;

  // process command line options
  int i;
  while ( (i = getopt(argc, argv, "d:o:w:h:v")) != EOF)
    {
      switch(i)
        {
        case 'd':
          videoDevice = optarg;
          break;
        case 'o':
          outputFile = optarg;
          break;
        case 'w':
          if ( (VI.width = atoi(optarg)) == 0)
            {
              fprintf(stderr, "Invalid width, using default\n");
              VI.width = DEFAULT_WIDTH;
            }
          break;
        case 'h':
          if ( (VI.height = atoi(optarg)) == 0)
            {
              fprintf(stderr, "Invalid height, using default\n");
              VI.height = DEFAULT_HEIGHT;
            }
          break;
        case 'v':
          VI.verbose = true;
          break;

        default:
          printUsage();
          return 1;
          break;
        }
    }

  if (!initVideoDevice(&VI, videoDevice))
    return 1;

  if (!setFormat(&VI))
    {
      close(VI.fd);
      return 1;
    }

  if (!allocBuffer(&VI))
    {
      close(VI.fd);
      return 1;
    }
  
  // read the previous image - we don't want it.  The issue here is that
  // without discarding the first frame, you tend to get the previous
  // frame, if there was one.
  if (!captureImage(&VI))
    {
      fprintf(stderr, "Error capturing initial frame, exiting\n");
      releaseBuffer(&VI);
      close(VI.fd);
      return 1;
    }
      
  
  // this time, convert to .jpg and save
  if (captureImage(&VI))
    saveImage(outputFile, &VI);
  
  releaseBuffer(&VI);
  close(VI.fd);

  return 0;
}
