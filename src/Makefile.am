lib_LTLIBRARIES = liblwlock.la
include_HEADERS = lw_lock.h \
                  lw_types.h \
				  lw_mutex.h \
				  lw_mutex2b.h \
				  lw_rwlock.h \
				  lw_cond_var.h \
				  lw_bitlock.h \
				  atomic_list.h \
				  murmur.h \
				  lf_stack.h \
				  lw_monitor.h \
				  delegate.h \
				  counter.h

liblwlock_la_SOURCES =  lw_lock.h \
                        lw_types.h \
		   		        lw_mutex.h \
				        lw_mutex2b.h \
						lw_rwlock.h \
				        lw_cond_var.h \
				        lw_util.h \
					    lw_atomic.h \
						lw_compiler.h \
						lw_cycles.h \
						lw_debug.h \
						lw_dlist.h \
						lw_event.h \
						lw_magic.h \
						lw_waiter.h \
						lw_waiter_intern.h \
						lw_lock.c \
						lw_mutex.c \
				        lw_mutex2b.c \
						lw_cond_var.c \
						lw_atomic.c \
						lw_cycles.c \
						lw_dlist.c \
						lw_waiter.c \
						lw_rwlock.c \
						lw_bitlock.c \
						atomic_list.c \
						lf_stack.c \
						lw_monitor.c \
						delegate.c \
						counter.c

liblwlock_la_LDFLAGS = -version-info 0:0:0
liblwlock_la_LIBADD  = $(PTHREAD_LIBS)
AM_LDFLAGS = $(PTHREAD_CFLAGS)
