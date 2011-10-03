/* -*- mode:C++; c-basic-offset:4 -*-
     Shore-MT -- Multi-threaded port of the SHORE storage manager
   
                       Copyright (c) 2007-2009
      Data Intensive Applications and Systems Labaratory (DIAS)
               Ecole Polytechnique Federale de Lausanne
   
                         All Rights Reserved.
   
   Permission to use, copy, modify and distribute this software and
   its documentation is hereby granted, provided that both the
   copyright notice and this permission notice appear in all copies of
   the software, derivative works or modified versions, and any
   portions thereof, and that both notices appear in supporting
   documentation.
   
   This code is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. THE AUTHORS
   DISCLAIM ANY LIABILITY OF ANY KIND FOR ANY DAMAGES WHATSOEVER
   RESULTING FROM THE USE OF THIS SOFTWARE.
*/

/*<std-header orig-src='shore' incl-file-exclusion='W_ERROR_H'>

 $Id: w_error.h,v 1.62 2010/12/08 17:37:37 nhall Exp $

SHORE -- Scalable Heterogeneous Object REpository

Copyright (c) 1994-99 Computer Sciences Department, University of
                      Wisconsin -- Madison
All Rights Reserved.

Permission to use, copy, modify and distribute this software and its
documentation is hereby granted, provided that both the copyright
notice and this permission notice appear in all copies of the
software, derivative works or modified versions, and any portions
thereof, and that both notices appear in supporting documentation.

THE AUTHORS AND THE COMPUTER SCIENCES DEPARTMENT OF THE UNIVERSITY
OF WISCONSIN - MADISON ALLOW FREE USE OF THIS SOFTWARE IN ITS
"AS IS" CONDITION, AND THEY DISCLAIM ANY LIABILITY OF ANY KIND
FOR ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.

This software was developed with support by the Advanced Research
Project Agency, ARPA order number 018 (formerly 8230), monitored by
the U.S. Army Research Laboratory under contract DAAB07-91-C-Q518.
Further funding for this work was provided by DARPA through
Rome Research Laboratory Contract No. F30602-97-2-0247.

*/

#ifndef W_ERROR_H
#define W_ERROR_H

#include "w_defines.h"

/*  -- do not edit anything above this line --   </std-header>*/


#ifdef __GNUG__
#pragma interface
#endif

#include "fc_error_enum_gen.h"
#include "tls.h"
#include "w_base.h"

#define USE_BLOCK_ALLOC_FOR_W_ERROR_T 1
#if USE_BLOCK_ALLOC_FOR_W_ERROR_T
DECLARE_TLS_SCHWARZ(w_error_alloc);
#endif

/**\brief Error code and associated string.
 *
 * used by w_error_t.
 */
struct w_error_info_t {
    uint32_t        err_num;
    const char                *errstr;
};


/**\brief These are pushed onto a stack(list) hanging off a w_rc_t, q.v.
 *
 * \attention Not for direct use. Included in documentation only for completeness.
 */
class w_error_t : public w_base_t {
public:
    typedef w_error_info_t info_t;
    /**\brief Integer-valued error code
     *
     * The domain for this type is a set of numbers
     * generated by Perl scripts, and found in header files of the
     * form *_gen.h
     *
     */
    typedef uint32_t        err_num_t;

    // kludge: make err_num come first:
    const err_num_t              err_num;

    const char* const            file;
    const uint32_t                line;
    const int32_t                 sys_err_num;

    w_error_t*                   next() { return _next; }
    w_error_t const*             next() const { return _next; }

    w_error_t&                   add_trace_info(
        const char* const             filename,
        uint32_t                       line_num);

    w_error_t&                   clear_more_info_msg();
    w_error_t&                   append_more_info_msg(const char* more_info);
    const char*                  get_more_info_msg() const;
    
    ostream                      &print_error(ostream &o) const;

#if USE_BLOCK_ALLOC_FOR_W_ERROR_T
    void operator delete(void* p);

    /* The following grunge is so that we can catch any cases
    * of deleting a w_error_t that are not through the
    * operator delete that we defined.
    */
#if W_DEBUG_LEVEL > 2
#define DEBUG_BLOCK_ALLOC_MARK_FOR_DELETION(p) if(p) (p)->debug_mark_for_deletion();
#define CHECK_DEBUG_BLOCK_ALLOC_MARKED_FOR_DELETION(p)  \
        if(p && p != no_error) {w_assert0((p)->debug_is_marked_for_deletion() ); }
private:  
    bool marked;
    void debug_mark_for_deletion() { marked = true; }
    bool debug_is_marked_for_deletion() const { return marked == true; }
public:
#else
#define DEBUG_BLOCK_ALLOC_MARK_FOR_DELETION(p)
#define CHECK_DEBUG_BLOCK_ALLOC_MARKED_FOR_DELETION(p) 
#endif
#else
#define CHECK_DEBUG_BLOCK_ALLOC_MARKED_FOR_DELETION(p) 
#endif
    
    static w_error_t*            make(
        const char* const            filename,
        uint32_t                      line_num,
        err_num_t                    err_num,
        w_error_t*                   list = 0,
        const char*                  more_info = 0);
    static w_error_t*            make(
        const char* const             filename,
        uint32_t                       line_num,
        err_num_t                     err_num,
        uint32_t                       sys_err,
        w_error_t*                    list = 0,
        const char*                   more_info = 0);

    static bool                  insert(
        const char                    *modulename,
        const info_t                  info[],
        uint32_t                       count);

    static const w_error_t       no_error_instance;
    static w_error_t* const      no_error;
    static const char*           error_string(err_num_t err_num);
    static const char*           module_name(err_num_t err_num);

    NORET                        ~w_error_t();

private:
    enum { max_range = 10, max_trace = 10 };
    
    
private:
    const char*                  more_info_msg;

    friend class w_rc_t;
                                     
    uint32_t                      _trace_cnt;
    w_error_t*                   _next;
    const char*                  _trace_file[max_trace];
    uint32_t                      _trace_line[max_trace];

    NORET                        w_error_t(
        const char* const            filename,
        uint32_t                      line_num,
        err_num_t                    err_num,
        w_error_t*                   list,
        const char*                  more_info);
    NORET                        w_error_t(
        const char* const             filename,
        uint32_t                       line_num,
        err_num_t                     err_num,
        uint32_t                       sys_err,
        w_error_t*                    list,
        const char*                    more_info);

    // disabled. 
    NORET                        w_error_t(const w_error_t&);
    w_error_t&                   operator=(const w_error_t&);

    static const info_t*         _range_start[max_range];
    static uint32_t               _range_cnt[max_range];
    static const char *          _range_name[max_range];
    static uint32_t               _nreg;

    static inline uint32_t        classify(int err_num);
public:
        // make public so it  can be exported to client side
    static const info_t          error_info[];
    static ostream &             print(ostream &out);
private:
    // disabled
    static void init_errorcodes(); 

};

extern ostream  &operator<<(ostream &o, const w_error_t &obj);

#if W_DEBUG_LEVEL > 1
#define CHECKIT do {\
        w_error_t*    my = _next; \
        w_error_t*    p = my; \
        while(p) { \
        if (p == p->_next || my == p->_next) { \
            cerr << "Recursive error detected:" << endl << *this << endl;\
            w_assert0(0); \
        } \
        p = p->_next; \
        } \
  } while(0)

#else
#define CHECKIT
#endif


inline NORET
w_error_t::~w_error_t()
{
    // CHECKIT;
    CHECK_DEBUG_BLOCK_ALLOC_MARKED_FOR_DELETION((w_error_t *)this)
    delete[] more_info_msg;
    more_info_msg = NULL;
#if USE_BLOCK_ALLOC_FOR_W_ERROR_T
    w_error_t::operator delete(_next); // make sure the right delete is used.
#else
    delete _next;
#endif
    _next = NULL;
}

/*<std-footer incl-file-exclusion='W_ERROR_H'>  -- do not edit anything below this line -- */

#endif          /*</std-footer>*/
