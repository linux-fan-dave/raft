#ifndef RAFT_H
#define RAFT_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#define RAFT_API __attribute__((visibility("default")))

/**
 * Error codes.
 */
#define RAFT_NOMEM 1            /* Out of memory */
#define RAFT_BADID 2            /* Server ID is not valid */
#define RAFT_DUPLICATEID 3      /* Server ID already in use */
#define RAFT_DUPLICATEADDRESS 4 /* Server address already in use */
#define RAFT_BADROLE 5          /* Server role is not valid */
#define RAFT_MALFORMED 6
#define RAFT_NOTLEADER 7
#define RAFT_LEADERSHIPLOST 8
#define RAFT_SHUTDOWN 9
#define RAFT_CANTBOOTSTRAP 10
#define RAFT_CANTCHANGE 11
#define RAFT_CORRUPT 12
#define RAFT_CANCELED 13
#define RAFT_NAMETOOLONG 14
#define RAFT_TOOBIG 15
#define RAFT_NOCONNECTION 16
#define RAFT_BUSY 17
#define RAFT_IOERR 18        /* File system or storage error */
#define RAFT_NOTFOUND 19     /* Resource not found */
#define RAFT_INVALID 20      /* Invalid parameter */
#define RAFT_UNAUTHORIZED 21 /* No access to a resource */
#define RAFT_NOSPACE 22      /* Not enough space on disk */
#define RAFT_TOOMANY 23      /* Some system or raft limit was hit */

/**
 * Size of human-readable error message buffers.
 */
#define RAFT_ERRMSG_BUF_SIZE 256

/**
 * Return the error message describing the given error code.
 */
RAFT_API const char *raft_strerror(int errnum);

/**
 * Hold the value of a raft server ID. Guaranteed to be at least 64-bit long.
 */
typedef unsigned long long raft_id;

/**
 * Hold the value of a raft term. Guaranteed to be at least 64-bit long.
 */
typedef unsigned long long raft_term;

/**
 * Hold the value of a raft entry index. Guaranteed to be at least 64-bit long.
 */
typedef unsigned long long raft_index;

/**
 * Hold a time value expressed in milliseconds since the epoch.
 */
typedef unsigned long long raft_time;

/**
 * A data buffer.
 */
struct raft_buffer
{
    void *base; /* Pointer to the buffer data. */
    size_t len; /* Length of the buffer. */
};

/**
 * Server role codes.
 */

#define RAFT_STANDBY 0 /* Replicate log, does not participate in quorum. */
#define RAFT_VOTER 1   /* Replicate log, does participate in quorum. */
#define RAFT_IDLE 2    /* Does not replicate log, or participate in quorum. */

/**
 * Hold information about a single server in the cluster configuration.
 */
struct raft_server
{
    raft_id id;    /* Server ID, must be greater than zero. */
    char *address; /* Server address. User defined. */
    int role;      /* Server role. */
};

/**
 * Hold information about all servers currently part of the cluster.
 */
struct raft_configuration
{
    struct raft_server *servers; /* Array of servers member of the cluster. */
    unsigned n;                  /* Number of servers in the array. */
};

/**
 * Initialize an empty raft configuration.
 */
RAFT_API void raft_configuration_init(struct raft_configuration *c);

/**
 * Release all memory used by the given configuration object.
 */
RAFT_API void raft_configuration_close(struct raft_configuration *c);

/**
 * Add a server to a raft configuration.
 *
 * The @id must be greater than zero and @address point to a valid string.
 *
 * The @role must be either #RAFT_VOTER, #RAFT_STANDBY, #RAFT_IDLE.
 *
 * If @id or @address are already in use by another server in the configuration,
 * an error is returned.
 *
 * The @address string will be copied and can be released after this function
 * returns.
 */
RAFT_API int raft_configuration_add(struct raft_configuration *c,
                                    raft_id id,
                                    const char *address,
                                    int role);

/**
 * Encode the given configuration object.
 *
 * The memory of the returned buffer is allocated using raft_malloc(), and
 * client code is responsible for releasing it when no longer needed.
 */
RAFT_API int raft_configuration_encode(const struct raft_configuration *c,
                                       struct raft_buffer *buf);

/**
 * Hash function which outputs a 64-bit value based on a text and a number.
 *
 * This can be used to generate a unique ID for a new server being added, for
 * example based on its address and on the current time in milliseconds since
 * the Epoch.
 *
 * It's internally implemented as a SHA1 where only the last 8 bytes of the hash
 * value are kept.
 */
RAFT_API unsigned long long raft_digest(const char *text, unsigned long long n);

/**
 * Log entry types.
 */
enum {
    RAFT_COMMAND = 1, /* Command for the application FSM. */
    RAFT_BARRIER,     /* Wait for all previous commands to be applied. */
    RAFT_CHANGE       /* Raft configuration change. */
};

/**
 * A single entry in the raft log.
 *
 * An entry that originated from this raft instance while it was the leader
 * (typically via client calls to raft_apply()) should normally have a @buf
 * attribute referencing directly the memory that was originally allocated by
 * the client itself to contain the entry data, and the @batch attribute set to
 * #NULL.
 *
 * An entry that was received from the network as part of an AppendEntries RPC
 * or that was loaded from disk at startup should normally have a @batch
 * attribute that points to a contiguous chunk of memory that contains the data
 * of the entry itself plus possibly the data for other entries that were
 * received or loaded with it at the same time. In this case the @buf pointer
 * will be equal to the @batch pointer plus an offset, that locates the position
 * of the entry's data within the batch.
 *
 * When the @batch attribute is not #NULL the raft library will take care of
 * releasing that memory only once there are no more references to the
 * associated entries.
 *
 * This arrangement makes it possible to minimize the amount of memory-copying
 * when performing I/O.
 */
struct raft_entry
{
    raft_term term;         /* Term in which the entry was created. */
    unsigned short type;    /* Type (FSM command, barrier, config change). */
    struct raft_buffer buf; /* Entry data. */
    void *batch;            /* Batch that buf's memory points to, if any. */
};

/**
 * Counter for outstanding references to a log entry.
 *
 * When an entry is first appended to the log, its refcount is set to one (the
 * log itself is the only one referencing the entry). Whenever an entry is
 * included in an I/O request (to write it to disk or to send it to other
 * servers) its refcount is increased by one. Whenever an entry gets deleted
 * from the log its refcount is decreased by one. Likewise, whenever an I/O
 * request is completed the refcount of the relevant entries is decreased by
 * one. When the refcount drops to zero the memory that its @buf attribute
 * points to gets released, or, if the @batch attribute is non-NULL, a check is
 * made to see if all other entries of the same batch also have a zero refcount,
 * and the memory that @batch points to gets released if that's the case.
 */
struct raft_entry_ref
{
    raft_term term;              /* Term of the entry being ref-counted. */
    raft_index index;            /* Index of the entry being ref-counted. */
    unsigned short count;        /* Number of references. */
    struct raft_entry_ref *next; /* Next item in the bucket (for collisions). */
};

/**
 * In-memory cache of the persistent raft log stored on disk.
 *
 * The raft log cache is implemented as a circular buffer of log entries, which
 * makes some frequent operations very efficient (e.g. deleting the first N
 * entries when snapshotting).
 */
struct raft_log
{
    struct raft_entry *entries;  /* Circular buffer of log entries. */
    size_t size;                 /* Number of available slots in the buffer. */
    size_t front, back;          /* Indexes of used slots [front, back). */
    raft_index offset;           /* Index of first entry is offset+1. */
    struct raft_entry_ref *refs; /* Log entries reference counts hash table. */
    size_t refs_size;            /* Size of the reference counts hash table. */
    struct                       /* Information about last snapshot, or zero. */
    {
        raft_index last_index; /* Snapshot replaces all entries up to here. */
        raft_term last_term;   /* Term of last index. */
    } snapshot;
};

/**
 * Hold the arguments of a RequestVote RPC.
 *
 * The RequestVote RPC is invoked by candidates to gather votes.
 */
struct raft_request_vote
{
    raft_term term;            /* Candidate's term. */
    raft_id candidate_id;      /* ID of the server requesting the vote. */
    raft_index last_log_index; /* Index of candidate's last log entry. */
    raft_index last_log_term;  /* Term of log entry at last_log_index. */
    bool disrupt_leader;       /* True if current leader should be discarded. */
};

/**
 * Hold the result of a RequestVote RPC.
 */
struct raft_request_vote_result
{
    raft_term term;    /* Receiver's current term (candidate updates itself). */
    bool vote_granted; /* True means candidate received vote. */
};

/**
 * Hold the arguments of an AppendEntries RPC.
 *
 * The AppendEntries RPC is invoked by the leader to replicate log entries. It's
 * also used as heartbeat (figure 3.1).
 */
struct raft_append_entries
{
    raft_term term;             /* Leader's term. */
    raft_index prev_log_index;  /* Index of log entry preceeding new ones. */
    raft_term prev_log_term;    /* Term of entry at prev_log_index. */
    raft_index leader_commit;   /* Leader's commit index. */
    struct raft_entry *entries; /* Log entries to append. */
    unsigned n_entries;         /* Size of the log entries array. */
};

/**
 * Hold the result of an AppendEntries RPC (figure 3.1).
 */
struct raft_append_entries_result
{
    raft_term term;            /* Receiver's current_term. */
    raft_index rejected;       /* If non-zero, the index that was rejected. */
    raft_index last_log_index; /* Receiver's last log entry index, as hint. */
};

/**
 * Hold the arguments of an InstallSnapshot RPC (figure 5.3).
 */
struct raft_install_snapshot
{
    raft_term term;                 /* Leader's term. */
    raft_index last_index;          /* Index of last entry in the snapshot. */
    raft_term last_term;            /* Term of last_index. */
    struct raft_configuration conf; /* Config as of last_index. */
    raft_index conf_index;          /* Commit index of conf. */
    struct raft_buffer data;        /* Raw snapshot data. */
};

/**
 * Hold the arguments of a TimeoutNow RPC.
 *
 * The TimeoutNow RPC is invoked by leaders to transfer leadership to a
 * follower.
 */
struct raft_timeout_now
{
    raft_term term;            /* Leader's term. */
    raft_index last_log_index; /* Index of leader's last log entry. */
    raft_index last_log_term;  /* Term of log entry at last_log_index. */
};

/**
 * Type codes for RPC messages.
 */
enum {
    RAFT_IO_APPEND_ENTRIES = 1,
    RAFT_IO_APPEND_ENTRIES_RESULT,
    RAFT_IO_REQUEST_VOTE,
    RAFT_IO_REQUEST_VOTE_RESULT,
    RAFT_IO_INSTALL_SNAPSHOT,
    RAFT_IO_TIMEOUT_NOW
};

/**
 * A single RPC message that can be sent or received over the network.
 */
struct raft_message
{
    unsigned short type;        /* RPC type code. */
    raft_id server_id;          /* ID of sending or destination server. */
    const char *server_address; /* Address of sending or destination server. */
    union {                     /* Type-specific data */
        struct raft_request_vote request_vote;
        struct raft_request_vote_result request_vote_result;
        struct raft_append_entries append_entries;
        struct raft_append_entries_result append_entries_result;
        struct raft_install_snapshot install_snapshot;
        struct raft_timeout_now timeout_now;
    };
};

/**
 * Hold the details of a snapshot.
 */
struct raft_snapshot
{
    /* Index and term of last entry included in the snapshot. */
    raft_index index;
    raft_term term;

    /* Last committed configuration included in the snapshot, along with the
     * index it was committed at. */
    struct raft_configuration configuration;
    raft_index configuration_index;

    /* Content of the snapshot. When a snapshot is taken, the user FSM can fill
     * the bufs array with more than one buffer. When a snapshot is restored,
     * there will always be a single buffer. */
    struct raft_buffer *bufs;
    unsigned n_bufs;
};

/**
 * Asynchronous request to send an RPC message.
 */
struct raft_io_send;
typedef void (*raft_io_send_cb)(struct raft_io_send *req, int status);
struct raft_io_send
{
    void *data;         /* User data */
    raft_io_send_cb cb; /* Request callback */
};

/**
 * Asynchronous request to store new log entries.
 */
struct raft_io_append;
typedef void (*raft_io_append_cb)(struct raft_io_append *req, int status);
struct raft_io_append
{
    void *data;           /* User data */
    raft_io_append_cb cb; /* Request callback */
};

/**
 * Asynchronous request to store a new snapshot.
 */
struct raft_io_snapshot_put;
typedef void (*raft_io_snapshot_put_cb)(struct raft_io_snapshot_put *req,
                                        int status);
struct raft_io_snapshot_put
{
    void *data;                 /* User data */
    raft_io_snapshot_put_cb cb; /* Request callback */
};

/**
 * Asynchronous request to load the most recent snapshot available.
 */
struct raft_io_snapshot_get;
typedef void (*raft_io_snapshot_get_cb)(struct raft_io_snapshot_get *req,
                                        struct raft_snapshot *snapshot,
                                        int status);
struct raft_io_snapshot_get
{
    void *data;                 /* User data */
    raft_io_snapshot_get_cb cb; /* Request callback */
};

/**
 * Customizable tracer, for debugging purposes.
 */
struct raft_tracer
{
    /**
     * Implementation-defined state object.
     */
    void *impl;

    /**
     * Emit the given trace message, possibly decorating it with the provided
     * metadata.
     */
    void (*emit)(struct raft_tracer *t,
                 const char *file,
                 int line,
                 const char *message);
};

struct raft_io; /* Forward declaration. */

/**
 * Callback invoked by the I/O implementation at regular intervals.
 */
typedef void (*raft_io_tick_cb)(struct raft_io *io);

/**
 * Callback invoked by the I/O implementation when an RPC message is received.
 */
typedef void (*raft_io_recv_cb)(struct raft_io *io, struct raft_message *msg);

/**
 * Callback invoked by the I/O implementation when the memory of the @io object
 * can be safely released.
 */
typedef void (*raft_io_close_cb)(struct raft_io *io);

/**
 * I/O backend interface implementing periodic ticks, log store read/writes
 * and send/receive of network RPCs.
 */
struct raft_io
{
    /**
     * API version implemented by this instance. Currently 1.
     */
    int version;

    /**
     * Custom user data.
     */
    void *data;

    /**
     * Implementation-defined state object.
     */
    void *impl;

    /**
     * Human-readable message providing diagnostic information about the last
     * error occurred.
     */
    char errmsg[RAFT_ERRMSG_BUF_SIZE];

    /**
     * Initialize the backend with operational parameters such as server ID and
     * address.
     */
    int (*init)(struct raft_io *io, raft_id id, const char *address);

    /**
     * Release all resources used by the backend.
     *
     * The @tick and @recv callbacks must not be invoked anymore, and pending
     * asynchronous requests be completed or canceled as soon as
     * possible. Invoke the close callback once the #raft_io instance can be
     * freed.
     */
    void (*close)(struct raft_io *io, raft_io_close_cb cb);

    /**
     * Load persisted state from storage.
     *
     * The implementation must synchronously load the current state from its
     * storage backend and return information about it through the given
     * pointers.
     *
     * The implementation can safely assume that this method will be invoked
     * exactly one time, before any call to append() or truncate(), and then
     * won't be invoked again.
     *
     * The snapshot object and entries array must be allocated and populated
     * using @raft_malloc. If this function completes successfully, ownership of
     * such memory is transfered to the caller.
     */
    int (*load)(struct raft_io *io,
                raft_term *term,
                raft_id *voted_for,
                struct raft_snapshot **snapshot,
                raft_index *start_index,
                struct raft_entry *entries[],
                size_t *n_entries);

    /**
     * Start the backend.
     *
     * From now on the implementation must start accepting RPC requests and must
     * invoke the @tick_cb callback every @msecs milliseconds. The @recv_cb
     * callback must be invoked when receiving a message.
     */
    int (*start)(struct raft_io *io,
                 unsigned msecs,
                 raft_io_tick_cb tick_cb,
                 raft_io_recv_cb recv_cb);

    /**
     * Bootstrap a server belonging to a new cluster.
     *
     * The I/O implementation must synchronously persist the given configuration
     * as the first entry of the log. The current persisted term must be set to
     * 1 and the vote to nil.
     *
     * If an attempt is made to bootstrap a server that has already some state,
     * then #RAFT_CANTBOOTSTRAP must be returned.
     */
    int (*bootstrap)(struct raft_io *io, const struct raft_configuration *conf);

    /**
     * Force appending a new configuration as last entry of the log.
     */
    int (*recover)(struct raft_io *io, const struct raft_configuration *conf);

    /**
     * Synchronously persist current term (and nil vote). The implementation
     * MUST ensure that the change is durable before returning (e.g. using
     * fdatasync() or #O_DSYNC).
     */
    int (*set_term)(struct raft_io *io, raft_term term);

    /**
     * Synchronously persist who we voted for. The implementation MUST ensure
     * that the change is durable before returning (e.g. using fdatasync() or
     * #O_DIRECT).
     */
    int (*set_vote)(struct raft_io *io, raft_id server_id);

    /**
     * Asynchronously send an RPC message.
     *
     * The implementation is guaranteed that the memory referenced in the given
     * message will not be released until the @cb callback is invoked.
     */
    int (*send)(struct raft_io *io,
                struct raft_io_send *req,
                const struct raft_message *message,
                raft_io_send_cb cb);

    /**
     * Asynchronously append the given entries to the log.
     *
     * The implementation is guaranteed that the memory holding the given
     * entries will not be released until the @cb callback is invoked.
     */
    int (*append)(struct raft_io *io,
                  struct raft_io_append *req,
                  const struct raft_entry entries[],
                  unsigned n,
                  raft_io_append_cb cb);

    /**
     * Asynchronously truncate all log entries from the given index onwards.
     */
    int (*truncate)(struct raft_io *io, raft_index index);

    /**
     * Asynchronously persist a new snapshot. If the @trailing parameter is
     * greater than zero, then all entries older that @snapshot->index -
     * @trailing must be deleted. If the @trailing parameter is 0, then the
     * snapshot completely replaces all existing entries, which should all be
     * deleted. Subsequent calls to append() should append entries starting at
     * index @snapshot->index + 1.
     *
     * If a request is submitted, the raft engine won't submit any other request
     * until the original one has completed.
     */
    int (*snapshot_put)(struct raft_io *io,
                        unsigned trailing,
                        struct raft_io_snapshot_put *req,
                        const struct raft_snapshot *snapshot,
                        raft_io_snapshot_put_cb cb);

    /**
     * Asynchronously load the last snapshot.
     */
    int (*snapshot_get)(struct raft_io *io,
                        struct raft_io_snapshot_get *req,
                        raft_io_snapshot_get_cb cb);

    /**
     * Return the current time, expressed in milliseconds since the epoch.
     */
    raft_time (*time)(struct raft_io *io);

    /**
     * Generate a random integer between min and max.
     */
    int (*random)(struct raft_io *io, int min, int max);
};

/**
 * Interface for the user-implemented finate state machine replicated through
 * Raft.
 */
struct raft_fsm
{
    /**
     * API version implemented by this instance. Currently 1.
     */
    int version;

    /**
     * Custom user data.
     */
    void *data;

    /**
     * Apply a committed RAFT_COMMAND entry to the state machine.
     */
    int (*apply)(struct raft_fsm *fsm,
                 const struct raft_buffer *buf,
                 void **result);

    /**
     * Take a snapshot of the state machine.
     */
    int (*snapshot)(struct raft_fsm *fsm,
                    struct raft_buffer *bufs[],
                    unsigned *n_bufs);

    /**
     * Restore a snapshot of the state machine.
     */
    int (*restore)(struct raft_fsm *fsm, struct raft_buffer *buf);
};

/**
 * State codes.
 */
enum { RAFT_UNAVAILABLE, RAFT_FOLLOWER, RAFT_CANDIDATE, RAFT_LEADER };

/**
 * Used by leaders to keep track of replication progress for each server.
 */
struct raft_progress
{
    unsigned short state;      /* Probe, pipeline or snapshot. */
    raft_index next_index;     /* Next entry to send. */
    raft_index match_index;    /* Highest index reported as replicated. */
    raft_index snapshot_index; /* Last index of most recent snapshot sent. */
    raft_time last_send;       /* Timestamp of last AppendEntries RPC. */
    bool recent_recv;          /* A msg was received within election timeout. */
};

struct raft; /* Forward declaration. */

/**
 * Transfer leadership callback.
 */
typedef void (*raft_transfer_leadership_cb)(struct raft *raft);

/**
 * Close callback.
 *
 * It's safe to release the memory of a raft instance only after this callback
 * has fired.
 */
typedef void (*raft_close_cb)(struct raft *raft);

/**
 * Hold and drive the state of a single raft server in a cluster.
 */
struct raft
{
    void *data;                 /* Custom user data. */
    struct raft_tracer *tracer; /* Tracer implementation. */
    struct raft_io *io;         /* Disk and network I/O implementation. */
    struct raft_fsm *fsm;       /* User-defined FSM to apply commands to. */
    raft_id id;                 /* Server ID of this raft instance. */
    char *address;              /* Server address of this raft instance. */

    /*
     * Cache of the server's persistent state, updated on stable storage before
     * responding to RPCs (Figure 3.1).
     */
    raft_term current_term; /* Latest term server has seen. */
    raft_id voted_for;      /* Candidate that received vote in current term. */
    struct raft_log log;    /* Log entries. */

    /*
     * Current membership configuration (Chapter 4).
     *
     * At any given moment the current configuration can be committed or
     * uncommitted.
     *
     * If a server is voting, the log entry with index 1 must always contain the
     * first committed configuration.
     *
     * The possible scenarios are:
     *
     * 1. #configuration_index and #configuration_uncommited_index are both
     *    zero. This should only happen when a brand new server starts joining a
     *    cluster and is waiting to receive log entries from the current
     *    leader. In this case #configuration must be empty and have no servers.
     *
     * 2. #configuration_index is non-zero while #configuration_uncommited_index
     *    is zero. In this case the content of #configuration must match the one
     *    of the log entry at #configuration_index.
     *
     * 3. #configuration_index and #configuration_uncommited_index are both
     *    non-zero, with the latter being greater than the former. In this case
     *    the content of #configuration must match the one of the log entry at
     *    #configuration_uncommitted_index.
     */
    struct raft_configuration configuration;
    raft_index configuration_index;
    raft_index configuration_uncommitted_index;

    /*
     * Election timeout in milliseconds (default 1000).
     *
     * From 3.4:
     *
     *   Raft uses a heartbeat mechanism to trigger leader election. When
     *   servers start up, they begin as followers. A server remains in follower
     *   state as long as it receives valid RPCs from a leader or
     *   candidate. Leaders send periodic heartbeats (AppendEntries RPCs that
     *   carry no log entries) to all followers in order to maintain their
     *   authority. If a follower receives no communication over a period of
     *   time called the election timeout, then it assumes there is no viable
     *   leader and begins an election to choose a new leader.
     *
     * This is the baseline value and will be randomized between 1x and 2x.
     *
     * See raft_change_election_timeout() to customize the value of this
     * attribute.
     */
    unsigned election_timeout;

    /*
     * Heartbeat timeout in milliseconds (default 100). This is relevant only
     * for when the raft instance is in leader state: empty AppendEntries RPCs
     * will be sent if this amount of milliseconds elapses without any
     * user-triggered AppendEntries RCPs being sent.
     *
     * From Figure 3.1:
     *
     *   [Leaders] Send empty AppendEntries RPC during idle periods to prevent
     *   election timeouts.
     */
    unsigned heartbeat_timeout;

    /*
     * The fields below hold the part of the server's volatile state which is
     * always applicable regardless of the whether the server is follower,
     * candidate or leader (Figure 3.1). This state is rebuilt automatically
     * after a server restart.
     */
    raft_index commit_index; /* Highest log entry known to be committed */
    raft_index last_applied; /* Highest log entry applied to the FSM */
    raft_index last_stored;  /* Highest log entry persisted on disk */

    /*
     * Current server state of this raft instance, along with a union defining
     * state-specific values.
     */
    unsigned short state;
    union {
        struct /* Follower */
        {
            unsigned randomized_election_timeout; /* Timer expiration. */
            struct                                /* Current leader info. */
            {
                raft_id id;
                char *address;
            } current_leader;
        } follower_state;
        struct
        {
            unsigned randomized_election_timeout; /* Timer expiration. */
            bool *votes;                          /* Vote results. */
        } candidate_state;
        struct
        {
            struct raft_progress *progress; /* Per-server replication state. */
            struct raft_change *change;     /* Pending membership change. */
            raft_id promotee_id;            /* ID of server being promoted. */
            unsigned short round_number;    /* Current sync round. */
            raft_index round_index;         /* Target of the current round. */
            raft_time round_start;          /* Start of current round. */
            void *requests[2];              /* Outstanding client requests. */
        } leader_state;
    };

    /* Election timer start.
     *
     * This timer has different purposes depending on the state. Followers
     * convert to candidate after the randomized election timeout has elapsed
     * without leader contact. Candidates start a new election after the
     * randomized election timeout has elapsed without a winner. Leaders step
     * down after the election timeout has elapsed without contacting a majority
     * of voting servers. */
    raft_time election_timer_start;

    /* Information about an in-progress leadership transfer. */
    struct
    {
        raft_id server_id;              /* ID of target server. */
        raft_time start;                /* Start of leadership transfer. */
        raft_transfer_leadership_cb cb; /* User callback. */
        struct raft_io_send send;       /* For sending TimeoutNow */
    } leadership_transfer;

    /*
     * Information about the last snapshot that was taken (if any).
     */
    struct
    {
        unsigned threshold;              /* N. of entries before snapshot */
        unsigned trailing;               /* N. of trailing entries to retain */
        struct raft_snapshot pending;    /* In progress snapshot */
        struct raft_io_snapshot_put put; /* Store snapshot request */
    } snapshot;

    /*
     * Callback to invoke once a close request has completed.
     */
    raft_close_cb close_cb;

    /*
     * Human-readable message providing diagnostic information about the last
     * error occurred.
     */
    char errmsg[RAFT_ERRMSG_BUF_SIZE];
};

/**
 * Initialize a raft server object.
 */
RAFT_API int raft_init(struct raft *r,
                       struct raft_io *io,
                       struct raft_fsm *fsm,
                       raft_id id,
                       const char *address);

/**
 * Close a raft instance, releasing all used resources.
 *
 * The memory of instance itself can be released only once the given callback
 * has been invoked.
 */
RAFT_API void raft_close(struct raft *r, raft_close_cb cb);

/**
 * Bootstrap this raft instance using the given configuration. The instance must
 * not have been started yet and must be completely pristine, otherwise
 * #RAFT_CANTBOOTSTRAP will be returned.
 */
RAFT_API int raft_bootstrap(struct raft *r,
                            const struct raft_configuration *conf);

/**
 * Force a new configuration in order to recover from a loss of quorum where the
 * current configuration cannot be restored, such as when a majority of servers
 * die at the same time.
 *
 * This works by appending the new configuration directly to the log stored on
 * disk.
 *
 * In order for this operation to be safe you must follow these steps:
 *
 * 1. Make sure that no servers in the cluster are running, either because they
 *    died or because you manually stopped them.
 *
 * 2. Run @raft_recover exactly one time, on the non-dead server which has
 *    the highest term and the longest log.
 *
 * 3. Copy the data directory of the server you ran @raft_recover on to all
 *    other non-dead servers in the cluster, replacing their current data
 *    directory.
 *
 * 4. Restart all servers.
 */
RAFT_API int raft_recover(struct raft *r,
                          const struct raft_configuration *conf);

/**
 * Start the given raft instance.
 *
 * The initial term, vote, snapshot and entries will be loaded from disk using
 * the raft_io->load method. The instance will start as #RAFT_FOLLOWER, unless
 * it's the only voting server in the cluster, in which case it will
 * automatically elect itself and become #RAFT_LEADER.
 */
RAFT_API int raft_start(struct raft *r);

/**
 * Set the election timeout.
 *
 * Every raft instance is initialized with a default election timeout of 1000
 * milliseconds. If you wish to tweak it, call this function before starting
 * your event loop.
 *
 * From Chapter 9:
 *
 *   We recommend a range that is 10-20 times the one-way network latency, which
 *   keeps split votes rates under 40% in all cases for reasonably sized
 *   clusters, and typically results in much lower rates.
 *
 * Note that the current random election timer will be reset and a new one timer
 * will be generated.
 */
RAFT_API void raft_set_election_timeout(struct raft *r, unsigned msecs);

/**
 * Set the heartbeat timeout.
 */
RAFT_API void raft_set_heartbeat_timeout(struct raft *r, unsigned msecs);

/**
 * Number of outstanding log entries before starting a new snapshot. The default
 * is 1024.
 */
RAFT_API void raft_set_snapshot_threshold(struct raft *r, unsigned n);

/**
 * Number of outstanding log entries to keep in the log after a snapshot has
 * been taken. This avoids sending snapshots when a follower is behind by just a
 * few entries. The default is 128.
 */
RAFT_API void raft_set_snapshot_trailing(struct raft *r, unsigned n);

/**
 * Return a human-readable description of the last error occured.
 */
RAFT_API const char *raft_errmsg(struct raft *r);

/**
 * Return the code of the current raft state.
 */
RAFT_API int raft_state(struct raft *r);

/**
 * Return the ID and address of the current known leader, if any.
 */
RAFT_API void raft_leader(struct raft *r, raft_id *id, const char **address);

/**
 * Return the index of the last entry that was appended to the local log.
 */
RAFT_API raft_index raft_last_index(struct raft *r);

/**
 * Return the index of the last entry that was applied to the local FSM.
 */
RAFT_API raft_index raft_last_applied(struct raft *r);

/* Common fields across client request types. */
#define RAFT__REQUEST \
    void *data;       \
    int type;         \
    raft_index index; \
    void *queue[2]

/**
 * Asynchronous request to append a new command entry to the log and apply it to
 * the FSM when a quorum is reached.
 */
struct raft_apply;
typedef void (*raft_apply_cb)(struct raft_apply *req, int status, void *result);
struct raft_apply
{
    RAFT__REQUEST;
    raft_apply_cb cb;
};

/**
 * Propose to append commands to the log and apply them to the FSM once
 * committed.
 *
 * If this server is the leader, it will create @n new log entries of type
 * #RAFT_COMMAND using the given buffers as their payloads, append them to its
 * own log and attempt to replicate them on other servers by sending
 * AppendEntries RPCs.
 *
 * The memory pointed at by the @base attribute of each #raft_buffer in the
 * given array must have been allocated with raft_malloc() or a compatible
 * allocator. If this function returns 0, the ownership of this memory is
 * implicitly transferred to the raft library, which will take care of releasing
 * it when appropriate. Any further client access to such memory leads to
 * undefined behavior.
 *
 * The ownership of the memory of the @bufs array itself is not transferred to
 * the raft library, and, if allocated dynamically, must be deallocated by the
 * caller.
 */
RAFT_API int raft_apply(struct raft *r,
                        struct raft_apply *req,
                        const struct raft_buffer bufs[],
                        const unsigned n,
                        raft_apply_cb cb);

/**
 * Asynchronous request to append a barrier entry.
 */
struct raft_barrier;
typedef void (*raft_barrier_cb)(struct raft_barrier *req, int status);
struct raft_barrier
{
    RAFT__REQUEST;
    raft_barrier_cb cb;
};

/**
 * Propose to append a log entry of type #RAFT_BARRIER.
 *
 * This can be used to ensure that there are no unapplied commands.
 */
RAFT_API int raft_barrier(struct raft *r,
                          struct raft_barrier *req,
                          raft_barrier_cb cb);

/**
 * Asynchronous request to change the raft configuration.
 */
struct raft_change;
typedef void (*raft_change_cb)(struct raft_change *req, int status);
struct raft_change
{
    void *data;
    raft_change_cb cb;
};

/**
 * Add a new server to the cluster configuration. Its initial role will be
 * #RAFT_IDLE.
 */
RAFT_API int raft_add(struct raft *r,
                      struct raft_change *req,
                      raft_id id,
                      const char *address,
                      raft_change_cb cb);

/**
 * Promote the given server to a higher role.
 *
 * If the server's current role is #RAFT_IDLE, the server can be promoted either
 * to #RAFT_STANDBY or #RAFT_VOTER.
 *
 * If the server's current role is #RAFT_STANDBY, the server can be promoted to
 * #RAFT_VOTER.
 *
 * In all other cases, #RAFT_BADROLE is returned.
 */
RAFT_API int raft_promote(struct raft *r,
                          struct raft_change *req,
                          raft_id id,
                          int role,
                          raft_change_cb cb);

/**
 * Demote the given server to a lesser role.
 *
 * If the server's current role is #RAFT_VOTER, the server can be demoted either
 * to #RAFT_STANDBY or #RAFT_IDLE.
 *
 * If the server's current role is #RAFT_STANDBY, the server can be demoted to
 * #RAFT_IDLE.
 *
 * In all other cases, #RAFT_BADROLE is returned.
 */
RAFT_API int raft_demote(struct raft *r,
                         struct raft_change *req,
                         raft_id id,
                         int role,
                         raft_change_cb cb);

/**
 * Remove the given server from the cluster configuration.
 */
RAFT_API int raft_remove(struct raft *r,
                         struct raft_change *req,
                         raft_id id,
                         raft_change_cb cb);

/**
 * Transfer leadership to the server with the given ID.
 *
 * If the target server is not part of the configuration, or it's the leader
 * itself, or it's not a #RAFT_VOTER, then #RAFT_BADID is returned.
 *
 * The special value #0 means to automatically select a voting follower to
 * transfer leadership to. If there are no voting followers, return
 * #RAFT_NOTFOUND.
 *
 * When this server detects that the target server has become the leader, or
 * when @election_timeout milliseconds have elapsed, the given callback will be
 * invoked.
 *
 * After the callback files, clients can check whether the operation was
 * successful or not by calling @raft_leader() and checking if it returns the
 * target server.
 */
RAFT_API int raft_transfer_leadership(struct raft *r,
                                      raft_id id,
                                      raft_transfer_leadership_cb cb);
/**
 * User-definable dynamic memory allocation functions.
 *
 * The @data field will be passed as first argument to all functions.
 */
struct raft_heap
{
    void *data; /* User data */
    void *(*malloc)(void *data, size_t size);
    void (*free)(void *data, void *ptr);
    void *(*calloc)(void *data, size_t nmemb, size_t size);
    void *(*realloc)(void *data, void *ptr, size_t size);
    void *(*aligned_alloc)(void *data, size_t alignment, size_t size);
};

RAFT_API void *raft_malloc(size_t size);
RAFT_API void raft_free(void *ptr);
RAFT_API void *raft_calloc(size_t nmemb, size_t size);
RAFT_API void *raft_realloc(void *ptr, size_t size);
RAFT_API void *raft_aligned_alloc(size_t alignment, size_t size);

/**
 * Use a custom dynamic memory allocator.
 */
RAFT_API void raft_heap_set(struct raft_heap *heap);

/**
 * Use the default dynamic memory allocator (from the stdlib). This clears any
 * custom allocator specified with @raft_heap_set.
 */
RAFT_API void raft_heap_set_default(void);

#undef RAFT__REQUEST

#endif /* RAFT_H */
