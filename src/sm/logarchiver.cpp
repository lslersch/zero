#include "w_defines.h"

#define SM_SOURCE
#define LOGARCHIVER_C

#include "logarchiver.h"
#include "sm_options.h"
#include "log_core.h"

#include <algorithm>
#include <sm_base.h>
#include <sstream>
#include <sys/stat.h>
#include <boost/regex.hpp>

#include "stopwatch.h"

// files and stuff
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

// TODO proper exception mechanism
#define CHECK_ERRNO(n) \
    if (n == -1) { \
        W_FATAL_MSG(fcOS, << "Kernel errno code: " << errno); \
    }

typedef mem_mgmt_t::slot_t slot_t;

const static int DFT_BLOCK_SIZE = 1024 * 1024; // 1MB = 128 pages
const static int IO_BLOCK_COUNT = 8; // total buffer = 8MB


// definition of static members
const string ArchiveDirectory::RUN_PREFIX = "archive_";
const string ArchiveDirectory::CURR_RUN_FILE = "current_run";
const string ArchiveDirectory::CURR_MERGE_FILE = "current_merge";
const string ArchiveDirectory::run_regex =
    "^archive_([1-9][0-9]*)_([1-9][0-9]*\\.[0-9]+)-([1-9][0-9]*\\.[0-9]+)$";
const string ArchiveDirectory::current_regex = "current_run|current_merge";

// CS: Aligning with the Linux standard FS block size
// We could try using 512 (typical hard drive sector) at some point,
// but none of this is actually standardized or portable
const size_t IO_ALIGN = 512;

baseLogHeader SKIP_LOGREC;

typedef ArchiveIndex::ProbeResult ProbeResult;

ArchiverControl::ArchiverControl(bool* shutdownFlag)
    : endLSN(lsn_t::null), activated(false), listening(false), shutdownFlag(shutdownFlag)
{
    DO_PTHREAD(pthread_mutex_init(&mutex, NULL));
    DO_PTHREAD(pthread_cond_init(&activateCond, NULL));
}

ArchiverControl::~ArchiverControl()
{
    DO_PTHREAD(pthread_mutex_destroy(&mutex));
    DO_PTHREAD(pthread_cond_destroy(&activateCond));
}

bool ArchiverControl::activate(bool wait, lsn_t lsn)
{
    if (wait) {
        DO_PTHREAD(pthread_mutex_lock(&mutex));
    }
    else {
        if (pthread_mutex_trylock(&mutex) != 0) {
            return false;
        }
    }
    // now we hold the mutex -- signal archiver thread and set endLSN

    /* Make sure signal is sent only if thread is listening.
     * TODO: BUG? The mutex alone cannot guarantee that the signal is not lost,
     * since the activate call may happen before the thread ever starts
     * listening. If we ever get problems with archiver getting stuck, this
     * would be one of the first things to try. We could, e.g., replace
     * the listening flag with something like "gotSignal" and loop this
     * method until it's true.
     */
    // activation may not decrease the endLSN
    w_assert0(lsn >= endLSN);
    endLSN = lsn;
    activated = true;
    DO_PTHREAD(pthread_cond_signal(&activateCond));
    DO_PTHREAD(pthread_mutex_unlock(&mutex));

    /*
     * Returning true only indicates that signal was sent, and not that the
     * archiver thread is running with the given endLSN. Another thread
     * calling activate may get the mutex before the log archiver and set
     * another endLSN. In fact, it does not even mean that the signal was
     * received, since the thread may not be listening yet.
     */
    return activated;
}

bool ArchiverControl::waitForActivation()
{
    // WARNING: mutex must be held by caller!
    listening = true;
    while(!activated) {
        struct timespec timeout;
        smthread_t::timeout_to_timespec(100, timeout); // 100ms
        int code = pthread_cond_timedwait(&activateCond, &mutex, &timeout);
        if (code == ETIMEDOUT) {
            //DBGTHRD(<< "Wait timed out -- try again");
            if (*shutdownFlag) {
                DBGTHRD(<< "Activation failed due to shutdown. Exiting");
                return false;
            }
        }
        DO_PTHREAD_TIMED(code);
    }
    listening = false;
    return true;
}

ReaderThread::ReaderThread(AsyncRingBuffer* readbuf,
        lsn_t startLSN)
    :
      BaseThread(readbuf), shutdownFlag(false), control(&shutdownFlag)
{
    // position initialized to startLSN
    pos = startLSN.lo();
    nextPartition = startLSN.hi();
}

void ReaderThread::shutdown()
{
    shutdownFlag = true;
    // make other threads see new shutdown value
    lintel::atomic_thread_fence(lintel::memory_order_release);
}

void ReaderThread::activate(lsn_t endLSN)
{
    // pos = startLSN.lo();
    DBGTHRD(<< "Activating reader thread until " << endLSN);
    control.activate(true, endLSN);
}

rc_t ReaderThread::openPartition()
{
    if (currentFd != -1) {
        auto ret = ::close(currentFd);
        CHECK_ERRNO(ret);
    }
    currentFd = -1;

    // open file for read -- copied from partition_t::peek()
    int fd;
    string fname = smlevel_0::log->make_log_name(nextPartition);

    int flags = O_RDONLY;
    fd = ::open(fname.c_str(), flags, 0744 /*mode*/);
    CHECK_ERRNO(fd);

    struct stat stat;
    auto ret = ::fstat(fd, &stat);
    CHECK_ERRNO(ret);
    if (stat.st_size == 0) { return RC(eEOF); }
    off_t partSize = stat.st_size;

    /*
     * The size of the file must be at least the offset of endLSN, otherwise
     * the given endLSN was incorrect. If this is not the partition of
     * endLSN.hi(), then we simply assert that its size is not zero.
     */
    if (control.endLSN.hi() == nextPartition) {
        w_assert0(partSize >= control.endLSN.lo());
    }
    else {
        w_assert1(partSize > 0);
    }

    DBGTHRD(<< "Opened log partition for read " << fname);

    currentFd = fd;
    nextPartition++;
    return RCOK;
}

void ReaderThread::run()
{
    while(true) {
        CRITICAL_SECTION(cs, control.mutex);

        bool activated = control.waitForActivation();
        if (!activated) {
            break;
        }

        lintel::atomic_thread_fence(lintel::memory_order_release);
        if (shutdownFlag) {
            break;
        }

        DBGTHRD(<< "Reader thread activated until " << control.endLSN);

        /*
         * CS: The code was changed to not rely on the file size anymore,
         * because we may read from a file that is still being appended to.
         * The correct behavior is to rely on the given endLSN, which must
         * be guaranteed to be persistent on the file. Therefore, we cannot
         * read past the end of the file if we only read until endLSN.
         * A physical read past the end is OK because we use pread_short().
         * The position from which the first logrec will be read is set in pos
         * by the activate method, which takes the startLSN as parameter.
         */

        while(true) {
            unsigned currPartition =
                currentFd == -1 ? nextPartition : nextPartition - 1;
            if (control.endLSN.hi() == currPartition
                    && pos >= control.endLSN.lo())
            {
                /*
                 * The requested endLSN is within a block which was already
                 * read. Stop and wait for next activation, which must start
                 * reading from endLSN, since anything beyond that might
                 * have been updated alread (usually, endLSN is the current
                 * end of log). Hence, we update pos with it.
                 */
                pos = control.endLSN.lo();
                DBGTHRD(<< "Reader thread reached endLSN -- sleeping."
                        << " New pos = " << pos);
                break;
            }

            // get buffer space to read into
            char* dest = buf->producerRequest();
            if (!dest) {
                W_FATAL_MSG(fcINTERNAL,
                     << "Error requesting block on reader thread");
                break;
            }


            if (currentFd == -1) {
                W_COERCE(openPartition());
            }

            // Read only the portion which was ignored on the last round
            size_t blockPos = pos % blockSize;
            int bytesRead = ::pread(currentFd, dest + blockPos, blockSize - blockPos, pos);
            CHECK_ERRNO(bytesRead);

            if (bytesRead == 0) {
                // Reached EOF -- open new file and try again
                DBGTHRD(<< "Reader reached EOF (bytesRead = 0)");
                W_COERCE(openPartition());
                pos = 0;
                blockPos = 0;
                bytesRead = ::pread(currentFd, dest, blockSize, pos);
                CHECK_ERRNO(bytesRead);
                if (bytesRead == 0) {
                    W_FATAL_MSG(fcINTERNAL,
                        << "Error reading from partition "
                        << nextPartition - 1);
                }
            }

            DBGTHRD(<< "Read block " << (void*) dest << " from fpos " << pos <<
                    " with size " << bytesRead << " into blockPos "
                    << blockPos);
            w_assert0(bytesRead > 0);

            pos += bytesRead;
            buf->producerRelease();
        }

        control.activated = false;
    }
}

void WriterThread::run()
{
    DBGTHRD(<< "Writer thread activated");

    while(true) {
        char* src = buf->consumerRequest();
        if (!src) {
            /* Is the finished flag necessary? Yes.
             * The reader thread stops once it reaches endLSN, and then it
             * sleeps and waits for the next activate signal. The writer
             * thread, on the other hand, does not need an activation signal,
             * because it runs indefinitely, just waiting for blocks to be
             * written. The only stop condition is when the write buffer itself
             * is marked finished, which is done in shutdown().
             * Nevertheless, a null block is only returned once the finished
             * flag is set AND there are no more blocks. Thus, we gaurantee
             * that all pending blocks are written out before shutdown.
             */
            DBGTHRD(<< "Finished flag set on writer thread");
            W_COERCE(directory->closeCurrentRun(maxLSNInRun, level));
            return; // finished is set on buf
        }

        DBGTHRD(<< "Picked block for write " << (void*) src);

        run_number_t run = BlockAssembly::getRunFromBlock(src);
        if (currentRun != run) {
            // when writer is restarted, currentRun resets to zero
            w_assert1(currentRun == 0 || run == currentRun + 1);
            /*
             * Selection (producer) guarantees that logrec fits in block.
             * lastLSN is the LSN of the first log record in the new block
             * -- it will be used as the upper bound when renaming the file
             *  of the current run. This same LSN will be used as lower
             *  bound on the next run, which allows us to verify whether
             *  holes exist in the archive.
             */
            W_COERCE(directory->closeCurrentRun(maxLSNInRun, level));
            w_assert1(directory->getLastLSN() == maxLSNInRun);
            currentRun = run;
            maxLSNInRun = lsn_t::null;
            DBGTHRD(<< "Opening file for new run " << run
                    << " starting on LSN " << directory->getLastLSN());
        }

        lsn_t blockLSN = BlockAssembly::getLSNFromBlock(src);
        if (blockLSN > maxLSNInRun) {
            maxLSNInRun = blockLSN;
        }

        size_t blockEnd = BlockAssembly::getEndOfBlock(src);
        size_t actualBlockSize= blockEnd - sizeof(BlockAssembly::BlockHeader);
        memmove(src, src + sizeof(BlockAssembly::BlockHeader), actualBlockSize);

        W_COERCE(directory->append(src, actualBlockSize, level));

        DBGTHRD(<< "Wrote out block " << (void*) src
                << " with max LSN " << blockLSN);

        buf->consumerRelease();
    }
}

LogArchiver::LogArchiver(
        ArchiveDirectory* d, LogConsumer* c, ArchiverHeap* h, BlockAssembly* b)
    :
    directory(d), consumer(c), heap(h), blkAssemb(b),
    shutdownFlag(false), control(&shutdownFlag), selfManaged(false),
    flushReqLSN(lsn_t::null)
{
    nextActLSN = directory->getStartLSN();
}

LogArchiver::LogArchiver(const sm_options& options)
    :
    shutdownFlag(false), control(&shutdownFlag), selfManaged(true),
    flushReqLSN(lsn_t::null)
{
    size_t workspaceSize = 1024 * 1024 * // convert MB -> B
        options.get_int_option("sm_archiver_workspace_size", DFT_WSPACE_SIZE);
    size_t blockSize = DFT_BLOCK_SIZE;
    // CS TODO: archiver currently only works with 1MB blocks
        // options.get_int_option("sm_archiver_block_size", DFT_BLOCK_SIZE);

    eager = options.get_bool_option("sm_archiver_eager", DFT_EAGER);
    readWholeBlocks = options.get_bool_option(
            "sm_archiver_read_whole_blocks", DFT_READ_WHOLE_BLOCKS);
    slowLogGracePeriod = options.get_int_option(
            "sm_archiver_slow_log_grace_period", DFT_GRACE_PERIOD);

    directory = new ArchiveDirectory(options);
    nextActLSN = directory->getStartLSN();

    consumer = new LogConsumer(directory->getStartLSN(), blockSize);
    heap = new ArchiverHeap(workspaceSize);
    blkAssemb = new BlockAssembly(directory);
}

void LogArchiver::initLogScanner(LogScanner* logScanner)
{
    logScanner->setIgnore(logrec_t::t_comment);
    logScanner->setIgnore(logrec_t::t_compensate);
    logScanner->setIgnore(logrec_t::t_chkpt_begin);
    logScanner->setIgnore(logrec_t::t_chkpt_bf_tab);
    logScanner->setIgnore(logrec_t::t_chkpt_xct_tab);
    logScanner->setIgnore(logrec_t::t_chkpt_xct_lock);
    logScanner->setIgnore(logrec_t::t_chkpt_backup_tab);
    logScanner->setIgnore(logrec_t::t_chkpt_end);
    logScanner->setIgnore(logrec_t::t_chkpt_restore_tab);
    logScanner->setIgnore(logrec_t::t_xct_abort);
    logScanner->setIgnore(logrec_t::t_xct_end);
    logScanner->setIgnore(logrec_t::t_xct_freeing_space);
    logScanner->setIgnore(logrec_t::t_restore_begin);
    logScanner->setIgnore(logrec_t::t_restore_segment);
    logScanner->setIgnore(logrec_t::t_restore_end);
    logScanner->setIgnore(logrec_t::t_tick_sec);
    logScanner->setIgnore(logrec_t::t_tick_msec);
    logScanner->setIgnore(logrec_t::t_page_read);
    logScanner->setIgnore(logrec_t::t_page_write);
}

/*
 * Shutdown sets the finished flag on read and write buffers, which makes
 * the reader and writer threads finish processing the current block and
 * then exit. The replacement-selection will exit as soon as it requests
 * a block and receives a null pointer. If the shutdown flag is set, the
 * method exits without error.
 *
 * Thread safety: since all we do with the shutdown flag is set it to true,
 * we do not worry about race conditions. A memory barrier is also not
 * required, because other threads don't have to immediately see that
 * the flag was set. As long as it is eventually set, it is OK.
 */
void LogArchiver::shutdown()
{
    // CS TODO BUG: we need some sort of pin mechanism (e.g., shared_ptr) for shutdown,
    // because threads may still be accessing the log archive here.
    DBGTHRD(<< "LOG ARCHIVER SHUTDOWN STARTING");
    // this flag indicates that reader and writer threads delivering null
    // blocks is not an error, but a termination condition
    shutdownFlag = true;
    // make other threads see new shutdown value
    lintel::atomic_thread_fence(lintel::memory_order_release);
    join();
    consumer->shutdown();
    blkAssemb->shutdown();
}

LogArchiver::~LogArchiver()
{
    if (!shutdownFlag) {
        shutdown();
    }
    if (selfManaged) {
        delete blkAssemb;
        delete consumer;
        delete heap;
        delete directory;
    }
}

bool ArchiveDirectory::parseRunFileName(string fname, RunFileStats& fstats)
{
    boost::regex run_rx(run_regex, boost::regex::perl);
    boost::smatch res;
    if (!boost::regex_match(fname, res, run_rx)) { return false; }

    fstats.level = std::stoi(res[1]);

    std::stringstream is;
    is.str(res[2]);
    is >> fstats.beginLSN;
    is.clear();
    is.str(res[3]);
    is >> fstats.endLSN;

    return true;
}

lsn_t ArchiveDirectory::getLastLSN()
{
    // CS TODO index mandatory
    w_assert0(archIndex);
    return archIndex->getLastLSN(1 /* level */);
}

size_t ArchiveDirectory::getFileSize(int fd)
{
    struct stat stat;
    auto ret = ::fstat(fd, &stat);
    CHECK_ERRNO(ret);
    return stat.st_size;
}

ArchiveDirectory::ArchiveDirectory(const sm_options& options)
{
    archdir = options.get_string_option("sm_archdir", "archive");
    // CS TODO: archiver currently only works with 1MB blocks
    blockSize = DFT_BLOCK_SIZE;
        // options.get_int_option("sm_archiver_block_size", DFT_BLOCK_SIZE);
    size_t bucketSize =
        options.get_int_option("sm_archiver_bucket_size", 128);
    w_assert0(bucketSize > 0);

    bool reformat = options.get_bool_option("sm_format", false);

    if (archdir.empty()) {
        W_FATAL_MSG(fcINTERNAL,
                << "Option for archive directory must be specified");
    }

    if (!fs::exists(archdir)) {
        if (reformat) {
            fs::create_directories(archdir);
        } else {
            cerr << "Error: could not open the log directory " << archdir <<endl;
            W_COERCE(RC(eOS));
        }
    }

    maxLevel = 0;
    archpath = archdir;
    fs::directory_iterator it(archpath), eod;
    boost::regex current_rx(current_regex, boost::regex::perl);
    lsn_t highestLSN = lsn_t::null;

    for (; it != eod; it++) {
        fs::path fpath = it->path();
        string fname = fpath.filename().string();
        RunFileStats fstats;

        if (parseRunFileName(fname, fstats)) {
            if (reformat) {
                fs::remove(fpath);
                continue;
            }
            // parse lsn from file name
            lsn_t currLSN = fstats.endLSN;
            if (currLSN > highestLSN) {
                DBGTHRD("Highest LSN found so far in archdir: " << currLSN);
                highestLSN = currLSN;
            }
            if (fstats.level > maxLevel) { maxLevel = fstats.level; }
        }
        else if (boost::regex_match(fname, current_rx)) {
            DBGTHRD(<< "Found unfinished log archive run. Deleting");
            fs::remove(fpath);
        }
        else {
            cerr << "ArchiveDirectory cannot parse filename " << fname << endl;
            W_FATAL(fcINTERNAL);
        }
    }
    startLSN = highestLSN;

    // no runs found in archive log -- start from first available log file
    if (startLSN.hi() == 0 && smlevel_0::log) {
        int nextPartition = startLSN.hi();

        int max = smlevel_0::log->durable_lsn().hi();

        while (nextPartition <= max) {
            string fname = smlevel_0::log->make_log_name(nextPartition);
            if (fs::exists(fname)) { break; }
            nextPartition++;
        }

        if (nextPartition > max) {
            W_FATAL_MSG(fcINTERNAL,
                << "Could not find partition files in log manager");
        }

        startLSN = lsn_t(nextPartition, 0);
    }

    // nothing worked -- start from 1.0 and hope for the best
    if (startLSN.hi() == 0) {
        startLSN = lsn_t(1,0);
    }

    // create/load index
    archIndex = new ArchiveIndex(blockSize, bucketSize);

    {
        int fd;
        std::list<RunFileStats> runFiles;
        listFileStats(runFiles);
        for(auto f : runFiles) {
            W_COERCE(openForScan(fd, f.beginLSN, f.endLSN, f.level));
            W_COERCE(archIndex->loadRunInfo(fd, f));
            W_COERCE(closeScan(fd));
        }

        // sort runinfo vector by lsn
        if (runFiles.size() > 0) {
            archIndex->init();
        }
    }

    // CS TODO this should be initialized statically, but whatever...
    memset(&SKIP_LOGREC, 0, sizeof(baseLogHeader));
    SKIP_LOGREC._len = sizeof(baseLogHeader);
    SKIP_LOGREC._type = logrec_t::t_skip;
    SKIP_LOGREC._cat = 1; // t_status is protected...

    DO_PTHREAD(pthread_mutex_init(&mutex, NULL));

    // ArchiveDirectory invariant is that current_run file always exists
    openNewRun(1);
}

ArchiveDirectory::~ArchiveDirectory()
{
    if(archIndex) {
        delete archIndex;
    }
    DO_PTHREAD(pthread_mutex_destroy(&mutex));
}

void ArchiveDirectory::listFiles(std::vector<std::string>& list,
        int level)
{
    list.clear();

    // CS TODO unify with listFileStats
    fs::directory_iterator it(archpath), eod;
    for (; it != eod; it++) {
        string fname = it->path().filename().string();
        RunFileStats fstats;
        if (parseRunFileName(fname, fstats)) {
            if (level < 0 || level == static_cast<int>(fstats.level)) {
                list.push_back(fname);
            }
        }
    }
}

void ArchiveDirectory::listFileStats(list<RunFileStats>& list,
        int level)
{
    list.clear();
    if (level > static_cast<int>(maxLevel)) { return; }

    vector<string> fnames;
    listFiles(fnames, level);

    RunFileStats stats;
    for (size_t i = 0; i < fnames.size(); i++) {
        parseRunFileName(fnames[i], stats);
        list.push_back(stats);
    }
}

/**
 * Opens a new run file of the log archive, closing the current run
 * if it exists. Upon closing, the file is renamed to contain the LSN
 * range of the log records contained in that run. The upper boundary
 * (lastLSN) is exclusive, meaning that it will be found on the beginning
 * of the following run. This also allows checking the filenames for any
 * any range of the LSNs which was "lost" when archiving.
 *
 * We assume the rename operation is atomic, even in case of OS crashes.
 *
 */
rc_t ArchiveDirectory::openNewRun(unsigned level)
{
    if (appendFd.size() > level && appendFd[level] >= 0) {
        return RC(fcINTERNAL);
    }

    int flags = O_WRONLY | O_SYNC | O_CREAT;
    std::string fname = archdir + "/" + CURR_RUN_FILE;
    auto fd = ::open(fname.c_str(), flags, 0744 /*mode*/);
    CHECK_ERRNO(fd);
    DBGTHRD(<< "Opened new output run in level " << level);

    appendFd.resize(level+1, -1);
    appendFd[level] = fd;
    appendPos.resize(level+1, 0);
    appendPos[level] = 0;
    return RCOK;
}

fs::path ArchiveDirectory::make_run_path(lsn_t begin, lsn_t end, unsigned level)
    const
{
    return archpath / fs::path(RUN_PREFIX + std::to_string(level) + "_" + begin.str()
            + "-" + end.str());
}

fs::path ArchiveDirectory::make_current_run_path() const
{
    return archpath / fs::path(CURR_RUN_FILE);
}

rc_t ArchiveDirectory::closeCurrentRun(lsn_t runEndLSN, unsigned level)
{
    CRITICAL_SECTION(cs, mutex);

    if (appendFd[level] >= 0) {
        if (appendPos[level] == 0 && runEndLSN == lsn_t::null) {
            // nothing was appended -- just close file and return
            auto ret = ::close(appendFd[level]);
            CHECK_ERRNO(ret);
            appendFd[level] = -1;
            return RCOK;
        }

        // CS TODO from now on, archiveIndex is mandatory
        // CS TODO unify ArchiveDirectory and ArchiveIndex
        w_assert0(archIndex);
        lsn_t lastLSN = archIndex->getLastLSN(level);
        if (lastLSN != runEndLSN) {
            // register index information and write it on end of file
            if (archIndex && appendPos[level] > 0) {
                // take into account space for skip log record
                appendPos[level] += sizeof(baseLogHeader);
                // and make sure data is written aligned to block boundary
                appendPos[level] -= appendPos[level] % blockSize;
                appendPos[level] += blockSize;
                archIndex->finishRun(lastLSN, runEndLSN, appendFd[level], appendPos[level], level);
            }

            fs::path new_path = make_run_path(lastLSN, runEndLSN, level);
            fs::rename(make_current_run_path(), new_path);

            DBGTHRD(<< "Closing current output run: " << new_path.string());
        }

        auto ret = ::close(appendFd[level]);
        CHECK_ERRNO(ret);
        appendFd[level] = -1;
    }

    openNewRun(level);

    return RCOK;
}

rc_t ArchiveDirectory::append(char* data, size_t length, unsigned level)
{
    // make sure there is always a skip log record at the end
    w_assert1(length + sizeof(baseLogHeader) <= blockSize);
    memcpy(data + length, &SKIP_LOGREC, sizeof(baseLogHeader));

    // beginning of block must be a valid log record
    w_assert1(reinterpret_cast<logrec_t*>(data)->valid_header());

    INC_TSTAT(la_block_writes);
    auto ret = ::pwrite(appendFd[level], data, length + sizeof(baseLogHeader),
                appendPos[level]);
    CHECK_ERRNO(ret);
    appendPos[level] += length;
    return RCOK;
}

rc_t ArchiveDirectory::openForScan(int& fd, lsn_t runBegin,
        lsn_t runEnd, unsigned level)
{
    fs::path fpath = make_run_path(runBegin, runEnd, level);

    // Using direct I/O
    int flags = O_RDONLY | O_DIRECT;
    fd = ::open(fpath.string().c_str(), flags, 0744 /*mode*/);
    CHECK_ERRNO(fd);

    return RCOK;
}

/** Note: buffer must be allocated for at least readSize + IO_ALIGN bytes,
 * otherwise direct I/O with alignment will corrupt memory.
 */
rc_t ArchiveDirectory::readBlock(int fd, char* buf,
        size_t& offset, size_t readSize)
{
    stopwatch_t timer;

    if (readSize == 0) { readSize = blockSize; }
    size_t actualOffset = IO_ALIGN * (offset / IO_ALIGN);
    size_t diff = offset - actualOffset;
    // make sure we don't read more than a block worth of data
    w_assert1(actualOffset <= offset);
    w_assert1(offset % blockSize != 0 || readSize == blockSize);
    w_assert1(diff < IO_ALIGN);

    size_t actualReadSize = readSize + diff;
    if (actualReadSize % IO_ALIGN != 0) {
        actualReadSize = (1 + actualReadSize / IO_ALIGN) * IO_ALIGN;
    }

    int howMuchRead = ::pread(fd, buf, actualReadSize, actualOffset);
    CHECK_ERRNO(howMuchRead);
    if (howMuchRead == 0) {
        // EOF is signalized by setting offset to zero
        offset = 0;
        return RCOK;
    }

    if (diff > 0) {
        memmove(buf, buf + diff, readSize);
    }

    ADD_TSTAT(la_read_time, timer.time_us());
    ADD_TSTAT(la_read_volume, howMuchRead);
    INC_TSTAT(la_read_count);

    offset += readSize;
    return RCOK;
}

rc_t ArchiveDirectory::closeScan(int& fd)
{
    auto ret = ::close(fd);
    CHECK_ERRNO(ret);
    fd = -1;
    return RCOK;
}

LogConsumer::LogConsumer(lsn_t startLSN, size_t blockSize, bool ignore)
    : nextLSN(startLSN), endLSN(lsn_t::null), currentBlock(NULL),
    blockSize(blockSize)
{
    DBGTHRD(<< "Starting log archiver at LSN " << nextLSN);

    // pos must be set to the correct offset within a block
    pos = startLSN.lo() % blockSize;

    readbuf = new AsyncRingBuffer(blockSize, IO_BLOCK_COUNT);
    reader = new ReaderThread(readbuf, startLSN);
    logScanner = new LogScanner(blockSize);

    if(ignore) {
        LogArchiver::initLogScanner(logScanner);
    }
    reader->fork();
}

LogConsumer::~LogConsumer()
{
    if (!readbuf->isFinished()) {
        shutdown();
    }
    delete reader;
    delete readbuf;
}

void LogConsumer::shutdown()
{
    if (!readbuf->isFinished()) {
        readbuf->set_finished();
        reader->shutdown();
        reader->join();
    }
}

void LogConsumer::open(lsn_t endLSN, bool readWholeBlocks)
{
    this->endLSN = endLSN;
    this->readWholeBlocks = readWholeBlocks;

    reader->activate(endLSN);

    nextBlock();
}

bool LogConsumer::nextBlock()
{
    if (currentBlock) {
        readbuf->consumerRelease();
        DBGTHRD(<< "Released block for replacement " << (void*) currentBlock);
        currentBlock = NULL;
    }

    // get a block from the reader thread
    currentBlock = readbuf->consumerRequest();
    if (!currentBlock) {
        if (!readbuf->isFinished()) {
            // This happens if log scanner finds a skip logrec, but
            // then the next partition does not exist. This would be a bug,
            // because endLSN should always be an existing LSN, or one
            // immediately after an existing LSN but in the same partition.
            W_FATAL_MSG(fcINTERNAL, << "Consume request failed!");
        }
        return false;
    }
    DBGTHRD(<< "Picked block for replacement " << (void*) currentBlock);
    if (pos >= blockSize) {
        // If we are reading the same block but from a continued reader cycle,
        // pos should be maintained. For this reason, pos should be set to
        // blockSize on constructor.
        pos = 0;
    }

    return true;
}

bool LogConsumer::next(logrec_t*& lr)
{
    w_assert1(nextLSN <= endLSN);

    int lrLength;
    bool scanned = logScanner->nextLogrec(currentBlock, pos, lr, &nextLSN,
            &endLSN, &lrLength);

    bool stopReading = nextLSN == endLSN;
    if (!scanned && readWholeBlocks && !stopReading) {
        /*
         * If the policy is to read whole blocks only, we must also stop
         * reading when an incomplete log record was fetched on the last block.
         * Under normal circumstances, we would fetch the next block to
         * assemble the remainder of the log record. In this case, however, we
         * must wait until the next activation. This case is detected when the
         * length of the next log record is larger than the space remaining in
         * the current block, or if the length is negative (meaning there are
         * not enough bytes left on the block to tell the length).
         */
        stopReading = endLSN.hi() == nextLSN.hi() &&
            (lrLength <= 0 || (endLSN.lo() - nextLSN.lo() < lrLength));
    }

    if (!scanned && stopReading) {
        DBGTHRD(<< "Consumer reached end LSN on " << nextLSN);
        /*
         * nextLogrec returns false if it is about to read the LSN given in the
         * last argument (endLSN). This means we should stop and not read any
         * further blocks.  On the next archiver activation, replacement must
         * start on this LSN, which will likely be in the middle of the block
         * currently being processed. However, we don't have to worry about
         * that because reader thread will start reading from this LSN on the
         * next activation.
         */
        return false;
    }

    w_assert1(nextLSN <= endLSN);
    w_assert1(!scanned || lr->lsn_ck() + lr->length() == nextLSN);

    if (!scanned || (lrLength > 0 && lr->type() == logrec_t::t_skip)) {
        /*
         * nextLogrec returning false with nextLSN != endLSN means that we are
         * suppose to read another block and call the method again.
         */
        if (scanned && lr->type() == logrec_t::t_skip) {
            // Try again if reached skip -- next block should be from next file
            nextLSN = lsn_t(nextLSN.hi() + 1, 0);
            pos = 0;
            DBGTHRD(<< "Reached skip logrec, set nextLSN = " << nextLSN);
            logScanner->reset();
            w_assert1(!logScanner->hasPartialLogrec());
        }
        if (!nextBlock()) {
            // reader thread finished and consume request failed
            DBGTHRD(<< "LogConsumer next-block request failed");
            return false;
        }
        return next(lr);
    }

    return true;
}

/**
 * Selection part of replacement-selection algorithm. Takes the smallest
 * record from the heap and copies it to the write buffer, one IO block
 * at a time. The block header contains the run number (1 byte) and the
 * logical size of the block (4 bytes). The former is required so that
 * the asynchronous writer thread knows when to start a new run file.
 * The latter simplifies the write process by not allowing records to
 * be split in the middle by block boundaries.
 */
bool LogArchiver::selection()
{
    if (heap->size() == 0) {
        // if there are no elements in the heap, we have nothing to write
        // -> return and wait for next activation
        DBGTHRD(<< "Selection got empty heap -- sleeping");
        return false;
    }

    run_number_t run = heap->topRun();
    if (!blkAssemb->start(run)) {
        return false;
    }

    DBGTHRD(<< "Producing block for selection on run " << run);
    while (true) {
        if (heap->size() == 0 || run != heap->topRun()) {
            break;
        }

        logrec_t* lr = heap->top();
        if (blkAssemb->add(lr)) {
            // DBGTHRD(<< "Selecting for output: " << *lr);
            heap->pop();
            // w_assert3(run != heap->topRun() ||
            //     heap->top()->pid()>= lr->pid());
        }
        else {
            break;
        }
    }
    blkAssemb->finish();

    return true;
}

BlockAssembly::BlockAssembly(ArchiveDirectory* directory, unsigned level)
    : dest(NULL), maxLSNInBlock(lsn_t::null), maxLSNLength(0),
    lastRun(-1), bucketSize(0), nextBucket(0), level(level)
{
    archIndex = directory->getIndex();
    w_assert0(archIndex);
    blockSize = directory->getBlockSize();
    bucketSize = archIndex->getBucketSize();
    writebuf = new AsyncRingBuffer(blockSize, IO_BLOCK_COUNT);
    writer = new WriterThread(writebuf, directory, level);
    writer->fork();
}

BlockAssembly::~BlockAssembly()
{
    if (!writebuf->isFinished()) {
        shutdown();
    }
    delete writer;
    delete writebuf;
}

bool BlockAssembly::hasPendingBlocks()
{
    return !writebuf->isEmpty();
}

run_number_t BlockAssembly::getRunFromBlock(const char* b)
{
    BlockHeader* h = (BlockHeader*) b;
    return h->run;
}

lsn_t BlockAssembly::getLSNFromBlock(const char* b)
{
    BlockHeader* h = (BlockHeader*) b;
    return h->lsn;
}

size_t BlockAssembly::getEndOfBlock(const char* b)
{
    BlockHeader* h = (BlockHeader*) b;
    return h->end;
}

bool BlockAssembly::start(run_number_t run)
{
    DBGTHRD(<< "Requesting write block for selection");
    dest = writebuf->producerRequest();
    if (!dest) {
        DBGTHRD(<< "Block request failed!");
        if (!writebuf->isFinished()) {
            W_FATAL_MSG(fcINTERNAL,
                    << "ERROR: write ring buffer refused produce request");
        }
        return false;
    }
    DBGTHRD(<< "Picked block for selection " << (void*) dest);

    pos = sizeof(BlockHeader);

    if (run != lastRun) {
        if (archIndex) {
            archIndex->appendNewEntry(level);
        }
        nextBucket = 0;
        fpos = 0;
        lastRun = run;
    }

    buckets.clear();

    return true;
}

bool BlockAssembly::add(logrec_t* lr)
{
    w_assert0(dest);

    size_t available = blockSize - (pos + sizeof(baseLogHeader));
    if (lr->length() > available) {
        return false;
    }

    if (firstPID == 0) {
        firstPID = lr->pid();
    }

    if (maxLSNInBlock < lr->lsn_ck()) {
        maxLSNInBlock = lr->lsn_ck();
        maxLSNLength = lr->length();
    }

    if (lr->pid() / bucketSize >= nextBucket) {
        PageID shpid = (lr->pid() / bucketSize) * bucketSize;
        buckets.push_back(
                pair<PageID, size_t>(shpid, fpos));
        nextBucket = shpid / bucketSize + 1;
    }

    w_assert1(pos > 0 || fpos % blockSize == 0);

    memcpy(dest + pos, lr, lr->length());
    pos += lr->length();
    fpos += lr->length();
    return true;
}

void BlockAssembly::finish()
{
    DBGTHRD("Selection produced block for writing " << (void*) dest <<
            " in run " << (int) lastRun << " with end " << pos);
    w_assert0(dest);

    w_assert0(archIndex);
    archIndex->newBlock(buckets, level);
    firstPID = 0;

    // write block header info
    BlockHeader* h = (BlockHeader*) dest;
    h->run = lastRun;
    h->end = pos;
    /*
     * CS: end LSN of a block/run has to be an exclusive boundary, whereas
     * lastLSN is an inclusive one (i.e., the LSN of the last logrec in this
     * block). To fix that, we simply add the length of the last log record to
     * its LSN, which yields the LSN of the following record in the recovery
     * log. It doesn't matter if this following record does not get archived or
     * if it is a skip log record, since the property that must be respected is
     * simply that run boundaries must match (i.e., endLSN(n) == beginLSN(n+1)
     */
    h->lsn = maxLSNInBlock.advance(maxLSNLength);

#if W_DEBUG_LEVEL>=3
    // verify that all log records are within end boundary
    size_t vpos = sizeof(BlockHeader);
    while (vpos < pos) {
        logrec_t* lr = (logrec_t*) (dest + vpos);
        w_assert3(lr->lsn_ck() < h->lsn);
        vpos += lr->length();
    }
#endif

    maxLSNInBlock = lsn_t::null;
    writebuf->producerRelease();
    dest = NULL;
}

void BlockAssembly::shutdown()
{
    w_assert0(!dest);
    writebuf->set_finished();
    writer->join();
}

ArchiveScanner::ArchiveScanner(ArchiveDirectory* directory)
    : directory(directory), archIndex(directory->getIndex())
{
    if (!archIndex) {
        W_FATAL_MSG(fcINTERNAL,
                << "ArchiveScanner requires a valid archive index!");
    }
}

ArchiveScanner::RunMerger*
ArchiveScanner::open(PageID startPID, PageID endPID,
        lsn_t startLSN, size_t readSize)
{
    RunMerger* merger = new RunMerger();
    vector<ProbeResult> probes;

    // probe for runs
    archIndex->probe(probes, startPID, endPID, startLSN);

    // construct one run scanner for each probed run
    for (size_t i = 0; i < probes.size(); i++) {
        RunScanner* runScanner = new RunScanner(
                probes[i].runBegin,
                probes[i].runEnd,
                probes[i].level,
                probes[i].pidBegin,
                probes[i].pidEnd,
                probes[i].offset,
                directory,
                readSize
        );

        merger->addInput(runScanner);
    }

    if (merger->heapSize() == 0) {
        // all runs pruned from probe
        delete merger;
        return NULL;
    }

    INC_TSTAT(la_open_count);

    return merger;
}

ArchiveScanner::RunScanner::RunScanner(lsn_t b, lsn_t e, unsigned level,
        PageID f, PageID l, off_t o, ArchiveDirectory* directory, size_t rSize)
    : runBegin(b), runEnd(e), level(level), firstPID(f), lastPID(l), offset(o),
        fd(-1), blockCount(0), readSize(rSize), directory(directory)
{
    if (readSize == 0) {
        readSize = directory->getBlockSize();
    }

    // Using direct I/O
    int res = posix_memalign((void**) &buffer, IO_ALIGN, readSize + IO_ALIGN);
    w_assert0(res == 0);
    // buffer = new char[directory->getBlockSize()];

    w_assert0(directory->getIndex());
    bucketSize = directory->getIndex()->getBucketSize();

    // bpos at the end of block triggers reading of the first block
    // when calling next()
    bpos = readSize;
    w_assert1(bpos > 0);

    scanner = new LogScanner(readSize);
}

ArchiveScanner::RunScanner::~RunScanner()
{
    if (fd > 0) {
        W_COERCE(directory->closeScan(fd));
    }

    delete scanner;

    // Using direct I/O
    free(buffer);
    // delete[] buffer;
}

bool ArchiveScanner::RunScanner::nextBlock()
{
    size_t blockSize = directory->getBlockSize();

    if (fd < 0) {
        W_COERCE(directory->openForScan(fd, runBegin, runEnd, level));

        if (directory->getIndex()) {
            directory->getIndex()->getBlockCounts(fd, NULL, &blockCount);
        }
    }

    // do not read past data blocks into index blocks
    if (blockCount == 0 || offset >= blockCount * blockSize)
    {
        W_COERCE(directory->closeScan(fd));
        return false;
    }

    // offset is updated by readBlock
    W_COERCE(directory->readBlock(fd, buffer, offset, readSize));

    // offset set to zero indicates EOF
    if (offset == 0) {
        W_COERCE(directory->closeScan(fd));
        return false;
    }

    bpos = 0;

    return true;
}

bool ArchiveScanner::RunScanner::next(logrec_t*& lr)
{
    while (true) {
        if (scanner->nextLogrec(buffer, bpos, lr)) { break; }
        if (!nextBlock()) { return false; }
    }

    if (lr->type() == logrec_t::t_skip ||
                (lastPID != 0 && lr->pid() >= lastPID))
    {
        // end of scan
        return false;
    }

    return true;
}

std::ostream& operator<< (ostream& os,
        const ArchiveScanner::RunScanner& m)
{
    os << m.runBegin << "-" << m.runEnd << " endPID=" << m.lastPID;
    return os;
}

ArchiveScanner::MergeHeapEntry::MergeHeapEntry(RunScanner* runScan)
    : active(true), runScan(runScan)
{
    PageID startPID = runScan->firstPID;
    // bring scanner up to starting point
    logrec_t* next = NULL;
    if (runScan->next(next)) {
        lr = next;
        lsn = lr->lsn();
        pid = lr->pid();
        active = true;
        DBG(<< "Run scan opened on pid " << lr->pid() << " afer " << startPID);

        // advance index until startPID is reached
        if (pid < startPID) {
            bool hasNext = true;
            while (hasNext && lr->pid() < startPID) {
                hasNext = runScan->next(lr);
            }
            if (hasNext) {
                DBG(<< "Run scan advanced to pid " << lr->pid() << " afer " << startPID);
                pid = lr->pid();
                lsn = lr->lsn();
            }
            else { active = false; }
        }
    }
    else { active = false; }
}

void ArchiveScanner::MergeHeapEntry::moveToNext()
{
    if (runScan->next(lr)) {
        pid = lr->pid();
        lsn = lr->lsn_ck();
    }
    else {
        active = false;
    }
}

void ArchiveScanner::RunMerger::addInput(RunScanner* r)
{
    w_assert0(!started);
    MergeHeapEntry entry(r);
    heap.AddElementDontHeapify(entry);

    if (endPID == 0) {
        endPID = r->lastPID;
    }
    w_assert1(endPID == r->lastPID);
}

bool ArchiveScanner::RunMerger::next(logrec_t*& lr)
{
    stopwatch_t timer;

    if (heap.NumElements() == 0) {
        return false;
    }

    if (!started) {
        started = true;
        heap.Heapify();
    }
    else {
        /*
         * CS: Before returning the next log record, the scanner at the top of
         * the heap must be recomputed and the heap re-organized. This is
         * because the caller maintains a pointer into the scanner's buffer,
         * and calling next before the log record is consumed may cause the
         * pointer to be invalidated if a new block is read into the buffer.
         */
        heap.First().moveToNext();
        heap.ReplacedFirst();
    }

    if (!heap.First().active) {
        /*
         * CS: If top run is inactive, then all runs are and scan is done
         * Memory of each scanner must be released here instead of when
         * destructing heap, because the heap internally copies entries and
         * destructs these copies in operations like SiftDown(). Therefore the
         * underlying buffer may get wrongly deleted
         */
        close();
        return false;
    }

    ADD_TSTAT(la_merge_heap_time, timer.time_us());

    lr = heap.First().lr;
    return true;
}

void ArchiveScanner::RunMerger::close()
{
    while (heap.NumElements() > 0) {
        delete heap.RemoveFirst().runScan;
    }
}

void ArchiveScanner::RunMerger::dumpHeap(ostream& out)
{
    heap.Print(out);
}

ArchiverHeap::ArchiverHeap(size_t workspaceSize)
    : currentRun(0), filledFirst(false), w_heap(heapCmp)
{
    workspace = new fixed_lists_mem_t(workspaceSize);
}

ArchiverHeap::~ArchiverHeap()
{
    delete workspace;
}

slot_t ArchiverHeap::allocate(size_t length)
{
    slot_t dest(NULL, 0);
    W_COERCE(workspace->allocate(length, dest));

    if (!dest.address) {
        // workspace full -> do selection until space available
        DBGTHRD(<< "Heap full! Size: " << w_heap.NumElements()
                << " alloc size: " << length);
        if (!filledFirst) {
            // first run generated by first full load of w_heap
            currentRun++;
            filledFirst = true;
            DBGTHRD(<< "Heap full for the first time; start run 1");
        }
    }

    return dest;
}

bool ArchiverHeap::push(logrec_t* lr, bool duplicate)
{
    slot_t dest = allocate(lr->length());
    if (!dest.address) {
        DBGTHRD(<< "heap full for logrec: " << lr->type_str()
                << " at " << lr->lsn());
        return false;
    }

    PageID pid = lr->pid();
    lsn_t lsn = lr->lsn();
    memcpy(dest.address, lr, lr->length());

    // CS: Multi-page log records are replicated so that each page can be
    // recovered from the log archive independently.  Note that this is not
    // required for Restart or Single-page recovery because following the
    // per-page log chain of both pages eventually lands on the same multi-page
    // log record. For restore, it must be duplicated because log records are
    // sorted and there is no chain.
    if (duplicate) {
        // If we have to duplciate the log record, make sure there is room by
        // calling recursively without duplication. Note that the original
        // contents were already saved with the memcpy operation above.
        lr->set_pid(lr->pid2());
        lr->set_page_prev_lsn(lr->page2_prev_lsn());
        if (!push(lr, false)) {
            // If duplicated did not fit, then insertion of the original must
            // also fail. We have to (1) restore the original contents of
            // the log record for the next attempt; and (2) free its memory
            // from the workspace. Since nothing was added to the heap yet, it
            // stays untouched.
            memcpy(lr, dest.address, lr->length());
            W_COERCE(workspace->free(dest));
            return false;
        }
    }
    else {
        // If all records of the current run are gone, start new run. But only
        // if we are not duplicating a log record -- otherwise two new runs
        // would be created.
        if (filledFirst &&
                (size() == 0 || w_heap.First().run == currentRun)) {
            currentRun++;
            DBGTHRD(<< "Replacement starting new run " << (int) currentRun
                    << " on LSN " << lr->lsn_ck());
        }
    }

    //DBGTHRD(<< "Processing logrec " << lr->lsn_ck() << ", type " <<
    //        lr->type() << "(" << lr->type_str() << ") length " <<
    //        lr->length() << " into run " << (int) currentRun);

    // insert key and pointer into w_heap
    HeapEntry k(currentRun, pid, lsn, dest);

    // CS: caution: AddElementDontHeapify does NOT work!!!
    w_heap.AddElement(k);

    return true;
}

void ArchiverHeap::pop()
{
    // DBGTHRD(<< "Selecting for output: "
    //         << *((logrec_t*) w_heap.First().slot.address));

    workspace->free(w_heap.First().slot);
    w_heap.RemoveFirst();

    if (size() == 0) {
        // If heap becomes empty, run generation must be reset with a new run
        filledFirst = false;
        currentRun++;
    }
}

logrec_t* ArchiverHeap::top()
{
    return (logrec_t*) w_heap.First().slot.address;
}

// gt is actually a less than function, to produce ascending order
bool ArchiverHeap::Cmp::gt(const HeapEntry& a,
        const HeapEntry& b) const
{
    if (a.run != b.run) {
        return a.run < b.run;
    }
    if (a.pid != b.pid) {
        return a.pid< b.pid;
    }
    return a.lsn < b.lsn;
}

/**
 * Replacement part of replacement-selection algorithm. Fetches log records
 * from the read buffer into the sort workspace and adds a correspondent
 * entry to the heap. When workspace is full, invoke selection until there
 * is space available for the current log record.
 *
 * Unlike standard replacement selection, runs are limited to the size of
 * the workspace, in order to maintain a simple non-overlapping mapping
 * between regions of the input file (i.e., the recovery log) and the runs.
 * To achieve that, we change the logic that assigns run numbers to incoming
 * records:
 *
 * a) Standard RS: if incoming key is larger than the last record written,
 * assign to current run, otherwise to the next run.
 * b) Log-archiving RS: keep track of run number currently being written,
 * always assigning the incoming records to a greater run. Once all records
 * from the current run are removed from the heap, increment the counter.
 * To start, initial input records are assigned to run 1 until the workspace
 * is full, after which incoming records are assigned to run 2.
 */
void LogArchiver::replacement()
{
    while(true) {
        logrec_t* lr;
        if (!consumer->next(lr)) {
            w_assert0(readWholeBlocks ||
                    control.endLSN <= consumer->getNextLSN());
            if (control.endLSN < consumer->getNextLSN()) {
                // nextLSN may be greater than endLSN due to skip
                control.endLSN = consumer->getNextLSN();
                // TODO: in which correct situation can this assert fail???
                // w_assert0(control.endLSN.hi() == 0);
                DBGTHRD(<< "Replacement changed endLSN to " << control.endLSN);
            }
            return;
        }

        if (!lr->is_redo()) {
            continue;
        }

        pushIntoHeap(lr, lr->is_multi_page());
    }
}

void LogArchiver::pushIntoHeap(logrec_t* lr, bool duplicate)
{
    while (!heap->push(lr, duplicate)) {
        if (heap->size() == 0) {
            W_FATAL_MSG(fcINTERNAL,
                    << "Heap empty but push not possible!");
        }

        // heap full -- invoke selection and try again
        if (heap->size() == 0) {
            // CS TODO this happens sometimes for very large page_img_format
            // logrecs. Inside this if, we should "reset" the heap and also
            // makesure that the log record is smaller than the max block.
            W_FATAL_MSG(fcINTERNAL,
                    << "Heap empty but push not possible!");
        }

        DBGTHRD(<< "Heap full! Invoking selection");
        bool success = selection();

        w_assert0(success || heap->size() == 0);
    }
}

void LogArchiver::activate(lsn_t endLSN, bool wait)
{
    if (eager) return;

    w_assert0(smlevel_0::log);
    if (endLSN == lsn_t::null) {
        endLSN = smlevel_0::log->durable_lsn();
    }

    while (!control.activate(wait, endLSN)) {
        if (!wait) break;
    }
}

bool LogArchiver::waitForActivation()
{
    if (eager) {
        lsn_t newEnd = smlevel_0::log->durable_lsn();
        while (control.endLSN == newEnd) {
            // we're going faster than log, sleep a bit (1ms)
            ::usleep(1000);
            newEnd = smlevel_0::log->durable_lsn();

            lintel::atomic_thread_fence(lintel::memory_order_consume);
            if (shutdownFlag) {
                return false;
            }

            // Flushing requested (e.g., by restore manager)
            if (flushReqLSN != lsn_t::null) {
                return true;
            }

            if (newEnd.lo() == 0) {
                // If durable_lsn is at the beginning of a new log partition,
                // it can happen that at this point the file was not created
                // yet, which would cause the reader thread to fail.
                continue;
            }
        }
        control.endLSN = newEnd;
    }
    else {
        bool activated = control.waitForActivation();
        if (!activated) {
            return false;
        }
    }

    lintel::atomic_thread_fence(lintel::memory_order_consume);
    if (shutdownFlag) {
        return false;
    }

    return true;
}

bool LogArchiver::processFlushRequest()
{
    if (flushReqLSN != lsn_t::null) {
        DBGTHRD(<< "Archive flush requested until LSN " << flushReqLSN);
        if (getNextConsumedLSN() < flushReqLSN) {
            // if logrec hasn't been read into heap yet, then selection
            // will never reach it. Do another round until heap has
            // consumed it.
            if (control.endLSN < flushReqLSN) {
                control.endLSN = flushReqLSN;
            }
            DBGTHRD(<< "LSN requested for flush hasn't been consumed yet. "
                    << "Trying again after another round");
            return false;
        }
        else {
            // consume whole heap
            while (selection()) {}
            // Heap empty: Wait for all blocks to be consumed and writen out
            w_assert0(heap->size() == 0);
            while (blkAssemb->hasPendingBlocks()) {
                ::usleep(10000); // 10ms
            }

            // Forcibly close current run to guarantee that LSN is persisted
            W_COERCE(directory->closeCurrentRun(flushReqLSN, 1 /* level */));
            blkAssemb->resetWriter();

            /* Now we know that the requested LSN has been processed by the
             * heap and all archiver temporary memory has been flushed. Thus,
             * we know it has been fully processed and all relevant log records
             * are available in the archive.
             */
            flushReqLSN = lsn_t::null;
            lintel::atomic_thread_fence(lintel::memory_order_release);
            return true;
        }
    }
    return false;
}

bool LogArchiver::isLogTooSlow()
{
    if (!eager) { return false; }

    int minActWindow = directory->getBlockSize();

    auto isSmallWindow = [minActWindow](lsn_t endLSN, lsn_t nextLSN) {
        int nextHi = nextLSN.hi();
        int nextLo = nextLSN.lo();
        int endHi = endLSN.hi();
        int endLo = endLSN.lo();
        return (endHi == nextHi && endLo - nextLo< minActWindow) ||
            (endHi == nextHi + 1 && endLo < minActWindow);
    };

    if (isSmallWindow(control.endLSN, nextActLSN))
    {
        // If this happens to often, the block size should be decreased.
        ::usleep(slowLogGracePeriod);
        // To better exploit device bandwidth, we only start archiving if
        // at least one block worth of log is available for consuption.
        // This happens when the log is growing too slow.
        // However, if it seems like log activity has stopped (i.e.,
        // durable_lsn did not advance since we started), then we proceed
        // with the small activation window.
        bool logStopped = control.endLSN == smlevel_0::log->durable_lsn();
        if (!isSmallWindow(control.endLSN, nextActLSN) && !logStopped) {
            return false;
        }
        INC_TSTAT(la_log_slow);
        DBGTHRD(<< "Log growing too slow");
        return true;
    }
    return false;
}

bool LogArchiver::shouldActivate(bool logTooSlow)
{
    if (flushReqLSN == control.endLSN) {
        return control.endLSN > nextActLSN;
    }

    if (logTooSlow && control.endLSN == smlevel_0::log->durable_lsn()) {
        // Special case: log is not only groing too slow, but it has actually
        // halted. This means the application/experiment probably already
        // finished and is just waiting for the archiver. In that case, we
        // allow the activation with a small window. However, it may not be
        // a window of size zero (s.t. endLSN == nextActLSN)
        DBGTHRD(<< "Log seems halted -- accepting small window");
        return control.endLSN > nextActLSN;
    }

    // Try to keep activation window at block boundaries to better utilize
    // I/O bandwidth
    if (eager && readWholeBlocks && !logTooSlow) {
        size_t boundary = directory->getBlockSize() *
            (control.endLSN.lo() / directory->getBlockSize());
        control.endLSN = lsn_t(control.endLSN.hi(), boundary);
        if (control.endLSN <= nextActLSN) {
            return false;
        }
        if (control.endLSN.lo() == 0) {
            // If durable_lsn is at the beginning of a new log partition,
            // it can happen that at this point the file was not created
            // yet, which would cause the reader thread to fail. This does
            // not happen with eager archiving, so we should eventually
            // remove it
            return false;
        }
        DBGTHRD(<< "Adjusted activation window to block boundary " <<
                control.endLSN);
    }

    if (control.endLSN == lsn_t::null
            || control.endLSN <= nextActLSN)
    {
        DBGTHRD(<< "Archiver already passed this range. Continuing...");
        return false;
    }

    w_assert1(control.endLSN > nextActLSN);
    return true;
}

void LogArchiver::run()
{
    while(true) {
        CRITICAL_SECTION(cs, control.mutex);

        if (!waitForActivation()) {
            break;
        }
        bool logTooSlow = isLogTooSlow();

        if (processFlushRequest()) {
            continue;
        }

        if (!shouldActivate(logTooSlow)) {
            continue;
        }
        INC_TSTAT(la_activations);

        DBGTHRD(<< "Log archiver activated from " << nextActLSN << " to "
                << control.endLSN);

        consumer->open(control.endLSN, readWholeBlocks && !logTooSlow);

        replacement();

        /*
         * Selection is not invoked here because log archiving should be a
         * continuous process, and so the heap should not be emptied at
         * every invocation. Instead, selection is invoked by the replacement
         * method when the heap is full. This also has the advantage that the
         * heap is kept as full as possible, which generates larger runs.
         * A consequence of this scheme is that the activation of the log
         * archiver until an LSN X just means that all log records up to X will
         * be inserted into the heap, and not that they will be persited into
         * runs. This means that log recycling must not rely on activation
         * cycles, but on signals/events generated by the writer thread (TODO)
         */

        // nextActLSN = consumer->getNextLSN();
        nextActLSN = control.endLSN;
        DBGTHRD(<< "Log archiver consumed all log records until LSN "
                << nextActLSN);

        if (!eager) {
            control.endLSN = lsn_t::null;
            control.activated = false;
        }
    }

    // Perform selection until all remaining entries are flushed out of
    // the heap into runs. Last run boundary is also enqueued.
    DBGTHRD(<< "Archiver exiting -- last round of selection to empty heap");
    while (selection()) {}

    w_assert0(heap->size() == 0);
}

bool LogArchiver::requestFlushAsync(lsn_t reqLSN)
{
    if (reqLSN == lsn_t::null) {
        return false;
    }
    lintel::atomic_thread_fence(lintel::memory_order_acquire);
    if (flushReqLSN != lsn_t::null) {
        return false;
    }
    flushReqLSN = reqLSN;
    lintel::atomic_thread_fence(lintel::memory_order_release);

    // Other thread may race with us and win -- recheck
    lintel::atomic_thread_fence(lintel::memory_order_acquire);
    if (flushReqLSN != reqLSN) {
        return false;
    }
    return true;
}

void LogArchiver::requestFlushSync(lsn_t reqLSN)
{
    DBGTHRD(<< "Requesting flush until LSN " << reqLSN);
    if (!eager) {
        activate(reqLSN);
    }
    while (!requestFlushAsync(reqLSN)) {
        usleep(1000); // 1ms
    }
    // When log archiver is done processing the flush request, it will set
    // flushReqLSN back to null. This method only guarantees that the flush
    // request was processed. The caller must still wait for the desired run to
    // be persisted -- if it so wishes.
    while(true) {
        lintel::atomic_thread_fence(lintel::memory_order_acquire);
        if (flushReqLSN == lsn_t::null) {
            break;
        }
        ::usleep(10000); // 10ms
    }
}

void LogArchiver::archiveUntilLSN(lsn_t lsn)
{
    // if lsn.lo() == 0, archiver will not activate and it will get stuck
    w_assert1(lsn.lo() > 0);

    // wait for log record to be consumed
    while (getNextConsumedLSN() < lsn) {
        activate(lsn, true);
        ::usleep(10000); // 10ms
    }

    if (getDirectory()->getLastLSN() < lsn) {
        requestFlushSync(lsn);
    }
}

void ArchiveDirectory::deleteAllRuns()
{
    fs::directory_iterator it(archpath), eod;
    boost::regex run_rx(run_regex, boost::regex::perl);
    for (; it != eod; it++) {
        string fname = it->path().filename().string();
        if (boost::regex_match(fname, run_rx)) {
            fs::remove(it->path());
        }
    }
}

bool LogScanner::hasPartialLogrec()
{
    return truncMissing > 0;
}

void LogScanner::reset()
{
    truncMissing = 0;
}

/**
 * Fetches a log record from the read buffer ('src' in offset 'pos').
 * Handles incomplete records due to block boundaries in the buffer
 * and skips checkpoints and skip log records. Returns false if whole
 * record could not be read in the current buffer block, indicating that
 * the caller must fetch a new block into 'src' and invoke method again.
 *
 * Method loops until any in-block skipping is completed.
 */
bool LogScanner::nextLogrec(char* src, size_t& pos, logrec_t*& lr, lsn_t* nextLSN,
        lsn_t* stopLSN, int* lrLength)
{
tryagain:
    if (nextLSN && stopLSN && *stopLSN == *nextLSN) {
        return false;
    }

    // whole log record is not guaranteed to fit in a block
    size_t remaining = blockSize - pos;
    if (remaining == 0) {
        return false;
    }

    lr = (logrec_t*) (src + pos);

    if (truncMissing > 0) {
        // finish up the trunc logrec from last block
        DBG5(<< "Reading partial log record -- missing: "
                << truncMissing << " of " << truncCopied + truncMissing);
        w_assert1(truncMissing <= remaining);
        memcpy(truncBuf + truncCopied, src + pos, truncMissing);
        pos += truncMissing;
        lr = (logrec_t*) truncBuf;
        truncCopied += truncMissing;
        truncMissing = 0;
        w_assert1(truncCopied == lr->length());
    }
    // we need at least two bytes to read the length
    else if (remaining == 1 || lr->length() > remaining) {
        // remainder of logrec must be read from next block
        w_assert0(remaining < sizeof(baseLogHeader) || lr->valid_header());
        DBG5(<< "Log record with length "
                << (remaining > 1 ? lr->length() : -1)
                << " does not fit in current block of " << remaining);
        w_assert0(remaining <= sizeof(logrec_t));
        memcpy(truncBuf, src + pos, remaining);
        truncCopied = remaining;
        truncMissing = lr->length() - remaining;
        pos += remaining;

        if (lrLength) {
            *lrLength = (remaining > 1) ? lr->length() : -1;
        }

        return false;
    }

    // assertions to check consistency of logrec
#if W_DEBUG_LEVEL >=1
    // TODO add assert macros with DBG message
    if (nextLSN != NULL && !lr->valid_header(*nextLSN)) {
        DBGTHRD(<< "Unexpected LSN in scanner at pos " << pos
                << " : " << lr->lsn_ck()
                << " expected " << *nextLSN);
    }
#endif

    w_assert1(lr->valid_header(nextLSN == NULL ? lsn_t::null : *nextLSN));

    if (nextLSN) {
        *nextLSN += lr->length();
    }

    if (lrLength) {
        *lrLength = lr->length();
    }

    // handle ignorred logrecs
    if (ignore[lr->type()]) {
        // if logrec was assembled from truncation, pos was already
        // incremented, and skip is not necessary
        if ((void*) lr == (void*) truncBuf) {
            goto tryagain;
        }
        // DBGTHRD(<< "Found " << lr->type_str() << " on " << lr->lsn_ck()
        //         << " pos " << pos << ", skipping " << lr->length());
        toSkip += lr->length();
    }

    // see if we have something to skip
    if (toSkip > 0) {
        if (toSkip <= remaining) {
            // stay in the same block after skipping
            pos += toSkip;
            //DBGTHRD(<< "In-block skip for replacement, new pos = " << pos);
            toSkip = 0;
            goto tryagain;
        }
        else {
            DBGTHRD(<< "Skipping to next block until " << toSkip);
            toSkip -= remaining;
            return false;
        }
    }

    // if logrec was assembled from truncation, pos was already incremented
    if ((void*) lr != (void*) truncBuf) {
        pos += lr->length();
    }

    // DBGTHRD(<< "Log scanner returning  " << lr->type_str()
    //         << " on pos " << pos << " lsn " << lr->lsn_ck());


    return true;
}

ArchiveIndex::ArchiveIndex(size_t blockSize, size_t bucketSize)
    : blockSize(blockSize), bucketSize(bucketSize), maxLevel(0)
{
    DO_PTHREAD(pthread_mutex_init(&mutex, NULL));
}

ArchiveIndex::~ArchiveIndex()
{
}

void ArchiveIndex::newBlock(const vector<pair<PageID, size_t> >&
        buckets, unsigned level)
{
    CRITICAL_SECTION(cs, mutex);

    w_assert1(bucketSize > 0);

    size_t prevOffset = 0;
    for (size_t i = 0; i < buckets.size(); i++) {
        BlockEntry e;
        e.pid = buckets[i].first;
        e.offset = buckets[i].second;
        w_assert1(e.offset == 0 || e.offset > prevOffset);
        prevOffset = e.offset;
        runs[level].back().entries.push_back(e);
    }
}

rc_t ArchiveIndex::finishRun(lsn_t first, lsn_t last, int fd,
        off_t offset, unsigned level)
{
    CRITICAL_SECTION(cs, mutex);
    w_assert1(offset % blockSize == 0);

    // check if it isn't an empty run (from truncation)
    int& lf = lastFinished[level];
    if (offset > 0 && lf < (int) runs[level].size()) {
        lf++;
        w_assert1(lf == 0 || first == runs[level][lf-1].lastLSN);
        w_assert1(lf < (int) runs[level].size());

        runs[level][lf].firstLSN = first;
        runs[level][lf].lastLSN = last;
        W_DO(serializeRunInfo(runs[level][lf], fd, offset));
    }

    return RCOK;
}

rc_t ArchiveIndex::serializeRunInfo(RunInfo& run, int fd,
        off_t offset)
{
    // Assumption: mutex is held by caller

    // lastPID is stored on first block, but we reserve space for it in every
    // block to simplify things
    int entriesPerBlock =
        (blockSize - sizeof(BlockHeader) - sizeof(PageID)) / sizeof(BlockEntry);
    int remaining = run.entries.size();
    int i = 0;
    size_t currEntry = 0;

    // CS TODO RAII
    char * writeBuffer = new char[blockSize];

    while (remaining > 0) {
        int j = 0;
        size_t bpos = sizeof(BlockHeader);
        while (j < entriesPerBlock && remaining > 0)
        {
            memcpy(writeBuffer + bpos, &run.entries[currEntry],
                        sizeof(BlockEntry));
            j++;
            currEntry++;
            remaining--;
            bpos += sizeof(BlockEntry);
        }
        BlockHeader* h = (BlockHeader*) writeBuffer;
        h->entries = j;
        h->blockNumber = i;

        // copy lastPID into last block (space was reserved above)
        // if (remaining == 0) {
        //     memcpy(writeBuffer + bpos, &run.lastPID, sizeof(PageID));
        // }

        auto ret = ::pwrite(fd, writeBuffer, blockSize, offset);
        CHECK_ERRNO(ret);
        offset += blockSize;
        i++;
    }

    delete[] writeBuffer;

    return RCOK;
}

void ArchiveIndex::init()
{
    for (unsigned l = 0; l < runs.size(); l++) {
        std::sort(runs[l].begin(), runs[l].end());
    }
}

void ArchiveIndex::appendNewEntry(unsigned level)
{
    CRITICAL_SECTION(cs, mutex);

    // if (runs.size() > 0) {
    //     runs.back().lastPID = lastPID;
    // }

    RunInfo newRun;
    if (level > maxLevel) {
        maxLevel = level;
        runs.resize(maxLevel+1);
        lastFinished.resize(maxLevel+1, -1);
    }
    runs[level].push_back(newRun);
}

lsn_t ArchiveIndex::getLastLSN(unsigned level)
{
    CRITICAL_SECTION(cs, mutex);

    if (level > maxLevel) { return lsn_t::null; }

    if (lastFinished[level] < 0) {
        // No runs exist in the given level. If a previous level exists, it
        // must be the first LSN in that level; otherwise, it's simply 1.0
        if (level == 0) { return lsn_t(1,0); }
        return getFirstLSN(level - 1);
    }

    return runs[level][lastFinished[level]].lastLSN;
}

lsn_t ArchiveIndex::getFirstLSN(unsigned level)
{
    if (level <= 1) { return lsn_t(1,0); }
    // If no runs exist at this level, recurse down to previous level;
    if (lastFinished[level] < 0) { return getFirstLSN(level-1); }

    return runs[level][0].firstLSN;
}

rc_t ArchiveIndex::loadRunInfo(int fd, const ArchiveDirectory::RunFileStats& fstats)
{
    RunInfo run;
    {
        memalign_allocator<char, IO_ALIGN> alloc;
        char* readBuffer = alloc.allocate(blockSize);

        size_t indexBlockCount = 0;
        size_t dataBlockCount = 0;
        W_DO(getBlockCounts(fd, &indexBlockCount, &dataBlockCount));

        off_t offset = dataBlockCount * blockSize;
        w_assert1(dataBlockCount == 0 || offset > 0);
        size_t lastOffset = 0;

        while (indexBlockCount > 0) {
            auto bytesRead = ::pread(fd, readBuffer, blockSize, offset);
            CHECK_ERRNO(bytesRead);
            if (bytesRead != blockSize) { return RC(stSHORTIO); }

            BlockHeader* h = (BlockHeader*) readBuffer;

            unsigned j = 0;
            size_t bpos = sizeof(BlockHeader);
            while(j < h->entries)
            {
                BlockEntry* e = (BlockEntry*)(readBuffer + bpos);
                w_assert1(lastOffset == 0 || e->offset > lastOffset);
                run.entries.push_back(*e);

                lastOffset = e->offset;
                bpos += sizeof(BlockEntry);
                j++;
            }
            indexBlockCount--;
            offset += blockSize;

            // if (indexBlockCount == 0) {
            //     // read lasPID from last block
            //     run.lastPID = *((PageID*) (readBuffer + bpos));
            // }
        }

        alloc.deallocate(readBuffer);
    }

    run.firstLSN = fstats.beginLSN;
    run.lastLSN = fstats.endLSN;

    if (fstats.level > maxLevel) {
        maxLevel = fstats.level;
        // level 0 reserved, so add 1
        runs.resize(maxLevel+1);
        lastFinished.resize(maxLevel+1);
    }
    runs[fstats.level].push_back(run);
    lastFinished[fstats.level] = runs[fstats.level].size() - 1;

    return RCOK;
}

rc_t ArchiveIndex::getBlockCounts(int fd, size_t* indexBlocks,
        size_t* dataBlocks)
{
    size_t fsize = ArchiveDirectory::getFileSize(fd);
    w_assert1(fsize % blockSize == 0);

    // skip emtpy runs
    if (fsize == 0) {
        if(indexBlocks) { *indexBlocks = 0; };
        if(dataBlocks) { *dataBlocks = 0; };
        return RCOK;
    }

    // read header of last block in file -- its number is the block count
    // Using direct I/O -- must read whole align block
    char* buffer;
    int res = posix_memalign((void**) &buffer, IO_ALIGN, IO_ALIGN);
    w_assert0(res == 0);

    auto bytesRead = ::pread(fd, buffer, IO_ALIGN, fsize - blockSize);
    CHECK_ERRNO(bytesRead);
    if (bytesRead != IO_ALIGN) { return RC(stSHORTIO); }

    BlockHeader* header = (BlockHeader*) buffer;
    if (indexBlocks) {
        *indexBlocks = header->blockNumber + 1;
    }
    if (dataBlocks) {
        *dataBlocks = (fsize / blockSize) - (header->blockNumber + 1);
        w_assert1(*dataBlocks > 0);
    }
    free(buffer);

    return RCOK;
}

size_t ArchiveIndex::findRun(lsn_t lsn, unsigned level)
{
    // Assumption: mutex is held by caller
    if (lsn == lsn_t::null) {
        // full log replay (backup-less)
        return 0;
    }

    /*
     * CS: requests are more likely to access the last runs, so
     * we do a linear search instead of binary search.
     */
    auto& lf = lastFinished[level];
    w_assert1(lf >= 0);

    if(lsn >= runs[level][lf].lastLSN) {
        return lf + 1;
    }

    int result = lf;
    while (result > 0 && runs[level][result].firstLSN > lsn) {
        result--;
    }

    // skip empty runs
    while (runs[level][result].entries.size() == 0 && result <= lf) {
        result++;
    }

    // caller must check if returned index is valid
    return result >= 0 ? result : runs[level].size();
}

size_t ArchiveIndex::findEntry(RunInfo* run,
        PageID pid, int from, int to)
{
    // Assumption: mutex is held by caller

    if (from > to) {
        if (from == 0) {
            // Queried pid lower than first in run
            return 0;
        }
        // Queried pid is greater than last in run.  This should not happen
        // because probes must not consider this run if that's the case
        W_FATAL_MSG(fcINTERNAL, << "Invalid probe on archiver index! "
                << " PID = " << pid << " run = " << run->firstLSN);
    }

    // negative value indicates first invocation
    if (to < 0) { to = run->entries.size() - 1; }
    if (from < 0) { from = 0; }

    w_assert1(run);
    w_assert1(run->entries.size() > 0);

    // binary search for page ID within run
    size_t i;
    if (from == to) {
        i = from;
    }
    else {
        i = from/2 + to/2;
    }

    w_assert0(i < run->entries.size());

    if (run->entries[i].pid <= pid &&
            (i == run->entries.size() - 1 || run->entries[i+1].pid >= pid))
    {
        // found it! must first check if previous does not contain same pid
        while (i > 0 && run->entries[i].pid == pid)
                //&& run->entries[i].pid == run->entries[i-1].pid)
        {
            i--;
        }
        return i;
    }

    // not found: recurse down
    if (run->entries[i].pid > pid) {
        return findEntry(run, pid, from, i-1);
    }
    else {
        return findEntry(run, pid, i+1, to);
    }
}

void ArchiveIndex::probeInRun(ProbeResult& res)
{
    // Assmuptions: mutex is held; run index and pid are set in given result
    size_t index = res.runIndex;
    auto level = res.level;
    w_assert1((int) index <= lastFinished[level]);
    RunInfo* run = &runs[level][index];

    res.runBegin = runs[level][index].firstLSN;
    res.runEnd = runs[level][index].lastLSN;

    size_t entryBegin = 0;
    if (res.pidBegin == 0) {
        res.offset = 0;
    }
    else {
        entryBegin = findEntry(run, res.pidBegin);
        // decide if we mean offset zero or entry zero
        if (entryBegin == 0 && run->entries[0].pid >= res.pidBegin)
        {
            res.offset = 0;
        }
        else {
            res.offset = run->entries[entryBegin].offset;
        }
    }
}

void ArchiveIndex::probe(std::vector<ProbeResult>& probes,
        PageID startPID, PageID endPID, lsn_t startLSN)
{
    CRITICAL_SECTION(cs, mutex);

    probes.clear();
    unsigned level = maxLevel;

    // Start collecting runs on the max level, which has the largest runs
    // and therefore requires the least random reads
    while (level > 0) {
        size_t index = findRun(startLSN, level);

        ProbeResult res;
        res.level = level;
        while ((int) index <= lastFinished[level]) {
            if (runs[level][index].entries.size() > 0) {
                res.pidBegin = startPID;
                res.pidEnd = endPID;
                res.runIndex = index;
                probeInRun(res);
                probes.push_back(res);
            }
            index++;
        }

        // Now go to the next level, starting on the last LSN covered
        // by the current level
        startLSN = res.runEnd;
        level--;
    }
}

void ArchiveIndex::dumpIndex(ostream& out)
{
    for (auto r : runs) {
        for (size_t i = 0; i < r.size(); i++) {
            size_t offset = 0, prevOffset = 0;
            for (size_t j = 0; j < r[i].entries.size(); j++) {
                offset = r[i].entries[j].offset;
                out << "run " << i << " entry " << j <<
                    " pid " << r[i].entries[j].pid <<
                    " offset " << offset <<
                    " delta " << offset - prevOffset <<
                    endl;
                prevOffset = offset;
            }
        }
    }
}

MergerDaemon::MergerDaemon(ArchiveDirectory* in,
        ArchiveDirectory* out)
    : indir(in), outdir(out)
{
    if (!outdir) { outdir = indir; }
    w_assert0(indir && outdir);
}

typedef ArchiveDirectory::RunFileStats RunFileStats;

bool runComp(const RunFileStats& a, const RunFileStats& b)
{
    return a.beginLSN < b.beginLSN;
}

// CS TODO: this currently only works when merging contiguous runs in ascending
// order, and only for all available runs at once. It fits the purposes of our
// restore experiments, but it should be fixed in the future. See comments in
// the header file.
rc_t MergerDaemon::runSync(unsigned level, unsigned fanin)
{
    list<RunFileStats> stats, statsNext;
    indir->listFileStats(stats, level);
    indir->listFileStats(statsNext, level+1);

    // sort list by LSN, since only contiguous runs are merged
    stats.sort(runComp);
    statsNext.sort(runComp);

    // grab first LSN which is missing from next level
    lsn_t nextLSN = stats.front().beginLSN;
    if (statsNext.size() > 0) {
        nextLSN = statsNext.back().endLSN;
    }
    w_assert1(nextLSN < stats.back().endLSN);

    // collect 'fanin' runs in the current level starting from nextLSN
    auto begin = stats.begin();
    while (begin->endLSN <= nextLSN) { begin++; }
    auto end = begin;
    unsigned count = 0;
    while (count < fanin && end != stats.end()) {
        end++;
        count++;
    }
    if (count < 2) {
        ERROUT(<< "Not enough runs to merge");
        return RCOK;
    }

    {
        ArchiveScanner::RunMerger merger;
        BlockAssembly blkAssemb(outdir, level+1);

        ERROUT(<< "doMerge");
        list<RunFileStats>::const_iterator iter = begin;
        while (iter != end) {
            ERROUT(<< "Merging " << iter->beginLSN << "-" << iter->endLSN);
            ArchiveScanner::RunScanner* runScanner =
                new ArchiveScanner::RunScanner(
                        iter->beginLSN, iter->endLSN, iter->level, 0, 0, 0 /* offset */, indir);

            merger.addInput(runScanner);
            iter++;
        }

        constexpr int runNumber = 0;
        if (merger.heapSize() > 0) {
            logrec_t* lr;
            blkAssemb.start(runNumber);
            while (merger.next(lr)) {
                if (!blkAssemb.add(lr)) {
                    blkAssemb.finish();
                    blkAssemb.start(runNumber);
                    blkAssemb.add(lr);
                }
            }
            blkAssemb.finish();
        }

        blkAssemb.shutdown();
    }

    return RCOK;
}

