// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Sultan Alsawaf <sultan@kerneltoast.com>.
 */

/**
 * DOC: Capacity Aware Superset Scheduler (CASS) description
 *
 * The Capacity Aware Superset Scheduler (CASS) optimizes runqueue selection of
 * CFS tasks. By using CPU capacity as a basis for comparing the relative
 * utilization between different CPUs, CASS fairly balances load across CPUs of
 * varying capacities. This results in improved multi-core performance,
 * especially when CPUs are overutilized because CASS doesn't clip a CPU's
 * utilization when it eclipses the CPU's capacity.
 *
 * As a superset of capacity aware scheduling, CASS implements a hierarchy of
 * criteria to determine the better CPU to wake a task upon between CPUs that
 * have the same relative utilization. This way, single-core performance,
 * latency, and cache affinity are all optimized where possible.
 *
 * CASS doesn't feature explicit energy awareness but its basic load balancing
 * principle results in decreased overall energy, often better than what is
 * possible with explicit energy awareness. By fairly balancing load based on
 * relative utilization, all CPUs are kept at their lowest P-state necessary to
 * satisfy the overall load at any given moment.
 */

struct cass_cpu_cand {
	int cpu;
	unsigned int exit_lat;
	unsigned long cap;
	unsigned long util;
};

static __always_inline
unsigned long cass_cpu_util(int cpu, bool sync)
{
	struct cfs_rq *cfs_rq = &cpu_rq(cpu)->cfs;
	unsigned long util = READ_ONCE(cfs_rq->avg.util_avg);

	/* Deduct @current's util from this CPU if this is a sync wake */
	if (sync && cpu == smp_processor_id())
		lsub_positive(&util, task_util(current));

	if (sched_feat(UTIL_EST))
		util = max_t(unsigned long, util,
			     READ_ONCE(cfs_rq->avg.util_est.enqueued));

	return util;
}

/* Returns true if @a is a better CPU than @b */
static __always_inline
bool cass_cpu_better(const struct cass_cpu_cand *a,
		     const struct cass_cpu_cand *b,
		     int prev_cpu, bool sync)
{
#define cass_cmp(a, b) ({ res = (a) - (b); })
#define cass_eq(a, b) ({ res = (a) == (b); })
	long res;

	/* Prefer the CPU with lower relative utilization */
	if (cass_cmp(b->util, a->util))
		goto done;

	/* Prefer the current CPU for sync wakes */
	if (sync && (cass_eq(a->cpu, smp_processor_id()) ||
		     !cass_cmp(b->cpu, smp_processor_id())))
		goto done;

	/* Prefer the CPU with higher capacity */
	if (cass_cmp(a->cap, b->cap))
		goto done;

	/* Prefer the CPU with lower idle exit latency */
	if (cass_cmp(b->exit_lat, a->exit_lat))
		goto done;

	/* Prefer the previous CPU */
	if (cass_eq(a->cpu, prev_cpu) || !cass_cmp(b->cpu, prev_cpu))
		goto done;

	/* Prefer the CPU that shares a cache with the previous CPU */
	if (cass_cmp(cpus_share_cache(a->cpu, prev_cpu),
		     cpus_share_cache(b->cpu, prev_cpu)))
		goto done;

	/* @a isn't a better CPU than @b. @res must be <=0 to indicate such. */
done:
	/* @a is a better CPU than @b if @res is positive */
	return res > 0;
}

static int cass_best_cpu(struct task_struct *p, int prev_cpu, bool sync)
{
	int best_cpu = prev_cpu;
	unsigned long min_score = ULONG_MAX;
	unsigned long p_util;
	int cpu;

	/* Get the utilization for this task */
	p_util = clamp(task_util_est(p),
		       uclamp_eff_value(p, UCLAMP_MIN),
		       uclamp_eff_value(p, UCLAMP_MAX));

	/* Fast path: for very light tasks, stick to prev_cpu if it's idle */
	if (p_util < 16 && available_idle_cpu(prev_cpu))
		return prev_cpu;

	for_each_cpu_and(cpu, p->cpus_ptr, cpu_active_mask) {
		unsigned long util, rel_util, cap, score;
		bool is_idle, fits;

		/*
		 * Check if this CPU is idle. For sync wakes, always treat
		 * the current CPU as available.
		 */
		is_idle = (sync && cpu == smp_processor_id()) ||
			  available_idle_cpu(cpu) || sched_idle_cpu(cpu);

		/* Get this CPU's utilization, possibly without @current */
		util = cass_cpu_util(cpu, sync);

		/*
		 * Add @p's utilization to this CPU if it's not @p's CPU, to
		 * find what this CPU's relative utilization would look like
		 * if @p were on it.
		 */
		if (cpu != task_cpu(p))
			util += p_util;

		/*
		 * Get the current capacity of this CPU adjusted for thermal
		 * pressure as well as IRQ and RT-task time.
		 */
		cap = arch_scale_cpu_capacity(cpu);

		/* Calculate the relative utilization for this CPU candidate */
		rel_util = (util << SCHED_CAPACITY_SHIFT) / cap;

		/*
		 * Energy-aware CASS: check if the task fits comfortably on this
		 * CPU (util < ~80% of capacity).
		 */
		fits = (util * 1280) < (cap << 10);

		if (fits) {
			/*
			 * If it fits, our primary goal is energy savings.
			 * Score based heavily on capacity (lower is better).
			 */
			score = cap + (rel_util >> 4);

			/* Bias towards idle cores */
			if (is_idle)
				score -= min(score, 32UL);
		} else {
			/*
			 * If it doesn't fit, we are in performance mode.
			 * Penalize heavily so it loses to a fitting core.
			 */
			score = 100000 + rel_util;

			/* Tie-breaker: prefer higher capacity when overloaded */
			score -= min(score, cap >> 4);
		}

		/* Update the best CPU found so far */
		if (score < min_score) {
			min_score = score;
			best_cpu = cpu;
		}
	}

	return best_cpu;
}

static int cass_select_task_rq_fair(struct task_struct *p, int prev_cpu,
				    int sd_flag, int wake_flags)
{
	bool sync;

	/* Don't balance on exec since we don't know what @p will look like */
	if (sd_flag & SD_BALANCE_EXEC)
		return prev_cpu;

	/*
	 * If there aren't any valid CPUs which are active, then just return the
	 * first valid CPU since it's possible for certain types of tasks to run
	 * on inactive CPUs.
	 */
	if (unlikely(!cpumask_intersects(p->cpus_ptr, cpu_active_mask)))
		return cpumask_first(p->cpus_ptr);

	/* cass_best_cpu() needs the task's utilization, so sync it up */
	if (!(sd_flag & SD_BALANCE_FORK))
		sync_entity_load_avg(&p->se);

	sync = (wake_flags & WF_SYNC) && !(current->flags & PF_EXITING);
	return cass_best_cpu(p, prev_cpu, sync);
}
