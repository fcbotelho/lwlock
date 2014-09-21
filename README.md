Light weight locks
==================

Overview
--------

Light Weight Locks are a near drop in replacement for pthread locking primitives. It provides
certain benefits over regular locking primitives.

* Small memory footprint with 4-byte reader-writer lock, 4 byte POSIX-complaint mutex, 2 byte
non-POSIX mutex and 4 byte condition variable. 
* Reader-writer locks are implemented with a single atomic instead of a mutex + counters that
doesn't suffer from unintended contention in pure reader case as does the pthread implementation.
* RW locks support upgrade / downgrade operations.
* Completely fair (FIFO). This may show poorly in benchmarks but works out far better in practice.

But where LWLocks truly outshine is their ability to expose the internal steps of a locking
operation to allow far more interesting operations. Acquiring a lock, when there is contention,
requires adding the acquirer to the wait list, suspending the thread, and at a later point when the
lock is available, assign it to the thread and wake it up (or in a poor implementation, wake the
thread up so it can retry the acquire-or-sleep step).  Most locking schemes have the 3 steps as one
indivisible whole (the lock API).  LWLocks break up the steps into separate stages which are
exposed, to those desiring to leverage them, to do more interesting things. 

* Being put on a wait list but not blocking right away. These are exposed via the various \_async
APIs. It allows the thread to take any corrective action or do other work before blocking.
* Wait list being accessible allows for creation of custom lock transfer schemes including
implementing end to end fairness across locks instead of localized decision per lock. (Exposing this
conveniently is a work in progress).

The included adlist library leverages the async APIs to work around lock ordering issues that would
otherwise cause deadlocks. It allows concurrent operations that do not overlap in the nodes touched
to proceed in parallel while conflicing operations are able to advance in a fair manner without
causing deadlocks.

The long term goal of this project is to provide high performance, highly concurrent higher order
abstractions that are also easy to work with. Lwlocks and adlists are only the first small steps.


What Is Available
-----------------

- Locking primitives
  * lw\_mutex\_t - a 4 byte POSIX complaint mutex.
  * lw\_mutex2b\_t - a 2 byte non-POSIX mutex.
  * lw\_rwlock\_t - a 4 byte reader writer lock.
  * lw\_condvar\_t - a 4 byte condition variable usable with all of the above primitives (watch out
    missed signals with reader locks!), pthread mutexes and more.
- Concurrent lists -  adlist\_t

Wish List
---------

- Lock free stacks
- Lock free queues
- Implicit locks
- Concurrent hash tables
- Concurrent trees
- ...

Interested?
-----------

If the functionality covered by lwlocks or any of the included structures is of interest to you,
drop us a line! We would be happy to guide you in integrating the code with your own. If you are
already using it, we would like to hear about your experience and any wish list items you may have.
Any suggestions for improvements, actual code improvements and bug fixes are of course always
welcome too.

