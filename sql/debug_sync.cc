/* Copyright (c) 2009, 2013, Oracle and/or its affiliates.
   Copyright (c) 2009, 2022, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

/* see include/mysql/service_debug_sync.h for debug sync documentation */

#include "mariadb.h"
#include "debug_sync.h"
#include <cstring>

#if defined(ENABLED_DEBUG_SYNC)

/*
  Due to weaknesses in our include files, we need to include
  sql_priv.h here. To have THD declared, we need to include
  sql_class.h. This includes log_event.h, which in turn requires
  declarations from sql_priv.h (e.g. OPTION_AUTO_IS_NULL).
  sql_priv.h includes almost everything, so is sufficient here.
*/
#include "sql_priv.h"
#include "sql_parse.h"

/*
  Action to perform at a synchronization point.
  NOTE: This structure is moved around in memory by realloc(), qsort(),
        and memmove(). Do not add objects with non-trivial constructors
        or destructors, which might prevent moving of this structure
        with these functions.
*/
struct st_debug_sync_action
{
  ulong         activation_count;       /* MY_MAX(hit_limit, execute) */
  ulong         hit_limit;              /* hits before kill query */
  ulong         execute;                /* executes before self-clear */
  ulong         timeout;                /* wait_for timeout */
  String        signal;                 /* signal to emit */
  String        wait_for;               /* signal to wait for */
  String        sync_point;             /* sync point name */
  bool          need_sort;              /* if new action, array needs sort */
  bool          clear_event;            /* do not clear signal when waited
                                           for if false. */
};

/* Debug sync control. Referenced by THD. */
struct st_debug_sync_control
{
  st_debug_sync_action  *ds_action;             /* array of actions */
  uint                  ds_active;              /* # active actions */
  uint                  ds_allocated;           /* # allocated actions */
  ulonglong             dsp_hits;               /* statistics */
  ulonglong             dsp_executed;           /* statistics */
  ulonglong             dsp_max_active;         /* statistics */
  /*
    thd->proc_info points at unsynchronized memory.
    It must not go away as long as the thread exists.
  */
  char                  ds_proc_info[80];       /* proc_info string */
};




/**
  Definitions for the debug sync facility.
  1. Global string variable to hold a set of of "signals".
  2. Global condition variable for signaling and waiting.
  3. Global mutex to synchronize access to the above.
*/
struct st_debug_sync_globals
{
  Hash_set<LEX_CSTRING> ds_signal_set;          /* A set of active signals */
  mysql_cond_t          ds_cond;                /* condition variable */
  mysql_mutex_t         ds_mutex;               /* mutex variable */
  ulonglong             dsp_hits;               /* statistics */
  ulonglong             dsp_executed;           /* statistics */
  ulonglong             dsp_max_active;         /* statistics */

  st_debug_sync_globals() :
    ds_signal_set(PSI_NOT_INSTRUMENTED, signal_key),
    dsp_hits (0), dsp_executed(0), dsp_max_active(0) {};
  ~st_debug_sync_globals()
  {
    clear_set();
  }

  void clear_set()
  {
    for (LEX_CSTRING &s : ds_signal_set)
      my_free(&s);
    ds_signal_set.clear();
  }

  /* Hash key function for ds_signal_set. */
  static const uchar *signal_key(const void *str_, size_t *klen, my_bool)
  {
    const LEX_CSTRING *str= static_cast<const LEX_CSTRING*>(str_);
    *klen= str->length;
    return reinterpret_cast<const uchar*>(str->str);
  }

  /**
    Return true if the signal is found in global signal list.

    @param signal_name Signal name identifying the signal.

    @note
      If signal is found in the global signal set, it means that the
      signal thread has signalled to the waiting thread. This method
      must be called with the debug_sync_global.ds_mutex held.

    @retval true  if signal is found in the global signal list.
    @retval false otherwise.
  */

  inline bool is_signalled(const char *signal_name, size_t length)
  {
    return ds_signal_set.find(signal_name, length);
  }

  void clear_signal(const String &signal_name)
  {
    DBUG_ENTER("clear_signal");
    LEX_CSTRING *record= ds_signal_set.find(signal_name.ptr(),
                                            signal_name.length());
    if (record)
    {
      ds_signal_set.remove(record);
      my_free(record);
    }
    DBUG_VOID_RETURN;
  }

  bool set_signal(const char *signal_name, size_t length)
  {
    /* Need to check if the signal is already in the hash set, because
       Hash_set doesn't differentiate between OOM and key already in. */
    if (is_signalled(signal_name, length))
      return FALSE;
    /* LEX_CSTRING and the string allocated with only one malloc. */
    LEX_CSTRING *s= (LEX_CSTRING *) my_malloc(PSI_NOT_INSTRUMENTED,
                                              sizeof(LEX_CSTRING) + length + 1,
                                              MYF(0));
    char *str= (char *)(s + 1);
    memcpy(str, signal_name, length);
    str[length]= '\0';

    s->length= length;
    s->str= str;
    if (ds_signal_set.insert(s))
      return TRUE;
    return FALSE;
  }
};

static st_debug_sync_globals *debug_sync_global; /* All globals in one object */

/**
  Callbacks from C files.
*/
C_MODE_START
static void debug_sync(THD *thd, const char *sync_point_name, size_t name_len);
static int debug_sync_qsort_cmp(const void *, const void *);
C_MODE_END

#ifdef HAVE_PSI_INTERFACE
static PSI_mutex_key key_debug_sync_globals_ds_mutex;

static PSI_mutex_info all_debug_sync_mutexes[]=
{
  { &key_debug_sync_globals_ds_mutex, "DEBUG_SYNC::mutex", PSI_FLAG_GLOBAL}
};

static PSI_cond_key key_debug_sync_globals_ds_cond;

static PSI_cond_info all_debug_sync_conds[]=
{
  { &key_debug_sync_globals_ds_cond, "DEBUG_SYNC::cond", PSI_FLAG_GLOBAL}
};

static void init_debug_sync_psi_keys(void)
{
  const char* category= "sql";
  int count;

  count= array_elements(all_debug_sync_mutexes);
  mysql_mutex_register(category, all_debug_sync_mutexes, count);

  count= array_elements(all_debug_sync_conds);
  mysql_cond_register(category, all_debug_sync_conds, count);
}
#endif /* HAVE_PSI_INTERFACE */


/**
  Set the THD::proc_info without instrumentation.
  This method is private to DEBUG_SYNC,
  and on purpose avoid any use of:
  - the SHOW PROFILE instrumentation
  - the PERFORMANCE_SCHEMA instrumentation
  so that using DEBUG_SYNC() in the server code
  does not cause the instrumentations to record
  spurious data.
*/
static const char*
debug_sync_thd_proc_info(THD *thd, const char* info)
{
  const char* old_proc_info= thd->proc_info;
  thd->proc_info= info;
  return old_proc_info;
}


/**
  Initialize the debug sync facility at server start.

  @return status
    @retval     0       ok
    @retval     != 0    error
*/

int debug_sync_init(void)
{
  DBUG_ENTER("debug_sync_init");

#ifdef HAVE_PSI_INTERFACE
  init_debug_sync_psi_keys();
#endif

  if (!debug_sync_global)
    debug_sync_global= new st_debug_sync_globals();

  if (opt_debug_sync_timeout)
  {
    int rc;

    /* Initialize the global variables. */
    debug_sync_global->clear_set();
    if ((rc= mysql_cond_init(key_debug_sync_globals_ds_cond,
                             &debug_sync_global->ds_cond, NULL)) ||
        (rc= mysql_mutex_init(key_debug_sync_globals_ds_mutex,
                              &debug_sync_global->ds_mutex,
                              MY_MUTEX_INIT_FAST)))
      DBUG_RETURN(rc); /* purecov: inspected */

    /* Set the call back pointer in C files. */
    debug_sync_C_callback_ptr= debug_sync;
  }

  DBUG_RETURN(0);
}


/**
  End the debug sync facility.

  @description
    This is called at server shutdown or after a thread initialization error.
*/

void debug_sync_end(void)
{
  DBUG_ENTER("debug_sync_end");

  /* End the facility only if it had been initialized. */
  if (debug_sync_C_callback_ptr)
  {
    /* Clear the call back pointer in C files. */
    debug_sync_C_callback_ptr= NULL;

    /* Destroy the global variables. */
    debug_sync_global->clear_set();
    mysql_cond_destroy(&debug_sync_global->ds_cond);
    mysql_mutex_destroy(&debug_sync_global->ds_mutex);

    /* Print statistics. */
    {
      sql_print_information("Debug sync points hit:                   %lld",
                            debug_sync_global->dsp_hits);
      sql_print_information("Debug sync points executed:              %lld",
                            debug_sync_global->dsp_executed);
      sql_print_information("Debug sync points max active per thread: %lld",
                            debug_sync_global->dsp_max_active);
    }
  }

  delete debug_sync_global;
  /* Just to be safe */
  debug_sync_global= NULL;

  DBUG_VOID_RETURN;
}


/* purecov: begin tested */

/**
  Disable the facility after lack of memory if no error can be returned.

  @note
    Do not end the facility here because the global variables can
    be in use by other threads.
*/

static void debug_sync_emergency_disable(void)
{
  DBUG_ENTER("debug_sync_emergency_disable");

  opt_debug_sync_timeout= 0;

  DBUG_PRINT("debug_sync",
             ("Debug Sync Facility disabled due to lack of memory."));
  sql_print_error("Debug Sync Facility disabled due to lack of memory.");

  DBUG_VOID_RETURN;
}

/* purecov: end */


/**
  Initialize the debug sync facility at thread start.

  @param[in]    thd             thread handle
*/

void debug_sync_init_thread(THD *thd)
{
  DBUG_ENTER("debug_sync_init_thread");
  DBUG_ASSERT(thd);

  if (opt_debug_sync_timeout)
  {
    thd->debug_sync_control= (st_debug_sync_control*)
      my_malloc(PSI_NOT_INSTRUMENTED,
                sizeof(st_debug_sync_control),
                MYF(MY_WME | MY_ZEROFILL | MY_THREAD_SPECIFIC));
    if (!thd->debug_sync_control)
    {
      /*
        Error is reported by my_malloc().
        We must disable the facility. We have no way to return an error.
      */
      debug_sync_emergency_disable(); /* purecov: tested */
    }
  }

  DBUG_VOID_RETURN;
}


/**
  Returns an allocated buffer containing a comma-separated C string of all
  active signals.

  Buffer must be freed by the caller.
*/
static const char *get_signal_set_as_string()
{
  mysql_mutex_assert_owner(&debug_sync_global->ds_mutex);
  size_t req_size= 1; // In case of empty set for the end '\0' char.

  for (size_t i= 0; i < debug_sync_global->ds_signal_set.size(); i++)
    req_size+= debug_sync_global->ds_signal_set.at(i)->length + 1;

  char *buf= (char *) my_malloc(PSI_NOT_INSTRUMENTED, req_size, MYF(0));
  if (!buf)
    return nullptr;
  memset(buf, '\0', req_size);

  char *cur_pos= buf;
  for (size_t i= 0; i < debug_sync_global->ds_signal_set.size(); i++)
  {
    const LEX_CSTRING *signal= debug_sync_global->ds_signal_set.at(i);
    memcpy(cur_pos, signal->str, signal->length);
    if (i != debug_sync_global->ds_signal_set.size() - 1)
      cur_pos[signal->length]= ',';
    else
      cur_pos[signal->length] = '\0';
    cur_pos+= signal->length + 1;
  }
  return buf;
}


/**
  End the debug sync facility at thread end.

  @param[in]    thd             thread handle
*/

void debug_sync_end_thread(THD *thd)
{
  DBUG_ENTER("debug_sync_end_thread");
  DBUG_ASSERT(thd);

  if (thd->debug_sync_control)
  {
    st_debug_sync_control *ds_control= thd->debug_sync_control;

    if (ds_control->ds_action)
    {
      st_debug_sync_action *action= ds_control->ds_action;
      st_debug_sync_action *action_end= action + ds_control->ds_allocated;
      for (; action < action_end; action++)
      {
        action->signal.free();
        action->wait_for.free();
        action->sync_point.free();
      }
      my_free(ds_control->ds_action);
    }

    /* Statistics. */
    /*
      Protect access with debug_sync_global->ds_mutex only if
      it had been initialized.
    */
    if (debug_sync_C_callback_ptr)
      mysql_mutex_lock(&debug_sync_global->ds_mutex);

    debug_sync_global->dsp_hits+=           ds_control->dsp_hits;
    debug_sync_global->dsp_executed+=       ds_control->dsp_executed;
    if (debug_sync_global->dsp_max_active < ds_control->dsp_max_active)
      debug_sync_global->dsp_max_active=    ds_control->dsp_max_active;

    /*
      Protect access with debug_sync_global->ds_mutex only if
      it had been initialized.
    */
    if (debug_sync_C_callback_ptr)
      mysql_mutex_unlock(&debug_sync_global->ds_mutex);

    my_free(ds_control);
    thd->debug_sync_control= NULL;
  }

  DBUG_VOID_RETURN;
}


void debug_sync_reset_thread(THD *thd)
{
  if (thd->debug_sync_control)
  {
    /*
      This synchronization point can be used to synchronize on thread end.
      This is the latest point in a THD's life, where this can be done.
    */
    DEBUG_SYNC(thd, "thread_end");
    thd->debug_sync_control->ds_active= 0;
  }
}


/**
  Move a string by length.

  @param[out]   to              buffer for the resulting string
  @param[in]    to_end          end of buffer
  @param[in]    from            source string
  @param[in]    length          number of bytes to copy

  @return       pointer to end of copied string
*/

static char *debug_sync_bmove_len(char *to, char *to_end,
                                  const char *from, size_t length)
{
  DBUG_ASSERT(to);
  DBUG_ASSERT(to_end);
  DBUG_ASSERT(!length || from);
  set_if_smaller(length, (size_t) (to_end - to));
  if (length)
    memcpy(to, from, length);
  return (to + length);
}


#ifdef DBUG_TRACE

/**
  Create a string that describes an action.

  @param[out]   result          buffer for the resulting string
  @param[in]    size            size of result buffer
  @param[in]    action          action to describe
*/

static void debug_sync_action_string(char *result, uint size,
                                     st_debug_sync_action *action)
{
  char  *wtxt= result;
  char  *wend= wtxt + size - 1; /* Allow emergency '\0'. */
  DBUG_ASSERT(result);
  DBUG_ASSERT(action);

  /* If an execute count is present, signal or wait_for are needed too. */
  DBUG_ASSERT(!action->execute ||
              action->signal.length() || action->wait_for.length());

  if (action->execute)
  {
    if (action->signal.length())
    {
      wtxt= debug_sync_bmove_len(wtxt, wend, STRING_WITH_LEN("SIGNAL "));
      wtxt= debug_sync_bmove_len(wtxt, wend, action->signal.ptr(),
                                 action->signal.length());
    }
    if (action->wait_for.length())
    {
      if ((wtxt == result) && (wtxt < wend))
        *(wtxt++)= ' ';
      wtxt= debug_sync_bmove_len(wtxt, wend, STRING_WITH_LEN(" WAIT_FOR "));
      wtxt= debug_sync_bmove_len(wtxt, wend, action->wait_for.ptr(),
                                 action->wait_for.length());

      if (action->timeout != opt_debug_sync_timeout)
      {
        wtxt+= my_snprintf(wtxt, wend - wtxt, " TIMEOUT %lu", action->timeout);
      }
    }
    if (action->execute != 1)
    {
      wtxt+= my_snprintf(wtxt, wend - wtxt, " EXECUTE %lu", action->execute);
    }
  }
  if (action->hit_limit)
  {
    wtxt+= my_snprintf(wtxt, wend - wtxt, "%sHIT_LIMIT %lu",
                       (wtxt == result) ? "" : " ", action->hit_limit);
  }

  /*
    If (wtxt == wend) string may not be terminated.
    There is one byte left for an emergency termination.
  */
  *wtxt= '\0';
}


/**
  Print actions.

  @param[in]    thd             thread handle
*/

static void debug_sync_print_actions(THD *thd)
{
  st_debug_sync_control *ds_control= thd->debug_sync_control;
  uint                  idx;
  DBUG_ENTER("debug_sync_print_actions");
  DBUG_ASSERT(thd);

  if (!ds_control)
    DBUG_VOID_RETURN;

  for (idx= 0; idx < ds_control->ds_active; idx++)
  {
    const char *dsp_name= ds_control->ds_action[idx].sync_point.c_ptr();
    char action_string[256];

    debug_sync_action_string(action_string, sizeof(action_string),
                             ds_control->ds_action + idx);
    DBUG_PRINT("debug_sync_list", ("%s %s", dsp_name, action_string));
  }

  DBUG_VOID_RETURN;
}
#endif /* defined(DBUG_TRACE) */


/**
  Compare two actions by sync point name length, string.

  @param[in]    arg1            reference to action1
  @param[in]    arg2            reference to action2

  @return       difference
    @retval     == 0            length1/string1 is same as length2/string2
    @retval     < 0             length1/string1 is smaller
    @retval     > 0             length1/string1 is bigger
*/

static int debug_sync_qsort_cmp(const void* arg1, const void* arg2)
{
  st_debug_sync_action *action1= (st_debug_sync_action*) arg1;
  st_debug_sync_action *action2= (st_debug_sync_action*) arg2;
  int diff;
  DBUG_ASSERT(action1);
  DBUG_ASSERT(action2);

  if (!(diff= action1->sync_point.length() - action2->sync_point.length()))
    diff= memcmp(action1->sync_point.ptr(), action2->sync_point.ptr(),
                 action1->sync_point.length());

  return diff;
}


/**
  Find a debug sync action.

  @param[in]    actionarr       array of debug sync actions
  @param[in]    quantity        number of actions in array
  @param[in]    dsp_name        name of debug sync point to find
  @param[in]    name_len        length of name of debug sync point

  @return       action
    @retval     != NULL         found sync point in array
    @retval     NULL            not found

  @description
    Binary search. Array needs to be sorted by length, sync point name.
*/

static st_debug_sync_action *debug_sync_find(st_debug_sync_action *actionarr,
                                             int quantity,
                                             const char *dsp_name,
                                             size_t name_len)
{
  st_debug_sync_action  *action;
  int                   low ;
  int                   high ;
  int                   mid ;
  ssize_t               diff ;
  DBUG_ASSERT(actionarr);
  DBUG_ASSERT(dsp_name);
  DBUG_ASSERT(name_len);

  low= 0;
  high= quantity;

  while (low < high)
  {
    mid= (low + high) / 2;
    action= actionarr + mid;
    if (!(diff= name_len - action->sync_point.length()) &&
        !(diff= memcmp(dsp_name, action->sync_point.ptr(), name_len)))
      return action;
    if (diff > 0)
      low= mid + 1;
    else
      high= mid - 1;
  }

  if (low < quantity)
  {
    action= actionarr + low;
    if ((name_len == action->sync_point.length()) &&
        !memcmp(dsp_name, action->sync_point.ptr(), name_len))
      return action;
  }

  return NULL;
}


/**
  Reset the debug sync facility.

  @param[in]    thd             thread handle

  @description
    Remove all actions of this thread.
    Clear the global signal.
*/

static void debug_sync_reset(THD *thd)
{
  st_debug_sync_control *ds_control= thd->debug_sync_control;
  DBUG_ENTER("debug_sync_reset");
  DBUG_ASSERT(thd);
  DBUG_ASSERT(ds_control);

  /* Remove all actions of this thread. */
  ds_control->ds_active= 0;

  /* Clear the global signal. */
  mysql_mutex_lock(&debug_sync_global->ds_mutex);
  debug_sync_global->clear_set();
  mysql_mutex_unlock(&debug_sync_global->ds_mutex);

  DBUG_VOID_RETURN;
}


/**
  Remove a debug sync action.

  @param[in]    ds_control      control object
  @param[in]    action          action to be removed

  @description
    Removing an action mainly means to decrement the ds_active counter.
    But if the action is between other active action in the array, then
    the array needs to be shrunk. The active actions above the one to
    be removed have to be moved down by one slot.
*/

static void debug_sync_remove_action(st_debug_sync_control *ds_control,
                                     st_debug_sync_action *action)
{
  uint dsp_idx= (uint)(action - ds_control->ds_action);
  DBUG_ENTER("debug_sync_remove_action");
  DBUG_ASSERT(ds_control);
  DBUG_ASSERT(ds_control == current_thd->debug_sync_control);
  DBUG_ASSERT(action);
  DBUG_ASSERT(dsp_idx < ds_control->ds_active);

  /* Decrement the number of currently active actions. */
  ds_control->ds_active--;

  /*
    If this was not the last active action in the array, we need to
    shift remaining active actions down to keep the array gap-free.
    Otherwise binary search might fail or take longer than necessary at
    least. Also new actions are always put to the end of the array.
  */
  if (ds_control->ds_active > dsp_idx)
  {
    /*
      Do not make save_action an object of class st_debug_sync_action.
      Its destructor would tamper with the String pointers.
    */
    uchar save_action[sizeof(st_debug_sync_action)];

    /*
      Copy the to-be-removed action object to temporary storage before
      the shift copies the string pointers over. Do not use assignment
      because it would use assignment operator methods for the Strings.
      This would copy the strings. The shift below overwrite the string
      pointers without freeing them first. By using memmove() we save
      the pointers, which are overwritten by the shift.
    */
    memmove(save_action, action, sizeof(st_debug_sync_action));

    /* Move actions down. */
    memmove((void*)(ds_control->ds_action + dsp_idx),
            ds_control->ds_action + dsp_idx + 1,
            (ds_control->ds_active - dsp_idx) *
            sizeof(st_debug_sync_action));

    /*
      Copy back the saved action object to the now free array slot. This
      replaces the double references of String pointers that have been
      produced by the shift. Again do not use an assignment operator to
      avoid string allocation/copy.
    */
    memmove((void*)(ds_control->ds_action + ds_control->ds_active),
            save_action, sizeof(st_debug_sync_action));
  }

  DBUG_VOID_RETURN;
}


/**
  Get a debug sync action.

  @param[in]    thd             thread handle
  @param[in]    dsp_name        debug sync point name
  @param[in]    name_len        length of sync point name

  @return       action
    @retval     != NULL         ok
    @retval     NULL            error

  @description
    Find the debug sync action for a debug sync point or make a new one.
*/

static st_debug_sync_action *debug_sync_get_action(THD *thd,
                                                   const char *dsp_name,
                                                   uint name_len)
{
  st_debug_sync_control *ds_control= thd->debug_sync_control;
  st_debug_sync_action  *action;
  DBUG_ENTER("debug_sync_get_action");
  DBUG_ASSERT(thd);
  DBUG_ASSERT(dsp_name);
  DBUG_ASSERT(name_len);
  DBUG_ASSERT(ds_control);
  DBUG_PRINT("debug_sync", ("sync_point: '%.*s'", (int) name_len, dsp_name));
  DBUG_PRINT("debug_sync", ("active: %u  allocated: %u",
                            ds_control->ds_active, ds_control->ds_allocated));

  /* There cannot be more active actions than allocated. */
  DBUG_ASSERT(ds_control->ds_active <= ds_control->ds_allocated);
  /* If there are active actions, the action array must be present. */
  DBUG_ASSERT(!ds_control->ds_active || ds_control->ds_action);

  /* Try to reuse existing action if there is one for this sync point. */
  if (ds_control->ds_active &&
      (action= debug_sync_find(ds_control->ds_action, ds_control->ds_active,
                               dsp_name, name_len)))
  {
    /* Reuse an already active sync point action. */
    DBUG_ASSERT((uint)(action - ds_control->ds_action) < ds_control->ds_active);
    DBUG_PRINT("debug_sync", ("reuse action idx: %ld",
                              (long) (action - ds_control->ds_action)));
  }
  else
  {
    /* Create a new action. */
    int dsp_idx= ds_control->ds_active++;
    set_if_bigger(ds_control->dsp_max_active, ds_control->ds_active);
    if (ds_control->ds_active > ds_control->ds_allocated)
    {
      uint new_alloc= ds_control->ds_active + 3;
      void *new_action= my_realloc(PSI_NOT_INSTRUMENTED, ds_control->ds_action,
                                   new_alloc * sizeof(st_debug_sync_action),
                                   MYF(MY_WME | MY_ALLOW_ZERO_PTR));
      if (!new_action)
      {
        /* Error is reported by my_malloc(). */
        goto err; /* purecov: tested */
      }
      ds_control->ds_action= (st_debug_sync_action*) new_action;
      ds_control->ds_allocated= new_alloc;
      /* Clear memory as we do not run string constructors here. */
      bzero((uchar*) (ds_control->ds_action + dsp_idx),
            (new_alloc - dsp_idx) * sizeof(st_debug_sync_action));
    }
    DBUG_PRINT("debug_sync", ("added action idx: %u", dsp_idx));
    action= ds_control->ds_action + dsp_idx;
    if (action->sync_point.copy(dsp_name, name_len, system_charset_info))
    {
      /* Error is reported by my_malloc(). */
      goto err; /* purecov: tested */
    }
    action->need_sort= TRUE;
  }
  DBUG_ASSERT(action >= ds_control->ds_action);
  DBUG_ASSERT(action < ds_control->ds_action + ds_control->ds_active);
  DBUG_PRINT("debug_sync", ("action: %p  array: %p  count: %u",
                            action, ds_control->ds_action,
                            ds_control->ds_active));

  DBUG_RETURN(action);

  /* purecov: begin tested */
 err:
  DBUG_RETURN(NULL);
  /* purecov: end */
}


class Debug_token: public Lex_ident_ci
{
public:
  using Lex_ident_ci::Lex_ident_ci;
};


/**
  Set a debug sync action.

  @param[in]    thd             thread handle
  @param[in]    action          synchronization action

  @return       status
    @retval     FALSE           ok
    @retval     TRUE            error

  @description
    This is called from the debug sync parser. It arms the action for
    the requested sync point. If the action parsed into an empty action,
    it is removed instead.

    Setting an action for a sync point means to make the sync point
    active. When it is hit it will execute this action.

    Before parsing, we "get" an action object. This is placed at the
    end of the thread's action array unless the requested sync point
    has an action already.

    Then the parser fills the action object from the request string.

    Finally the action is "set" for the sync point. If it was parsed
    to be empty, it is removed from the array. If it did belong to a
    sync point before, the sync point becomes inactive. If the action
    became non-empty and it did not belong to a sync point before (it
    was added at the end of the action array), the action array needs
    to be sorted by sync point.

    If the sync point name is "now", it is executed immediately.
*/

static bool debug_sync_set_action(THD *thd, st_debug_sync_action *action)
{
  st_debug_sync_control *ds_control= thd->debug_sync_control;
  bool is_dsp_now= FALSE;
  DBUG_ENTER("debug_sync_set_action");
  DBUG_ASSERT(thd);
  DBUG_ASSERT(action);
  DBUG_ASSERT(ds_control);

  action->activation_count= MY_MAX(action->hit_limit, action->execute);
  if (!action->activation_count)
  {
    debug_sync_remove_action(ds_control, action);
    DBUG_PRINT("debug_sync", ("action cleared"));
  }
  else
  {
    const Debug_token dsp_name(action->sync_point.to_lex_cstring());
#ifdef DBUG_TRACE
    DBUG_EXECUTE("debug_sync", {
        /* Functions as DBUG_PRINT args can change keyword and line nr. */
        const char *sig_emit= action->signal.c_ptr();
        const char *sig_wait= action->wait_for.c_ptr();
        DBUG_PRINT("debug_sync",
                   ("sync_point: '%.*s'  activation_count: %lu  hit_limit: %lu  "
                    "execute: %lu  timeout: %lu  signal: '%s'  wait_for: '%s'",
                    (int) dsp_name.length, dsp_name.str,
                    action->activation_count,
                    action->hit_limit, action->execute, action->timeout,
                    sig_emit, sig_wait));});
#endif

    /* Check this before sorting the array. action may move. */
    is_dsp_now= dsp_name.streq("now"_LEX_CSTRING);

    if (action->need_sort)
    {
      action->need_sort= FALSE;
      /* Sort actions by (name_len, name). */
      my_qsort(ds_control->ds_action, ds_control->ds_active,
               sizeof(st_debug_sync_action), debug_sync_qsort_cmp);
    }
  }
#ifdef DBUG_TRACE
  DBUG_EXECUTE("debug_sync_list", debug_sync_print_actions(thd););
#endif

  /* Execute the special sync point 'now' if activated above. */
  if (is_dsp_now)
  {
    DEBUG_SYNC(thd, "now");
    /*
      If HIT_LIMIT for sync point "now" was 1, the execution of the sync
      point decremented it to 0. In this case the following happened:

      - an error message was reported with my_error() and
      - the statement was killed with thd->killed= THD::KILL_QUERY.

      If a statement reports an error, it must not call send_ok().
      The calling functions will not call send_ok(), if we return TRUE
      from this function.

      thd->killed is also set if the wait is interrupted from a
      KILL or KILL QUERY statement. In this case, no error is reported
      and shall not be reported as a result of SET DEBUG_SYNC.
      Hence, we check for the first condition above.
    */
    if (unlikely(thd->is_error()))
      DBUG_RETURN(TRUE);
  }

  DBUG_RETURN(FALSE);
}


/**
  Extract a token from a string.

  @param[out]     token_p         returns start of token
  @param[out]     token_length_p  returns length of token
  @param[in,out]  ptr             current string pointer, adds '\0' terminators

  @return       string pointer or NULL
    @retval     != NULL         ptr behind token terminator or at string end
    @retval     NULL            no token found in remainder of string

  @note
    This function assumes that the string is in system_charset_info,
    that this charset is single byte for ASCII NUL ('\0'), that no
    character except of ASCII NUL ('\0') contains a byte with value 0,
    and that ASCII NUL ('\0') is used as the string terminator.

    This function needs to return tokens that are terminated with ASCII
    NUL ('\0'). The tokens are used in strtoul().

    To return the last token without copying it, we require the input
    string to be nul terminated.

  @description
    This function skips space characters at string begin.

    It returns a pointer to the first non-space character in *token_p.

    If no non-space character is found before the string terminator
    ASCII NUL ('\0'), the function returns NULL. *token_p and
    *token_length_p remain unchanged in this case (they are not set).

    The function takes a space character or an ASCII NUL ('\0') as a
    terminator of the token. The space character could be multi-byte.

    It returns the length of the token in bytes, excluding the
    terminator, in *token_length_p.

    If the terminator of the token is ASCII NUL ('\0'), it returns a
    pointer to the terminator (string end).

    If the terminator is a space character, it replaces the the first
    byte of the terminator character by ASCII NUL ('\0'), skips the (now
    corrupted) terminator character, and skips all following space
    characters. It returns a pointer to the next non-space character or
    to the string terminator ASCII NUL ('\0').
*/

static char *debug_sync_token(Debug_token *to,
                              char *ptr, char *ptrend)
{
  DBUG_ASSERT(to);
  DBUG_ASSERT(ptr);

  /* Skip leading space */
  ptr+= system_charset_info->scan(ptr, ptrend, MY_SEQ_SPACES);
  if (!*ptr)
  {
    // Keep "to" intact.
    return NULL;
  }

  /* Get token start. */
  to->str= ptr;

  /* Find token end. */
  ptr+= system_charset_info->scan(ptr, ptrend, MY_SEQ_NONSPACES);

  /* Get token length. */
  to->length= (uint)(ptr - to->str);

  /* If necessary, terminate token. */
  if (*ptr)
  {
    DBUG_ASSERT(ptr < ptrend);
    /* Get terminator character length. */
    uint mbspacelen= system_charset_info->charlen_fix(ptr, ptrend);

    /* Terminate token. */
    *ptr= '\0';

    /* Skip the terminator. */
    ptr+= mbspacelen;

    /* Skip trailing space */
    ptr+= system_charset_info->scan(ptr, ptrend, MY_SEQ_SPACES);
  }

  return ptr;
}


/**
  Extract a number from a string.

  @param[out]   number_p        returns number
  @param[in]    actstrptr       current pointer in action string

  @return       string pointer or NULL
    @retval     != NULL         ptr behind token terminator or at string end
    @retval     NULL            no token found or token is not valid number

  @note
    The same assumptions about charset apply as for debug_sync_token().

  @description
    This function fetches a token from the string and converts it
    into a number.

    If there is no token left in the string, or the token is not a valid
    decimal number, NULL is returned. The result in *number_p is
    undefined in this case.
*/

static char *debug_sync_number(ulong *number_p, char *actstrptr,
                                                char *actstrend)
{
  char                  *ptr;
  char                  *ept;
  Debug_token           token;
  DBUG_ASSERT(number_p);
  DBUG_ASSERT(actstrptr);

  /* Get token from string. */
  if (!(ptr= debug_sync_token(&token, actstrptr, actstrend)))
    goto end;

  *number_p= strtoul(token.str, &ept, 10);
  if (*ept)
    ptr= NULL;

 end:
  return ptr;
}


/**
  Evaluate a debug sync action string.

  @param[in]        thd             thread handle
  @param[in,out]    action_str      action string to receive '\0' terminators

  @return           status
    @retval         FALSE           ok
    @retval         TRUE            error

  @description
    This is called when the DEBUG_SYNC system variable is set.
    Parse action string, build a debug sync action, activate it.

    Before parsing, we "get" an action object. This is placed at the
    end of the thread's action array unless the requested sync point
    has an action already.

    Then the parser fills the action object from the request string.

    Finally the action is "set" for the sync point. This means that the
    sync point becomes active or inactive, depending on the action
    values.

  @note
    The input string needs to be ASCII NUL ('\0') terminated. We split
    nul-terminated tokens in it without copy.

  @note
    The current implementation does not support two 'now SIGNAL xxx' commands
    in a row for multiple threads as the first one can get lost while
    the waiting threads are sleeping on mysql_cond_timedwait().
    One reason for this is that the signal name is stored in a global variable
    that is overwritten.  A better way would be to store all signals in
    an array together with a 'time' when the signal was sent. This array
    should be checked on broadcast.

  @see the function comment of debug_sync_token() for more constraints
    for the string.
*/

static bool debug_sync_eval_action(THD *thd, char *action_str, char *action_end)
{
  st_debug_sync_action  *action= NULL;
  const char            *errmsg;
  char                  *ptr;
  Debug_token           token;
  DBUG_ENTER("debug_sync_eval_action");
  DBUG_ASSERT(thd);
  DBUG_ASSERT(action_str);
  DBUG_PRINT("debug_sync", ("action_str: '%s'", action_str));

  /*
    Get debug sync point name. Or a special command.
  */
  if (!(ptr= debug_sync_token(&token, action_str, action_end)))
  {
    errmsg= "Missing synchronization point name";
    goto err;
  }

  /*
    If there is a second token, the first one is the sync point name.
  */
  if (*ptr)
  {
    /* Get an action object to collect the requested action parameters. */
    action= debug_sync_get_action(thd, token.str, (uint) token.length);
    if (!action)
    {
      /* Error message is sent. */
      DBUG_RETURN(TRUE); /* purecov: tested */
    }
  }

  /*
    Get kind of action to be taken at sync point.
  */
  if (!(ptr= debug_sync_token(&token, ptr, action_end)))
  {
    /* No action present. Try special commands. Token unchanged. */

    /*
      Try RESET.
    */
    if (token.streq("RESET"_LEX_CSTRING))
    {
      /* It is RESET. Reset all actions and global signal. */
      debug_sync_reset(thd);
      goto end;
    }

    /* Token unchanged. It still contains sync point name. */
    errmsg= "Missing action after synchronization point name '%.*s'";
    goto err;
  }

  /*
    Check for pseudo actions first. Start with actions that work on
    an existing action.
  */
  DBUG_ASSERT(action);

  /*
    Try TEST.
  */
  if (token.streq("TEST"_LEX_CSTRING))
  {
    /* It is TEST. Nothing must follow it. */
    if (*ptr)
    {
      errmsg= "Nothing must follow action TEST";
      goto err;
    }

    /* Execute sync point. */
    debug_sync(thd, action->sync_point.ptr(), action->sync_point.length());
    /* Fix statistics. This was not a real hit of the sync point. */
    thd->debug_sync_control->dsp_hits--;
    goto end;
  }

  /*
    Now check for actions that define a new action.
    Initialize action. Do not use bzero(). Strings may have malloced.
  */
  action->activation_count= 0;
  action->hit_limit= 0;
  action->execute= 0;
  action->timeout= 0;
  action->signal.length(0);
  action->wait_for.length(0);

  /*
    Try CLEAR.
  */
  if (token.streq("CLEAR"_LEX_CSTRING))
  {
    /* It is CLEAR. Nothing must follow it. */
    if (*ptr)
    {
      errmsg= "Nothing must follow action CLEAR";
      goto err;
    }

    /* Set (clear/remove) action. */
    goto set_action;
  }

  /*
    Now check for real sync point actions.
  */

  /*
    Try SIGNAL.
  */
  if (token.streq("SIGNAL"_LEX_CSTRING))
  {
    /* It is SIGNAL. Signal name must follow. */
    if (!(ptr= debug_sync_token(&token, ptr, action_end)))
    {
      errmsg= "Missing signal name after action SIGNAL";
      goto err;
    }
    if (action->signal.copy(token.str, token.length, system_charset_info))
    {
      /* Error is reported by my_malloc(). */
      /* purecov: begin tested */
      errmsg= NULL;
      goto err;
      /* purecov: end */
    }

    /* Set default for EXECUTE option. */
    action->execute= 1;

    /* Get next token. If none follows, set action. */
    if (!(ptr= debug_sync_token(&token, ptr, action_end)))
      goto set_action;
  }

  /*
    Try WAIT_FOR.
  */
  if (token.streq("WAIT_FOR"_LEX_CSTRING))
  {
    /* It is WAIT_FOR. Wait_for signal name must follow. */
    if (!(ptr= debug_sync_token(&token, ptr, action_end)))
    {
      errmsg= "Missing signal name after action WAIT_FOR";
      goto err;
    }
    if (action->wait_for.copy(token.str, token.length, system_charset_info))
    {
      /* Error is reported by my_malloc(). */
      /* purecov: begin tested */
      errmsg= NULL;
      goto err;
      /* purecov: end */
    }

    /* Set default for EXECUTE and TIMEOUT options. */
    action->execute= 1;
    action->timeout= opt_debug_sync_timeout;
    action->clear_event= true;

    /* Get next token. If none follows, set action. */
    if (!(ptr= debug_sync_token(&token, ptr, action_end)))
      goto set_action;

    /*
      Try TIMEOUT.
    */
    if (token.streq("TIMEOUT"_LEX_CSTRING))
    {
      /* It is TIMEOUT. Number must follow. */
      if (!(ptr= debug_sync_number(&action->timeout, ptr, action_end)))
      {
        errmsg= "Missing valid number after TIMEOUT";
        goto err;
      }

      /* Get next token. If none follows, set action. */
      if (!(ptr= debug_sync_token(&token, ptr, action_end)))
        goto set_action;
    }
  }

  /*
    Try EXECUTE.
  */
  if (token.streq("EXECUTE"_LEX_CSTRING))
  {
    /*
      EXECUTE requires either SIGNAL and/or WAIT_FOR to be present.
      In this case action->execute has been preset to 1.
    */
    if (!action->execute)
    {
      errmsg= "Missing action before EXECUTE";
      goto err;
    }

    /* Number must follow. */
    if (!(ptr= debug_sync_number(&action->execute, ptr, action_end)))
    {
      errmsg= "Missing valid number after EXECUTE";
      goto err;
    }

    /* Get next token. If none follows, set action. */
    if (!(ptr= debug_sync_token(&token, ptr, action_end)))
      goto set_action;
  }

  /*
    Try NO_CLEAR_EVENT.
  */
  if (token.streq("NO_CLEAR_EVENT"_LEX_CSTRING))
  {
    action->clear_event= false;
    /* Get next token. If none follows, set action. */
    if (!(ptr = debug_sync_token(&token, ptr, action_end))) goto set_action;
  }

  /*
    Try HIT_LIMIT.
  */
  if (token.streq("HIT_LIMIT"_LEX_CSTRING))
  {
    /* Number must follow. */
    if (!(ptr= debug_sync_number(&action->hit_limit, ptr, action_end)))
    {
      errmsg= "Missing valid number after HIT_LIMIT";
      goto err;
    }

    /* Get next token. If none follows, set action. */
    if (!(ptr= debug_sync_token(&token, ptr, action_end)))
      goto set_action;
  }

  errmsg= "Illegal or out of order stuff: '%.*s'";

 err:
  if (errmsg)
  {
    /*
      NOTE: errmsg must either have %.*s or none % at all.
      It can be NULL if an error message is already reported
      (e.g. by my_malloc()).
    */
    set_if_smaller(token.length, 64); /* Limit error message length. */
    my_printf_error(ER_PARSE_ERROR, errmsg, MYF(0), token.length, token.str);
  }
  if (action)
    debug_sync_remove_action(thd->debug_sync_control, action);
  DBUG_RETURN(TRUE);

 set_action:
  DBUG_RETURN(debug_sync_set_action(thd, action));

 end:
  DBUG_RETURN(FALSE);
}

/**
  Set the system variable 'debug_sync'.

  @param[in]    thd             thread handle
  @param[in]    var             set variable request

  @return       status
    @retval     FALSE           ok, variable is set
    @retval     TRUE            error, variable could not be set

  @note
    "Setting" of the system variable 'debug_sync' does not mean to
    assign a value to it as usual. Instead a debug sync action is parsed
    from the input string and stored apart from the variable value.

  @note
    For efficiency reasons, the action string parser places '\0'
    terminators in the string. So we need to take a copy here.
*/

bool debug_sync_update(THD *thd, char *val_str, size_t len)
{
  DBUG_ENTER("debug_sync_update");
  DBUG_PRINT("debug_sync", ("set action: '%s'", val_str));

  /*
    debug_sync_eval_action() places '\0' in the string, which itself
    must be '\0' terminated.
  */
  DBUG_ASSERT(val_str[len] == '\0');
  DBUG_RETURN(opt_debug_sync_timeout ?
              debug_sync_eval_action(thd, val_str, val_str + len) :
              FALSE);
}


/**
  Retrieve the value of the system variable 'debug_sync'.

  @param[in]    thd             thread handle

  @return       string
    @retval     != NULL         ok, string pointer
    @retval     NULL            memory allocation error

  @note
    The value of the system variable 'debug_sync' reflects if
    the facility is enabled ("ON") or disabled (default, "OFF").

    When "ON", the current signal is added.
*/

uchar *debug_sync_value_ptr(THD *thd)
{
  char *value;
  DBUG_ENTER("debug_sync_value_ptr");

  if (opt_debug_sync_timeout)
  {
    static const char on[]= "ON - current signals: '";

    // Ensure exclusive access to debug_sync_global.ds_signal
    mysql_mutex_lock(&debug_sync_global->ds_mutex);

    size_t lgt= sizeof(on) + 1; /* +1 as we'll have to append ' at the end. */

    for (size_t i= 0; i < debug_sync_global->ds_signal_set.size(); i++)
    {
      /* Assume each signal is separated by a comma, hence +1. */
      lgt+= debug_sync_global->ds_signal_set.at(i)->length + 1;
    }

    char *vend;
    char *vptr;

    if ((value= (char*) alloc_root(thd->mem_root, lgt)))
    {
      vend= value + lgt - 1; /* reserve space for '\0'. */
      vptr= debug_sync_bmove_len(value, vend, STRING_WITH_LEN(on));
      for (size_t i= 0; i < debug_sync_global->ds_signal_set.size(); i++)
      {
        const LEX_CSTRING *s= debug_sync_global->ds_signal_set.at(i);
        vptr= debug_sync_bmove_len(vptr, vend, s->str, s->length);
        if (i != debug_sync_global->ds_signal_set.size() - 1)
          *(vptr++)= ',';
      }
      DBUG_ASSERT(vptr < vend);
      *(vptr++)= '\'';
      *vptr= '\0'; /* We have one byte reserved for the worst case. */
    }
    mysql_mutex_unlock(&debug_sync_global->ds_mutex);
  }
  else
  {
    /* purecov: begin tested */
    value= const_cast<char*>("OFF");
    /* purecov: end */
  }

  DBUG_RETURN((uchar*) value);
}





/**
  Execute requested action at a synchronization point.

  @param[in]    thd                 thread handle
  @param[in]    action              action to be executed

  @note
    This is to be called only if activation count > 0.
*/

static void debug_sync_execute(THD *thd, st_debug_sync_action *action)
{
#ifdef DBUG_TRACE
  const char *dsp_name= action->sync_point.c_ptr();
  const char *sig_emit= action->signal.c_ptr();
  const char *sig_wait= action->wait_for.c_ptr();
#endif
  DBUG_ENTER("debug_sync_execute");
  DBUG_ASSERT(thd);
  DBUG_ASSERT(action);
  DBUG_PRINT("debug_sync",
             ("sync_point: '%s'  activation_count: %lu  hit_limit: %lu  "
              "execute: %lu  timeout: %lu  signal: '%s'  wait_for: '%s'",
              dsp_name, action->activation_count, action->hit_limit,
              action->execute, action->timeout, sig_emit, sig_wait));

  DBUG_ASSERT(action->activation_count);
  action->activation_count--;

  if (action->execute)
  {
    const char  *UNINIT_VAR(old_proc_info);

    action->execute--;

    /*
      If we will be going to wait, set proc_info for the PROCESSLIST table.
      Do this before emitting the signal, so other threads can see it
      if they awake before we enter_cond() below.
    */
    if (action->wait_for.length())
    {
      st_debug_sync_control *ds_control= thd->debug_sync_control;
      strxnmov(ds_control->ds_proc_info, sizeof(ds_control->ds_proc_info)-1,
               "debug sync point: ", action->sync_point.c_ptr(), NullS);
      old_proc_info= thd->proc_info;
      debug_sync_thd_proc_info(thd, ds_control->ds_proc_info);
    }

    /*
      Take mutex to ensure that only one thread access
      debug_sync_global.ds_signal at a time.  Need to take mutex for
      read access too, to create a memory barrier in order to avoid that
      threads just reads an old cached version of the signal.
    */

    mysql_mutex_lock(&debug_sync_global->ds_mutex);

    if (action->signal.length())
    {
      int offset= 0, pos;
      bool error= false;

      /* This loop covers all signals in the list except for the last one.
         Split the signal string by commas and set a signal in the global
         variable for each one. */
      while (!error && (pos= action->signal.strstr(",", 1, offset)) > 0)
      {
        error= debug_sync_global->set_signal(action->signal.ptr() + offset,
                                             pos - offset);
        offset= pos + 1;
      }

      if (error ||
          /* The last signal in the list. */
          debug_sync_global->set_signal(action->signal.ptr() + offset,
                                        action->signal.length() - offset))
      {
        /*
          Error is reported by my_malloc().
          We must disable the facility. We have no way to return an error.
        */
        debug_sync_emergency_disable(); /* purecov: tested */
      }
      /* Wake threads waiting in a sync point. */
      mysql_cond_broadcast(&debug_sync_global->ds_cond);
      DBUG_PRINT("debug_sync_exec", ("signal '%s'  at: '%s'",
                                     sig_emit, dsp_name));
    } /* end if (action->signal.length()) */

    if (action->wait_for.length())
    {
      mysql_mutex_t *old_mutex= NULL;
      mysql_cond_t  *old_cond= NULL;
      bool           restore_current_mutex;
      int             error= 0;
      struct timespec abstime;

      /*
        We don't use enter_cond()/exit_cond(). They do not save old
        mutex and cond. This would prohibit the use of DEBUG_SYNC
        between other places of enter_cond() and exit_cond().

        We need to check for existence of thd->mysys_var to also make
        it possible to use DEBUG_SYNC framework in scheduler when this
        variable has been set to NULL.
      */
      if (thd->mysys_var)
      {
        old_mutex= thd->mysys_var->current_mutex;
        old_cond= thd->mysys_var->current_cond;
        restore_current_mutex = true;
        thd->mysys_var->current_mutex= &debug_sync_global->ds_mutex;
        thd->mysys_var->current_cond= &debug_sync_global->ds_cond;
      }
      else
        restore_current_mutex = false;

      set_timespec(abstime, action->timeout);
      DBUG_EXECUTE("debug_sync_exec", {
          const char *signal_set= get_signal_set_as_string();
          if (!signal_set)
          {
            DBUG_PRINT("debug_sync_exec",
                       ("Out of memory when fetching signal set"));
          }
          else
          {
            /* Functions as DBUG_PRINT args can change keyword and line nr. */
            DBUG_PRINT("debug_sync_exec",
                       ("wait for '%s'  at: '%s', curr: '%s'",
                        sig_wait, dsp_name, signal_set));
            my_free((void *)signal_set);
          }});


      /*
        Wait until the signal set contains the wait_for string.
        Interrupt when thread or query is killed or facility is disabled.
        The facility can become disabled when some thread cannot get
        the required dynamic memory allocated.
      */
      while (!debug_sync_global->is_signalled(action->wait_for.ptr(),
                                              action->wait_for.length()) &&
             !(thd->killed & KILL_HARD_BIT) &&
             opt_debug_sync_timeout)
      {
        error= mysql_cond_timedwait(&debug_sync_global->ds_cond,
                                    &debug_sync_global->ds_mutex,
                                    &abstime);
        // TODO turn this into a for loop printing.
        DBUG_EXECUTE("debug_sync", {
            /* Functions as DBUG_PRINT args can change keyword and line nr. */
            DBUG_PRINT("debug_sync",
                       ("awoke from %s error: %d",
                        sig_wait, error));});
        if (unlikely(error == ETIMEDOUT || error == ETIME))
        {
          // We should not make the statement fail, even if in strict mode.
          Abort_on_warning_instant_set aws(thd, false);
          push_warning(thd, Sql_condition::WARN_LEVEL_WARN,
                       ER_DEBUG_SYNC_TIMEOUT,
                       ER_THD(thd, ER_DEBUG_SYNC_TIMEOUT));
          DBUG_EXECUTE_IF("debug_sync_abort_on_timeout", DBUG_ASSERT(0););
          break;
        }
        error= 0;
      }

      if (action->clear_event)
        debug_sync_global->clear_signal(action->wait_for);

      DBUG_EXECUTE("debug_sync_exec",
                   if (thd->killed)
                     DBUG_PRINT("debug_sync_exec",
                                ("killed %d from '%s'  at: '%s'",
                                 thd->killed, sig_wait, dsp_name));
                   else
                     DBUG_PRINT("debug_sync_exec",
                                ("%s from '%s'  at: '%s'",
                                 error ? "timeout" : "resume",
                                 sig_wait, dsp_name)););

      /*
        We don't use enter_cond()/exit_cond(). They do not save old
        mutex and cond. This would prohibit the use of DEBUG_SYNC
        between other places of enter_cond() and exit_cond(). The
        protected mutex must always unlocked _before_ mysys_var->mutex
        is locked. (See comment in THD::exit_cond().)
      */
      mysql_mutex_unlock(&debug_sync_global->ds_mutex);
      if (restore_current_mutex)
      {
        mysql_mutex_lock(&thd->mysys_var->mutex);
        thd->mysys_var->current_mutex= old_mutex;
        thd->mysys_var->current_cond= old_cond;
        debug_sync_thd_proc_info(thd, old_proc_info);
        mysql_mutex_unlock(&thd->mysys_var->mutex);
      }
      else
        debug_sync_thd_proc_info(thd, old_proc_info);
    }
    else
    {
      /* In case we don't wait, we just release the mutex. */
      mysql_mutex_unlock(&debug_sync_global->ds_mutex);
    } /* end if (action->wait_for.length()) */

  } /* end if (action->execute) */

  /* hit_limit is zero for infinite. Don't decrement unconditionally. */
  if (action->hit_limit)
  {
    if (!--action->hit_limit)
    {
      thd->set_killed(KILL_QUERY);
      my_error(ER_DEBUG_SYNC_HIT_LIMIT, MYF(0));
    }
    DBUG_PRINT("debug_sync_exec", ("hit_limit: %lu  at: '%s'",
                                   action->hit_limit, dsp_name));
  }

  DBUG_VOID_RETURN;
}


/**
  Execute requested action at a synchronization point.

  @param[in]     thd                thread handle
  @param[in]     sync_point_name    name of synchronization point
  @param[in]     name_len           length of sync point name
*/

static void debug_sync(THD *thd, const char *sync_point_name, size_t name_len)
{
  if (!thd)
  {
    if (!(thd= current_thd))
      return;
  }

  st_debug_sync_control *ds_control= thd->debug_sync_control;
  st_debug_sync_action  *action;
  DBUG_ENTER("debug_sync");
  DBUG_PRINT("debug_sync_point", ("hit: '%s'", sync_point_name));
  DBUG_ASSERT(sync_point_name);
  DBUG_ASSERT(name_len);
  DBUG_ASSERT(ds_control);

  /* Statistics. */
  ds_control->dsp_hits++;

  if (ds_control->ds_active &&
      (action= debug_sync_find(ds_control->ds_action, ds_control->ds_active,
                               sync_point_name, name_len)) &&
      action->activation_count)
  {
    /* Sync point is active (action exists). */
    debug_sync_execute(thd, action);

    /* Statistics. */
    ds_control->dsp_executed++;

    /* If action became inactive, remove it to shrink the search array. */
    if (!action->activation_count)
      debug_sync_remove_action(ds_control, action);
  }

  DBUG_VOID_RETURN;
}

/**
  Define debug sync action.

  @param[in]        thd             thread handle
  @param[in]        action_str      action string

  @return           status
    @retval         FALSE           ok
    @retval         TRUE            error

  @description
    The function is similar to @c debug_sync_eval_action but is
    to be called immediately from the server code rather than 
    to be triggered by setting a value to DEBUG_SYNC system variable.

  @note
    The input string is copied prior to be fed to
    @c debug_sync_eval_action to let the latter modify it.

    Caution.
    The function allocates in THD::mem_root and therefore
    is not recommended to be deployed inside big loops.    
*/

bool debug_sync_set_action(THD *thd, const char *action_str, size_t len)
{
  bool                  rc;
  char *value;
  DBUG_ENTER("debug_sync_set_action");
  DBUG_ASSERT(thd);
  DBUG_ASSERT(action_str);
  
  value= strmake_root(thd->mem_root, action_str, len);
  rc= debug_sync_eval_action(thd, value, value + len);
  DBUG_RETURN(rc);
}


#else /* defined(ENABLED_DEBUG_SYNC) */
/* prevent linker/lib warning about file without public symbols */
int debug_sync_dummy; 
#endif /* defined(ENABLED_DEBUG_SYNC) */
