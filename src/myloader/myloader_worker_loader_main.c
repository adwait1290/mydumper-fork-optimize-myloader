/*
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

        Authors:    David Ducos, Percona (david dot ducos at percona dot com)
*/
#include <glib.h>
#include <stdlib.h>
#include <unistd.h>

#include "myloader_control_job.h"
#include "myloader_restore_job.h"
#include "myloader_common.h"
#include "myloader_restore.h"
#include "myloader_worker_loader_main.h"
#include "myloader_global.h"
#include "myloader_worker_loader.h"
#include "myloader_worker_index.h"
#include "myloader_worker_schema.h"
#include "myloader_database.h"

gboolean control_job_ended=FALSE;
gboolean all_jobs_are_enqueued=FALSE;
/* data_control_queue is for data loads */
GAsyncQueue *data_control_queue = NULL; //, *data_queue=NULL;
static GThread *_worker_loader_main = NULL;
guint threads_waiting = 0;
static GMutex *threads_waiting_mutex= NULL;

// OPTIMIZATION: Track statistics for performance monitoring
static guint64 jobs_dispatched = 0;
static guint64 dispatch_iterations = 0;
static guint64 queue_hits = 0;
static guint64 queue_misses = 0;

void *worker_loader_main_thread(struct configuration *conf);

// OPTIMIZATION: Ready table queue functions for O(1) dispatch
// Add a table to the ready queue if it meets all criteria:
// - schema_state == CREATED
// - job_count > 0
// - current_threads < max_threads
// - not already in queue
// CALLER MUST HOLD dbt->mutex (table lock)
void enqueue_table_if_ready_locked(struct configuration *conf, struct db_table *dbt) {
  // Safety check: queue may not exist in --no-data mode
  // (loader threads skipped, but schema workers still call this)
  if (conf->ready_table_queue == NULL) {
    return;
  }

  // Check all readiness conditions
  if (dbt->schema_state == CREATED &&
      dbt->job_count > 0 &&
      dbt->current_threads < dbt->max_threads &&
      !dbt->in_ready_queue &&
      !dbt->object_to_export.no_data &&
      !dbt->is_view &&
      !dbt->is_sequence) {
    dbt->in_ready_queue = TRUE;
    g_async_queue_push(conf->ready_table_queue, dbt);
    trace("[READY_QUEUE] Enqueued %s.%s (jobs=%u, threads=%u/%u)",
          dbt->database->target_database, dbt->source_table_name,
          dbt->job_count, dbt->current_threads, dbt->max_threads);

    // FIX: Wake waiting data threads when we have work available
    // Without this, threads could be waiting in threads_waiting while
    // ready_table_queue has work, causing the dispatch loop to stall
    wake_data_threads();
  }
}

// Version that acquires the lock itself
void enqueue_table_if_ready(struct configuration *conf, struct db_table *dbt) {
  table_lock(dbt);
  enqueue_table_if_ready_locked(conf, dbt);
  table_unlock(dbt);
}

void initialize_worker_loader_main (struct configuration *conf){
  data_control_queue = g_async_queue_new();
//  data_job_queue = g_async_queue_new();
//  data_queue = g_async_queue_new();
  threads_waiting_mutex=g_mutex_new();
  _worker_loader_main = m_thread_new("myloader_ctr",(GThreadFunc)worker_loader_main_thread, conf, "Control job thread could not be created");
}

void wait_worker_loader_main()
{
  trace("Waiting control job to finish");
  g_thread_join(_worker_loader_main);
  trace("Control job to finished");
}

void data_control_queue_push(enum data_control_type current_ft){
  // Safety check: queue may not exist in --no-data mode
  // (loader threads skipped, but schema workers still call this on completion)
  if (data_control_queue == NULL) {
    trace("data_control_queue is NULL (--no-data mode), skipping push of %s", data_control_type2str(current_ft));
    return;
  }
  trace("data_control_queue <- %s", data_control_type2str(current_ft));
  g_async_queue_push(data_control_queue, GINT_TO_POINTER(current_ft));
}

gboolean give_me_next_data_job_conf(struct configuration *conf, struct restore_job ** rj){
  struct restore_job *job = NULL;
  struct db_table *dbt = NULL;
  gboolean giveup = TRUE;

  // OPTIMIZATION: Track dispatch iterations for monitoring
  dispatch_iterations++;

  // OPTIMIZATION: Try ready_table_queue first for O(1) dispatch
  // The queue contains tables that were previously determined to be ready
  while ((dbt = g_async_queue_try_pop(conf->ready_table_queue)) != NULL) {
    table_lock(dbt);
    dbt->in_ready_queue = FALSE;  // Mark as removed from queue

    // Re-validate readiness conditions (may have changed since enqueue)
    if (dbt->schema_state != CREATED ||
        dbt->job_count == 0 ||
        dbt->current_threads >= dbt->max_threads ||
        dbt->object_to_export.no_data ||
        dbt->is_view ||
        dbt->is_sequence) {
      // Table no longer ready - skip it
      queue_misses++;

      // Handle special cases
      if (dbt->schema_state == CREATED && dbt->job_count == 0 &&
          dbt->current_threads == 0 && all_jobs_are_enqueued &&
          g_atomic_int_get(&(dbt->remaining_jobs)) == 0) {
        dbt->schema_state = DATA_DONE;
        enqueue_index_for_dbt_if_possible(conf, dbt);
        trace("[READY_QUEUE] %s.%s -> DATA_DONE (no more jobs)",
              dbt->database->target_database, dbt->source_table_name);
      }

      table_unlock(dbt);
      continue;
    }

    // Table is ready - dispatch a job!
    queue_hits++;
    job = dbt->restore_job_list->data;
    dbt->restore_job_list = g_list_delete_link(dbt->restore_job_list, dbt->restore_job_list);
    dbt->job_count--;
    dbt->current_threads++;
    jobs_dispatched++;

    g_debug("[LOADER_MAIN] DISPATCH #%"G_GUINT64_FORMAT" (queue): %s.%s threads=%u/%u jobs_left=%u",
            jobs_dispatched, dbt->database->target_database, dbt->source_table_name,
            dbt->current_threads, dbt->max_threads, dbt->job_count);

    // Re-enqueue if still has more jobs and room for threads
    enqueue_table_if_ready_locked(conf, dbt);

    table_unlock(dbt);
    giveup = FALSE;
    *rj = job;

    // Log queue statistics periodically
    if (dispatch_iterations % 1000 == 0) {
      g_debug("[LOADER_MAIN] Queue stats: iterations=%"G_GUINT64_FORMAT" dispatched=%"G_GUINT64_FORMAT" hits=%"G_GUINT64_FORMAT" misses=%"G_GUINT64_FORMAT,
              dispatch_iterations, jobs_dispatched, queue_hits, queue_misses);
    }
    return giveup;
  }

  // FALLBACK: Queue was empty, scan the table list
  // This handles tables that weren't yet added to the queue
  g_mutex_lock(conf->table_list_mutex);
  GList *iter = conf->loading_table_list;
  guint tables_checked = 0;
  guint tables_not_ready = 0;
  guint tables_at_max_threads = 0;

  while (iter != NULL) {
    dbt = iter->data;
    tables_checked++;

    // Quick pre-check for database state (safe without lock)
    if (dbt->database->schema_state == NOT_FOUND) {
      iter = iter->next;
      continue;
    }

    table_lock(dbt);
    enum schema_status current_state = dbt->schema_state;

    // Skip if table processing is complete
    if (current_state >= DATA_DONE ||
        (current_state == CREATED && (dbt->is_view || dbt->is_sequence))) {
      table_unlock(dbt);
      iter = iter->next;
      continue;
    }

    // Skip if not CREATED
    if (current_state != CREATED) {
      giveup = FALSE;
      tables_not_ready++;
      table_unlock(dbt);
      iter = iter->next;
      continue;
    }

    // At this point, schema_state == CREATED
    if (dbt->job_count > 0) {
      if (dbt->object_to_export.no_data) {
        GList *current = dbt->restore_job_list;
        while (current) {
          g_free(((struct restore_job *)current->data)->data.drj);
          current = current->next;
        }
        dbt->schema_state = ALL_DONE;
        g_atomic_int_inc(&(conf->tables_all_done));
        g_debug("[LOADER_MAIN] %s.%s -> ALL_DONE (no_data flag)",
                dbt->database->target_database, dbt->source_table_name);
      } else if (dbt->current_threads >= dbt->max_threads) {
        giveup = FALSE;
        tables_at_max_threads++;
        // Don't enqueue - will be re-enqueued when a thread finishes
        table_unlock(dbt);
        iter = iter->next;
        continue;
      } else {
        // Dispatch a job
        job = dbt->restore_job_list->data;
        dbt->restore_job_list = g_list_delete_link(dbt->restore_job_list, dbt->restore_job_list);
        dbt->job_count--;
        dbt->current_threads++;
        jobs_dispatched++;

        g_debug("[LOADER_MAIN] DISPATCH #%"G_GUINT64_FORMAT" (scan): %s.%s threads=%u/%u",
                jobs_dispatched, dbt->database->target_database, dbt->source_table_name,
                dbt->current_threads, dbt->max_threads);

        // Enqueue for future O(1) dispatch if still has jobs
        enqueue_table_if_ready_locked(conf, dbt);

        table_unlock(dbt);
        giveup = FALSE;
        break;
      }
    } else {
      // No jobs for this table
      trace("No remaining jobs on %s.%s", dbt->database->target_database, dbt->source_table_name);
      if (all_jobs_are_enqueued && dbt->current_threads == 0 &&
          g_atomic_int_get(&(dbt->remaining_jobs)) == 0) {
        dbt->schema_state = DATA_DONE;
        enqueue_index_for_dbt_if_possible(conf, dbt);
        trace("%s.%s queuing indexes", dbt->database->target_database, dbt->source_table_name);
      } else {
        giveup = FALSE;
      }
    }

    table_unlock(dbt);
    iter = iter->next;
  }

  // Log dispatch statistics periodically
  if (dispatch_iterations % 1000 == 0 && dispatch_iterations > 0) {
    g_debug("[LOADER_MAIN] Dispatch stats: iterations=%"G_GUINT64_FORMAT" jobs=%"G_GUINT64_FORMAT" checked=%u not_ready=%u at_max=%u hits=%"G_GUINT64_FORMAT" misses=%"G_GUINT64_FORMAT,
            dispatch_iterations, jobs_dispatched, tables_checked, tables_not_ready, tables_at_max_threads, queue_hits, queue_misses);
  }

  g_mutex_unlock(conf->table_list_mutex);
  *rj = job;
  return giveup;
}

static
void wake_threads_waiting(){
  // Safety check: mutex may not exist in --no-data mode
  if (threads_waiting_mutex == NULL) {
    return;
  }
  g_mutex_lock(threads_waiting_mutex);
  while(threads_waiting>0){
    trace("Waking up threads");
    data_control_queue_push(REQUEST_DATA_JOB);
    threads_waiting=threads_waiting - 1;
  }
  g_mutex_unlock(threads_waiting_mutex);
}

void wake_data_threads(){
  // Safety check: mutex may not exist in --no-data mode
  if (threads_waiting_mutex == NULL) {
    return;
  }
  g_mutex_lock(threads_waiting_mutex);
  if (threads_waiting>0){
    data_control_queue_push(WAKE_DATA_THREAD);
  }else
    trace("No threads sleeping");
  g_mutex_unlock(threads_waiting_mutex);
}

void *worker_loader_main_thread(struct configuration *conf){
  enum data_control_type ft;
  struct restore_job *rj=NULL;
  guint _num_threads = num_threads;
//  guint threads_waiting = 0; //num_threads;
  gboolean giveup;
  gboolean cont=TRUE;
  set_thread_name("CJT");
  
  trace("Thread worker_loader_main_thread started");
  while(cont){
    ft=(enum data_control_type)GPOINTER_TO_INT(g_async_queue_pop(data_control_queue));
    g_mutex_lock(threads_waiting_mutex);
    trace("data_control_queue -> %s (%u loaders waiting)", data_control_type2str(ft), threads_waiting);
    g_mutex_unlock(threads_waiting_mutex);
    switch (ft){
    case WAKE_DATA_THREAD:
      wake_threads_waiting();
      break;
    case REQUEST_DATA_JOB:
      trace("Thread is asking for job");
      giveup = give_me_next_data_job_conf(conf, &rj);
      if (rj != NULL){
        trace("job available in give_me_next_data_job_conf");
        data_job_push(DATA_JOB, rj);
      }else{
        trace("No job available");
        if (all_jobs_are_enqueued && giveup){
          trace("Giving up...");
          control_job_ended = TRUE;
          data_ended();
          cont=FALSE;
/*          guint i;

          for (i=0;i<num_threads;i++){
            trace("data_job_queue <- %s", ft2str(SHUTDOWN));
            g_async_queue_push(data_job_queue, GINT_TO_POINTER(SHUTDOWN));
          }
          */
        }else{
          trace("Thread will be waiting | all_jobs_are_enqueued: %d | giveup: %d", all_jobs_are_enqueued, giveup);
          g_mutex_lock(threads_waiting_mutex);
          if (threads_waiting<_num_threads)
            threads_waiting++;
          g_mutex_unlock(threads_waiting_mutex);
        }
      }
      break;
    case FILE_TYPE_ENDED:
      // FIX: Force table list refresh to capture ALL tables
      refresh_table_list(conf);
      enqueue_indexes_if_possible(conf);
      all_jobs_are_enqueued = TRUE;
//      data_ended();
      data_control_queue_push(REQUEST_DATA_JOB);
//      wake_threads_waiting();
//      wait_loader_threads_to_finish();
      break;
    case SHUTDOWN:
      cont=FALSE;
      trace("SHUTDOWN");
      break;
    case FILE_TYPE_SCHEMA_ENDED:
      wake_threads_waiting();
//      data_control_queue_push(REQUEST_DATA_JOB);
      break;
//    case SCHEMA_TABLE_JOB:
//    case SCHEMA_CREATE_JOB:
      trace("Thread control_job_thread received:  %d", ft);
      break;
    }
  }
//  data_ended();
  wait_loader_threads_to_finish();
  start_optimize_keys_all_tables();

  trace("Thread worker_loader_main_thread finished");
  return NULL;
}
