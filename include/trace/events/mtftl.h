#undef TRACE_SYSTEM
#define TRACE_SYSTEM mtftl

#if !defined(_TRACE_MTFTL_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_MTFTL_H

#include <linux/tracepoint.h>


TRACE_EVENT(mt_rb, 
	TP_PROTO(unsigned int rwb, unsigned int nentry),

	TP_ARGS(rwb, nentry),

	TP_STRUCT__entry(
		__field(unsigned int, rwb)
		__field(unsigned int, nentry)
	),

	TP_fast_assign(
		__entry->rwb = rwb;
		__entry->nentry = nentry;
	),

	TP_printk("RB B %u n %u",
		__entry->rwb, __entry->nentry)
);

TRACE_EVENT(mt_us, 
	TP_PROTO(int type), 
	TP_ARGS(type),

	TP_STRUCT__entry(
		__field(int, type)
	),

	TP_fast_assign(
		__entry->type = type;
	),

	TP_printk("%d", __entry->type)
);

TRACE_EVENT(mt_ue, 
	TP_PROTO(int type), 
	TP_ARGS(type),

	TP_STRUCT__entry(
		__field(int, type)
	),

	TP_fast_assign(
		__entry->type = type;
	),

	TP_printk("%d", __entry->type)
);

TRACE_EVENT(mt_ws, 
	TP_PROTO(int type), 
	TP_ARGS(type),

	TP_STRUCT__entry(
		__field(int, type)
	),

	TP_fast_assign(
		__entry->type = type;
	),

	TP_printk("%d", __entry->type)
);

TRACE_EVENT(mt_we, 
	TP_PROTO(int ret), 
	TP_ARGS(ret),

	TP_STRUCT__entry(
		__field(int, ret)
	),

	TP_fast_assign(
		__entry->ret = ret;
	),

	TP_printk("ret %d", __entry->ret)
);



#endif /* _TRACE_MTFTL_H */

#include <trace/define_trace.h>
