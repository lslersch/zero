#include <iostream>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>

#include "w_stream.h"
#include "w.h"
#include "w_strstream.h"
#include "sm_vas.h"
#include "sm_base.h"
#include "generic_page.h"
#include "bf.h"
#include "smthread.h"
#include "btree.h"
#include "btcursor.h"
#include "btree_impl.h"
#include "btree_page_h.h"
#include "btree_test_env.h"
#include "sm_options.h"
#include "xct.h"
#include "backup.h"

#include "../nullbuf.h"
#if W_DEBUG_LEVEL <= 3
nullbuf null_obj;
std::ostream vout (&null_obj);
#endif // W_DEBUG_LEVEL
namespace {
    char device_name[MAXPATHLEN] = "./volumes/dev_test";
    char global_log_dir[MAXPATHLEN] = "./log";
    char global_backup_dir[MAXPATHLEN] = "./backups";
}

sm_options btree_test_env::make_sm_options(
            int32_t locktable_size,
            int bufferpool_size_in_pages,
            uint32_t cleaner_threads,
            uint32_t cleaner_interval_millisec_min,
            uint32_t cleaner_interval_millisec_max,
            uint32_t cleaner_write_buffer_pages,
            bool initially_enable_cleaners,
            bool enable_swizzling,
            const std::vector<std::pair<const char*, int64_t> > &additional_int_params,
            const std::vector<std::pair<const char*, bool> > &additional_bool_params,
            const std::vector<std::pair<const char*, const char*> > &additional_string_params) {
    sm_options options;
    options.set_int_option("sm_bufpoolsize", SM_PAGESIZE / 1024 * bufferpool_size_in_pages);
    options.set_int_option("sm_locktablesize", locktable_size);
    options.set_string_option("sm_logdir", global_log_dir);
    options.set_int_option("sm_num_page_writers", cleaner_threads);
    options.set_int_option("sm_cleaner_interval_millisec_min", cleaner_interval_millisec_min);
    options.set_int_option("sm_cleaner_interval_millisec_max", cleaner_interval_millisec_max);
    options.set_int_option("sm_cleaner_write_buffer_pages", cleaner_write_buffer_pages);
    options.set_bool_option("sm_backgroundflush", initially_enable_cleaners);
    options.set_bool_option("sm_bufferpool_swizzle", enable_swizzling);

    for (std::vector<std::pair<const char*, int64_t> >::const_iterator iter = additional_int_params.begin();
            iter != additional_int_params.end(); ++iter) {
        std::cout << "additional int parameter: " << iter->first << "=" << iter->second << std::endl;
        options.set_int_option(iter->first, iter->second);
    }
    for (std::vector<std::pair<const char*, bool> >::const_iterator iter = additional_bool_params.begin();
            iter != additional_bool_params.end(); ++iter) {
        std::cout << "additional bool parameter: " << iter->first << "=" << iter->second << std::endl;
        options.set_bool_option(iter->first, iter->second);
    }
    for (std::vector<std::pair<const char*, const char*> >::const_iterator iter = additional_string_params.begin();
            iter != additional_string_params.end(); ++iter) {
        std::cout << "additional string parameter: " << iter->first << "=" << iter->second << std::endl;
        options.set_string_option(iter->first, iter->second);
    }
    return options;
}
sm_options btree_test_env::make_sm_options(
            int32_t locktable_size,
            int bufferpool_size_in_pages,
            uint32_t cleaner_threads,
            uint32_t cleaner_interval_millisec_min,
            uint32_t cleaner_interval_millisec_max,
            uint32_t cleaner_write_buffer_pages,
            bool initially_enable_cleaners,
            bool enable_swizzling) {
    std::vector<std::pair<const char*, int64_t> > dummy_int;
    std::vector<std::pair<const char*, bool> > dummy_bool;
    std::vector<std::pair<const char*, const char*> > dummy_string;
    return make_sm_options(locktable_size,
            bufferpool_size_in_pages,
            cleaner_threads,
            cleaner_interval_millisec_min,
            cleaner_interval_millisec_max,
            cleaner_write_buffer_pages,
            initially_enable_cleaners,
            enable_swizzling,
            dummy_int, dummy_bool, dummy_string);
}

/** thread object to host Btree test functors. */
class testdriver_thread_t : public smthread_t {
public:

        testdriver_thread_t(test_functor *functor,
            btree_test_env *env,
            int disk_quota_in_pages,
            const sm_options &options)
                : smthread_t(t_regular, "testdriver_thread_t"),
                _env(env),
                _options(options),
                _disk_quota_in_pages(disk_quota_in_pages),
                _retval(0),
                _functor(functor) {
            // complement required options if not set
            std::string not_set("not_set");
            int not_set_int = -1;
            if (_options.get_string_option("sm_logdir", not_set) == not_set) {
                _options.set_string_option("sm_logdir", global_log_dir);
            }
            if (_options.get_string_option("sm_backup_dir", not_set) == not_set) {
                _options.set_string_option("sm_backup_dir", global_backup_dir);
            }
            if (_options.get_int_option("sm_bufpoolsize", not_set_int) == not_set_int) {
                _options.set_int_option("sm_bufpoolsize",
                            SM_PAGESIZE / 1024 * default_bufferpool_size_in_pages);
            }
        }

        ~testdriver_thread_t()  {}

        void run();
        int  return_value() const { return _retval; }


private:
        w_rc_t do_init(ss_m &ssm);
    
        btree_test_env *_env;
        sm_options      _options; // run-time options
        int             _disk_quota_in_pages;
        int             _retval; // return value from run()
        test_functor*   _functor;// test functor object
};

rc_t
testdriver_thread_t::do_init(ss_m &ssm)
{
    const int quota_in_kb = SM_PAGESIZE / 1024 * _disk_quota_in_pages;
    if (_functor->_need_init) {
        _functor->_test_volume._device_name = device_name;
        _functor->_test_volume._vid = 1;

        vout << "Formatting device: " << _functor->_test_volume._device_name 
                << " with a " << quota_in_kb << "KB quota ..." << endl;
        W_DO(ssm.format_dev(_functor->_test_volume._device_name, quota_in_kb, true));
    } else {
        w_assert0(_functor->_test_volume._device_name);
    }    

    devid_t        devid;
    vout << "Mounting device: " << _functor->_test_volume._device_name  << endl;
    // mount the new device
    u_int        vol_cnt;
    W_DO(ssm.mount_dev(_functor->_test_volume._device_name, vol_cnt, devid));

    vout << "Mounted device: " << _functor->_test_volume._device_name  
            << " volume count " << vol_cnt
            << " device " << devid
            << endl;

    if (_functor->_need_init) {
        // generate a volume ID for the new volume we are about to
        // create on the device
        vout << "Generating new lvid: " << endl;
        W_DO(ssm.generate_new_lvid(_functor->_test_volume._lvid));
        vout << "Generated lvid " << _functor->_test_volume._lvid <<  endl;

        // create the new volume 
        vout << "Creating a new volume on the device" << endl;
        vout << "    with a " << quota_in_kb << "KB quota ..." << endl;

        W_DO(ssm.create_vol(_functor->_test_volume._device_name, _functor->_test_volume._lvid,
                        quota_in_kb, false, _functor->_test_volume._vid));
        vout << "    with local handle(phys volid) " << _functor->_test_volume._vid << endl;
    } else {
        w_assert0(_functor->_test_volume._vid != vid_t::null);
        w_assert0(_functor->_test_volume._lvid != lvid_t::null);
    }

    return RCOK;
}

void testdriver_thread_t::run()
{
    // Now start a storage manager.
    vout << "Starting SSM and performing recovery ..." << endl;
    {
        ss_m ssm (_options);
        _env->_ssm = &ssm;

        sm_config_info_t config_info;
        rc_t rc = ss_m::config_info(config_info);
        if(rc.is_error()) {
            cerr << "Could not get storage manager configuration info: " << rc << endl; 
            _retval = 1;
            return;
        }
        rc = do_init(ssm);
        if(rc.is_error()) {
            cerr << "Init failed: " << rc << endl; 
            _retval = 1;
            return;
        }

        rc = _functor->run_test (&ssm);
        if(rc.is_error()) {
            cerr << "Failure: " << rc << endl; 
            _retval = 1;
            return;
        }
        
        if (!_functor->_clean_shutdown) {
            ssm.set_shutdown_flag(false);
        }

        // end of ssm scope
        // Clean up and shut down
        vout << "\nShutting down SSM " << (_functor->_clean_shutdown ? "cleanly" : "simulating a crash") << "..." << endl;
        _env->_ssm = NULL;
    }
    vout << "Finished!" << endl;
}


btree_test_env::btree_test_env()
{
    _ssm = NULL;
    _use_locks = false;
}

btree_test_env::~btree_test_env()
{
}


void btree_test_env::assure_empty_dir(const char *folder_name)
{
    assure_dir(folder_name);
    empty_dir(folder_name);
}
void btree_test_env::assure_dir(const char *folder_name)
{
    vout << "creating folder '" << folder_name << "' if not exists..." << endl;
    if (mkdir(folder_name, S_IRWXU) == 0) {
        vout << "created." << endl;
    } else {
        if (errno == EEXIST) {
            vout << "already exists." << endl;
        } else {
            cerr << "couldn't create folder. error:" << errno << "." << endl;
            ASSERT_TRUE(false);
        }
    }
}

void btree_test_env::empty_dir(const char *folder_name)
{
    // want to use boost::filesystem... but let's keep this project boost-free.
    vout << "removing existing files..." << endl;
    DIR *d = opendir(folder_name);
    ASSERT_TRUE(d != NULL);
    for (struct dirent *ent = readdir(d); ent != NULL; ent = readdir(d)) {
        // Skip "." and ".."
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) {
            continue;
        }
        std::stringstream buf;
        buf << folder_name << "/" << ent->d_name;
        std::remove (buf.str().c_str());
    }
    ASSERT_EQ(closedir (d), 0);
}
void btree_test_env::SetUp()
{
    char tests_dir[MAXPATHLEN] = "/dev/shm/";
    strcat(tests_dir, getenv("USER"));
    assure_dir(tests_dir);
    strcat(tests_dir, "/btree_test_env");
    assure_dir(tests_dir);
    strcpy(log_dir, tests_dir);
    strcpy(vol_dir, tests_dir);
    strcat(log_dir, "/log");
    strcat(vol_dir, "/volumes");
    assure_empty_dir(log_dir);
    assure_empty_dir(vol_dir);
    strcpy(device_name, vol_dir);
    strcat(device_name, "/dev_test");
    strcpy(global_log_dir, log_dir);
    _use_locks = false;
}
void btree_test_env::empty_logdata_dir()
{
    empty_dir(log_dir);
    empty_dir(vol_dir);
}

void btree_test_env::TearDown()
{
    
}

int btree_test_env::runBtreeTest (w_rc_t (*func)(ss_m*, test_volume_t*),
                    bool use_locks, int32_t lock_table_size,
                    int disk_quota_in_pages,
                    int bufferpool_size_in_pages,
                    uint32_t cleaner_threads,
                    uint32_t cleaner_interval_millisec_min,
                    uint32_t cleaner_interval_millisec_max,
                    uint32_t cleaner_write_buffer_pages,
                    bool initially_enable_cleaners,
                    bool enable_swizzling
                                 )
{
    return runBtreeTest(func, use_locks, disk_quota_in_pages,
                make_sm_options(lock_table_size,
                    bufferpool_size_in_pages,
                    cleaner_threads,
                    cleaner_interval_millisec_min,
                    cleaner_interval_millisec_max,
                    cleaner_write_buffer_pages,
                    initially_enable_cleaners,
                    enable_swizzling));
}

int btree_test_env::runBtreeTest (w_rc_t (*func)(ss_m*, test_volume_t*),
        bool use_locks, int32_t lock_table_size,
        int disk_quota_in_pages,
        int bufferpool_size_in_pages,
        uint32_t cleaner_threads,
        uint32_t cleaner_interval_millisec_min,
        uint32_t cleaner_interval_millisec_max,
        uint32_t cleaner_write_buffer_pages,
        bool initially_enable_cleaners,
        bool enable_swizzling,
        const std::vector<std::pair<const char*, int64_t> > &additional_int_params,
        const std::vector<std::pair<const char*, bool> > &additional_bool_params,
        const std::vector<std::pair<const char*, const char*> > &additional_string_params
) {
    return runBtreeTest(func, use_locks, disk_quota_in_pages,
                make_sm_options(lock_table_size,
                    bufferpool_size_in_pages,
                    cleaner_threads,
                    cleaner_interval_millisec_min,
                    cleaner_interval_millisec_max,
                    cleaner_write_buffer_pages,
                    initially_enable_cleaners,
                    enable_swizzling,
                    additional_int_params, additional_bool_params, additional_string_params));
}

int btree_test_env::runBtreeTest (w_rc_t (*func)(ss_m*, test_volume_t*),
    bool use_locks, int disk_quota_in_pages, const sm_options &options) {
    _use_locks = use_locks;
    int rv;
    {
        default_test_functor functor(func);
        testdriver_thread_t smtu(&functor, this, disk_quota_in_pages, options);

        /* cause the thread's run() method to start */
        w_rc_t e = smtu.fork();
        if(e.is_error()) {
            cerr << "Error forking thread: " << e <<endl;
            return 1;
        }

        /* wait for the thread's run() method to end */
        e = smtu.join();
        if(e.is_error()) {
            cerr << "Error joining thread: " << e <<endl;
            return 1;
        }

        rv = smtu.return_value();
    }
    return rv;
}

// Begin... for test_restart.cpp
int btree_test_env::runRestartTest (restart_test_base *context,
    bool fCrash,
    bool use_locks, int32_t lock_table_size,
    int disk_quota_in_pages, int bufferpool_size_in_pages,
    uint32_t cleaner_threads,
    uint32_t cleaner_interval_millisec_min,
    uint32_t cleaner_interval_millisec_max,
    uint32_t cleaner_write_buffer_pages,
    bool initially_enable_cleaners,
    bool enable_swizzling) {
    return runRestartTest(context, fCrash, use_locks, disk_quota_in_pages,
            make_sm_options(lock_table_size,
                    bufferpool_size_in_pages,
                    cleaner_threads,
                    cleaner_interval_millisec_min,
                    cleaner_interval_millisec_max,
                    cleaner_write_buffer_pages,
                    initially_enable_cleaners,
                    enable_swizzling));
}

int btree_test_env::runRestartTest (restart_test_base *context,
    bool fCrash,
    bool use_locks, int32_t lock_table_size,
    int disk_quota_in_pages, int bufferpool_size_in_pages,
    uint32_t cleaner_threads,
    uint32_t cleaner_interval_millisec_min,
    uint32_t cleaner_interval_millisec_max,
    uint32_t cleaner_write_buffer_pages,
    bool initially_enable_cleaners,
    bool enable_swizzling,
    const std::vector<std::pair<const char*, int64_t> > &additional_int_params,
    const std::vector<std::pair<const char*, bool> > &additional_bool_params,
    const std::vector<std::pair<const char*, const char*> > &additional_string_params) {
    return runRestartTest(context, fCrash, use_locks, disk_quota_in_pages,
        make_sm_options(lock_table_size,
                    bufferpool_size_in_pages,
                    cleaner_threads,
                    cleaner_interval_millisec_min,
                    cleaner_interval_millisec_max,
                    cleaner_write_buffer_pages,
                    initially_enable_cleaners,
                    enable_swizzling,
                    additional_int_params, additional_bool_params, additional_string_params));
}
int btree_test_env::runRestartTest (restart_test_base *context, bool fCrash, bool use_locks, int disk_quota_in_pages, const sm_options &options) {
    _use_locks = use_locks;

    DBGOUT2 ( << "Going to call pre_shutdown()...");
    int rv;
    w_rc_t e;
        {
        if (true == fCrash)
            {
            // Simulated crash
            restart_dirty_test_pre_functor functor(context);
            testdriver_thread_t smtu(&functor, this, disk_quota_in_pages, options);
            e = smtu.fork();
            if(e.is_error()) 
                {
                cerr << "Error forking thread while pre_shutdown: " << e <<endl;
                return 1;
                }
            e = smtu.join();
            if(e.is_error()) 
                {
                cerr << "Error joining thread while pre_shutdown: " << e <<endl;
                return 1;
                }

            rv = smtu.return_value();
            if (rv != 0) 
                {
                cerr << "Error while pre_shutdown rv= " << rv <<endl;
                return rv;
                }
            }
        else
            {
            // Clean shutdown
            restart_clean_test_pre_functor functor(context);
            testdriver_thread_t smtu(&functor, this, disk_quota_in_pages, options);
            e = smtu.fork();
            if(e.is_error()) 
                {
                cerr << "Error forking thread while pre_shutdown: " << e <<endl;
                return 1;
                }
            e = smtu.join();
            if(e.is_error()) 
                {
                cerr << "Error joining thread while pre_shutdown: " << e <<endl;
                return 1;
                }

            rv = smtu.return_value();
            if (rv != 0) 
                {
                cerr << "Error while pre_shutdown rv= " << rv <<endl;
                return rv;
                }
            }
        }
    DBGOUT2 ( << "Going to call post_shutdown()...");
        {
        restart_test_post_functor functor(context);
        testdriver_thread_t smtu(&functor, this, disk_quota_in_pages, options);

        w_rc_t e = smtu.fork();
        if(e.is_error()) 
            {
            cerr << "Error forking thread while post_shutdown: " << e <<endl;
            return 1;
            }

        e = smtu.join();
        if(e.is_error()) 
            {
            cerr << "Error joining thread while post_shutdown: " << e <<endl;
            return 1;
            }

        rv = smtu.return_value();
        }

    return rv;
        
}
// End... for test_restart.cpp

int btree_test_env::runCrashTest (crash_test_base *context,
    bool use_locks, int32_t lock_table_size,
    int disk_quota_in_pages, int bufferpool_size_in_pages,
    uint32_t cleaner_threads,
    uint32_t cleaner_interval_millisec_min,
    uint32_t cleaner_interval_millisec_max,
    uint32_t cleaner_write_buffer_pages,
    bool initially_enable_cleaners,
    bool enable_swizzling) {
    return runCrashTest(context, use_locks, disk_quota_in_pages,
                make_sm_options(lock_table_size,
                    bufferpool_size_in_pages,
                    cleaner_threads,
                    cleaner_interval_millisec_min,
                    cleaner_interval_millisec_max,
                    cleaner_write_buffer_pages,
                    initially_enable_cleaners,
                    enable_swizzling));
}

int btree_test_env::runCrashTest (crash_test_base *context,
    bool use_locks, int32_t lock_table_size,
    int disk_quota_in_pages, int bufferpool_size_in_pages,
    uint32_t cleaner_threads,
    uint32_t cleaner_interval_millisec_min,
    uint32_t cleaner_interval_millisec_max,
    uint32_t cleaner_write_buffer_pages,
    bool initially_enable_cleaners,
    bool enable_swizzling,
    const std::vector<std::pair<const char*, int64_t> > &additional_int_params,
    const std::vector<std::pair<const char*, bool> > &additional_bool_params,
    const std::vector<std::pair<const char*, const char*> > &additional_string_params) {
    return runCrashTest(context, use_locks, disk_quota_in_pages,
                make_sm_options(lock_table_size,
                    bufferpool_size_in_pages,
                    cleaner_threads,
                    cleaner_interval_millisec_min,
                    cleaner_interval_millisec_max,
                    cleaner_write_buffer_pages,
                    initially_enable_cleaners,
                    enable_swizzling,
                    additional_int_params, additional_bool_params, additional_string_params));
}
int btree_test_env::runCrashTest (crash_test_base *context, bool use_locks, int disk_quota_in_pages, const sm_options &options) {
    _use_locks = use_locks;

    DBGOUT2 ( << "Going to call pre_crash()...");
    int rv;
    {
        crash_test_pre_functor functor(context);
        testdriver_thread_t smtu(&functor, this,  disk_quota_in_pages, options);

        w_rc_t e = smtu.fork();
        if(e.is_error()) {
            cerr << "Error forking thread while pre_crash: " << e <<endl;
            return 1;
        }

        e = smtu.join();
        if(e.is_error()) {
            cerr << "Error joining thread while pre_crash: " << e <<endl;
            return 1;
        }

        rv = smtu.return_value();
        if (rv != 0) {
            cerr << "Error while pre_crash rv= " << rv <<endl;
            return rv;
        }
    }
    DBGOUT2 ( << "Crash simulated! going to call post_crash()...");
    {
        crash_test_post_functor functor(context);
        testdriver_thread_t smtu(&functor, this, disk_quota_in_pages, options);

        w_rc_t e = smtu.fork();
        if(e.is_error()) {
            cerr << "Error forking thread while post_crash: " << e <<endl;
            return 1;
        }

        e = smtu.join();
        if(e.is_error()) {
            cerr << "Error joining thread while post_crash: " << e <<endl;
            return 1;
        }

        rv = smtu.return_value();
    }

    return rv;
    
}


void btree_test_env::set_xct_query_lock() {
    if (_use_locks) {
        xct()->set_query_concurrency(smlevel_0::t_cc_keyrange);
    } else {
        xct()->set_query_concurrency(smlevel_0::t_cc_none);
    }
}

w_rc_t x_btree_create_index(ss_m* ssm, test_volume_t *test_volume, stid_t &stid, lpid_t &root_pid)
{
    W_DO(ssm->begin_xct());
    W_DO(ssm->create_index(test_volume->_vid, stid));
    W_DO(ssm->open_store(stid, root_pid));
    W_DO(ssm->commit_xct());
    return RCOK;
}

w_rc_t x_begin_xct(ss_m* ssm, bool use_locks)
{
    W_DO(ssm->begin_xct());
    if (use_locks) {
        xct()->set_query_concurrency(smlevel_0::t_cc_keyrange);
    }
    return RCOK;
}
w_rc_t x_commit_xct(ss_m* ssm)
{
    W_DO(ssm->commit_xct());
    return RCOK;
}

w_rc_t x_btree_get_root_pid(ss_m* ssm, const stid_t &stid, lpid_t &root_pid)
{
    W_DO(ssm->open_store_nolock(stid, root_pid));
    return RCOK;
}
w_rc_t x_btree_adopt_foster_all(ss_m* ssm, const stid_t &stid)
{
    lpid_t root_pid;
    W_DO (x_btree_get_root_pid (ssm, stid, root_pid));
    W_DO(ssm->begin_xct());
    {
        btree_page_h root_p;
        W_DO(root_p.fix_root(stid.vol.vol, stid.store, LATCH_EX));
        W_DO(btree_impl::_sx_adopt_foster_all(root_p, true));
    }
    W_DO(ssm->commit_xct());
    return RCOK;
}
w_rc_t x_btree_verify(ss_m* ssm, const stid_t &stid) {
    W_DO(ssm->begin_xct());
    bool consistent;
    W_DO(ssm->verify_index(stid, 19, consistent));
    EXPECT_TRUE (consistent) << "BTree verification of index " << stid << " failed";
    W_DO(ssm->commit_xct());
    if (!consistent) {
        return RC(eBADARGUMENT); // or some other thing
    }
    return RCOK;
}

// helper function for insert. keystr/datastr have to be NULL terminated
w_rc_t x_btree_lookup_and_commit(ss_m* ssm, const stid_t &stid, const char *keystr, std::string &data, bool use_locks) {
    W_DO(ssm->begin_xct());
    if (use_locks) {
        xct()->set_query_concurrency(smlevel_0::t_cc_keyrange);
    }
    rc_t rc = x_btree_lookup (ssm, stid, keystr, data);
    if (rc.is_error()) {
        W_DO (ssm->abort_xct());
    } else {
        W_DO(ssm->commit_xct());
    }
    return rc;
}
w_rc_t x_btree_lookup(ss_m* ssm, const stid_t &stid, const char *keystr, std::string &data) {
    w_keystr_t key;
    key.construct_regularkey(keystr, strlen(keystr));
    char buf[SM_PAGESIZE];
    smsize_t elen = SM_PAGESIZE;
    bool found;
    W_DO(ssm->find_assoc(stid, key, buf, elen, found));
    if (found) {
        data.assign(buf, elen);
    } else {
        data.clear();
    }
    return RCOK;
}

w_rc_t x_btree_insert(ss_m* ssm, const stid_t &stid, const char *keystr, const char *datastr) {
    w_keystr_t key;
    key.construct_regularkey(keystr, strlen(keystr));
    vec_t data;
    data.set(datastr, strlen(datastr));
    W_DO(ssm->create_assoc(stid, key, data));    
    return RCOK;
}
w_rc_t x_btree_insert_and_commit(ss_m* ssm, const stid_t &stid, const char *keystr, const char *datastr, bool use_locks) {
    W_DO(ssm->begin_xct());
    if (use_locks) {
        xct()->set_query_concurrency(smlevel_0::t_cc_keyrange);
    }
    rc_t rc = x_btree_insert (ssm, stid, keystr, datastr);
    if (rc.is_error()) {
        W_DO (ssm->abort_xct());
    } else {
        W_DO(ssm->commit_xct());
    }
    return rc;
}

// helper function for remove
w_rc_t x_btree_remove(ss_m* ssm, const stid_t &stid, const char *keystr) {
    w_keystr_t key;
    key.construct_regularkey(keystr, strlen(keystr));
    W_DO(ssm->destroy_assoc(stid, key));
    return RCOK;
}
w_rc_t x_btree_remove_and_commit(ss_m* ssm, const stid_t &stid, const char *keystr, bool use_locks) {
    W_DO(ssm->begin_xct());
    if (use_locks) {
        xct()->set_query_concurrency(smlevel_0::t_cc_keyrange);
    }
    rc_t rc = x_btree_remove(ssm, stid, keystr);
    if (rc.is_error()) {
        W_DO (ssm->abort_xct());
    } else {
        W_DO(ssm->commit_xct());
    }
    return rc;
}

w_rc_t x_btree_update_and_commit(ss_m* ssm, const stid_t &stid, const char *keystr, const char *datastr, bool use_locks)
{
    W_DO(ssm->begin_xct());
    if (use_locks) {
        xct()->set_query_concurrency(smlevel_0::t_cc_keyrange);
    }
    rc_t rc = x_btree_update(ssm, stid, keystr, datastr);
    if (rc.is_error()) {
        W_DO (ssm->abort_xct());
    } else {
        W_DO(ssm->commit_xct());
    }
    return rc;
}
w_rc_t x_btree_update(ss_m* ssm, const stid_t &stid, const char *keystr, const char *datastr)
{
    w_keystr_t key;
    key.construct_regularkey(keystr, strlen(keystr));
    vec_t data;
    data.set(datastr, strlen(datastr));
    W_DO(ssm->update_assoc(stid, key, data));    
    return RCOK;
}

w_rc_t x_btree_overwrite_and_commit(ss_m* ssm, const stid_t &stid, const char *keystr, const char *datastr, smsize_t offset, bool use_locks)
{
    W_DO(ssm->begin_xct());
    if (use_locks) {
        xct()->set_query_concurrency(smlevel_0::t_cc_keyrange);
    }
    rc_t rc = x_btree_overwrite(ssm, stid, keystr, datastr, offset);
    if (rc.is_error()) {
        W_DO (ssm->abort_xct());
    } else {
        W_DO(ssm->commit_xct());
    }
    return rc;
}
w_rc_t x_btree_overwrite(ss_m* ssm, const stid_t &stid, const char *keystr, const char *datastr, smsize_t offset)
{
    w_keystr_t key;
    key.construct_regularkey(keystr, strlen(keystr));
    smsize_t elen = strlen(datastr);
    W_DO(ssm->overwrite_assoc(stid, key, datastr, offset, elen));
    return RCOK;
}

// helper function to briefly check the stored contents in BTree
w_rc_t x_btree_scan(ss_m* ssm, const stid_t &stid, x_btree_scan_result &result, bool use_locks) {
    W_DO(ssm->begin_xct());
    if (use_locks) {
        xct()->set_query_concurrency(smlevel_0::t_cc_keyrange);
    }
    // fully scan the BTree
    bt_cursor_t cursor (stid.vol.vol, stid.store, true);
    
    result.rownum = 0;
    bool first_key = true;
    do {
        W_DO(cursor.next());
        if (cursor.eof()) {
            break;
        }
        if (first_key) {
            first_key = false;
            result.minkey.assign((const char*)cursor.key().serialize_as_nonkeystr().data(), cursor.key().get_length_as_nonkeystr());
        }
        result.maxkey.assign((const char*)cursor.key().serialize_as_nonkeystr().data(), cursor.key().get_length_as_nonkeystr());
        ++result.rownum;
    } while (true);
    W_DO(ssm->commit_xct());
    return RCOK;
}

/** Delete backup if exists. */
void x_delete_backup(ss_m* ssm, test_volume_t *test_volume) {
    BackupManager *bk = ssm->bk;
    std::string backup_path(bk->get_backup_path(test_volume->_vid));
    std::remove(backup_path.c_str());
}

/** Take a backup of the test volume. */
w_rc_t x_take_backup(ss_m* ssm, test_volume_t *test_volume) {
    // Flush the volume before taking backup
    W_DO(ssm->force_buffers());

    BackupManager *bk = ssm->bk;
    ::mkdir(bk->get_backup_folder().c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
    std::string backup_path(bk->get_backup_path(test_volume->_vid));
    std::ifstream copy_from(test_volume->_device_name, std::ios::binary);
    std::ofstream copy_to(backup_path.c_str(), std::ios::binary);
    copy_to << copy_from.rdbuf();

    return RCOK;
}
