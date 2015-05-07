#include "w_defines.h"

#ifndef RESTORE_H
#define RESTORE_H

#include "sm_int_1.h"

#include <queue>

class sm_options;
class RestoreBitmap;
class RestoreScheduler;
class generic_page;

/** \brief Class that controls the process of restoring a failed volume
 *
 * \author Caetano Sauer
 */
class RestoreMgr : public smthread_t {
    friend class RestoreThread;
public:
    RestoreMgr(const sm_options&, LogArchiver::ArchiveDirectory*, vol_t*);
    virtual ~RestoreMgr();

    /** \brief Returns true if given page is already restored.
     *
     * This method is used to check if a page has already been restored, i.e.,
     * if it can be read from the volume already.
     */
    bool isRestored(const shpid_t& pid);

    /** \brief Request restoration of a given page
     *
     * This method is used by on-demand restore to signal the intention of
     * reading a specific page which is not yet restored. This method simply
     * generates a request with the restore scheduler -- no guarantees are
     * provided w.r.t. when page will be restored.
     *
     * The restored contents of the page will be copied into the given
     * address (if not null). This enables reuse in a buffer pool "fix" call,
     * foregoing the need for an extra read on the restored device. However,
     * this copy only happens if the segment still happens to be unrestored
     * when this method enters the critical section. If it gets restored
     * immediately before that, then the request is ignored and the method
     * returns false. This condition tells the caller it must read the page
     * contents from the restored device.
     */
    bool requestRestore(const shpid_t& pid, generic_page* addr = NULL);

    /** \brief Blocks until given page is restored
     *
     * This method will block until the given page ID is restored or the given
     * timeout is reached. It returns false in case of timeout and true if the
     * page is restored. When this method returns true, the caller is allowed
     * to read the page from the volume. This is basically equivalent to
     * polling on the isRestored() method.
     */
    bool waitUntilRestored(const shpid_t& pid, size_t timeout_in_ms = 0);

    size_t getNumPages() { return numPages; }
    size_t getSegmentSize() { return segmentSize; }
    shpid_t getFirstDataPid() { return firstDataPid; }

    virtual void run();

protected:
    RestoreBitmap* bitmap;
    RestoreScheduler* scheduler;
    LogArchiver::ArchiveDirectory* archive;
    vol_t* volume;

    std::map<shpid_t, generic_page*> bufferedRequests;
    mcs_rwlock requestMutex;

    pthread_cond_t restoreCond;
    pthread_mutex_t restoreCondMutex;

    /** \brief Number of pages restored so far
     * (must be a multiple of segmentSize)
     */
    size_t numRestoredPages;

    /** \brief Total number of pages in the failed volume
     */
    size_t numPages;

    /** \brief First page ID to be restored (i.e., skipping metadata pages)
     */
    shpid_t firstDataPid;

    /** \brief Size of a segment in pages
     *
     * The segment is the unit of restore, i.e., one segment is restored at a
     * time. The bitmap keeps track of segments already restored, i.e., one bit
     * per segment.
     */
    int segmentSize;

    /** \brief Whether volume metadata is alread restored or not
     */
    bool metadataRestored;

    /** \brief Whether to copy restored pages into caller's buffers, avoiding
     * extra reads
     */
    bool reuseRestoredBuffer;

    /** \brief Gives the segment number of a certain page ID.
     */
    size_t getSegmentForPid(const shpid_t& pid);

    /** \brief Gives the first page ID of a given segment number.
     */
    shpid_t getPidForSegment(size_t segment);

    /** \brief Restores metadata by replaying store operation log records
     *
     * This method is invoked before the restore loop starts (i.e., before any
     * data page is restored). It replays all store operations -- which are
     * logged on page id 0 -- in order to correctly restore volume metadata,
     * i.e., stnode_cache_t. Allocation pages (i.e., alloc_cache_t) doesn't
     * have to be restored explicitly, because pages are re-allocated when
     * replaying their first log records (e.g., page_img_format, btree_split,
     * etc.)
     */
    void restoreMetadata();

    /** \brief Method that executes the actual restore operations in a loop
     *
     * This method continuously gets page IDs to be restored from the scheduler
     * and performs the restore operation on the corresponding segment. The
     * method only returns once all segments have been restored.
     *
     * In the future, we may consider partitioning the volume and restore it in
     * parallel with multiple threads. To that end, this method should receive
     * a page ID interval to be restored.
     */
    void restoreLoop();

    /** \brief Concludes restore of a segment and flushes to replacement device
     *
     */
    void finishSegment(size_t segment, char* workspace, size_t count);
};

/** \brief Bitmap data structure that controls the progress of restore
 *
 * The bitmap contains one bit for each segment of the failed volume.  All bits
 * are initially "false", and a bit is set to "true" when the corresponding
 * segment has been restored. This class is completely oblivious to pages
 * inside a segment -- it is the callers resposibility to interpret what a
 * segment consists of.
 */
class RestoreBitmap {
public:
    RestoreBitmap(size_t size);
    virtual ~RestoreBitmap();

    size_t getSize() { return bits.size(); }

    bool get(size_t i);
    void set(size_t i);
protected:
    std::vector<bool> bits;
    mcs_rwlock mutex;
};

/** \brief Scheduler for restore operations. Decides what page to restore next.
 *
 * The restore loop in RestoreMgr restores segments in the order dictated
 * by this scheduler, using its next() method. The current implementation
 * is a simple FIFO queue. When the queue is empty, the first non-restored
 * segment in disk order is returned. This means that if no requests come in,
 * the restore loop behaves like a single-pass restore.
 */
class RestoreScheduler {
public:
    RestoreScheduler(const sm_options& options, RestoreMgr* restore);
    virtual ~RestoreScheduler();

    void enqueue(const shpid_t& pid);
    shpid_t next();

protected:
    RestoreMgr* restore;

    mcs_rwlock mutex;
    std::queue<shpid_t> queue;

    /// Perform single-pass restore while no requests are available
    bool trySinglePass;

    size_t numPages;

    /** Keep track of first pid not restored to continue single-pass restore.
     * This is just a guess to prune the search for the next not restored.
     */
    shpid_t firstNotRestored;

};

class RestoreThread : public smthread_t {
public:
    RestoreThread(RestoreMgr*, RestoreScheduler*);
    virtual ~RestoreThread();

    virtual void run();
};

#endif
