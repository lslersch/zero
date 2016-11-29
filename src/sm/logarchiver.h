#include "w_defines.h"

#ifndef LOGARCHIVER_H
#define LOGARCHIVER_H

#define SM_SOURCE

#include "thread_wrapper.h"
#include "sm_base.h"

#include "w_heap.h"
#include "ringbuffer.h"
#include "mem_mgmt.h"
#include "log_storage.h"

#include <bitset>
#include <queue>
#include <set>

#define BOOST_FILESYSTEM_NO_DEPRECATED
#include <boost/filesystem.hpp>
namespace fs = boost::filesystem;

class sm_options;
class LogScanner;

typedef int32_t run_number_t;

/** \brief Object to control execution of background threads.
 *
 * Encapsulates an activation loop that relies on pthread condition variables.
 * The background thread calls waitForActivation() while it waits for an
 * activation from an orchestrating thread. Before calling this method,
 * however, it must acquire the mutex, in order to obey the pthread wait
 * protocol.  Once its work is complete, the activated state is unsed and the
 * mutex must be released. In practice, therefore, waitForActivation() is
 * usually invoked as follows.
 *
 * \code{.cpp}
    while (true) {
        CRITICAL_SECTION(cs, control.mutex);
        bool activated = control.waitForActivation();
        if (!activated) {
            break;
        }

        // do work...

    } // mutex released due to CRITICAL_SECTION macro
 * \endcode
 *
 * The wait for an activation is interrupted either by receiving a signal or by
 * setting a shutdown flag, in which case the method returns false. The flag is
 * simply a pointer to some external boolean variable, which means that the
 * background thread "watches for" a shutdown flag somewhere else. The
 * orchestrating thread calls activate() to wake up the background thread,
 * causing the waitForActivation() call to return with true.  The wait
 * parameter makes the activation wait to acquire the mutex, which guarantees
 * that the signal was sent. Otherwise, if the mutex is already held (i.e.,
 * background thread is already running) the method returns false immediately.
 *
 * Optionally, the activate method accepts an lsn, which is stored in endLSN,
 * but only if it is greater than the currently set value. This class does not
 * interpret this LSN value. It is only used by the background thread itself
 * as a marker for the end of its job. This is useful for threads that consume
 * its owrk units from the log, such as LogArchiver or ReaderThread. Other
 * thread classes may completely ignore this variable.
 *
 * \sa LogArchiver, LogArchiver::ReaderThread, ArchiveMerger
 *
 * \author Caetano Sauer
 */
struct ArchiverControl {
    pthread_mutex_t mutex;
    pthread_cond_t activateCond;
    lsn_t endLSN;
    bool activated;
    bool listening;
    bool* shutdownFlag;

    ArchiverControl(bool* shutdown);
    ~ArchiverControl();
    bool activate(bool wait, lsn_t lsn = lsn_t::null);
    bool waitForActivation();
};

/** \brief Implementation of a log archiver using asynchronous reader and
 * writer threads.
 *
 * The log archiver runs as a background daemon whose execution is controlled
 * by an ArchiverControl object. Once a log archiver thread is created and
 * forked, it waits for an activation to start working. The caller thread must
 * invoke the activate() method to perform this activation.
 *
 * Log archiving works in <b>activation cycles</b>, in which it first waits for
 * an activation and then consumes the recovery log up to a given LSN value
 * (\see activate(bool, lsn_t)).  This cycle is executed in an infinite loop
 * until the method shutdown() is invoked.  Once shutdown is invoked, the
 * current cycle is <b>not</b> interrupted. Instead, it finishes consuming the
 * log until the LSN given in the last successful activation and only then it
 * exits. The destructor also invokes shutdown() if not done yet.
 *
 * The class LogArchiver itself serves merely as an orchestrator of its
 * components, which are:
 * - LogArchiver::LogConsumer, which encapsulates a reader thread and parsing
 *   individual log records from the recovery log.
 * - LogArchiver::ArchiverHeap, which performs run generation by sorting the
 *   input stream given by the log consumer.
 * - LogArchiver::BlockAssembly, which consumes the sorted output from the
 *   heap, builds indexed blocks of log records (used for instant restore), and
 *   passes them over to the asynchronous writer thread
 * - LogArchiver::ArchiveDirectory, which represents the set of sorted runs
 *   that compose the log archive itself. It manages filesystem operations to
 *   read from and write to the log archive, controls access to the archive
 *   index, and provides scanning facilities used by restore.
 *
 * One activation cycle consists of consuming all log records from the log
 * consumer, which must first be opened with the given "end LSN". Each log
 * record is then inserted into the heap until it becomes full. Then, log
 * records are removed from the heap (usually in bulk, e.g., one block at a
 * time) and passed to the block assembly component. The cycle finishes once
 * all log records up to the given LSN are <b>inserted into the heap</b>, which
 * does not necessarily mean that the persistent log archive will contain all
 * those log records. The only way to enforce that is to perform a shutdown.
 * This design maintains the heap always as full as possible, which generates
 * runs whose size is (i) as large as possible and (ii) independent of the
 * activation behavior.
 *
 * In the typical operation mode, a LogArchiver instance is constructed using
 * the sm_options provided by the user, but for tests and external experiments,
 * it can also be constructed by passing instances of these four components
 * above.
 *
 * A note on processing older log partitions (TODO): Before we implemented the
 * archiver, the log manager would delete a partition once it was eliminated
 * from the list of 8 open partitions. The compiler flag KEEP_LOG_PARTITIONS
 * was used to omit the delete operation, leaving the complete history of the
 * database in the log directory. However, if log archiving is enabled, it
 * should take over the responsibility of deleting old log partitions.
 * Currently, if the flag is not set and the archiver cannot keep up with the
 * growth of the log, partitions would be lost from archiving.
 *
 * \sa LogArchiver::LogConsumer
 * \sa LogArchiver::ArchiverHeap
 * \sa LogArchiver::BlockAssembly
 * \sa LogArchiver::ArchiveDirectory
 *
 * \author Caetano Sauer
 */
class LogArchiver : public thread_wrapper_t {
    friend class ArchiveMerger;
public:
    /** \brief Abstract class used by both reader and writer threads.
     *
     * Encapsulates a file descriptor for the current file being read/written,
     * the offset within that file, an asynchronous buffer for blocks already read
     * or to be written, and the size of such blocks.
     *
     * \author Caetano Sauer
     */
    class BaseThread : public thread_wrapper_t {
    protected:
        AsyncRingBuffer* buf;
        int currentFd;
        off_t pos;
        size_t blockSize;

    public:
        size_t getBlockSize() { return blockSize; }

        BaseThread(AsyncRingBuffer* buf)
            : buf(buf), currentFd(-1), pos(0)
        {
            blockSize = buf->getBlockSize();
        }
    };

    /** \brief Asynchronous reader thread for the recovery log.
     *
     * Similarly to the LogArchiver itself, this thread operates on activation
     * cycles based on given "end LSN" values (see LogArchiver). This thread is
     * controlled by LogArchiver::LogConsumer
     *
     * The recovery log is read one block at a time, and each block is placed
     * on an asynchronous ring buffer (see AsyncRingBuffer). Once the buffer is
     * full, it automatically blocks waiting for a free slot.
     *
     * Once the thread is shutdown, it exits its current/next wait and marks
     * the buffer as "finished", which makes consumers stop waiting for new
     * blocks once the buffer is empty.
     *
     * \author Caetano Sauer
     */
    class ReaderThread : public BaseThread {
    protected:
        uint nextPartition;
        rc_t openPartition();

        bool shutdownFlag;
        ArchiverControl control;
    public:
        virtual void run();

        ReaderThread(AsyncRingBuffer* readbuf, lsn_t startLSN);
        virtual ~ReaderThread() {}

        void shutdown();
        void activate(lsn_t endLSN);

        bool isActive() { return control.activated; }
    };

    // forward declaration
    class ArchiveIndex;

    /** \brief Encapsulates all file and I/O operations on the log archive
     *
     * The directory object serves the following purposes:
     * - Inspecting the existing archive files at startup in order to determine
     *   the last LSN persisted (i.e., from where to resume archiving) and to
     *   delete incomplete or already merged (TODO) files that can result from
     *   a system crash.
     * - Support run generation by providing operations to open a new run,
     *   append blocks of data to the current run, and closing the current run
     *   by renaming its file with the given LSN boundaries.
     * - Support scans by opening files given their LSN boundaries (which are
     *   determined by the archive index), reading arbitrary blocks of data
     *   from them, and closing them.
     * - In the near future, it should also support the new (i.e.,
     *   instant-restore-enabled) asynchronous merge daemon (TODO).
     * - Support auxiliary file-related operations that are used, e.g., in
     *   tests and experiments.  Currently, the only such operation is
     *   parseLSN.
     *
     * \author Caetano Sauer
     */
    class ArchiveDirectory {
    public:
        ArchiveDirectory(const sm_options& options);
        virtual ~ArchiveDirectory();

        struct RunFileStats {
            lsn_t beginLSN;
            lsn_t endLSN;
            unsigned level;
        };

        lsn_t getStartLSN() const { return startLSN; }
        ArchiveIndex* getIndex() { return archIndex; }
        size_t getBlockSize() const { return blockSize; }
        std::string getArchDir() const { return archdir; }
        unsigned getMaxLevel() const { return maxLevel; }

        lsn_t getLastLSN();

        // run generation methods
        rc_t append(char* data, size_t length);
        rc_t closeCurrentRun(lsn_t runEndLSN, unsigned level);

        // run scanning methods
        rc_t openForScan(int& fd, lsn_t runBegin, lsn_t runEnd, unsigned level);
        rc_t readBlock(int fd, char* buf, size_t& offset, size_t readSize = 0);
        rc_t closeScan(int& fd);

        void listFiles(std::vector<std::string>& list, int level = -1);
        void listFileStats(std::list<RunFileStats>& list, int level = -1);
        void deleteAllRuns();

        static bool parseRunFileName(string fname, RunFileStats& fstats);
        static size_t getFileSize(int fd);
    private:
        ArchiveIndex* archIndex;
        std::string archdir;
        lsn_t startLSN;
        int appendFd;
        int mergeFd;
        off_t appendPos;
        size_t blockSize;
        unsigned maxLevel;

        fs::path archpath;

        // closeCurrentRun needs mutual exclusion because it is called by both
        // the writer thread and the archiver thread in processFlushRequest
        pthread_mutex_t mutex;

        fs::path make_run_path(lsn_t begin, lsn_t end, unsigned level = 1) const;
        fs::path make_current_run_path() const;
        rc_t openNewRun();

    public:
        const static string RUN_PREFIX;
        const static string CURR_RUN_FILE;
        const static string CURR_MERGE_FILE;
        const static string run_regex;
        const static string current_regex;
    };

    /** \brief Simple implementation of a (naive) log archive index.
     *
     * No caching and one single mutex for all operations.  When log archiver
     * is initialized, the information of every run is loaded in main memory.
     * This class is still under test and development, so more documentation
     * should be added later (TODO)
     *
     * \author Caetano Sauer
     */
    class ArchiveIndex {
    public:
        ArchiveIndex(size_t blockSize, lsn_t startLSN, size_t bucketSize = 0);
        virtual ~ArchiveIndex();

        struct ProbeResult {
            PageID pidBegin;
            PageID pidEnd;
            lsn_t runBegin;
            lsn_t runEnd;
            unsigned level;
            size_t offset;
            size_t runIndex;
        };

        void init();

        void newBlock(PageID firstPID, unsigned level);
        void newBlock(const vector<pair<PageID, size_t> >& buckets, unsigned level);

        rc_t finishRun(lsn_t first, lsn_t last, int fd, off_t offset, unsigned level);
        void probe(std::vector<ProbeResult>& probes,
                PageID startPID, PageID endPID, lsn_t startLSN);

        rc_t getBlockCounts(int fd, size_t* indexBlocks, size_t* dataBlocks);
        rc_t loadRunInfo(int fd, const ArchiveDirectory::RunFileStats&);
        void appendNewEntry(unsigned level);

        lsn_t getLastLSN(unsigned level);
        lsn_t getFirstLSN(unsigned level);

        size_t getBucketSize() { return bucketSize; }

        void dumpIndex(ostream& out);

    private:
        struct BlockEntry {
            size_t offset;
            PageID pid;
        };
        struct BlockHeader {
            uint32_t entries;
            uint32_t blockNumber;
        };
        struct RunInfo {
            lsn_t firstLSN;
            // lastLSN must be equal to firstLSN of the following run.  We keep
            // it redundantly so that index probes don't have to look beyond
            // the last finished run. We used to keep a global lastLSN field in
            // the index, but there can be a race between the writer thread
            // inserting new runs and probes on the last finished, so it was
            // removed.
            lsn_t lastLSN;

            // Simple min-max filter for page IDs (min is in 1st entry)
            PageID lastPID;

            std::vector<BlockEntry> entries;

            bool operator<(const RunInfo& other) const
            {
                return firstLSN < other.firstLSN;
            }
        };

        pthread_mutex_t mutex;
        size_t blockSize;

        // Run information for each level of the index
        std::vector<std::vector<RunInfo>> runs;

        // Last finished run on each level -- this is required because runs are
        // generated asynchronously, so that a new one may be appended to the
        // index before the last one is finished. Thus, when calling finishRun(),
        // we cannot simply take the last run in the vector.
        std::vector<int> lastFinished;

        /** Whether this index uses variable-sized buckets, i.e., entries in
         * the index refer to fixed ranges of page ID for which the amount of
         * log records is variable. The number gives the size of a bucket in
         * terms of number of page ID's (or segment size in the restore case).
         * If this is zero, then the index behaves like a B-tree, in which a
         * bucket corresponds to a block, therefore having fixed sizes (but
         * variable number of log records, obviously).
         */
        size_t bucketSize;

        unsigned maxLevel;

        size_t findRun(lsn_t lsn, unsigned level);
        void probeInRun(ProbeResult&);
        // binary search
        size_t findEntry(RunInfo* run, PageID pid,
                int from = -1, int to = -1);
        rc_t serializeRunInfo(RunInfo&, int fd, off_t);

    };

    /** \brief Asynchronous writer thread to produce run files on disk
     *
     * Consumes blocks of data produced by the BlockAssembly component and
     * writes them to the corresponding run files on disk. Metadata on each
     * block is used to control to which run each block belongs and what LSN
     * ranges are contained in each run (see BlockAssembly).
     *
     * \author Caetano Sauer
     */
    class WriterThread : public BaseThread {
    private:
        ArchiveDirectory* directory;
        lsn_t maxLSNInRun;
        run_number_t currentRun;
        unsigned level;

    public:
        virtual void run();

        ArchiveDirectory* getDirectory() { return directory; }

        /*
         * Called by processFlushRequest to forcibly start a new run
         */
        void resetCurrentRun()
        {
            currentRun++;
            maxLSNInRun = lsn_t::null;
        }

        WriterThread(AsyncRingBuffer* writebuf, ArchiveDirectory* directory, unsigned level)
            :
              BaseThread(writebuf),
              directory(directory), maxLSNInRun(lsn_t::null), currentRun(0), level(level)
        {
        }

        virtual ~WriterThread() {}
    };

    /** \brief Component that consumes a partially-sorted log record stream and
     * generates indexed runs from it.
     *
     * This class serves two purposes:
     * - It assembles individual log records into blocks which are written to
     *   persistent storage using an asynchronous writer thread (see
     *   WriterThread).
     * - For each block generated, it generates an entry on the archive index,
     *   allowing direct access to each block based on log record attributes
     *   (page id & lsn).
     *
     * The writer thread is controlled solely using an asynchronous ring
     * buffer. This works because the writer should keep writing as long as
     * there are blocks available -- unlike the reader thread, which must stop
     * once a certain LSN is reached.
     *
     * Each generated block contains a <b>header</b>, which specifies the run
     * number, the offset up to which valid log records are found within that
     * block, and the LSN of the last log record in the block. The run number
     * is used by the writer thread to write blocks to the correct run file --
     * once it changes from one block to another, it must close the currently
     * generated run file an open a new one. The LSN in the last block header
     * is then used to rename the file with the correct LSN range. (We used to
     * control these LSN boundaries with an additional queue structure, but it
     * required too many dependencies between modules that are otherwise
     * independent)
     *
     * \author Caetano Sauer
     */
    class BlockAssembly {
    public:
        BlockAssembly(ArchiveDirectory* directory, unsigned level = 1);
        virtual ~BlockAssembly();

        bool start(run_number_t run);
        bool add(logrec_t* lr);
        void finish();
        void shutdown();
        bool hasPendingBlocks();

        void resetWriter()
        {
            writer->resetCurrentRun();
        }

        // methods that abstract block metadata
        static run_number_t getRunFromBlock(const char* b);
        static lsn_t getLSNFromBlock(const char* b);
        static size_t getEndOfBlock(const char* b);
    private:
        char* dest;
        AsyncRingBuffer* writebuf;
        WriterThread* writer;
        ArchiveIndex* archIndex;
        size_t blockSize;
        size_t pos;
        size_t fpos;

        PageID firstPID;
        // PageID lastPID;
        lsn_t maxLSNInBlock;
        int maxLSNLength;
        run_number_t lastRun;

        // if using a variable-bucket index, this is the number of page IDs
        // that will be stored within a bucket (aka restore's segment)
        size_t bucketSize;
        // list of buckets beginning in the current block
        vector<pair<PageID, size_t> > buckets;
        // number of the nex bucket to be indexed
        size_t nextBucket;

        unsigned level;
    public:
        struct BlockHeader {
            lsn_t lsn;
            uint32_t end;
            run_number_t run;
        };

    };

    /** \brief Provides scans over the log archive for restore operations.
     *
     * More documentation to follow, as class is still under test and
     * development (TODO).
     *
     * \author Caetano Sauer
     */
    class ArchiveScanner {
    public:
        ArchiveScanner(ArchiveDirectory*);
        virtual ~ArchiveScanner() {};

        struct RunMerger;

        RunMerger* open(PageID startPID, PageID endPID, lsn_t startLSN,
                size_t readSize);

        void close (RunMerger* merger)
        {
            delete merger;
        }

        struct RunScanner {
            const lsn_t runBegin;
            const lsn_t runEnd;
            const unsigned level;
            const PageID firstPID;
            const PageID lastPID;

            size_t offset;
            char* buffer;
            size_t bpos;
            int fd;
            size_t blockCount;
            size_t bucketSize;
            size_t readSize;

            ArchiveDirectory* directory;
            LogScanner* scanner;

            RunScanner(lsn_t b, lsn_t e, unsigned level, PageID f, PageID l, off_t o,
                    ArchiveDirectory* directory, size_t readSize = 0);
            ~RunScanner();

            bool next(logrec_t*& lr);

            friend std::ostream& operator<< (ostream& os, const RunScanner& m);

        private:
            bool nextBlock();
        };

    private:
        ArchiveDirectory* directory;
        ArchiveIndex* archIndex;

        struct MergeHeapEntry {
            // store pid and lsn here to speed up comparisons
            bool active;
            PageID pid;
            lsn_t lsn;
            logrec_t* lr;
            RunScanner* runScan;

            MergeHeapEntry(RunScanner* runScan);

            // required by w_heap
            MergeHeapEntry() : runScan(NULL) {}

            virtual ~MergeHeapEntry() {}

            void moveToNext();
            PageID lastPIDinBlock();

            friend std::ostream& operator<<(std::ostream& os,
                    const MergeHeapEntry& e)
            {
                os << "[run " << *(e.runScan) << ", " << e.pid << ", " << e.lsn <<
                    " active=" << e.active <<
                    ", logrec=" << e.lr->lsn() << " " << e.lr->type_str()
                    << ")]";
                return os;
            }
        };

        struct MergeHeapCmp {
            bool gt(const MergeHeapEntry& a, const MergeHeapEntry& b) const
            {
                if (!a.active) return false;
                if (!b.active) return true;
                if (a.pid != b.pid) {
                    return a.pid< b.pid;
                }
                return a.lsn < b.lsn;
            }
        };

    public:
        // Scan interface exposed to caller
        struct RunMerger {
            RunMerger()
                : heap(cmp), started(false), endPID(0)
            {}

            virtual ~RunMerger() {}

            void addInput(RunScanner* r);
            bool next(logrec_t*& lr);
            void dumpHeap(ostream& out);
            void close();

            size_t heapSize() { return heap.NumElements(); }
            PageID getEndPID() { return endPID; }

        private:
            MergeHeapCmp cmp;
            Heap<MergeHeapEntry, MergeHeapCmp> heap;
            bool started;
            PageID endPID;
        };
    };

    /** \brief Heap data structure that supports log archive run generation
     *
     * This class encapsulates a heap data structure in a way which is
     * transparent to the replacement-selection logic of the enclosing
     * LogArchiver instance. It contains a heap data structure as well as a
     * memory manager for the variable-length log records.
     *
     * The heap contains instances of HeapEntry, which contains the sort key of
     * log records (run number, page id, lsn) and a pointer to the log record
     * data in the memory manager workspace (slot_t).
     *
     * This class is more than just a heap data structure because it is aware
     * of run boundaries.  Therefore, it can be seen as a replacement-selection
     * module, with the particularity that records are not delayed to future
     * runs, in order to maintain a fixed mapping from regions of the recovery
     * log (i.e., LSN ranges) to runs in the log archive. The goal of such
     * mapping is to facilitate the resume of log archiving after a system
     * failure (see our BTW 2015 paper on Single-Pass Restore for more
     * details).
     *
     * This class participates in the log archiving pipeline in which the input
     * stream of log records coming from the log consumer is fed into the heap
     * by invoking push() for each log record. This is analogous to the
     * replacement step of the sorting algorithm. On the other side, the
     * selection step pops log records out of the heap and feeds them to the
     * BlockAssembly component.
     *
     * \author Caetano Sauer
     */
    class ArchiverHeap {
    public:
        ArchiverHeap(size_t workspaceSize);
        virtual ~ArchiverHeap();

        bool push(logrec_t* lr, bool duplicate);
        logrec_t* top();
        void pop();

        run_number_t topRun() { return w_heap.First().run; }
        size_t size() { return w_heap.NumElements(); }
    private:
        run_number_t currentRun;
        bool filledFirst;
        mem_mgmt_t* workspace;

        mem_mgmt_t::slot_t allocate(size_t length);

        struct HeapEntry {
            mem_mgmt_t::slot_t slot;
            lsn_t lsn;
            run_number_t run;
            PageID pid;

            HeapEntry(run_number_t run, PageID pid, lsn_t lsn,
                    mem_mgmt_t::slot_t slot)
                : slot(slot), lsn(lsn), run(run), pid(pid)
            {}

            HeapEntry()
                : slot(NULL, 0), lsn(lsn_t::null), run(0), pid(0)
            {}

            friend std::ostream& operator<<(std::ostream& os,
                    const HeapEntry& e)
            {
                os << "[run " << e.run << ", " << e.pid << ", " << e.lsn <<
                    ", slot(" << e.slot.address << ", " << e.slot.length << ")]";
                return os;
            }
        };

        struct Cmp {
            bool gt(const HeapEntry& a, const HeapEntry& b) const;
        };

        Cmp heapCmp;
        Heap<HeapEntry, Cmp> w_heap;
    };

    /** \brief Provides a record-at-a-time interface to the recovery log using
     * asynchronous read operations.
     *
     * This class manages an asynchronous reader thread (see ReaderThread) and
     * the corresponding read buffer (see AsyncRingBuffer). It provides a
     * record-at-a-time synchronous interface to the caller. It is used to read
     * log records from the recovery log and push them into the archiver heap.
     *
     * Access requires a preliminary call to the open() method, which activates
     * the reader thread with the given end LSN. Otherwise, the next() method
     * bay block indefinitely.
     *
     * next() returns false when it reaches the end LSN, implying that the
     * returned log record is invalid.
     *
     * \author Caetano Sauer
     */
    class LogConsumer {
    public:
        LogConsumer(lsn_t startLSN, size_t blockSize, bool ignore = true);
        virtual ~LogConsumer();
        void shutdown();

        void open(lsn_t endLSN, bool readWholeBlocks = DFT_READ_WHOLE_BLOCKS);
        bool next(logrec_t*& lr);
        lsn_t getNextLSN() { return nextLSN; }

    private:
        AsyncRingBuffer* readbuf;
        ReaderThread* reader;
        LogScanner* logScanner;

        lsn_t nextLSN;
        lsn_t endLSN;

        char* currentBlock;
        size_t blockSize;
        size_t pos;
        bool readWholeBlocks;

        bool nextBlock();
    };

    /**
     * Basic service to merge existing log archive runs into larger ones.
     * Currently, the merge logic only supports the *very limited* use case of
     * merging all N run files into a smaller n, depending on a given fan-in
     * and size limits. Currently, it is used simply to run our restore
     * experiments with different number of runs for the same log archive
     * volume.
     *
     * In a proper implementation, we have to support useful policies, with the
     * restriction that only consecutive runs can be merged. The biggest
     * limitation right now is that we reuse the logic of BlockAssembly, but
     * its control logic -- especially the coordination with the WriterThread
     * -- is quite restricted to the usual case of a consumption of log records
     * from the standard recovery log, i.e., ascending LSNs and run numbers,
     * startLSN coming from the existing run files, etc. We have to make that
     * logic clever and more abstract; or simply don't reuse the BlockAssembly
     * infrastructure.
     */
    class MergerDaemon {
    public:
        MergerDaemon(ArchiveDirectory* in, ArchiveDirectory* out);
        virtual ~MergerDaemon()
        {}

        rc_t runSync(unsigned level, unsigned fanin);
        rc_t join(bool terminate);

    private:
        ArchiveDirectory* indir;
        ArchiveDirectory* outdir;
        rc_t asyncRC;

    };

public:
    LogArchiver(const sm_options& options);
    LogArchiver(
            ArchiveDirectory*,
            LogConsumer*,
            ArchiverHeap*,
            BlockAssembly*
    );

    virtual ~LogArchiver();

    virtual void run();
    void activate(lsn_t endLSN = lsn_t::null, bool wait = true);
    void shutdown();
    bool requestFlushAsync(lsn_t);
    void requestFlushSync(lsn_t);
    void archiveUntilLSN(lsn_t);

    ArchiveDirectory* getDirectory() { return directory; }
    lsn_t getNextConsumedLSN() { return consumer->getNextLSN(); }
    void setEager(bool e)
    {
        eager = e;
        lintel::atomic_thread_fence(lintel::memory_order_release);
    }

    bool getEager() const { return eager; }

    static void initLogScanner(LogScanner* logScanner);

    /*
     * IMPORTANT: the block size must be a multiple of the log
     * page size to ensure that logrec headers are not truncated
     */
    const static int DFT_BLOCK_SIZE = 1024 * 1024; // 1MB = 128 pages
    const static int DFT_WSPACE_SIZE= 100; // 100MB
    const static bool DFT_EAGER = true;
    const static bool DFT_READ_WHOLE_BLOCKS = true;
    const static int DFT_GRACE_PERIOD = 1000000; // 1 sec

    const static int IO_BLOCK_COUNT = 8; // total buffer = 8MB
    const static size_t IO_ALIGN;

private:
    ArchiveDirectory* directory;
    LogConsumer* consumer;
    ArchiverHeap* heap;
    BlockAssembly* blkAssemb;

    bool shutdownFlag;
    ArchiverControl control;
    bool selfManaged;
    bool eager;
    bool readWholeBlocks;
    int slowLogGracePeriod;
    lsn_t nextActLSN;
    lsn_t flushReqLSN;

    void replacement();
    bool selection();
    void pushIntoHeap(logrec_t*, bool duplicate);
    bool waitForActivation();
    bool processFlushRequest();
    bool isLogTooSlow();
    bool shouldActivate(bool logTooSlow);

};

/** \brief Parses log records from a stream of binary data.
 *
 * This class is not a scanner per se, as it does not perform any I/O.
 * However, it enables efficient scanning by reading whole blocks of binary
 * data from the recovery log and parsing log records from them using this
 * class. This is an improvement over the traditional "scan" implemented in
 * Shore-MT, which performs one random read for each log record.  The major
 * task implemented in this class is the control of block boundaries, which can
 * occur in the middle of a log record (i.e., log records may spawn multiple
 * blocks). To that end, it maintains an internal log record buffer to
 * reconstruct such log records.
 *
 * Log records are delivered via the nextLogrec() method, which takes the block
 * address and offset within the block as parameters. Once the log record is
 * parsed, the offset is updated (i.e., it is an output parameter). The method
 * returns true if the log record was found entirely in the current block.
 * Otherwise, it saves the partial data into its internal buffer and returns
 * false, indicating to the caller that a new block must be provided. Upon
 * invoking nextLogrec() once again, the caller then receives the complete log
 * record.
 *
 * \author Caetano Sauer
 */
class LogScanner {
public:
    bool nextLogrec(char* src, size_t& pos, logrec_t*& lr,
            lsn_t* nextLSN = NULL, lsn_t* stopLSN = NULL,
            int* lrLength = NULL);

    bool hasPartialLogrec();
    void reset();

    LogScanner(size_t blockSize)
        : truncCopied(0), truncMissing(0), toSkip(0), blockSize(blockSize)
    {
        // maximum logrec size = 3 pages
        truncBuf = new char[3 * log_storage::BLOCK_SIZE];
    }

    ~LogScanner() {
        delete[] truncBuf;
    }

    size_t getBlockSize() {
        return blockSize;
    }

    void setIgnore(logrec_t::kind_t type) {
        ignore.set(type);
    }

    void ignoreAll() {
        ignore.set();
    }

    void unsetIgnore(logrec_t::kind_t type) {
        ignore.reset(type);
    }

    bool isIgnored(logrec_t::kind_t type) {
        return ignore[type];
    }

private:
    size_t truncCopied;
    size_t truncMissing;
    size_t toSkip;
    const size_t blockSize;
    char* truncBuf;
    bitset<logrec_t::t_max_logrec> ignore;
};

#endif
