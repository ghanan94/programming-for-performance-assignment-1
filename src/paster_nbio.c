/*
 * Paste together PNG files downloaded from the network.
 *
 * Downloads a bunch of PNG files from BASE_URL and concatenates them.
 *
 * Derived from curl and libpng base code.
 * curl examples are from simple.c included with curl distribution:
 * Copyright (C) 1998 - 2013, Daniel Stenberg, <daniel@haxx.se>, et al.
 * libpng examples are from http://zarb.org/~gc/html/libpng.html
 * Copyright 2002-2011 Guillaume Cottenceau and contributors.
 *
 * Modifications to integrate the code are
 * Copyright 2013 Patrick Lam.
 *
 * This software may be freely redistributed under the terms
 * of the X11 license.
 *
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>

#define PNG_DEBUG 3
#include <png.h>
#include <curl/curl.h>
#include <curl/multi.h>

struct bufdata {
  png_bytep buf;
  int len, pos;
  size_t max_size;
};

#define N 20
#define WIDTH 4000
#define HEIGHT 3000

#define BASE_URL_1 "http://berkeley.uwaterloo.ca:4590/image?img=%d"
#define BASE_URL_2 "http://patricklam.ca:4590/image?img=%d"
#define BASE_URL_3 "http://ece459-1.uwaterloo.ca:4590/image?img=%d"

#define BUF_WIDTH WIDTH/N
#define BUF_HEIGHT HEIGHT
#define BUF_SIZE 10485760
#define ECE459_HEADER "X-Ece459-Fragment: "

#ifdef DEBUG
#define DEBUG_PRINT(x) (printf x)
#else
#define DEBUG_PRINT(x) /* DEBUG is not defined/enabled */
#endif

/* error handling macro */
void abort_(const char * s, ...)
{
  va_list args;
  va_start(args, s);
  vfprintf(stderr, s, args);
  fprintf(stderr, "\n");
  va_end(args);
  abort();
}

/***********************************************************************************/
/* routines to parse PNG data and copy it to an internal buffer */

void read_cb (png_structp png_ptr, png_bytep outBytes, png_size_t byteCountToRead);

/* Given PNG-formatted data at bd, read the data into a buffer that we allocate
 * and return (row_pointers, here).
 *
 * Note: caller must free the returned value. */
png_bytep* read_png_file(png_structp png_ptr, png_infop * info_ptr, struct bufdata * bd)
{
  int y;

  int height;
  png_byte bit_depth;

  png_bytep * row_pointers;

  if (png_sig_cmp(bd->buf, 0, 8))
    abort_("[read_png_file] Input is not recognized as a PNG file");

  *info_ptr = png_create_info_struct(png_ptr);
  if (!*info_ptr)
    abort_("[read_png_file] png_create_info_struct failed");

  if (setjmp(png_jmpbuf(png_ptr)))
    abort_("[read_png_file] Error during init_io");

  bd->pos = 0;
  png_set_read_fn(png_ptr, bd, read_cb);
  png_read_info(png_ptr, *info_ptr);
  height = png_get_image_height(png_ptr, *info_ptr);
  bit_depth = png_get_bit_depth(png_ptr, *info_ptr);
  if (bit_depth != 8)
    abort_("[read_png_file] bit depth 16 PNG files unsupported");

  if (setjmp(png_jmpbuf(png_ptr)))
    abort_("[read_png_file] Error during read_image");

  row_pointers = (png_bytep*) malloc(sizeof(png_bytep) * BUF_HEIGHT);
  for (y=0; y<height; y++)
    row_pointers[y] = (png_byte*) malloc(png_get_rowbytes(png_ptr, *info_ptr));

  png_read_image(png_ptr, row_pointers);

  return row_pointers;
}

/* libpng calls this (at read_png_data's request)
 * to copy data from the in-RAM PNG into our bitmap */
void read_cb (png_structp png_ptr, png_bytep outBytes, png_size_t byteCountToRead) {
  struct bufdata * bd = png_get_io_ptr(png_ptr);

  if (bd == NULL)
    abort_("[read_png_file/read_cb] invalid memory passed to png reader");
  if (bd->pos + byteCountToRead >= bd->len)
    abort_("[read_png_file/read_cb] attempting to read beyond end of buffer");

  memcpy(outBytes, bd->buf+bd->pos, byteCountToRead);
  bd->pos += byteCountToRead;
}

/* copy from row_pointers data array to dest data array, at offset (x0, y0) */
void paint_destination(png_structp png_ptr, png_bytep * row_pointers,
		       int x0, int y0, png_byte* dest)
{
  int x, y, i;

  for (y=0; y<BUF_HEIGHT && (y0+y) < HEIGHT; y++) {
    png_byte* row = row_pointers[y];
    for (x=0; x<BUF_WIDTH; x++) {
      png_byte* ptr = &(row[x*4]);
      int index = ((y0+y)*WIDTH+(x0+x))*4;
      for (i = 0; i < 4; i++)
	dest[index+i] = ptr[i];
    }
  }
}

/***********************************************************************************/
/* routine used by curl to read from the network                                   */

/* curl calls this to transfer data from the network into RAM */
size_t write_cb(char * ptr, size_t size, size_t nmemb, void *userdata) {
  struct bufdata * bd = userdata;

  if (size * nmemb >= bd->max_size) {
    return 0;
  }

  memcpy(bd->buf+bd->pos, ptr, size * nmemb);
  bd->pos += size*nmemb;
  bd->len += size*nmemb;
  return size * nmemb;
}

/***********************************************************************************/
/* routine to write data to disk                                                   */

/* write output_row_pointers back to PNG file as specified by file_name. */
void write_png_file(char* file_name, png_bytep * output_row_pointers)
{
  png_structp png_ptr;
  png_infop info_ptr;

  /* create file */
  FILE *fp = fopen(file_name, "wb");
  if (!fp)
    abort_("[write_png_file] File %s could not be opened for writing", file_name);


  /* initialize stuff */
  png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);

  if (!png_ptr)
    abort_("[write_png_file] png_create_write_struct failed");

  info_ptr = png_create_info_struct(png_ptr);
  if (!info_ptr)
    abort_("[write_png_file] png_create_info_struct failed");

  if (setjmp(png_jmpbuf(png_ptr)))
    abort_("[write_png_file] Error during init_io");

  png_init_io(png_ptr, fp);

  /* write header */
  if (setjmp(png_jmpbuf(png_ptr)))
    abort_("[write_png_file] Error during writing header");

  png_set_IHDR(png_ptr, info_ptr, WIDTH, HEIGHT,
	       8, 6, PNG_INTERLACE_NONE,
	       PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);

  png_write_info(png_ptr, info_ptr);

  /* write bytes */
  if (setjmp(png_jmpbuf(png_ptr)))
    abort_("[write_png_file] Error during writing bytes");

  png_write_image(png_ptr, output_row_pointers);

  if (setjmp(png_jmpbuf(png_ptr)))
    abort_("[write_png_file] Error during end of write");

  png_write_end(png_ptr, NULL);

  fclose(fp);
  png_destroy_write_struct(&png_ptr, &info_ptr);
}

struct headerdata {
  int n;
  bool * received_fragments;
};

size_t header_cb (char * buf, size_t size, size_t nmemb, void * userdata)
{
  struct headerdata * hd = userdata;
  int bytes_in_header = size * nmemb;

  if (bytes_in_header > strlen(ECE459_HEADER) && strncmp(buf, ECE459_HEADER, strlen(ECE459_HEADER)) == 0) {
    // one ought to check that buf is 0-terminated
    //  not guaranteed by spec (!)
    hd->n = atoi(buf+strlen(ECE459_HEADER));
    hd->received_fragments[hd->n] = true;
    printf("received fragment %d\n", hd->n);
  }
  return bytes_in_header;
}

/***********************************************************************************/
//
// Get url to get image from
//
void get_url (char ** url, int img)
{
  static unsigned int counter;

  switch (counter)
  {
  case 0:
    sprintf(*url, BASE_URL_1, img);
    break;
  case 1:
    sprintf(*url, BASE_URL_2, img);
    break;
  case 2:
  default:
    sprintf(*url, BASE_URL_3, img);
    break;
  }

  counter = (counter + 1) % 3;
}

typedef struct _curl_context
{
  int curl_id;
  CURL * curl;
  bool * received_fragments;
  struct bufdata bd;
  struct headerdata hd;
  png_bytep input_buffer;
  char * url;
  int img;
} curl_context, * pcurl_context;

pcurl_context get_curl_context (pcurl_context contexts, int num_contexts, CURL * curl)
{
  int i;

  for (i = 0; i < num_contexts; ++i)
  {
    if (contexts[i].curl == curl)
    {
      return &contexts[i];
    }
  }

  return 0;
}

void init_curl (CURLM * curlm, pcurl_context context)
{
  context->curl = curl_easy_init();
  if (!context->curl)
  {
    abort_("[%s] could not init curl", __FUNCTION__);
  }

  // request appropriate URL
  get_url(&context->url, context->img);
  DEBUG_PRINT(("[%s] Curl #%d requesting URL %s\n", __FUNCTION__, context->curl_id, context->url));
  curl_easy_setopt(context->curl, CURLOPT_URL, context->url);

  context->bd.len = 0;
  context->bd.pos = 0;
  context->bd.max_size = BUF_SIZE;
  curl_easy_setopt(context->curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(context->curl, CURLOPT_WRITEDATA, &context->bd);

  curl_easy_setopt(context->curl, CURLOPT_HEADERDATA, &context->hd);
  curl_easy_setopt(context->curl, CURLOPT_HEADERFUNCTION, header_cb);

  curl_multi_add_handle(curlm, context->curl);
}

void init_curl_for_multi_curl (CURLM * curlm, pcurl_context context, int curl_id, bool * received_fragments, int img)
{
  context->curl_id = curl_id;
  context->img = img;

  context->input_buffer = (png_bytep) malloc(sizeof(png_byte)*BUF_SIZE);
  if (!context->input_buffer)
  {
    abort_("[%s] input_buffer malloc failed\n", __FUNCTION__);
  }

  context->url = (char *) malloc(sizeof(char)*strlen(BASE_URL_1)+4*5);
  if (!context->url)
  {
    abort_("[%s] could not malloc url", __FUNCTION__);
  }

  context->bd.buf = context->input_buffer;
  context->hd.received_fragments = received_fragments;

  init_curl(curlm, context);
}

/***********************************************************************************/

int main(int argc, char **argv)
{
  int c;
  int num_threads = 4;
  int img = 1;
  bool received_all_fragments = false;
  bool * received_fragments = calloc(N, sizeof(bool));
  pcurl_context contexts;
  int i;
  CURLM * curlm;
  int U;
  CURLMsg * msg;
  int Q;
  CURL * curl;
  pcurl_context curr_context;

  while ((c = getopt (argc, argv, "t:i:")) != -1) {
    switch (c) {
    case 't':
      num_threads = strtoul(optarg, NULL, 10);
      if (num_threads == 0) {
	printf("%s: option requires an argument > 0 -- 't'\n", argv[0]);
	return -1;
      }
      break;
    case 'i':
      img = strtoul(optarg, NULL, 10);
      if (img == 0) {
	printf("%s: option requires an argument > 0 -- 'i'\n", argv[0]);
	return -1;
      }
      break;
    default:
      return -1;
    }
  }

  contexts = (pcurl_context) calloc(num_threads, sizeof(curl_context));
  if (!contexts)
  {
    abort_("[%s] contexts malloc failed", __FUNCTION__);
  }

  curl_global_init(CURL_GLOBAL_ALL);

  curlm = curl_multi_init();
  if (!curlm)
  {
    abort_("[%s] curlm init failed\n", __FUNCTION__);
  }

  // Set max connections
  // curl_multi_setopt(curlm, CURLMOPT_MAXCONNECTS, (long) num_threads);

  for (i = 0; i < num_threads; ++i)
  {
    init_curl_for_multi_curl(curlm, &contexts[i], i, received_fragments, img);
  }

  png_structp png_ptr;
  png_infop info_ptr;

  png_byte * output_buffer = calloc(WIDTH*HEIGHT*4, sizeof(png_byte));

  U = -1;

  while (U) {
    // run all curls
    curl_multi_perform(curlm, &U);

    if (U) {
      // TODO
    }

    while ((msg = curl_multi_info_read(curlm, &Q)))
    {
      // Check to make sure the CURL-ing is done
      if (msg->msg == CURLMSG_DONE) {
	curl = msg->easy_handle;
	curr_context = get_curl_context(contexts, num_threads, curl);
	curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, curr_context->url);
	curl_multi_remove_handle(curlm, curl);
	curl_easy_cleanup(curl);
      }
      else
      {
	fprintf(stderr, "E: CURLMsg (%d)\n", msg->msg);
	abort_("[%s] curl msg not done\n", __FUNCTION__);
      }

      png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
      if (!png_ptr)
	abort_("[main] png_create_read_struct failed");

      // read PNG (as downloaded from network) and copy it to output buffer
      png_bytep* row_pointers = read_png_file(png_ptr, &info_ptr, &curr_context->bd);
      paint_destination(png_ptr, row_pointers, curr_context->hd.n*BUF_WIDTH, 0, output_buffer);

      // free allocated memory
      for (int y=0; y<BUF_HEIGHT; y++)
	free(row_pointers[y]);
      free(row_pointers);
      png_destroy_read_struct(&png_ptr, &info_ptr, NULL);

      // check for unreceived fragments
      received_all_fragments = true;
      for (int i = 0; i < N; i++)
	if (!received_fragments[i])
	  received_all_fragments = false;

      if (!received_all_fragments)
      {
	// init a new curl
	init_curl(curlm, curr_context);

	// Call this to start the newly added curl request
	curl_multi_perform(curlm, &U);
      }
    }
  }

  curl_multi_cleanup(curlm);
  curl_global_cleanup();

  for (i = 0; i < num_threads; ++i)
  {
    // Clear all pointers created for each context
    free(contexts[i].input_buffer);
    free(contexts[i].url);
  }
  free(contexts);

  // now, write the array back to disk using write_png_file
  png_bytep * output_row_pointers = (png_bytep*) malloc(sizeof(png_bytep) * HEIGHT);

  for (int i = 0; i < HEIGHT; i++)
    output_row_pointers[i] = &output_buffer[i*WIDTH*4];

  write_png_file("output.png", output_row_pointers);
  free(output_row_pointers);
  free(output_buffer);
  free(received_fragments);

  return 0;
}
