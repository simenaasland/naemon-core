/*
 * This file holds all naemon<->libnaemon integration stuff, so that
 * libnaemon itself is usable as a standalone library for addon
 * writers to use as they see fit.
 *
 * This means apis inside libnaemon can be tested without compiling
 * all of Naemon into it, and that they can remain general-purpose
 * code that can be reused for other things later.
 */
#include "workers.h"
#include "config.h"
#include <string.h>
#include "query-handler.h"
#include "utils.h"
#include "logging.h"
#include "globals.h"
#include "defaults.h"
#include "nm_alloc.h"

#include "loadctl.h"

/* perfect hash function for wproc response codes */
#include "wpres-phash.h"

struct wproc_worker;

struct wproc_job {
	unsigned int id;
	unsigned int timeout;
	char *command;
	void (*callback)(struct wproc_result *, void *, int);
	void *data;
	struct wproc_worker *wp;
};

struct wproc_list;

struct wproc_worker {
	char *name; /**< check-source name of this worker */
	int sd;     /**< communication socket */
	pid_t pid;  /**< pid */
	int max_jobs; /**< Max number of jobs the worker can handle */
	int jobs_running; /**< jobs running */
	int jobs_started; /**< jobs started */
	int job_index; /**< round-robin slot allocator (this wraps) */
	iocache *ioc;  /**< iocache for reading from worker */
	fanout_table *jobs; /**< array of jobs */
	struct wproc_list *wp_list;
};

struct wproc_list {
	unsigned int len;
	unsigned int idx;
	struct wproc_worker **wps;
};

static struct wproc_list workers = {0, 0, NULL};

static dkhash_table *specialized_workers;
static struct wproc_list *to_remove = NULL;

unsigned int wproc_num_workers_online = 0, wproc_num_workers_desired = 0;
unsigned int wproc_num_workers_spawned = 0;

#define tv2float(tv) ((float)((tv)->tv_sec) + ((float)(tv)->tv_usec) / 1000000.0)

static void wproc_logdump_buffer(int debuglevel, int verbosity, const char *prefix, char *buf)
{
	char *ptr, *eol;
	unsigned int line = 1;

	if (!buf || !*buf)
		return;
	for (ptr = buf; ptr && *ptr; ptr = eol ? eol + 1 : NULL) {
		if ((eol = strchr(ptr, '\n')))
			* eol = 0;
		log_debug_info(debuglevel, verbosity, "%s line %.02d: %s\n", prefix, line++, ptr);
		if (eol)
			*eol = '\n';
		else
			break;
	}
}

/* reap 'jobs' jobs or 'secs' seconds, whichever comes first */
void wproc_reap(int jobs, int msecs)
{
	time_t start, now;
	start = time(NULL);

	/* one input equals one job (or close enough to it anyway) */
	do {
		int inputs;

		now = time(NULL);
		inputs = iobroker_poll(nagios_iobs, (now - start) * 1000);
		jobs -= inputs;
	} while (jobs > 0 && start + (msecs * 1000) <= now);
}

int wproc_can_spawn(struct load_control *lc)
{
	unsigned int old = 0;
	time_t now;

	/* if no load control is enabled, we can safely run this job */
	if (!(lc->options & LOADCTL_ENABLED))
		return 1;

	now = time(NULL);
	if (lc->last_check + lc->check_interval > now) {
		lc->last_check = now;

		if (getloadavg(lc->load, 3) < 0)
			return lc->jobs_limit > lc->jobs_running;

		if (lc->load[0] > lc->backoff_limit) {
			old = lc->jobs_limit;
			lc->jobs_limit -= lc->backoff_change;
		} else if (lc->load[0] < lc->rampup_limit) {
			old = lc->jobs_limit;
			lc->jobs_limit += lc->rampup_change;
		}

		if (lc->jobs_limit > lc->jobs_max) {
			lc->jobs_limit = lc->jobs_max;
		} else if (lc->jobs_limit < lc->jobs_min) {
			nm_log(NSLOG_RUNTIME_WARNING, "Warning: Tried to set jobs_limit to %u, below jobs_min (%u)\n",
			       lc->jobs_limit, lc->jobs_min);
			lc->jobs_limit = lc->jobs_min;
		}

		if (old && old != lc->jobs_limit) {
			if (lc->jobs_limit < old) {
				nm_log(NSLOG_RUNTIME_WARNING, "Warning: loadctl.jobs_limit changed from %u to %u\n", old, lc->jobs_limit);
			} else {
				nm_log(NSLOG_INFO_MESSAGE, "wproc: loadctl.jobs_limit changed from %u to %u\n", old, lc->jobs_limit);
			}
		}
	}

	return lc->jobs_limit > lc->jobs_running;
}

static int get_job_id(struct wproc_worker *wp)
{
	return wp->job_index++;
}

static struct wproc_job *get_job(struct wproc_worker *wp, int job_id)
{
	return fanout_remove(wp->jobs, job_id);
}


static struct wproc_list *get_wproc_list(const char *cmd)
{
	struct wproc_list *wp_list;
	char *cmd_name = NULL, *slash = NULL, *space;

	if (!specialized_workers)
		return &workers;

	/* first, look for a specialized worker for this command */
	if ((space = strchr(cmd, ' ')) != NULL) {
		int namelen = (unsigned long)space - (unsigned long)cmd;
		cmd_name = nm_calloc(1, namelen + 1);
		memcpy(cmd_name, cmd, namelen);
		slash = strrchr(cmd_name, '/');
	}

	wp_list = dkhash_get(specialized_workers, cmd_name ? cmd_name : cmd, NULL);
	if (!wp_list && slash) {
		wp_list = dkhash_get(specialized_workers, ++slash, NULL);
	}
	if (wp_list != NULL) {
		log_debug_info(DEBUGL_CHECKS, 1, "Found specialized worker(s) for '%s'", (slash && *slash != '/') ? slash : cmd_name);
	}
	if (cmd_name)
		free(cmd_name);

	return wp_list ? wp_list : &workers;
}

static struct wproc_worker *get_worker(const char *cmd)
{
	struct wproc_list *wp_list;

	if (!cmd)
		return NULL;

	wp_list = get_wproc_list(cmd);
	if (!wp_list || !wp_list->wps || !wp_list->len)
		return NULL;

	return wp_list->wps[wp_list->idx++ % wp_list->len];
}

static void run_job_callback(struct wproc_job *job, struct wproc_result *wpres, int val)
{
	if (!job || !job->callback)
		return;

	(*job->callback)(wpres, job->data, val);
	job->callback = NULL;
}

static void destroy_job(struct wproc_job *job)
{
	if (!job)
		return;

	/* call with NULL result to make callback clean things up */
	run_job_callback(job, NULL, 0);

	nm_free(job->command);
	if (job->wp) {
		fanout_remove(job->wp->jobs, job->id);
		job->wp->jobs_running--;
	}
	loadctl.jobs_running--;

	free(job);
}

static void fo_destroy_job(void *job)
{
	destroy_job((struct wproc_job *)job);
}

static int wproc_is_alive(struct wproc_worker *wp)
{
	if (!wp || !wp->pid)
		return 0;
	if (kill(wp->pid, 0) == 0 && iobroker_is_registered(nagios_iobs, wp->sd))
		return 1;
	return 0;
}

static int wproc_destroy(struct wproc_worker *wp, int flags)
{
	int i = 0, force = 0, self;

	if (!wp)
		return 0;

	force = !!(flags & WPROC_FORCE);

	self = getpid();

	if (self == nagios_pid && !force)
		return 0;

	/* free all memory when either forcing or a worker called us */
	iocache_destroy(wp->ioc);
	wp->ioc = NULL;
	nm_free(wp->name);
	fanout_destroy(wp->jobs, fo_destroy_job);
	wp->jobs = NULL;

	/* workers must never control other workers, so they return early */
	if (self != nagios_pid)
		return 0;

	/* kill(0, SIGKILL) equals suicide, so we avoid it */
	if (wp->pid) {
		kill(wp->pid, SIGKILL);
	}

	iobroker_close(nagios_iobs, wp->sd);

	/* reap this child if it still exists */
	do {
		int ret = waitpid(wp->pid, &i, 0);
		if (ret == wp->pid || (ret < 0 && errno == ECHILD))
			break;
	} while(1);

	free(wp);

	return 0;
}

/* remove the worker list pointed to by to_remove */
static int remove_specialized(void *data)
{
	if (data == to_remove)
		return DKHASH_WALK_REMOVE;
	return 0;
}

/* remove worker from job assignment list */
static void remove_worker(struct wproc_worker *worker)
{
	unsigned int i, j = 0;
	struct wproc_list *wpl = worker->wp_list;
	for (i = 0; i < wpl->len; i++) {
		if (wpl->wps[i] == worker)
			continue;
		wpl->wps[j++] = wpl->wps[i];
	}
	wpl->len = j;

	if (!specialized_workers || wpl->len)
		return;

	to_remove = wpl;
	dkhash_walk_data(specialized_workers, remove_specialized);
}


/*
 * This gets called from both parent and worker process, so
 * we must take care not to blindly shut down everything here
 */
void free_worker_memory(int flags)
{
	if (workers.wps) {
		unsigned int i;

		for (i = 0; i < workers.len; i++) {
			if (!workers.wps[i])
				continue;

			wproc_destroy(workers.wps[i], flags);
			workers.wps[i] = NULL;
		}

		free(workers.wps);
	}
	to_remove = NULL;
	dkhash_walk_data(specialized_workers, remove_specialized);
	dkhash_destroy(specialized_workers);
	workers.wps = NULL;
	workers.len = 0;
	workers.idx = 0;
}

static int str2timeval(char *str, struct timeval *tv)
{
	char *ptr, *ptr2;

	tv->tv_sec = strtoul(str, &ptr, 10);
	if (ptr == str) {
		tv->tv_sec = tv->tv_usec = 0;
		return -1;
	}
	if (*ptr == '.' || *ptr == ',') {
		ptr2 = ptr + 1;
		tv->tv_usec = strtoul(ptr2, &ptr, 10);
	}
	return 0;
}

/*
 * parses a worker result. We do no strdup()'s here, so when
 * kvv is destroyed, all references to strings will become
 * invalid
 */
static int parse_worker_result(wproc_result *wpres, struct kvvec *kvv)
{
	int i;

	for (i = 0; i < kvv->kv_pairs; i++) {
		struct wpres_key *k;
		char *key, *value;
		key = kvv->kv[i].key;
		value = kvv->kv[i].value;

		k = wpres_get_key(key, kvv->kv[i].key_len);
		if (!k) {
			nm_log(NSLOG_RUNTIME_WARNING, "wproc: Unrecognized result variable: (i=%d) %s=%s\n", i, key, value);
			continue;
		}
		switch (k->code) {
		case WPRES_job_id:
			wpres->job_id = atoi(value);
			break;
		case WPRES_command:
			wpres->command = value;
			break;
		case WPRES_timeout:
			wpres->timeout = atoi(value);
			break;
		case WPRES_wait_status:
			wpres->wait_status = atoi(value);
			break;
		case WPRES_start:
			str2timeval(value, &wpres->start);
			break;
		case WPRES_stop:
			str2timeval(value, &wpres->stop);
			break;
		case WPRES_type:
			/* Keep for backward compatibility of nagios special purpose workers */
			break;
		case WPRES_outstd:
			wpres->outstd = value;
			break;
		case WPRES_outerr:
			wpres->outerr = value;
			break;
		case WPRES_exited_ok:
			wpres->exited_ok = atoi(value);
			break;
		case WPRES_error_msg:
			wpres->exited_ok = FALSE;
			wpres->error_msg = value;
			break;
		case WPRES_error_code:
			wpres->exited_ok = FALSE;
			wpres->error_code = atoi(value);
			break;
		case WPRES_runtime:
			/* ignored */
			break;
		case WPRES_ru_utime:
			str2timeval(value, &wpres->rusage.ru_utime);
			break;
		case WPRES_ru_stime:
			str2timeval(value, &wpres->rusage.ru_stime);
			break;
		case WPRES_ru_minflt:
			wpres->rusage.ru_minflt = atoi(value);
			break;
		case WPRES_ru_majflt:
			wpres->rusage.ru_majflt = atoi(value);
			break;
		case WPRES_ru_nswap:
			wpres->rusage.ru_nswap = atoi(value);
			break;
		case WPRES_ru_inblock:
			wpres->rusage.ru_inblock = atoi(value);
			break;
		case WPRES_ru_oublock:
			wpres->rusage.ru_oublock = atoi(value);
			break;
		case WPRES_ru_msgsnd:
			wpres->rusage.ru_msgsnd = atoi(value);
			break;
		case WPRES_ru_msgrcv:
			wpres->rusage.ru_msgrcv = atoi(value);
			break;
		case WPRES_ru_nsignals:
			wpres->rusage.ru_nsignals = atoi(value);
			break;
		case WPRES_ru_nvcsw:
			wpres->rusage.ru_nsignals = atoi(value);
			break;
		case WPRES_ru_nivcsw:
			wpres->rusage.ru_nsignals = atoi(value);
			break;

		default:
			nm_log(NSLOG_RUNTIME_WARNING, "wproc: Recognized but unhandled result variable: %s=%s\n", key, value);
			break;
		}
	}
	return 0;
}

static int wproc_run_job(struct wproc_job *job, nagios_macros *mac);
static void fo_reassign_wproc_job(void *job_)
{
	struct wproc_job *job = (struct wproc_job *)job_;
	job->wp = get_worker(job->command);
	job->id = get_job_id(job->wp);
	/* macros aren't used right now anyways */
	wproc_run_job(job, NULL);
}

static int handle_worker_result(int sd, int events, void *arg)
{
	char *buf, *error_reason = NULL;
	unsigned long size;
	int ret;
	static struct kvvec kvv = KVVEC_INITIALIZER;
	struct wproc_worker *wp = (struct wproc_worker *)arg;

	if (iocache_capacity(wp->ioc) == 0) {
		nm_log(NSLOG_RUNTIME_WARNING, "wproc: iocache_capacity() is 0 for worker %s.\n", wp->name);
	}

	ret = iocache_read(wp->ioc, wp->sd);

	if (ret < 0) {
		nm_log(NSLOG_RUNTIME_WARNING, "wproc: iocache_read() from %s returned %d: %s\n",
		       wp->name, ret, strerror(errno));
		return 0;
	} else if (ret == 0) {
		nm_log(NSLOG_INFO_MESSAGE, "wproc: Socket to worker %s broken, removing", wp->name);
		wproc_num_workers_online--;
		iobroker_unregister(nagios_iobs, sd);
		if (workers.len <= 0) {
			/* there aren't global workers left, we can't run any more checks
			 * we should try respawning a few of the standard ones
			 */
			nm_log(NSLOG_RUNTIME_ERROR, "wproc: All our workers are dead, we can't do anything!");
		}
		remove_worker(wp);
		fanout_destroy(wp->jobs, fo_reassign_wproc_job);
		wp->jobs = NULL;
		wproc_destroy(wp, 0);
		return 0;
	}
	while ((buf = worker_ioc2msg(wp->ioc, &size, 0))) {
		struct wproc_job *job;
		wproc_result wpres;

		/* log messages are handled first */
		if (size > 5 && !memcmp(buf, "log=", 4)) {
			nm_log(NSLOG_INFO_MESSAGE, "wproc: %s: %s\n", wp->name, buf + 4);
			continue;
		}

		/* for everything else we need to actually parse */
		if (buf2kvvec_prealloc(&kvv, buf, size, '=', '\0', KVVEC_ASSIGN) <= 0) {
			nm_log(NSLOG_RUNTIME_ERROR,
			       "wproc: Failed to parse key/value vector from worker response with len %lu. First kv=%s",
			       size, buf ? buf : "(NULL)");
			continue;
		}

		memset(&wpres, 0, sizeof(wpres));
		wpres.job_id = -1;
		wpres.response = &kvv;
		wpres.source = wp->name;
		parse_worker_result(&wpres, &kvv);

		job = get_job(wp, wpres.job_id);
		if (!job) {
			nm_log(NSLOG_RUNTIME_WARNING, "wproc: Job with id '%d' doesn't exist on %s.\n", wpres.job_id, wp->name);
			continue;
		}

		/*
		 * ETIME ("Timer expired") doesn't really happen
		 * on any modern systems, so we reuse it to mean
		 * "program timed out"
		 */
		if (wpres.error_code == ETIME) {
			wpres.early_timeout = TRUE;
		}

		if (wpres.early_timeout) {
			nm_asprintf(&error_reason, "timed out after %.2fs", tv_delta_f(&wpres.start, &wpres.stop));
		} else if (WIFSIGNALED(wpres.wait_status)) {
			nm_asprintf(&error_reason, "died by signal %d%s after %.2f seconds",
			         WTERMSIG(wpres.wait_status),
			         WCOREDUMP(wpres.wait_status) ? " (core dumped)" : "",
			         tv_delta_f(&wpres.start, &wpres.stop));
		}
		if (error_reason) {
			log_debug_info(DEBUGL_IPC, DEBUGV_BASIC, "wproc: job %d from worker %s %s",
					job->id, wp->name, error_reason);
			log_debug_info(DEBUGL_IPC, DEBUGV_MORE, "wproc:   command: %s\n", job->command);
			log_debug_info(DEBUGL_IPC, DEBUGV_MORE, "wproc:   early_timeout=%d; exited_ok=%d; wait_status=%d; error_code=%d;\n",
			      wpres.early_timeout, wpres.exited_ok, wpres.wait_status, wpres.error_code);
			wproc_logdump_buffer(DEBUGL_IPC, DEBUGV_MORE, "wproc:   stderr", wpres.outerr);
			wproc_logdump_buffer(DEBUGL_IPC, DEBUGV_MORE, "wproc:   stdout", wpres.outstd);
		}
		nm_free(error_reason);

		run_job_callback(job, &wpres, 0);

		destroy_job(job);
	}

	return 0;
}

int workers_alive(void)
{
	unsigned int i;
	int alive = 0;

	for (i = 0; i < workers.len; i++) {
		if (wproc_is_alive(workers.wps[i]))
			alive++;
	}

	return alive;
}

/* a service for registering workers */
static int register_worker(int sd, char *buf, unsigned int len)
{
	int i, is_global = 1;
	struct kvvec *info;
	struct wproc_worker *worker;

	nm_log(NSLOG_INFO_MESSAGE, "wproc: Registry request: %s\n", buf);
	worker = nm_calloc(1, sizeof(*worker));
	info = buf2kvvec(buf, len, '=', ';', 0);
	if (info == NULL) {
		free(worker);
		nm_log(NSLOG_RUNTIME_ERROR, "wproc: Failed to parse registration request\n");
		return 500;
	}

	worker->sd = sd;
	worker->ioc = iocache_create(1 * 1024 * 1024);

	iobroker_unregister(nagios_iobs, sd);
	iobroker_register(nagios_iobs, sd, worker, handle_worker_result);

	for (i = 0; i < info->kv_pairs; i++) {
		struct key_value *kv = &info->kv[i];
		if (!strcmp(kv->key, "name")) {
			worker->name = nm_strdup(kv->value);
		} else if (!strcmp(kv->key, "pid")) {
			worker->pid = atoi(kv->value);
		} else if (!strcmp(kv->key, "max_jobs")) {
			worker->max_jobs = atoi(kv->value);
		} else if (!strcmp(kv->key, "plugin")) {
			struct wproc_list *command_handlers;
			is_global = 0;
			if (!(command_handlers = dkhash_get(specialized_workers, kv->value, NULL))) {
				command_handlers = nm_calloc(1, sizeof(struct wproc_list));
				command_handlers->wps = nm_calloc(1, sizeof(struct wproc_worker **));
				command_handlers->len = 1;
				command_handlers->wps[0] = worker;
				dkhash_insert(specialized_workers, nm_strdup(kv->value), NULL, command_handlers);
			} else {
				command_handlers->len++;
				command_handlers->wps = nm_realloc(command_handlers->wps, command_handlers->len * sizeof(struct wproc_worker **));
				command_handlers->wps[command_handlers->len - 1] = worker;
			}
			worker->wp_list = command_handlers;
		}
	}

	if (!worker->max_jobs) {
		/*
		 * each worker uses two filedescriptors per job, one to
		 * connect to the master and about 13 to handle libraries
		 * and memory allocation, so this guesstimate shouldn't
		 * be too far off (for local workers, at least).
		 */
		worker->max_jobs = (iobroker_max_usable_fds() / 2) - 50;
	}
	worker->jobs = fanout_create(worker->max_jobs);

	if (is_global) {
		workers.len++;
		workers.wps = nm_realloc(workers.wps, workers.len * sizeof(struct wproc_worker *));
		workers.wps[workers.len - 1] = worker;
		worker->wp_list = &workers;
	}
	wproc_num_workers_online++;
	kvvec_destroy(info, 0);
	nsock_printf_nul(sd, "OK");

	/* signal query handler to release its iocache for this one */
	return QH_TAKEOVER;
}

static int wproc_query_handler(int sd, char *buf, unsigned int len)
{
	char *space, *rbuf = NULL;

	if (!*buf || !strcmp(buf, "help")) {
		nsock_printf_nul(sd, "Control worker processes.\n"
		                 "Valid commands:\n"
		                 "  wpstats              Print general job information\n"
		                 "  register <options>   Register a new worker\n"
		                 "                       <options> can be name, pid, max_jobs and/or plugin.\n"
		                 "                       There can be many plugin args.");
		return 0;
	}

	if ((space = memchr(buf, ' ', len)) != NULL)
		* space = 0;

	rbuf = space ? space + 1 : buf;
	len -= (unsigned long)rbuf - (unsigned long)buf;

	if (!strcmp(buf, "register"))
		return register_worker(sd, rbuf, len);
	if (!strcmp(buf, "wpstats")) {
		unsigned int i;

		for (i = 0; i < workers.len; i++) {
			struct wproc_worker *wp = workers.wps[i];
			nsock_printf(sd, "name=%s;pid=%d;jobs_running=%u;jobs_started=%u\n",
			             wp->name, wp->pid,
			             wp->jobs_running, wp->jobs_started);
		}
		return 0;
	}

	return 400;
}

static int spawn_core_worker(void)
{
	char *argvec[] = {naemon_binary_path, "--worker", qh_socket_path ? qh_socket_path : DEFAULT_QUERY_SOCKET, NULL};
	int ret;

	if ((ret = spawn_helper(argvec)) < 0)
		nm_log(NSLOG_RUNTIME_ERROR, "wproc: Failed to launch core worker: %s\n", strerror(errno));
	else
		wproc_num_workers_spawned++;

	return ret;
}


int init_workers(int desired_workers)
{
	int i;

	/*
	 * we register our query handler before launching workers,
	 * so other workers can join us whenever they're ready
	 */
	specialized_workers = dkhash_create(512);
	if (!qh_register_handler("wproc", "Worker process management and info", 0, wproc_query_handler))
		nm_log(NSLOG_INFO_MESSAGE, "wproc: Successfully registered manager as @wproc with query handler\n");
	else
		nm_log(NSLOG_RUNTIME_ERROR, "wproc: Failed to register manager with query handler\n");

	if (desired_workers <= 0) {
		int cpus = online_cpus();

		if (desired_workers < 0) {
			desired_workers = cpus - desired_workers;
		}
		if (desired_workers <= 0) {
			desired_workers = cpus * 1.5;
			/* min 4 workers, as it's tested and known to work */
			if (desired_workers < 4)
				desired_workers = 4;
			else if (desired_workers > 48) {
				/* don't go crazy in NASA's network (1024 cores) */
				desired_workers = 48;
			}
		}
	}
	wproc_num_workers_desired = desired_workers;

	if (workers_alive() == desired_workers)
		return 0;

	/* can't shrink the number of workers (yet) */
	if (desired_workers < (int)workers.len)
		return -1;

	for (i = 0; i < desired_workers; i++)
		spawn_core_worker();

	return 0;
}


static struct wproc_job *create_job(void (*callback)(struct wproc_result *, void *, int), void *data, time_t timeout, const char *cmd)
{
	struct wproc_job *job;
	struct wproc_worker *wp;

	wp = get_worker(cmd);
	if (!wp)
		return NULL;

	job = nm_calloc(1, sizeof(*job));
	job->wp = wp;
	job->id = get_job_id(wp);
	job->callback = callback;
	job->data = data;
	job->timeout = timeout;
	if (fanout_add(wp->jobs, job->id, job) < 0 || !(job->command = nm_strdup(cmd))) {
		free(job);
		return NULL;
	}

	return job;
}

/*
 * Handles adding the command and macros to the kvvec,
 * as well as shipping the command off to a designated
 * worker
 */
static int wproc_run_job(struct wproc_job *job, nagios_macros *mac)
{
	static struct kvvec kvv = KVVEC_INITIALIZER;
	struct kvvec_buf *kvvb;
	struct wproc_worker *wp;
	int ret, result = OK;

	if (!job || !job->wp)
		return ERROR;

	wp = job->wp;

	/*
	 * XXX FIXME: add environment macros as
	 *  kvvec_addkv(kvv, "env", "NAGIOS_LALAMACRO=VALUE");
	 *  kvvec_addkv(kvv, "env", "NAGIOS_LALAMACRO2=VALUE");
	 * so workers know to add them to environment. For now,
	 * we don't support that though.
	 */
	if (!kvvec_init(&kvv, 4))	/* job_id, command and timeout */
		return ERROR;

	kvvec_addkv(&kvv, "job_id", (char *)mkstr("%d", job->id));
	kvvec_addkv(&kvv, "type", "0");
	kvvec_addkv(&kvv, "command", job->command);
	kvvec_addkv(&kvv, "timeout", (char *)mkstr("%u", job->timeout));
	kvvb = build_kvvec_buf(&kvv);
	ret = write(wp->sd, kvvb->buf, kvvb->bufsize);
	if (ret != (int)kvvb->bufsize) {
		nm_log(NSLOG_RUNTIME_ERROR, "wproc: '%s' seems to be choked. ret = %d; bufsize = %lu: errno = %d (%s)\n",
		       wp->name, ret, kvvb->bufsize, errno, strerror(errno));
		// these two will be decremented by destroy_job, so preemptively increment them
		wp->jobs_running++;
		loadctl.jobs_running++;
		destroy_job(job);
		result = ERROR;
	} else {
		wp->jobs_running++;
		wp->jobs_started++;
		loadctl.jobs_running++;
	}
	free(kvvb->buf);
	free(kvvb);

	return result;
}

int wproc_run_callback(char *cmd, int timeout,
                       void (*cb)(struct wproc_result *, void *, int), void *data,
                       nagios_macros *mac)
{
	struct wproc_job *job;
	job = create_job(cb, data, timeout, cmd);
	return wproc_run_job(job, mac);
}
