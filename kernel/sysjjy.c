#include <linux/kernel.h>
#include <linux/sched.h>
#include "sched/sched.h"

asmlinkage long sys_jjycall(void)
{
	struct rq *rq = task_rq(current);

	printk("JJY: pid %d nr_running %d", current->pid, rq->nr_running);

	return 0;
}

