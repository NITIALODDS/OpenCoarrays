/* One-sided MPI implementation of Libcaf

Copyright (c) 2012-2016, Sourcery, Inc.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the Sourcery, Inc., nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL SOURCERY, INC., BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.  */

/****l* mpi/mpi_caf.c
 * NAME
 *   mpi_caf
 * SYNOPSIS
 *   This program implements the LIBCAF_MPI transport layer.
******
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>        /* For memcpy.  */
#include <stdarg.h>        /* For variadic arguments.  */
#ifndef ALLOCA_MISSING
#include <alloca.h>        /* Assume functionality provided elsewhere if missing */
#endif
#include <unistd.h>
#include <mpi.h>
#include <pthread.h>
#include <signal.h>        /* For raise */

#ifdef HAVE_MPI_EXT_H
#include <mpi-ext.h>
#endif
#ifdef USE_FAILED_IMAGES
  #define WITH_FAILED_IMAGES 1
#endif

#include "libcaf.h"

/* Define GFC_CAF_CHECK to enable run-time checking.  */
/* #define GFC_CAF_CHECK  1  */


#ifndef EXTRA_DEBUG_OUTPUT
#define dprint(...)
#else
#define dprint(args...) fprintf (stderr, args)
#endif

#ifdef GCC_GE_7
/** The caf-token of the mpi-library.

Objects of this data structure are owned by the library and are treated as a
black box by the compiler.  In the coarray-program the tokens are opaque
pointers, i.e. black boxes.

For each coarray (allocatable|save|pointer) (scalar|array|event|lock) a token
needs to be present.
*/
typedef struct mpi_caf_token_t
{
  /** The pointer to memory associated to this token's data on the local image.
  The compiler uses the address for direct access to the memory of the object
  this token is assocated to, i.e., the memory pointed to be local_memptr is
  the scalar or array.
  When the library is responsible for deleting the memory, then this is the one
  to free.  */
  void *memptr;
  /** The MPI window to associated to the object's data.
  The window is used to access the data on other images. In pre GCC_GE_7
  installations this was the token.  */
  MPI_Win memptr_win;
  /** The pointer to the primary array, i.e., to coarrays that are arrays and
  not a derived type. */
  gfc_descriptor_t *desc;
} mpi_caf_token_t;

/** For components of derived type coarrays a slave_token is needed when the
component has the allocatable or pointer attribute. The token is reduced in
size, because the other data is already accessible and has been read from the
remote to fullfill the request.

  TYPE t
  +------------------+
  | comp *           |
  | comp_token *     |
  +------------------+

  TYPE(t) : o                struct T // the mpi_caf_token to t
                             +----------------+
                             | ...            |
                             +----------------+

  o[2]%.comp                 // using T to get o of [2]

  +-o-on-image-2----+ // "copy" of the requierd parts of o[2] on current image
  | 0x4711          | // comp * in global_dynamic_window
  | 0x2424          | // comp_token * of type slave_token
  +-----------------+
  // now all required data is present on the current image to access the remote
  // components. This nests without limit.
*/
typedef struct mpi_caf_slave_token_t
{
  /** The pointer to the memory associated to this slave token's data on the
  local image.  When the library is responsible for deleting the memory, then
  this is the one to free.  And this is the only reason why its stored here.  */
  void *memptr;
  /** The pointer to the descriptor or NULL for scalars.
  When referencing a remote component array, then the extensions of the array
  are needed. Usually the data pointer is at offset zero of the descriptor_t
  structure, but we don't rely on it. So store the pointer to the base address
  of the descriptor. The descriptor always is in the window of the master data
  or the allocated component and is never stored at an address not accessible
  by a window. */
  gfc_descriptor_t *desc;
} mpi_caf_slave_token_t;

#define TOKEN(X) &(((mpi_caf_token_t *) (X))->memptr_win)
#else
typedef MPI_Win *mpi_caf_token_t;
#define TOKEN(X) ((mpi_caf_token_t) (X))
#endif

/* Forward declaration of prototype.  */

static void terminate_internal (int stat_code, int exit_code)
            __attribute__ ((noreturn));
static void sync_images_internal (int count, int images[], int *stat,
                                  char *errmsg, int errmsg_len, bool internal);

/* Global variables.  */
static int caf_this_image;
static int caf_num_images = 0;
static int caf_is_finalized = 0;
static MPI_Win global_dynamic_win;

#if MPI_VERSION >= 3
  MPI_Info mpi_info_same_size;
#endif // MPI_VERSION

/* The size of pointer on this plattform. */
static const size_t stdptr_size = sizeof(void *);

/* Variables needed for syncing images. */

static int *images_full;
MPI_Request *sync_handles;
static int *arrived;
static const int MPI_TAG_CAF_SYNC_IMAGES = 424242;

/* Pending puts */
#if defined(NONBLOCKING_PUT) && !defined(CAF_MPI_LOCK_UNLOCK)
typedef struct win_sync {
  MPI_Win *win;
  int img;
  struct win_sync *next;
} win_sync;

static win_sync *last_elem = NULL;
static win_sync *pending_puts = NULL;
#endif

/* Linked list of static coarrays registered.  Do not expose to public in the
header, because it is implementation specific.  */
struct caf_allocated_tokens_t {
  caf_token_t token;
  struct caf_allocated_tokens_t *prev;
} *caf_allocated_tokens = NULL;

#ifdef GCC_GE_7
/* Linked list of slave coarrays registered. */
struct caf_allocated_slave_tokens_t {
  mpi_caf_slave_token_t *token;
  struct caf_allocated_slave_tokens_t *prev;
} *caf_allocated_slave_tokens = NULL;
#endif

/* Image status variable */

static int img_status = 0;
static MPI_Win *stat_tok;

/* Active messages variables */

char **buff_am;
MPI_Status *s_am;
MPI_Request *req_am;
MPI_Datatype *dts;
char *msgbody;
pthread_mutex_t lock_am;
int done_am=0;

char err_buffer[MPI_MAX_ERROR_STRING];

/* All CAF runtime calls should use this comm instead of
   MPI_COMM_WORLD for interoperability purposes. */
MPI_Comm CAF_COMM_WORLD;

#ifdef WITH_FAILED_IMAGES
/* The stati of the other images.  image_stati is an array of size
 * caf_num_images at the beginning the status of each image is noted here
 * where the index is the image number minus one.  */
int *image_stati;

/* This gives the number of all images that are known to have failed.  */
int num_images_failed = 0;

/* This is the number of all images that are known to have stopped.  */
int num_images_stopped = 0;

/* The async. request-handle to all participating images.  */
MPI_Request alive_request;

/* This dummy is used for the alive request.  Its content is arbitrary and
 * never read.  Its just a memory location where one could put something,
 * which is never done.  */
int alive_dummy;

/* The mpi error-handler object associate to CAF_COMM_WORLD.  */
MPI_Errhandler failed_stopped_CAF_COMM_WORLD_mpi_errorhandler;

/* The monitor comm for detecting failed images. We can not attach the monitor
 * to CAF_COMM_WORLD or the messages send by sync images would be caught by
 * the monitor. */
MPI_Comm alive_comm;

/* Set when entering a sync_images_internal, to prevent the error handler from
 * eating our messages. */
bool no_stopped_images_check_in_errhandler = 0;
#endif

/* For MPI interoperability, allow external initialization
   (and thus finalization) of MPI. */
bool caf_owns_mpi = false;

/* Foo function pointers for coreduce.
  The handles when arguments are passed by reference.  */
int (*int32_t_by_reference)(void *, void *);
float (*float_by_reference)(void *, void *);
double (*double_by_reference)(void *, void *);
/* Strings are always passed by reference.  */
void (*char_by_reference)(void *, int, void *, void *, int, int);
/* The handles when arguments are passed by value.  */
int (*int32_t_by_value)(int32_t, int32_t);
float (*float_by_value)(float, float);
double (*double_by_value)(double, double);

/* Define shortcuts for Win_lock and _unlock depending on whether the primitives
   are available in the MPI implementation.  When they are not available the
   shortcut is expanded to nothing by the preprocessor else to the API call.
   This prevents having #ifdef #else #endif constructs strewn all over the code
   reducing its readability.  */
#ifdef CAF_MPI_LOCK_UNLOCK
#define CAF_Win_lock(type, img, win) MPI_Win_lock (type, img, 0, win)
#define CAF_Win_unlock(img, win) MPI_Win_unlock (img, win)
#define CAF_Win_lock_all(win)
#define CAF_Win_unlock_all(win)
#else //CAF_MPI_LOCK_UNLOCK
#define CAF_Win_lock(type, img, win)
#define CAF_Win_unlock(img, win) MPI_Win_flush (img, win)
#if MPI_VERSION >= 3
#define CAF_Win_lock_all(win) MPI_Win_lock_all (MPI_MODE_NOCHECK, win)
#else
#define CAF_Win_lock_all(win)
#endif
#define CAF_Win_unlock_all(win) MPI_Win_unlock_all (win)
#endif //CAF_MPI_LOCK_UNLOCK

#define MIN(X, Y) (((X) < (Y)) ? (X) : (Y))

#if defined(NONBLOCKING_PUT) && !defined(CAF_MPI_LOCK_UNLOCK)
void explicit_flush()
{
  win_sync *w=pending_puts, *t;
  MPI_Win *p;
  while(w != NULL)
    {
      p = w->win;
      MPI_Win_flush(w->img,*p);
      t = w;
      w = w->next;
      free(t);
    }
  last_elem = NULL;
  pending_puts = NULL;
}
#endif

#ifdef HELPER
void helperFunction()
{
  int i = 0, flag = 0, msgid = 0;
  int ndim = 0, position = 0;

  s_am = calloc(caf_num_images, sizeof(MPI_Status));
  req_am = calloc(caf_num_images, sizeof(MPI_Request));
  dts = calloc(caf_num_images, sizeof(MPI_Datatype));

  for(i=0;i<caf_num_images;i++)
    MPI_Irecv(buff_am[i], 1000, MPI_PACKED, i, 1, CAF_COMM_WORLD, &req_am[i]);

  while(1)
    {
      pthread_mutex_lock(&lock_am);
      for(i=0;i<caf_num_images;i++)
        {
          if(!caf_is_finalized)
            {
              MPI_Test(&req_am[i], &flag, &s_am[i]);
              if(flag==1)
                {
                  position = 0;
                  MPI_Unpack(buff_am[i], 1000, &position, &msgid, 1, MPI_INT, CAF_COMM_WORLD);
                  /* msgid=2 was initially assigned to strided transfers, it can be reused */
                  /* Strided transfers Msgid=2 */

                  /* You can add you own function */

                  if(msgid==2)
                    {
                      msgid=0; position=0;
                    }
                  MPI_Irecv(buff_am[i], 1000, MPI_PACKED, i, 1, CAF_COMM_WORLD, &req_am[i]);
                  flag=0;
                }
            }
          else
            {
              done_am=1;
              pthread_mutex_unlock(&lock_am);
              return;
            }
        }
        pthread_mutex_unlock(&lock_am);
    }
}
#endif


/* Keep in sync with single.c.  */

static void
caf_runtime_error (const char *message, ...)
{
  va_list ap;
  fprintf (stderr, "Fortran runtime error on image %d: ", caf_this_image);
  va_start (ap, message);
  vfprintf (stderr, message, ap);
  va_end (ap);
  fprintf (stderr, "\n");

  /* FIXME: Shutdown the Fortran RTL to flush the buffer.  PR 43849.  */
  /* FIXME: Do some more effort than just to abort.  */
  //  MPI_Finalize();

  /* Should be unreachable, but to make sure also call exit.  */
  exit (EXIT_FAILURE);
}

/* Forward declaration of the feature unsupported message for failed images
 * functions. */
static void
unsupported_fail_images_message(const char * functionname);

/* Forward declaration of the feature unimplemented message for allocatable
 * components. */
static void
unimplemented_alloc_comps_message(const char * functionname);

static void
locking_atomic_op(MPI_Win win, int *value, int newval,
                  int compare, int image_index, int index)
{
  CAF_Win_lock (MPI_LOCK_EXCLUSIVE, image_index-1, win);
  MPI_Compare_and_swap (&newval,&compare,value, MPI_INT,image_index-1,
                        index*sizeof(int), win);
  CAF_Win_unlock (image_index-1, win);
}


/* Define a helper to check whether the image at the given index is healthy,
 * i.e., it hasn't failed.  */
#ifdef WITH_FAILED_IMAGES
#define check_image_health(image_index, stat) \
  if (image_stati[image_index - 1] == STAT_FAILED_IMAGE) \
    { \
      if (stat == NULL) terminate_internal (STAT_FAILED_IMAGE, 0); \
      *stat = STAT_FAILED_IMAGE; \
      return; \
    }
#else
#define check_image_health(image_index, stat)
#endif

#ifdef WITH_FAILED_IMAGES
/** Handle failed image's errors and try to recover the remaining process to
 * allow the user to detect an image fail and exit gracefully. */
static void
failed_stopped_errorhandler_function (MPI_Comm* pcomm, int* perr, ...)
{
  MPI_Comm comm, shrunk, newcomm;
  int num_failed_in_group, i, err;
  MPI_Group comm_world_group, failed_group;
  int *ranks_of_failed_in_comm_world, *ranks_failed;
  int ns, srank, crank, rc, flag, drank, ierr, newrank;
  bool stopped = false;

  comm = *pcomm;

  MPI_Error_class (*perr, &err);
  if (err != MPIX_ERR_PROC_FAILED && err != MPIX_ERR_REVOKED)
    {
      /* We can handle PROC_FAILED and REVOKED ones only. */
      char errstr[MPI_MAX_ERROR_STRING];
      int errlen;
      MPI_Error_string (err, errstr, &errlen);
      /* We can't use caf_runtime_error here, because that would exit, which
       * means only the one process will stop, but we need to stop MPI
       * completely, which can be done by calling MPI_Abort(). */
      fprintf (stderr, "Fortran runtime error on image #%d:\nMPI error: '%s'.\n",
               caf_this_image, errstr);
      MPI_Abort (*pcomm, err);
    }

  dprint ("%d/%d: %s (error = %d)\n", caf_this_image, caf_num_images, __FUNCTION__, err);

  MPIX_Comm_failure_ack (comm);
  MPIX_Comm_failure_get_acked (comm, &failed_group);
  MPI_Group_size (failed_group, &num_failed_in_group);

  dprint ("%d/%d: %s: %d images failed.\n", caf_this_image, caf_num_images, __FUNCTION__, num_failed_in_group);
  if (num_failed_in_group <= 0)
    {
      *perr = MPI_SUCCESS;
      return;
    }
  if (num_failed_in_group > caf_num_images)
    {
      *perr = MPI_SUCCESS;
      return;
    }

  MPI_Comm_group (MPI_COMM_WORLD, &comm_world_group);
  ranks_of_failed_in_comm_world = (int *) alloca (sizeof (int)
						  * num_failed_in_group);
  ranks_failed = (int *) alloca (sizeof (int) * num_failed_in_group);
  for (i = 0; i < num_failed_in_group; ++i)
    ranks_failed[i] = i;
  /* Now translate the ranks of the failed images into communicator world. */
  MPI_Group_translate_ranks (failed_group, num_failed_in_group, ranks_failed,
			     comm_world_group, ranks_of_failed_in_comm_world);

  num_images_failed += num_failed_in_group;

  /* if (!no_stopped_images_check_in_errhandler) */
  /*   { */
  /*     int buffer, flag; */
  /*     MPI_Request req; */
  /*     MPI_Status request_status; */
  /*     dprint ("%d/%d: Checking for stopped images.\n", caf_this_image, */
  /*             caf_num_images); */
  /*     ierr = MPI_Irecv (&buffer, 1, MPI_INT, MPI_ANY_SOURCE, MPI_TAG_CAF_SYNC_IMAGES, */
  /*                       CAF_COMM_WORLD, &req); */
  /*     if (ierr == MPI_SUCCESS) */
  /*       { */
  /*         ierr = MPI_Test (&req, &flag, &request_status); */
  /*         if (flag) */
  /*           { */
  /*             // Received a result */
  /*             if (buffer == STAT_STOPPED_IMAGE) */
  /*               { */
  /*                 dprint ("%d/%d: Image #%d found stopped.\n", */
  /*                         caf_this_image, caf_num_images, request_status.MPI_SOURCE); */
  /*                 stopped = true; */
  /*                 if (image_stati[request_status.MPI_SOURCE] == 0) */
  /*                   ++num_images_stopped; */
  /*                 image_stati[request_status.MPI_SOURCE] = STAT_STOPPED_IMAGE; */
  /*               } */
  /*           } */
  /*         else */
  /*           { */
  /*             dprint ("%d/%d: No stopped images found.\n", */
  /*                     caf_this_image, caf_num_images); */
  /*             MPI_Cancel (&req); */
  /*           } */
  /*       } */
  /*     else */
  /*       { */
  /*         int err; */
  /*         MPI_Error_class (ierr, &err); */
  /*         dprint ("%d/%d: Error on checking for stopped images %d.\n", */
  /*                 caf_this_image, caf_num_images, err); */
  /*       } */
  /* 	} */

  /* /\* TODO: Consider whether removing the failed image from images_full will be */
  /*  * necessary. This is more or less politics. *\/ */
  /* for (i = 0; i < num_failed_in_group; ++i) */
  /*   { */
  /*     if (ranks_of_failed_in_comm_world[i] >= 0 */
  /*         && ranks_of_failed_in_comm_world[i] < caf_num_images) */
  /*       { */
  /*         if (image_stati[ranks_of_failed_in_comm_world[i]] == 0) */
  /*           image_stati[ranks_of_failed_in_comm_world[i]] = STAT_FAILED_IMAGE; */
  /*       } */
  /*     else */
  /*       { */
  /*         dprint ("%d/%d: Rank of failed image %d out of range of images 0..%d.\n", */
  /*                 caf_this_image, caf_num_images, ranks_of_failed_in_comm_world[i], */
  /*                 caf_num_images); */
  /*       } */
  /*   } */

redo:
  dprint ("%d/%d: %s: Before shrink. \n", caf_this_image, caf_num_images, __FUNCTION__);
  ierr = MPIX_Comm_shrink (*pcomm, &shrunk);
  dprint ("%d/%d: %s: After shrink, rc = %d.\n", caf_this_image, caf_num_images, __FUNCTION__, ierr);
  MPI_Comm_set_errhandler (shrunk, failed_stopped_CAF_COMM_WORLD_mpi_errorhandler);
  MPI_Comm_size (shrunk, &ns);
  MPI_Comm_rank (shrunk, &srank);

  MPI_Comm_rank (*pcomm, &crank);

  dprint ("%d/%d: %s: After getting ranks, ns = %d, srank = %d, crank = %d.\n",
	  caf_this_image, caf_num_images, __FUNCTION__, ns, srank, crank);

  /* Split does the magic: removing spare processes and reordering ranks
   * so that all surviving processes remain at their former place */
  rc = MPI_Comm_split (shrunk, crank < 0 ? MPI_UNDEFINED : 1, crank, &newcomm);
  MPI_Comm_rank (newcomm, &newrank);
  dprint ("%d/%d: %s: After split, rc = %d, rank = %d.\n", caf_this_image, caf_num_images, __FUNCTION__, rc, newrank);
  flag = (rc == MPI_SUCCESS);
  /* Split or some of the communications above may have failed if
   * new failures have disrupted the process: we need to
   * make sure we succeeded at all ranks, or retry until it works. */
  flag = MPIX_Comm_agree (newcomm, &flag);
  dprint ("%d/%d: %s: After agree, flag = %d.\n", caf_this_image, caf_num_images, __FUNCTION__, flag);

  MPI_Comm_rank (newcomm, &drank);
  dprint ("%d/%d: %s: After rank, drank = %d.\n", caf_this_image, caf_num_images, __FUNCTION__, drank);

  MPI_Comm_free (&shrunk);
  if (MPI_SUCCESS != flag) {
    if (MPI_SUCCESS == rc)
      MPI_Comm_free (&newcomm);
    goto redo;
  }

  {
    int cmpres;
    ierr = MPI_Comm_compare (*pcomm, CAF_COMM_WORLD, &cmpres);
    dprint ("%d/%d: %s: Comm_compare(*comm, CAF_COMM_WORLD, res = %d) = %d.\n", caf_this_image,
	   caf_num_images, __FUNCTION__, cmpres, ierr);
    ierr = MPI_Comm_compare (*pcomm, alive_comm, &cmpres);
    dprint ("%d/%d: %s: Comm_compare(*comm, alive_comm, res = %d) = %d.\n", caf_this_image,
           caf_num_images, __FUNCTION__, cmpres, ierr);
    if (cmpres == MPI_CONGRUENT)
      {
        MPI_Win_detach (*stat_tok, &img_status);
        dprint ("%d/%d: %s: detached win img_status.\n", caf_this_image, caf_num_images, __FUNCTION__);
        MPI_Win_free (stat_tok);
        dprint ("%d/%d: %s: freed win img_status.\n", caf_this_image, caf_num_images, __FUNCTION__);
        MPI_Win_create (&img_status, sizeof (int), 1, mpi_info_same_size, newcomm,
                        stat_tok);
        dprint ("%d/%d: %s: (re-)created win img_status.\n", caf_this_image, caf_num_images, __FUNCTION__);
        CAF_Win_lock_all (*stat_tok);
        dprint ("%d/%d: %s: Win_lock_all on img_status.\n", caf_this_image, caf_num_images, __FUNCTION__);
      }
  }
  /* Also free the old communicator before replacing it. */
  MPI_Comm_free (pcomm);
  *pcomm = newcomm;
  alive_comm = newcomm;
  *perr = stopped ? STAT_STOPPED_IMAGE : STAT_FAILED_IMAGE;
}
#endif

void mutex_lock(MPI_Win win, int image_index, int index, int *stat,
		int *acquired_lock, char *errmsg, int errmsg_len)
{
  const char msg[] = "Already locked";
#if MPI_VERSION >= 3
  int value = 0, compare = 0, newval = caf_this_image, ierr = 0, i = 0;
#ifdef WITH_FAILED_IMAGES
  int flag, check_failure = 100, zero = 0;
#endif

  if(stat != NULL)
    *stat = 0;

#ifdef WITH_FAILED_IMAGES
  MPI_Test(&alive_request, &flag, MPI_STATUS_IGNORE);
#endif

  locking_atomic_op (win, &value, newval, compare, image_index, index);

  if (value == caf_this_image && image_index == caf_this_image)
    goto stat_error;

  if(acquired_lock != NULL)
    {
      if(value == 0)
        *acquired_lock = 1;
      else
	*acquired_lock = 0;
      return;
    }

  while (value != 0)
    {
      ++i;
#ifdef WITH_FAILED_IMAGES
      if (i == check_failure)
        {
          i = 1;
          MPI_Test (&alive_request, &flag, MPI_STATUS_IGNORE);
        }
#endif

      locking_atomic_op(win, &value, newval, compare, image_index, index);
#ifdef WITH_FAILED_IMAGES
      if (image_stati[value] == STAT_FAILED_IMAGE)
        {
          CAF_Win_lock (MPI_LOCK_EXCLUSIVE, image_index - 1, win);
          /* MPI_Fetch_and_op(&zero, &newval, MPI_INT, image_index - 1, index * sizeof(int), MPI_REPLACE, win); */
          MPI_Compare_and_swap (&zero, &value, &newval, MPI_INT, image_index - 1, index * sizeof (int), win);
          CAF_Win_unlock (image_index - 1, win);
          break;
        }
#else
      usleep(caf_this_image * i);
#endif
    }

  if (stat)
    *stat = ierr;
  else if (ierr == STAT_FAILED_IMAGE)
    terminate_internal (ierr, 0);

  return;

stat_error:
  if (errmsg != NULL)
    {
      memset(errmsg,' ',errmsg_len);
      memcpy(errmsg, msg, MIN(errmsg_len,strlen(msg)));
    }

  if(stat != NULL)
    *stat = 99;
  else
    terminate_internal(99, 1);
#else // MPI_VERSION
#warning Locking for MPI-2 is not implemented
  printf ("Locking for MPI-2 is not supported, please update your MPI implementation\n");
#endif // MPI_VERSION
}

void mutex_unlock(MPI_Win win, int image_index, int index, int *stat,
		  char* errmsg, int errmsg_len)
{
  const char msg[] = "Variable is not locked";
  if(stat != NULL)
    *stat = 0;
#if MPI_VERSION >= 3
  int value=1, ierr = 0, newval = 0;
#ifdef WITH_FAILED_IMAGES
  int flag;

  MPI_Test(&alive_request, &flag, MPI_STATUS_IGNORE);
#endif

  CAF_Win_lock (MPI_LOCK_EXCLUSIVE, image_index-1, win);
  MPI_Fetch_and_op(&newval, &value, MPI_INT, image_index-1, index*sizeof(int), MPI_REPLACE, win);
  CAF_Win_unlock (image_index-1, win);

  /* Temporarily commented */
  /* if(value == 0) */
  /*   goto stat_error; */

  if(stat)
    *stat = ierr;
  else if(ierr == STAT_FAILED_IMAGE)
    terminate_internal (ierr, 0);

  return;

stat_error:
  if(errmsg != NULL)
    {
      memset(errmsg,' ',errmsg_len);
      memcpy(errmsg, msg, MIN(errmsg_len,strlen(msg)));
    }
  if(stat != NULL)
    *stat = 99;
  else
    terminate_internal(99, 1);
#else // MPI_VERSION
#warning Locking for MPI-2 is not implemented
  printf ("Locking for MPI-2 is not supported, please update your MPI implementation\n");
#endif // MPI_VERSION
}

/* Initialize coarray program.  This routine assumes that no other
   MPI initialization happened before. */

void
PREFIX (init) (int *argc, char ***argv)
{
#ifdef WITH_FAILED_IMAGES
  int flag;
#endif
  if (caf_num_images == 0)
    {
      int ierr = 0, i = 0, j = 0, rc;

      int is_init = 0, prior_thread_level = MPI_THREAD_SINGLE;
      MPI_Initialized (&is_init);

      if (is_init) {
          MPI_Query_thread (&prior_thread_level);
      }
#ifdef HELPER
      int prov_lev=0;
      if (is_init) {
          prov_lev = prior_thread_level;
          caf_owns_mpi = false;
      } else {
          MPI_Init_thread (argc, argv, MPI_THREAD_MULTIPLE, &prov_lev);
          caf_owns_mpi = true;
      }

      if (caf_this_image == 0 && MPI_THREAD_MULTIPLE != prov_lev)
        caf_runtime_error ("MPI_THREAD_MULTIPLE is not supported: %d", prov_lev);
#else
      if (is_init) {
          caf_owns_mpi = false;
      } else {
          MPI_Init (argc, argv);
          caf_owns_mpi = true;
      }
#endif
      if (unlikely ((ierr != MPI_SUCCESS)))
        caf_runtime_error ("Failure when initializing MPI: %d", ierr);

      /* Duplicate MPI_COMM_WORLD so that no CAF internal functions
         use it - this is critical for MPI-interoperability. */
      rc = MPI_Comm_dup (MPI_COMM_WORLD, &CAF_COMM_WORLD);
#ifdef WITH_FAILED_IMAGES
      flag = (MPI_SUCCESS == rc);
      rc = MPIX_Comm_agree (MPI_COMM_WORLD, &flag);
      if (rc != MPI_SUCCESS) {
          dprint ("%d/%d: %s: MPIX_Comm_agree(flag = %d) = %d.\n",
                   caf_this_image, caf_num_images, __FUNCTION__, flag, rc);
          fflush (stderr);
        MPI_Abort (MPI_COMM_WORLD, 10000);
        }
      MPI_Barrier (MPI_COMM_WORLD);
#endif

      MPI_Comm_size (CAF_COMM_WORLD, &caf_num_images);
      MPI_Comm_rank (CAF_COMM_WORLD, &caf_this_image);

      ++caf_this_image;
      caf_is_finalized = 0;

      /* BEGIN SYNC IMAGE preparation
       * Prepare memory for syncing images.  */
      images_full = (int *) calloc (caf_num_images-1, sizeof (int));
      for (i = 1, j = 0; i <= caf_num_images; ++i)
        if (i != caf_this_image)
          images_full[j++] = i;

      arrived = calloc (caf_num_images, sizeof (int));
      sync_handles = malloc (caf_num_images * sizeof (MPI_Request));
      /* END SYNC IMAGE preparation.  */

      stat_tok = malloc (sizeof (MPI_Win));

#ifdef WITH_FAILED_IMAGES
      MPI_Comm_dup (MPI_COMM_WORLD, &alive_comm);
      /* Handling of failed/stopped images is done by setting an error handler
       * on a asynchronous request to each other image.  For a failing image
       * the request will trigger the call of the error handler thus allowing
       * each other image to handle the failed/stopped image.  */
      MPI_Comm_create_errhandler (failed_stopped_errorhandler_function,
                               &failed_stopped_CAF_COMM_WORLD_mpi_errorhandler);
      MPI_Comm_set_errhandler (CAF_COMM_WORLD,
                               failed_stopped_CAF_COMM_WORLD_mpi_errorhandler);
      MPI_Comm_set_errhandler (alive_comm,
                               failed_stopped_CAF_COMM_WORLD_mpi_errorhandler);
      MPI_Comm_set_errhandler (MPI_COMM_WORLD, MPI_ERRORS_RETURN);

      MPI_Irecv (&alive_dummy, 1, MPI_INT, MPI_ANY_SOURCE, MPI_ANY_TAG,
                 alive_comm, &alive_request);

      image_stati = (int *) calloc (caf_num_images, sizeof (int));
#endif

#if MPI_VERSION >= 3
      MPI_Info_create (&mpi_info_same_size);
      MPI_Info_set (mpi_info_same_size, "same_size", "true");

      /* Setting img_status */
      MPI_Win_create (&img_status, sizeof(int), 1, mpi_info_same_size, CAF_COMM_WORLD, stat_tok);
      CAF_Win_lock_all (*stat_tok);
#else
      MPI_Win_create (&img_status, sizeof(int), 1, MPI_INFO_NULL, CAF_COMM_WORLD, stat_tok);
#endif // MPI_VERSION

      /* Create the dynamic window to allow images to asyncronously attach
       * memory. */
      MPI_Win_create_dynamic (MPI_INFO_NULL, CAF_COMM_WORLD, &global_dynamic_win);
      CAF_Win_lock_all (global_dynamic_win);
    }
}


/* Internal finalize of coarray program.   */

void
finalize_internal (int status_code)
{
  dprint ("%d/%d: %s(status_code = %d)\n",
	  caf_this_image, caf_num_images, __FUNCTION__, status_code);

#ifdef WITH_FAILED_IMAGES
  no_stopped_images_check_in_errhandler = true;
  MPI_Win_flush_all (*stat_tok);
#endif
  /* For future security enclose setting img_status in a lock.  */
  CAF_Win_lock (MPI_LOCK_EXCLUSIVE, caf_this_image - 1, *stat_tok);
  if (status_code == 0)
    {
      img_status = STAT_STOPPED_IMAGE;
#ifdef WITH_FAILED_IMAGES
      image_stati[caf_this_image - 1] = STAT_STOPPED_IMAGE;
#endif
    }
  else
    {
      img_status = status_code;
#ifdef WITH_FAILED_IMAGES
      image_stati[caf_this_image - 1] = status_code;
#endif
    }
  CAF_Win_unlock (caf_this_image - 1, *stat_tok);

  /* Announce to all other images, that this one has changed its execution
   * status.  */
  for (int i = 0; i < caf_num_images - 1; ++i)
      MPI_Send (&img_status, 1, MPI_INT, images_full[i] - 1,
                MPI_TAG_CAF_SYNC_IMAGES, CAF_COMM_WORLD);

#ifdef WITH_FAILED_IMAGES
  /* Terminate the async request before revoking the comm, or we will get
   * triggered by the errorhandler, which we don't want here anymore.  */
  MPI_Cancel (&alive_request);

  if (status_code == 0) {
      /* In finalization do not report stopped or failed images any more. */
      MPI_Errhandler_set (CAF_COMM_WORLD, MPI_ERRORS_RETURN);
      MPI_Errhandler_set (alive_comm, MPI_ERRORS_RETURN);
      /* Only add a conventional barrier to prevent images from quitting to early,
       * when this images is not failing.  */
      dprint ("%d/%d: %s: Before MPI_Barrier (CAF_COMM_WORLD)\n",
              caf_this_image, caf_num_images, __FUNCTION__);
      int ierr = MPI_Barrier (CAF_COMM_WORLD);
      dprint ("%d/%d: %s: After MPI_Barrier (CAF_COMM_WORLD) = %d\n",
              caf_this_image, caf_num_images, __FUNCTION__, ierr);
    }
  else
    return;
#else
  /* Add a conventional barrier to prevent images from quitting to early.  */
  if (status_code == 0)
    MPI_Barrier (CAF_COMM_WORLD);
  else
    /* Without failed images support, but a given status_code, we need to return
     * to the caller, or we will hang in the following instead of terminating the
     * program. */
    return;
#endif

#ifdef GCC_GE_7
  struct caf_allocated_slave_tokens_t *cur_stok = caf_allocated_slave_tokens,
      *prev_stok = NULL;
  CAF_Win_unlock_all (global_dynamic_win);
  while (cur_stok)
    {
      prev_stok = cur_stok->prev;
      MPI_Win_detach (global_dynamic_win, cur_stok);
      if (cur_stok->token->memptr)
        {
          MPI_Win_detach (global_dynamic_win, cur_stok->token->memptr);
          free (cur_stok->token->memptr);
        }
      free (cur_stok->token);
      free (cur_stok);
      cur_stok = prev_stok;
    }
#else
  CAF_Win_unlock_all (global_dynamic_win);
#endif

  dprint ("%d/%d: finalize(): Freeed all slave tokens.\n", caf_this_image,
          caf_num_images);
  struct caf_allocated_tokens_t *cur_tok = caf_allocated_tokens, *prev = caf_allocated_tokens;
  MPI_Win *p;

  while(cur_tok)
    {
      prev = cur_tok->prev;
      p = TOKEN(cur_tok->token);
      if (p != NULL)
        CAF_Win_unlock_all (*p);
#ifdef GCC_GE_7
      /* Unregister the window to the descriptors when freeing the token.  */
      dprint ("%d/%d: MPI_Win_free (p);\n", caf_this_image,
              caf_num_images);
      MPI_Win_free (p);
      free (cur_tok->token);
#else // GCC_GE_7
      MPI_Win_free (p);
#endif // GCC_GE_7
      free (cur_tok);
      cur_tok = prev;
    }
#if MPI_VERSION >= 3
  MPI_Info_free (&mpi_info_same_size);
#endif // MPI_VERSION

  /* Free the global dynamic window. */
  MPI_Win_free (&global_dynamic_win);
#ifdef WITH_FAILED_IMAGES
  if (status_code == 0)
    {
      dprint ("%d/%d: %s: before Win_unlock_all.\n",
              caf_this_image, caf_num_images, __FUNCTION__);
      CAF_Win_unlock_all (*stat_tok);
      dprint ("%d/%d: %s: before Win_free(stat_tok)\n",
              caf_this_image, caf_num_images, __FUNCTION__);
      MPI_Win_free (stat_tok);
      dprint ("%d/%d: %s: before Comm_free(CAF_COMM_WORLD)\n",
              caf_this_image, caf_num_images, __FUNCTION__);
      MPI_Comm_free (&CAF_COMM_WORLD);
      MPI_Comm_free (&alive_comm);
      dprint ("%d/%d: %s: after Comm_free(CAF_COMM_WORLD)\n",
              caf_this_image, caf_num_images, __FUNCTION__);
    }

  MPI_Errhandler_free (&failed_stopped_CAF_COMM_WORLD_mpi_errorhandler);

  /* Only call Finalize if CAF runtime Initialized MPI. */
  if (caf_owns_mpi)
    MPI_Finalize ();
#else
  MPI_Comm_free (&CAF_COMM_WORLD);

  CAF_Win_unlock_all (*stat_tok);
  MPI_Win_free (stat_tok);

  /* Only call Finalize if CAF runtime Initialized MPI. */
  if (caf_owns_mpi)
    MPI_Finalize ();
#endif

  pthread_mutex_lock (&lock_am);
  caf_is_finalized = 1;
  pthread_mutex_unlock (&lock_am);
  free (sync_handles);
  dprint ("%d/%d: %s: Finalisation done!!!\n", caf_this_image, caf_num_images,
         __FUNCTION__);
}


/* Finalize coarray program.  */

void
PREFIX (finalize) (void)
{
  finalize_internal (0);
}

/* TODO: This is interface is violating the F2015 standard, but not the gfortran
 * API. Fix it (the fortran API). */
int
PREFIX (this_image) (int distance __attribute__ ((unused)))
{
  return caf_this_image;
}

/* TODO: This is interface is violating the F2015 standard, but not the gfortran
 * API. Fix it (the fortran API). */
int
PREFIX (num_images) (int distance __attribute__ ((unused)),
                     int failed __attribute__ ((unused)))
{
  return caf_num_images;
}

#ifdef GCC_GE_7
/** Register an object with the coarray library creating a token where
    necessary/requested.

    See the ABI-documentation of gfortran for the expected behavior.
    Contrary to this expected behavior is this routine not registering memory
    in the descriptor, that is already present.  I.e., when the compiler
    expects the library to allocate the memory for an object in desc, then
    its data_ptr is NULL. This is still missing here.  At the moment the
    compiler also does not make use of it, but it is contrary to the
    documentation.
    */
void
PREFIX (register) (size_t size, caf_register_t type, caf_token_t *token,
                   gfc_descriptor_t *desc, int *stat, char *errmsg,
		   int errmsg_len)
{
  /* int ierr; */
  void *mem = NULL;
  size_t actual_size;
  int l_var=0, *init_array=NULL;

  if (unlikely (caf_is_finalized))
    goto error;

  /* Start GASNET if not already started.  */
  if (caf_num_images == 0)
    PREFIX (init) (NULL, NULL);

  if(type == CAF_REGTYPE_LOCK_STATIC || type == CAF_REGTYPE_LOCK_ALLOC ||
     type == CAF_REGTYPE_CRITICAL || type == CAF_REGTYPE_EVENT_STATIC ||
     type == CAF_REGTYPE_EVENT_ALLOC)
    {
      actual_size = size * sizeof(int);
      l_var = 1;
    }
  else
    actual_size = size;

  switch (type)
    {
    case CAF_REGTYPE_COARRAY_ALLOC_REGISTER_ONLY:
    case CAF_REGTYPE_COARRAY_ALLOC_ALLOCATE_ONLY:
      {
        /* Create or allocate a slave token. */
        mpi_caf_slave_token_t *slave_token;
        MPI_Aint mpi_address;
        CAF_Win_unlock_all (global_dynamic_win);
        if (type == CAF_REGTYPE_COARRAY_ALLOC_REGISTER_ONLY)
          {
            *token = calloc (1, sizeof(mpi_caf_slave_token_t));
            slave_token = (mpi_caf_slave_token_t *)(*token);
            MPI_Win_attach (global_dynamic_win, *token,
                            sizeof (mpi_caf_slave_token_t));
            MPI_Get_address(*token, &mpi_address);
            dprint ("%d/%d: Attach slave token %p (mpi-address: %p) to global_dynamic_window = %p\n",
                    caf_this_image, caf_num_images, slave_token, mpi_address,
                    global_dynamic_win);

            /* Register the memory for auto freeing. */
            struct caf_allocated_slave_tokens_t *tmp =
                malloc (sizeof (struct caf_allocated_slave_tokens_t));
            tmp->prev  = caf_allocated_slave_tokens;
            tmp->token = *token;
            caf_allocated_slave_tokens = tmp;
          }
        else // (type == CAF_REGTYPE_COARRAY_ALLOC_ALLOCATE_ONLY)
          {
            int ierr;
            slave_token = (mpi_caf_slave_token_t *)(*token);
            mem = malloc (actual_size);
            slave_token->memptr = mem;
            ierr = MPI_Win_attach (global_dynamic_win, mem, actual_size);
            MPI_Get_address(mem, &mpi_address);
            dprint ("%d/%d: Attach mem %p (mpi-address: %p) to global_dynamic_window = %p on slave_token %p, ierr: %d\n",
                    caf_this_image, caf_num_images, mem, mpi_address,
                    global_dynamic_win, slave_token, ierr);
            if (desc != NULL && GFC_DESCRIPTOR_RANK (desc) != 0)
              {
                slave_token->desc = desc;
                MPI_Get_address (desc, &mpi_address);
                dprint ("%d/%d: Attached descriptor %p (mpi-address: %p) to global_dynamic_window %p at address %p, ierr = %d.\n",
                        caf_this_image, caf_num_images, desc, mpi_address,
                        global_dynamic_win, &slave_token->desc, ierr);
              }
          }
        CAF_Win_lock_all (global_dynamic_win);
        dprint ("%d/%d: Slave token %p on exit: mpi_caf_slave_token_t { desc: %p }\n",
                caf_this_image, caf_num_images, slave_token, slave_token->desc);
      }
      break;
    default:
      {
        mpi_caf_token_t *mpi_token;
        MPI_Win *p;

        *token = calloc (1, sizeof (mpi_caf_token_t));
        mpi_token = (mpi_caf_token_t *) (*token);
        p = TOKEN (mpi_token);

#if MPI_VERSION >= 3
        MPI_Win_allocate (actual_size, 1, MPI_INFO_NULL, CAF_COMM_WORLD, &mem, p);
        CAF_Win_lock_all (*p);
#else // MPI_VERSION
        MPI_Alloc_mem(actual_size, MPI_INFO_NULL, &mem);
        MPI_Win_create(mem, actual_size, 1, MPI_INFO_NULL, CAF_COMM_WORLD, p);
#endif // MPI_VERSION
        if (GFC_DESCRIPTOR_RANK (desc) != 0)
          mpi_token->desc = desc;

        if(l_var)
          {
            init_array = (int *)calloc(size, sizeof(int));
            CAF_Win_lock (MPI_LOCK_EXCLUSIVE, caf_this_image - 1, *p);
            MPI_Put (init_array, size, MPI_INT, caf_this_image-1,
                            0, size, MPI_INT, *p);
            CAF_Win_unlock (caf_this_image - 1, *p);
            free(init_array);
          }

        struct caf_allocated_tokens_t *tmp =
            malloc (sizeof (struct caf_allocated_tokens_t));
        tmp->prev  = caf_allocated_tokens;
        tmp->token = *token;
        caf_allocated_tokens = tmp;

        if (stat)
          *stat = 0;

        /* The descriptor will be initialized only after the call to register.  */
        mpi_token->memptr = mem;
        dprint ("%d/%d: Token %p on exit: mpi_caf_token_t { (local_)memptr: %p, memptr_win: %p  }\n",
                caf_this_image, caf_num_images, mpi_token, mpi_token->memptr,
                mpi_token->memptr_win);
      } // default:
      break;
    } // switch

  desc->base_addr = mem;
  return;

error:
  {
    char *msg;

    if (caf_is_finalized)
      msg = "Failed to allocate coarray - there are stopped images";
    else
      msg = "Failed to allocate coarray";

    if (stat)
      {
        *stat = caf_is_finalized ? STAT_STOPPED_IMAGE : 1;
        if (errmsg_len > 0)
          {
            int len = ((int) strlen (msg) > errmsg_len) ? errmsg_len
                                                        : (int) strlen (msg);
            memcpy (errmsg, msg, len);
            if (errmsg_len > len)
              memset (&errmsg[len], ' ', errmsg_len-len);
          }
      }
    else
      caf_runtime_error (msg);
  }
}
#else // GCC_GE_7
void *
PREFIX (register) (size_t size, caf_register_t type, caf_token_t *token,
                   int *stat, char *errmsg, int errmsg_len)
{
  /* int ierr; */
  void *mem;
  size_t actual_size;
  int l_var=0, *init_array = NULL;

  if (unlikely (caf_is_finalized))
    goto error;

  /* Start GASNET if not already started.  */
  if (caf_num_images == 0)
#ifdef COMPILER_SUPPORTS_CAF_INTRINSICS
    _gfortran_caf_init (NULL, NULL);
#else
    PREFIX (init) (NULL, NULL);
#endif

  /* Token contains only a list of pointers.  */
  *token = malloc (sizeof(MPI_Win));
  MPI_Win *p = *token;

  if(type == CAF_REGTYPE_LOCK_STATIC || type == CAF_REGTYPE_LOCK_ALLOC ||
     type == CAF_REGTYPE_CRITICAL || type == CAF_REGTYPE_EVENT_STATIC ||
     type == CAF_REGTYPE_EVENT_ALLOC)
    {
      actual_size = size*sizeof(int);
      l_var = 1;
    }
  else
    actual_size = size;

#if MPI_VERSION >= 3
  MPI_Win_allocate(actual_size, 1, mpi_info_same_size, CAF_COMM_WORLD, &mem, p);
  CAF_Win_lock_all (*p);
#else // MPI_VERSION
  MPI_Alloc_mem(actual_size, MPI_INFO_NULL, &mem);
  MPI_Win_create(mem, actual_size, 1, MPI_INFO_NULL, CAF_COMM_WORLD, p);
#endif // MPI_VERSION

  if(l_var)
    {
      init_array = (int *)calloc(size, sizeof(int));
      CAF_Win_lock(MPI_LOCK_EXCLUSIVE, caf_this_image-1, *p);
      MPI_Put (init_array, size, MPI_INT, caf_this_image-1,
               0, size, MPI_INT, *p);
      CAF_Win_unlock(caf_this_image - 1, *p);
      free(init_array);
    }

  PREFIX(sync_all) (NULL, NULL, 0);

  struct caf_allocated_tokens_t *tmp = malloc (sizeof (struct caf_allocated_tokens_t));
  tmp->prev  = caf_allocated_tokens;
  tmp->token = *token;
  caf_allocated_tokens = tmp;

  if (stat)
    *stat = 0;
  return mem;

error:
  {
    char *msg;

    if (caf_is_finalized)
      msg = "Failed to allocate coarray - there are stopped images";
    else
      msg = "Failed to allocate coarray";

    if (stat)
      {
        *stat = caf_is_finalized ? STAT_STOPPED_IMAGE : 1;
        if (errmsg_len > 0)
          {
            int len = ((int) strlen (msg) > errmsg_len) ? errmsg_len
                                                        : (int) strlen (msg);
            memcpy (errmsg, msg, len);
            if (errmsg_len > len)
              memset (&errmsg[len], ' ', errmsg_len-len);
          }
      }
    else
      caf_runtime_error (msg);
  }
  return NULL;
}
#endif


#ifdef GCC_GE_7
void
PREFIX (deregister) (caf_token_t *token, int type, int *stat, char *errmsg,
		     int errmsg_len)
#else
void
PREFIX (deregister) (caf_token_t *token, int *stat, char *errmsg, int errmsg_len)
#endif
{
  dprint ("%d/%d: deregister(%p)\n", caf_this_image, caf_num_images, *token);

  if (unlikely (caf_is_finalized))
    {
      const char msg[] = "Failed to deallocate coarray - "
                          "there are stopped images";
      if (stat)
        {
          *stat = STAT_STOPPED_IMAGE;

          if (errmsg_len > 0)
            {
              int len = ((int) sizeof (msg) - 1 > errmsg_len)
                        ? errmsg_len : (int) sizeof (msg) - 1;
              memcpy (errmsg, msg, len);
              if (errmsg_len > len)
                memset (&errmsg[len], ' ', errmsg_len-len);
            }
          return;
        }
      caf_runtime_error (msg);
    }

  if (stat)
    *stat = 0;

#ifdef GCC_GE_7
  if (type != CAF_DEREGTYPE_COARRAY_DEALLOCATE_ONLY)
    {
      /* Sync all images only, when deregistering the token. Just freeing the
       * memory needs no sync. */
#ifdef WITH_FAILED_IMAGES
      MPI_Barrier (CAF_COMM_WORLD);
#else
      PREFIX (sync_all) (NULL, NULL, 0);
#endif
    }
#endif

  {
    struct caf_allocated_tokens_t *cur = caf_allocated_tokens, *prev,
        *next = caf_allocated_tokens;
    MPI_Win *p;

    while (cur)
      {
        prev = cur->prev;

        if (cur->token == *token)
          {
            p = TOKEN(*token);
#ifdef GCC_GE_7
            dprint ("%d/%d: Found regular token %p for memptr_win: %p.\n",
                    caf_this_image, caf_num_images, *token,
                    ((mpi_caf_token_t *)*token)->memptr_win);
#endif
            CAF_Win_unlock_all (*p);
            MPI_Win_free (p);

            if (prev)
              next->prev = prev->prev;
            else
              next->prev = NULL;

            if (cur == caf_allocated_tokens)
              caf_allocated_tokens = prev;

            free (cur);
            free (*token);
            return;
          }

        next = cur;
        cur = prev;
      }
  }

#ifdef GCC_GE_7
  /* Feel through: Has to be a component token. */
  {
    struct caf_allocated_slave_tokens_t *cur_stok = caf_allocated_slave_tokens,
        *prev_stok, *next_stok = caf_allocated_slave_tokens;

    while (cur_stok)
      {
        prev_stok = cur_stok->prev;

        if (cur_stok->token == *token)
          {
            dprint ("%d/%d: Found sub token %p.\n",
                    caf_this_image, caf_num_images, *token);

            mpi_caf_slave_token_t *slave_token = *(mpi_caf_slave_token_t **)token;
            CAF_Win_unlock_all (global_dynamic_win);

            if (slave_token->memptr)
              {
                MPI_Win_detach (global_dynamic_win, slave_token->memptr);
                free (slave_token->memptr);
                slave_token->memptr = NULL;
                if (type == CAF_DEREGTYPE_COARRAY_DEALLOCATE_ONLY)
                  {
                    CAF_Win_lock_all (global_dynamic_win);
                    return; // All done.
                  }
              }
            MPI_Win_detach (global_dynamic_win, slave_token);
            CAF_Win_lock_all (global_dynamic_win);

            if (prev_stok)
              next_stok->prev = prev_stok->prev;
            else
              next_stok->prev = NULL;

            if (cur_stok == caf_allocated_slave_tokens)
              caf_allocated_slave_tokens = prev_stok;

            free (cur_stok);
            free (*token);
            return;
          }

        next_stok = cur_stok;
        cur_stok = prev_stok;
      }
  }
#endif
#ifdef EXTRA_DEBUG_OUTPUT
  fprintf (stderr, "Fortran runtime warning on image %d: Could not find token to free %p",
           caf_this_image, *token);
#endif
}

void
PREFIX (sync_memory) (int *stat __attribute__ ((unused)),
                      char *errmsg __attribute__ ((unused)),
                      int errmsg_len __attribute__ ((unused)))
{
#if defined(NONBLOCKING_PUT) && !defined(CAF_MPI_LOCK_UNLOCK)
  explicit_flush ();
#endif
}


void
PREFIX (sync_all) (int *stat, char *errmsg, int errmsg_len)
{
  int ierr = 0;

  dprint ("%d/%d: Entering sync all.\n", caf_this_image, caf_num_images);
  if (unlikely (caf_is_finalized))
    ierr = STAT_STOPPED_IMAGE;
  else
    {
      int mpi_err;
#if defined(NONBLOCKING_PUT) && !defined(CAF_MPI_LOCK_UNLOCK)
      explicit_flush();
#endif

#ifdef WITH_FAILED_IMAGES
      mpi_err = MPI_Barrier (alive_comm);
#else
      mpi_err = MPI_Barrier (CAF_COMM_WORLD);
#endif
      dprint ("%d/%d: %s: MPI_Barrier = %d.\n", caf_this_image, caf_num_images,
             __FUNCTION__, mpi_err);
      if (mpi_err == STAT_FAILED_IMAGE)
        ierr = STAT_FAILED_IMAGE;
      else if (mpi_err != 0)
        MPI_Error_class (mpi_err, &ierr);
    }

  if (stat != NULL)
    *stat = ierr;
#ifdef WITH_FAILED_IMAGES
  else if (ierr == STAT_FAILED_IMAGE)
    /* F2015 requests stat to be set for FAILED IMAGES, else error out. */
    terminate_internal (ierr, 0);
#endif

  /* if (ierr != 0 && ierr != STAT_FAILED_IMAGE) */
  /*   { */
  /*     char *msg; */
  /*     if (caf_is_finalized) */
  /*       msg = "SYNC ALL failed - there are stopped images"; */
  /*     else */
  /*       msg = "SYNC ALL failed"; */

  /*     if (errmsg_len > 0) */
  /*       { */
  /*         int len = ((int) strlen (msg) > errmsg_len) ? errmsg_len */
  /*                                                     : (int) strlen (msg); */
  /*         memcpy (errmsg, msg, len); */
  /*         if (errmsg_len > len) */
  /*           memset (&errmsg[len], ' ', errmsg_len-len); */
  /*       } */
  /*     else if (stat == NULL) */
  /*       caf_runtime_error (msg); */
  /*   } */
  dprint ("%d/%d: Leaving sync all.\n", caf_this_image, caf_num_images);
}

/* token: The token of the array to be written to. */
/* offset: Difference between the coarray base address and the actual data, used for caf(3)[2] = 8 or caf[4]%a(4)%b = 7. */
/* image_index: Index of the coarray (typically remote, though it can also be on this_image). */
/* data: Pointer to the to-be-transferred data. */
/* size: The number of bytes to be transferred. */
/* asynchronous: Return before the data transfer has been complete  */

static void
selectType (int size, MPI_Datatype *dt)
{
  int t_s;

  MPI_Type_size (MPI_INT, &t_s);

  if (t_s == size)
    {
      *dt = MPI_INT;
      return;
    }

  MPI_Type_size (MPI_DOUBLE, &t_s);

  if (t_s == size)
    {
      *dt = MPI_DOUBLE;
      return;
    }

  MPI_Type_size (MPI_COMPLEX, &t_s);

  if (t_s == size)
    {
      *dt = MPI_COMPLEX;
      return;
    }

  MPI_Type_size (MPI_DOUBLE_COMPLEX, &t_s);

  if (t_s == size)
    {
      *dt = MPI_DOUBLE_COMPLEX;
      return;
    }
}

void
PREFIX (sendget) (caf_token_t token_s, size_t offset_s, int image_index_s,
                  gfc_descriptor_t *dest,
                  caf_vector_t *dst_vector __attribute__ ((unused)),
                  caf_token_t token_g, size_t offset_g,
                  int image_index_g, gfc_descriptor_t *src ,
                  caf_vector_t *src_vector __attribute__ ((unused)),
                  int src_kind, int dst_kind, bool mrt, int *stat)
{
  int ierr = 0;
  size_t i, size;
  int j;
  int rank = GFC_DESCRIPTOR_RANK (dest);
  MPI_Win *p_s = TOKEN(token_s), *p_g = TOKEN(token_g);
  ptrdiff_t dst_offset = 0;
  ptrdiff_t src_offset = 0;
  void *pad_str = NULL;
  size_t src_size = GFC_DESCRIPTOR_SIZE (src);
  size_t dst_size = GFC_DESCRIPTOR_SIZE (dest);
  char *tmp;

  size = 1;
  for (j = 0; j < rank; j++)
    {
      ptrdiff_t dimextent = dest->dim[j]._ubound - dest->dim[j].lower_bound + 1;
      if (dimextent < 0)
        dimextent = 0;
      size *= dimextent;
    }

  if (size == 0)
    return;

  check_image_health (image_index_s, stat);
  check_image_health (image_index_g, stat);

  if (rank == 0
      || (GFC_DESCRIPTOR_TYPE (dest) == GFC_DESCRIPTOR_TYPE (src)
          && dst_kind == src_kind && GFC_DESCRIPTOR_RANK (src) != 0
          && (GFC_DESCRIPTOR_TYPE (dest) != BT_CHARACTER || dst_size == src_size)
          && PREFIX (is_contiguous) (dest) && PREFIX (is_contiguous) (src)))
    {
      tmp = (char *) calloc (size, dst_size);

      CAF_Win_lock (MPI_LOCK_SHARED, image_index_g - 1, *p_g);
      ierr = MPI_Get (tmp, dst_size*size, MPI_BYTE,
                      image_index_g-1, offset_g, dst_size*size, MPI_BYTE, *p_g);
      if (pad_str)
        memcpy ((char *) tmp + src_size, pad_str,
                dst_size-src_size);
      CAF_Win_unlock (image_index_g-1, *p_g);

      CAF_Win_lock (MPI_LOCK_EXCLUSIVE, image_index_s - 1, *p_s);
      if (GFC_DESCRIPTOR_TYPE (dest) == GFC_DESCRIPTOR_TYPE (src)
          && dst_kind == src_kind)
        ierr = MPI_Put (tmp, dst_size*size, MPI_BYTE,
                        image_index_s-1, offset_s,
                        (dst_size > src_size ? src_size : dst_size) * size,
                        MPI_BYTE, *p_s);
      if (pad_str)
        ierr = MPI_Put (pad_str, dst_size-src_size, MPI_BYTE, image_index_s-1,
                        offset_s, dst_size - src_size, MPI_BYTE, *p_s);
      CAF_Win_unlock (image_index_s - 1, *p_s);

      if (ierr != 0)
        terminate_internal (ierr, 0);
      return;

      free(tmp);
    }
  else
    {
      tmp = calloc(1, dst_size);

      for (i = 0; i < size; i++)
        {
          ptrdiff_t array_offset_dst = 0;
          ptrdiff_t stride = 1;
          ptrdiff_t extent = 1;
	  ptrdiff_t tot_ext = 1;
          for (j = 0; j < rank-1; j++)
            {
              array_offset_dst += ((i / tot_ext)
                                   % (dest->dim[j]._ubound
                                      - dest->dim[j].lower_bound + 1))
                * dest->dim[j]._stride;
              extent = (dest->dim[j]._ubound - dest->dim[j].lower_bound + 1);
              stride = dest->dim[j]._stride;
	      tot_ext *= extent;
            }

	  array_offset_dst += (i / tot_ext) * dest->dim[rank-1]._stride;
          dst_offset = offset_s + array_offset_dst*GFC_DESCRIPTOR_SIZE (dest);

          ptrdiff_t array_offset_sr = 0;
          if (GFC_DESCRIPTOR_RANK (src) != 0)
            {
              stride = 1;
              extent = 1;
	      tot_ext = 1;
              for (j = 0; j < GFC_DESCRIPTOR_RANK (src)-1; j++)
                {
                  array_offset_sr += ((i / tot_ext)
                                      % (src->dim[j]._ubound
                                         - src->dim[j].lower_bound + 1))
                    * src->dim[j]._stride;
                  extent = (src->dim[j]._ubound - src->dim[j].lower_bound + 1);
                  stride = src->dim[j]._stride;
		  tot_ext *= extent;
                }

              array_offset_sr += (i / tot_ext) * src->dim[rank-1]._stride;
              array_offset_sr *= GFC_DESCRIPTOR_SIZE (src);
            }
          src_offset = offset_g + array_offset_sr;

          CAF_Win_lock (MPI_LOCK_SHARED, image_index_g - 1, *p_g);
          ierr = MPI_Get (tmp, dst_size, MPI_BYTE,
                          image_index_g-1, src_offset, src_size, MPI_BYTE, *p_g);
          CAF_Win_unlock (image_index_g - 1, *p_g);

          CAF_Win_lock (MPI_LOCK_EXCLUSIVE, image_index_s - 1, *p_s);
          ierr = MPI_Put (tmp, GFC_DESCRIPTOR_SIZE (dest), MPI_BYTE, image_index_s-1,
                          dst_offset, GFC_DESCRIPTOR_SIZE (dest), MPI_BYTE, *p_s);
          if (pad_str)
            ierr = MPI_Put (pad_str, dst_size - src_size, MPI_BYTE, image_index_s-1,
                            dst_offset, dst_size - src_size, MPI_BYTE, *p_s);
          CAF_Win_unlock (image_index_s - 1, *p_s);

          if (ierr != 0)
            {
              terminate_internal (ierr, 0);
              return;
            }
        }
      free(tmp);
    }

}


/* Send array data from src to dest on a remote image.  */
/* The last argument means may_require_temporary */

void
PREFIX (send) (caf_token_t token, size_t offset, int image_index,
               gfc_descriptor_t *dest,
               caf_vector_t *dst_vector __attribute__ ((unused)),
               gfc_descriptor_t *src, int dst_kind, int src_kind,
               bool mrt, int *stat)
{
  /* FIXME: Implement vector subscripts, type conversion and check whether
     string-kind conversions are permitted.
     FIXME: Implement sendget as well.  */
  int ierr = 0, flag = 0;
  size_t i, size;
  int j;
  /* int position, msg = 0;  */
  int rank = GFC_DESCRIPTOR_RANK (dest);
  MPI_Win *p = TOKEN(token);
  ptrdiff_t dst_offset = 0;
  void *pad_str = NULL;
  void *t_buff = NULL;
  bool *buff_map = NULL;
  size_t src_size = GFC_DESCRIPTOR_SIZE (src);
  size_t dst_size = GFC_DESCRIPTOR_SIZE (dest);

  size = 1;
  for (j = 0; j < rank; j++)
    {
      ptrdiff_t dimextent = dest->dim[j]._ubound - dest->dim[j].lower_bound + 1;
      if (dimextent < 0)
        dimextent = 0;
      size *= dimextent;
    }

  if (size == 0)
    return;

  check_image_health(image_index, stat);

  if (GFC_DESCRIPTOR_TYPE (dest) == BT_CHARACTER && dst_size > src_size)
    {
      pad_str = alloca (dst_size - src_size);
      if (dst_kind == 1)
        memset (pad_str, ' ', dst_size-src_size);
      else /* dst_kind == 4.  */
        for (i = 0; i < (dst_size-src_size)/4; i++)
              ((int32_t*) pad_str)[i] = (int32_t) ' ';
    }
  if (rank == 0
      || (GFC_DESCRIPTOR_TYPE (dest) == GFC_DESCRIPTOR_TYPE (src)
          && dst_kind == src_kind && GFC_DESCRIPTOR_RANK (src) != 0
          && (GFC_DESCRIPTOR_TYPE (dest) != BT_CHARACTER || dst_size == src_size)
          && PREFIX (is_contiguous) (dest) && PREFIX (is_contiguous) (src)))
    {
      if(caf_this_image == image_index)
        {
          /* The address of source passed by the compiler points on the right
           * memory location. No offset summation is needed.  */
          void *dest_tmp = (void *) ((char *) dest->base_addr);// + offset);
          memmove (dest_tmp,src->base_addr,size*dst_size);
          return;
        }
      else
        {
          CAF_Win_lock (MPI_LOCK_EXCLUSIVE, image_index - 1, *p);
          if (GFC_DESCRIPTOR_TYPE (dest) == GFC_DESCRIPTOR_TYPE (src)
              && dst_kind == src_kind)
            ierr = MPI_Put (src->base_addr, (dst_size > src_size ? src_size : dst_size)*size, MPI_BYTE,
                            image_index-1, offset,
                            (dst_size > src_size ? src_size : dst_size) * size,
                            MPI_BYTE, *p);
          if (pad_str)
	    {
	      size_t newoff = offset + (dst_size > src_size ? src_size : dst_size) * size;
	      ierr = MPI_Put (pad_str, dst_size-src_size, MPI_BYTE, image_index-1,
			      newoff, dst_size - src_size, MPI_BYTE, *p);
	    }
#ifdef CAF_MPI_LOCK_UNLOCK
          MPI_Win_unlock (image_index-1, *p);
#elif NONBLOCKING_PUT
	  /* Pending puts init */
	  if(pending_puts == NULL)
	    {
	      pending_puts = calloc(1,sizeof(win_sync));
	      pending_puts->next=NULL;
	      pending_puts->win = token;
	      pending_puts->img = image_index-1;
	      last_elem = pending_puts;
	      last_elem->next = NULL;
	    }
	  else
	    {
	      last_elem->next = calloc(1,sizeof(win_sync));
	      last_elem = last_elem->next;
	      last_elem->win = token;
	      last_elem->img = image_index-1;
	      last_elem->next = NULL;
	    }
#else
	  MPI_Win_flush (image_index-1, *p);
#endif // CAF_MPI_LOCK_UNLOCK
        }

#ifdef WITH_FAILED_IMAGES
      check_image_health (image_index , stat);
#else
      if (ierr != 0)
        terminate_internal (ierr, 0);
#endif
      return;
    }
  else
    {
#ifdef STRIDED
      MPI_Datatype dt_s, dt_d, base_type_src, base_type_dst;
      int *arr_bl;
      int *arr_dsp_s, *arr_dsp_d;

      void *sr = src->base_addr;

      selectType (GFC_DESCRIPTOR_SIZE (src), &base_type_src);
      selectType (GFC_DESCRIPTOR_SIZE (dest), &base_type_dst);

      if(rank == 1)
        {
          MPI_Type_vector(size, 1, src->dim[0]._stride, base_type_src, &dt_s);
          MPI_Type_vector(size, 1, dest->dim[0]._stride, base_type_dst, &dt_d);
        }
      /* else if(rank == 2) */
      /*   { */
      /*     MPI_Type_vector(size/src->dim[0]._ubound, src->dim[0]._ubound, src->dim[1]._stride, base_type_src, &dt_s); */
      /*     MPI_Type_vector(size/dest->dim[0]._ubound, dest->dim[0]._ubound, dest->dim[1]._stride, base_type_dst, &dt_d); */
      /*   } */
      else
        {
          arr_bl = calloc (size, sizeof (int));
          arr_dsp_s = calloc (size, sizeof (int));
          arr_dsp_d = calloc (size, sizeof (int));

          for (i = 0; i < size; i++)
            arr_bl[i] = 1;

          for (i = 0; i < size; i++)
            {
              ptrdiff_t array_offset_dst = 0;
              ptrdiff_t stride = 1;
              ptrdiff_t extent = 1;
	      ptrdiff_t tot_ext = 1;
              for (j = 0; j < rank-1; j++)
                {
                  array_offset_dst += ((i / tot_ext)
                                       % (dest->dim[j]._ubound
                                          - dest->dim[j].lower_bound + 1))
                    * dest->dim[j]._stride;
                  extent = (dest->dim[j]._ubound - dest->dim[j].lower_bound + 1);
                  stride = dest->dim[j]._stride;
		  tot_ext *= extent;
                }

              array_offset_dst += (i / tot_ext) * dest->dim[rank-1]._stride;
              arr_dsp_d[i] = array_offset_dst;

              if (GFC_DESCRIPTOR_RANK (src) != 0)
                {
                  ptrdiff_t array_offset_sr = 0;
                  stride = 1;
                  extent = 1;
                  tot_ext = 1;
                  for (j = 0; j < GFC_DESCRIPTOR_RANK (src)-1; j++)
                    {
                      array_offset_sr += ((i / tot_ext)
                                          % (src->dim[j]._ubound
                                             - src->dim[j].lower_bound + 1))
                        * src->dim[j]._stride;
                      extent = (src->dim[j]._ubound - src->dim[j].lower_bound + 1);
                      stride = src->dim[j]._stride;
                      tot_ext *= extent;
                    }

                  array_offset_sr += (i / tot_ext) * src->dim[rank-1]._stride;
                  arr_dsp_s[i] = array_offset_sr;
                }
              else
                arr_dsp_s[i] = 0;
            }

          MPI_Type_indexed(size, arr_bl, arr_dsp_s, base_type_src, &dt_s);
          MPI_Type_indexed(size, arr_bl, arr_dsp_d, base_type_dst, &dt_d);

          free (arr_bl);
          free (arr_dsp_s);
          free (arr_dsp_d);
        }

      MPI_Type_commit(&dt_s);
      MPI_Type_commit(&dt_d);

      dst_offset = offset;

      CAF_Win_lock (MPI_LOCK_EXCLUSIVE, image_index - 1, *p);
      ierr = MPI_Put (sr, 1, dt_s, image_index-1, dst_offset, 1, dt_d, *p);
      CAF_Win_unlock (image_index - 1, *p);

#ifdef WITH_FAILED_IMAGES
      check_image_health (image_index, stat);

      if(!stat && ierr == STAT_FAILED_IMAGE)
        terminate_internal (ierr, 1);

      if(stat)
        *stat = ierr;
#else
      if (ierr != 0)
         {
           terminate_internal (ierr, 1);
           return;
         }
#endif
      MPI_Type_free (&dt_s);
      MPI_Type_free (&dt_d);

#else
      if(caf_this_image == image_index && mrt)
        {
          t_buff = calloc(size,GFC_DESCRIPTOR_SIZE (dest));
          buff_map = calloc(size,sizeof(bool));
        }

      CAF_Win_lock (MPI_LOCK_EXCLUSIVE, image_index - 1, *p);
      for (i = 0; i < size; i++)
        {
          ptrdiff_t array_offset_dst = 0;
          ptrdiff_t stride = 1;
          ptrdiff_t extent = 1;
	  ptrdiff_t tot_ext = 1;
          for (j = 0; j < rank-1; j++)
            {
              array_offset_dst += ((i / tot_ext)
                                   % (dest->dim[j]._ubound
                                      - dest->dim[j].lower_bound + 1))
                * dest->dim[j]._stride;
              extent = (dest->dim[j]._ubound - dest->dim[j].lower_bound + 1);
              stride = dest->dim[j]._stride;
	      tot_ext *= extent;
            }

          array_offset_dst += (i / tot_ext) * dest->dim[rank-1]._stride;
          dst_offset = offset + array_offset_dst*GFC_DESCRIPTOR_SIZE (dest);

          void *sr;
          if (GFC_DESCRIPTOR_RANK (src) != 0)
            {
              ptrdiff_t array_offset_sr = 0;
              stride = 1;
              extent = 1;
	      tot_ext = 1;
              for (j = 0; j < GFC_DESCRIPTOR_RANK (src)-1; j++)
                {
                  array_offset_sr += ((i / tot_ext)
                                      % (src->dim[j]._ubound
                                         - src->dim[j].lower_bound + 1))
                    * src->dim[j]._stride;
                  extent = (src->dim[j]._ubound - src->dim[j].lower_bound + 1);
                  stride = src->dim[j]._stride;
		  tot_ext *= extent;
                }

              array_offset_sr += (i / tot_ext) * src->dim[rank-1]._stride;
              sr = (void *)((char *) src->base_addr
                            + array_offset_sr*GFC_DESCRIPTOR_SIZE (src));
            }
          else
            sr = src->base_addr;

          if(caf_this_image == image_index)
            {
              if(!mrt)
                memmove(dest->base_addr+dst_offset,sr,GFC_DESCRIPTOR_SIZE (src));
              else
                {
                  memmove(t_buff+i*GFC_DESCRIPTOR_SIZE (src),sr,GFC_DESCRIPTOR_SIZE (src));
                  buff_map[i] = true;
                }
            }
          else
            {
              ierr = MPI_Put (sr, GFC_DESCRIPTOR_SIZE (dest), MPI_BYTE, image_index-1,
                              dst_offset, GFC_DESCRIPTOR_SIZE (dest), MPI_BYTE, *p);
              if (pad_str)
                ierr = MPI_Put (pad_str, dst_size - src_size, MPI_BYTE, image_index-1,
                                dst_offset, dst_size - src_size, MPI_BYTE, *p);
            }

#ifndef WITH_FAILED_IMAGES
          if (ierr != 0)
            {
              caf_runtime_error ("MPI Error: %d", ierr);
              return;
            }
#endif
        }

      if(caf_this_image == image_index && mrt)
        {
          for(i=0;i<size;i++)
            {
              if(buff_map[i])
                {
                  ptrdiff_t array_offset_dst = 0;
                  ptrdiff_t stride = 1;
                  ptrdiff_t extent = 1;
		  ptrdiff_t tot_ext = 1;
                  for (j = 0; j < rank-1; j++)
                    {
                      array_offset_dst += ((i / tot_ext)
                                           % (dest->dim[j]._ubound
                                              - dest->dim[j].lower_bound + 1))
                        * dest->dim[j]._stride;
                      extent = (dest->dim[j]._ubound - dest->dim[j].lower_bound + 1);
                      stride = dest->dim[j]._stride;
		      tot_ext *= extent;
                    }

		  //extent = (dest->dim[rank-1]._ubound - dest->dim[rank-1].lower_bound + 1);
                  array_offset_dst += (i / tot_ext) * dest->dim[rank-1]._stride;
                  dst_offset = offset + array_offset_dst*GFC_DESCRIPTOR_SIZE (dest);
                  memmove(src->base_addr+dst_offset,t_buff+i*GFC_DESCRIPTOR_SIZE (src),GFC_DESCRIPTOR_SIZE (src));
                }
            }
          free(t_buff);
          free(buff_map);
        }
      CAF_Win_unlock (image_index - 1, *p);
#endif

      check_image_health (image_index, stat);
    }
}


/* Get array data from a remote src to a local dest.  */

void
PREFIX (get) (caf_token_t token, size_t offset,
              int image_index,
              gfc_descriptor_t *src,
              caf_vector_t *src_vector __attribute__ ((unused)),
              gfc_descriptor_t *dest, int src_kind, int dst_kind,
              bool mrt, int *stat)
{
  size_t i, size;
  int ierr = 0, j;
  MPI_Win *p = TOKEN(token);
  int rank = GFC_DESCRIPTOR_RANK (src);
  size_t src_size = GFC_DESCRIPTOR_SIZE (src);
  size_t dst_size = GFC_DESCRIPTOR_SIZE (dest);
  void *t_buff = NULL;
  bool *buff_map = NULL;
  void *pad_str = NULL;
  /* size_t sr_off = 0;  */

  size = 1;
  for (j = 0; j < rank; j++)
    {
      ptrdiff_t dimextent = dest->dim[j]._ubound - dest->dim[j].lower_bound + 1;
      if (dimextent < 0)
        dimextent = 0;
      size *= dimextent;
    }

  if (size == 0)
    return;

  check_image_health (image_index, stat);

  if (GFC_DESCRIPTOR_TYPE (dest) == BT_CHARACTER && dst_size > src_size)
    {
      pad_str = alloca (dst_size - src_size);
      if (dst_kind == 1)
        memset (pad_str, ' ', dst_size-src_size);
      else /* dst_kind == 4.  */
        for (i = 0; i < (dst_size-src_size)/4; i++)
              ((int32_t*) pad_str)[i] = (int32_t) ' ';
    }

  if (rank == 0
      || (GFC_DESCRIPTOR_TYPE (dest) == GFC_DESCRIPTOR_TYPE (src)
          && dst_kind == src_kind
          && (GFC_DESCRIPTOR_TYPE (dest) != BT_CHARACTER || dst_size == src_size)
          && PREFIX (is_contiguous) (dest) && PREFIX (is_contiguous) (src)))
    {
      /*  if (async == false) */
      if(caf_this_image == image_index)
        {
          /* The address of source passed by the compiler points on the right
           * memory location. No offset summation is needed.  */
          void *src_tmp = (void *) ((char *) src->base_addr);// + offset);
          memmove(dest->base_addr,src_tmp,size*src_size);
          return;
        }
      else
        {
          CAF_Win_lock (MPI_LOCK_SHARED, image_index - 1, *p);
          ierr = MPI_Get (dest->base_addr, dst_size*size, MPI_BYTE,
                          image_index-1, offset, dst_size*size, MPI_BYTE, *p);
          if (pad_str)
            memcpy ((char *) dest->base_addr + src_size, pad_str,
                    dst_size-src_size);
          CAF_Win_unlock (image_index - 1, *p);

          check_image_health (image_index, stat);
        }
      if (ierr != 0)
        terminate_internal (ierr, 0);
      return;
    }

#ifdef STRIDED

  MPI_Datatype dt_s, dt_d, base_type_src, base_type_dst;
  int *arr_bl;
  int *arr_dsp_s, *arr_dsp_d;

  void *dst = dest->base_addr;

  selectType(GFC_DESCRIPTOR_SIZE (src), &base_type_src);
  selectType(GFC_DESCRIPTOR_SIZE (dest), &base_type_dst);

  if(rank == 1)
    {
      MPI_Type_vector(size, 1, src->dim[0]._stride, base_type_src, &dt_s);
      MPI_Type_vector(size, 1, dest->dim[0]._stride, base_type_dst, &dt_d);
    }
  /* else if(rank == 2) */
  /*   { */
  /*     MPI_Type_vector(size/src->dim[0]._ubound, src->dim[0]._ubound, src->dim[1]._stride, base_type_src, &dt_s); */
  /*     MPI_Type_vector(size/dest->dim[0]._ubound, dest->dim[0]._ubound, dest->dim[1]._stride, base_type_dst, &dt_d); */
  /*   } */
  else
    {
      arr_bl = calloc(size, sizeof(int));
      arr_dsp_s = calloc(size, sizeof(int));
      arr_dsp_d = calloc(size, sizeof(int));

      for(i=0;i<size;i++)
        arr_bl[i]=1;

      for (i = 0; i < size; i++)
        {
          ptrdiff_t array_offset_dst = 0;
          ptrdiff_t stride = 1;
          ptrdiff_t extent = 1;
	  ptrdiff_t tot_ext = 1;
          for (j = 0; j < rank-1; j++)
            {
              array_offset_dst += ((i / tot_ext)
                                   % (dest->dim[j]._ubound
                                      - dest->dim[j].lower_bound + 1))
                * dest->dim[j]._stride;
              extent = (dest->dim[j]._ubound - dest->dim[j].lower_bound + 1);
              stride = dest->dim[j]._stride;
	      tot_ext *= extent;
            }

	  //extent = (dest->dim[rank-1]._ubound - dest->dim[rank-1].lower_bound + 1);
          array_offset_dst += (i / tot_ext) * dest->dim[rank-1]._stride;
          arr_dsp_d[i] = array_offset_dst;

          ptrdiff_t array_offset_sr = 0;
          stride = 1;
          extent = 1;
	  tot_ext = 1;
          for (j = 0; j < GFC_DESCRIPTOR_RANK (src)-1; j++)
            {
              array_offset_sr += ((i / tot_ext)
                                  % (src->dim[j]._ubound
                                     - src->dim[j].lower_bound + 1))
                * src->dim[j]._stride;
              extent = (src->dim[j]._ubound - src->dim[j].lower_bound + 1);
              stride = src->dim[j]._stride;
	      tot_ext *= extent;
            }

	  //extent = (src->dim[rank-1]._ubound - src->dim[rank-1].lower_bound + 1);
          array_offset_sr += (i / tot_ext) * src->dim[rank-1]._stride;
          arr_dsp_s[i] = array_offset_sr;

        }

      MPI_Type_indexed(size, arr_bl, arr_dsp_s, base_type_src, &dt_s);
      MPI_Type_indexed(size, arr_bl, arr_dsp_d, base_type_dst, &dt_d);

      free(arr_bl);
      free(arr_dsp_s);
      free(arr_dsp_d);
    }

  MPI_Type_commit(&dt_s);
  MPI_Type_commit(&dt_d);

  //sr_off = offset;

  CAF_Win_lock (MPI_LOCK_SHARED, image_index - 1, *p);
  ierr = MPI_Get (dst, 1, dt_d, image_index-1, offset, 1, dt_s, *p);
#ifdef WITH_FAILED_IMAGES
  check_image_health (image_index, stat);

  if(stat)
    *stat = ierr;
  else if(ierr == STAT_FAILED_IMAGE)
    terminate_internal (STAT_FAILED_IMAGE, 1);
#else
  CAF_Win_unlock (image_index - 1, *p);

  if(stat)
    *stat = ierr;
  else if (ierr != 0)
    terminate_internal (ierr, 1);
#endif

  MPI_Type_free(&dt_s);
  MPI_Type_free(&dt_d);

#else
  if(caf_this_image == image_index && mrt)
    {
      t_buff = calloc(size,GFC_DESCRIPTOR_SIZE (dest));
      buff_map = calloc(size,sizeof(bool));
    }

  CAF_Win_lock (MPI_LOCK_SHARED, image_index - 1, *p);
  for (i = 0; i < size; i++)
    {
      ptrdiff_t array_offset_dst = 0;
      ptrdiff_t stride = 1;
      ptrdiff_t extent = 1;
      ptrdiff_t tot_ext = 1;
      for (j = 0; j < rank-1; j++)
        {
          array_offset_dst += ((i / tot_ext)
                               % (dest->dim[j]._ubound
                                  - dest->dim[j].lower_bound + 1))
                              * dest->dim[j]._stride;
          extent = (dest->dim[j]._ubound - dest->dim[j].lower_bound + 1);
          stride = dest->dim[j]._stride;
	  tot_ext *= extent;
        }

      array_offset_dst += (i / tot_ext) * dest->dim[rank-1]._stride;

      ptrdiff_t array_offset_sr = 0;
      stride = 1;
      extent = 1;
      tot_ext = 1;
      for (j = 0; j < GFC_DESCRIPTOR_RANK (src)-1; j++)
        {
          array_offset_sr += ((i / tot_ext)
                           % (src->dim[j]._ubound
                              - src->dim[j].lower_bound + 1))
                          * src->dim[j]._stride;
          extent = (src->dim[j]._ubound - src->dim[j].lower_bound + 1);
          stride = src->dim[j]._stride;
	  tot_ext *= extent;
        }

      array_offset_sr += (i / tot_ext) * src->dim[rank-1]._stride;

      size_t sr_off = offset + array_offset_sr*GFC_DESCRIPTOR_SIZE (src);
      void *dst = (void *) ((char *) dest->base_addr
                            + array_offset_dst*GFC_DESCRIPTOR_SIZE (dest));
      /* FIXME: Handle image_index == this_image().  */
      /*  if (async == false) */
      if(caf_this_image == image_index)
        {
          /* Is this needed? */
          if(!mrt)
            memmove(dst,src->base_addr+array_offset_sr*GFC_DESCRIPTOR_SIZE(src),GFC_DESCRIPTOR_SIZE (src));
          else
            {
              memmove(t_buff+i*GFC_DESCRIPTOR_SIZE (dest),dst,GFC_DESCRIPTOR_SIZE (dest));
              buff_map[i] = true;
            }
        }
      else
        {
          ierr = MPI_Get (dst, GFC_DESCRIPTOR_SIZE (dest),
                          MPI_BYTE, image_index-1, sr_off,
                          GFC_DESCRIPTOR_SIZE (src), MPI_BYTE, *p);
          if (pad_str)
            memcpy ((char *) dst + src_size, pad_str, dst_size-src_size);
        }
      if (ierr != 0)
        terminate_internal (ierr, 0);
    }

  if(caf_this_image == image_index && mrt)
    {
      for(i=0;i<size;i++)
        {
          if(buff_map[i])
            {
              ptrdiff_t array_offset_sr = 0;
              ptrdiff_t stride = 1;
              ptrdiff_t extent = 1;
	      ptrdiff_t tot_ext = 1;
              for (j = 0; j < GFC_DESCRIPTOR_RANK (src)-1; j++)
                {
                  array_offset_sr += ((i / tot_ext)
                                      % (src->dim[j]._ubound
                                         - src->dim[j].lower_bound + 1))
                    * src->dim[j]._stride;
                  extent = (src->dim[j]._ubound - src->dim[j].lower_bound + 1);
                  stride = src->dim[j]._stride;
		  tot_ext *= extent;
                }

	      //extent = (src->dim[rank-1]._ubound - src->dim[rank-1].lower_bound + 1);
              array_offset_sr += (i / tot_ext) * src->dim[rank-1]._stride;

              size_t sr_off = offset + array_offset_sr*GFC_DESCRIPTOR_SIZE (src);

              memmove(dest->base_addr+sr_off,t_buff+i*GFC_DESCRIPTOR_SIZE (src),GFC_DESCRIPTOR_SIZE (src));
            }
        }
      free(t_buff);
      free(buff_map);
    }
  CAF_Win_unlock (image_index - 1, *p);
#endif
}


#ifdef GCC_GE_7
/* Emitted when a theorectically unreachable part is reached.  */
const char unreachable[] = "Fatal error: unreachable alternative found.\n";

/** Convert kind 4 characters into kind 1 one.
    Copied from the gcc:libgfortran/caf/single.c.
*/
static void
assign_char4_from_char1 (size_t dst_size, size_t src_size, uint32_t *dst,
			 unsigned char *src)
{
  size_t i, n;
  n = dst_size/4 > src_size ? src_size : dst_size/4;
  for (i = 0; i < n; ++i)
    dst[i] = (int32_t) src[i];
  for (; i < dst_size/4; ++i)
    dst[i] = (int32_t) ' ';
}


/** Convert kind 1 characters into kind 4 one.
    Copied from the gcc:libgfortran/caf/single.c.
*/
static void
assign_char1_from_char4 (size_t dst_size, size_t src_size, unsigned char *dst,
			 uint32_t *src)
{
  size_t i, n;
  n = dst_size > src_size/4 ? src_size/4 : dst_size;
  for (i = 0; i < n; ++i)
    dst[i] = src[i] > UINT8_MAX ? (unsigned char) '?' : (unsigned char) src[i];
  if (dst_size > n)
    memset (&dst[n], ' ', dst_size - n);
}


/** Convert convertable types.
    Copied from the gcc:libgfortran/caf/single.c. Can't say much about it.
*/
static void
convert_type (void *dst, int dst_type, int dst_kind, void *src, int src_type,
	      int src_kind, int *stat)
{
#ifdef HAVE_GFC_INTEGER_16
  typedef __int128 int128t;
#else
  typedef int64_t int128t;
#endif

#if defined(GFC_REAL_16_IS_LONG_DOUBLE)
  typedef long double real128t;
  typedef _Complex long double complex128t;
#elif defined(HAVE_GFC_REAL_16)
  typedef _Complex float __attribute__((mode(TC))) __complex128;
  typedef __float128 real128t;
  typedef __complex128 complex128t;
#elif defined(HAVE_GFC_REAL_10)
  typedef long double real128t;
  typedef long double complex128t;
#else
  typedef double real128t;
  typedef _Complex double complex128t;
#endif

  int128t int_val = 0;
  real128t real_val = 0;
  complex128t cmpx_val = 0;

  switch (src_type)
    {
    case BT_INTEGER:
      if (src_kind == 1)
	int_val = *(int8_t*) src;
      else if (src_kind == 2)
	int_val = *(int16_t*) src;
      else if (src_kind == 4)
	int_val = *(int32_t*) src;
      else if (src_kind == 8)
	int_val = *(int64_t*) src;
#ifdef HAVE_GFC_INTEGER_16
      else if (src_kind == 16)
	int_val = *(int128t*) src;
#endif
      else
	goto error;
      break;
    case BT_REAL:
      if (src_kind == 4)
	real_val = *(float*) src;
      else if (src_kind == 8)
	real_val = *(double*) src;
#ifdef HAVE_GFC_REAL_10
      else if (src_kind == 10)
	real_val = *(long double*) src;
#endif
#ifdef HAVE_GFC_REAL_16
      else if (src_kind == 16)
	real_val = *(real128t*) src;
#endif
      else
	goto error;
      break;
    case BT_COMPLEX:
      if (src_kind == 4)
	cmpx_val = *(_Complex float*) src;
      else if (src_kind == 8)
	cmpx_val = *(_Complex double*) src;
#ifdef HAVE_GFC_REAL_10
      else if (src_kind == 10)
	cmpx_val = *(_Complex long double*) src;
#endif
#ifdef HAVE_GFC_REAL_16
      else if (src_kind == 16)
	cmpx_val = *(complex128t*) src;
#endif
      else
	goto error;
      break;
    default:
      goto error;
    }

  switch (dst_type)
    {
    case BT_INTEGER:
      if (src_type == BT_INTEGER)
	{
	  if (dst_kind == 1)
	    *(int8_t*) dst = (int8_t) int_val;
	  else if (dst_kind == 2)
	    *(int16_t*) dst = (int16_t) int_val;
	  else if (dst_kind == 4)
	    *(int32_t*) dst = (int32_t) int_val;
	  else if (dst_kind == 8)
	    *(int64_t*) dst = (int64_t) int_val;
#ifdef HAVE_GFC_INTEGER_16
	  else if (dst_kind == 16)
	    *(int128t*) dst = (int128t) int_val;
#endif
	  else
	    goto error;
	}
      else if (src_type == BT_REAL)
	{
	  if (dst_kind == 1)
	    *(int8_t*) dst = (int8_t) real_val;
	  else if (dst_kind == 2)
	    *(int16_t*) dst = (int16_t) real_val;
	  else if (dst_kind == 4)
	    *(int32_t*) dst = (int32_t) real_val;
	  else if (dst_kind == 8)
	    *(int64_t*) dst = (int64_t) real_val;
#ifdef HAVE_GFC_INTEGER_16
	  else if (dst_kind == 16)
	    *(int128t*) dst = (int128t) real_val;
#endif
	  else
	    goto error;
	}
      else if (src_type == BT_COMPLEX)
	{
	  if (dst_kind == 1)
	    *(int8_t*) dst = (int8_t) cmpx_val;
	  else if (dst_kind == 2)
	    *(int16_t*) dst = (int16_t) cmpx_val;
	  else if (dst_kind == 4)
	    *(int32_t*) dst = (int32_t) cmpx_val;
	  else if (dst_kind == 8)
	    *(int64_t*) dst = (int64_t) cmpx_val;
#ifdef HAVE_GFC_INTEGER_16
	  else if (dst_kind == 16)
	    *(int128t*) dst = (int128t) cmpx_val;
#endif
	  else
	    goto error;
	}
      else
	goto error;
      return;
    case BT_REAL:
      if (src_type == BT_INTEGER)
	{
	  if (dst_kind == 4)
	    *(float*) dst = (float) int_val;
	  else if (dst_kind == 8)
	    *(double*) dst = (double) int_val;
#ifdef HAVE_GFC_REAL_10
	  else if (dst_kind == 10)
	    *(long double*) dst = (long double) int_val;
#endif
#ifdef HAVE_GFC_REAL_16
	  else if (dst_kind == 16)
	    *(real128t*) dst = (real128t) int_val;
#endif
	  else
	    goto error;
	}
      else if (src_type == BT_REAL)
	{
	  if (dst_kind == 4)
	    *(float*) dst = (float) real_val;
	  else if (dst_kind == 8)
	    *(double*) dst = (double) real_val;
#ifdef HAVE_GFC_REAL_10
	  else if (dst_kind == 10)
	    *(long double*) dst = (long double) real_val;
#endif
#ifdef HAVE_GFC_REAL_16
	  else if (dst_kind == 16)
	    *(real128t*) dst = (real128t) real_val;
#endif
	  else
	    goto error;
	}
      else if (src_type == BT_COMPLEX)
	{
	  if (dst_kind == 4)
	    *(float*) dst = (float) cmpx_val;
	  else if (dst_kind == 8)
	    *(double*) dst = (double) cmpx_val;
#ifdef HAVE_GFC_REAL_10
	  else if (dst_kind == 10)
	    *(long double*) dst = (long double) cmpx_val;
#endif
#ifdef HAVE_GFC_REAL_16
	  else if (dst_kind == 16)
	    *(real128t*) dst = (real128t) cmpx_val;
#endif
	  else
	    goto error;
	}
      return;
    case BT_COMPLEX:
      if (src_type == BT_INTEGER)
	{
	  if (dst_kind == 4)
	    *(_Complex float*) dst = (_Complex float) int_val;
	  else if (dst_kind == 8)
	    *(_Complex double*) dst = (_Complex double) int_val;
#ifdef HAVE_GFC_REAL_10
	  else if (dst_kind == 10)
	    *(_Complex long double*) dst = (_Complex long double) int_val;
#endif
#ifdef HAVE_GFC_REAL_16
	  else if (dst_kind == 16)
	    *(complex128t*) dst = (complex128t) int_val;
#endif
	  else
	    goto error;
	}
      else if (src_type == BT_REAL)
	{
	  if (dst_kind == 4)
	    *(_Complex float*) dst = (_Complex float) real_val;
	  else if (dst_kind == 8)
	    *(_Complex double*) dst = (_Complex double) real_val;
#ifdef HAVE_GFC_REAL_10
	  else if (dst_kind == 10)
	    *(_Complex long double*) dst = (_Complex long double) real_val;
#endif
#ifdef HAVE_GFC_REAL_16
	  else if (dst_kind == 16)
	    *(complex128t*) dst = (complex128t) real_val;
#endif
	  else
	    goto error;
	}
      else if (src_type == BT_COMPLEX)
	{
	  if (dst_kind == 4)
	    *(_Complex float*) dst = (_Complex float) cmpx_val;
	  else if (dst_kind == 8)
	    *(_Complex double*) dst = (_Complex double) cmpx_val;
#ifdef HAVE_GFC_REAL_10
	  else if (dst_kind == 10)
	    *(_Complex long double*) dst = (_Complex long double) cmpx_val;
#endif
#ifdef HAVE_GFC_REAL_16
	  else if (dst_kind == 16)
	    *(complex128t*) dst = (complex128t) cmpx_val;
#endif
	  else
	    goto error;
	}
      else
	goto error;
      return;
    default:
      goto error;
    }

error:
  fprintf (stderr, "libcaf_mpi RUNTIME ERROR: Cannot convert type %d kind "
	   "%d to type %d kind %d\n", src_type, src_kind, dst_type, dst_kind);
  if (stat)
    *stat = 1;
  else
    abort ();
}


/** Copy a chunk of data from one image to the current one, with type
    conversion.

    Copied from the gcc:libgfortran/caf/single.c. Can't say much about it.
*/
static void
copy_data (void *ds, mpi_caf_token_t *token, MPI_Aint offset, int dst_type,
           int src_type, int dst_kind, int src_kind, size_t dst_size,
           size_t src_size, size_t num, int *stat, int image_index)
{
  size_t k;
  MPI_Win win = token == NULL ? global_dynamic_win : token->memptr_win;
  if (dst_type == src_type && dst_kind == src_kind)
    {
      size_t sz = (dst_size > src_size ? src_size : dst_size) * num;
#ifdef EXTRA_DEBUG_OUTPUT
      if (token)
        dprint ("%d/%d: %s() %p = win: %p -> offset: %d of size %d bytes\n",
                caf_this_image, caf_num_images, __FUNCTION__, ds, win,
                offset, sz);
      else
        dprint ("%d/%d: %s() %p = global_win offset: %d of size %d bytes\n",
                caf_this_image, caf_num_images, __FUNCTION__, ds,
                offset, sz);
#endif
      MPI_Get (ds, sz, MPI_BYTE, image_index, offset, sz, MPI_BYTE,
               win);
      if ((dst_type == BT_CHARACTER || src_type == BT_CHARACTER)
          && dst_size > src_size)
        {
          if (dst_kind == 1)
            memset ((void*)(char*) ds + src_size, ' ', dst_size-src_size);
          else /* dst_kind == 4.  */
            for (k = src_size/4; k < dst_size/4; k++)
              ((int32_t*) ds)[k] = (int32_t) ' ';
        }
    }
  else if (dst_type == BT_CHARACTER && dst_kind == 1)
    {
      /* Get the required amount of memory on the stack.  */
      void *srh = alloca (src_size);
      MPI_Get (srh, src_size, MPI_BYTE, image_index, offset,
               src_size, MPI_BYTE, win);
      assign_char1_from_char4 (dst_size, src_size, ds, srh);
    }
  else if (dst_type == BT_CHARACTER)
    {
      /* Get the required amount of memory on the stack.  */
      void *srh = alloca (src_size);
      MPI_Get (srh, src_size, MPI_BYTE, image_index, offset,
               src_size, MPI_BYTE, win);
      assign_char4_from_char1 (dst_size, src_size, ds, srh);
    }
  else
    {
      /* Get the required amount of memory on the stack.  */
      void *srh = alloca (src_size * num);
      MPI_Get (srh, src_size * num, MPI_BYTE, image_index, offset,
               src_size * num, MPI_BYTE, win);
      for (k = 0; k < num; ++k)
        {
          convert_type (ds, dst_type, dst_kind, srh, src_type, src_kind, stat);
          ds += dst_size;
          srh += src_size;
        }
    }
}


/** Compute the number of items referenced.

    Computes the number of items between lower bound (lb) and upper bound (ub)
    with respect to the stride taking corner cases into account.  */
#define COMPUTE_NUM_ITEMS(num, stride, lb, ub) \
  do { \
    ptrdiff_t abs_stride = (stride) > 0 ? (stride) : -(stride); \
    num = (stride) > 0 ? (ub) + 1 - (lb) : (lb) + 1 - (ub); \
    if (num <= 0 || abs_stride < 1) return; \
    num = (abs_stride > 1) ? (1 + (num - 1) / abs_stride) : num; \
  } while (0)


/** Convenience macro to get the extent of a descriptor in a certain dimension.

    Copied from gcc:libgfortran/libgfortran.h. */
#define GFC_DESCRIPTOR_EXTENT(desc,i) ((desc)->dim[i]._ubound + 1 \
                                      - (desc)->dim[i].lower_bound)


#define sizeof_desc_for_rank(rank) sizeof (gfc_descriptor_t) + (rank) * sizeof (descriptor_dimension)

/** Define the descriptor of max rank.

    This typedef is made to allow storing a copy of a remote descriptor on the
    stack without having to care about the rank.  */
typedef struct gfc_max_dim_descriptor_t {
  void *base_addr;
  size_t offset;
  ptrdiff_t dtype;
  descriptor_dimension dim[GFC_MAX_DIMENSIONS];
} gfc_max_dim_descriptor_t;

static void
get_for_ref (caf_reference_t *ref, size_t *i, size_t dst_index,
             mpi_caf_token_t *mpi_token, gfc_descriptor_t *dst,
             gfc_descriptor_t *src, void *ds, void *sr,
             ptrdiff_t sr_byte_offset, ptrdiff_t desc_byte_offset,
             int dst_kind, int src_kind, size_t dst_dim, size_t src_dim,
             size_t num, int *stat, int image_index,
             bool sr_global, /* access sr through global_dynamic_win */
             bool desc_global /* access desc through global_dynamic_win */)
{
  ptrdiff_t extent_src = 1, array_offset_src = 0, stride_src;
  size_t next_dst_dim, ref_rank;
  gfc_max_dim_descriptor_t src_desc_data;

  if (unlikely (ref == NULL))
    /* May be we should issue an error here, because this case should not
       occur.  */
    return;

  dprint ("%d/%d: %s() sr_offset = %d, sr = %p, desc_offset = %d, src = %p, sr_glb = %d, desc_glb = %d\n", caf_this_image,
          caf_num_images, __FUNCTION__, sr_byte_offset, sr, desc_byte_offset, src, sr_global, desc_global);

  if (ref->next == NULL)
    {
      size_t dst_size = GFC_DESCRIPTOR_SIZE (dst);
      int src_type = -1;

      switch (ref->type)
        {
        case CAF_REF_COMPONENT:
          if (ref->u.c.caf_token_offset > 0)
            {
              sr_byte_offset += ref->u.c.offset;
              if (sr_global)
                {
                  MPI_Get (&sr, stdptr_size, MPI_BYTE, image_index,
                           MPI_Aint_add ((MPI_Aint)sr, sr_byte_offset),
                           stdptr_size, MPI_BYTE, global_dynamic_win);
                  desc_global = true;
                }
              else
                {
                  MPI_Get (&sr, stdptr_size, MPI_BYTE, image_index,
                           MPI_Aint_add ((MPI_Aint)sr, sr_byte_offset),
                           stdptr_size, MPI_BYTE, global_dynamic_win);
                  sr_global = true;
                }
              sr_byte_offset = 0;
            }
          else
            sr_byte_offset += ref->u.c.offset;
          if (sr_global)
            copy_data (ds, NULL, MPI_Aint_add ((MPI_Aint)sr, sr_byte_offset),
                       GFC_DESCRIPTOR_TYPE (dst), GFC_DESCRIPTOR_TYPE (dst),
                       dst_kind, src_kind, dst_size, ref->item_size, 1, stat,
                       image_index);
          else
            copy_data (ds, mpi_token, sr_byte_offset,
                       GFC_DESCRIPTOR_TYPE (dst), GFC_DESCRIPTOR_TYPE (src),
                       dst_kind, src_kind, dst_size, ref->item_size, 1, stat,
                       image_index);
          ++(*i);
          return;
        case CAF_REF_STATIC_ARRAY:
          src_type = ref->u.a.static_array_type;
          /* Intentionally fall through.  */
        case CAF_REF_ARRAY:
          if (ref->u.a.mode[src_dim] == CAF_ARR_REF_NONE)
            {
              if (sr_global)
                copy_data (ds + dst_index * dst_size, NULL,
                           MPI_Aint_add ((MPI_Aint)sr,  sr_byte_offset),
                           GFC_DESCRIPTOR_TYPE (dst),
                           src_type == -1 ? GFC_DESCRIPTOR_TYPE (src) : src_type,
                           dst_kind, src_kind, dst_size, ref->item_size, num,
                           stat, image_index);
              else
                {
                  copy_data (ds + dst_index * dst_size, mpi_token,
                             sr_byte_offset, GFC_DESCRIPTOR_TYPE (dst),
                             src_type == -1 ? GFC_DESCRIPTOR_TYPE (src) : src_type,
                             dst_kind, src_kind, dst_size, ref->item_size, num,
                             stat, image_index);
                }
              *i += num;
              return;
            }
          break;
        default:
          caf_runtime_error (unreachable);
        }
    }

  switch (ref->type)
    {
    case CAF_REF_COMPONENT:
      if (ref->u.c.caf_token_offset > 0)
        {
          sr_byte_offset += ref->u.c.offset;
          desc_byte_offset = sr_byte_offset;
          if (sr_global)
            {
              MPI_Get (&sr, stdptr_size, MPI_BYTE, image_index,
                       MPI_Aint_add ((MPI_Aint)sr, sr_byte_offset),
                       stdptr_size, MPI_BYTE, global_dynamic_win);
              desc_global = true;
            }
          else
            {
              MPI_Get (&sr, stdptr_size, MPI_BYTE, image_index,
                       sr_byte_offset, stdptr_size, MPI_BYTE,
                       mpi_token->memptr_win);
              sr_global = true;
            }
          sr_byte_offset = 0;
        }
      else
        {
          sr_byte_offset += ref->u.c.offset;
          desc_byte_offset += ref->u.c.offset;
        }
      get_for_ref (ref->next, i, dst_index, mpi_token, dst, NULL, ds,
                   sr, sr_byte_offset, desc_byte_offset, dst_kind, src_kind,
                   dst_dim, 0, 1, stat, image_index, sr_global, desc_global);
      return;
    case CAF_REF_ARRAY:
      if (ref->u.a.mode[src_dim] == CAF_ARR_REF_NONE)
        {
          get_for_ref (ref->next, i, dst_index, mpi_token, dst,
                       src, ds, sr, sr_byte_offset, desc_byte_offset, dst_kind,
                       src_kind, dst_dim, 0, 1, stat, image_index, sr_global,
                       desc_global);
          return;
        }
      /* Only when on the left most index switch the data pointer to
      the array's data pointer.  */
      if (src_dim == 0)
        {
          if (sr_global)
            {
              for (ref_rank = 0; ref->u.a.mode[ref_rank] != CAF_ARR_REF_NONE; ++ref_rank) ;
              /* Get the remote descriptor. */
              if (desc_global)
                MPI_Get (&src_desc_data, sizeof_desc_for_rank(ref_rank), MPI_BYTE,
                         image_index, MPI_Aint_add ((MPI_Aint)sr, desc_byte_offset),
                         sizeof_desc_for_rank(ref_rank), MPI_BYTE,
                         global_dynamic_win);
              else
                {
                  MPI_Get (&src_desc_data, sizeof_desc_for_rank(ref_rank), MPI_BYTE,
                           image_index, desc_byte_offset, sizeof_desc_for_rank(ref_rank),
                           MPI_BYTE, mpi_token->memptr_win);
                  desc_global = true;
                }
              src = (gfc_descriptor_t *)&src_desc_data;
            }
          else
            src = mpi_token->desc;
          sr_byte_offset = 0;
          desc_byte_offset = 0;
#ifdef EXTRA_DEBUG_OUTPUT
          fprintf (stderr, "%d/%d: %s() remote desc rank: %d (ref_rank: %d)\n", caf_this_image, caf_num_images,
                   __FUNCTION__, GFC_DESCRIPTOR_RANK (src), ref_rank);
          for (int r = 0; r < GFC_DESCRIPTOR_RANK (src); ++r)
            fprintf (stderr, "%d/%d: %s() remote desc dim[%d] = (lb = %d, ub = %d, stride = %d)\n", caf_this_image, caf_num_images,
                     __FUNCTION__, r, src->dim[r].lower_bound, src->dim[r]._ubound, src->dim[r]._stride);
#endif
        }
      switch (ref->u.a.mode[src_dim])
        {
        case CAF_ARR_REF_VECTOR:
          extent_src = GFC_DESCRIPTOR_EXTENT (src, src_dim);
          array_offset_src = 0;
          for (size_t idx = 0; idx < ref->u.a.dim[src_dim].v.nvec;
               ++idx)
            {
#define KINDCASE(kind, type) case kind: \
              array_offset_src = (((ptrdiff_t) \
                  ((type *)ref->u.a.dim[src_dim].v.vector)[idx]) \
                  - src->dim[src_dim].lower_bound \
                  * src->dim[src_dim]._stride); \
              break

              switch (ref->u.a.dim[src_dim].v.kind)
                {
                KINDCASE (1, int8_t);
                KINDCASE (2, int16_t);
                KINDCASE (4, int32_t);
                KINDCASE (8, int64_t);
#ifdef HAVE_GFC_INTEGER_16
                KINDCASE (16, __int128);
#endif
                default:
                  caf_runtime_error (unreachable);
                  return;
                }
#undef KINDCASE

              get_for_ref (ref, i, dst_index, mpi_token, dst, src, ds, sr,
                           sr_byte_offset + array_offset_src * ref->item_size,
                           desc_byte_offset + array_offset_src * ref->item_size,
                           dst_kind, src_kind, dst_dim + 1, src_dim + 1,
                           1, stat, image_index, sr_global, desc_global);
              dst_index += dst->dim[dst_dim]._stride;
            }
          return;
        case CAF_ARR_REF_FULL:
          COMPUTE_NUM_ITEMS (extent_src,
                             ref->u.a.dim[src_dim].s.stride,
                             src->dim[src_dim].lower_bound,
                             src->dim[src_dim]._ubound);
          stride_src = src->dim[src_dim]._stride
                       * ref->u.a.dim[src_dim].s.stride;
          array_offset_src = 0;
          for (ptrdiff_t idx = 0; idx < extent_src;
               ++idx, array_offset_src += stride_src)
            {
              get_for_ref (ref, i, dst_index, mpi_token, dst, src, ds, sr,
                           sr_byte_offset + array_offset_src * ref->item_size,
                           desc_byte_offset + array_offset_src * ref->item_size,
                           dst_kind, src_kind, dst_dim + 1, src_dim + 1,
                           1, stat, image_index, sr_global, desc_global);
              dst_index += dst->dim[dst_dim]._stride;
            }
          return;
        case CAF_ARR_REF_RANGE:
          COMPUTE_NUM_ITEMS (extent_src,
                             ref->u.a.dim[src_dim].s.stride,
                             ref->u.a.dim[src_dim].s.start,
                             ref->u.a.dim[src_dim].s.end);
          array_offset_src = (ref->u.a.dim[src_dim].s.start
                              - src->dim[src_dim].lower_bound)
                             * src->dim[src_dim]._stride;
          stride_src = src->dim[src_dim]._stride
                       * ref->u.a.dim[src_dim].s.stride;
          /* Increase the dst_dim only, when the src_extent is greater one
             or src and dst extent are both one.  Don't increase when the scalar
             source is not present in the dst.  */
          next_dst_dim = extent_src > 1
                         || (GFC_DESCRIPTOR_EXTENT (dst, dst_dim) == 1
                             && extent_src == 1) ? (dst_dim + 1) : dst_dim;
          for (ptrdiff_t idx = 0; idx < extent_src; ++idx)
            {
              get_for_ref (ref, i, dst_index, mpi_token, dst, src, ds, sr,
                           sr_byte_offset + array_offset_src * ref->item_size,
                           desc_byte_offset + array_offset_src * ref->item_size,
                           dst_kind, src_kind, next_dst_dim, src_dim + 1,
                           1, stat, image_index, sr_global, desc_global);
              dst_index += dst->dim[dst_dim]._stride;
              array_offset_src += stride_src;
            }
          return;
        case CAF_ARR_REF_SINGLE:
          array_offset_src = (ref->u.a.dim[src_dim].s.start
                              - src->dim[src_dim].lower_bound)
                             * src->dim[src_dim]._stride;
          get_for_ref (ref, i, dst_index, mpi_token, dst, src, ds, sr,
                       sr_byte_offset + array_offset_src * ref->item_size,
                       desc_byte_offset + array_offset_src * ref->item_size,
                       dst_kind, src_kind, dst_dim, src_dim + 1, 1,
                       stat, image_index, sr_global, desc_global);
          return;
        case CAF_ARR_REF_OPEN_END:
          COMPUTE_NUM_ITEMS (extent_src,
                             ref->u.a.dim[src_dim].s.stride,
                             ref->u.a.dim[src_dim].s.start,
                             src->dim[src_dim]._ubound);
          stride_src = src->dim[src_dim]._stride
                       * ref->u.a.dim[src_dim].s.stride;
          array_offset_src = (ref->u.a.dim[src_dim].s.start
                              - src->dim[src_dim].lower_bound)
                             * src->dim[src_dim]._stride;
          for (ptrdiff_t idx = 0; idx < extent_src; ++idx)
            {
              get_for_ref (ref, i, dst_index, mpi_token, dst, src, ds, sr,
                           sr_byte_offset + array_offset_src * ref->item_size,
                           desc_byte_offset + array_offset_src * ref->item_size,
                           dst_kind, src_kind, dst_dim + 1, src_dim + 1,
                           1, stat, image_index, sr_global, desc_global);
              dst_index += dst->dim[dst_dim]._stride;
              array_offset_src += stride_src;
            }
          return;
        case CAF_ARR_REF_OPEN_START:
          COMPUTE_NUM_ITEMS (extent_src,
                             ref->u.a.dim[src_dim].s.stride,
                             src->dim[src_dim].lower_bound,
                             ref->u.a.dim[src_dim].s.end);
          stride_src = src->dim[src_dim]._stride
                       * ref->u.a.dim[src_dim].s.stride;
          array_offset_src = 0;
          for (ptrdiff_t idx = 0; idx < extent_src; ++idx)
            {
              get_for_ref (ref, i, dst_index, mpi_token, dst, src, ds, sr,
                           sr_byte_offset + array_offset_src * ref->item_size,
                           desc_byte_offset + array_offset_src * ref->item_size,
                           dst_kind, src_kind, dst_dim + 1, src_dim + 1,
                           1, stat, image_index, sr_global, desc_global);
              dst_index += dst->dim[dst_dim]._stride;
              array_offset_src += stride_src;
            }
          return;
        default:
          caf_runtime_error (unreachable);
        }
      return;
    case CAF_REF_STATIC_ARRAY:
      if (ref->u.a.mode[src_dim] == CAF_ARR_REF_NONE)
        {
          get_for_ref (ref->next, i, dst_index, mpi_token, dst, NULL, ds, sr,
                       sr_byte_offset, desc_byte_offset, dst_kind, src_kind,
                       dst_dim, 0, 1, stat, image_index, sr_global, desc_global);
          return;
        }
      switch (ref->u.a.mode[src_dim])
        {
        case CAF_ARR_REF_VECTOR:
          array_offset_src = 0;
          for (size_t idx = 0; idx < ref->u.a.dim[src_dim].v.nvec;
               ++idx)
            {
#define KINDCASE(kind, type) case kind: \
             array_offset_src = ((type *)ref->u.a.dim[src_dim].v.vector)[idx]; \
              break

              switch (ref->u.a.dim[src_dim].v.kind)
                {
                KINDCASE (1, int8_t);
                KINDCASE (2, int16_t);
                KINDCASE (4, int32_t);
                KINDCASE (8, int64_t);
#ifdef HAVE_GFC_INTEGER_16
                KINDCASE (16, __int128);
#endif
                default:
                  caf_runtime_error (unreachable);
                  return;
                }
#undef KINDCASE

              get_for_ref (ref, i, dst_index, mpi_token, dst, NULL, ds, sr,
                           sr_byte_offset + array_offset_src * ref->item_size,
                           desc_byte_offset + array_offset_src * ref->item_size,
                           dst_kind, src_kind, dst_dim + 1, src_dim + 1,
                           1, stat, image_index, sr_global, desc_global);
              dst_index += dst->dim[dst_dim]._stride;
            }
          return;
        case CAF_ARR_REF_FULL:
          for (array_offset_src = 0 ;
               array_offset_src <= ref->u.a.dim[src_dim].s.end;
               array_offset_src += ref->u.a.dim[src_dim].s.stride)
            {
              get_for_ref (ref, i, dst_index, mpi_token, dst, NULL, ds, sr,
                           sr_byte_offset + array_offset_src * ref->item_size,
                           desc_byte_offset + array_offset_src * ref->item_size,
                           dst_kind, src_kind, dst_dim + 1, src_dim + 1,
                           1, stat, image_index, sr_global, desc_global);
              dst_index += dst->dim[dst_dim]._stride;
            }
          return;
        case CAF_ARR_REF_RANGE:
          COMPUTE_NUM_ITEMS (extent_src,
                             ref->u.a.dim[src_dim].s.stride,
                             ref->u.a.dim[src_dim].s.start,
                             ref->u.a.dim[src_dim].s.end);
          array_offset_src = ref->u.a.dim[src_dim].s.start;
          for (ptrdiff_t idx = 0; idx < extent_src; ++idx)
            {
              get_for_ref (ref, i, dst_index, mpi_token, dst, NULL, ds, sr,
                           sr_byte_offset + array_offset_src * ref->item_size,
                           desc_byte_offset + array_offset_src * ref->item_size,
                           dst_kind, src_kind, dst_dim + 1, src_dim + 1,
                           1, stat, image_index, sr_global, desc_global);
              dst_index += dst->dim[dst_dim]._stride;
              array_offset_src += ref->u.a.dim[src_dim].s.stride;
            }
          return;
        case CAF_ARR_REF_SINGLE:
          array_offset_src = ref->u.a.dim[src_dim].s.start;
          get_for_ref (ref, i, dst_index, mpi_token, dst, NULL, ds, sr,
                       sr_byte_offset + array_offset_src * ref->item_size,
                       desc_byte_offset + array_offset_src * ref->item_size,
                       dst_kind, src_kind, dst_dim, src_dim + 1, 1,
                       stat, image_index, sr_global, desc_global);
          return;
          /* The OPEN_* are mapped to a RANGE and therefore can not occur.  */
        case CAF_ARR_REF_OPEN_END:
        case CAF_ARR_REF_OPEN_START:
        default:
          caf_runtime_error (unreachable);
        }
      return;
    default:
      caf_runtime_error (unreachable);
    }
}

void
_gfortran_caf_get_by_ref (caf_token_t token, int image_index,
			  gfc_descriptor_t *dst, caf_reference_t *refs,
			  int dst_kind, int src_kind,
			  bool may_require_tmp __attribute__ ((unused)),
			  bool dst_reallocatable, int *stat)
{
  const char vecrefunknownkind[] = "libcaf_single::caf_get_by_ref(): "
				   "unknown kind in vector-ref.\n";
  const char unknownreftype[] = "libcaf_single::caf_get_by_ref(): "
				"unknown reference type.\n";
  const char unknownarrreftype[] = "libcaf_single::caf_get_by_ref(): "
				   "unknown array reference type.\n";
  const char rankoutofrange[] = "libcaf_single::caf_get_by_ref(): "
				"rank out of range.\n";
  const char extentoutofrange[] = "libcaf_single::caf_get_by_ref(): "
				  "extent out of range.\n";
  const char cannotallocdst[] = "libcaf_single::caf_get_by_ref(): "
				"can not allocate %d bytes of memory.\n";
  const char nonallocextentmismatch[] = "libcaf_single::caf_get_by_ref(): "
      "extent of non-allocatable arrays mismatch (%lu != %lu).\n";
  const char doublearrayref[] = "libcaf_single::caf_get_by_ref(): "
      "two or more array part references are not supported.\n";
  size_t size, i, ref_rank;
  size_t dst_index;
  int dst_rank = GFC_DESCRIPTOR_RANK (dst);
  int dst_cur_dim = 0;
  size_t src_size;
  mpi_caf_token_t *mpi_token = (mpi_caf_token_t *) token;
  void *remote_memptr = mpi_token->memptr, *remote_base_memptr = NULL;
  gfc_max_dim_descriptor_t src_desc;
  gfc_descriptor_t *src = (gfc_descriptor_t *)&src_desc;
  caf_reference_t *riter = refs;
  long delta;
  ptrdiff_t data_offset = 0, desc_offset = 0;
  const int remote_image = image_index - 1;
  /* Reallocation of dst.data is needed (e.g., array to small).  */
  bool realloc_needed;
  /* Reallocation of dst.data is required, because data is not alloced at
     all.  */
  bool realloc_required;
  bool extent_mismatch = false;
  /* Set when the first non-scalar array reference is encountered.  */
  bool in_array_ref = false;
  bool array_extent_fixed = false;
  /* Set when remote data is to be accessed through the global dynamic window. */
  bool access_data_through_global_win = false;
  /* Set when the remote descriptor is to accessed through the global window. */
  bool access_desc_through_global_win = false;

  realloc_needed = realloc_required = dst->base_addr == NULL;

  if (stat)
    *stat = 0;

  check_image_health (image_index, stat);

  dprint ("%d/%d: Entering get_by_ref(may_require_tmp = %d).\n", caf_this_image,
          caf_num_images, may_require_tmp);

  /* Compute the size of the result.  In the beginning size just counts the
     number of elements.  */
  size = 1;
  /* Shared lock both windows to prevent bother in the sub-routines. */
  CAF_Win_lock (MPI_LOCK_SHARED, remote_image, global_dynamic_win);
  CAF_Win_lock (MPI_LOCK_SHARED, remote_image, mpi_token->memptr_win);
  while (riter)
    {
      dprint ("%d/%d: %s() offset = %d, remote_mem = %p\n", caf_this_image,
              caf_num_images, __FUNCTION__, data_offset, remote_memptr);
      switch (riter->type)
        {
        case CAF_REF_COMPONENT:
          if (riter->u.c.caf_token_offset > 0)
            {
              if (access_data_through_global_win)
                {
                  data_offset += riter->u.c.offset;
                  remote_base_memptr = remote_memptr;
                  MPI_Get (&remote_memptr, stdptr_size, MPI_BYTE, remote_image,
                           MPI_Aint_add ((MPI_Aint)remote_memptr, data_offset),
                           stdptr_size, MPI_BYTE, global_dynamic_win);
                  /* On the second indirection access also the remote descriptor
                  using the global window. */
                  access_desc_through_global_win = true;
                }
              else
                {
                  data_offset += riter->u.c.offset;
                  MPI_Get (&remote_memptr, stdptr_size, MPI_BYTE, remote_image,
                           data_offset, stdptr_size, MPI_BYTE, mpi_token->memptr_win);
                  /* All future access is through the global dynamic window. */
                  access_data_through_global_win = true;
                }
              desc_offset = data_offset;
              data_offset = 0;
            }
          else
            {
              data_offset += riter->u.c.offset;
              desc_offset += riter->u.c.offset;
            }
          break;
        case CAF_REF_ARRAY:
          /* When there has been no CAF_REF_COMP before hand, then the
          descriptor is stored in the token and the extends are the same on all
          images, which is taken care of in the else part.  */
          if (access_data_through_global_win)
            {
              for (ref_rank = 0; riter->u.a.mode[ref_rank] != CAF_ARR_REF_NONE; ++ref_rank) ;
              /* Get the remote descriptor and use the stack to store it. Note,
              src may be pointing to mpi_token->desc therefore it needs to be
              reset here.  */
              src = (gfc_descriptor_t *)&src_desc;
              if (access_desc_through_global_win)
                {
                  dprint ("%d/%d: %s() remote desc fetch from %p, offset = %d\n",
                          caf_this_image, caf_num_images, __FUNCTION__,
                          remote_base_memptr, desc_offset);
                  MPI_Get (src, sizeof_desc_for_rank(ref_rank), MPI_BYTE, remote_image,
                           MPI_Aint_add ((MPI_Aint)remote_base_memptr, desc_offset),
                           sizeof_desc_for_rank(ref_rank),
                           MPI_BYTE, global_dynamic_win);
                }
              else
                {
                  dprint ("%d/%d: %s() remote desc fetch from win %p, offset = %d\n",
                          caf_this_image, caf_num_images, __FUNCTION__,
                          mpi_token->memptr_win, desc_offset);
                  MPI_Get (src, sizeof_desc_for_rank(ref_rank), MPI_BYTE, remote_image,
                           desc_offset, sizeof_desc_for_rank(ref_rank),
                           MPI_BYTE, mpi_token->memptr_win);
                  access_desc_through_global_win = true;
                }
            }
          else
            src = mpi_token->desc;
#ifdef EXTRA_DEBUG_OUTPUT
          fprintf (stderr, "%d/%d: %s() remote desc rank: %d (ref_rank: %d)\n", caf_this_image, caf_num_images,
                   __FUNCTION__, GFC_DESCRIPTOR_RANK (src), ref_rank);
          for (i = 0; i < GFC_DESCRIPTOR_RANK (src); ++i)
            fprintf (stderr, "%d/%d: %s() remote desc dim[%d] = (lb = %d, ub = %d, stride = %d)\n", caf_this_image, caf_num_images,
                     __FUNCTION__, i, src->dim[i].lower_bound, src->dim[i]._ubound, src->dim[i]._stride);
#endif
          for (i = 0; riter->u.a.mode[i] != CAF_ARR_REF_NONE; ++i)
            {
              switch (riter->u.a.mode[i])
                {
                case CAF_ARR_REF_VECTOR:
                  delta = riter->u.a.dim[i].v.nvec;
#define KINDCASE(kind, type) case kind: \
                    remote_memptr += (((ptrdiff_t) \
                        ((type *)riter->u.a.dim[i].v.vector)[0]) \
                        - src->dim[i].lower_bound) \
                        * src->dim[i]._stride \
                        * riter->item_size; \
                    break

                  switch (riter->u.a.dim[i].v.kind)
                    {
                    KINDCASE (1, int8_t);
                    KINDCASE (2, int16_t);
                    KINDCASE (4, int32_t);
                    KINDCASE (8, int64_t);
#if HAVE_GFC_INTEGER_16
                    KINDCASE (16, __int128);
#endif
                    default:
                      caf_runtime_error (vecrefunknownkind, stat, NULL, 0);
                      return;
                    }
#undef KINDCASE
                  break;
                case CAF_ARR_REF_FULL:
                  COMPUTE_NUM_ITEMS (delta,
                                     riter->u.a.dim[i].s.stride,
                                     src->dim[i].lower_bound,
                                     src->dim[i]._ubound);
                  /* The memptr stays unchanged when ref'ing the first element
                     in a dimension.  */
                  break;
                case CAF_ARR_REF_RANGE:
                  COMPUTE_NUM_ITEMS (delta,
                                     riter->u.a.dim[i].s.stride,
                                     riter->u.a.dim[i].s.start,
                                     riter->u.a.dim[i].s.end);
                  remote_memptr += (riter->u.a.dim[i].s.start
                                    - src->dim[i].lower_bound)
                                   * src->dim[i]._stride
                                   * riter->item_size;
                  break;
                case CAF_ARR_REF_SINGLE:
                  delta = 1;
                  remote_memptr += (riter->u.a.dim[i].s.start
                                    - src->dim[i].lower_bound)
                                   * src->dim[i]._stride
                                   * riter->item_size;
                  break;
                case CAF_ARR_REF_OPEN_END:
                  COMPUTE_NUM_ITEMS (delta,
                                     riter->u.a.dim[i].s.stride,
                                     riter->u.a.dim[i].s.start,
                                     src->dim[i]._ubound);
                  remote_memptr += (riter->u.a.dim[i].s.start
                                    - src->dim[i].lower_bound)
                                   * src->dim[i]._stride
                                   * riter->item_size;
                  break;
                case CAF_ARR_REF_OPEN_START:
                  COMPUTE_NUM_ITEMS (delta,
                                     riter->u.a.dim[i].s.stride,
                                     src->dim[i].lower_bound,
                                     riter->u.a.dim[i].s.end);
                  /* The memptr stays unchanged when ref'ing the first element
                     in a dimension.  */
                  break;
                default:
                  caf_runtime_error (unknownarrreftype, stat, NULL, 0);
                  return;
                }
              if (delta <= 0)
                return;
              /* Check the various properties of the destination array.
                 Is an array expected and present?  */
              if (delta > 1 && dst_rank == 0)
                {
                  /* No, an array is required, but not provided.  */
                  caf_runtime_error (extentoutofrange, stat, NULL, 0);
                  return;
                }
              /* When dst is an array.  */
              if (dst_rank > 0)
                {
                  /* Check that dst_cur_dim is valid for dst.  Can be
                     superceeded only by scalar data.  */
                  if (dst_cur_dim >= dst_rank && delta != 1)
                    {
                      caf_runtime_error (rankoutofrange, stat, NULL, 0);
                      return;
                    }
                  /* Do further checks, when the source is not scalar.  */
                  else if (delta != 1)
                    {
                      /* Check that the extent is not scalar and we are not in
                         an array ref for the dst side.  */
                      if (!in_array_ref)
                        {
                          /* Check that this is the non-scalar extent.  */
                          if (!array_extent_fixed)
                            {
                              /* In an array extent now.  */
                              in_array_ref = true;
                              /* Check that we haven't skipped any scalar
                                 dimensions yet and that the dst is
                                 compatible.  */
                              if (i > 0
                                  && dst_rank == GFC_DESCRIPTOR_RANK (src))
                                {
                                  if (dst_reallocatable)
                                    {
                                      /* Dst is reallocatable, which means that
                                         the bounds are not set.  Set them.  */
                                      for (dst_cur_dim= 0; dst_cur_dim < (int)i;
                                           ++dst_cur_dim)
                                        {
                                          dst->dim[dst_cur_dim].lower_bound = 1;
                                          dst->dim[dst_cur_dim]._ubound = 1;
                                          dst->dim[dst_cur_dim]._stride = 1;
                                        }
                                    }
                                  else
                                    dst_cur_dim = i;
                                }
                              /* Else press thumbs, that there are enough
                                 dimensional refs to come.  Checked below.  */
                            }
                          else
                            {
                              caf_runtime_error (doublearrayref, stat, NULL,
                                                 0);
                              return;
                            }
                        }
                      /* When the realloc is required, then no extent may have
                         been set.  */
                      extent_mismatch = realloc_required
                                        || GFC_DESCRIPTOR_EXTENT (dst, dst_cur_dim) != delta;
                      /* When it already known, that a realloc is needed or
                         the extent does not match the needed one.  */
                      if (realloc_required || realloc_needed
                          || extent_mismatch)
                        {
                          /* Check whether dst is reallocatable.  */
                          if (unlikely (!dst_reallocatable))
                            {
                              caf_runtime_error (nonallocextentmismatch, stat,
                                                 NULL, 0, delta,
                                                 GFC_DESCRIPTOR_EXTENT (dst,
                                                                        dst_cur_dim));
                              return;
                            }
                          /* Only report an error, when the extent needs to be
                             modified, which is not allowed.  */
                          else if (!dst_reallocatable && extent_mismatch)
                            {
                              caf_runtime_error (extentoutofrange, stat, NULL,
                                                 0);
                              return;
                            }
                          realloc_needed = true;
                        }
                      /* Only change the extent when it does not match.  This is
                         to prevent resetting given array bounds.  */
                      if (extent_mismatch)
                        {
                          dst->dim[dst_cur_dim].lower_bound = 1;
                          dst->dim[dst_cur_dim]._ubound = delta;
                          dst->dim[dst_cur_dim]._stride = size;
                        }
                    }

                  /* Only increase the dim counter, when in an array ref.  */
                  if (in_array_ref && dst_cur_dim < dst_rank)
                    ++dst_cur_dim;
                }
              size *= (ptrdiff_t)delta;
            }
          if (in_array_ref)
            {
              array_extent_fixed = true;
              in_array_ref = false;
            }
          break;
        case CAF_REF_STATIC_ARRAY:
          for (i = 0; riter->u.a.mode[i] != CAF_ARR_REF_NONE; ++i)
            {
              switch (riter->u.a.mode[i])
                {
                case CAF_ARR_REF_VECTOR:
                  delta = riter->u.a.dim[i].v.nvec;
#define KINDCASE(kind, type) case kind: \
                    remote_memptr += ((type *)riter->u.a.dim[i].v.vector)[0] \
                        * riter->item_size; \
                  break

                  switch (riter->u.a.dim[i].v.kind)
                    {
                    KINDCASE (1, int8_t);
                    KINDCASE (2, int16_t);
                    KINDCASE (4, int32_t);
                    KINDCASE (8, int64_t);
#if HAVE_GFC_INTEGER_16
                    KINDCASE (16, __int128);
#endif
                    default:
                      caf_runtime_error (vecrefunknownkind, stat, NULL, 0);
                      return;
                    }
#undef KINDCASE
                  break;
                case CAF_ARR_REF_FULL:
                  delta = riter->u.a.dim[i].s.end / riter->u.a.dim[i].s.stride
                          + 1;
                  /* The memptr stays unchanged when ref'ing the first element
                     in a dimension.  */
                  break;
                case CAF_ARR_REF_RANGE:
                  COMPUTE_NUM_ITEMS (delta,
                                     riter->u.a.dim[i].s.stride,
                                     riter->u.a.dim[i].s.start,
                                     riter->u.a.dim[i].s.end);
                  remote_memptr += riter->u.a.dim[i].s.start
                                   * riter->u.a.dim[i].s.stride
                                   * riter->item_size;
                  break;
                case CAF_ARR_REF_SINGLE:
                  delta = 1;
                  remote_memptr += riter->u.a.dim[i].s.start
                                   * riter->u.a.dim[i].s.stride
                                   * riter->item_size;
                  break;
                case CAF_ARR_REF_OPEN_END:
                  /* This and OPEN_START are mapped to a RANGE and therefore
                     can not occur here.  */
                case CAF_ARR_REF_OPEN_START:
                default:
                  caf_runtime_error (unknownarrreftype, stat, NULL, 0);
                  return;
                }
              if (delta <= 0)
                return;
              /* Check the various properties of the destination array.
                 Is an array expected and present?  */
              if (delta > 1 && dst_rank == 0)
                {
                  /* No, an array is required, but not provided.  */
                  caf_runtime_error (extentoutofrange, stat, NULL, 0);
                  return;
                }
              /* When dst is an array.  */
              if (dst_rank > 0)
                {
                  /* Check that dst_cur_dim is valid for dst.  Can be
                     superceeded only by scalar data.  */
                  if (dst_cur_dim >= dst_rank && delta != 1)
                    {
                      caf_runtime_error (rankoutofrange, stat, NULL, 0);
                      return;
                    }
                  /* Do further checks, when the source is not scalar.  */
                  else if (delta != 1)
                    {
                      /* Check that the extent is not scalar and we are not in
                         an array ref for the dst side.  */
                      if (!in_array_ref)
                        {
                          /* Check that this is the non-scalar extent.  */
                          if (!array_extent_fixed)
                            {
                              /* In an array extent now.  */
                              in_array_ref = true;
                              /* The dst is not reallocatable, so nothing more
                                 to do, then correct the dim counter.  */
                              dst_cur_dim = i;
                            }
                          else
                            {
                              caf_runtime_error (doublearrayref, stat, NULL,
                                                 0);
                              return;
                            }
                        }
                      /* When the realloc is required, then no extent may have
                         been set.  */
                      extent_mismatch = realloc_required
                                        || GFC_DESCRIPTOR_EXTENT (dst, dst_cur_dim) != delta;
                      /* When it is already known, that a realloc is needed or
                         the extent does not match the needed one.  */
                      if (realloc_required || realloc_needed
                          || extent_mismatch)
                        {
                          /* Check whether dst is reallocatable.  */
                          if (unlikely (!dst_reallocatable))
                            {
                              caf_runtime_error (nonallocextentmismatch, stat,
                                                 NULL, 0, delta,
                                                 GFC_DESCRIPTOR_EXTENT (dst,
                                                                        dst_cur_dim));
                              return;
                            }
                          /* Only report an error, when the extent needs to be
                             modified, which is not allowed.  */
                          else if (!dst_reallocatable && extent_mismatch)
                            {
                              caf_runtime_error (extentoutofrange, stat, NULL,
                                                 0);
                              return;
                            }
                          realloc_needed = true;
                        }
                      /* Only change the extent when it does not match.  This is
                         to prevent resetting given array bounds.  */
                      if (extent_mismatch)
                        {
                          dst->dim[dst_cur_dim].lower_bound = 1;
                          dst->dim[dst_cur_dim]._ubound = delta;
                          dst->dim[dst_cur_dim]._stride = size;
                        }
                    }
                  /* Only increase the dim counter, when in an array ref.  */
                  if (in_array_ref && dst_cur_dim < dst_rank)
                    ++dst_cur_dim;
                }
              size *= (ptrdiff_t)delta;
            }
          if (in_array_ref)
            {
              array_extent_fixed = true;
              in_array_ref = false;
            }
          break;
        default:
          caf_runtime_error (unknownreftype, stat, NULL, 0);
          return;
        }
      src_size = riter->item_size;
      riter = riter->next;
    }
  if (size == 0 || src_size == 0)
    return;
  /* Postcondition:
     - size contains the number of elements to store in the destination array,
     - src_size gives the size in bytes of each item in the destination array.
  */

  if (realloc_needed)
    {
      if (!array_extent_fixed)
        {
          /* This can happen only, when the result is scalar.  */
          for (dst_cur_dim = 0; dst_cur_dim < dst_rank; ++dst_cur_dim)
            {
              dst->dim[dst_cur_dim].lower_bound = 1;
              dst->dim[dst_cur_dim]._ubound = 1;
              dst->dim[dst_cur_dim]._stride = 1;
            }
        }
      dst->base_addr = malloc (size * GFC_DESCRIPTOR_SIZE (dst));
      if (unlikely (dst->base_addr == NULL))
        {
          caf_runtime_error (cannotallocdst, stat, size * GFC_DESCRIPTOR_SIZE (dst));
          return;
        }
    }

  /* Reset the token.  */
  mpi_token = (mpi_caf_token_t *) token;
  remote_memptr = mpi_token->memptr;
  dst_index = 0;
#ifdef EXTRA_DEBUG_OUTPUT
  fprintf (stderr, "%d/%d: %s() dst_rank: %d\n", caf_this_image, caf_num_images,
           __FUNCTION__, GFC_DESCRIPTOR_RANK (dst));
  for (i = 0; i < GFC_DESCRIPTOR_RANK (dst); ++i)
    fprintf (stderr, "%d/%d: %s() dst_dim[%d] = (%d, %d)\n", caf_this_image, caf_num_images,
             __FUNCTION__, i, dst->dim[i].lower_bound, dst->dim[i]._ubound);
#endif
  i = 0;
  dprint ("%d/%d: get_by_ref() calling get_for_ref.\n", caf_this_image,
          caf_num_images);
  get_for_ref (refs, &i, dst_index, mpi_token, dst, mpi_token->desc,
               dst->base_addr, remote_memptr, 0, 0, dst_kind, src_kind, 0, 0,
               1, stat, remote_image, false, false);
  CAF_Win_unlock (remote_image, global_dynamic_win);
  CAF_Win_unlock (remote_image, mpi_token->memptr_win);
}


void
PREFIX(send_by_ref) (caf_token_t token, int image_index,
                         gfc_descriptor_t *src, caf_reference_t *refs,
                         int dst_kind, int src_kind, bool may_require_tmp,
                         bool dst_reallocatable, int *stat)
{
  unimplemented_alloc_comps_message("caf_send_by_ref()");
  // Make sure we exit
  terminate_internal (1, 1);
}


void
PREFIX(sendget_by_ref) (caf_token_t dst_token, int dst_image_index,
                            caf_reference_t *dst_refs, caf_token_t src_token,
                            int src_image_index, caf_reference_t *src_refs,
                            int dst_kind, int src_kind,
                            bool may_require_tmp, int *dst_stat, int *src_stat)
{
  unimplemented_alloc_comps_message("caf_sendget_by_ref()");
  // Make sure we exit
  terminate_internal (1, 1);
}

int
PREFIX(is_present) (caf_token_t token, int image_index, caf_reference_t *refs)
{
  const char unsupportedRefType[] = "Unsupported ref-type in caf_is_present().";
  const char unexpectedEndOfRefs[] = "Unexpected end of references in caf_is_present.";
  const char remotesInnerRefNA[] = "Memory referenced on the remote image is not allocated.";
  const int ptr_size = sizeof (void *);
  const int remote_image = image_index - 1;
  mpi_caf_token_t *mpi_token = (mpi_caf_token_t *)token;
  ptrdiff_t local_offset = 0;
  void *remote_memptr = NULL, *remote_base_memptr = NULL;
  bool carryOn = true, firstDesc = true;
  caf_reference_t *riter = refs, *prev;
  size_t i, ref_rank;
  gfc_max_dim_descriptor_t src_desc;

  while (carryOn && riter)
    {
      switch (riter->type)
        {
        case CAF_REF_COMPONENT:
          if (riter->u.c.caf_token_offset)
            {
              CAF_Win_lock (MPI_LOCK_SHARED, remote_image, mpi_token->memptr_win);
              MPI_Get (&remote_memptr, ptr_size, MPI_BYTE, remote_image,
                       local_offset + riter->u.c.offset,
                       ptr_size, MPI_BYTE, mpi_token->memptr_win);
              CAF_Win_unlock (remote_image, mpi_token->memptr_win);
              dprint ("%d/%d: %s() Got first remote address %p from offset %d\n",
                      caf_this_image, caf_num_images, __FUNCTION__, remote_memptr,
                      local_offset);
              local_offset = 0;
              carryOn = false;
            }
          else
            local_offset += riter->u.c.offset;
          break;
        case CAF_REF_ARRAY:
          {
            const gfc_descriptor_t *src = (gfc_descriptor_t *)(mpi_token->memptr + local_offset);
            for (i = 0; riter->u.a.mode[i] != CAF_ARR_REF_NONE; ++i)
              {
                switch (riter->u.a.mode[i])
                  {
                  case CAF_ARR_REF_FULL:
                    /* The local_offset stays unchanged when ref'ing the first element
                     in a dimension.  */
                    break;
                  case CAF_ARR_REF_SINGLE:
                    local_offset += (riter->u.a.dim[i].s.start
                                     - src->dim[i].lower_bound)
                                    * src->dim[i]._stride
                                    * riter->item_size;
                    break;
                  case CAF_ARR_REF_VECTOR:
                  case CAF_ARR_REF_RANGE:
                  case CAF_ARR_REF_OPEN_END:
                  case CAF_ARR_REF_OPEN_START:
                    /* Intentionally fall through, because these are not suported
                   * here. */
                  default:
                    caf_runtime_error (unsupportedRefType);
                    return false;
                  }
              }
          }
          break;
        case CAF_REF_STATIC_ARRAY:
          for (i = 0; riter->u.a.mode[i] != CAF_ARR_REF_NONE; ++i)
            {
              switch (riter->u.a.mode[i])
                {
                case CAF_ARR_REF_FULL:
                  /* The local_offset stays unchanged when ref'ing the first element
                     in a dimension.  */
                  break;
                case CAF_ARR_REF_SINGLE:
                  local_offset += riter->u.a.dim[i].s.start
                                  * riter->u.a.dim[i].s.stride
                                  * riter->item_size;
                  break;
                case CAF_ARR_REF_VECTOR:
                case CAF_ARR_REF_RANGE:
                case CAF_ARR_REF_OPEN_END:
                case CAF_ARR_REF_OPEN_START:
                default:
                  caf_runtime_error (unsupportedRefType);
                  return false;
                }
            }
          break;
        default:
          caf_runtime_error (unsupportedRefType);
          return false;
        }
      prev = riter;
      riter = riter->next;
    }

  if (carryOn)
    {
      // This can only happen, when riter == NULL.
      caf_runtime_error (unexpectedEndOfRefs);
    }

  CAF_Win_lock (MPI_LOCK_SHARED, remote_image, global_dynamic_win);
  if (remote_memptr != NULL)
    remote_base_memptr = remote_memptr + local_offset;

  dprint ("%d/%d: %s() Remote desc address is %p from remote memptr %p and offset %d\n",
          caf_this_image, caf_num_images, __FUNCTION__, remote_base_memptr,
          remote_memptr, local_offset);

  while (riter)
    {
      switch (riter->type)
        {
        case CAF_REF_COMPONENT:
          /* After reffing the first allocatable/pointer component, descriptors
          need to be picked up from the global_win.  */
          firstDesc = firstDesc && riter->u.c.caf_token_offset == 0;
          local_offset += riter->u.c.offset;
          remote_base_memptr = remote_memptr + local_offset;
          MPI_Get (&remote_memptr, ptr_size, MPI_BYTE, remote_image,
                   (MPI_Aint)remote_base_memptr,
                   ptr_size, MPI_BYTE, global_dynamic_win);
          dprint ("%d/%d: %s() Got remote address %p from offset %d and base memptr %p\n",
                  caf_this_image, caf_num_images, __FUNCTION__, remote_memptr,
                  local_offset, remote_base_memptr);
          local_offset = 0;
          break;
        case CAF_REF_ARRAY:
          if (remote_base_memptr == NULL)
            {
              /* Refing an unallocated array ends in a full_ref. Check that this
               * is true. Error when not full-refing. */
              for (i = 0; riter->u.a.mode[i] != CAF_ARR_REF_NONE; ++i)
                if (riter->u.a.mode[i] != CAF_ARR_REF_FULL)
                  break;
              if (riter->u.a.mode[i] != CAF_ARR_REF_NONE)
                caf_runtime_error(remotesInnerRefNA);
              break;
            }
          if (firstDesc)
            {
              /* The first descriptor is accessible by the
              mpi_token->memptr_win.
              Count the dims to fetch.  */
              for (ref_rank = 0; riter->u.a.mode[ref_rank] != CAF_ARR_REF_NONE; ++ref_rank) ;
              dprint ("%d/%d: %s() Getting remote descriptor of rank %d from win: %p, sizeof() %d\n",
                      caf_this_image, caf_num_images, __FUNCTION__,
                      ref_rank, mpi_token->memptr_win, sizeof_desc_for_rank(ref_rank));
              MPI_Get (&src_desc, sizeof_desc_for_rank(ref_rank), MPI_BYTE, remote_image,
                       local_offset, sizeof_desc_for_rank(ref_rank),
                       MPI_BYTE, mpi_token->memptr_win);
              firstDesc = false;
            }
          else
            {
              /* All inner descriptors go by the dynamic window.
              Count the dims to fetch.  */
              for (ref_rank = 0; riter->u.a.mode[ref_rank] != CAF_ARR_REF_NONE; ++ref_rank) ;
              dprint ("%d/%d: %s() Getting remote descriptor of rank %d from: %p, sizeof() %d\n",
                      caf_this_image, caf_num_images, __FUNCTION__,
                      ref_rank, remote_base_memptr, sizeof_desc_for_rank(ref_rank));
              MPI_Get (&src_desc, sizeof_desc_for_rank(ref_rank), MPI_BYTE, remote_image,
                       (MPI_Aint)remote_base_memptr, sizeof_desc_for_rank(ref_rank),
                       MPI_BYTE, global_dynamic_win);
            }
#ifdef EXTRA_DEBUG_OUTPUT
          fprintf (stderr, "%d/%d: %s() remote desc rank: %d (ref_rank: %d)\n", caf_this_image, caf_num_images,
                   __FUNCTION__, GFC_DESCRIPTOR_RANK (&src_desc), ref_rank);
          for (i = 0; i < GFC_DESCRIPTOR_RANK (&src_desc); ++i)
            fprintf (stderr, "%d/%d: %s() remote desc dim[%d] = (lb = %d, ub = %d, stride = %d)\n", caf_this_image, caf_num_images,
                     __FUNCTION__, i, src_desc.dim[i].lower_bound, src_desc.dim[i]._ubound, src_desc.dim[i]._stride);
#endif

          for (i = 0; riter->u.a.mode[i] != CAF_ARR_REF_NONE; ++i)
            {
              switch (riter->u.a.mode[i])
                {
                case CAF_ARR_REF_FULL:
                  /* The local_offset stays unchanged when ref'ing the first element
                     in a dimension.  */
                  break;
                case CAF_ARR_REF_SINGLE:
                  local_offset += (riter->u.a.dim[i].s.start
                                   - src_desc.dim[i].lower_bound)
                                  * src_desc.dim[i]._stride
                                  * riter->item_size;
                  break;
                case CAF_ARR_REF_VECTOR:
                case CAF_ARR_REF_RANGE:
                case CAF_ARR_REF_OPEN_END:
                case CAF_ARR_REF_OPEN_START:
                  /* Intentionally fall through, because these are not suported
                   * here. */
                default:
                  caf_runtime_error (unsupportedRefType);
                  CAF_Win_unlock (remote_image, global_dynamic_win);
                  return false;
                }
            }
          break;
        case CAF_REF_STATIC_ARRAY:
          for (i = 0; riter->u.a.mode[i] != CAF_ARR_REF_NONE; ++i)
            {
              switch (riter->u.a.mode[i])
                {
                case CAF_ARR_REF_FULL:
                  /* The memptr stays unchanged when ref'ing the first element
                     in a dimension.  */
                  break;
                case CAF_ARR_REF_SINGLE:
                  local_offset += riter->u.a.dim[i].s.start
                                  * riter->u.a.dim[i].s.stride
                                  * riter->item_size;
                  break;
                case CAF_ARR_REF_VECTOR:
                case CAF_ARR_REF_RANGE:
                case CAF_ARR_REF_OPEN_END:
                case CAF_ARR_REF_OPEN_START:
                default:
                  caf_runtime_error (unsupportedRefType);
                  CAF_Win_unlock (remote_image, global_dynamic_win);
                  return false;
                }
            }
          break;
        default:
          caf_runtime_error (unsupportedRefType);
          CAF_Win_unlock (remote_image, global_dynamic_win);
          return false;
        }
      riter = riter->next;
    }
  CAF_Win_unlock (remote_image, global_dynamic_win);

  dprint ("%d/%d: %s() Got remote_memptr: %p\n",
          caf_this_image, caf_num_images, __FUNCTION__, remote_memptr);
  return remote_memptr != NULL;
}
#endif


/* SYNC IMAGES. Note: SYNC IMAGES(*) is passed as count == -1 while
   SYNC IMAGES([]) has count == 0. Note further that SYNC IMAGES(*)
   is not semantically equivalent to SYNC ALL. */

void
PREFIX (sync_images) (int count, int images[], int *stat, char *errmsg,
                     int errmsg_len)
{
  sync_images_internal (count, images, stat, errmsg, errmsg_len, false);
}

static void
sync_images_internal (int count, int images[], int *stat, char *errmsg,
                      int errmsg_len, bool internal)
{
  int ierr = 0, i = 0, j = 0, int_zero = 0, done_count = 0;
  MPI_Status s;

#ifdef WITH_FAILED_IMAGES
  no_stopped_images_check_in_errhandler = true;
#endif
  dprint ("%d/%d: Entering %s.\n", caf_this_image, caf_num_images, __FUNCTION__);
  if (count == 0 || (count == 1 && images[0] == caf_this_image))
    {
      if (stat)
        *stat = 0;
#ifdef WITH_FAILED_IMAGES
      no_stopped_images_check_in_errhandler = false;
#endif
      dprint ("%d/%d: Leaving %s early.\n", caf_this_image, caf_num_images, __FUNCTION__);
      return;
    }

  /* halt execution if sync images contains duplicate image numbers */
  for(i = 0; i < count; ++i)
    for(j = 0; j < i; ++j)
      if(images[i] == images[j])
	{
	  ierr = STAT_DUP_SYNC_IMAGES;
	  if(stat)
	    *stat = ierr;
	  goto sync_images_err_chk;
	}

#ifdef GFC_CAF_CHECK
  {
    for (i = 0; i < count; ++i)
      if (images[i] < 1 || images[i] > caf_num_images)
        {
          fprintf (stderr, "COARRAY ERROR: Invalid image index %d to SYNC "
                   "IMAGES", images[i]);
          terminate_internal (1, 1);
        }
  }
#endif

  if (unlikely (caf_is_finalized))
    ierr = STAT_STOPPED_IMAGE;
  else
    {
       if(count == -1)
        {
          count = caf_num_images - 1;
          images = images_full;
        }

#if defined(NONBLOCKING_PUT) && !defined(CAF_MPI_LOCK_UNLOCK)
      explicit_flush();
#endif

#ifdef WITH_FAILED_IMAGES
      {
        int flag;
        /* Provoke detecting process fails. */
        MPI_Test (&alive_request, &flag, MPI_STATUS_IGNORE);
      }
#endif
      /* A rather simple way to synchronice:
         - expect all images to sync with receiving an int,
         - on the other side, send all processes to sync with an int,
         - when the int received is STAT_STOPPED_IMAGE the return immediately,
           else wait until all images in the current set of images have send
           some data, i.e., synced.

         This approach as best as possible implements the syncing of different
         sets of images and figuring that an image has stopped.  MPI does not
         provide any direct means of syncing non-coherent sets of images.
         The groups/communicators of MPI always need to be consistent, i.e.,
         have the same members on all images participating.  This is
         contradictiory to the sync images statement, where syncing, e.g., in a
         ring pattern is possible.

         This implementation guarantees, that as long as no image is stopped
         an image only is allowed to continue, when all its images to sync to
         also have reached a sync images statement.  This implementation makes
         no assumption when the image continues or in which order synced
         images continue.  */
      for (i = 0; i < count; ++i)
        /* Need to have the request handlers contigously in the handlers
           array or waitany below will trip about the handler as illegal.  */
        ierr = MPI_Irecv (&arrived[images[i] - 1], 1, MPI_INT, images[i] - 1,
            MPI_TAG_CAF_SYNC_IMAGES, CAF_COMM_WORLD, &sync_handles[i]);
      for (i = 0; i < count; ++i)
        MPI_Send (&int_zero, 1, MPI_INT, images[i] - 1, MPI_TAG_CAF_SYNC_IMAGES,
                  CAF_COMM_WORLD);
      done_count = 0;
      while (done_count < count)
        {
          ierr = MPI_Waitany (count, sync_handles, &i, &s);
          if (ierr == MPI_SUCCESS && i != MPI_UNDEFINED)
            {
              ++done_count;
              if (ierr == MPI_SUCCESS && arrived[s.MPI_SOURCE] == STAT_STOPPED_IMAGE)
                {
                  /* Possible future extension: Abort pending receives.  At the
                     moment the receives are discarded by the program
                     termination.  For the tested mpi-implementation this is ok.
                   */
                  ierr = STAT_STOPPED_IMAGE;
                  break;
                }
            }
          else if (ierr != MPI_SUCCESS)
#ifdef WITH_FAILED_IMAGES
            {
              int err;
              MPI_Error_class (ierr, &err);
              if (err == MPIX_ERR_PROC_FAILED)
                {
                  int flag;
                  dprint ("%d/%d: Image failed, provoking error handling.\n",
                         caf_this_image, caf_num_images);
                  ierr = STAT_FAILED_IMAGE;
                  /* Provoke detecting process fails. */
                  MPI_Test (&alive_request, &flag, MPI_STATUS_IGNORE);
                }
              break;
            }
#else
            break;
#endif
        }
    }

sync_images_err_chk:
#ifdef WITH_FAILED_IMAGES
  no_stopped_images_check_in_errhandler = false;
#endif
  dprint ("%d/%d: Leaving %s.\n", caf_this_image, caf_num_images, __FUNCTION__);
  if (stat)
    *stat = ierr;
#ifdef WITH_FAILED_IMAGES
  else if (ierr == STAT_FAILED_IMAGE)
    terminate_internal (ierr, 0);
#endif

  if (ierr != 0 && ierr != STAT_FAILED_IMAGE)
    {
      char *msg;
      if (caf_is_finalized)
	msg = "SYNC IMAGES failed - there are stopped images";
      else
        msg = "SYNC IMAGES failed";

      if (errmsg_len > 0)
        {
          int len = ((int) strlen (msg) > errmsg_len) ? errmsg_len
                                                      : (int) strlen (msg);
          memcpy (errmsg, msg, len);
          if (errmsg_len > len)
            memset (&errmsg[len], ' ', errmsg_len-len);
        }
      else if (!internal && stat == NULL)
        caf_runtime_error (msg);
    }
}


#define GEN_REDUCTION(name, datatype, operator) \
static void \
name (datatype *invec, datatype *inoutvec, int *len, \
               MPI_Datatype *datatype __attribute__ ((unused))) \
{ \
  int i; \
  for (i = 0; i < len; i++) \
    operator; \
}

#define REFERENCE_FUNC(TYPE) TYPE ## _by_reference
#define VALUE_FUNC(TYPE) TYPE ## _by_value

#define GEN_COREDUCE(name, dt)			\
static void \
name##_by_reference_adapter (void *invec, void *inoutvec, int *len, \
      MPI_Datatype *datatype)		     \
{ \
  int i;	     \
  for(i=0;i<*len;i++)				\
    {								\
      *((dt*)inoutvec) = (dt)(REFERENCE_FUNC(dt)((dt *)invec, (dt *)inoutvec));	\
     invec+=sizeof(dt); inoutvec+=sizeof(dt);	\
   } \
} \
static void \
name##_by_value_adapter (void *invec, void *inoutvec, int *len, \
      MPI_Datatype *datatype)		     \
{ \
  int i;	     \
  for(i=0;i<*len;i++)				\
    {								\
      *((dt*)inoutvec) = (dt)(VALUE_FUNC(dt)(*(dt *)invec, *(dt *)inoutvec));	\
     invec+=sizeof(dt); inoutvec+=sizeof(dt);	\
   } \
}

GEN_COREDUCE (redux_int32, int32_t)
GEN_COREDUCE (redux_real32, float)
GEN_COREDUCE (redux_real64, double)

static void \
redux_char_by_reference_adapter (void *invec, void *inoutvec, int *len,
      MPI_Datatype *datatype)
{
  MPI_Aint string_len;
  MPI_Type_extent(*datatype, &string_len);
  for(int i = 0; i < *len; i++)
    {
      /* The length of the result is fixed, i.e., no deferred string length is
       * allowed there.  */
      REFERENCE_FUNC(char)((char *)inoutvec, string_len, (char *)invec, (char *)inoutvec, string_len, string_len);
      invec += sizeof(char) * string_len;
      inoutvec += sizeof(char) * string_len;
    }
}

#ifndef MPI_INTEGER1
GEN_REDUCTION (do_sum_int1, int8_t, inoutvec[i] += invec[i])
GEN_REDUCTION (do_min_int1, int8_t,
               inoutvec[i] = invec[i] >= inoutvec[i] ? inoutvec[i] : invec[i])
GEN_REDUCTION (do_max_int1, int8_t,
               inoutvec[i] = invec[i] <= inoutvec[i] ? inoutvec[i] : invec[i])
#endif

#ifndef MPI_INTEGER2
GEN_REDUCTION (do_sum_int1, int16_t, inoutvec[i] += invec[i])
GEN_REDUCTION (do_min_int1, int16_t,
               inoutvec[i] = invec[i] >= inoutvec[i] ? inoutvec[i] : invec[i])
GEN_REDUCTION (do_max_int1, int16_t,
               inoutvec[i] = invec[i] <= inoutvec[i] ? inoutvec[i] : invec[i])
#endif

#if defined(MPI_INTEGER16) && defined(GFC_INTEGER_16)
GEN_REDUCTION (do_sum_int1, GFC_INTEGER_16, inoutvec[i] += invec[i])
GEN_REDUCTION (do_min_int1, GFC_INTEGER_16,
               inoutvec[i] = invec[i] >= inoutvec[i] ? inoutvec[i] : invec[i])
GEN_REDUCTION (do_max_int1, GFC_INTEGER_16,
               inoutvec[i] = invec[i] <= inoutvec[i] ? inoutvec[i] : invec[i])
#endif

#if defined(GFC_DTYPE_REAL_10) \
    || (!defined(GFC_DTYPE_REAL_10)  && defined(GFC_DTYPE_REAL_16))
GEN_REDUCTION (do_sum_real10, long double, inoutvec[i] += invec[i])
GEN_REDUCTION (do_min_real10, long double,
               inoutvec[i] = invec[i] >= inoutvec[i] ? inoutvec[i] : invec[i])
GEN_REDUCTION (do_max_real10, long double,
               inoutvec[i] = invec[i] <= inoutvec[i] ? inoutvec[i] : invec[i])
GEN_REDUCTION (do_sum_complex10, _Complex long double, inoutvec[i] += invec[i])
GEN_REDUCTION (do_min_complex10, _Complex long double,
               inoutvec[i] = invec[i] >= inoutvec[i] ? inoutvec[i] : invec[i])
GEN_REDUCTION (do_max_complex10, _Complex long double,
               inoutvec[i] = invec[i] <= inoutvec[i] ? inoutvec[i] : invec[i])
#endif

#if defined(GFC_DTYPE_REAL_10) && defined(GFC_DTYPE_REAL_16)
GEN_REDUCTION (do_sum_real10, __float128, inoutvec[i] += invec[i])
GEN_REDUCTION (do_min_real10, __float128,
               inoutvec[i] = invec[i] >= inoutvec[i] ? inoutvec[i] : invec[i])
GEN_REDUCTION (do_max_real10, __float128,
               inoutvec[i] = invec[i] <= inoutvec[i] ? inoutvec[i] : invec[i])
GEN_REDUCTION (do_sum_complex10, _Complex __float128, inoutvec[i] += invec[i])
GEN_REDUCTION (do_mincomplexl10, _Complex __float128,
               inoutvec[i] = invec[i] >= inoutvec[i] ? inoutvec[i] : invec[i])
GEN_REDUCTION (do_max_complex10, _Complex __float128,
               inoutvec[i] = invec[i] <= inoutvec[i] ? inoutvec[i] : invec[i])
#endif
#undef GEN_REDUCTION


static MPI_Datatype
get_MPI_datatype (gfc_descriptor_t *desc, int char_len)
{
  /* FIXME: Better check whether the sizes are okay and supported;
     MPI3 adds more types, e.g. MPI_INTEGER1.  */
  switch (GFC_DTYPE_TYPE_SIZE (desc))
    {
#ifdef MPI_INTEGER1
    case GFC_DTYPE_INTEGER_1:
      return MPI_INTEGER1;
#endif
#ifdef MPI_INTEGER2
    case GFC_DTYPE_INTEGER_2:
      return MPI_INTEGER2;
#endif
    case GFC_DTYPE_INTEGER_4:
#ifdef MPI_INTEGER4
      return MPI_INTEGER4;
#else
      return MPI_INTEGER;
#endif
#ifdef MPI_INTEGER8
    case GFC_DTYPE_INTEGER_8:
      return MPI_INTEGER8;
#endif
#if defined(MPI_INTEGER16) && defined(GFC_DTYPE_INTEGER_16)
    case GFC_DTYPE_INTEGER_16:
      return MPI_INTEGER16;
#endif

    case GFC_DTYPE_LOGICAL_4:
      return MPI_INT;

    case GFC_DTYPE_REAL_4:
#ifdef MPI_REAL4
      return MPI_REAL4;
#else
      return MPI_REAL;
#endif
    case GFC_DTYPE_REAL_8:
#ifdef MPI_REAL8
      return MPI_REAL8;
#else
      return MPI_DOUBLE_PRECISION;
#endif

/* Note that we cannot use REAL_16 as we do not know whether it matches REAL(10)
   or REAL(16), which have both the same bitsize and only make use of less
   bits.  */
    case GFC_DTYPE_COMPLEX_4:
      return MPI_COMPLEX;
    case GFC_DTYPE_COMPLEX_8:
      return MPI_DOUBLE_COMPLEX;
    }
/* gfortran passes character string arguments with a
   GFC_DTYPE_TYPE_SIZE == GFC_TYPE_CHARACTER + 64*strlen
*/
  if ( (GFC_DTYPE_TYPE_SIZE(desc)-GFC_DTYPE_CHARACTER)%64==0 )
    {
      MPI_Datatype string;
      if (char_len == 0)
	char_len = GFC_DESCRIPTOR_SIZE (desc);
      MPI_Type_contiguous(char_len, MPI_CHARACTER, &string);
      MPI_Type_commit(&string);
      return string;
    }

  caf_runtime_error ("Unsupported data type in collective: %ld\n",GFC_DTYPE_TYPE_SIZE (desc));
  return 0;
}


static void
internal_co_reduce (MPI_Op op, gfc_descriptor_t *source, int result_image, int *stat,
	     char *errmsg, int src_len, int errmsg_len)
{
  size_t i, size;
  int j, ierr;
  int rank = GFC_DESCRIPTOR_RANK (source);

  MPI_Datatype datatype = get_MPI_datatype (source, src_len);

  size = 1;
  for (j = 0; j < rank; j++)
    {
      ptrdiff_t dimextent = source->dim[j]._ubound
                            - source->dim[j].lower_bound + 1;
      if (dimextent < 0)
        dimextent = 0;
      size *= dimextent;
    }

  if (rank == 0 || PREFIX (is_contiguous) (source))
    {
      if (result_image == 0)
	ierr = MPI_Allreduce (MPI_IN_PLACE, source->base_addr, size, datatype,
                              op, CAF_COMM_WORLD);
      else if (result_image == caf_this_image)
	ierr = MPI_Reduce (MPI_IN_PLACE, source->base_addr, size, datatype, op,
                           result_image-1, CAF_COMM_WORLD);
      else
	ierr = MPI_Reduce (source->base_addr, NULL, size, datatype, op,
                           result_image-1, CAF_COMM_WORLD);
      if (ierr)
        goto error;
      goto co_reduce_cleanup;
    }

  for (i = 0; i < size; i++)
    {
      ptrdiff_t array_offset_sr = 0;
      ptrdiff_t stride = 1;
      ptrdiff_t extent = 1;
      for (j = 0; j < GFC_DESCRIPTOR_RANK (source)-1; j++)
        {
          array_offset_sr += ((i / (extent*stride))
                           % (source->dim[j]._ubound
                              - source->dim[j].lower_bound + 1))
                          * source->dim[j]._stride;
          extent = (source->dim[j]._ubound - source->dim[j].lower_bound + 1);
          stride = source->dim[j]._stride;
        }
      array_offset_sr += (i / extent) * source->dim[rank-1]._stride;
      void *sr = (void *)((char *) source->base_addr
                          + array_offset_sr*GFC_DESCRIPTOR_SIZE (source));
      if (result_image == 0)
	ierr = MPI_Allreduce (MPI_IN_PLACE, sr, 1, datatype, op,
                              CAF_COMM_WORLD);
      else if (result_image == caf_this_image)
	ierr = MPI_Reduce (MPI_IN_PLACE, sr, 1, datatype, op,
                           result_image-1, CAF_COMM_WORLD);
      else
	ierr = MPI_Reduce (sr, NULL, 1, datatype, op, result_image-1,
                           CAF_COMM_WORLD);
      if (ierr)
        goto error;
    }

co_reduce_cleanup:
  if (GFC_DESCRIPTOR_TYPE (source) == BT_CHARACTER)
    MPI_Type_free (&datatype);
  if (stat)
    *stat = 0;
  return;
error:
  /* FIXME: Put this in an extra function and use it elsewhere.  */
  if (stat)
    {
      *stat = ierr;
      if (!errmsg)
        return;
    }

  int len = sizeof (err_buffer);
  MPI_Error_string (ierr, err_buffer, &len);
  if (!stat)
    {
      err_buffer[len == sizeof (err_buffer) ? len-1 : len] = '\0';
      caf_runtime_error ("CO_SUM failed with %s\n", err_buffer);
    }
  memcpy (errmsg, err_buffer, errmsg_len > len ? len : errmsg_len);
  if (errmsg_len > len)
    memset (&errmsg[len], '\0', errmsg_len - len);
}

void
PREFIX (co_broadcast) (gfc_descriptor_t *a, int source_image, int *stat, char *errmsg,
                       int errmsg_len)
{
  size_t i, size;
  int j, ierr;
  int rank = GFC_DESCRIPTOR_RANK (a);

  MPI_Datatype datatype = get_MPI_datatype (a, 0);

  size = 1;
  for (j = 0; j < rank; j++)
    {
      ptrdiff_t dimextent = a->dim[j]._ubound
                            - a->dim[j].lower_bound + 1;
      if (dimextent < 0)
        dimextent = 0;
      size *= dimextent;
    }

  if (rank == 0)
    {
      if (datatype != MPI_CHARACTER)
	ierr = MPI_Bcast(a->base_addr, size, datatype, source_image-1, CAF_COMM_WORLD);
      else
      {
        int a_length;
        if (caf_this_image==source_image)
          a_length=strlen(a->base_addr);
        /* Broadcast the string lenth */
        ierr = MPI_Bcast(&a_length, 1, MPI_INT, source_image-1, CAF_COMM_WORLD);
        if (ierr)
          goto error;
        /* Broadcast the string itself */
	ierr = MPI_Bcast(a->base_addr, a_length, datatype, source_image-1, CAF_COMM_WORLD);
      }

      if (ierr)
        goto error;
      goto co_broadcast_exit;
    }
    else if (datatype == MPI_CHARACTER) /* rank !=0  */
    {
        caf_runtime_error ("Co_broadcast of character arrays not yet supported\n");
    }

  for (i = 0; i < size; i++)
    {
      ptrdiff_t array_offset_sr = 0;
      ptrdiff_t stride = 1;
      ptrdiff_t extent = 1;
      for (j = 0; j < GFC_DESCRIPTOR_RANK (a)-1; j++)
        {
          array_offset_sr += ((i / (extent*stride))
                           % (a->dim[j]._ubound
                              - a->dim[j].lower_bound + 1))
                          * a->dim[j]._stride;
          extent = (a->dim[j]._ubound - a->dim[j].lower_bound + 1);
          stride = a->dim[j]._stride;
        }
      array_offset_sr += (i / extent) * a->dim[rank-1]._stride;
      void *sr = (void *)((char *) a->base_addr
                          + array_offset_sr*GFC_DESCRIPTOR_SIZE (a));

      ierr = MPI_Bcast(sr, 1, datatype, source_image-1, CAF_COMM_WORLD);

      if (ierr)
        goto error;
    }

co_broadcast_exit:
  if (stat)
    *stat = 0;
  if (GFC_DESCRIPTOR_TYPE(a) == BT_CHARACTER)
    MPI_Type_free(&datatype);
  return;

error:
  /* FIXME: Put this in an extra function and use it elsewhere.  */
  if (stat)
    {
      *stat = ierr;
      if (!errmsg)
        return;
    }

  int len = sizeof (err_buffer);
  MPI_Error_string (ierr, err_buffer, &len);
  if (!stat)
    {
      err_buffer[len == sizeof (err_buffer) ? len-1 : len] = '\0';
      caf_runtime_error ("CO_SUM failed with %s\n", err_buffer);
    }
  memcpy (errmsg, err_buffer, errmsg_len > len ? len : errmsg_len);
  if (errmsg_len > len)
    memset (&errmsg[len], '\0', errmsg_len - len);
}

/** The front-end function for co_reduce functionality.  It sets up the MPI_Op
 * for use in MPI_*Reduce functions. */
void
PREFIX (co_reduce) (gfc_descriptor_t *a, void *(*opr) (void *, void *), int opr_flags,
		    int result_image, int *stat, char *errmsg, int a_len, int errmsg_len)
{
  MPI_Op op;
  /* Integers and logicals can be treated the same. */
  if(GFC_DESCRIPTOR_TYPE(a) == BT_INTEGER
     || GFC_DESCRIPTOR_TYPE(a) == BT_LOGICAL)
    {
      /* When the ARG_VALUE opr_flag is set, then the user-function expects its
       * arguments to be passed by value. */
      if ((opr_flags & GFC_CAF_ARG_VALUE) > 0)
	{
	  int32_t_by_value = (typeof (VALUE_FUNC(int32_t)))opr;
	  MPI_Op_create(redux_int32_by_value_adapter, 1, &op);
	}
      else
	{
	  int32_t_by_reference = (typeof (REFERENCE_FUNC(int32_t)))opr;
	  MPI_Op_create(redux_int32_by_reference_adapter, 1, &op);
	}
    }
  /* Treat reals/doubles. */
  else if(GFC_DESCRIPTOR_TYPE(a) == BT_REAL)
    {
      /* When the ARG_VALUE opr_flag is set, then the user-function expects its
       * arguments to be passed by value. */
      if(GFC_DESCRIPTOR_SIZE(a) == sizeof(float))
  	{
	  if ((opr_flags & GFC_CAF_ARG_VALUE) > 0)
	    {
	      float_by_value = (typeof (VALUE_FUNC(float)))opr;
	      MPI_Op_create(redux_real32_by_value_adapter, 1, &op);
	    }
	  else
	    {
	      float_by_reference = (typeof (REFERENCE_FUNC(float)))opr;
	      MPI_Op_create(redux_real32_by_reference_adapter, 1, &op);
	    }
  	}
      else
	{
	  /* When the ARG_VALUE opr_flag is set, then the user-function expects its
	   * arguments to be passed by value. */
	  if ((opr_flags & GFC_CAF_ARG_VALUE) > 0)
	    {
	      double_by_value = (typeof (VALUE_FUNC(double)))opr;
	      MPI_Op_create(redux_real64_by_value_adapter, 1, &op);
	    }
	  else
	    {
	      double_by_reference = (typeof (REFERENCE_FUNC(double)))opr;
	      MPI_Op_create(redux_real64_by_reference_adapter, 1, &op);
	    }
  	}
    }
  else if (GFC_DESCRIPTOR_TYPE(a) == BT_CHARACTER)
    {
      /* Char array functions always pass by reference. */
      char_by_reference = (typeof (REFERENCE_FUNC(char)))opr;
      MPI_Op_create(redux_char_by_reference_adapter, 1, &op);
    }
  else
    {
      caf_runtime_error ("Data type not yet supported for co_reduce\n");
    }

  internal_co_reduce (op, a, result_image, stat, errmsg, a_len, errmsg_len);
}

void
PREFIX (co_sum) (gfc_descriptor_t *a, int result_image, int *stat, char *errmsg,
                 int errmsg_len)
{
  internal_co_reduce (MPI_SUM, a, result_image, stat, errmsg, 0, errmsg_len);
}


void
PREFIX (co_min) (gfc_descriptor_t *a, int result_image, int *stat, char *errmsg,
                 int src_len, int errmsg_len)
{
  internal_co_reduce (MPI_MIN, a, result_image, stat, errmsg, src_len, errmsg_len);
}


void
PREFIX (co_max) (gfc_descriptor_t *a, int result_image, int *stat,
                 char *errmsg, int src_len, int errmsg_len)
{
  internal_co_reduce (MPI_MAX, a, result_image, stat, errmsg, src_len, errmsg_len);
}


/* Locking functions */

void
PREFIX (lock) (caf_token_t token, size_t index, int image_index,
               int *acquired_lock, int *stat, char *errmsg,
               int errmsg_len)
{
  int dest_img;
  MPI_Win *p = TOKEN(token);

  if(image_index == 0)
    dest_img = caf_this_image;
  else
    dest_img = image_index;

  mutex_lock(*p, dest_img, index, stat, acquired_lock, errmsg, errmsg_len);
}


void
PREFIX (unlock) (caf_token_t token, size_t index, int image_index,
                 int *stat, char *errmsg, int errmsg_len)
{
  int dest_img;
  MPI_Win *p = TOKEN(token);

  if(image_index == 0)
    dest_img = caf_this_image;
  else
    dest_img = image_index;

  mutex_unlock(*p, dest_img, index, stat, errmsg, errmsg_len);
}

/* Atomics operations */

void
PREFIX (atomic_define) (caf_token_t token, size_t offset,
                        int image_index, void *value, int *stat,
                        int type __attribute__ ((unused)), int kind)
{
  MPI_Win *p = TOKEN(token);
  MPI_Datatype dt;
  int ierr = 0;
  int image;

  if(image_index != 0)
    image = image_index-1;
  else
    image = caf_this_image-1;

  selectType(kind, &dt);

#if MPI_VERSION >= 3
  CAF_Win_lock (MPI_LOCK_EXCLUSIVE, image, *p);
  ierr = MPI_Accumulate (value, 1, dt, image, offset, 1, dt, MPI_REPLACE, *p);
  CAF_Win_unlock (image, *p);
#else // MPI_VERSION
  CAF_Win_lock (MPI_LOCK_EXCLUSIVE, image, *p);
  ierr = MPI_Put (value, 1, dt, image, offset, 1, dt, *p);
  CAF_Win_unlock (image, *p);
#endif // MPI_VERSION

  if (stat)
    *stat = ierr;
  else if (ierr != 0)
    terminate_internal (ierr, 0);

  return;
}

void
PREFIX(atomic_ref) (caf_token_t token, size_t offset,
                    int image_index,
                    void *value, int *stat,
                    int type __attribute__ ((unused)), int kind)
{
  MPI_Win *p = TOKEN(token);
  MPI_Datatype dt;
  int ierr = 0;
  int image;

  if(image_index != 0)
    image = image_index-1;
  else
    image = caf_this_image-1;

  selectType(kind, &dt);

#if MPI_VERSION >= 3
  CAF_Win_lock (MPI_LOCK_EXCLUSIVE, image, *p);
  ierr = MPI_Fetch_and_op(NULL, value, dt, image, offset, MPI_NO_OP, *p);
  CAF_Win_unlock (image, *p);
#else // MPI_VERSION
  CAF_Win_lock (MPI_LOCK_EXCLUSIVE, image, *p);
  ierr = MPI_Get (value, 1, dt, image, offset, 1, dt, *p);
  CAF_Win_unlock (image, *p);
#endif // MPI_VERSION

  if (stat)
    *stat = ierr;
  else if (ierr != 0)
    terminate_internal (ierr, 0);

  return;
}


void
PREFIX(atomic_cas) (caf_token_t token, size_t offset,
                    int image_index, void *old, void *compare,
                    void *new_val, int *stat,
                    int type __attribute__ ((unused)), int kind)
{
  MPI_Win *p = TOKEN(token);
  MPI_Datatype dt;
  int ierr = 0;
  int image;

  if(image_index != 0)
    image = image_index-1;
  else
    image = caf_this_image-1;

  selectType (kind, &dt);

#if MPI_VERSION >= 3
  CAF_Win_lock (MPI_LOCK_EXCLUSIVE, image, *p);
  ierr = MPI_Compare_and_swap (new_val, compare, old, dt, image,
                               offset, *p);
  CAF_Win_unlock (image, *p);
#else // MPI_VERSION
#warning atomic_cas for MPI-2 is not yet implemented
  printf ("We apologize but atomic_cas for MPI-2 is not yet implemented\n");
  ierr = 1;
#endif // MPI_VERSION

  if (stat)
    *stat = ierr;
  else if (ierr != 0)
    terminate_internal (ierr, 0);

  return;
}

void
PREFIX (atomic_op) (int op, caf_token_t token ,
                    size_t offset, int image_index,
                    void *value, void *old, int *stat,
                    int type __attribute__ ((unused)),
                    int kind)
{
  int ierr = 0;
  MPI_Datatype dt;
  MPI_Win *p = TOKEN(token);
  int image;

#if MPI_VERSION >= 3
  old = malloc(kind);

  if(image_index != 0)
    image = image_index-1;
  else
    image = caf_this_image-1;

  selectType (kind, &dt);

  CAF_Win_lock (MPI_LOCK_EXCLUSIVE, image, *p);
  /* Atomic_add */
  switch(op) {
    case 1:
      ierr = MPI_Fetch_and_op(value, old, dt, image, offset, MPI_SUM, *p);
      break;
    case 2:
      ierr = MPI_Fetch_and_op(value, old, dt, image, offset, MPI_BAND, *p);
      break;
    case 4:
      ierr = MPI_Fetch_and_op(value, old, dt, image, offset, MPI_BOR, *p);
      break;
    case 5:
      ierr = MPI_Fetch_and_op(value, old, dt, image, offset, MPI_BXOR, *p);
      break;
    default:
      printf ("We apologize but the atomic operation requested for MPI < 3 is not yet implemented\n");
      break;
    }
  CAF_Win_unlock (image, *p);

  free(old);
#else // MPI_VERSION
  #warning atomic_op for MPI is not yet implemented
  printf ("We apologize but atomic_op for MPI < 3 is not yet implemented\n");
#endif // MPI_VERSION
  if (stat)
    *stat = ierr;
  else if (ierr != 0)
    terminate_internal (ierr, 0);

  return;
}

/* Events */

void
PREFIX (event_post) (caf_token_t token, size_t index,
		     int image_index, int *stat,
		     char *errmsg, int errmsg_len)
{
  int image, value = 1, ierr = 0,flag;
  MPI_Win *p = TOKEN(token);
  const char msg[] = "Error on event post";

  if(image_index == 0)
    image = caf_this_image-1;
  else
    image = image_index-1;

  if(stat != NULL)
    *stat = 0;

#if MPI_VERSION >= 3
  CAF_Win_lock (MPI_LOCK_EXCLUSIVE, image, *p);
  ierr = MPI_Accumulate (&value, 1, MPI_INT, image, index*sizeof(int), 1, MPI_INT, MPI_SUM, *p);
  CAF_Win_unlock (image, *p);
#else // MPI_VERSION
  #warning Events for MPI-2 are not implemented
  printf ("Events for MPI-2 are not supported, please update your MPI implementation\n");
#endif // MPI_VERSION

  check_image_health (image_index, stat);

  if(!stat && ierr == STAT_FAILED_IMAGE)
    terminate_internal (ierr, 0);

  if(ierr != MPI_SUCCESS)
    {
      if(stat != NULL)
	*stat = ierr;
      if(errmsg != NULL)
	{
	  memset(errmsg,' ',errmsg_len);
	  memcpy(errmsg, msg, MIN(errmsg_len,strlen(msg)));
	}
    }
}

void
PREFIX (event_wait) (caf_token_t token, size_t index,
		     int until_count, int *stat,
		     char *errmsg, int errmsg_len)
{
  int ierr = 0, count = 0, i, image = caf_this_image - 1;
  int *var = NULL, flag, old = 0;
  int newval = 0;
  const int spin_loop_max = 20000;
  MPI_Win *p = TOKEN(token);
  const char msg[] = "Error on event wait";

  if(stat != NULL)
    *stat = 0;

  MPI_Win_get_attr (*p, MPI_WIN_BASE, &var, &flag);

  for(i = 0; i < spin_loop_max; ++i)
    {
      MPI_Win_sync (*p);
      count = var[index];
      if(count >= until_count)
	break;
    }

  i = 1;
  while(count < until_count)
    {
      MPI_Win_sync (*p);
      count = var[index];
      usleep (10 * i);
	++i;
      /* Needed to enforce MPI progress */
      MPI_Win_flush (image, *p);
    }

  newval = -until_count;

  CAF_Win_lock (MPI_LOCK_EXCLUSIVE, image, *p);
  ierr = MPI_Fetch_and_op(&newval, &old, MPI_INT, image, index*sizeof(int), MPI_SUM, *p);
  CAF_Win_unlock (image, *p);

  check_image_health (image, stat);

  if(!stat && ierr == STAT_FAILED_IMAGE)
    terminate_internal (ierr, 0);

  if(ierr != MPI_SUCCESS)
    {
      if(stat != NULL)
	*stat = ierr;
      if(errmsg != NULL)
	{
	  memset(errmsg,' ',errmsg_len);
	  memcpy(errmsg, msg, MIN(errmsg_len,strlen(msg)));
	}
    }
}

void
PREFIX (event_query) (caf_token_t token, size_t index,
		      int image_index, int *count, int *stat)
{
  int image,ierr=0;
  MPI_Win *p = TOKEN(token);

  if(image_index == 0)
    image = caf_this_image-1;
  else
    image = image_index-1;

  if(stat != NULL)
    *stat = 0;

#if MPI_VERSION >= 3
  CAF_Win_lock (MPI_LOCK_EXCLUSIVE, image, *p);
  ierr = MPI_Fetch_and_op(NULL, count, MPI_INT, image, index*sizeof(int), MPI_NO_OP, *p);
  CAF_Win_unlock (image, *p);
#else // MPI_VERSION
#warning Events for MPI-2 are not implemented
  printf ("Events for MPI-2 are not supported, please update your MPI implementation\n");
#endif // MPI_VERSION
  if(ierr != MPI_SUCCESS && stat != NULL)
    *stat = ierr;
}


/* Internal function to execute the part that is common to all (error) stop
 * functions.  */

static void
terminate_internal (int stat_code, int exit_code)
{
  dprint ("%d/%d: terminate_internal (stat_code = %d, exit_code = %d).\n",
          caf_this_image, caf_num_images, stat_code, exit_code);
  finalize_internal (stat_code);

#ifndef WITH_FAILED_IMAGES
  MPI_Abort(MPI_COMM_WORLD, exit_code);
#endif
  exit (exit_code);
}


/* STOP function for integer arguments.  */

void
PREFIX (stop_numeric) (int32_t stop_code)
{
  fprintf (stderr, "STOP %d\n", stop_code);

  /* Stopping includes taking down the runtime regularly and returning the
   * stop_code. */
  terminate_internal (STAT_STOPPED_IMAGE, stop_code);
}


/* STOP function for string arguments.  */

void
PREFIX (stop_str) (const char *string, int32_t len)
{
  fputs ("STOP ", stderr);
  while (len--)
    fputc (*(string++), stderr);
  fputs ("\n", stderr);

  /* Stopping includes taking down the runtime regularly. */
  terminate_internal (STAT_STOPPED_IMAGE, 0);
}


/* ERROR STOP function for string arguments.  */

void
PREFIX (error_stop_str) (const char *string, int32_t len)
{
  fputs ("ERROR STOP ", stderr);
  while (len--)
    fputc (*(string++), stderr);
  fputs ("\n", stderr);

  terminate_internal (STAT_STOPPED_IMAGE, 1);
}


/* ERROR STOP function for numerical arguments.  */

void
PREFIX (error_stop) (int32_t error)
{
  fprintf (stderr, "ERROR STOP %d\n", error);

  terminate_internal (STAT_STOPPED_IMAGE, error);
}


/* FAIL IMAGE statement.  */

void
PREFIX (fail_image) (void)
{
  fputs ("IMAGE FAILED!\n", stderr);

  raise(SIGKILL);
  /* A failing image is expected to take down the runtime regularly. */
  terminate_internal (STAT_FAILED_IMAGE, 0);
}

int
PREFIX (image_status) (int image)
{
#ifdef GFC_CAF_CHECK
  if (image < 1 || image > caf_num_images)
    {
      char errmsg[60];
      sprintf (errmsg, "Image #%d out of bounds of images 1..%d.", image,
               caf_num_images);
      caf_runtime_error (errmsg);
    }
#endif
#ifdef WITH_FAILED_IMAGES
  if (image_stati[image - 1] == 0)
    {
      int status, ierr;
      /* Check that we are fine before doing anything.
       *
       * Do an MPI-operation to learn about failed/stopped images, that have
       * not been detected yet.  */
      ierr = MPI_Test (&alive_request, &status, MPI_STATUSES_IGNORE);
      MPI_Error_class (ierr, &status);
      if (ierr == MPI_SUCCESS)
        {
          CAF_Win_lock (MPI_LOCK_SHARED, image - 1, *stat_tok);
          ierr = MPI_Get (&status, 1, MPI_INT, image - 1, 0, 1, MPI_INT, *stat_tok);
          dprint ("%d/%d: Image status of image #%d is: %d\n", caf_this_image,
                 caf_num_images, image, status);
          CAF_Win_unlock (image - 1, *stat_tok);
          image_stati[image - 1] = status;
        }
      else if (status == MPIX_ERR_PROC_FAILED)
        image_stati[image - 1] = STAT_FAILED_IMAGE;
      else
        {
          const int strcap = 200;
          char errmsg[strcap];
          int slen, supplied_len;
          sprintf (errmsg, "Image status for image #%d returned mpi error: ",
                   image);
          slen = strlen (errmsg);
          supplied_len = strcap - slen;
          MPI_Error_string (status, &errmsg[slen], &supplied_len);
          caf_runtime_error (errmsg);
        }
    }
  return image_stati[image - 1];
#else
  unsupported_fail_images_message ("IMAGE_STATUS()");
#endif

  return 0;
}

void
PREFIX (failed_images) (gfc_descriptor_t *array, int team __attribute__ ((unused)),
			int * kind)
{
  int local_kind = kind ? *kind : 4; /* GFC_DEFAULT_INTEGER_KIND = 4*/

#ifdef WITH_FAILED_IMAGES
  void *mem = calloc (num_images_failed, local_kind);
  array->base_addr = mem;
  for (int i = 0; i < caf_num_images; ++i)
    {
      if (image_stati[i] == STAT_FAILED_IMAGE)
	{
	  switch (local_kind)
	    {
	    case 1:
	      *(int8_t *)mem = i + 1;
	      break;
	    case 2:
	      *(int16_t *)mem = i + 1;
	      break;
	    case 4:
	      *(int32_t *)mem = i + 1;
	      break;
	    case 8:
	      *(int64_t *)mem = i + 1;
	      break;
#ifdef HAVE_GFC_INTEGER_16
	    case 16:
	      *(int128t *)mem = i + 1;
	      break;
#endif
	    default:
	      caf_runtime_error("Unsupported integer kind %1 in caf_failed_images.", local_kind);
	    }
	  mem += local_kind;
	}
    }
  array->dim[0]._ubound = num_images_failed-1;
#else
  unsupported_fail_images_message ("FAILED_IMAGES()");
  array->dim[0]._ubound = -1;
  array->base_addr = NULL;
#endif
  array->dtype = ((BT_INTEGER << GFC_DTYPE_TYPE_SHIFT)
		  | (local_kind << GFC_DTYPE_SIZE_SHIFT));
  array->dim[0].lower_bound = 0;
  array->dim[0]._stride = 1;
  array->offset = 0;
}

void
PREFIX (stopped_images) (gfc_descriptor_t *array, int team __attribute__ ((unused)),
			 int * kind)
{
  int local_kind = kind ? *kind : 4; /* GFC_DEFAULT_INTEGER_KIND = 4*/

#ifdef WITH_FAILED_IMAGES
  void *mem = calloc (num_images_stopped, local_kind);
  array->base_addr = mem;
  for (int i = 0; i < caf_num_images; ++i)
    {
      if (image_stati[i])
	{
	  switch (local_kind)
	    {
	    case 1:
	      *(int8_t *)mem = i + 1;
	      break;
	    case 2:
	      *(int16_t *)mem = i + 1;
	      break;
	    case 4:
	      *(int32_t *)mem = i + 1;
	      break;
	    case 8:
	      *(int64_t *)mem = i + 1;
	      break;
#ifdef HAVE_GFC_INTEGER_16
	    case 16:
	      *(int128t *)mem = i + 1;
	      break;
#endif
	    default:
	      caf_runtime_error("Unsupported integer kind %1 in caf_stopped_images.", local_kind);
	    }
	  mem += local_kind;
	}
    }
  array->dim[0]._ubound = num_images_stopped - 1;
#else
  unsupported_fail_images_message ("STOPPED_IMAGES()");
  array->dim[0]._ubound = -1;
  array->base_addr = NULL;
#endif
  array->dtype = ((BT_INTEGER << GFC_DTYPE_TYPE_SHIFT)
		  | (local_kind << GFC_DTYPE_SIZE_SHIFT));
  array->dim[0].lower_bound = 0;
  array->dim[0]._stride = 1;
  array->offset = 0;
}

/* Give a descriptive message when failed images support is not available. */
void
unsupported_fail_images_message (const char * functionname)
{
  fprintf (stderr, "*** caf_mpi-lib runtime message on image %d:\n"
           "*** The failed images feature '%s' of Fortran 2015 standard\n"
           "*** is not available in this build. You need a compiler with failed images\n"
           "*** support activated and compile OpenCoarrays with failed images support.\n",
           caf_this_image, functionname);
#ifdef STOP_ON_UNSUPPORTED
  exit (EXIT_FAILURE);
#endif
}

/* Give a descriptive message when support for an allocatable components feature
 * is not available. */
void
unimplemented_alloc_comps_message (const char * functionname)
{
  fprintf (stderr,
           "*** Message from libcaf_mpi runtime function '%s' on image %d:\n"
           "*** Assigning to an allocatable coarray component of a derived type is not yet supported with GCC 7.\n"
           "*** Either revert to GCC 6 or convert all puts (type(foo)::x; x%%y[recipient] = z) to gets (z = x%%y[provider]).\n",
           functionname, caf_this_image );
#ifdef STOP_ON_UNSUPPORTED
  exit (EXIT_FAILURE);
#endif
}
