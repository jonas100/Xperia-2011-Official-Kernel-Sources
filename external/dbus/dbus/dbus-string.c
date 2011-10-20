/* -*- mode: C; c-file-style: "gnu" -*- */
/* dbus-string.c String utility class (internal to D-Bus implementation)
 * 
 * Copyright (C) 2002, 2003, 2004, 2005 Red Hat, Inc.
 *
 * Licensed under the Academic Free License version 2.1
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include "dbus-internals.h"
#include "dbus-string.h"
/* we allow a system header here, for speed/convenience */
#include <string.h>
/* for vsnprintf */
#include <stdio.h>
#define DBUS_CAN_USE_DBUS_STRING_PRIVATE 1
#include "dbus-string-private.h"
#include "dbus-marshal-basic.h" /* probably should be removed by moving the usage of DBUS_TYPE
                                 * into the marshaling-related files
                                 */
/* for DBUS_VA_COPY */
#include "dbus-sysdeps.h"

/**
 * @defgroup DBusString DBusString class
 * @ingroup  DBusInternals
 * @brief DBusString data structure for safer string handling
 *
 * Types and functions related to DBusString. DBusString is intended
 * to be a string class that makes it hard to mess up security issues
 * (and just in general harder to write buggy code).  It should be
 * used (or extended and then used) rather than the libc stuff in
 * string.h.  The string class is a bit inconvenient at spots because
 * it handles out-of-memory failures and tries to be extra-robust.
 * 
 * A DBusString has a maximum length set at initialization time; this
 * can be used to ensure that a buffer doesn't get too big.  The
 * _dbus_string_lengthen() method checks for overflow, and for max
 * length being exceeded.
 * 
 * Try to avoid conversion to a plain C string, i.e. add methods on
 * the string object instead, only convert to C string when passing
 * things out to the public API. In particular, no sprintf, strcpy,
 * strcat, any of that should be used. The GString feature of
 * accepting negative numbers for "length of string" is also absent,
 * because it could keep us from detecting bogus huge lengths. i.e. if
 * we passed in some bogus huge length it would be taken to mean
 * "current length of string" instead of "broken crack"
 *
 * @todo #DBusString needs a lot of cleaning up; some of the
 * API is no longer used, and the API is pretty inconsistent.
 * In particular all the "append" APIs, especially those involving
 * alignment but probably lots of them, are no longer used by the
 * marshaling code which always does "inserts" now.
 */

/**
 * @addtogroup DBusString
 * @{
 */

static void
fixup_alignment (DBusRealString *real)
{
  unsigned char *aligned;
  unsigned char *real_block;
  unsigned int old_align_offset;

  /* we have to have extra space in real->allocated for the align offset and nul byte */
  _dbus_assert (real->len <= real->allocated - _DBUS_STRING_ALLOCATION_PADDING);
  
  old_align_offset = real->align_offset;
  real_block = real->str - old_align_offset;
  
  aligned = _DBUS_ALIGN_ADDRESS (real_block, 8);

  real->align_offset = aligned - real_block;
  real->str = aligned;
  
  if (old_align_offset != real->align_offset)
    {
      /* Here comes the suck */
      memmove (real_block + real->align_offset,
               real_block + old_align_offset,
               real->len + 1);
    }

  _dbus_assert (real->align_offset < 8);
  _dbus_assert (_DBUS_ALIGN_ADDRESS (real->str, 8) == real->str);
}

static void
undo_alignment (DBusRealString *real)
{
  if (real->align_offset != 0)
    {
      memmove (real->str - real->align_offset,
               real->str,
               real->len + 1);

      real->str = real->str - real->align_offset;
      real->align_offset = 0;
    }
}

/**
 * Initializes a string that can be up to the given allocation size
 * before it has to realloc. The string starts life with zero length.
 * The string must eventually be freed with _dbus_string_free().
 * 
 * @param str memory to hold the string
 * @param allocate_size amount to preallocate
 * @returns #TRUE on success, #FALSE if no memory
 */
dbus_bool_t
_dbus_string_init_preallocated (DBusString *str,
                                int         allocate_size)
{
  DBusRealString *real;
  
  _dbus_assert (str != NULL);

  _dbus_assert (sizeof (DBusString) == sizeof (DBusRealString));
  
  real = (DBusRealString*) str;

  /* It's very important not to touch anything
   * other than real->str if we're going to fail,
   * since we also use this function to reset
   * an existing string, e.g. in _dbus_string_steal_data()
   */
  
  real->str = dbus_malloc (_DBUS_STRING_ALLOCATION_PADDING + allocate_size);
  if (real->str == NULL)
    return FALSE;  
  
  real->allocated = _DBUS_STRING_ALLOCATION_PADDING + allocate_size;
  real->len = 0;
  real->str[real->len] = '\0';
  
  real->max_length = _DBUS_STRING_MAX_MAX_LENGTH;
  real->constant = FALSE;
  real->locked = FALSE;
  real->invalid = FALSE;
  real->align_offset = 0;
  
  fixup_alignment (real);
  
  return TRUE;
}

/**
 * Initializes a string. The string starts life with zero length.  The
 * string must eventually be freed with _dbus_string_free().
 * 
 * @param str memory to hold the string
 * @returns #TRUE on success, #FALSE if no memory
 */
dbus_bool_t
_dbus_string_init (DBusString *str)
{
  return _dbus_string_init_preallocated (str, 0);
}

#ifdef DBUS_BUILD_TESTS
/* The max length thing is sort of a historical artifact
 * from a feature that turned out to be dumb; perhaps
 * we should purge it entirely. The problem with
 * the feature is that it looks like memory allocation
 * failure, but is not a transient or resolvable failure.
 */
static void
set_max_length (DBusString *str,
                int         max_length)
{
  DBusRealString *real;
  
  real = (DBusRealString*) str;

  real->max_length = max_length;
}
#endif /* DBUS_BUILD_TESTS */

/**
 * Initializes a constant string. The value parameter is not copied
 * (should be static), and the string may never be modified.
 * It is safe but not necessary to call _dbus_string_free()
 * on a const string. The string has a length limit of MAXINT - 8.
 * 
 * @param str memory to use for the string
 * @param value a string to be stored in str (not copied!!!)
 */
void
_dbus_string_init_const (DBusString *str,
                         const char *value)
{
  _dbus_assert (value != NULL);
  
  _dbus_string_init_const_len (str, value,
                               strlen (value));
}

/**
 * Initializes a constant string with a length. The value parameter is
 * not copied (should be static), and the string may never be
 * modified.  It is safe but not necessary to call _dbus_string_free()
 * on a const string.
 * 
 * @param str memory to use for the string
 * @param value a string to be stored in str (not copied!!!)
 * @param len the length to use
 */
void
_dbus_string_init_const_len (DBusString *str,
                             const char *value,
                             int         len)
{
  DBusRealString *real;
  
  _dbus_assert (str != NULL);
  _dbus_assert (len == 0 || value != NULL);
  _dbus_assert (len <= _DBUS_STRING_MAX_MAX_LENGTH);
  _dbus_assert (len >= 0);
  
  real = (DBusRealString*) str;
  
  real->str = (unsigned char*) value;
  real->len = len;
  real->allocated = real->len + _DBUS_STRING_ALLOCATION_PADDING; /* a lie, just to avoid special-case assertions... */
  real->max_length = real->len + 1;
  real->constant = TRUE;
  real->locked = TRUE;
  real->invalid = FALSE;
  real->align_offset = 0;

  /* We don't require const strings to be 8-byte aligned as the
   * memory is coming from elsewhere.
   */
}

/**
 * Frees a string created by _dbus_string_init().
 *
 * @param str memory where the string is stored.
 */
void
_dbus_string_free (DBusString *str)
{
  DBusRealString *real = (DBusRealString*) str;
  DBUS_GENERIC_STRING_PREAMBLE (real);
  
  if (real->constant)
    return;
  dbus_free (real->str - real->align_offset);

  real->invalid = TRUE;
}

#ifdef DBUS_BUILD_TESTS
/* Not using this feature at the moment,
 * so marked DBUS_BUILD_TESTS-only
 */
/**
 * Locks a string such that any attempts to change the string will
 * result in aborting the program. Also, if the string is wasting a
 * lot of memory (allocation is sufficiently larger than what the
 * string is really using), _dbus_string_lock() will realloc the
 * string's data to "compact" it.
 *
 * @param str the string to lock.
 */
void
_dbus_string_lock (DBusString *str)
{  
  DBUS_LOCKED_STRING_PREAMBLE (str); /* can lock multiple times */

  real->locked = TRUE;

  /* Try to realloc to avoid excess memory usage, since
   * we know we won't change the string further
   */
#define MAX_WASTE 48
  if (real->allocated - MAX_WASTE > real->len)
    {
      unsigned char *new_str;
      int new_allocated;

      new_allocated = real->len + _DBUS_STRING_ALLOCATION_PADDING;

      new_str = dbus_realloc (real->str - real->align_offset,
                              new_allocated);
      if (new_str != NULL)
        {
          real->str = new_str + real->align_offset;
          real->allocated = new_allocated;
          fixup_alignment (real);
        }
    }
}
#endif /* DBUS_BUILD_TESTS */

static dbus_bool_t
reallocate_for_length (DBusRealString *real,
                       int             new_length)
{
  int new_allocated;
  unsigned char *new_str;

  /* at least double our old allocation to avoid O(n), avoiding
   * overflow
   */
  if (real->allocated > (_DBUS_STRING_MAX_MAX_LENGTH + _DBUS_STRING_ALLOCATION_PADDING) / 2)
    new_allocated = _DBUS_STRING_MAX_MAX_LENGTH + _DBUS_STRING_ALLOCATION_PADDING;
  else
    new_allocated = real->allocated * 2;

  /* if you change the code just above here, run the tests without
   * the following assert-only hack before you commit
   */
  /* This is keyed off asserts in addition to tests so when you
   * disable asserts to profile, you don't get this destroyer
   * of profiles.
   */
#ifdef DBUS_DISABLE_ASSERT
#else
#ifdef DBUS_BUILD_TESTS
  new_allocated = 0; /* ensure a realloc every time so that we go
                      * through all malloc failure codepaths
                      */
#endif /* DBUS_BUILD_TESTS */
#endif /* !DBUS_DISABLE_ASSERT */

  /* But be sure we always alloc at least space for the new length */
  new_allocated = MAX (new_allocated,
                       new_length + _DBUS_STRING_ALLOCATION_PADDING);

  _dbus_assert (new_allocated >= real->allocated); /* code relies on this */
  new_str = dbus_realloc (real->str - real->align_offset, new_allocated);
  if (_DBUS_UNLIKELY (new_str == NULL))
    return FALSE;

  real->str = new_str + real->align_offset;
  real->allocated = new_allocated;
  fixup_alignment (real);

  return TRUE;
}

static dbus_bool_t
set_length (DBusRealString *real,
            int             new_length)
{
  /* Note, we are setting the length not including nul termination */

  /* exceeding max length is the same as failure to allocate memory */
  if (_DBUS_UNLIKELY (new_length > real->max_length))
    return FALSE;
  else if (new_length > (real->allocated - _DBUS_STRING_ALLOCATION_PADDING) &&
           _DBUS_UNLIKELY (!reallocate_for_length (real, new_length)))
    return FALSE;
  else
    {
      real->len = new_length;
      real->str[new_length] = '\0';
      return TRUE;
    }
}

static dbus_bool_t
open_gap (int             len,
          DBusRealString *dest,
          int             insert_at)
{
  if (len == 0)
    return TRUE;

  if (len > dest->max_length - dest->len)
    return FALSE; /* detected overflow of dest->len + len below */
  
  if (!set_length (dest, dest->len + len))
    return FALSE;

  memmove (dest->str + insert_at + len, 
           dest->str + insert_at,
           dest->len - len - insert_at);

  return TRUE;
}

#ifndef _dbus_string_get_data
/**
 * Gets the raw character buffer from the string.  The returned buffer
 * will be nul-terminated, but note that strings may contain binary
 * data so there may be extra nul characters prior to the termination.
 * This function should be little-used, extend DBusString or add
 * stuff to dbus-sysdeps.c instead. It's an error to use this
 * function on a const string.
 *
 * @param str the string
 * @returns the data
 */
char*
_dbus_string_get_data (DBusString *str)
{
  DBUS_STRING_PREAMBLE (str);
  
  return (char*) real->str;
}
#endif /* _dbus_string_get_data */

/* only do the function if we don't have the macro */
#ifndef _dbus_string_get_const_data
/**
 * Gets the raw character buffer from a const string.
 *
 * @param str the string
 * @returns the string data
 */
const char*
_dbus_string_get_const_data (const DBusString  *str)
{
  DBUS_CONST_STRING_PREAMBLE (str);
  
  return (const char*) real->str;
}
#endif /* _dbus_string_get_const_data */

/**
 * Gets a sub-portion of the raw character buffer from the
 * string. The "len" field is required simply for error
 * checking, to be sure you don't try to use more
 * string than exists. The nul termination of the
 * returned buffer remains at the end of the entire
 * string, not at start + len.
 *
 * @param str the string
 * @param start byte offset to return
 * @param len length of segment to return
 * @returns the string data
 */
char*
_dbus_string_get_data_len (DBusString *str,
                           int         start,
                           int         len)
{
  DBUS_STRING_PREAMBLE (str);
  _dbus_assert (start >= 0);
  _dbus_assert (len >= 0);
  _dbus_assert (start <= real->len);
  _dbus_assert (len <= real->len - start);
  
  return (char*) real->str + start;
}

/* only do the function if we don't have the macro */
#ifndef _dbus_string_get_const_data_len
/**
 * const version of _dbus_string_get_data_len().
 *
 * @param str the string
 * @param start byte offset to return
 * @param len length of segment to return
 * @returns the string data
 */
const char*
_dbus_string_get_const_data_len (const DBusString  *str,
                                 int                start,
                                 int                len)
{
  DBUS_CONST_STRING_PREAMBLE (str);
  _dbus_assert (start >= 0);
  _dbus_assert (len >= 0);
  _dbus_assert (start <= real->len);
  _dbus_assert (len <= real->len - start);
  
  return (const char*) real->str + start;
}
#endif /* _dbus_string_get_const_data_len */

/* only do the function if we don't have the macro */
#ifndef _dbus_string_set_byte
/**
 * Sets the value of the byte at the given position.
 *
 * @param str the string
 * @param i the position
 * @param byte the new value
 */
void
_dbus_string_set_byte (DBusString    *str,
                       int            i,
                       unsigned char  byte)
{
  DBUS_STRING_PREAMBLE (str);
  _dbus_assert (i < real->len);
  _dbus_assert (i >= 0);
  
  real->str[i] = byte;
}
#endif /* _dbus_string_set_byte */

/* only have the function if we didn't create a macro */
#ifndef _dbus_string_get_byte
/**
 * Gets the byte at the given position. It is
 * allowed to ask for the nul byte at the end of
 * the string.
 *
 * @param str the string
 * @param start the position
 * @returns the byte at that position
 */
unsigned char
_dbus_string_get_byte (const DBusString  *str,
                       int                start)
{
  DBUS_CONST_STRING_PREAMBLE (str);
  _dbus_assert (start <= real->len);
  _dbus_assert (start >= 0);
  
  return real->str[start];
}
#endif /* _dbus_string_get_byte */

/**
 * Inserts a number of bytes of a given value at the
 * given position.
 *
 * @param str the string
 * @param i the position
 * @param n_bytes number of bytes
 * @param byte the value to insert
 * @returns #TRUE on success
 */
dbus_bool_t
_dbus_string_insert_bytes (DBusString   *str,
			   int           i,
			   int           n_bytes,
			   unsigned char byte)
{
  DBUS_STRING_PREAMBLE (str);
  _dbus_assert (i <= real->len);
  _dbus_assert (i >= 0);
  _dbus_assert (n_bytes >= 0);

  if (n_bytes == 0)
    return TRUE;
  
  if (!open_gap (n_bytes, real, i))
    return FALSE;
  
  memset (real->str + i, byte, n_bytes);

  return TRUE;
}

/**
 * Inserts a single byte at the given position.
 *
 * @param str the string
 * @param i the position
 * @param byte the value to insert
 * @returns #TRUE on success
 */
dbus_bool_t
_dbus_string_insert_byte (DBusString   *str,
			   int           i,
			   unsigned char byte)
{
  DBUS_STRING_PREAMBLE (str);
  _dbus_assert (i <= real->len);
  _dbus_assert (i >= 0);
  
  if (!open_gap (1, real, i))
    return FALSE;

  real->str[i] = byte;

  return TRUE;
}

/**
 * Like _dbus_string_get_data(), but removes the
 * gotten data from the original string. The caller
 * must free the data returned. This function may
 * fail due to lack of memory, and return #FALSE.
 *
 * @param str the string
 * @param data_return location to return the buffer
 * @returns #TRUE on success
 */
dbus_bool_t
_dbus_string_steal_data (DBusString        *str,
                         char             **data_return)
{
  int old_max_length;
  DBUS_STRING_PREAMBLE (str);
  _dbus_assert (data_return != NULL);

  undo_alignment (real);
  
  *data_return = (char*) real->str;

  old_max_length = real->max_length;
  
  /* reset the string */
  if (!_dbus_string_init (str))
    {
      /* hrm, put it back then */
      real->str = (unsigned char*) *data_return;
      *data_return = NULL;
      fixup_alignment (real);
      return FALSE;
    }

  real->max_length = old_max_length;

  return TRUE;
}

#ifdef DBUS_BUILD_TESTS
/**
 * Like _dbus_string_get_data_len(), but removes the gotten data from
 * the original string. The caller must free the data returned. This
 * function may fail due to lack of memory, and return #FALSE.
 * The returned string is nul-terminated and has length len.
 *
 * @todo this function is broken because on failure it
 * may corrupt the source string.
 * 
 * @param str the string
 * @param data_return location to return the buffer
 * @param start the start of segment to steal
 * @param len the length of segment to steal
 * @returns #TRUE on success
 */
dbus_bool_t
_dbus_string_steal_data_len (DBusString        *str,
                             char             **data_return,
                             int                start,
                             int                len)
{
  DBusString dest;
  DBUS_STRING_PREAMBLE (str);
  _dbus_assert (data_return != NULL);
  _dbus_assert (start >= 0);
  _dbus_assert (len >= 0);
  _dbus_assert (start <= real->len);
  _dbus_assert (len <= real->len - start);

  if (!_dbus_string_init (&dest))
    return FALSE;

  set_max_length (&dest, real->max_length);
  
  if (!_dbus_string_move_len (str, start, len, &dest, 0))
    {
      _dbus_string_free (&dest);
      return FALSE;
    }

  _dbus_warn ("Broken code in _dbus_string_steal_data_len(), see @todo, FIXME\n");
  if (!_dbus_string_steal_data (&dest, data_return))
    {
      _dbus_string_free (&dest);
      return FALSE;
    }

  _dbus_string_free (&dest);
  return TRUE;
}
#endif /* DBUS_BUILD_TESTS */

/**
 * Copies the data from the string into a char*
 *
 * @param str the string
 * @param data_return place to return the data
 * @returns #TRUE on success, #FALSE on no memory
 */
dbus_bool_t
_dbus_string_copy_data (const DBusString  *str,
                        char             **data_return)
{
  DBUS_CONST_STRING_PREAMBLE (str);
  _dbus_assert (data_return != NULL);
  
  *data_return = dbus_malloc (real->len + 1);
  if (*data_return == NULL)
    return FALSE;

  memcpy (*data_return, real->str, real->len + 1);

  return TRUE;
}

/**
 * Copies the contents of a DBusString into a different
 * buffer. The resulting buffer will be nul-terminated.
 * 
 * @param str a string
 * @param buffer a C buffer to copy data to
 * @param avail_len maximum length of C buffer
 */
void
_dbus_string_copy_to_buffer (const DBusString  *str,
			     char              *buffer,
			     int                avail_len)
{
  int copy_len;
  DBUS_CONST_STRING_PREAMBLE (str);

  _dbus_assert (avail_len >= 0);

  copy_len = MIN (avail_len, real->len+1);
  memcpy (buffer, real->str, copy_len);
  if (avail_len > 0 && avail_len == copy_len)
    buffer[avail_len-1] = '\0';
}

#ifdef DBUS_BUILD_TESTS
/**
 * Copies a segment of the string into a char*
 *
 * @param str the string
 * @param data_return place to return the data
 * @param start start index
 * @param len length to copy
 * @returns #FALSE if no memory
 */
dbus_bool_t
_dbus_string_copy_data_len (const DBusString  *str,
                            char             **data_return,
                            int                start,
                            int                len)
{
  DBusString dest;

  DBUS_CONST_STRING_PREAMBLE (str);
  _dbus_assert (data_return != NULL);
  _dbus_assert (start >= 0);
  _dbus_assert (len >= 0);
  _dbus_assert (start <= real->len);
  _dbus_assert (len <= real->len - start);

  if (!_dbus_string_init (&dest))
    return FALSE;

  set_max_length (&dest, real->max_length);

  if (!_dbus_string_copy_len (str, start, len, &dest, 0))
    {
      _dbus_string_free (&dest);
      return FALSE;
    }

  if (!_dbus_string_steal_data (&dest, data_return))
    {
      _dbus_string_free (&dest);
      return FALSE;
    }

  _dbus_string_free (&dest);
  return TRUE;
}
#endif /* DBUS_BUILD_TESTS */

/* Only have the function if we don't have the macro */
#ifndef _dbus_string_get_length
/**
 * Gets the length of a string (not including nul termination).
 *
 * @returns the length.
 */
int
_dbus_string_get_length (const DBusString  *str)
{
  /* The assertion should not fail for empty strings. */
  DBusRealString *real = (DBusRealString *)str;
  if (((DBusRealString *)str)->len || ((DBusRealString *)str)->allocated) {
      DBUS_CONST_STRING_PREAMBLE (str);
  }
  
  return real->len;
}
#endif /* !_dbus_string_get_length */

/**
 * Makes a string longer by the given number of bytes.  Checks whether
 * adding additional_length to the current length would overflow an
 * integer, and checks for exceeding a string's max length.
 * The new bytes are not initialized, other than nul-terminating
 * the end of the string. The uninitialized bytes may contain
 * nul bytes or other junk.
 *
 * @param str a string
 * @param additional_length length to add to the string.
 * @returns #TRUE on success.
 */
dbus_bool_t
_dbus_string_lengthen (DBusString *str,
                       int         additional_length)
{
  DBUS_STRING_PREAMBLE (str);  
  _dbus_assert (additional_length >= 0);

  if (_DBUS_UNLIKELY (additional_length > real->max_length - real->len))
    return FALSE; /* would overflow */
  
  return set_length (real,
                     real->len + additional_length);
}

/**
 * Makes a string shorter by the given number of bytes.
 *
 * @param str a string
 * @param length_to_remove length to remove from the string.
 */
void
_dbus_string_shorten (DBusString *str,
                      int         length_to_remove)
{
  DBUS_STRING_PREAMBLE (str);
  _dbus_assert (length_to_remove >= 0);
  _dbus_assert (length_to_remove <= real->len);

  set_length (real,
              real->len - length_to_remove);
}

/**
 * Sets the length of a string. Can be used to truncate or lengthen
 * the string. If the string is lengthened, the function may fail and
 * return #FALSE. Newly-added bytes are not initialized, as with
 * _dbus_string_lengthen().
 *
 * @param str a string
 * @param length new length of the string.
 * @returns #FALSE on failure.
 */
dbus_bool_t
_dbus_string_set_length (DBusString *str,
                         int         length)
{
  DBUS_STRING_PREAMBLE (str);
  _dbus_assert (length >= 0);

  return set_length (real, length);
}

static dbus_bool_t
align_insert_point_then_open_gap (DBusString *str,
                                  int        *insert_at_p,
                                  int         alignment,
                                  int         gap_size)
{
  unsigned long new_len; /* ulong to avoid _DBUS_ALIGN_VALUE overflow */
  unsigned long gap_pos;
  int insert_at;
  int delta;
  DBUS_STRING_PREAMBLE (str);
  _dbus_assert (alignment >= 1);
  _dbus_assert (alignment <= 8); /* it has to be a bug if > 8 */

  insert_at = *insert_at_p;

  _dbus_assert (insert_at <= real->len);
  
  gap_pos = _DBUS_ALIGN_VALUE (insert_at, alignment);
  new_len = real->len + (gap_pos - insert_at) + gap_size;
  
  if (_DBUS_UNLIKELY (new_len > (unsigned long) real->max_length))
    return FALSE;
  
  delta = new_len - real->len;
  _dbus_assert (delta >= 0);

  if (delta == 0) /* only happens if gap_size == 0 and insert_at is aligned already */
    {
      _dbus_assert (((unsigned long) *insert_at_p) == gap_pos);
      return TRUE;
    }

  if (_DBUS_UNLIKELY (!open_gap (new_len - real->len,
                                 real, insert_at)))
    return FALSE;

  /* nul the padding if we had to add any padding */
  if (gap_size < delta)
    {
      memset (&real->str[insert_at], '\0',
              gap_pos - insert_at);
    }

  *insert_at_p = gap_pos;
  
  return TRUE;
}

static dbus_bool_t
align_length_then_lengthen (DBusString *str,
                            int         alignment,
                            int         then_lengthen_by)
{
  int insert_at;

  insert_at = _dbus_string_get_length (str);
  
  return align_insert_point_then_open_gap (str,
                                           &insert_at,
                                           alignment, then_lengthen_by);
}

/**
 * Align the length of a string to a specific alignment (typically 4 or 8)
 * by appending nul bytes to the string.
 *
 * @param str a string
 * @param alignment the alignment
 * @returns #FALSE if no memory
 */
dbus_bool_t
_dbus_string_align_length (DBusString *str,
                           int         alignment)
{
  return align_length_then_lengthen (str, alignment, 0);
}

/**
 * Preallocate extra_bytes such that a future lengthening of the
 * string by extra_bytes is guaranteed to succeed without an out of
 * memory error.
 *
 * @param str a string
 * @param extra_bytes bytes to alloc
 * @returns #FALSE if no memory
 */
dbus_bool_t
_dbus_string_alloc_space (DBusString        *str,
                          int                extra_bytes)
{
  if (!_dbus_string_lengthen (str, extra_bytes))
    return FALSE;
  _dbus_string_shorten (str, extra_bytes);

  return TRUE;
}

static dbus_bool_t
append (DBusRealString *real,
        const char     *buffer,
        int             buffer_len)
{
  if (buffer_len == 0)
    return TRUE;

  if (!_dbus_string_lengthen ((DBusString*)real, buffer_len))
    return FALSE;

  memcpy (real->str + (real->len - buffer_len),
          buffer,
          buffer_len);

  return TRUE;
}

/**
 * Appends a nul-terminated C-style string to a DBusString.
 *
 * @param str the DBusString
 * @param buffer the nul-terminated characters to append
 * @returns #FALSE if not enough memory.
 */
dbus_bool_t
_dbus_string_append (DBusString *str,
                     const char *buffer)
{
  unsigned long buffer_len;
  
  DBUS_STRING_PREAMBLE (str);
  _dbus_assert (buffer != NULL);
  
  buffer_len = strlen (buffer);
  if (buffer_len > (unsigned long) real->max_length)
    return FALSE;
  
  return append (real, buffer, buffer_len);
}

/** assign 2 bytes from one string to another */
#define ASSIGN_2_OCTETS(p, octets) \
  *((dbus_uint16_t*)(p)) = *((dbus_uint16_t*)(octets));

/** assign 4 bytes from one string to another */
#define ASSIGN_4_OCTETS(p, octets) \
  *((dbus_uint32_t*)(p)) = *((dbus_uint32_t*)(octets));

#ifdef DBUS_HAVE_INT64
/** assign 8 bytes from one string to another */
#define ASSIGN_8_OCTETS(p, octets) \
  *((dbus_uint64_t*)(p)) = *((dbus_uint64_t*)(octets));
#else
/** assign 8 bytes from one string to another */
#define ASSIGN_8_OCTETS(p, octets)              \
do {                                            \
  unsigned char *b;                             \
                                                \
  b = p;                                        \
                                                \
  *b++ = octets[0];                             \
  *b++ = octets[1];                             \
  *b++ = octets[2];                             \
  *b++ = octets[3];                             \
  *b++ = octets[4];                             \
  *b++ = octets[5];                             \
  *b++ = octets[6];                             \
  *b++ = octets[7];                             \
  _dbus_assert (b == p + 8);                    \
} while (0)
#endif /* DBUS_HAVE_INT64 */

#ifdef DBUS_BUILD_TESTS
/**
 * Appends 4 bytes aligned on a 4 byte boundary
 * with any alignment padding initialized to 0.
 *
 * @param str the DBusString
 * @param octets 4 bytes to append
 * @returns #FALSE if not enough memory.
 */
dbus_bool_t
_dbus_string_append_4_aligned (DBusString         *str,
                               const unsigned char octets[4])
{
  DBUS_STRING_PREAMBLE (str);
  
  if (!align_length_then_lengthen (str, 4, 4))
    return FALSE;

  ASSIGN_4_OCTETS (real->str + (real->len - 4), octets);

  return TRUE;
}
#endif /* DBUS_BUILD_TESTS */

#ifdef DBUS_BUILD_TESTS
/**
 * Appends 8 bytes aligned on an 8 byte boundary
 * with any alignment padding initialized to 0.
 *
 * @param str the DBusString
 * @param octets 8 bytes to append
 * @returns #FALSE if not enough memory.
 */
dbus_bool_t
_dbus_string_append_8_aligned (DBusString         *str,
                               const unsigned char octets[8])
{
  DBUS_STRING_PREAMBLE (str);
  
  if (!align_length_then_lengthen (str, 8, 8))
    return FALSE;

  ASSIGN_8_OCTETS (real->str + (real->len - 8), octets);

  return TRUE;
}
#endif /* DBUS_BUILD_TESTS */

/**
 * Inserts 2 bytes aligned on a 2 byte boundary
 * with any alignment padding initialized to 0.
 *
 * @param str the DBusString
 * @param insert_at where to insert
 * @param octets 2 bytes to insert
 * @returns #FALSE if not enough memory.
 */
dbus_bool_t
_dbus_string_insert_2_aligned (DBusString         *str,
                               int                 insert_at,
                               const unsigned char octets[4])
{
  DBUS_STRING_PREAMBLE (str);
  
  if (!align_insert_point_then_open_gap (str, &insert_at, 2, 2))
    return FALSE;

  ASSIGN_2_OCTETS (real->str + insert_at, octets);

  return TRUE;
}

/**
 * Inserts 4 bytes aligned on a 4 byte boundary
 * with any alignment padding initialized to 0.
 *
 * @param str the DBusString
 * @param insert_at where to insert
 * @param octets 4 bytes to insert
 * @returns #FALSE if not enough memory.
 */
dbus_bool_t
_dbus_string_insert_4_aligned (DBusString         *str,
                               int                 insert_at,
                               const unsigned char octets[4])
{
  DBUS_STRING_PREAMBLE (str);
  
  if (!align_insert_point_then_open_gap (str, &insert_at, 4, 4))
    return FALSE;

  ASSIGN_4_OCTETS (real->str + insert_at, octets);

  return TRUE;
}

/**
 * Inserts 8 bytes aligned on an 8 byte boundary
 * with any alignment padding initialized to 0.
 *
 * @param str the DBusString
 * @param insert_at where to insert
 * @param octets 8 bytes to insert
 * @returns #FALSE if not enough memory.
 */
dbus_bool_t
_dbus_string_insert_8_aligned (DBusString         *str,
                               int                 insert_at,
                               const unsigned char octets[8])
{
  DBUS_STRING_PREAMBLE (str);
  
  if (!align_insert_point_then_open_gap (str, &insert_at, 8, 8))
    return FALSE;

  _dbus_assert (_DBUS_ALIGN_VALUE (insert_at, 8) == (unsigned) insert_at);
  
  ASSIGN_8_OCTETS (real->str + insert_at, octets);

  return TRUE;
}


/**
 * Inserts padding at *insert_at such to align it to the given
 * boundary. Initializes the padding to nul bytes. Sets *insert_at
 * to the aligned position.
 *
 * @param str the DBusString
 * @param insert_at location to be aligned
 * @param alignment alignment boundary (1, 2, 4, or 8)
 * @returns #FALSE if not enough memory.
 */
dbus_bool_t
_dbus_string_insert_alignment (DBusString        *str,
                               int               *insert_at,
                               int                alignment)
{
  DBUS_STRING_PREAMBLE (str);
  
  if (!align_insert_point_then_open_gap (str, insert_at, alignment, 0))
    return FALSE;

  _dbus_assert (_DBUS_ALIGN_VALUE (*insert_at, alignment) == (unsigned) *insert_at);

  return TRUE;
}

/**
 * Appends a printf-style formatted string
 * to the #DBusString.
 *
 * @param str the string
 * @param format printf format
 * @param args variable argument list
 * @returns #FALSE if no memory
 */
dbus_bool_t
_dbus_string_append_printf_valist  (DBusString        *str,
                                    const char        *format,
                                    va_list            args)
{
  int len;
  va_list args_copy;

  DBUS_STRING_PREAMBLE (str);

  DBUS_VA_COPY (args_copy, args);

  /* Measure the message length without terminating nul */
  len = _dbus_printf_string_upper_bound (format, args);

  if (!_dbus_string_lengthen (str, len))
    {
      /* don't leak the copy */
      va_end (args_copy);
      return FALSE;
    }
  
  vsprintf ((char*) (real->str + (real->len - len)),
            format, args_copy);

  va_end (args_copy);

  return TRUE;
}

/**
 * Appends a printf-style formatted string
 * to the #DBusString.
 *
 * @param str the string
 * @param format printf format
 * @returns #FALSE if no memory
 */
dbus_bool_t
_dbus_string_append_printf (DBusString        *str,
                            const char        *format,
                            ...)
{
  va_list args;
  dbus_bool_t retval;
  
  va_start (args, format);
  retval = _dbus_string_append_printf_valist (str, format, args);
  va_end (args);

  return retval;
}

/**
 * Appends block of bytes with the given length to a DBusString.
 *
 * @param str the DBusString
 * @param buffer the bytes to append
 * @param len the number of bytes to append
 * @returns #FALSE if not enough memory.
 */
dbus_bool_t
_dbus_string_append_len (DBusString *str,
                         const char *buffer,
                         int         len)
{
  DBUS_STRING_PREAMBLE (str);
  _dbus_assert (buffer != NULL);
  _dbus_assert (len >= 0);

  return append (real, buffer, len);
}

/**
 * Appends a single byte to the string, returning #FALSE
 * if not enough memory.
 *
 * @param str the string
 * @param byte the byte to append
 * @returns #TRUE on success
 */
dbus_bool_t
_dbus_string_append_byte (DBusString    *str,
                          unsigned char  byte)
{
  DBUS_STRING_PREAMBLE (str);

  if (!set_length (real, real->len + 1))
    return FALSE;

  real->str[real->len-1] = byte;

  return TRUE;
}

#ifdef DBUS_BUILD_TESTS
/**
 * Appends a single Unicode character, encoding the character
 * in UTF-8 format.
 *
 * @param str the string
 * @param ch the Unicode character
 */
dbus_bool_t
_dbus_string_append_unichar (DBusString    *str,
                             dbus_unichar_t ch)
{
  int len;
  int first;
  int i;
  unsigned char *out;
  
  DBUS_STRING_PREAMBLE (str);

  /* this code is from GLib but is pretty standard I think */
  
  len = 0;
  
  if (ch < 0x80)
    {
      first = 0;
      len = 1;
    }
  else if (ch < 0x800)
    {
      first = 0xc0;
      len = 2;
    }
  else if (ch < 0x10000)
    {
      first = 0xe0;
      len = 3;
    }
   else if (ch < 0x200000)
    {
      first = 0xf0;
      len = 4;
    }
  else if (ch < 0x4000000)
    {
      first = 0xf8;
      len = 5;
    }
  else
    {
      first = 0xfc;
      len = 6;
    }

  if (len > (real->max_length - real->len))
    return FALSE; /* real->len + len would overflow */
  
  if (!set_length (real, real->len + len))
    return FALSE;

  out = real->str + (real->len - len);
  
  for (i = len - 1; i > 0; --i)
    {
      out[i] = (ch & 0x3f) | 0x80;
      ch >>= 6;
    }
  out[0] = ch | first;

  return TRUE;
}
#endif /* DBUS_BUILD_TESTS */

static void
delete (DBusRealString *real,
        int             start,
        int             len)
{
  if (len == 0)
    return;
  
  memmove (real->str + start, real->str + start + len, real->len - (start + len));
  real->len -= len;
  real->str[real->len] = '\0';
}

/**
 * Deletes a segment of a DBusString with length len starting at
 * start. (Hint: to clear an entire string, setting length to 0
 * with _dbus_string_set_length() is easier.)
 *
 * @param str the DBusString
 * @param start where to start deleting
 * @param len the number of bytes to delete
 */
void
_dbus_string_delete (DBusString       *str,
                     int               start,
                     int               len)
{
  DBUS_STRING_PREAMBLE (str);
  _dbus_assert (start >= 0);
  _dbus_assert (len >= 0);
  _dbus_assert (start <= real->len);
  _dbus_assert (len <= real->len - start);
  
  delete (real, start, len);
}

static dbus_bool_t
copy (DBusRealString *source,
      int             start,
      int             len,
      DBusRealString *dest,
      int             insert_at)
{
  if (len == 0)
    return TRUE;

  if (!open_gap (len, dest, insert_at))
    return FALSE;
  
  memmove (dest->str + insert_at,
           source->str + start,
           len);

  return TRUE;
}

/**
 * Checks assertions for two strings we're copying a segment between,
 * and declares real_source/real_dest variables.
 *
 * @param source the source string
 * @param start the starting offset
 * @param dest the dest string
 * @param insert_at where the copied segment is inserted
 */
#define DBUS_STRING_COPY_PREAMBLE(source, start, dest, insert_at)       \
  DBusRealString *real_source = (DBusRealString*) source;               \
  DBusRealString *real_dest = (DBusRealString*) dest;                   \
  _dbus_assert ((source) != (dest));                                    \
  DBUS_GENERIC_STRING_PREAMBLE (real_source);                           \
  DBUS_GENERIC_STRING_PREAMBLE (real_dest);                             \
  _dbus_assert (!real_dest->constant);                                  \
  _dbus_assert (!real_dest->locked);                                    \
  _dbus_assert ((start) >= 0);                                          \
  _dbus_assert ((start) <= real_source->len);                           \
  _dbus_assert ((insert_at) >= 0);                                      \
  _dbus_assert ((insert_at) <= real_dest->len)

/**
 * Moves the end of one string into another string. Both strings
 * must be initialized, valid strings.
 *
 * @param source the source string
 * @param start where to chop off the source string
 * @param dest the destination string
 * @param insert_at where to move the chopped-off part of source string
 * @returns #FALSE if not enough memory
 */
dbus_bool_t
_dbus_string_move (DBusString       *source,
                   int               start,
                   DBusString       *dest,
                   int               insert_at)
{
  DBusRealString *real_source = (DBusRealString*) source;
  _dbus_assert (start <= real_source->len);
  
  return _dbus_string_move_len (source, start,
                                real_source->len - start,
                                dest, insert_at);
}

/**
 * Like _dbus_string_move(), but does not delete the section
 * of the source string that's copied to the dest string.
 *
 * @param source the source string
 * @param start where to start copying the source string
 * @param dest the destination string
 * @param insert_at where to place the copied part of source string
 * @returns #FALSE if not enough memory
 */
dbus_bool_t
_dbus_string_copy (const DBusString *source,
                   int               start,
                   DBusString       *dest,
                   int               insert_at)
{
  DBUS_STRING_COPY_PREAMBLE (source, start, dest, insert_at);

  return copy (real_source, start,
               real_source->len - start,
               real_dest,
               insert_at);
}

/**
 * Like _dbus_string_move(), but can move a segment from
 * the middle of the source string.
 *
 * @todo this doesn't do anything with max_length field.
 * we should probably just kill the max_length field though.
 * 
 * @param source the source string
 * @param start first byte of source string to move
 * @param len length of segment to move
 * @param dest the destination string
 * @param insert_at where to move the bytes from the source string
 * @returns #FALSE if not enough memory
 */
dbus_bool_t
_dbus_string_move_len (DBusString       *source,
                       int               start,
                       int               len,
                       DBusString       *dest,
                       int               insert_at)

{
  DBUS_STRING_COPY_PREAMBLE (source, start, dest, insert_at);
  _dbus_assert (len >= 0);
  _dbus_assert ((start + len) <= real_source->len);


  if (len == 0)
    {
      return TRUE;
    }
  else if (start == 0 &&
           len == real_source->len &&
           real_dest->len == 0)
    {
      /* Short-circuit moving an entire existing string to an empty string
       * by just swapping the buffers.
       */
      /* we assume ->constant doesn't matter as you can't have
       * a constant string involved in a move.
       */
#define ASSIGN_DATA(a, b) do {                  \
        (a)->str = (b)->str;                    \
        (a)->len = (b)->len;                    \
        (a)->allocated = (b)->allocated;        \
        (a)->align_offset = (b)->align_offset;  \
      } while (0)
      
      DBusRealString tmp;

      ASSIGN_DATA (&tmp, real_source);
      ASSIGN_DATA (real_source, real_dest);
      ASSIGN_DATA (real_dest, &tmp);

      return TRUE;
    }
  else
    {
      if (!copy (real_source, start, len,
                 real_dest,
                 insert_at))
        return FALSE;
      
      delete (real_source, start,
              len);
      
      return TRUE;
    }
}

/**
 * Like _dbus_string_copy(), but can copy a segment from the middle of
 * the source string.
 *
 * @param source the source string
 * @param start where to start copying the source string
 * @param len length of segment to copy
 * @param dest the destination string
 * @param insert_at where to place the copied segment of source string
 * @returns #FALSE if not enough memory
 */
dbus_bool_t
_dbus_string_copy_len (const DBusString *source,
                       int               start,
                       int               len,
                       DBusString       *dest,
                       int               insert_at)
{
  DBUS_STRING_COPY_PREAMBLE (source, start, dest, insert_at);
  _dbus_assert (len >= 0);
  _dbus_assert (start <= real_source->len);
  _dbus_assert (len <= real_source->len - start);
  
  return copy (real_source, start, len,
               real_dest,
               insert_at);
}

/**
 * Replaces a segment of dest string with a segment of source string.
 *
 * @todo optimize the case where the two lengths are the same, and
 * avoid memmoving the data in the trailing part of the string twice.
 *
 * @todo avoid inserting the source into dest, then deleting
 * the replaced chunk of dest (which creates a potentially large
 * intermediate string). Instead, extend the replaced chunk
 * of dest with padding to the same size as the source chunk,
 * then copy in the source bytes.
 * 
 * @param source the source string
 * @param start where to start copying the source string
 * @param len length of segment to copy
 * @param dest the destination string
 * @param replace_at start of segment of dest string to replace
 * @param replace_len length of segment of dest string to replace
 * @returns #FALSE if not enough memory
 *
 */
dbus_bool_t
_dbus_string_replace_len (const DBusString *source,
                          int               start,
                          int               len,
                          DBusString       *dest,
                          int               replace_at,
                          int               replace_len)
{
  DBUS_STRING_COPY_PREAMBLE (source, start, dest, replace_at);
  _dbus_assert (len >= 0);
  _dbus_assert (start <= real_source->len);
  _dbus_assert (len <= real_source->len - start);
  _dbus_assert (replace_at >= 0);
  _dbus_assert (replace_at <= real_dest->len);
  _dbus_assert (replace_len <= real_dest->len - replace_at);

  if (!copy (real_source, start, len,
             real_dest, replace_at))
    return FALSE;

  delete (real_dest, replace_at + len, replace_len);

  return TRUE;
}

/* Unicode macros and utf8_validate() from GLib Owen Taylor, Havoc
 * Pennington, and Tom Tromey are the authors and authorized relicense.
 */

/** computes length and mask of a unicode character
 * @param Char the char
 * @param Mask the mask variable to assign to
 * @param Len the length variable to assign to
 */
#define UTF8_COMPUTE(Char, Mask, Len)					      \
  if (Char < 128)							      \
    {									      \
      Len = 1;								      \
      Mask = 0x7f;							      \
    }									      \
  else if ((Char & 0xe0) == 0xc0)					      \
    {									      \
      Len = 2;								      \
      Mask = 0x1f;							      \
    }									      \
  else if ((Char & 0xf0) == 0xe0)					      \
    {									      \
      Len = 3;								      \
      Mask = 0x0f;							      \
    }									      \
  else if ((Char & 0xf8) == 0xf0)					      \
    {									      \
      Len = 4;								      \
      Mask = 0x07;							      \
    }									      \
  else if ((Char & 0xfc) == 0xf8)					      \
    {									      \
      Len = 5;								      \
      Mask = 0x03;							      \
    }									      \
  else if ((Char & 0xfe) == 0xfc)					      \
    {									      \
      Len = 6;								      \
      Mask = 0x01;							      \
    }									      \
  else                                                                        \
    {                                                                         \
      Len = 0;                                                               \
      Mask = 0;                                                               \
    }

/**
 * computes length of a unicode character in UTF-8
 * @param Char the char
 */
#define UTF8_LENGTH(Char)              \
  ((Char) < 0x80 ? 1 :                 \
   ((Char) < 0x800 ? 2 :               \
    ((Char) < 0x10000 ? 3 :            \
     ((Char) < 0x200000 ? 4 :          \
      ((Char) < 0x4000000 ? 5 : 6)))))
   
/**
 * Gets a UTF-8 value.
 *
 * @param Result variable for extracted unicode char.
 * @param Chars the bytes to decode
 * @param Count counter variable
 * @param Mask mask for this char
 * @param Len length for this char in bytes
 */
#define UTF8_GET(Result, Chars, Count, Mask, Len)			      \
  (Result) = (Chars)[0] & (Mask);					      \
  for ((Count) = 1; (Count) < (Len); ++(Count))				      \
    {									      \
      if (((Chars)[(Count)] & 0xc0) != 0x80)				      \
	{								      \
	  (Result) = -1;						      \
	  break;							      \
	}								      \
      (Result) <<= 6;							      \
      (Result) |= ((Chars)[(Count)] & 0x3f);				      \
    }

/**
 * Check whether a unicode char is in a valid range.
 *
 * @param Char the character
 */
#define UNICODE_VALID(Char)                   \
    ((Char) < 0x110000 &&                     \
     (((Char) & 0xFFFFF800) != 0xD800) &&     \
     ((Char) < 0xFDD0 || (Char) > 0xFDEF) &&  \
     ((Char) & 0xFFFF) != 0xFFFF)

#ifdef DBUS_BUILD_TESTS
/**
 * Gets a unicode character from a UTF-8 string. Does no validation;
 * you must verify that the string is valid UTF-8 in advance and must
 * pass in the start of a character.
 *
 * @param str the string
 * @param start the start of the UTF-8 character.
 * @param ch_return location to return the character
 * @param end_return location to return the byte index of next character
 */
void
_dbus_string_get_unichar (const DBusString *str,
                          int               start,
                          dbus_unichar_t   *ch_return,
                          int              *end_return)
{
  int i, mask, len;
  dbus_unichar_t result;
  unsigned char c;
  unsigned char *p;
  DBUS_CONST_STRING_PREAMBLE (str);
  _dbus_assert (start >= 0);
  _dbus_assert (start <= real->len);
  
  if (ch_return)
    *ch_return = 0;
  if (end_return)
    *end_return = real->len;
  
  mask = 0;
  p = real->str + start;
  c = *p;
  
  UTF8_COMPUTE (c, mask, len);
  if (len == 0)
    return;
  UTF8_GET (result, p, i, mask, len);

  if (result == (dbus_unichar_t)-1)
    return;

  if (ch_return)
    *ch_return = result;
  if (end_return)
    *end_return = start + len;
}
#endif /* DBUS_BUILD_TESTS */

/**
 * Finds the given substring in the string,
 * returning #TRUE and filling in the byte index
 * where the substring was found, if it was found.
 * Returns #FALSE if the substring wasn't found.
 * Sets *start to the length of the string if the substring
 * is not found.
 *
 * @param str the string
 * @param start where to start looking
 * @param substr the substring
 * @param found return location for where it was found, or #NULL
 * @returns #TRUE if found
 */
dbus_bool_t
_dbus_string_find (const DBusString *str,
                   int               start,
                   const char       *substr,
                   int              *found)
{
  return _dbus_string_find_to (str, start,
                               ((const DBusRealString*)str)->len,
                               substr, found);
}

/**
 * Finds the given substring in the string,
 * up to a certain position,
 * returning #TRUE and filling in the byte index
 * where the substring was found, if it was found.
 * Returns #FALSE if the substring wasn't found.
 * Sets *start to the length of the string if the substring
 * is not found.
 *
 * @param str the string
 * @param start where to start looking
 * @param end where to stop looking
 * @param substr the substring
 * @param found return location for where it was found, or #NULL
 * @returns #TRUE if found
 */
dbus_bool_t
_dbus_string_find_to (const DBusString *str,
		      int               start,
		      int               end,
		      const char       *substr,
		      int              *found)
{
  int i;
  DBUS_CONST_STRING_PREAMBLE (str);
  _dbus_assert (substr != NULL);
  _dbus_assert (start <= real->len);
  _dbus_assert (start >= 0);
  _dbus_assert (substr != NULL);
  _dbus_assert (end <= real->len);
  _dbus_assert (start <= end);

  /* we always "find" an empty string */
  if (*substr == '\0')
    {
      if (found)
        *found = start;
      return TRUE;
    }

  i = start;
  while (i < end)
    {
      if (real->str[i] == substr[0])
        {
          int j = i + 1;
          
          while (j < end)
            {
              if (substr[j - i] == '\0')
                break;
              else if (real->str[j] != substr[j - i])
                break;
              
              ++j;
            }

          if (substr[j - i] == '\0')
            {
              if (found)
                *found = i;
              return TRUE;
            }
        }
      
      ++i;
    }

  if (found)
    *found = end;
  
  return FALSE;  
}

/**
 * Finds a blank (space or tab) in the string. Returns #TRUE
 * if found, #FALSE otherwise. If a blank is not found sets
 * *found to the length of the string.
 *
 * @param str the string
 * @param start byte index to start looking
 * @param found place to store the location of the first blank
 * @returns #TRUE if a blank was found
 */
dbus_bool_t
_dbus_string_find_blank (const DBusString *str,
                         int               start,
                         int              *found)
{
  int i;
  DBUS_CONST_STRING_PREAMBLE (str);
  _dbus_assert (start <= real->len);
  _dbus_assert (start >= 0);
  
  i = start;
  while (i < real->len)
    {
      if (real->str[i] == ' ' ||
          real->str[i] == '\t')
        {
          if (found)
            *found = i;
          return TRUE;
        }
      
      ++i;
    }

  if (found)
    *found = real->len;
  
  return FALSE;
}

/**
 * Skips blanks from start, storing the first non-blank in *end
 * (blank is space or tab).
 *
 * @param str the string
 * @param start where to start
 * @param end where to store the first non-blank byte index
 */
void
_dbus_string_skip_blank (const DBusString *str,
                         int               start,
                         int              *end)
{
  int i;
  DBUS_CONST_STRING_PREAMBLE (str);
  _dbus_assert (start <= real->len);
  _dbus_assert (start >= 0);
  
  i = start;
  while (i < real->len)
    {
      if (!DBUS_IS_ASCII_BLANK (real->str[i]))
        break;
      
      ++i;
    }

  _dbus_assert (i == real->len || !DBUS_IS_ASCII_WHITE (real->str[i]));
  
  if (end)
    *end = i;
}


/**
 * Skips whitespace from start, storing the first non-whitespace in *end.
 * (whitespace is space, tab, newline, CR).
 *
 * @param str the string
 * @param start where to start
 * @param end where to store the first non-whitespace byte index
 */
void
_dbus_string_skip_white (const DBusString *str,
                         int               start,
                         int              *end)
{
  int i;
  DBUS_CONST_STRING_PREAMBLE (str);
  _dbus_assert (start <= real->len);
  _dbus_assert (start >= 0);
  
  i = start;
  while (i < real->len)
    {
      if (!DBUS_IS_ASCII_WHITE (real->str[i]))
        break;
      
      ++i;
    }

  _dbus_assert (i == real->len || !(DBUS_IS_ASCII_WHITE (real->str[i])));
  
  if (end)
    *end = i;
}

/**
 * Skips whitespace from end, storing the start index of the trailing
 * whitespace in *start. (whitespace is space, tab, newline, CR).
 *
 * @param str the string
 * @param end where to start scanning backward
 * @param start where to store the start of whitespace chars
 */
void
_dbus_string_skip_white_reverse (const DBusString *str,
                                 int               end,
                                 int              *start)
{
  int i;
  DBUS_CONST_STRING_PREAMBLE (str);
  _dbus_assert (end <= real->len);
  _dbus_assert (end >= 0);
  
  i = end;
  while (i > 0)
    {
      if (!DBUS_IS_ASCII_WHITE (real->str[i-1]))
        break;
      --i;
    }

  _dbus_assert (i >= 0 && (i == 0 || !(DBUS_IS_ASCII_WHITE (real->str[i-1]))));
  
  if (start)
    *start = i;
}

/**
 * Assigns a newline-terminated or \\r\\n-terminated line from the front
 * of the string to the given dest string. The dest string's previous
 * contents are deleted. If the source string contains no newline,
 * moves the entire source string to the dest string.
 *
 * @todo owen correctly notes that this is a stupid function (it was
 * written purely for test code,
 * e.g. dbus-message-builder.c). Probably should be enforced as test
 * code only with ifdef DBUS_BUILD_TESTS
 * 
 * @param source the source string
 * @param dest the destination string (contents are replaced)
 * @returns #FALSE if no memory, or source has length 0
 */
dbus_bool_t
_dbus_string_pop_line (DBusString *source,
                       DBusString *dest)
{
  int eol;
  dbus_bool_t have_newline;
  
  _dbus_string_set_length (dest, 0);
  
  eol = 0;
  if (_dbus_string_find (source, 0, "\n", &eol))
    {
      have_newline = TRUE;
      eol += 1; /* include newline */
    }
  else
    {
      eol = _dbus_string_get_length (source);
      have_newline = FALSE;
    }

  if (eol == 0)
    return FALSE; /* eof */
  
  if (!_dbus_string_move_len (source, 0, eol,
                              dest, 0))
    {
      return FALSE;
    }

  /* dump the newline and the \r if we have one */
  if (have_newline)
    {
      dbus_bool_t have_cr;
      
      _dbus_assert (_dbus_string_get_length (dest) > 0);

      if (_dbus_string_get_length (dest) > 1 &&
          _dbus_string_get_byte (dest,
                                 _dbus_string_get_length (dest) - 2) == '\r')
        have_cr = TRUE;
      else
        have_cr = FALSE;
        
      _dbus_string_set_length (dest,
                               _dbus_string_get_length (dest) -
                               (have_cr ? 2 : 1));
    }
  
  return TRUE;
}

#ifdef DBUS_BUILD_TESTS
/**
 * Deletes up to and including the first blank space
 * in the string.
 *
 * @param str the string
 */
void
_dbus_string_delete_first_word (DBusString *str)
{
  int i;
  
  if (_dbus_string_find_blank (str, 0, &i))
    _dbus_string_skip_blank (str, i, &i);

  _dbus_string_delete (str, 0, i);
}
#endif

#ifdef DBUS_BUILD_TESTS
/**
 * Deletes any leading blanks in the string
 *
 * @param str the string
 */
void
_dbus_string_delete_leading_blanks (DBusString *str)
{
  int i;
  
  _dbus_string_skip_blank (str, 0, &i);

  if (i > 0)
    _dbus_string_delete (str, 0, i);
}
#endif

/**
 * Deletes leading and trailing whitespace
 * 
 * @param str the string
 */
void
_dbus_string_chop_white(DBusString *str)
{
  int i;
  
  _dbus_string_skip_white (str, 0, &i);

  if (i > 0)
    _dbus_string_delete (str, 0, i);
  
  _dbus_string_skip_white_reverse (str, _dbus_string_get_length (str), &i);

  _dbus_string_set_length (str, i);
}

/**
 * Tests two DBusString for equality.
 *
 * @todo memcmp is probably faster
 *
 * @param a first string
 * @param b second string
 * @returns #TRUE if equal
 */
dbus_bool_t
_dbus_string_equal (const DBusString *a,
                    const DBusString *b)
{
  const unsigned char *ap;
  const unsigned char *bp;
  const unsigned char *a_end;
  const DBusRealString *real_a = (const DBusRealString*) a;
  const DBusRealString *real_b = (const DBusRealString*) b;
  DBUS_GENERIC_STRING_PREAMBLE (real_a);
  DBUS_GENERIC_STRING_PREAMBLE (real_b);

  if (real_a->len != real_b->len)
    return FALSE;

  ap = real_a->str;
  bp = real_b->str;
  a_end = real_a->str + real_a->len;
  while (ap != a_end)
    {
      if (*ap != *bp)
        return FALSE;
      
      ++ap;
      ++bp;
    }

  return TRUE;
}

#ifdef DBUS_BUILD_TESTS
/**
 * Tests two DBusString for equality up to the given length.
 * The strings may be shorter than the given length.
 *
 * @todo write a unit test
 *
 * @todo memcmp is probably faster
 *
 * @param a first string
 * @param b second string
 * @param len the maximum length to look at
 * @returns #TRUE if equal for the given number of bytes
 */
dbus_bool_t
_dbus_string_equal_len (const DBusString *a,
                        const DBusString *b,
                        int               len)
{
  const unsigned char *ap;
  const unsigned char *bp;
  const unsigned char *a_end;
  const DBusRealString *real_a = (const DBusRealString*) a;
  const DBusRealString *real_b = (const DBusRealString*) b;
  DBUS_GENERIC_STRING_PREAMBLE (real_a);
  DBUS_GENERIC_STRING_PREAMBLE (real_b);

  if (real_a->len != real_b->len &&
      (real_a->len < len || real_b->len < len))
    return FALSE;

  ap = real_a->str;
  bp = real_b->str;
  a_end = real_a->str + MIN (real_a->len, len);
  while (ap != a_end)
    {
      if (*ap != *bp)
        return FALSE;
      
      ++ap;
      ++bp;
    }

  return TRUE;
}
#endif /* DBUS_BUILD_TESTS */

/**
 * Tests two sub-parts of two DBusString for equality.  The specified
 * range of the first string must exist; the specified start position
 * of the second string must exist.
 *
 * @todo write a unit test
 *
 * @todo memcmp is probably faster
 *
 * @param a first string
 * @param a_start where to start substring in first string
 * @param a_len length of substring in first string
 * @param b second string
 * @param b_start where to start substring in second string
 * @returns #TRUE if the two substrings are equal
 */
dbus_bool_t
_dbus_string_equal_substring (const DBusString  *a,
                              int                a_start,
                              int                a_len,
                              const DBusString  *b,
                              int                b_start)
{
  const unsigned char *ap;
  const unsigned char *bp;
  const unsigned char *a_end;
  const DBusRealString *real_a = (const DBusRealString*) a;
  const DBusRealString *real_b = (const DBusRealString*) b;
  DBUS_GENERIC_STRING_PREAMBLE (real_a);
  DBUS_GENERIC_STRING_PREAMBLE (real_b);
  _dbus_assert (a_start >= 0);
  _dbus_assert (a_len >= 0);
  _dbus_assert (a_start <= real_a->len);
  _dbus_assert (a_len <= real_a->len - a_start);
  _dbus_assert (b_start >= 0);
  _dbus_assert (b_start <= real_b->len);
  
  if (a_len > real_b->len - b_start)
    return FALSE;

  ap = real_a->str + a_start;
  bp = real_b->str + b_start;
  a_end = ap + a_len;
  while (ap != a_end)
    {
      if (*ap != *bp)
        return FALSE;
      
      ++ap;
      ++bp;
    }

  _dbus_assert (bp <= (real_b->str + real_b->len));
  
  return TRUE;
}

/**
 * Checks whether a string is equal to a C string.
 *
 * @param a the string
 * @param c_str the C string
 * @returns #TRUE if equal
 */
dbus_bool_t
_dbus_string_equal_c_str (const DBusString *a,
                          const char       *c_str)
{
  const unsigned char *ap;
  const unsigned char *bp;
  const unsigned char *a_end;
  const DBusRealString *real_a = (const DBusRealString*) a;
  DBUS_GENERIC_STRING_PREAMBLE (real_a);
  _dbus_assert (c_str != NULL);
  
  ap = real_a->str;
  bp = (const unsigned char*) c_str;
  a_end = real_a->str + real_a->len;
  while (ap != a_end && *bp)
    {
      if (*ap != *bp)
        return FALSE;
      
      ++ap;
      ++bp;
    }

  if (ap != a_end || *bp)
    return FALSE;
  
  return TRUE;
}

#ifdef DBUS_BUILD_TESTS
/**
 * Checks whether a string starts with the given C string.
 *
 * @param a the string
 * @param c_str the C string
 * @returns #TRUE if string starts with it
 */
dbus_bool_t
_dbus_string_starts_with_c_str (const DBusString *a,
                                const char       *c_str)
{
  const unsigned char *ap;
  const unsigned char *bp;
  const unsigned char *a_end;
  const DBusRealString *real_a = (const DBusRealString*) a;
  DBUS_GENERIC_STRING_PREAMBLE (real_a);
  _dbus_assert (c_str != NULL);
  
  ap = real_a->str;
  bp = (const unsigned char*) c_str;
  a_end = real_a->str + real_a->len;
  while (ap != a_end && *bp)
    {
      if (*ap != *bp)
        return FALSE;
      
      ++ap;
      ++bp;
    }

  if (*bp == '\0')
    return TRUE;
  else
    return FALSE;
}
#endif /* DBUS_BUILD_TESTS */

/**
 * Appends a two-character hex digit to a string, where the hex digit
 * has the value of the given byte.
 *
 * @param str the string
 * @param byte the byte
 * @returns #FALSE if no memory
 */
dbus_bool_t
_dbus_string_append_byte_as_hex (DBusString *str,
                                 int         byte)
{
  const char hexdigits[16] = {
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
    'a', 'b', 'c', 'd', 'e', 'f'
  };

  if (!_dbus_string_append_byte (str,
                                 hexdigits[(byte >> 4)]))
    return FALSE;
  
  if (!_dbus_string_append_byte (str,
                                 hexdigits[(byte & 0x0f)]))
    {
      _dbus_string_set_length (str,
                               _dbus_string_get_length (str) - 1);
      return FALSE;
    }

  return TRUE;
}

/**
 * Encodes a string in hex, the way MD5 and SHA-1 are usually
 * encoded. (Each byte is two hex digits.)
 *
 * @param source the string to encode
 * @param start byte index to start encoding
 * @param dest string where encoded data should be placed
 * @param insert_at where to place encoded data
 * @returns #TRUE if encoding was successful, #FALSE if no memory etc.
 */
dbus_bool_t
_dbus_string_hex_encode (const DBusString *source,
                         int               start,
                         DBusString       *dest,
                         int               insert_at)
{
  DBusString result;
  const unsigned char *p;
  const unsigned char *end;
  dbus_bool_t retval;
  
  _dbus_assert (start <= _dbus_string_get_length (source));

  if (!_dbus_string_init (&result))
    return FALSE;

  retval = FALSE;
  
  p = (const unsigned char*) _dbus_string_get_const_data (source);
  end = p + _dbus_string_get_length (source);
  p += start;
  
  while (p != end)
    {
      if (!_dbus_string_append_byte_as_hex (&result, *p))
        goto out;
      
      ++p;
    }

  if (!_dbus_string_move (&result, 0, dest, insert_at))
    goto out;

  retval = TRUE;

 out:
  _dbus_string_free (&result);
  return retval;
}

/**
 * Decodes a string from hex encoding.
 *
 * @param source the string to decode
 * @param start byte index to start decode
 * @param end_return return location of the end of the hex data, or #NULL
 * @param dest string where decoded data should be placed
 * @param insert_at where to place decoded data
 * @returns #TRUE if decoding was successful, #FALSE if no memory.
 */
dbus_bool_t
_dbus_string_hex_decode (const DBusString *source,
                         int               start,
			 int              *end_return,
                         DBusString       *dest,
                         int               insert_at)
{
  DBusString result;
  const unsigned char *p;
  const unsigned char *end;
  dbus_bool_t retval;
  dbus_bool_t high_bits;
  
  _dbus_assert (start <= _dbus_string_get_length (source));

  if (!_dbus_string_init (&result))
    return FALSE;

  retval = FALSE;

  high_bits = TRUE;
  p = (const unsigned char*) _dbus_string_get_const_data (source);
  end = p + _dbus_string_get_length (source);
  p += start;
  
  while (p != end)
    {
      unsigned int val;

      switch (*p)
        {
        case '0':
          val = 0;
          break;
        case '1':
          val = 1;
          break;
        case '2':
          val = 2;
          break;
        case '3':
          val = 3;
          break;
        case '4':
          val = 4;
          break;
        case '5':
          val = 5;
          break;
        case '6':
          val = 6;
          break;
        case '7':
          val = 7;
          break;
        case '8':
          val = 8;
          break;
        case '9':
          val = 9;
          break;
        case 'a':
        case 'A':
          val = 10;
          break;
        case 'b':
        case 'B':
          val = 11;
          break;
        case 'c':
        case 'C':
          val = 12;
          break;
        case 'd':
        case 'D':
          val = 13;
          break;
        case 'e':
        case 'E':
          val = 14;
          break;
        case 'f':
        case 'F':
          val = 15;
          break;
        default:
          goto done;
        }

      if (high_bits)
        {
          if (!_dbus_string_append_byte (&result,
                                         val << 4))
	    goto out;
        }
      else
        {
          int len;
          unsigned char b;

          len = _dbus_string_get_length (&result);
          
          b = _dbus_string_get_byte (&result, len - 1);

          b |= val;

          _dbus_string_set_byte (&result, len - 1, b);
        }

      high_bits = !high_bits;

      ++p;
    }

 done:
  if (!_dbus_string_move (&result, 0, dest, insert_at))
    goto out;

  if (end_return)
    *end_return = p - (const unsigned char*) _dbus_string_get_const_data (source);

  retval = TRUE;
  
 out:
  _dbus_string_free (&result);  
  return retval;
}

/**
 * Checks that the given range of the string is valid ASCII with no
 * nul bytes. If the given range is not entirely contained in the
 * string, returns #FALSE.
 *
 * @todo this is inconsistent with most of DBusString in that
 * it allows a start,len range that extends past the string end.
 * 
 * @param str the string
 * @param start first byte index to check
 * @param len number of bytes to check
 * @returns #TRUE if the byte range exists and is all valid ASCII
 */
dbus_bool_t
_dbus_string_validate_ascii (const DBusString *str,
                             int               start,
                             int               len)
{
  const unsigned char *s;
  const unsigned char *end;
  DBUS_CONST_STRING_PREAMBLE (str);
  _dbus_assert (start >= 0);
  _dbus_assert (start <= real->len);
  _dbus_assert (len >= 0);
  
  if (len > real->len - start)
    return FALSE;
  
  s = real->str + start;
  end = s + len;
  while (s != end)
    {
      if (_DBUS_UNLIKELY (!_DBUS_ISASCII (*s)))
        return FALSE;
        
      ++s;
    }
  
  return TRUE;
}

/**
 * Checks that the given range of the string is valid UTF-8. If the
 * given range is not entirely contained in the string, returns
 * #FALSE. If the string contains any nul bytes in the given range,
 * returns #FALSE. If the start and start+len are not on character
 * boundaries, returns #FALSE.
 *
 * @todo this is inconsistent with most of DBusString in that
 * it allows a start,len range that extends past the string end.
 * 
 * @param str the string
 * @param start first byte index to check
 * @param len number of bytes to check
 * @returns #TRUE if the byte range exists and is all valid UTF-8
 */
dbus_bool_t
_dbus_string_validate_utf8  (const DBusString *str,
                             int               start,
                             int               len)
{
  const unsigned char *p;
  const unsigned char *end;
  DBUS_CONST_STRING_PREAMBLE (str);
  _dbus_assert (start >= 0);
  _dbus_assert (start <= real->len);
  _dbus_assert (len >= 0);

  /* we are doing _DBUS_UNLIKELY() here which might be
   * dubious in a generic library like GLib, but in D-Bus
   * we know we're validating messages and that it would
   * only be evil/broken apps that would have invalid
   * UTF-8. Also, this function seems to be a performance
   * bottleneck in profiles.
   */
  
  if (_DBUS_UNLIKELY (len > real->len - start))
    return FALSE;
  
  p = real->str + start;
  end = p + len;
  
  while (p < end)
    {
      int i, mask, char_len;
      dbus_unichar_t result;

      /* nul bytes considered invalid */
      if (*p == '\0')
        break;
      
      /* Special-case ASCII; this makes us go a lot faster in
       * D-Bus profiles where we are typically validating
       * function names and such. We have to know that
       * all following checks will pass for ASCII though,
       * comments follow ...
       */      
      if (*p < 128)
        {
          ++p;
          continue;
        }
      
      UTF8_COMPUTE (*p, mask, char_len);

      if (_DBUS_UNLIKELY (char_len == 0))  /* ASCII: char_len == 1 */
        break;

      /* check that the expected number of bytes exists in the remaining length */
      if (_DBUS_UNLIKELY ((end - p) < char_len)) /* ASCII: p < end and char_len == 1 */
        break;
        
      UTF8_GET (result, p, i, mask, char_len);

      /* Check for overlong UTF-8 */
      if (_DBUS_UNLIKELY (UTF8_LENGTH (result) != char_len)) /* ASCII: UTF8_LENGTH == 1 */
        break;
#if 0
      /* The UNICODE_VALID check below will catch this */
      if (_DBUS_UNLIKELY (result == (dbus_unichar_t)-1)) /* ASCII: result = ascii value */
        break;
#endif

      if (_DBUS_UNLIKELY (!UNICODE_VALID (result))) /* ASCII: always valid */
        break;

      /* UNICODE_VALID should have caught it */
      _dbus_assert (result != (dbus_unichar_t)-1);
      
      p += char_len;
    }

  /* See that we covered the entire length if a length was
   * passed in
   */
  if (_DBUS_UNLIKELY (p != end))
    return FALSE;
  else
    return TRUE;
}

/**
 * Checks that the given range of the string is all nul bytes. If the
 * given range is not entirely contained in the string, returns
 * #FALSE.
 *
 * @todo this is inconsistent with most of DBusString in that
 * it allows a start,len range that extends past the string end.
 * 
 * @param str the string
 * @param start first byte index to check
 * @param len number of bytes to check
 * @returns #TRUE if the byte range exists and is all nul bytes
 */
dbus_bool_t
_dbus_string_validate_nul (const DBusString *str,
                           int               start,
                           int               len)
{
  const unsigned char *s;
  const unsigned char *end;
  DBUS_CONST_STRING_PREAMBLE (str);
  _dbus_assert (start >= 0);
  _dbus_assert (len >= 0);
  _dbus_assert (start <= real->len);
  
  if (len > real->len - start)
    return FALSE;
  
  s = real->str + start;
  end = s + len;
  while (s != end)
    {
      if (_DBUS_UNLIKELY (*s != '\0'))
        return FALSE;
      ++s;
    }
  
  return TRUE;
}

/**
 * Clears all allocated bytes in the string to zero.
 *
 * @param str the string
 */
void
_dbus_string_zero (DBusString *str)
{
  DBUS_STRING_PREAMBLE (str);

  memset (real->str - real->align_offset, '\0', real->allocated);
}
/** @} */

/* tests are in dbus-string-util.c */