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

#include <pthread.h>

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

//
// dest is a shared memory space between threads. This function can be called my
// multiple threads at the same time so we need to make sure that when
// writing to threads we dont have different threads writing different fragments
// at the same time
//
static pthread_mutex_t paint_destination_lock;

void paint_destination(png_structp png_ptr, png_bytep * row_pointers,
		       int x0, int y0, png_byte* dest)
{
  int x, y, i;

  pthread_mutex_lock(&paint_destination_lock);
  for (y=0; y<BUF_HEIGHT && (y0+y) < HEIGHT; y++) {
    png_byte* row = row_pointers[y];
    for (x=0; x<BUF_WIDTH; x++) {
      png_byte* ptr = &(row[x*4]);
      int index = ((y0+y)*WIDTH+(x0+x))*4;
      for (i = 0; i < 4; i++)
	dest[index+i] = ptr[i];
    }
  }
  pthread_mutex_unlock(&paint_destination_lock);
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

//
// I (Ghanan) added the mutex because multiple threads can call this function
// at the same time and there is a chance that they obtained the same fragments
// so two threads may attmept to write to same location in memory (the
// received_fragments array is shard between threads).:
// >>>hd->received_fragments[hd->n] = true;
//
static pthread_mutex_t header_cb_lock;

size_t header_cb (char * buf, size_t size, size_t nmemb, void * userdata)
{
  struct headerdata * hd = userdata;
  int bytes_in_header = size * nmemb;

  if (bytes_in_header > strlen(ECE459_HEADER) && strncmp(buf, ECE459_HEADER, strlen(ECE459_HEADER)) == 0) {
    // one ought to check that buf is 0-terminated
    //  not guaranteed by spec (!)
    hd->n = atoi(buf+strlen(ECE459_HEADER));

    pthread_mutex_lock(&header_cb_lock);
    hd->received_fragments[hd->n] = true;
    pthread_mutex_unlock(&header_cb_lock);

    printf("received fragment %d\n", hd->n);
  }

  return bytes_in_header;
}

/***********************************************************************************/

typedef struct _thread_function_context
{
  int thread_id;
  bool * received_fragments;
  int img;
  png_byte * output_buffer;
} thread_function_context;

static pthread_mutex_t get_url_lock;

//
// Get url to get image from
//
void get_url (char ** url, int img)
{
  static unsigned int counter;

  pthread_mutex_lock(&get_url_lock);

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

  pthread_mutex_unlock(&get_url_lock);
}

//
// Funciton that each thread will run
//
void *thread_function (void * context)
{
  bool received_all_fragments;
  thread_function_context * tf_context;
  CURL *curl;
  CURLcode res;
  png_structp png_ptr;
  png_infop info_ptr;

  tf_context = (thread_function_context *) context;

  printf("[%s] Thread #%d started...\n", __FUNCTION__, tf_context->thread_id);

  curl = curl_easy_init();
  if (!curl)
    abort_("[%s] could not initialize curl", __FUNCTION__);

  char * url = malloc(sizeof(char)*strlen(BASE_URL_1)+4*5);
  png_bytep input_buffer = malloc(sizeof(png_byte)*BUF_SIZE);

  struct bufdata bd;
  bd.buf = input_buffer;
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &bd);

  struct headerdata hd; hd.received_fragments = tf_context->received_fragments;
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &hd);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);

  do {
    // request appropriate URL
    // Calling get_url each loop iterations allows it to change
    // urls each time
    get_url(&url, tf_context->img);
    DEBUG_PRINT(("[%s] thread id #%d requesting URL %s\n", __FUNCTION__, tf_context->thread_id, url));
    curl_easy_setopt(curl, CURLOPT_URL, url);

    png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr)
      abort_("[%s] png_create_read_struct failed", __FUNCTION__);

    // reset input buffer
    bd.len = bd.pos = 0; bd.max_size = BUF_SIZE;

    // do curl request; check for errors
    res = curl_easy_perform(curl);
    if(res != CURLE_OK)
      abort_("[%s] curl_easy_perform() failed: %s\n",
	     __FUNCTION__, curl_easy_strerror(res));

    // read PNG (as downloaded from network) and copy it to output buffer
    png_bytep* row_pointers = read_png_file(png_ptr, &info_ptr, &bd);
    paint_destination(png_ptr, row_pointers, hd.n*BUF_WIDTH, 0, tf_context->output_buffer);

    // free allocated memory
    for (int y=0; y<BUF_HEIGHT; y++)
      free(row_pointers[y]);
    free(row_pointers);
    png_destroy_read_struct(&png_ptr, &info_ptr, NULL);

    // check for unreceived fragments
    received_all_fragments = true;
    for (int i = 0; i < N; i++)
      if (!tf_context->received_fragments[i])
	received_all_fragments = false;
  } while (!received_all_fragments);
  free(url);
  free(input_buffer);

  curl_easy_cleanup(curl);

  pthread_exit(0);
}

/***********************************************************************************/

int main(int argc, char **argv)
{
  int c;
  int num_threads = 4;
  int img = 1;
  bool * received_fragments;
  pthread_t * threads;
  thread_function_context * thread_function_contexts;
  int i;
  png_byte * output_buffer;

  while ((c = getopt (argc, argv, "t:")) != -1) {
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

  DEBUG_PRINT(("[%s] Number of threads: %d\n", __FUNCTION__, num_threads));
  DEBUG_PRINT(("[%s] Img #: %d\n", __FUNCTION__, img));

  received_fragments = calloc(N, sizeof(bool));
  if (!received_fragments)
  {
    abort_("[%s] received_fragments calloc failed", __FUNCTION__);
  }

  output_buffer = calloc(WIDTH*HEIGHT*4, sizeof(png_byte));
  if (!output_buffer)
  {
    abort_("[%s] output_buffer calloc failed", __FUNCTION__);
  }

  if (pthread_mutex_init(&get_url_lock, NULL))
  {
    abort_("[%s] get_url_lock pthread_mutex_init failed", __FUNCTION__);
  }

  if (pthread_mutex_init(&header_cb_lock, NULL))
  {
    abort_("[%s] header_cb_lock pthread_mutex_init failed", __FUNCTION__);
  }

  if (pthread_mutex_init(&paint_destination_lock, NULL))
  {
    abort_("[%s] paint_destination_lock pthread_mutex_init failed", __FUNCTION__);
  }

  threads = (pthread_t *) calloc(num_threads, sizeof(pthread_t));
  if (!threads)
  {
    abort_("[%s] thread calloc failed", __FUNCTION__);
  }

  thread_function_contexts = (thread_function_context *) calloc(num_threads, sizeof(thread_function_context));
  if (!thread_function_contexts)
  {
    abort_("%s] thread_function_contexts calloc failed");
  }

  printf("[%s] Dispatching threads...\n", __FUNCTION__);
  for (i = 0; i < num_threads; ++i)
  {
    thread_function_contexts[i].thread_id = i;
    thread_function_contexts[i].received_fragments = received_fragments;
    thread_function_contexts[i].img = img;
    thread_function_contexts[i].output_buffer = output_buffer;

    if (pthread_create(&threads[i], NULL, thread_function, (void *) &thread_function_contexts[i]))
    {
      abort_("%s] failed to create thread %d\n", __FUNCTION__, i);
    }
  }

  printf("[%s] Waiting for threads to finish...\n", __FUNCTION__);
  for (i = 0; i != num_threads; ++i)
  {
    pthread_join(threads[i], NULL);
  }

  // call each thread

  // now, write the array back to disk using write_png_file
  png_bytep * output_row_pointers = (png_bytep*) malloc(sizeof(png_bytep) * HEIGHT);

  for (int i = 0; i < HEIGHT; i++)
    output_row_pointers[i] = &output_buffer[i*WIDTH*4];

  write_png_file("output.png", output_row_pointers);
  free(output_row_pointers);
  free(output_buffer);
  free(received_fragments);
  free(threads);
  free(thread_function_contexts);

  return 0;
}
