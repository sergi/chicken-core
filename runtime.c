/* runtime.c - Runtime code for compiler generated executables
;
; Copyright (c) 2008-2014, The CHICKEN Team
; Copyright (c) 2000-2007, Felix L. Winkelmann
; All rights reserved.
;
; Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following
; conditions are met:
;
;   Redistributions of source code must retain the above copyright notice, this list of conditions and the following
;     disclaimer. 
;   Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following
;     disclaimer in the documentation and/or other materials provided with the distribution. 
;   Neither the name of the author nor the names of its contributors may be used to endorse or promote
;     products derived from this software without specific prior written permission. 
;
; THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS
; OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
; AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR
; CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
; CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
; SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
; THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
; OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
; POSSIBILITY OF SUCH DAMAGE.
*/


#include "chicken.h"
#include <assert.h>
#include <errno.h>
#include <float.h>
#include <signal.h>
#include <sys/stat.h>

#ifdef HAVE_SYSEXITS_H
# include <sysexits.h>
#endif

#ifdef __ANDROID__
# include <android/log.h>
#endif

#if !defined(PIC)
# define NO_DLOAD2
#endif

#ifndef NO_DLOAD2
# ifdef HAVE_DLFCN_H
#  include <dlfcn.h>
# endif

# ifdef HAVE_DL_H
#  include <dl.h>
# endif
#endif

#ifndef EX_SOFTWARE
# define EX_SOFTWARE  70
#endif

#ifndef EOVERFLOW
# define EOVERFLOW  0
#endif

/* TODO: Include sys/select.h? Windows doesn't seem to have it... */
#ifndef NO_POSIX_POLL
#  include <poll.h>
#endif

#if !defined(C_NONUNIX)

# include <sys/time.h>
# include <sys/resource.h>
# include <sys/wait.h>

#else

#ifdef ECOS
#include <cyg/kernel/kapi.h>
static C_TLS int timezone;
#define NSIG                          32
#endif

#endif

#ifndef RTLD_GLOBAL
# define RTLD_GLOBAL                   0
#endif

#ifndef RTLD_NOW
# define RTLD_NOW                      0
#endif

#ifndef RTLD_LOCAL
# define RTLD_LOCAL                    0
#endif

#ifndef RTLD_LAZY
# define RTLD_LAZY                     0
#endif

#if defined(_WIN32) && !defined(__CYGWIN__)
/* Include winsock2 to get select() for check_fd_ready() */
# include <winsock2.h>
# include <windows.h>
#endif

#ifdef HAVE_CONFIG_H
# ifdef PACKAGE
#  undef PACKAGE
# endif
# ifdef VERSION
#  undef VERSION
# endif
# include <chicken-config.h>

# ifndef HAVE_ALLOCA
#  error this package requires "alloca()"
# endif
#endif

#ifdef C_HACKED_APPLY
# if defined(C_MACOSX) || defined(__MINGW32__) || defined(__CYGWIN__)
extern void C_do_apply_hack(void *proc, C_word *args, int count) C_noret;
# else
extern void _C_do_apply_hack(void *proc, C_word *args, int count) C_noret;
#  define C_do_apply_hack _C_do_apply_hack
# endif
#endif

#if defined(C_NO_HACKED_APPLY) && defined(C_HACKED_APPLY)
# undef C_HACKED_APPLY
#endif

/* Parameters: */

#define RELAX_MULTIVAL_CHECK

#ifdef C_SIXTY_FOUR
# define DEFAULT_STACK_SIZE            (1024 * 1024)
#else
# define DEFAULT_STACK_SIZE            (256 * 1024)
#endif

#define DEFAULT_SYMBOL_TABLE_SIZE      2999
#define DEFAULT_HEAP_SIZE              DEFAULT_STACK_SIZE
#define MINIMAL_HEAP_SIZE              DEFAULT_STACK_SIZE
#define DEFAULT_MAXIMAL_HEAP_SIZE      0x7ffffff0
#define DEFAULT_HEAP_GROWTH            200
#define DEFAULT_HEAP_SHRINKAGE         50
#define DEFAULT_HEAP_SHRINKAGE_USED    25
#define DEFAULT_FORWARDING_TABLE_SIZE  32
#define DEFAULT_LOCATIVE_TABLE_SIZE    32
#define DEFAULT_COLLECTIBLES_SIZE      1024
#define DEFAULT_TRACE_BUFFER_SIZE      16
#define MIN_TRACE_BUFFER_SIZE          3

#define MAX_HASH_PREFIX                64

#define WEAK_TABLE_SIZE                997
#define WEAK_HASH_ITERATIONS           4
#define WEAK_HASH_DISPLACEMENT         7
#define WEAK_COUNTER_MASK              3
#define WEAK_COUNTER_MAX               2

#define TEMPORARY_STACK_SIZE	       2048
#define STRING_BUFFER_SIZE             4096
#define DEFAULT_MUTATION_STACK_SIZE    1024

#define MAX_PENDING_INTERRUPTS         100

#ifdef C_DOUBLE_IS_32_BITS
# define FLONUM_PRINT_PRECISION         7
#else
# define FLONUM_PRINT_PRECISION         15
#endif

#define WORDS_PER_FLONUM               C_SIZEOF_FLONUM
#define INITIAL_TIMER_INTERRUPT_PERIOD 10000
#define HDUMP_TABLE_SIZE               1001

/* only for relevant for Windows: */

#define MAXIMAL_NUMBER_OF_COMMAND_LINE_ARGUMENTS 256


/* Constants: */

#ifdef C_SIXTY_FOUR
# define ALIGNMENT_HOLE_MARKER         ((C_word)0xfffffffffffffffeL)
# define FORWARDING_BIT_SHIFT          63
# define UWORD_FORMAT_STRING           "0x%016lx"
# define UWORD_COUNT_FORMAT_STRING     "%u"
#else
# define ALIGNMENT_HOLE_MARKER         ((C_word)0xfffffffe)
# define FORWARDING_BIT_SHIFT          31
# define UWORD_FORMAT_STRING           "0x%08x"
# define UWORD_COUNT_FORMAT_STRING     "%u"
#endif

#ifdef C_LLP
# define LONG_FORMAT_STRING            "%lldf"
#else
# define LONG_FORMAT_STRING            "%ld"
#endif

#define GC_MINOR           0
#define GC_MAJOR           1
#define GC_REALLOC         2


/* Macros: */

#define nmax(x, y)                   ((x) > (y) ? (x) : (y))
#define nmin(x, y)                   ((x) < (y) ? (x) : (y))
#define percentage(n, p)             ((C_long)(((double)(n) * (double)p) / 100))

/* The bignum digit representation is fullword- little endian, so on
 * LE machines the halfdigits are numbered in the same order.  On BE
 * machines, we must swap the odd and even positions.
 */
#ifdef C_BIG_ENDIAN
#define C_uhword_ref(x, p)           ((C_uhword *)(x))[(p)^1]
#else
#define C_uhword_ref(x, p)           ((C_uhword *)(x))[(p)]
#endif
#define C_uhword_set(x, p, d)        (C_uhword_ref(x,p) = (d))

#define free_tmp_bignum(b)           C_free((void *)(b))
#define is_fptr(x)                   (((x) & C_GC_FORWARDING_BIT) != 0)
#define ptr_to_fptr(x)               ((((x) >> FORWARDING_BIT_SHIFT) & 1) | C_GC_FORWARDING_BIT | ((x) & ~1))
#define fptr_to_ptr(x)               (((x) << FORWARDING_BIT_SHIFT) | ((x) & ~(C_GC_FORWARDING_BIT | 1)))

#define C_check_real(x, w, v)       if(((x) & C_FIXNUM_BIT) != 0) v = C_unfix(x); \
                                     else if(C_immediatep(x) || C_block_header(x) != C_FLONUM_TAG) \
                                       barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, w, x); \
                                     else v = C_flonum_magnitude(x);


/* XXX TODO OBSOLETE: This can be removed after recompiling c-platform.scm */
#ifdef BITWISE_UINT_ONLY
#define C_check_uint(x, f, n, w)    if(((x) & C_FIXNUM_BIT) != 0) n = C_unfix(x); \
                                     else if(C_immediatep(x) || C_block_header(x) != C_FLONUM_TAG) \
                                       barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, w, x); \
                                     else { double _m; \
                                       f = C_flonum_magnitude(x); \
                                       if(modf(f, &_m) != 0.0 || f < 0 || f > C_UWORD_MAX) \
                                         barf(C_BAD_ARGUMENT_TYPE_NO_UINTEGER_ERROR, w, x); \
                                       else n = (C_uword)f; \
                                     }
#else
#define C_check_uint(x, f, n, w)    if(((x) & C_FIXNUM_BIT) != 0) n = C_unfix(x); \
                                      else if(C_immediatep(x) || C_block_header(x) != C_FLONUM_TAG) \
                                        barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, w, x); \
                                      else { double _m; \
                                        f = C_flonum_magnitude(x); \
                                        if(modf(f, &_m) != 0.0 || f > C_UWORD_MAX) \
                                          barf(C_BAD_ARGUMENT_TYPE_NO_UINTEGER_ERROR, w, x); \
                                        else n = (C_uword)f; \
                                      }
#endif

/* XXX TODO OBSOLETE: This can be removed after recompiling c-platform.scm */
#ifdef C_SIXTY_FOUR
#define C_limit_fixnum(n)            ((n) & C_MOST_POSITIVE_FIXNUM)
#else
#define C_limit_fixnum(n)            (n)
#endif

#define C_pte(name)                  pt[ i ].id = #name; pt[ i++ ].ptr = (void *)name;

#ifndef SIGBUS
# define SIGBUS                      0
#endif


/* Type definitions: */

typedef void (*TOPLEVEL)(C_word c, C_word self, C_word k) C_noret;
typedef void (C_fcall *TRAMPOLINE)(void *proc) C_regparm C_noret;

typedef struct lf_list_struct
{
  C_word *lf;
  int count;
  struct lf_list_struct *next, *prev;
  C_PTABLE_ENTRY *ptable;
  void *module_handle;
  char *module_name;
} LF_LIST;

typedef struct weak_table_entry_struct
{
  C_word item,			/* item weakly held (symbol) */
         container;		/* object holding reference to symbol, lowest 3 bits are */
} WEAK_TABLE_ENTRY;		/*   also used as a counter, saturated at 2 or more */

typedef struct finalizer_node_struct
{
  struct finalizer_node_struct
    *next,
    *previous;
  C_word
    item,
    finalizer;
} FINALIZER_NODE;

typedef struct trace_info_struct
{
  C_char *raw;
  C_word cooked1, cooked2, thread;
} TRACE_INFO;

typedef struct hdump_bucket_struct
{
  C_word key;
  int count, total;
  struct hdump_bucket_struct *next;
} HDUMP_BUCKET;


/* Variables: */

C_TLS C_word
  *C_temporary_stack,
  *C_temporary_stack_bottom,
  *C_temporary_stack_limit,
  *C_stack_limit,
   C_ratnum_type_tag,
   C_cplxnum_type_tag;
C_TLS C_long
  C_timer_interrupt_counter,
  C_initial_timer_interrupt_period;
C_TLS C_byte 
  *C_fromspace_top,
  *C_fromspace_limit;
#ifdef HAVE_SIGSETJMP
C_TLS sigjmp_buf C_restart;
#else
C_TLS jmp_buf C_restart;
#endif
C_TLS void *C_restart_address;
C_TLS int C_entry_point_status;
C_TLS int (*C_gc_mutation_hook)(C_word *slot, C_word val);
C_TLS void (*C_gc_trace_hook)(C_word *var, int mode);
C_TLS void (*C_panic_hook)(C_char *msg) = NULL;
C_TLS void (*C_pre_gc_hook)(int mode) = NULL;
C_TLS void (*C_post_gc_hook)(int mode, C_long ms) = NULL;
C_TLS void (C_fcall *C_restart_trampoline)(void *proc) C_regparm C_noret;

C_TLS int
  C_gui_mode = 0,
  C_abort_on_thread_exceptions,
  C_enable_repl,
  C_interrupts_enabled,
  C_disable_overflow_check,
#ifdef C_COLLECT_ALL_SYMBOLS
  C_enable_gcweak = 1,
#else
  C_enable_gcweak = 0,
#endif
  C_heap_size_is_fixed,
  C_trace_buffer_size = DEFAULT_TRACE_BUFFER_SIZE,
  C_max_pending_finalizers = C_DEFAULT_MAX_PENDING_FINALIZERS,
  C_main_argc;
C_TLS C_uword 
  C_heap_growth,
  C_heap_shrinkage;
C_TLS C_uword C_maximal_heap_size;
C_TLS time_t C_startup_time_seconds;
C_TLS char 
  **C_main_argv,
  *C_dlerror;

static C_TLS TRACE_INFO
  *trace_buffer,
  *trace_buffer_limit,
  *trace_buffer_top;

static C_TLS C_byte 
  *heapspace1, 
  *heapspace2,
  *fromspace_start,
  *tospace_start,
  *tospace_top,
  *tospace_limit,
  *new_tospace_start,
  *new_tospace_top,
  *new_tospace_limit,
  *heap_scan_top;
static C_TLS size_t
  heapspace1_size,
  heapspace2_size,
  heap_size;
static C_TLS C_char 
  buffer[ STRING_BUFFER_SIZE ],
  *private_repository = NULL,
  *current_module_name,
  *save_string;
static C_TLS C_SYMBOL_TABLE
  *symbol_table,
  *symbol_table_list;
static C_TLS C_word 
  **collectibles,
  **collectibles_top,
  **collectibles_limit,
  *saved_stack_limit,
  **mutation_stack_bottom,
  **mutation_stack_limit,
  **mutation_stack_top,
  *stack_bottom,
  *locative_table,
  error_location,
  interrupt_hook_symbol,
  current_thread_symbol,
  error_hook_symbol,
  pending_finalizers_symbol,
  callback_continuation_stack_symbol,
  *forwarding_table;
static C_TLS int 
  trace_buffer_full,
  forwarding_table_size,
  return_to_host,
  page_size,
  show_trace,
  fake_tty_flag,
  debug_mode,
  dump_heap_on_exit,
  gc_bell,
  gc_report_flag = 0,
  gc_mode,
  gc_count_1,
  gc_count_1_total,
  gc_count_2,
  weak_table_randomization,
  stack_size_changed,
  dlopen_flags,
  heap_size_changed,
  chicken_is_running,
  chicken_ran_once,
  pass_serious_signals = 1,
  callback_continuation_level;
static volatile C_TLS int serious_signal_occurred = 0;
static C_TLS unsigned int
  mutation_count,
  tracked_mutation_count,
  stack_size;
static C_TLS int chicken_is_initialized;
#ifdef HAVE_SIGSETJMP
static C_TLS sigjmp_buf gc_restart;
#else
static C_TLS jmp_buf gc_restart;
#endif
static C_TLS double
  timer_start_ms,
  gc_ms,
  timer_accumulated_gc_ms,
  interrupt_time,
  last_interrupt_latency;
static C_TLS LF_LIST *lf_list;
static C_TLS int signal_mapping_table[ NSIG ];
static C_TLS int
  locative_table_size,
  locative_table_count,
  live_finalizer_count,
  allocated_finalizer_count,
  pending_finalizer_count,
  callback_returned_flag;
static C_TLS WEAK_TABLE_ENTRY *weak_item_table;
static C_TLS C_GC_ROOT *gc_root_list = NULL;
static C_TLS FINALIZER_NODE 
  *finalizer_list,
  *finalizer_free_list,
  **pending_finalizer_indices;
static C_TLS void *current_module_handle;
static C_TLS int flonum_print_precision = FLONUM_PRINT_PRECISION;
static C_TLS HDUMP_BUCKET **hdump_table;
static C_TLS int 
  pending_interrupts[ MAX_PENDING_INTERRUPTS ],
  pending_interrupts_count,
  handling_interrupts;


/* Prototypes: */

static void parse_argv(C_char *cmds);
static void initialize_symbol_table(void);
static void global_signal_handler(int signum);
static C_word arg_val(C_char *arg);
static void barf(int code, char *loc, ...) C_noret;
static void try_extended_number(char *ext_proc_name, C_word c, C_word k, ...) C_noret;
static void panic(C_char *msg) C_noret;
static void usual_panic(C_char *msg) C_noret;
static void horror(C_char *msg) C_noret;
static void C_fcall initial_trampoline(void *proc) C_regparm C_noret;
static C_ccall void termination_continuation(C_word c, C_word self, C_word result) C_noret;
static void C_fcall mark_system_globals(void) C_regparm;
static void C_fcall really_mark(C_word *x) C_regparm;
static WEAK_TABLE_ENTRY *C_fcall lookup_weak_table_entry(C_word item, C_word container) C_regparm;
static C_ccall void values_continuation(C_word c, C_word closure, C_word dummy, ...) C_noret;
static C_word add_symbol(C_word **ptr, C_word key, C_word string, C_SYMBOL_TABLE *stable);
static C_regparm int C_fcall C_in_new_heapp(C_word x);
static void bignum_negate_2(C_word c, C_word self, C_word new_big);
static void bignum_bitwise_and_2(C_word c, C_word self, C_word result) C_noret;
static void bignum_bitwise_ior_2(C_word c, C_word self, C_word result) C_noret;
static void bignum_bitwise_xor_2(C_word c, C_word self, C_word result) C_noret;
static void bignum_actual_shift(C_word c, C_word self, C_word result) C_noret;
static void bignum_times_bignum_unsigned(C_word k, C_word x, C_word y, C_word negp) C_noret;
static void bignum_times_bignum_unsigned_2(C_word c, C_word self, C_word result) C_noret;
static void integer_times_2(C_word c, C_word self, C_word new_big) C_noret;
static void bignum_plus_unsigned(C_word k, C_word x, C_word y, C_word negp) C_noret;
static void bignum_plus_unsigned_2(C_word c, C_word self, C_word result) C_noret;
static void bignum_minus_unsigned(C_word k, C_word x, C_word y) C_noret;
static void bignum_minus_unsigned_2(C_word c, C_word self, C_word result) C_noret;
static C_regparm void basic_divrem(C_word c, C_word self, C_word k, C_word x, C_word y, C_word return_r, C_word return_q) C_noret;
static C_regparm void integer_divrem(C_word c, C_word self, C_word k, C_word x, C_word y, C_word return_q, C_word return_r) C_noret;
static C_word bignum_remainder_unsigned_halfdigit(C_word num, C_word den);
static C_regparm void bignum_divrem(C_word c, C_word self, C_word k, C_word x, C_word y, C_word return_q, C_word return_r) C_noret;
static void divrem_intflo_2(C_word c, C_word self, ...) C_noret;
static void bignum_divrem_fixnum_2(C_word c, C_word self, C_word negated_big) C_noret;
static C_word rat_cmp(C_word x, C_word y);
static void flo_to_int_2(C_word c, C_word self, C_word result) C_noret;
static void fabs_frexp_to_digits(C_uword exp, double sign, C_uword *start, C_uword *scan);
static C_word flo_to_tmp_bignum(C_word x);
static C_word int_flo_cmp(C_word intnum, C_word flonum);
static C_word flo_int_cmp(C_word flonum, C_word intnum);
static C_word rat_flo_cmp(C_word ratnum, C_word flonum);
static C_word flo_rat_cmp(C_word flonum, C_word ratnum);
static C_word basic_cmp(C_word x, C_word y, char *loc, int eqp);
static int bignum_cmp_unsigned(C_word x, C_word y);
static C_word C_fcall hash_string(int len, C_char *str, C_word m, C_word r, int ci) C_regparm;
static C_word C_fcall lookup(C_word key, int len, C_char *str, C_SYMBOL_TABLE *stable) C_regparm;
static double compute_symbol_table_load(double *avg_bucket_len, int *total);
static C_word C_fcall convert_string_to_number(C_char *str, int radix, C_word *fix, double *flo) C_regparm;
static void digits_to_integer_2(C_word c, C_word self, C_word result) C_noret;
static C_regparm C_word str_to_bignum(C_word bignum, char *str, char *str_end, int radix);
static void bignum_to_str_2(C_word c, C_word self, C_word string) C_noret;
/* XXX TODO OBSOLETE: This can be removed after recompiling c-platform.scm */
static C_word C_fcall maybe_inexact_to_exact(C_word n) C_regparm;
static void C_fcall remark_system_globals(void) C_regparm;
static void C_fcall really_remark(C_word *x) C_regparm;
static C_word C_fcall intern0(C_char *name) C_regparm;
static void C_fcall update_locative_table(int mode) C_regparm;
static LF_LIST *find_module_handle(C_char *name);

static C_ccall void call_cc_wrapper(C_word c, C_word closure, C_word k, C_word result) C_noret;
static C_ccall void call_cc_values_wrapper(C_word c, C_word closure, C_word k, ...) C_noret;
static void gc_2(void *dummy) C_noret;
static void allocate_vector_2(void *dummy) C_noret;
static void allocate_bignum_2(void *dummy) C_noret;
static C_word allocate_tmp_bignum(C_word size, C_word negp, C_word initp);
static void bignum_digits_destructive_negate(C_word bignum);
static C_uword bignum_digits_destructive_scale_up_with_carry(C_uword *start, C_uword *end, C_uword factor, C_uword carry);
static C_uword bignum_digits_destructive_scale_down(C_uword *start, C_uword *end, C_uword denominator);
static C_uword bignum_digits_destructive_shift_right(C_uword *start, C_uword *end, int shift_right, int negp);
static C_uword bignum_digits_destructive_shift_left(C_uword *start, C_uword *end, int shift_left);
static C_regparm void bignum_digits_multiply(C_word x, C_word y, C_word result);
static void bignum_divide_2_unsigned(C_word c, C_word self, C_word quotient);
static void bignum_divide_2_unsigned_2(C_word c, C_word self, C_word remainder);
static void bignum_destructive_divide_unsigned_small(C_word c, C_word self, C_word quotient);
static C_regparm void bignum_destructive_divide_normalized(C_word big_u, C_word big_v, C_word big_q);
static void make_structure_2(void *dummy) C_noret;
static void generic_trampoline(void *dummy) C_noret;
static void handle_interrupt(void *trampoline, void *proc) C_noret;
static void callback_trampoline(void *dummy) C_noret;
static C_ccall void callback_return_continuation(C_word c, C_word self, C_word r) C_noret;
static void become_2(void *dummy) C_noret;
static void copy_closure_2(void *dummy) C_noret;
static void dump_heap_state_2(void *dummy) C_noret;
static void C_fcall sigsegv_trampoline(void *) C_regparm;
static void C_fcall sigill_trampoline(void *) C_regparm;
static void C_fcall sigfpe_trampoline(void *) C_regparm;
static void C_fcall sigbus_trampoline(void *) C_regparm;

static C_PTABLE_ENTRY *create_initial_ptable();

#if !defined(NO_DLOAD2) && (defined(HAVE_DLFCN_H) || defined(HAVE_DL_H) || (defined(HAVE_LOADLIBRARY) && defined(HAVE_GETPROCADDRESS)))
static void dload_2(void *dummy) C_noret;
#endif


static void
C_dbg(C_char *prefix, C_char *fstr, ...)
{
  va_list va;

  va_start(va, fstr);
#ifdef __ANDROID__
  __android_log_vprint(ANDROID_LOG_DEBUG, prefix, fstr, va);
#else
  C_fflush(C_stdout);
  C_fprintf(C_stderr, "[%s] ", prefix);
  C_vfprintf(C_stderr, fstr, va);
  C_fflush(C_stderr);
#endif
  va_end(va);
}


/* Startup code: */

int CHICKEN_main(int argc, char *argv[], void *toplevel) 
{
  C_word h, s, n;

  if(C_gui_mode) {
#ifdef _WIN32
    parse_argv(GetCommandLine());
    argc = C_main_argc;
    argv = C_main_argv;
#else
    /* ??? */
#endif
  }

  pass_serious_signals = 0;
  CHICKEN_parse_command_line(argc, argv, &h, &s, &n);
  
  if(!CHICKEN_initialize(h, s, n, toplevel))
    panic(C_text("cannot initialize - out of memory"));

  CHICKEN_run(NULL);
  return 0;
}


/* Custom argv parser for Windoze: */

void parse_argv(C_char *cmds)
{
  C_char *ptr = cmds,
         *bptr0, *bptr, *aptr;
  int n = 0;

  C_main_argv = (C_char **)malloc(MAXIMAL_NUMBER_OF_COMMAND_LINE_ARGUMENTS * sizeof(C_char *));

  if(C_main_argv == NULL)
    panic(C_text("cannot allocate argument-list buffer"));

  C_main_argc = 0;

  for(;;) {
    while(isspace((int)(*ptr))) ++ptr;

    if(*ptr == '\0') break;

    for(bptr0 = bptr = buffer; !isspace((int)(*ptr)) && *ptr != '\0'; *(bptr++) = *(ptr++))
      ++n;

    *bptr = '\0';

    aptr = (C_char*) malloc(sizeof(C_char) * (n + 1));
    if (!aptr)
      panic(C_text("cannot allocate argument buffer"));

    C_strlcpy(aptr, bptr0, sizeof(C_char) * (n + 1));

    C_main_argv[ C_main_argc++ ] = aptr;
  }
}


/* Initialize runtime system: */

int CHICKEN_initialize(int heap, int stack, int symbols, void *toplevel)
{
  int i;
#ifdef HAVE_SIGACTION
  struct sigaction sa;
#endif

  /*FIXME Should have C_tzset in chicken.h? */
#ifdef C_NONUNIX
  C_startup_time_seconds = (time_t)0;
# if defined(__MINGW32__)
  /* Make sure _tzname, _timezone, and _daylight are set */
  _tzset();
# endif
#else
  struct timeval tv;
  C_gettimeofday(&tv, NULL);
  C_startup_time_seconds = tv.tv_sec;
  /* Make sure tzname, timezone, and daylight are set */
  tzset();
#endif

  if(chicken_is_initialized) return 1;
  else chicken_is_initialized = 1;

#if defined(__ANDROID__) && defined(DEBUGBUILD)
  debug_mode = 2;
#endif

  if(debug_mode) 
    C_dbg(C_text("debug"), C_text("application startup...\n"));

  C_panic_hook = usual_panic;
  symbol_table_list = NULL;

  symbol_table = C_new_symbol_table(".", symbols ? symbols : DEFAULT_SYMBOL_TABLE_SIZE);

  if(symbol_table == NULL)
    return 0;

  page_size = 0;
  stack_size = stack ? stack : DEFAULT_STACK_SIZE;
  C_set_or_change_heap_size(heap ? heap : DEFAULT_HEAP_SIZE, 0);

  /* Allocate temporary stack: */
  if((C_temporary_stack_limit = (C_word *)C_malloc(TEMPORARY_STACK_SIZE * sizeof(C_word))) == NULL)
    return 0;
  
  C_temporary_stack_bottom = C_temporary_stack_limit + TEMPORARY_STACK_SIZE;
  C_temporary_stack = C_temporary_stack_bottom;
  
  /* Allocate mutation stack: */
  mutation_stack_bottom = (C_word **)C_malloc(DEFAULT_MUTATION_STACK_SIZE * sizeof(C_word *));

  if(mutation_stack_bottom == NULL) return 0;

  mutation_stack_top = mutation_stack_bottom;
  mutation_stack_limit = mutation_stack_bottom + DEFAULT_MUTATION_STACK_SIZE;
  C_gc_mutation_hook = NULL;
  C_gc_trace_hook = NULL;

  /* Allocate weak item table: */
  if(C_enable_gcweak) {
    weak_item_table = (WEAK_TABLE_ENTRY *)C_calloc(WEAK_TABLE_SIZE, sizeof(WEAK_TABLE_ENTRY));

    if(weak_item_table == NULL)
      return 0;
  }

  /* Initialize finalizer lists: */
  finalizer_list = NULL;
  finalizer_free_list = NULL;
  pending_finalizer_indices =
      (FINALIZER_NODE **)C_malloc(C_max_pending_finalizers * sizeof(FINALIZER_NODE *));

  if(pending_finalizer_indices == NULL) return 0;

  /* Initialize forwarding table: */
  forwarding_table =
      (C_word *)C_malloc((DEFAULT_FORWARDING_TABLE_SIZE + 1) * 2 * sizeof(C_word));

  if(forwarding_table == NULL) return 0;
  
  *forwarding_table = 0;
  forwarding_table_size = DEFAULT_FORWARDING_TABLE_SIZE;

  /* Initialize locative table: */
  locative_table = (C_word *)C_malloc(DEFAULT_LOCATIVE_TABLE_SIZE * sizeof(C_word));
   
  if(locative_table == NULL) return 0;
 
  locative_table_size = DEFAULT_LOCATIVE_TABLE_SIZE;
  locative_table_count = 0;

  /* Setup collectibles: */
  collectibles = (C_word **)C_malloc(sizeof(C_word *) * DEFAULT_COLLECTIBLES_SIZE);

  if(collectibles == NULL) return 0;

  collectibles_top = collectibles;
  collectibles_limit = collectibles + DEFAULT_COLLECTIBLES_SIZE;
  gc_root_list = NULL;
 
  /* Initialize global variables: */
  if(C_heap_growth <= 0) C_heap_growth = DEFAULT_HEAP_GROWTH;

  if(C_heap_shrinkage <= 0) C_heap_shrinkage = DEFAULT_HEAP_SHRINKAGE;

  if(C_maximal_heap_size <= 0) C_maximal_heap_size = DEFAULT_MAXIMAL_HEAP_SIZE;

#if !defined(NO_DLOAD2) && defined(HAVE_DLFCN_H)
  dlopen_flags = RTLD_LAZY | RTLD_GLOBAL;
#else
  dlopen_flags = 0;
#endif

  /* setup signal handlers */
  if(!pass_serious_signals) {
#ifdef HAVE_SIGACTION
    sa.sa_flags = 0;
    sigfillset(&sa.sa_mask); /* See note in C_establish_signal_handler() */
    sa.sa_handler = global_signal_handler;
    C_sigaction(SIGBUS, &sa, NULL);
    C_sigaction(SIGFPE, &sa, NULL);
    C_sigaction(SIGILL, &sa, NULL);
    C_sigaction(SIGSEGV, &sa, NULL);
#else
    C_signal(SIGBUS, global_signal_handler);
    C_signal(SIGILL, global_signal_handler);
    C_signal(SIGFPE, global_signal_handler);
    C_signal(SIGSEGV, global_signal_handler);
#endif
  }

  tracked_mutation_count = mutation_count = gc_count_1 = gc_count_1_total = gc_count_2 = 0;
  lf_list = NULL;
  C_register_lf2(NULL, 0, create_initial_ptable());
  C_restart_address = toplevel;
  C_restart_trampoline = initial_trampoline;
  trace_buffer = NULL;
  C_clear_trace_buffer();
  chicken_is_running = chicken_ran_once = 0;
  pending_interrupts_count = 0;
  handling_interrupts = 0;
  last_interrupt_latency = 0;
  C_interrupts_enabled = 1;
  C_initial_timer_interrupt_period = INITIAL_TIMER_INTERRUPT_PERIOD;
  C_timer_interrupt_counter = INITIAL_TIMER_INTERRUPT_PERIOD;
  memset(signal_mapping_table, 0, sizeof(int) * NSIG);
  initialize_symbol_table();
  C_dlerror = "cannot load compiled code dynamically - this is a statically linked executable";
  error_location = C_SCHEME_FALSE;
  C_pre_gc_hook = NULL;
  C_post_gc_hook = NULL;
  live_finalizer_count = 0;
  allocated_finalizer_count = 0;
  current_module_name = NULL;
  current_module_handle = NULL;
  callback_continuation_level = 0;
  gc_ms = 0;
  (void)C_randomize(C_fix(time(NULL)));
  return 1;
}


static C_PTABLE_ENTRY *create_initial_ptable()
{
  /* IMPORTANT: hardcoded table size -
     this must match the number of C_pte calls + 1 (NULL terminator)! */
  C_PTABLE_ENTRY *pt = (C_PTABLE_ENTRY *)C_malloc(sizeof(C_PTABLE_ENTRY) * 78);
  int i = 0;

  if(pt == NULL)
    panic(C_text("out of memory - cannot create initial ptable"));

  C_pte(termination_continuation);
  C_pte(callback_return_continuation);
  C_pte(values_continuation);
  C_pte(call_cc_values_wrapper);
  C_pte(call_cc_wrapper);
  C_pte(C_gc);
  C_pte(C_allocate_vector);
  C_pte(C_make_structure);
  C_pte(C_ensure_heap_reserve);
  C_pte(C_return_to_host);
  C_pte(C_get_symbol_table_info);
  C_pte(C_get_memory_info);
  C_pte(C_decode_seconds);
  C_pte(C_stop_timer);
  C_pte(C_dload);
  C_pte(C_set_dlopen_flags);
  C_pte(C_become);
  C_pte(C_apply_values);
  /* XXX TODO OBSOLETE: This can be removed after recompiling c-platform.scm */
  C_pte(C_times);
  /* XXX TODO OBSOLETE: This can be removed after recompiling c-platform.scm */
  C_pte(C_minus);
  /* XXX TODO OBSOLETE: This can be removed after recompiling c-platform.scm */
  C_pte(C_plus);
  /* XXX TODO OBSOLETE: This can be removed after recompiling c-platform.scm */
  C_pte(C_divide);
  C_pte(C_nequalp);
  C_pte(C_greaterp);
  /* IMPORTANT: have you read the comments at the start and the end of this function? */
  C_pte(C_lessp);
  C_pte(C_greater_or_equal_p);
  C_pte(C_less_or_equal_p);
  C_pte(C_quotient);
  C_pte(C_number_to_string);
  C_pte(C_make_symbol);
  C_pte(C_string_to_symbol);
  C_pte(C_apply);
  C_pte(C_call_cc);
  C_pte(C_values);
  C_pte(C_call_with_values);
  C_pte(C_continuation_graft);
  C_pte(C_open_file_port);
  C_pte(C_software_type);
  C_pte(C_machine_type);
  C_pte(C_machine_byte_order);
  C_pte(C_software_version);
  C_pte(C_build_platform);
  C_pte(C_make_pointer);
  C_pte(C_make_tagged_pointer);
  C_pte(C_peek_signed_integer);
  C_pte(C_peek_unsigned_integer);
  C_pte(C_context_switch);
  C_pte(C_register_finalizer);
  C_pte(C_locative_ref);
  C_pte(C_copy_closure);
  C_pte(C_dump_heap_state);
  C_pte(C_filter_heap_objects);
  C_pte(C_digits_to_integer);
  C_pte(C_number_to_string);
  C_pte(C_fixnum_to_string);
  C_pte(C_integer_to_string);
  C_pte(C_flonum_to_string);
  /* IMPORTANT: have you read the comments at the start and the end of this function? */
  C_pte(C_abs);
  C_pte(C_u_integer_abs);
  C_pte(C_negate);
  C_pte(C_u_integer_negate);
  C_pte(C_2_basic_plus);
  C_pte(C_2_basic_minus);
  C_pte(C_2_basic_times);
  C_pte(C_basic_quotient);
  C_pte(C_basic_remainder);
  C_pte(C_basic_divrem);
  C_pte(C_u_2_integer_plus);
  C_pte(C_u_2_integer_minus);
  C_pte(C_u_2_integer_times);
  C_pte(C_u_integer_quotient);
  C_pte(C_u_integer_remainder);
  C_pte(C_u_integer_divrem);
  C_pte(C_u_2_integer_bitwise_and);
  C_pte(C_u_2_integer_bitwise_ior);
  C_pte(C_u_2_integer_bitwise_xor);
  C_pte(C_u_integer_shift);

  /* IMPORTANT: did you remember the hardcoded pte table size? */
  pt[ i ].id = NULL;
  return pt;
}


void *CHICKEN_new_gc_root_2(int finalizable)
{
  C_GC_ROOT *r = (C_GC_ROOT *)C_malloc(sizeof(C_GC_ROOT));

  if(r == NULL)
    panic(C_text("out of memory - cannot allocate GC root"));

  r->value = C_SCHEME_UNDEFINED;
  r->next = gc_root_list;
  r->prev = NULL;
  r->finalizable = finalizable;

  if(gc_root_list != NULL) gc_root_list->prev = r;

  gc_root_list = r;
  return (void *)r;
}


void *CHICKEN_new_gc_root()
{
  return CHICKEN_new_gc_root_2(0);
}


void *CHICKEN_new_finalizable_gc_root()
{
  return CHICKEN_new_gc_root_2(1);
}


void CHICKEN_delete_gc_root(void *root)
{
  C_GC_ROOT *r = (C_GC_ROOT *)root;

  if(r->prev == NULL) gc_root_list = r->next;
  else r->prev->next = r->next;

  if(r->next != NULL) r->next->prev = r->prev;

  C_free(root);
}


void *CHICKEN_global_lookup(char *name)
{
  int 
    len = C_strlen(name),
    key = hash_string(len, name, symbol_table->size, symbol_table->rand, 0);
  C_word s;
  void *root = CHICKEN_new_gc_root();

  if(C_truep(s = lookup(key, len, name, symbol_table))) {
    if(C_block_item(s, 0) != C_SCHEME_UNBOUND) {
      CHICKEN_gc_root_set(root, s);
      return root;
    }
  }

  return NULL;
}


int CHICKEN_is_running()
{
  return chicken_is_running;
}


void CHICKEN_interrupt()
{
  C_timer_interrupt_counter = 0;
}


C_regparm C_SYMBOL_TABLE *C_new_symbol_table(char *name, unsigned int size)
{
  C_SYMBOL_TABLE *stp;
  int i;

  if((stp = C_find_symbol_table(name)) != NULL) return stp;

  if((stp = (C_SYMBOL_TABLE *)C_malloc(sizeof(C_SYMBOL_TABLE))) == NULL)
    return NULL;

  stp->name = name;
  stp->size = size;
  stp->next = symbol_table_list;
  stp->rand = rand();

  if((stp->table = (C_word *)C_malloc(size * sizeof(C_word))) == NULL)
    return NULL;

  for(i = 0; i < stp->size; stp->table[ i++ ] = C_SCHEME_END_OF_LIST);

  symbol_table_list = stp;
  return stp;
}  


C_regparm void C_delete_symbol_table(C_SYMBOL_TABLE *st)
{
  C_SYMBOL_TABLE *stp, *prev = NULL;

  for(stp = symbol_table_list; stp != NULL; stp = stp->next)
    if(stp == st) {
      if(prev != NULL) prev->next = stp->next;
      else symbol_table_list = stp->next;

      return;
    }
}


C_regparm void C_set_symbol_table(C_SYMBOL_TABLE *st)
{
  symbol_table = st;
}


C_regparm C_SYMBOL_TABLE *C_find_symbol_table(char *name)
{
  C_SYMBOL_TABLE *stp;

  for(stp = symbol_table_list; stp != NULL; stp = stp->next)
    if(!C_strcmp(name, stp->name)) return stp;

  return NULL;
}


C_regparm C_word C_find_symbol(C_word str, C_SYMBOL_TABLE *stable)
{
  char *sptr = C_c_string(str);
  int 
    len = C_header_size(str),
    key = hash_string(len, sptr, stable->size, stable->rand, 0);
  C_word s;

  if(C_truep(s = lookup(key, len, sptr, stable))) return s;
  else return C_SCHEME_FALSE;
}


/* Setup symbol-table with internally used symbols; */

void initialize_symbol_table(void)
{
  int i;

  for(i = 0; i < symbol_table->size; symbol_table->table[ i++ ] = C_SCHEME_END_OF_LIST);

  /* Obtain reference to hooks for later: */
  C_ratnum_type_tag = C_intern2(C_heaptop, C_text("\003sysratnum"));
  C_cplxnum_type_tag = C_intern2(C_heaptop, C_text("\003syscplxnum"));
  interrupt_hook_symbol = C_intern2(C_heaptop, C_text("\003sysinterrupt-hook"));
  error_hook_symbol = C_intern2(C_heaptop, C_text("\003syserror-hook"));
  callback_continuation_stack_symbol = C_intern3(C_heaptop, C_text("\003syscallback-continuation-stack"), C_SCHEME_END_OF_LIST);
  pending_finalizers_symbol = C_intern2(C_heaptop, C_text("\003syspending-finalizers"));
  current_thread_symbol = C_intern3(C_heaptop, C_text("\003syscurrent-thread"), C_SCHEME_FALSE);
}


C_regparm void C_fcall 
sigsegv_trampoline(void *dummy)
{
  barf(C_MEMORY_VIOLATION_ERROR, NULL);
}


C_regparm void C_fcall 
sigbus_trampoline(void *dummy)
{
  barf(C_BUS_ERROR, NULL);
}


C_regparm void C_fcall 
sigfpe_trampoline(void *dummy)
{
  barf(C_FLOATING_POINT_EXCEPTION_ERROR, NULL);
}


C_regparm void C_fcall 
sigill_trampoline(void *dummy)
{
  barf(C_ILLEGAL_INSTRUCTION_ERROR, NULL);
}


/* This is called from POSIX signals: */

void global_signal_handler(int signum)
{
#if defined(HAVE_SIGPROCMASK)
  if(signum == SIGSEGV || signum == SIGFPE || signum == SIGILL || signum == SIGBUS) {
    sigset_t sset;
    
    if(serious_signal_occurred || !chicken_is_running) {
      switch(signum) {
      case SIGSEGV: panic(C_text("unrecoverable segmentation violation"));
      case SIGFPE: panic(C_text("unrecoverable floating-point exception"));
      case SIGILL: panic(C_text("unrecoverable illegal instruction error"));
      case SIGBUS: panic(C_text("unrecoverable bus error"));
      default: panic(C_text("unrecoverable serious condition"));
      }
    }
    else serious_signal_occurred = 1;

    /* unblock signal to avoid nested invocation of the handler */
    sigemptyset(&sset);
    sigaddset(&sset, signum);
    C_sigprocmask(SIG_UNBLOCK, &sset, NULL);

    switch(signum) {
    case SIGSEGV: C_reclaim(sigsegv_trampoline, NULL);
    case SIGFPE: C_reclaim(sigfpe_trampoline, NULL);
    case SIGILL: C_reclaim(sigill_trampoline, NULL);
    case SIGBUS: C_reclaim(sigbus_trampoline, NULL);
    default: panic(C_text("invalid serious signal"));
    }
  }
#endif

  C_raise_interrupt(signal_mapping_table[ signum ]);
#ifndef HAVE_SIGACTION
  /* not necessarily needed, but older UNIXen may not leave the handler installed: */
  C_signal(signum, global_signal_handler);
#endif
}


/* Align memory to page boundary */

static void *align_to_page(void *mem)
{
  return (void *)C_align((C_uword)mem);
}


static C_byte *
heap_alloc (size_t size, C_byte **page_aligned)
{
  C_byte *p;
  p = (C_byte *)C_malloc (size + page_size);

  if (p != NULL && page_aligned) *page_aligned = align_to_page (p);

  return p;
}


static void
heap_free (C_byte *ptr, size_t size)
{
  C_free (ptr);
}


static C_byte *
heap_realloc (C_byte *ptr, size_t old_size,
	      size_t new_size, C_byte **page_aligned)
{
  C_byte *p;
  p = (C_byte *)C_realloc (ptr, new_size + page_size);

  if (p != NULL && page_aligned) *page_aligned = align_to_page (p);

  return p;
}


/* Modify heap size at runtime: */

void C_set_or_change_heap_size(C_word heap, int reintern)
{
  C_byte *ptr1, *ptr2, *ptr1a, *ptr2a;
  C_word size = heap / 2;

  if(heap_size_changed && fromspace_start) return;

  if(fromspace_start && heap_size >= heap) return;

  if(debug_mode)
    C_dbg(C_text("debug"), C_text("heap resized to %d bytes\n"), (int)heap);

  heap_size = heap;

  if((ptr1 = heap_realloc (fromspace_start,
			   C_fromspace_limit - fromspace_start,
			   size, &ptr1a)) == NULL ||
     (ptr2 = heap_realloc (tospace_start,
			   tospace_limit - tospace_start,
			   size, &ptr2a)) == NULL)
    panic(C_text("out of memory - cannot allocate heap"));

  heapspace1 = ptr1;
  heapspace1_size = size;
  heapspace2 = ptr2;
  heapspace2_size = size;
  fromspace_start = ptr1a;
  C_fromspace_top = fromspace_start;
  C_fromspace_limit = fromspace_start + size;
  tospace_start = ptr2a;
  tospace_top = tospace_start;
  tospace_limit = tospace_start + size;
  mutation_stack_top = mutation_stack_bottom;

  if(reintern) initialize_symbol_table();
}
 

/* Modify stack-size at runtime: */

void C_do_resize_stack(C_word stack)
{
  C_uword old = stack_size,
          diff = stack - old;

  if(diff != 0 && !stack_size_changed) {
    if(debug_mode) 
      C_dbg(C_text("debug"), C_text("stack resized to %d bytes\n"), (int)stack);

    stack_size = stack;

#if C_STACK_GROWS_DOWNWARD
    C_stack_limit = (C_word *)((C_byte *)C_stack_limit - diff);
#else
    C_stack_limit = (C_word *)((C_byte *)C_stack_limit + diff);
#endif
  }
}


/* Check whether nursery is sufficiently big: */

void C_check_nursery_minimum(C_word words)
{
  if(words >= C_bytestowords(stack_size))
    panic(C_text("nursery is too small - try higher setting using the `-:s' option"));
}

C_word C_resize_pending_finalizers(C_word size) {
  int sz = C_num_to_int(size);

  FINALIZER_NODE **newmem = 
    (FINALIZER_NODE **)C_realloc(pending_finalizer_indices, sz * sizeof(FINALIZER_NODE *));
  
  if (newmem == NULL)
    return C_SCHEME_FALSE;

  pending_finalizer_indices = newmem;
  C_max_pending_finalizers = sz;
  return C_SCHEME_TRUE;
}


/* Parse runtime options from command-line: */

void CHICKEN_parse_command_line(int argc, char *argv[], C_word *heap, C_word *stack, C_word *symbols)
{
  int i;
  char *ptr;
  C_word x;

  C_main_argc = argc;
  C_main_argv = argv;
  *heap = DEFAULT_HEAP_SIZE;
  *stack = DEFAULT_STACK_SIZE;
  *symbols = DEFAULT_SYMBOL_TABLE_SIZE;

  for(i = 1; i < C_main_argc; ++i)
    if(!strncmp(C_main_argv[ i ], C_text("-:"), 2)) {
      for(ptr = &C_main_argv[ i ][ 2 ]; *ptr != '\0';) {
	switch(*(ptr++)) {
	case '?':
	  C_dbg("Runtime options", "\n\n"
		 " -:?              display this text\n"
		 " -:c              always treat stdin as console\n"
		 " -:d              enable debug output\n"
		 " -:D              enable more debug output\n"
		 " -:g              show GC information\n"
		 " -:o              disable stack overflow checks\n"
		 " -:hiSIZE         set initial heap size\n"
		 " -:hmSIZE         set maximal heap size\n"
		 " -:hgPERCENTAGE   set heap growth percentage\n"
		 " -:hsPERCENTAGE   set heap shrink percentage\n"
		 " -:hSIZE          set fixed heap size\n"
		 " -:r              write trace output to stderr\n"
		 " -:sSIZE          set nursery (stack) size\n"
		 " -:tSIZE          set symbol-table size\n"
                 " -:fSIZE          set maximal number of pending finalizers\n"
		 " -:w              enable garbage collection of unused symbols\n"
		 " -:x              deliver uncaught exceptions of other threads to primordial one\n"
		 " -:b              enter REPL on error\n"
		 " -:B              sound bell on major GC\n"
		 " -:G              force GUI mode\n"
		 " -:aSIZE          set trace-buffer/call-chain size\n"
		 " -:H              dump heap state on exit\n"
		 " -:S              do not handle segfaults or other serious conditions\n"
		 "\n  SIZE may have a `k' (`K'), `m' (`M') or `g' (`G') suffix, meaning size\n"
		 "  times 1024, 1048576, and 1073741824, respectively.\n\n");
	  exit(0);

	case 'h':
	  switch(*ptr) {
	  case 'i':
	    *heap = arg_val(ptr + 1); 
	    heap_size_changed = 1;
	    goto next;
	  case 'g':
	    C_heap_growth = arg_val(ptr + 1);
	    goto next;
	  case 'm':
	    C_maximal_heap_size = arg_val(ptr + 1);
	    goto next;
	  case 's':
	    C_heap_shrinkage = arg_val(ptr + 1);
	    goto next;
	  default:
	    *heap = arg_val(ptr); 
	    heap_size_changed = 1;
	    C_heap_size_is_fixed = 1;
	    goto next;
	  }

	case 'o':
	  C_disable_overflow_check = 1;
	  break;

	case 'B':
	  gc_bell = 1;
	  break;

	case 'G':
	  C_gui_mode = 1;
	  break;

	case 'H':
	  dump_heap_on_exit = 1;
	  break;

	case 'S':
	  pass_serious_signals = 1;
	  break;

	case 's':
	  *stack = arg_val(ptr);
	  stack_size_changed = 1;
	  goto next;

	case 'f':
	  C_max_pending_finalizers = arg_val(ptr);
	  goto next;

	case 'a':
	  C_trace_buffer_size = arg_val(ptr);
	  goto next;

	case 't':
	  *symbols = arg_val(ptr);
	  goto next;

	case 'c':
	  fake_tty_flag = 1;
	  break;

	case 'd':
	  debug_mode = 1;
	  break;

	case 'D':
	  debug_mode = 2;
	  break;

	case 'g':
	  gc_report_flag = 2;
	  break;

	case 'w':
	  C_enable_gcweak = 1;
	  break;

	case 'r':
	  show_trace = 1;
	  break;

	case 'x':
	  C_abort_on_thread_exceptions = 1;
	  break;

	case 'b':
	  C_enable_repl = 1;
	  break;

	default: panic(C_text("illegal runtime option"));
	}
      }

    next:;
    }
}


C_word arg_val(C_char *arg)
{
  int len;
  C_char *end;
  C_long val, mul = 1;

  if (arg == NULL) panic(C_text("illegal runtime-option argument"));
      
  len = C_strlen(arg);
      
  if(len < 1) panic(C_text("illegal runtime-option argument"));
      
  switch(arg[ len - 1 ]) {
  case 'k':
  case 'K': mul = 1024; break;
 	  
  case 'm':
  case 'M': mul = 1024 * 1024; break;
 	  
  case 'g':
  case 'G': mul = 1024 * 1024 * 1024; break;

  default: mul = 1;
  }

  val = C_strtow(arg, &end, 10);

  if((mul != 1 ? end[ 1 ] != '\0' : end[ 0 ] != '\0')) 
    panic(C_text("invalid runtime-option argument suffix"));

  return val * mul;
}


/* Run embedded code with arguments: */

C_word CHICKEN_run(void *toplevel)
{
  if(!chicken_is_initialized && !CHICKEN_initialize(0, 0, 0, toplevel))
    panic(C_text("could not initialize"));

  if(chicken_is_running)
    panic(C_text("re-invocation of Scheme world while process is already running"));

  chicken_is_running = chicken_ran_once = 1;
  return_to_host = 0;

#if C_STACK_GROWS_DOWNWARD
  C_stack_limit = (C_word *)((C_byte *)C_stack_pointer - stack_size);
#else
  C_stack_limit = (C_word *)((C_byte *)C_stack_pointer + stack_size);
#endif

  stack_bottom = C_stack_pointer;

  if(debug_mode)
    C_dbg(C_text("debug"), C_text("stack bottom is 0x%lx.\n"), (C_word)stack_bottom);

  /* The point of (usually) no return... */
#ifdef HAVE_SIGSETJMP
  C_sigsetjmp(C_restart, 0);
#else
  C_setjmp(C_restart);
#endif

  serious_signal_occurred = 0;

  if(!return_to_host)
    (C_restart_trampoline)(C_restart_address);

  chicken_is_running = 0;
  return C_restore;
}


C_word CHICKEN_continue(C_word k)
{
  if(C_temporary_stack_bottom != C_temporary_stack)
    panic(C_text("invalid temporary stack level"));

  if(!chicken_is_initialized)
    panic(C_text("runtime system has not been initialized - `CHICKEN_run' has probably not been called"));

  C_save(k);
  return CHICKEN_run(NULL);
}


/* Trampoline called at system startup: */

C_regparm void C_fcall initial_trampoline(void *proc)
{
  TOPLEVEL top = (TOPLEVEL)proc;
  C_word closure = (C_word)C_alloc(C_SIZEOF_CLOSURE(1));

  ((C_SCHEME_BLOCK *)closure)->header = C_CLOSURE_TYPE | 1;
  C_set_block_item(closure, 0, (C_word)termination_continuation);
  (top)(2, C_SCHEME_UNDEFINED, closure);
}


/* The final continuation: */

void C_ccall termination_continuation(C_word c, C_word self, C_word result)
{
  if(debug_mode) {
    C_dbg(C_text("debug"), C_text("application terminated normally\n"));
  }

  exit(0);
}


/* Signal unrecoverable runtime error: */

void panic(C_char *msg)
{
  if(C_panic_hook != NULL) C_panic_hook(msg);

  usual_panic(msg);
}


void usual_panic(C_char *msg)
{
  C_char *dmp = C_dump_trace(0);

  C_dbg_hook(C_SCHEME_UNDEFINED);

  if(C_gui_mode) {
    C_snprintf(buffer, sizeof(buffer), C_text("%s\n\n%s"), msg, dmp);
#if defined(_WIN32) && !defined(__CYGWIN__)
    MessageBox(NULL, buffer, C_text("CHICKEN runtime"), MB_OK | MB_ICONERROR);
    ExitProcess(1);
#endif
  } /* fall through if not WIN32 GUI app */

  C_dbg("panic", C_text("%s - execution terminated\n\n%s"), msg, dmp);
  C_exit(1);
}


void horror(C_char *msg)
{
  C_dbg_hook(C_SCHEME_UNDEFINED);

  if(C_gui_mode) {
    C_snprintf(buffer, sizeof(buffer), C_text("%s"), msg);
#if defined(_WIN32) && !defined(__CYGWIN__)
    MessageBox(NULL, buffer, C_text("CHICKEN runtime"), MB_OK | MB_ICONERROR);
    ExitProcess(1);
#endif
  } /* fall through */

  C_dbg("horror", C_text("\n%s - execution terminated"), msg);  
  C_exit(1);
}


/* Error-hook, called from C-level runtime routines: */

void barf(int code, char *loc, ...)
{
  C_char *msg;
  C_word err = error_hook_symbol;
  int c, i;
  va_list v;

  C_dbg_hook(C_SCHEME_UNDEFINED);

  C_temporary_stack = C_temporary_stack_bottom;
  err = C_block_item(err, 0);

  if(C_immediatep(err))
    panic(C_text("`##sys#error-hook' is not defined - the `library' unit was probably not linked with this executable"));

  switch(code) {
  case C_BAD_ARGUMENT_COUNT_ERROR:
    msg = C_text("bad argument count");
    c = 3;
    break;

  case C_BAD_MINIMUM_ARGUMENT_COUNT_ERROR:
    msg = C_text("too few arguments");
    c = 3;
    break;
  
  case C_BAD_ARGUMENT_TYPE_ERROR:
    msg = C_text("bad argument type");
    c = 1;
    break;

  case C_UNBOUND_VARIABLE_ERROR:
    msg = C_text("unbound variable");
    c = 1;
    break;

  case C_TOO_MANY_PARAMETERS_ERROR:
    msg = C_text("parameter limit exceeded");
    c = 0;
    break;

  case C_OUT_OF_MEMORY_ERROR:
    msg = C_text("not enough memory");
    c = 0;
    break;

  case C_DIVISION_BY_ZERO_ERROR:
    msg = C_text("division by zero");
    c = 0;
    break;

  case C_OUT_OF_RANGE_ERROR:
    msg = C_text("out of range");
    c = 2;
    break;

  case C_NOT_A_CLOSURE_ERROR:
    msg = C_text("call of non-procedure");
    c = 1;
    break;

  case C_CONTINUATION_CANT_RECEIVE_VALUES_ERROR:
    msg = C_text("continuation cannot receive multiple values");
    c = 1;
    break;

  case C_BAD_ARGUMENT_TYPE_CYCLIC_LIST_ERROR:
    msg = C_text("bad argument type - not a non-cyclic list");
    c = 1;
    break;

  case C_TOO_DEEP_RECURSION_ERROR:
    msg = C_text("recursion too deep");
    c = 0;
    break;

  case C_CANT_REPRESENT_INEXACT_ERROR:
    msg = C_text("inexact number cannot be represented as an exact number");
    c = 1;
    break;

  case C_NOT_A_PROPER_LIST_ERROR:
    msg = C_text("bad argument type - not a proper list");
    c = 1;
    break;

  case C_BAD_ARGUMENT_TYPE_NO_FIXNUM_ERROR:
    msg = C_text("bad argument type - not a fixnum");
    c = 1;
    break;

  case C_BAD_ARGUMENT_TYPE_NO_STRING_ERROR:
    msg = C_text("bad argument type - not a string");
    c = 1;
    break;

  case C_BAD_ARGUMENT_TYPE_NO_PAIR_ERROR:
    msg = C_text("bad argument type - not a pair");
    c = 1;
    break;

  case C_BAD_ARGUMENT_TYPE_NO_BOOLEAN_ERROR:
    msg = C_text("bad argument type - not a boolean");
    c = 1;
    break;

  case C_BAD_ARGUMENT_TYPE_NO_LOCATIVE_ERROR:
    msg = C_text("bad argument type - not a locative");
    c = 1;
    break;

  case C_BAD_ARGUMENT_TYPE_NO_LIST_ERROR:
    msg = C_text("bad argument type - not a list");
    c = 1;
    break;

  case C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR:
    msg = C_text("bad argument type - not a number");
    c = 1;
    break;

  case C_BAD_ARGUMENT_TYPE_NO_SYMBOL_ERROR:
    msg = C_text("bad argument type - not a symbol");
    c = 1;
    break;

  case C_BAD_ARGUMENT_TYPE_NO_VECTOR_ERROR:
    msg = C_text("bad argument type - not a vector");
    c = 1;
    break;

  case C_BAD_ARGUMENT_TYPE_NO_CHAR_ERROR:
    msg = C_text("bad argument type - not a character");
    c = 1;
    break;

  case C_STACK_OVERFLOW_ERROR:
    msg = C_text("stack overflow");
    c = 0;
    break;

  case C_BAD_ARGUMENT_TYPE_BAD_STRUCT_ERROR:
    msg = C_text("bad argument type - not a structure of the required type");
    c = 2;
    break;

  case C_BAD_ARGUMENT_TYPE_NO_BYTEVECTOR_ERROR:
    msg = C_text("bad argument type - not a blob");
    c = 1;
    break;

  case C_LOST_LOCATIVE_ERROR:
    msg = C_text("locative refers to reclaimed object");
    c = 1;
    break;

  case C_BAD_ARGUMENT_TYPE_NO_BLOCK_ERROR:
    msg = C_text("bad argument type - not a non-immediate value");
    c = 1;
    break;

  case C_BAD_ARGUMENT_TYPE_NO_NUMBER_VECTOR_ERROR:
    msg = C_text("bad argument type - not a number vector");
    c = 2;
    break;

  case C_BAD_ARGUMENT_TYPE_NO_INTEGER_ERROR:
    msg = C_text("bad argument type - not an integer");
    c = 1;
    break;

  case C_BAD_ARGUMENT_TYPE_NO_UINTEGER_ERROR:
    msg = C_text("bad argument type - not an unsigned integer");
    c = 1;
    break;

  case C_BAD_ARGUMENT_TYPE_NO_POINTER_ERROR:
    msg = C_text("bad argument type - not a pointer");
    c = 1;
    break;

  case C_BAD_ARGUMENT_TYPE_NO_TAGGED_POINTER_ERROR:
    msg = C_text("bad argument type - not a tagged pointer");
    c = 2;
    break;

  case C_BAD_ARGUMENT_TYPE_NO_FLONUM_ERROR:
    msg = C_text("bad argument type - not a flonum");
    c = 1;
    break;

  case C_BAD_ARGUMENT_TYPE_NO_CLOSURE_ERROR:
    msg = C_text("bad argument type - not a procedure");
    c = 1;
    break;

  case C_BAD_ARGUMENT_TYPE_BAD_BASE_ERROR:
    msg = C_text("bad argument type - invalid base");
    c = 1;
    break;

  case C_CIRCULAR_DATA_ERROR:
    msg = C_text("recursion too deep or circular data encountered");
    c = 0;
    break;

  case C_BAD_ARGUMENT_TYPE_NO_PORT_ERROR:
    msg = C_text("bad argument type - not a port");
    c = 1;
    break;

  case C_BAD_ARGUMENT_TYPE_NO_INPUT_PORT_ERROR:
    msg = C_text("bad argument type - not an input-port");
    c = 1;
    break;

  case C_BAD_ARGUMENT_TYPE_NO_OUTPUT_PORT_ERROR:
    msg = C_text("bad argument type - not an output-port");
    c = 1;
    break;

  case C_PORT_CLOSED_ERROR:
    msg = C_text("port already closed");
    c = 1;
    break;
 
  case C_ASCIIZ_REPRESENTATION_ERROR:
    msg = C_text("cannot represent string with NUL bytes as C string");
    c = 1;
    break;

  case C_MEMORY_VIOLATION_ERROR:
    msg = C_text("segmentation violation");
    c = 0;
    break;

  case C_BAD_ARGUMENT_TYPE_NO_EXACT_ERROR:
    msg = C_text("bad argument type - not an exact number");
    c = 1;
    break;

  case C_BAD_ARGUMENT_TYPE_NO_INEXACT_ERROR:
    msg = C_text("bad argument type - not an inexact number");
    c = 1;
    break;

  case C_BAD_ARGUMENT_TYPE_NO_REAL_ERROR:
    msg = C_text("bad argument type - not an real");
    c = 1;
    break;

  case C_BAD_ARGUMENT_TYPE_COMPLEX_NO_ORDERING_ERROR:
    msg = C_text("bad argument type - complex number has no ordering");
    c = 1;
    break;

  case C_BAD_ARGUMENT_TYPE_NO_EXACT_INTEGER_ERROR:
    msg = C_text("bad argument type - not an exact integer");
    c = 1;
    break;

  default: panic(C_text("illegal internal error code"));
  }
  
  if(!C_immediatep(err)) {
    C_save(C_fix(code));
    
    if(loc != NULL)
      C_save(intern0(loc));
    else {
      C_save(error_location);
      error_location = C_SCHEME_FALSE;
    }
    
    va_start(v, loc);
    i = c;

    while(i--)
      C_save(va_arg(v, C_word));

    va_end(v);
    /* No continuation is passed: '##sys#error-hook' may not return: */
    C_do_apply(c + 2, err, C_SCHEME_UNDEFINED); 
  }
  else panic(msg);
}


/* Never use extended number hook procedure names longer than this! */
/* Current longest name: ##sys#bignum-divrem-burnikel-ziegler */
#define MAX_EXTNUM_HOOK_NAME 64

/* This exists so that we don't have to create any extra closures */
static void try_extended_number(char *ext_proc_name, C_word c, C_word k, ...)
{
  static C_word ab[C_SIZEOF_STRING(MAX_EXTNUM_HOOK_NAME)];
  int i;
  va_list v;
  C_word ext_proc_sym, ext_proc = C_SCHEME_FALSE, *a = ab;

  ext_proc_sym = C_lookup_symbol(C_intern2(&a, ext_proc_name));

  if(!C_immediatep(ext_proc_sym))
    ext_proc = C_block_item(ext_proc_sym, 0);

  if (!C_immediatep(ext_proc) && C_closurep(ext_proc)) {
    va_start(v, k);
    i = c - 1;

    while(i--)
      C_save(va_arg(v, C_word));

    va_end(v);
    C_do_apply(c - 1, ext_proc, k);
  } else {
    barf(C_UNBOUND_VARIABLE_ERROR, NULL, ext_proc_sym);
  }
}


/* Hook for setting breakpoints */

C_word C_dbg_hook(C_word dummy)
{
  return dummy;
}


/* Timing routines: */

C_regparm double C_fcall C_milliseconds(void)
{
#ifdef C_NONUNIX
    if(CLOCKS_PER_SEC == 1000) return clock();
    else return C_floor(((double)clock() / (double)CLOCKS_PER_SEC) * 1000);
#else
    struct timeval tv;

    if(C_gettimeofday(&tv, NULL) == -1) return 0;
    else return 
	   C_floor(((double)tv.tv_sec - C_startup_time_seconds) * 1000.0 + tv.tv_usec / 1000);
#endif
}


C_regparm time_t C_fcall C_seconds(C_long *ms)
{
#ifdef C_NONUNIX
  if(ms != NULL) *ms = 0;

  return (time_t)(clock() / CLOCKS_PER_SEC);
#else
  struct timeval tv;

  if(C_gettimeofday(&tv, NULL) == -1) {
    if(ms != NULL) *ms = 0;

    return (time_t)0;
  }
  else {
    if(ms != NULL) *ms = tv.tv_usec / 1000;

    return tv.tv_sec;
  }
#endif
}


C_regparm double C_fcall C_cpu_milliseconds(void)
{
#if defined(C_NONUNIX) || defined(__CYGWIN__)
    if(CLOCKS_PER_SEC == 1000) return clock();
    else return C_floor(((double)clock() / (double)CLOCKS_PER_SEC) * 1000);
#else
    struct rusage ru;

    if(C_getrusage(RUSAGE_SELF, &ru) == -1) return 0;
    else return 
	   C_floor(((double)ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000
		   + ((double)ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1000);
#endif
}


/* Support code for callbacks: */

int C_fcall C_save_callback_continuation(C_word **ptr, C_word k)
{
  C_word p = C_a_pair(ptr, k, C_block_item(callback_continuation_stack_symbol, 0));
  
  C_mutate_slot(&C_block_item(callback_continuation_stack_symbol, 0), p);
  return ++callback_continuation_level;
}


C_word C_fcall C_restore_callback_continuation(void) 
{
  /* obsolete, but retained for keeping old code working */
  C_word p = C_block_item(callback_continuation_stack_symbol, 0),
         k;

  assert(!C_immediatep(p) && C_block_header(p) == C_PAIR_TAG);
  k = C_u_i_car(p);

  C_mutate2(&C_block_item(callback_continuation_stack_symbol, 0), C_u_i_cdr(p));
  --callback_continuation_level;
  return k;
}


C_word C_fcall C_restore_callback_continuation2(int level) 
{
  C_word p = C_block_item(callback_continuation_stack_symbol, 0),
         k;

  if(level != callback_continuation_level || C_immediatep(p) || C_block_header(p) != C_PAIR_TAG)
    panic(C_text("unbalanced callback continuation stack"));

  k = C_u_i_car(p);

  C_mutate2(&C_block_item(callback_continuation_stack_symbol, 0), C_u_i_cdr(p));
  --callback_continuation_level;
  return k;
}


C_word C_fcall C_callback(C_word closure, int argc)
{
#ifdef HAVE_SIGSETJMP
  sigjmp_buf prev;                                                                                                                                      
#else                                                                                                                                                   
  jmp_buf prev;
#endif
  C_word 
    *a = C_alloc(C_SIZEOF_CLOSURE(2)),
    k = C_closure(&a, 2, (C_word)callback_return_continuation, C_SCHEME_FALSE);
  int old = chicken_is_running;

  if(old && C_block_item(callback_continuation_stack_symbol, 0) == C_SCHEME_END_OF_LIST)
    panic(C_text("callback invoked in non-safe context"));

  C_memcpy(&prev, &C_restart, sizeof(C_restart));
  callback_returned_flag = 0;       
  chicken_is_running = 1;

#ifdef HAVE_SIGSETJMP
  if(!C_sigsetjmp(C_restart, 0)) C_do_apply(argc, closure, k);
#else
  if(!C_setjmp(C_restart)) C_do_apply(argc, closure, k);
#endif

  serious_signal_occurred = 0;

  if(!callback_returned_flag) (C_restart_trampoline)(C_restart_address);
  else {
    C_memcpy(&C_restart, &prev, sizeof(C_restart));
    callback_returned_flag = 0;
  }
 
  chicken_is_running = old;
  return C_restore;
}


void C_fcall C_callback_adjust_stack(C_word *a, int size)
{
  if(!chicken_is_running && !C_in_stackp((C_word)a)) {
    if(debug_mode)
      C_dbg(C_text("debug"), 
	    C_text("callback invoked in lower stack region - adjusting limits:\n"
		   "[debug]   current:  \t%p\n"
		   "[debug]   previous: \t%p (bottom) - %p (limit)\n"),
	    a, stack_bottom, C_stack_limit);

#if C_STACK_GROWS_DOWNWARD
    C_stack_limit = (C_word *)((C_byte *)a - stack_size);
    stack_bottom = a + size;
#else
    C_stack_limit = (C_word *)((C_byte *)a + stack_size);
    stack_bottom = a;
#endif

    if(debug_mode)
      C_dbg(C_text("debug"), C_text("new:      \t%p (bottom) - %p (limit)\n"),
	    stack_bottom, C_stack_limit);
  }
}


C_word C_fcall C_callback_wrapper(void *proc, int argc)
{
  C_word
    *a = C_alloc(C_SIZEOF_CLOSURE(1)),
    closure = C_closure(&a, 1, (C_word)proc),
    result;
  
  result = C_callback(closure, argc);
  assert(C_temporary_stack == C_temporary_stack_bottom);
  return result;
}


void C_ccall callback_return_continuation(C_word c, C_word self, C_word r)
{
  if(C_block_item(self, 1) == C_SCHEME_TRUE)
    panic(C_text("callback returned twice"));

  assert(callback_returned_flag == 0);
  callback_returned_flag = 1;
  C_set_block_item(self, 1, C_SCHEME_TRUE);
  C_save(r);
  C_reclaim(NULL, NULL);
}


/* Register/unregister literal frame: */

void C_initialize_lf(C_word *lf, int count)
{
  while(count-- > 0)
    *(lf++) = C_SCHEME_UNBOUND;
}


void *C_register_lf(C_word *lf, int count)
{
  return C_register_lf2(lf, count, NULL);
}


void *C_register_lf2(C_word *lf, int count, C_PTABLE_ENTRY *ptable)
{
  LF_LIST *node = (LF_LIST *)C_malloc(sizeof(LF_LIST));
  LF_LIST *np;
  int status = 0;

  node->lf = lf;
  node->count = count;
  node->ptable = ptable;
  node->module_name = current_module_name;
  node->module_handle = current_module_handle;
  current_module_handle = NULL;

  if(lf_list) lf_list->prev = node;

  node->next = lf_list;
  node->prev = NULL;
  lf_list = node;
  return (void *)node;
}


LF_LIST *find_module_handle(char *name)
{
  LF_LIST *np;

  for(np = lf_list; np != NULL; np = np->next) {
    if(np->module_name != NULL && !C_strcmp(np->module_name, name)) 
      return np;
  }

  return NULL;
}


void C_unregister_lf(void *handle)
{
  LF_LIST *node = (LF_LIST *) handle;

  if (node->next) node->next->prev = node->prev;

  if (node->prev) node->prev->next = node->next;

  if (lf_list == node) lf_list = node->next;

  C_free(node->module_name);
  C_free(node);
}


/* Intern symbol into symbol-table: */

C_regparm C_word C_fcall C_intern(C_word **ptr, int len, C_char *str) 
{
  return C_intern_in(ptr, len, str, symbol_table);
}


C_regparm C_word C_fcall C_h_intern(C_word *slot, int len, C_char *str)
{
  return C_h_intern_in(slot, len, str, symbol_table);
}


C_regparm C_word C_fcall C_intern_in(C_word **ptr, int len, C_char *str, C_SYMBOL_TABLE *stable)
{
  int key;
  C_word s;

  if(stable == NULL) stable = symbol_table;

  key = hash_string(len, str, stable->size, stable->rand, 0);

  if(C_truep(s = lookup(key, len, str, stable))) return s;

  s = C_string(ptr, len, str);
  return add_symbol(ptr, key, s, stable);
}


C_regparm C_word C_fcall C_h_intern_in(C_word *slot, int len, C_char *str, C_SYMBOL_TABLE *stable)
{
  /* Intern as usual, but remember slot, if looked up symbol is in nursery.
     also: allocate in static memory. */
  int key;
  C_word s;

  if(stable == NULL) stable = symbol_table;

  key = hash_string(len, str, stable->size, stable->rand, 0);

  if(C_truep(s = lookup(key, len, str, stable))) {
    if(C_in_stackp(s)) C_mutate_slot(slot, s);
    
    return s;
  }

  s = C_static_string(C_heaptop, len, str);
  return add_symbol(C_heaptop, key, s, stable);
}


C_regparm C_word C_fcall intern0(C_char *str)
{
  int len = C_strlen(str);
  int key = hash_string(len, str, symbol_table->size, symbol_table->rand, 0);
  C_word s;

  if(C_truep(s = lookup(key, len, str, symbol_table))) return s;
  else return C_SCHEME_FALSE;
}


C_regparm C_word C_fcall C_lookup_symbol(C_word sym)
{
  int key;
  C_word str = C_block_item(sym, 1);
  int len = C_header_size(str);

  key = hash_string(len, C_c_string(str), symbol_table->size, symbol_table->rand, 0);

  return lookup(key, len, C_c_string(str), symbol_table);
}


C_regparm C_word C_fcall C_intern2(C_word **ptr, C_char *str)
{
  return C_intern_in(ptr, C_strlen(str), str, symbol_table);
}


C_regparm C_word C_fcall C_intern3(C_word **ptr, C_char *str, C_word value)
{
  C_word s = C_intern_in(ptr, C_strlen(str), str, symbol_table);
  
  C_mutate2(&C_block_item(s,0), value);
  return s;
}


C_regparm C_word C_fcall hash_string(int len, C_char *str, C_word m, C_word r, int ci)
{
  C_uword key = r;

  if (ci)
    while(len--) key ^= (key << 6) + (key >> 2) + C_tolower((int)(*str++));
  else
    while(len--) key ^= (key << 6) + (key >> 2) + *(str++);

  return (C_word)(key % (C_uword)m);
}


C_regparm C_word C_fcall lookup(C_word key, int len, C_char *str, C_SYMBOL_TABLE *stable)
{
  C_word bucket, sym, s;

  for(bucket = stable->table[ key ]; bucket != C_SCHEME_END_OF_LIST; 
      bucket = C_block_item(bucket,1)) {
    sym = C_block_item(bucket,0);
    s = C_block_item(sym, 1);

    if(C_header_size(s) == (C_word)len
       && !C_memcmp(str, (C_char *)C_data_pointer(s), len))
      return sym;
  }

  return C_SCHEME_FALSE;
}


double compute_symbol_table_load(double *avg_bucket_len, int *total_n)
{
  C_word bucket;
  int i, j, alen = 0, bcount = 0, total = 0;

  for(i = 0; i < symbol_table->size; ++i) {
    bucket = symbol_table->table[ i ];

    for(j = 0; bucket != C_SCHEME_END_OF_LIST; ++j)
      bucket = C_block_item(bucket,1);

    if(j > 0) {
      alen += j;
      ++bcount;
    }

    total += j;
  }

  if(avg_bucket_len != NULL)
    *avg_bucket_len = (double)alen / (double)bcount;

  *total_n = total;

  /* return load: */
  return (double)total / (double)symbol_table->size;
}


C_word add_symbol(C_word **ptr, C_word key, C_word string, C_SYMBOL_TABLE *stable)
{
  C_word bucket, sym, b2, *p;
  int keyw = C_header_size(string) > 0 && *((char *)C_data_pointer(string)) == 0;

  p = *ptr;
  sym = (C_word)p;
  p += C_SIZEOF_SYMBOL;
  C_block_header_init(sym, C_SYMBOL_TYPE | (C_SIZEOF_SYMBOL - 1));
  C_set_block_item(sym, 0, keyw ? sym : C_SCHEME_UNBOUND); /* keyword? */
  C_set_block_item(sym, 1, string);
  C_set_block_item(sym, 2, C_SCHEME_END_OF_LIST);
  *ptr = p;
  b2 = stable->table[ key ];	/* previous bucket */
  bucket = C_a_bucket(ptr, sym, b2); /* create new bucket */

  if(ptr != C_heaptop) C_mutate_slot(&stable->table[ key ], bucket);
  else {
    /* If a stack-allocated bucket was here, and we allocate from 
       heap-top (say, in a toplevel literal frame allocation) then we have
       to inform the memory manager that a 2nd gen. block points to a 
       1st gen. block, hence the mutation: */
    C_mutate2(&C_block_item(bucket,1), b2);
    stable->table[ key ] = bucket;
  }

  return sym;
}


C_regparm int C_in_stackp(C_word x)
{
  C_word *ptr = (C_word *)(C_uword)x;

#if C_STACK_GROWS_DOWNWARD
  return ptr >= C_stack_pointer_test && ptr <= stack_bottom;
#else
  return ptr < C_stack_pointer_test && ptr >= stack_bottom;
#endif
}


C_regparm int C_fcall C_in_heapp(C_word x)
{
  C_byte *ptr = (C_byte *)(C_uword)x;
  return (ptr >= fromspace_start && ptr < C_fromspace_limit) ||
         (ptr >= tospace_start && ptr < tospace_limit);
}

/* Only used during major GC (heap realloc) */
static C_regparm int C_fcall C_in_new_heapp(C_word x)
{
  C_byte *ptr = (C_byte *)(C_uword)x;
  return (ptr >= new_tospace_start && ptr < new_tospace_limit);
}

C_regparm int C_fcall C_in_fromspacep(C_word x)
{
  C_byte *ptr = (C_byte *)(C_uword)x;
  return (ptr >= fromspace_start && ptr < C_fromspace_limit);
}


/* Cons the rest-aguments together: */

C_regparm C_word C_fcall C_restore_rest(C_word *ptr, int num)
{
  C_word x = C_SCHEME_END_OF_LIST;
  C_SCHEME_BLOCK *node;

  while(num--) {
    node = (C_SCHEME_BLOCK *)ptr;
    ptr += 3;
    node->header = C_PAIR_TYPE | (C_SIZEOF_PAIR - 1);
    node->data[ 0 ] = C_restore;
    node->data[ 1 ] = x;
    x = (C_word)node;
  }

  return x;
}


/* I? */
C_regparm C_word C_fcall C_restore_rest_vector(C_word *ptr, int num)
{
  C_word *p0 = ptr;

  *(ptr++) = C_VECTOR_TYPE | num;
  ptr += num;

  while(num--) *(--ptr) = C_restore;

  return (C_word)p0;
}


/* Print error messages and exit: */

void C_bad_memory(void)
{
  panic(C_text("there is not enough stack-space to run this executable"));
}


void C_bad_memory_2(void)
{
  panic(C_text("there is not enough heap-space to run this executable - try using the '-:h...' option"));
}


/* The following two can be thrown out in the next release... */

void C_bad_argc(int c, int n)
{
  C_bad_argc_2(c, n, C_SCHEME_FALSE);
}


void C_bad_min_argc(int c, int n)
{
  C_bad_min_argc_2(c, n, C_SCHEME_FALSE);
}


void C_bad_argc_2(int c, int n, C_word closure)
{
  barf(C_BAD_ARGUMENT_COUNT_ERROR, NULL, C_fix(n - 2), C_fix(c - 2), closure);
}


void C_bad_min_argc_2(int c, int n, C_word closure)
{
  barf(C_BAD_MINIMUM_ARGUMENT_COUNT_ERROR, NULL, C_fix(n - 2), C_fix(c - 2), closure);
}


void C_stack_overflow(void)
{
  barf(C_STACK_OVERFLOW_ERROR, NULL);
}


void C_stack_overflow_with_msg(C_char *msg)
{
  barf(C_STACK_OVERFLOW_ERROR, NULL);
}

void C_temp_stack_overflow(void)
{
  /* Just raise a "too many parameters" error; it isn't very useful to
     show a different message here. */
  barf(C_TOO_MANY_PARAMETERS_ERROR, NULL);
}


void C_unbound_error(C_word sym)
{
  barf(C_UNBOUND_VARIABLE_ERROR, NULL, sym);
}


void C_no_closure_error(C_word x)
{
  barf(C_NOT_A_CLOSURE_ERROR, NULL, x);
}


void C_div_by_zero_error(char *loc)
{
  barf(C_DIVISION_BY_ZERO_ERROR, loc);
}

void C_not_an_integer_error(char *loc, C_word x)
{
  barf(C_BAD_ARGUMENT_TYPE_NO_INTEGER_ERROR, loc, x);
}

void C_not_an_uinteger_error(char *loc, C_word x)
{
  barf(C_BAD_ARGUMENT_TYPE_NO_UINTEGER_ERROR, loc, x);
}

/* Allocate and initialize record: */

C_regparm C_word C_fcall C_string(C_word **ptr, int len, C_char *str)
{
  C_word strblock = (C_word)(*ptr);

  *ptr = (C_word *)((C_word)(*ptr) + sizeof(C_header) + C_align(len));
  C_block_header_init(strblock, C_STRING_TYPE | len);
  C_memcpy(C_data_pointer(strblock), str, len);
  return strblock;
}


C_regparm C_word C_fcall C_static_string(C_word **ptr, int len, C_char *str)
{
  C_word *dptr = (C_word *)C_malloc(sizeof(C_header) + C_align(len));
  C_word strblock;

  if(dptr == NULL)
    panic(C_text("out of memory - cannot allocate static string"));
    
  strblock = (C_word)dptr;
  C_block_header_init(strblock, C_STRING_TYPE | len);
  C_memcpy(C_data_pointer(strblock), str, len);
  return strblock;
}


C_regparm C_word C_fcall C_static_lambda_info(C_word **ptr, int len, C_char *str)
{
  int dlen = sizeof(C_header) + C_align(len);
  void *dptr = C_malloc(dlen);
  C_word strblock;

  if(dptr == NULL)
    panic(C_text("out of memory - cannot allocate static lambda info"));

  strblock = (C_word)dptr;
  C_block_header_init(strblock, C_LAMBDA_INFO_TYPE | len);
  C_memcpy(C_data_pointer(strblock), str, len);
  return strblock;
}


C_regparm C_word C_fcall C_bytevector(C_word **ptr, int len, C_char *str)
{
  C_word strblock = C_string(ptr, len, str);

  (void)C_string_to_bytevector(strblock);
  return strblock;
}


C_regparm C_word C_fcall C_static_bytevector(C_word **ptr, int len, C_char *str)
{
  C_word strblock = C_static_string(ptr, len, str);

  C_block_header_init(strblock, C_BYTEVECTOR_TYPE | len);
  return strblock;
}


C_regparm C_word C_fcall C_pbytevector(int len, C_char *str)
{
  C_SCHEME_BLOCK *pbv = C_malloc(len + sizeof(C_header));

  if(pbv == NULL) panic(C_text("out of memory - cannot allocate permanent blob"));

  pbv->header = C_BYTEVECTOR_TYPE | len;
  C_memcpy(pbv->data, str, len);
  return (C_word)pbv;
}


C_regparm C_word C_fcall C_string_aligned8(C_word **ptr, int len, C_char *str)
{
  C_word *p = *ptr,
         *p0;

#ifndef C_SIXTY_FOUR
  /* Align on 8-byte boundary: */
  if(C_aligned8(p)) ++p;
#endif

  p0 = p;
  *ptr = p + 1 + C_bytestowords(len);
  *(p++) = C_STRING_TYPE | C_8ALIGN_BIT | len;
  C_memcpy(p, str, len);
  return (C_word)p0;
}


C_regparm C_word C_fcall C_string2(C_word **ptr, C_char *str)
{
  C_word strblock = (C_word)(*ptr);
  int len;

  if(str == NULL) return C_SCHEME_FALSE;

  len = C_strlen(str);
  *ptr = (C_word *)((C_word)(*ptr) + sizeof(C_header) + C_align(len));
  C_block_header_init(strblock, C_STRING_TYPE | len);
  C_memcpy(C_data_pointer(strblock), str, len);
  return strblock;
}


C_regparm C_word C_fcall C_string2_safe(C_word **ptr, int max, C_char *str)
{
  C_word strblock = (C_word)(*ptr);
  int len;

  if(str == NULL) return C_SCHEME_FALSE;

  len = C_strlen(str);

  if(len >= max) {
    C_snprintf(buffer, sizeof(buffer), C_text("foreign string result exceeded maximum of %d bytes"), max);
    panic(buffer);
  }

  *ptr = (C_word *)((C_word)(*ptr) + sizeof(C_header) + C_align(len));
  C_block_header_init(strblock, C_STRING_TYPE | len);
  C_memcpy(C_data_pointer(strblock), str, len);
  return strblock;
}


C_word C_fcall C_closure(C_word **ptr, int cells, C_word proc, ...)
{
  va_list va;
  C_word *p = *ptr,
         *p0 = p;

  *p = C_CLOSURE_TYPE | cells;
  *(++p) = proc;

  for(va_start(va, proc); --cells; *(++p) = va_arg(va, C_word));

  va_end(va);
  *ptr = p + 1;
  return (C_word)p0;
}


/* obsolete: replaced by C_a_pair in chicken.h */
C_regparm C_word C_fcall C_pair(C_word **ptr, C_word car, C_word cdr)
{
  C_word *p = *ptr,
         *p0 = p;
 
  *(p++) = C_PAIR_TYPE | (C_SIZEOF_PAIR - 1);
  *(p++) = car;
  *(p++) = cdr;
  *ptr = p;
  return (C_word)p0;
}


C_regparm C_word C_fcall C_number(C_word **ptr, double n)
{
  C_word 
    *p = *ptr,
    *p0;
  double m;

  if(n <= (double)C_MOST_POSITIVE_FIXNUM
     && n >= (double)C_MOST_NEGATIVE_FIXNUM && modf(n, &m) == 0.0) {
    return C_fix(n);
  }

#ifndef C_SIXTY_FOUR
#ifndef C_DOUBLE_IS_32_BITS
  /* Align double on 8-byte boundary: */
  if(C_aligned8(p)) ++p;
#endif
#endif

  p0 = p;
  *(p++) = C_FLONUM_TAG;
  *((double *)p) = n;
  *ptr = p + sizeof(double) / sizeof(C_word);
  return (C_word)p0;
}


C_regparm C_word C_fcall C_mpointer(C_word **ptr, void *mp)
{
  C_word 
    *p = *ptr,
    *p0 = p;

  *(p++) = C_POINTER_TYPE | 1;
  *((void **)p) = mp;
  *ptr = p + 1;
  return (C_word)p0;
}


C_regparm C_word C_fcall C_mpointer_or_false(C_word **ptr, void *mp)
{
  C_word 
    *p = *ptr,
    *p0 = p;

  if(mp == NULL) return C_SCHEME_FALSE;

  *(p++) = C_POINTER_TYPE | 1;
  *((void **)p) = mp;
  *ptr = p + 1;
  return (C_word)p0;
}


C_regparm C_word C_fcall C_taggedmpointer(C_word **ptr, C_word tag, void *mp)
{
  C_word 
    *p = *ptr,
    *p0 = p;

  *(p++) = C_TAGGED_POINTER_TAG;
  *((void **)p) = mp;
  *(++p) = tag;
  *ptr = p + 1;
  return (C_word)p0;
}


C_regparm C_word C_fcall C_taggedmpointer_or_false(C_word **ptr, C_word tag, void *mp)
{
  C_word 
    *p = *ptr,
    *p0 = p;

  if(mp == NULL) return C_SCHEME_FALSE;
 
  *(p++) = C_TAGGED_POINTER_TAG;
  *((void **)p) = mp;
  *(++p) = tag;
  *ptr = p + 1;
  return (C_word)p0;
}


C_regparm C_word C_fcall C_swigmpointer(C_word **ptr, void *mp, void *sdata)
{
  C_word 
    *p = *ptr,
    *p0 = p;

  *(p++) = C_SWIG_POINTER_TAG;
  *((void **)p) = mp;
  *((void **)p + 1) = sdata;
  *ptr = p + 2;
  return (C_word)p0;
}


C_word C_vector(C_word **ptr, int n, ...)
{
  va_list v;
  C_word 
    *p = *ptr,
    *p0 = p; 

  *(p++) = C_VECTOR_TYPE | n;
  va_start(v, n);

  while(n--)
    *(p++) = va_arg(v, C_word);

  *ptr = p;
  va_end(v);
  return (C_word)p0;
}


C_word C_structure(C_word **ptr, int n, ...)
{
  va_list v;
  C_word *p = *ptr,
         *p0 = p; 

  *(p++) = C_STRUCTURE_TYPE | n;
  va_start(v, n);

  while(n--)
    *(p++) = va_arg(v, C_word);

  *ptr = p;
  va_end(v);
  return (C_word)p0;
}


C_regparm C_word C_fcall
C_mutate_slot(C_word *slot, C_word val)
{
  unsigned int mssize, newmssize, bytes;

  ++mutation_count;
  /* Mutation stack exists to track mutations pointing from elsewhere
   * into nursery.  Stuff pointing anywhere else can be skipped, as
   * well as mutations on nursery objects.
   */
  if(!C_in_stackp(val) || C_in_stackp((C_word)slot))
    return *slot = val;

#ifdef C_GC_HOOKS
  if(C_gc_mutation_hook != NULL && C_gc_mutation_hook(slot, val)) return val;
#endif

  if(mutation_stack_top >= mutation_stack_limit) {
    assert(mutation_stack_top == mutation_stack_limit);
    mssize = mutation_stack_top - mutation_stack_bottom;
    newmssize = mssize * 2;
    bytes = newmssize * sizeof(C_word *);

    if(debug_mode) 
      C_dbg(C_text("debug"), C_text("resizing mutation-stack from " UWORD_COUNT_FORMAT_STRING "k to " UWORD_COUNT_FORMAT_STRING "k ...\n"), 
	    (mssize * sizeof(C_word *)) / 1024, bytes / 1024);

    mutation_stack_bottom = (C_word **)realloc(mutation_stack_bottom, bytes);

    if(mutation_stack_bottom == NULL)
      panic(C_text("out of memory - cannot re-allocate mutation stack"));

    mutation_stack_limit = mutation_stack_bottom + newmssize;
    mutation_stack_top = mutation_stack_bottom + mssize;
  }

  *(mutation_stack_top++) = slot;
  ++tracked_mutation_count;
  return *slot = val;
}


/* Initiate garbage collection: */


void C_save_and_reclaim(void *trampoline, void *proc, int n, ...)
{
  va_list v;
  
  va_start(v, n);

  while(n--) C_save(va_arg(v, C_word));

  va_end(v);
  C_reclaim(trampoline, proc);
}


#ifdef __SUNPRO_C
static void mark(C_word *x) { \
  C_word *_x = (x), _val = *_x; \
  if(!C_immediatep(_val)) really_mark(_x); \
}
#else
# define mark(x)				\
  C_cblock						\
  C_word *_x = (x), _val = *_x;				\
  if(!C_immediatep(_val)) really_mark(_x);		\
  C_cblockend
#endif


C_regparm void C_fcall C_reclaim(void *trampoline, void *proc)
{
  int i, j, n, fcount, weakn = 0;
  C_uword count, bytes;
  C_word *p, **msp, bucket, last, item, container;
  C_header h;
  C_byte *tmp, *start;
  LF_LIST *lfn;
  C_SCHEME_BLOCK *bp;
  C_GC_ROOT *gcrp;
  WEAK_TABLE_ENTRY *wep;
  double tgc = 0;
  C_SYMBOL_TABLE *stp;
  volatile int finalizers_checked;
  FINALIZER_NODE *flist;
  TRACE_INFO *tinfo;

  /* assert(C_timer_interrupt_counter >= 0); */

  if(pending_interrupts_count > 0 && C_interrupts_enabled)
    handle_interrupt(trampoline, proc);

  /* Note: the mode argument will always be GC_MINOR or GC_REALLOC. */
  if(C_pre_gc_hook != NULL) C_pre_gc_hook(GC_MINOR);

  finalizers_checked = 0;
  C_restart_trampoline = (TRAMPOLINE)trampoline;
  C_restart_address = proc;
  heap_scan_top = (C_byte *)C_align((C_uword)C_fromspace_top);
  gc_mode = GC_MINOR;
  start = C_fromspace_top;

  if(C_enable_gcweak) 
    weak_table_randomization = rand();

  /* Entry point for second-level GC (on explicit request or because of full fromspace): */
#ifdef HAVE_SIGSETJMP
  if(C_sigsetjmp(gc_restart, 0) || start >= C_fromspace_limit) {
#else
  if(C_setjmp(gc_restart) || start >= C_fromspace_limit) {
#endif
    if(gc_bell) {
      C_putchar(7);
      C_fflush(stdout);
    }

    tgc = C_cpu_milliseconds();

    if(gc_mode == GC_REALLOC) {
      C_rereclaim2(percentage(heap_size, C_heap_growth), 0);
      gc_mode = GC_MAJOR;
      goto i_like_spaghetti;
    }

    heap_scan_top = (C_byte *)C_align((C_uword)tospace_top);    
    gc_mode = GC_MAJOR;

    /* Mark items in forwarding table: */
    for(p = forwarding_table; *p != 0; p += 2) {
      last = p[ 1 ];
      mark(&p[ 1 ]);
      C_block_header(p[ 0 ]) = C_block_header(last);
    }

    /* Mark literal frames: */
    for(lfn = lf_list; lfn != NULL; lfn = lfn->next)
      for(i = 0; i < lfn->count; ++i)
        mark(&lfn->lf[i]);

    /* Mark symbol tables: */
    for(stp = symbol_table_list; stp != NULL; stp = stp->next)
      for(i = 0; i < stp->size; ++i)
        mark(&stp->table[i]);

    /* Mark collectibles: */
    for(msp = collectibles; msp < collectibles_top; ++msp)
      if(*msp != NULL) mark(*msp);

    /* mark normal GC roots: */
    for(gcrp = gc_root_list; gcrp != NULL; gcrp = gcrp->next) {
      if(!gcrp->finalizable) mark(&gcrp->value);
    }

    mark_system_globals();
  }
  else {
    /* Mark mutated slots: */
    for(msp = mutation_stack_bottom; msp < mutation_stack_top; ++msp)
      mark(*msp);
  }

  /* Clear the mutated slot stack: */
  mutation_stack_top = mutation_stack_bottom;

  /* Mark live values: */
  for(p = C_temporary_stack; p < C_temporary_stack_bottom; ++p)
    mark(p);

  /* Mark trace-buffer: */
  for(tinfo = trace_buffer; tinfo < trace_buffer_limit; ++tinfo) {
    mark(&tinfo->cooked1);
    mark(&tinfo->cooked2);
    mark(&tinfo->thread);
  }

 rescan:
  /* Mark nested values in already moved (marked) blocks in breadth-first manner: */
  while(heap_scan_top < (gc_mode == GC_MINOR ? C_fromspace_top : tospace_top)) {
    bp = (C_SCHEME_BLOCK *)heap_scan_top;

    if(*((C_word *)bp) == ALIGNMENT_HOLE_MARKER) 
      bp = (C_SCHEME_BLOCK *)((C_word *)bp + 1);

    n = C_header_size(bp);
    h = bp->header;
    bytes = (h & C_BYTEBLOCK_BIT) ? n : n * sizeof(C_word);
    p = bp->data;

    if(n > 0 && (h & C_BYTEBLOCK_BIT) == 0) {
      if(h & C_SPECIALBLOCK_BIT) {
	--n;
	++p;
      }

      while(n--) mark(p++);
    }

    heap_scan_top = (C_byte *)bp + C_align(bytes) + sizeof(C_word);
  }

  if(gc_mode == GC_MINOR) {
    count = (C_uword)C_fromspace_top - (C_uword)start;
    ++gc_count_1;
    ++gc_count_1_total;
    update_locative_table(GC_MINOR);
  }
  else {
    if(!finalizers_checked) {
      /* Mark finalizer list and remember pointers to non-forwarded items: */
      last = C_block_item(pending_finalizers_symbol, 0);

      if(!C_immediatep(last) && (j = C_unfix(C_block_item(last, 0))) != 0) { 
	/* still finalizers pending: just mark table items... */
	if(gc_report_flag)
	  C_dbg(C_text("GC"), C_text("%d finalized item(s) still pending\n"), j);

	j = fcount = 0;

	for(flist = finalizer_list; flist != NULL; flist = flist->next) {
	  mark(&flist->item);
	  mark(&flist->finalizer);
	  ++fcount;
	}

	/* mark finalizable GC roots: */
	for(gcrp = gc_root_list; gcrp != NULL; gcrp = gcrp->next) {
	  if(gcrp->finalizable) mark(&gcrp->value);
	}

	if(gc_report_flag && fcount > 0)
	  C_dbg(C_text("GC"), C_text("%d finalizer value(s) marked\n"), fcount);
      }
      else {
	j = fcount = 0;

	/* move into pending */
	for(flist = finalizer_list; flist != NULL; flist = flist->next) {
	  if(j < C_max_pending_finalizers) {
	    if(!is_fptr(C_block_header(flist->item))) 
	      pending_finalizer_indices[ j++ ] = flist;
	  }
	}

	/* mark */
	for(flist = finalizer_list; flist != NULL; flist = flist->next) {
	  mark(&flist->item);
	  mark(&flist->finalizer);
	}

	/* mark finalizable GC roots: */
	for(gcrp = gc_root_list; gcrp != NULL; gcrp = gcrp->next) {
	  if(gcrp->finalizable) mark(&gcrp->value);
	}
      }

      pending_finalizer_count = j;
      finalizers_checked = 1;

      if(pending_finalizer_count > 0 && gc_report_flag)
	C_dbg(C_text("GC"), C_text("%d finalizer(s) pending (%d live)\n"), 
	      pending_finalizer_count, live_finalizer_count);

      goto rescan;
    }
    else {
      /* Copy finalized items with remembered indices into `##sys#pending-finalizers' 
	 (and release finalizer node): */
      if(pending_finalizer_count > 0) {
	if(gc_report_flag)
	  C_dbg(C_text("GC"), C_text("queueing %d finalizer(s)\n"), pending_finalizer_count);

	last = C_block_item(pending_finalizers_symbol, 0);
	assert(C_block_item(last, 0) == C_fix(0));
	C_set_block_item(last, 0, C_fix(pending_finalizer_count));

	for(i = 0; i < pending_finalizer_count; ++i) {
	  flist = pending_finalizer_indices[ i ];
	  C_set_block_item(last, 1 + i * 2, flist->item);
	  C_set_block_item(last, 2 + i * 2, flist->finalizer);
	  
	  if(flist->previous != NULL) flist->previous->next = flist->next;
	  else finalizer_list = flist->next;

	  if(flist->next != NULL) flist->next->previous = flist->previous;

	  flist->next = finalizer_free_list;
	  flist->previous = NULL;
	  finalizer_free_list = flist;
	  --live_finalizer_count;
	}
      }
    }

    update_locative_table(gc_mode);
    count = (C_uword)tospace_top - (C_uword)tospace_start;

    /*** isn't gc_mode always GC_MAJOR here? */
    if(gc_mode == GC_MAJOR && 
       count < percentage(percentage(heap_size, C_heap_shrinkage), DEFAULT_HEAP_SHRINKAGE_USED) &&
       heap_size > MINIMAL_HEAP_SIZE && !C_heap_size_is_fixed)
      C_rereclaim2(percentage(heap_size, C_heap_shrinkage), 0);
    else {
      C_fromspace_top = tospace_top;
      tmp = fromspace_start;
      fromspace_start = tospace_start;
      tospace_start = tospace_top = tmp;
      tmp = C_fromspace_limit;
      C_fromspace_limit = tospace_limit;
      tospace_limit = tmp;
    }

  i_like_spaghetti:
    ++gc_count_2;

    if(C_enable_gcweak) {
      /* Check entries in weak item table and recover items ref'd only
         once, which are unbound symbols and have empty property-lists: */
      weakn = 0;
      wep = weak_item_table;

      for(i = 0; i < WEAK_TABLE_SIZE; ++i, ++wep)
	if(wep->item != 0) { 
	  if((wep->container & WEAK_COUNTER_MAX) == 0 && /* counter saturated? (more than 1) */
	     is_fptr((item = C_block_header(wep->item)))) { /* and forwarded/collected */
	    item = fptr_to_ptr(item);			    /* recover obj from forwarding ptr */
	    container = wep->container & ~WEAK_COUNTER_MASK;

	    if(C_header_bits(item) == C_SYMBOL_TYPE && 
	       C_block_item(item, 0) == C_SCHEME_UNBOUND &&
	       C_block_item(item, 2) == C_SCHEME_END_OF_LIST) {
	      ++weakn;
	      C_set_block_item(container, 0, C_SCHEME_UNDEFINED); /* clear reference to item */
	    }
	  }

	  wep->item = wep->container = 0;
	}

      /* Remove empty buckets in symbol table: */
      for(stp = symbol_table_list; stp != NULL; stp = stp->next) {
	for(i = 0; i < stp->size; ++i) {
	  last = 0;
	  
	  for(bucket = stp->table[ i ]; bucket != C_SCHEME_END_OF_LIST; bucket = C_block_item(bucket,1))
	    if(C_block_item(bucket,0) == C_SCHEME_UNDEFINED) {
	      if(last) C_set_block_item(last, 1, C_block_item(bucket,1));
	      else stp->table[ i ] = C_block_item(bucket,1);
	    }
	    else last = bucket;
	}
      }
    }
  }

  if(gc_mode == GC_MAJOR) {
    tgc = C_cpu_milliseconds() - tgc;
    gc_ms += tgc;
    timer_accumulated_gc_ms += tgc;
  }

  /* Display GC report: 
     Note: stubbornly writes to stderr - there is no provision for other output-ports */
  if(gc_report_flag == 1 || (gc_report_flag && gc_mode == GC_MAJOR)) {
    C_dbg(C_text("GC"), C_text("level  %d\tgcs(minor)  %d\tgcs(major)  %d\n"),
	  gc_mode, gc_count_1, gc_count_2);
    i = (C_uword)C_stack_pointer;

#if C_STACK_GROWS_DOWNWARD
    C_dbg("GC", C_text("stack\t" UWORD_FORMAT_STRING "\t" UWORD_FORMAT_STRING "\t" UWORD_FORMAT_STRING), 
	  (C_uword)C_stack_limit, (C_uword)i, (C_uword)C_stack_limit + stack_size);
#else
    C_dbg("GC", C_text("stack\t" UWORD_FORMAT_STRING "\t" UWORD_FORMAT_STRING "\t" UWORD_FORMAT_STRING), 
	  (C_uword)C_stack_limit - stack_size, (C_uword)i, (C_uword)C_stack_limit);
#endif

    if(gc_mode == GC_MINOR) 
      C_fprintf(C_stderr, C_text("\t" UWORD_FORMAT_STRING), (C_uword)count);

    C_fputc('\n', C_stderr);
    C_dbg("GC", C_text(" from\t" UWORD_FORMAT_STRING "\t" UWORD_FORMAT_STRING "\t" UWORD_FORMAT_STRING),
	  (C_uword)fromspace_start, (C_uword)C_fromspace_top, (C_uword)C_fromspace_limit);

    if(gc_mode == GC_MAJOR) 
      C_fprintf(C_stderr, C_text("\t" UWORD_FORMAT_STRING), (C_uword)count);

    C_fputc('\n', C_stderr);
    C_dbg("GC", C_text("   to\t" UWORD_FORMAT_STRING "\t" UWORD_FORMAT_STRING "\t" UWORD_FORMAT_STRING" \n"), 
	  (C_uword)tospace_start, (C_uword)tospace_top, 
	  (C_uword)tospace_limit);

    if(gc_mode == GC_MAJOR && C_enable_gcweak && weakn)
      C_dbg("GC", C_text("%d recoverable weakly held items found\n"), weakn);
    
    C_dbg("GC", C_text("%d locatives (from %d)\n"), locative_table_count, locative_table_size);
  }

  if(gc_mode == GC_MAJOR) gc_count_1 = 0;

  if(C_post_gc_hook != NULL) C_post_gc_hook(gc_mode, (C_long)tgc);

  /* Unwind stack completely */
#ifdef HAVE_SIGSETJMP
  C_siglongjmp(C_restart, 1);
#else
  C_longjmp(C_restart, 1);
#endif
}


C_regparm void C_fcall mark_system_globals(void)
{
  mark(&C_ratnum_type_tag);
  mark(&C_cplxnum_type_tag);
  mark(&interrupt_hook_symbol);
  mark(&error_hook_symbol);
  mark(&callback_continuation_stack_symbol);
  mark(&pending_finalizers_symbol);
  mark(&current_thread_symbol);
}


C_regparm void C_fcall really_mark(C_word *x)
{
  C_word val, item;
  C_uword n, bytes;
  C_header h;
  C_SCHEME_BLOCK *p, *p2;
  WEAK_TABLE_ENTRY *wep;

  val = *x;

  if (!C_in_stackp(val) && !C_in_heapp(val)) {
#ifdef C_GC_HOOKS
      if(C_gc_trace_hook != NULL) 
	C_gc_trace_hook(x, gc_mode);
#endif

      return;
  }

  p = (C_SCHEME_BLOCK *)val;
  
  h = p->header;

  if(gc_mode == GC_MINOR) {
    if(is_fptr(h)) {
      *x = val = fptr_to_ptr(h);
      return;
    }

    if((C_uword)val >= (C_uword)fromspace_start && (C_uword)val < (C_uword)C_fromspace_top) return;

    p2 = (C_SCHEME_BLOCK *)C_align((C_uword)C_fromspace_top);

#ifndef C_SIXTY_FOUR
    if((h & C_8ALIGN_BIT) && C_aligned8(p2) && (C_byte *)p2 < C_fromspace_limit) {
      *((C_word *)p2) = ALIGNMENT_HOLE_MARKER;
      p2 = (C_SCHEME_BLOCK *)((C_word *)p2 + 1);
    }
#endif

    n = C_header_size(p);
    bytes = (h & C_BYTEBLOCK_BIT) ? n : n * sizeof(C_word);

    if(((C_byte *)p2 + bytes + sizeof(C_word)) > C_fromspace_limit)
#ifdef HAVE_SIGSETJMP
      C_siglongjmp(gc_restart, 1);
#else
      C_longjmp(gc_restart, 1);
#endif

    C_fromspace_top = (C_byte *)p2 + C_align(bytes) + sizeof(C_word);

  scavenge:
    *x = (C_word)p2;
    p2->header = h;
    p->header = ptr_to_fptr((C_uword)p2);
    C_memcpy(p2->data, p->data, bytes);
  }
  else { /* (major GC) */
    /* Increase counter (saturated at 2) if weakly held item (someone pointed to this object): */
    if(C_enable_gcweak &&
       (h & C_HEADER_TYPE_BITS) == C_SYMBOL_TYPE &&
       (wep = lookup_weak_table_entry(val, 0)) != NULL) {
      if((wep->container & WEAK_COUNTER_MAX) == 0) ++wep->container;
    }

    if(is_fptr(h)) {
      val = fptr_to_ptr(h);

      if((C_uword)val >= (C_uword)tospace_start && (C_uword)val < (C_uword)tospace_top) {
	*x = val;
	return;
      }

      /* Link points into fromspace: fetch new pointer + header and copy... */
      p = (C_SCHEME_BLOCK *)val;
      h = p->header;

      if(is_fptr(h)) {
	/* Link points into fromspace and into a link which points into from- or tospace: */
	val = fptr_to_ptr(h);
	
	if((C_uword)val >= (C_uword)tospace_start && (C_uword)val < (C_uword)tospace_top) {
	  *x = val;
	  return;
	}

	p = (C_SCHEME_BLOCK *)val;
	h = p->header;
      }
    }

    p2 = (C_SCHEME_BLOCK *)C_align((C_uword)tospace_top);

#ifndef C_SIXTY_FOUR
    if((h & C_8ALIGN_BIT) && C_aligned8(p2) && (C_byte *)p2 < tospace_limit) {
      *((C_word *)p2) = ALIGNMENT_HOLE_MARKER;
      p2 = (C_SCHEME_BLOCK *)((C_word *)p2 + 1);
    }
#endif

    if(C_enable_gcweak && (h & C_HEADER_TYPE_BITS) == C_BUCKET_TYPE) {
      item = C_block_item(val,0);

      /* Lookup item in weak item table or add entry: */
      if((wep = lookup_weak_table_entry(item, (C_word)p2)) != NULL) {
	/* If item is already forwarded, then set count to 2: */
	if(is_fptr(C_block_header(item))) wep->container |= 2;
      }
    }

    n = C_header_size(p);
    bytes = (h & C_BYTEBLOCK_BIT) ? n : n * sizeof(C_word);

    if(((C_byte *)p2 + bytes + sizeof(C_word)) > tospace_limit) {
      if(C_heap_size_is_fixed)
	panic(C_text("out of memory - heap full"));
      
      gc_mode = GC_REALLOC;
#ifdef HAVE_SIGSETJMP
      C_siglongjmp(gc_restart, 1);
#else
      C_longjmp(gc_restart, 1);
#endif
    }

    tospace_top = (C_byte *)p2 + C_align(bytes) + sizeof(C_word);
    goto scavenge;
  }
}


#ifdef __SUNPRO_C
static void remark(C_word *x) { \
  C_word *_x = (x), _val = *_x;		     \
  if(!C_immediatep(_val)) really_remark(_x); \
}
#else
#define remark(x)				\
  C_cblock					\
  C_word *_x = (x), _val = *_x;			\
  if(!C_immediatep(_val)) really_remark(_x);	\
  C_cblockend
#endif


/* Do a major GC into a freshly allocated heap: */

C_regparm void C_fcall C_rereclaim2(C_uword size, int double_plus)
{
  int i, j;
  C_uword count, n, bytes;
  C_word *p, **msp, item, last;
  C_header h;
  C_byte *tmp, *start;
  LF_LIST *lfn;
  C_SCHEME_BLOCK *bp;
  WEAK_TABLE_ENTRY *wep;
  C_GC_ROOT *gcrp;
  C_SYMBOL_TABLE *stp;
  FINALIZER_NODE *flist;
  TRACE_INFO *tinfo;
  C_byte *new_heapspace;
  size_t  new_heapspace_size;

  if(C_pre_gc_hook != NULL) C_pre_gc_hook(GC_REALLOC);

  if(double_plus) size = heap_size * 2 + size;

  if(size < MINIMAL_HEAP_SIZE) size = MINIMAL_HEAP_SIZE;

  /*
   * When heap grows, ensure it's enough to accommodate first
   * generation (nursery).  Because we're calculating the total heap
   * size here (fromspace *AND* tospace), we have to double the stack
   * size, otherwise we'd accommodate only half the stack in the tospace.
   */
  if(size > heap_size && size - heap_size < stack_size * 2)
    size = heap_size + stack_size * 2;
	  
  if(size > C_maximal_heap_size) size = C_maximal_heap_size;

  if(debug_mode) 
    C_dbg(C_text("debug"), C_text("resizing heap dynamically from " UWORD_COUNT_FORMAT_STRING "k to " UWORD_COUNT_FORMAT_STRING "k ...\n"), 
	  heap_size / 1024, size / 1024);

  if(gc_report_flag) {
    C_dbg(C_text("GC"), C_text("(old) fromspace: \tstart=" UWORD_FORMAT_STRING 
			       ", \tlimit=" UWORD_FORMAT_STRING "\n"),
	  (C_word)fromspace_start, (C_word)C_fromspace_limit);
    C_dbg(C_text("GC"), C_text("(old) tospace:   \tstart=" UWORD_FORMAT_STRING
			       ", \tlimit=" UWORD_FORMAT_STRING "\n"),
	  (C_word)tospace_start, (C_word)tospace_limit);
  }

  heap_size = size;         /* Total heap size of the two halves... */
  size /= 2;                /* ...each half is this big */
  
  /*
   * Start by allocating the new heap's fromspace.  After remarking,
   * allocate the other half of the new heap (its tospace).
   *
   * To clarify: what we call "new_space" here is what will eventually
   * be cycled over to "fromspace" when re-reclamation has finished
   * (that is, after the old one has been freed).
   */
  if ((new_heapspace = heap_alloc (size, &new_tospace_start)) == NULL)
    panic(C_text("out of memory - cannot allocate heap segment"));
  new_heapspace_size = size;

  new_tospace_top = new_tospace_start;
  new_tospace_limit = new_tospace_start + size;
  heap_scan_top = new_tospace_top;

  /* Mark items in forwarding table: */
  for(p = forwarding_table; *p != 0; p += 2) {
    last = p[ 1 ];
    remark(&p[ 1 ]);
    C_block_header(p[ 0 ]) = C_block_header(last);
  }

  /* Mark literal frames: */
  for(lfn = lf_list; lfn != NULL; lfn = lfn->next)
    for(i = 0; i < lfn->count; ++i)
      remark(&lfn->lf[i]);

  /* Mark symbol table: */
  for(stp = symbol_table_list; stp != NULL; stp = stp->next)
    for(i = 0; i < stp->size; ++i)
      remark(&stp->table[i]);

  /* Mark collectibles: */
  for(msp = collectibles; msp < collectibles_top; ++msp)
    if(*msp != NULL) remark(*msp);

  for(gcrp = gc_root_list; gcrp != NULL; gcrp = gcrp->next)
    remark(&gcrp->value);

  remark_system_globals();

  /* Clear the mutated slot stack: */
  mutation_stack_top = mutation_stack_bottom;

  /* Mark live values: */
  for(p = C_temporary_stack; p < C_temporary_stack_bottom; ++p)
    remark(p);

  /* Mark locative table: */
  for(i = 0; i < locative_table_count; ++i)
    remark(&locative_table[ i ]);

  /* Mark finalizer table: */
  for(flist = finalizer_list; flist != NULL; flist = flist->next) {
    remark(&flist->item);
    remark(&flist->finalizer);
  }

  /* Clear weakly held items: */
  if(C_enable_gcweak) {
    wep = weak_item_table; 

    for(i = 0; i < WEAK_TABLE_SIZE; ++i, ++wep)
      wep->item = wep->container = 0;
  }

  /* Mark trace-buffer: */
  for(tinfo = trace_buffer; tinfo < trace_buffer_limit; ++tinfo) {
    remark(&tinfo->cooked1);
    remark(&tinfo->cooked2);
    remark(&tinfo->thread);
  }

  update_locative_table(GC_REALLOC);

  /* Mark nested values in already moved (marked) blocks in breadth-first manner: */
  while(heap_scan_top < new_tospace_top) {
    bp = (C_SCHEME_BLOCK *)heap_scan_top;

    if(*((C_word *)bp) == ALIGNMENT_HOLE_MARKER) 
      bp = (C_SCHEME_BLOCK *)((C_word *)bp + 1);

    n = C_header_size(bp);
    h = bp->header;
    assert(!is_fptr(h));
    bytes = (h & C_BYTEBLOCK_BIT) ? n : n * sizeof(C_word);
    p = bp->data;

    if(n > 0 && (h & C_BYTEBLOCK_BIT) == 0) {
      if(h & C_SPECIALBLOCK_BIT) {
	--n;
	++p;
      }

      while(n--) remark(p++);
    }

    heap_scan_top = (C_byte *)bp + C_align(bytes) + sizeof(C_word);
  }

  heap_free (heapspace1, heapspace1_size);
  heap_free (heapspace2, heapspace2_size);
  
  if ((heapspace2 = heap_alloc (size, &tospace_start)) == NULL)
    panic(C_text("out of memory - cannot allocate next heap segment"));
  heapspace2_size = size;

  heapspace1 = new_heapspace;
  heapspace1_size = new_heapspace_size;
  tospace_limit = tospace_start + size;
  tospace_top = tospace_start;
  fromspace_start = new_tospace_start;
  C_fromspace_top = new_tospace_top;
  C_fromspace_limit = new_tospace_limit;

  if(gc_report_flag) {
    C_dbg(C_text("GC"), C_text("resized heap to %d bytes\n"), heap_size);
    C_dbg(C_text("GC"), C_text("(new) fromspace: \tstart=" UWORD_FORMAT_STRING 
			       ", \tlimit=" UWORD_FORMAT_STRING "\n"),
	  (C_word)fromspace_start, (C_word)C_fromspace_limit);
    C_dbg(C_text("GC"), C_text("(new) tospace:   \tstart=" UWORD_FORMAT_STRING
			       ", \tlimit=" UWORD_FORMAT_STRING "\n"),
	  (C_word)tospace_start, (C_word)tospace_limit);
  }

  if(C_post_gc_hook != NULL) C_post_gc_hook(GC_REALLOC, 0);
}


C_regparm void C_fcall remark_system_globals(void)
{
  remark(&C_ratnum_type_tag);
  remark(&C_cplxnum_type_tag);
  remark(&interrupt_hook_symbol);
  remark(&error_hook_symbol);
  remark(&callback_continuation_stack_symbol);
  remark(&pending_finalizers_symbol);
  remark(&current_thread_symbol);
}


C_regparm void C_fcall really_remark(C_word *x)
{
  C_word val, item;
  C_uword n, bytes;
  C_header h;
  C_SCHEME_BLOCK *p, *p2;
  WEAK_TABLE_ENTRY *wep;

  val = *x;

  if (!C_in_stackp(val) && !C_in_heapp(val) && !C_in_new_heapp(val)) {
#ifdef C_GC_HOOKS
      if(C_gc_trace_hook != NULL) 
	C_gc_trace_hook(x, gc_mode);
#endif

      return;
  }

  p = (C_SCHEME_BLOCK *)val;
  
  h = p->header;

  if(is_fptr(h)) {
    val = fptr_to_ptr(h);

    if((C_uword)val >= (C_uword)new_tospace_start && (C_uword)val < (C_uword)new_tospace_top) {
      *x = val;
      return;
    }

    /* Link points into nursery, fromspace or the old tospace:
    * fetch new pointer + header and copy... */
    p = (C_SCHEME_BLOCK *)val;
    h = p->header;
    n = 1;

    while(is_fptr(h)) {
      /* Link points into fromspace or old tospace and into a link which
       * points into tospace or new-tospace: */
      val = fptr_to_ptr(h);
	
      if((C_uword)val >= (C_uword)new_tospace_start && (C_uword)val < (C_uword)new_tospace_top) {
	*x = val;
	return;
      }

      p = (C_SCHEME_BLOCK *)val;
      h = p->header;

      if(++n > 3)
	panic(C_text("forwarding chain during re-reclamation is longer than 3. somethings fishy."));
    }
  }

  p2 = (C_SCHEME_BLOCK *)C_align((C_uword)new_tospace_top);

#ifndef C_SIXTY_FOUR
  if((h & C_8ALIGN_BIT) && C_aligned8(p2) && (C_byte *)p2 < new_tospace_limit) {
    *((C_word *)p2) = ALIGNMENT_HOLE_MARKER;
    p2 = (C_SCHEME_BLOCK *)((C_word *)p2 + 1);
  }
#endif

  n = C_header_size(p);
  bytes = (h & C_BYTEBLOCK_BIT) ? n : n * sizeof(C_word);

  new_tospace_top = ((C_byte *)p2 + C_align(bytes) + sizeof(C_word));
  if(new_tospace_top > new_tospace_limit) {
    panic(C_text("out of memory - heap full while resizing"));
  }

  *x = (C_word)p2;
  p2->header = h;
  assert(!is_fptr(h));
  p->header = ptr_to_fptr((C_word)p2);
  C_memcpy(p2->data, p->data, bytes);
}


C_regparm void C_fcall update_locative_table(int mode)
{
  int i, hi = 0, invalidated = 0;
  C_header h;
  C_word loc, obj, obj2, offset, loc2, ptr;
  C_uword ptr2;

  for(i = 0; i < locative_table_count; ++i) {
    loc = locative_table[ i ];

    if(loc != C_SCHEME_UNDEFINED) {
      h = C_block_header(loc);

      switch(mode) {
      case GC_MINOR:
        if(is_fptr(h))		/* forwarded? update l-table entry */
          loc = locative_table[ i ] = fptr_to_ptr(h);
        /* otherwise it must have been GC'd (since this is a minor one) */
        else if(C_in_stackp(loc)) {
          locative_table[ i ] = C_SCHEME_UNDEFINED;
          C_set_block_item(loc, 0, 0);
	  ++invalidated;
          break;
        }

        /* forwarded. fix up ptr and check pointed-at object for being forwarded... */
        ptr = C_block_item(loc, 0);
        offset = C_unfix(C_block_item(loc, 1));
        obj = ptr - offset;
        h = C_block_header(obj);

        if(is_fptr(h)) {	/* pointed-at object forwarded? update */
          C_set_block_item(loc, 0, (C_uword)fptr_to_ptr(h) + offset);
	  hi = i + 1;
	}
        else if(C_in_stackp(obj)) { /* pointed-at object GC'd, locative is invalid */
          locative_table[ i ] = C_SCHEME_UNDEFINED;
          C_set_block_item(loc, 0, 0);
        }
	else hi = i + 1;
        
        break;

      case GC_MAJOR:
        if(is_fptr(h))		/* forwarded? update l-table entry */
          loc = locative_table[ i ] = fptr_to_ptr(h);
        else {			/* otherwise, throw away */
          locative_table[ i ] = C_SCHEME_UNDEFINED;
          C_set_block_item(loc, 0, 0);
	  ++invalidated;
          break;
        }

        h = C_block_header(loc);
        
        if(is_fptr(h))		/* new instance is forwarded itself? update again */
          loc = locative_table[ i ] = fptr_to_ptr(h);

        ptr = C_block_item(loc, 0); /* fix up ptr */
        offset = C_unfix(C_block_item(loc, 1));
        obj = ptr - offset;
        h = C_block_header(obj);

        if(is_fptr(h)) {	/* pointed-at object has been forwarded? */
	  ptr2 = (C_uword)fptr_to_ptr(h);
	  h = C_block_header(ptr2);

	  if(is_fptr(h)) {	/* secondary forwarding check for pointed-at object */
	    ptr2 = (C_uword)fptr_to_ptr(h) + offset;
	    C_set_block_item(loc, 0, ptr2);
	  }
	  else C_set_block_item(loc, 0, ptr2 + offset); /* everything's fine, fixup pointer */

	  hi = i + 1;
        }
        else {
          locative_table[ i ] = C_SCHEME_UNDEFINED; /* pointed-at object is dead */
          C_set_block_item(loc, 0, 0);
	  ++invalidated;
        }
        
        break;

      case GC_REALLOC:
        ptr = C_block_item(loc, 0); /* just update ptr's pointed-at objects */
        offset = C_unfix(C_block_item(loc, 1));
        obj = ptr - offset;
        remark(&obj);
        C_set_block_item(loc, 0, obj + offset);        
        break;
      }
    }
  }

  if(gc_report_flag && invalidated > 0)
    C_dbg(C_text("GC"), C_text("locative-table entries reclaimed: %d\n"), invalidated);

  if(mode != GC_REALLOC) locative_table_count = hi;
}


C_regparm WEAK_TABLE_ENTRY *C_fcall lookup_weak_table_entry(C_word item, C_word container)
{
  C_uword
    key = (C_uword)item >> 2,
    disp = 0,
    n;
  WEAK_TABLE_ENTRY *wep;

  for(n = 0; n < WEAK_HASH_ITERATIONS; ++n) {
    key = (key + disp + weak_table_randomization) % WEAK_TABLE_SIZE;
    wep = &weak_item_table[ key ];

    if(wep->item == 0) {
      if(container != 0) {
	/* Add fresh entry: */
	wep->item = item;
	wep->container = container;
	return wep;
      }

      return NULL;
    }
    else if(wep->item == item) return wep;
    else disp += WEAK_HASH_DISPLACEMENT;
  }

  return NULL;
}


void handle_interrupt(void *trampoline, void *proc)
{
  C_word *p, x, n;
  double c;

  /* Build vector with context information: */
  n = C_temporary_stack_bottom - C_temporary_stack;
  p = C_alloc(C_SIZEOF_VECTOR(2) + C_SIZEOF_VECTOR(n+1));
  x = (C_word)p;
  *(p++) = C_VECTOR_TYPE | C_BYTEBLOCK_BIT | (2 * sizeof(C_word));
  *(p++) = (C_word)trampoline;
  *(p++) = (C_word)proc;
  C_save(x);
  x = (C_word)p;
  *(p++) = C_VECTOR_TYPE | (n + 1);
  *(p++) = C_restore;
  C_memcpy(p, C_temporary_stack, n * sizeof(C_word));

  /* Restore state to the one at the time of the interrupt: */
  C_temporary_stack = C_temporary_stack_bottom;
  C_stack_limit = saved_stack_limit;

  /* Invoke high-level interrupt handler: */
  C_save(C_fix(pending_interrupts[ --pending_interrupts_count ]));
  C_save(x);
  x = C_block_item(interrupt_hook_symbol, 0);

  if(C_immediatep(x))
    panic(C_text("`##sys#interrupt-hook' is not defined"));

  c = C_cpu_milliseconds() - interrupt_time;
  last_interrupt_latency = c;
  C_timer_interrupt_counter = C_initial_timer_interrupt_period;
  /* <- no continuation is passed: "##sys#interrupt-hook" may not return! */
  C_do_apply(2, x, C_SCHEME_UNDEFINED); 
}


void 
C_unbound_variable(C_word sym)
{
  barf(C_UNBOUND_VARIABLE_ERROR, NULL, sym);
}

/* XXX: This needs to be given a better name.
   C_retrieve used to exist but it just called C_fast_retrieve */
C_regparm C_word C_fcall C_retrieve2(C_word val, char *name)
{
  C_word *p;
  int len;

  if(val == C_SCHEME_UNBOUND) {
    len = C_strlen(name);
    /* this is ok: we won't return from `C_retrieve2'
     * (or the value isn't needed). */
    p = C_alloc(C_SIZEOF_STRING(len));
    C_unbound_variable(C_string2(&p, name));
  }

  return val;
}


void C_ccall
C_invalid_procedure(int c, C_word self, ...)
{
  barf(C_NOT_A_CLOSURE_ERROR, NULL, self);  
}


C_regparm void *C_fcall C_retrieve2_symbol_proc(C_word val, char *name)
{
  C_word *p;
  int len;

  if(val == C_SCHEME_UNBOUND) {
    len = C_strlen(name);
    /* this is ok: we won't return from `C_retrieve2' (or the value isn't needed). */
    p = C_alloc(C_SIZEOF_STRING(len));
    barf(C_UNBOUND_VARIABLE_ERROR, NULL, C_string2(&p, name));
  }

  return C_fast_retrieve_proc(val);
}


C_regparm void C_fcall C_trace(C_char *name)
{
  if(show_trace) {
    C_fputs(name, C_stderr);
    C_fputc('\n', C_stderr);
  }

  if(trace_buffer_top >= trace_buffer_limit) {
    trace_buffer_top = trace_buffer;
    trace_buffer_full = 1;
  }

  trace_buffer_top->raw = name;
  trace_buffer_top->cooked1 = C_SCHEME_FALSE;
  trace_buffer_top->cooked2 = C_SCHEME_FALSE;
  trace_buffer_top->thread = C_block_item(current_thread_symbol, 0);
  ++trace_buffer_top;
}


C_regparm C_word C_fcall C_emit_trace_info2(char *raw, C_word x, C_word y, C_word t)
{
  if(trace_buffer_top >= trace_buffer_limit) {
    trace_buffer_top = trace_buffer;
    trace_buffer_full = 1;
  }

  trace_buffer_top->raw = raw;
  trace_buffer_top->cooked1 = x;
  trace_buffer_top->cooked2 = y;
  trace_buffer_top->thread = t;
  ++trace_buffer_top;
  return x;
}


C_char *C_dump_trace(int start)
{
  TRACE_INFO *ptr;
  C_char *result;
  int i, result_len;

  result_len = STRING_BUFFER_SIZE;
  if((result = (char *)C_malloc(result_len)) == NULL)
    horror(C_text("out of memory - cannot allocate trace-dump buffer"));

  *result = '\0';

  if(trace_buffer_top > trace_buffer || trace_buffer_full) {
    if(trace_buffer_full) {
      i = C_trace_buffer_size;
      C_strlcat(result, C_text("...more...\n"), result_len);
    }
    else i = trace_buffer_top - trace_buffer;

    ptr = trace_buffer_full ? trace_buffer_top : trace_buffer;
    ptr += start;
    i -= start;

    for(;i--; ++ptr) {
      if(ptr >= trace_buffer_limit) ptr = trace_buffer;

      if(C_strlen(result) > STRING_BUFFER_SIZE - 32) {
        result_len = C_strlen(result) * 2;
        result = C_realloc(result, result_len);
	if(result == NULL)
	  horror(C_text("out of memory - cannot reallocate trace-dump buffer"));
      }

      C_strlcat(result, ptr->raw, result_len);

      if(i > 0) C_strlcat(result, "\n", result_len);
      else C_strlcat(result, " \t<--\n", result_len);
    }
  }

  return result;
}


C_regparm void C_fcall C_clear_trace_buffer(void)
{
  int i;

  if(trace_buffer == NULL) {
    if(C_trace_buffer_size < MIN_TRACE_BUFFER_SIZE)
      C_trace_buffer_size = MIN_TRACE_BUFFER_SIZE;

    trace_buffer = (TRACE_INFO *)C_malloc(sizeof(TRACE_INFO) * C_trace_buffer_size);

    if(trace_buffer == NULL)
      panic(C_text("out of memory - cannot allocate trace-buffer"));
  }

  trace_buffer_top = trace_buffer;
  trace_buffer_limit = trace_buffer + C_trace_buffer_size;
  trace_buffer_full = 0;

  for(i = 0; i < C_trace_buffer_size; ++i) {
    trace_buffer[ i ].cooked1 = C_SCHEME_FALSE;
    trace_buffer[ i ].cooked2 = C_SCHEME_FALSE;
    trace_buffer[ i ].thread = C_SCHEME_FALSE;
  }
}

C_word C_resize_trace_buffer(C_word size) {
  int old_size = C_trace_buffer_size;
  assert(trace_buffer);
  free(trace_buffer);
  trace_buffer = NULL;
  C_trace_buffer_size = C_unfix(size);
  C_clear_trace_buffer();
  return(C_fix(old_size));
}

C_word C_fetch_trace(C_word starti, C_word buffer)
{
  TRACE_INFO *ptr;
  int i, p = 0, start = C_unfix(starti);

  if(trace_buffer_top > trace_buffer || trace_buffer_full) {
    if(trace_buffer_full) i = C_trace_buffer_size;
    else i = trace_buffer_top - trace_buffer;

    ptr = trace_buffer_full ? trace_buffer_top : trace_buffer;
    ptr += start;
    i -= start;

    if(C_header_size(buffer) < i * 4)
      panic(C_text("destination buffer too small for call-chain"));

    for(;i--; ++ptr) {
      if(ptr >= trace_buffer_limit) ptr = trace_buffer;

      /* outside-pointer, will be ignored by GC */
      C_mutate2(&C_block_item(buffer, p++), (C_word)ptr->raw);

      /* subject to GC */
      C_mutate2(&C_block_item(buffer, p++), ptr->cooked1);
      C_mutate2(&C_block_item(buffer, p++), ptr->cooked2);
      C_mutate2(&C_block_item(buffer, p++), ptr->thread);
    }
  }

  return C_fix(p);
}

C_regparm C_word C_fcall C_u_i_string_hash(C_word str, C_word rnd)
{
  int len = C_header_size(str);
  C_char *ptr = C_data_pointer(str);
  return C_fix(hash_string(len, ptr, C_MOST_POSITIVE_FIXNUM, C_unfix(rnd), 0));
}

C_regparm C_word C_fcall C_u_i_string_ci_hash(C_word str, C_word rnd)
{
  int len = C_header_size(str);
  C_char *ptr = C_data_pointer(str);
  return C_fix(hash_string(len, ptr, C_MOST_POSITIVE_FIXNUM, C_unfix(rnd), 1));
}

C_regparm void C_fcall C_toplevel_entry(C_char *name)
{
  if(debug_mode)
    C_dbg(C_text("debug"), C_text("entering toplevel %s...\n"), name);
}


C_word C_halt(C_word msg)
{
  C_char *dmp = msg != C_SCHEME_FALSE ? C_dump_trace(0) : NULL;

  if(C_gui_mode) {
    if(msg != C_SCHEME_FALSE) {
      int n = C_header_size(msg);

      if (n >= sizeof(buffer))
	n = sizeof(buffer) - 1;
      C_strlcpy(buffer, (C_char *)C_data_pointer(msg), n);
      /* XXX msg isn't checked for NUL bytes, but we can't barf here either! */
    }
    else C_strlcpy(buffer, C_text("(aborted)"), sizeof(buffer));

    C_strlcat(buffer, C_text("\n\n"), sizeof(buffer));

    if(dmp != NULL) C_strlcat(buffer, dmp, sizeof(buffer));

#if defined(_WIN32) && !defined(__CYGWIN__) 
    MessageBox(NULL, buffer, C_text("CHICKEN runtime"), MB_OK | MB_ICONERROR);
    ExitProcess(1);
#endif
  } /* otherwise fall through */

  if(msg != C_SCHEME_FALSE) {
    C_fwrite(C_data_pointer(msg), C_header_size(msg), sizeof(C_char), C_stderr);
    C_fputc('\n', C_stderr);
  }

  if(dmp != NULL) 
    C_dbg("", C_text("\n%s"), dmp);
  
  C_exit(EX_SOFTWARE);
  return 0;
}


C_word C_message(C_word msg)
{
  unsigned int n = C_header_size(msg);
  /*
   * Strictly speaking this isn't necessary for the non-gui-mode,
   * but let's try and keep this consistent across modes.
   */
  if (memchr(C_c_string(msg), '\0', n) != NULL)
    barf(C_ASCIIZ_REPRESENTATION_ERROR, "##sys#message", msg);

  if(C_gui_mode) {
    if (n >= sizeof(buffer))
      n = sizeof(buffer) - 1;
    C_strncpy(buffer, C_c_string(msg), n);
    buffer[ n ] = '\0';
#if defined(_WIN32) && !defined(__CYGWIN__)
    MessageBox(NULL, buffer, C_text("CHICKEN runtime"), MB_OK | MB_ICONEXCLAMATION);
    return C_SCHEME_UNDEFINED;
#endif
  } /* fall through */

  C_fwrite(C_c_string(msg), n, sizeof(C_char), stdout);
  C_putchar('\n');
  return C_SCHEME_UNDEFINED;
}


C_regparm C_word C_fcall C_equalp(C_word x, C_word y)
{
  C_header header;
  C_word bits, n, i;

  C_stack_check1(barf(C_CIRCULAR_DATA_ERROR, "equal?"));

 loop:
  if(x == y) return 1;

  if(C_immediatep(x) || C_immediatep(y)) return 0;

  if((header = C_block_header(x)) != C_block_header(y)) return 0;
  else if((bits = header & C_HEADER_BITS_MASK) & C_BYTEBLOCK_BIT) {
    if(header == C_FLONUM_TAG && C_block_header(y) == C_FLONUM_TAG)
      return C_flonum_magnitude(x) == C_flonum_magnitude(y);
    else return !C_memcmp(C_data_pointer(x), C_data_pointer(y), header & C_HEADER_SIZE_MASK);
  }
  else if(header == C_SYMBOL_TAG) return 0;
  else {
    i = 0;
    n = header & C_HEADER_SIZE_MASK;

    if(bits & C_SPECIALBLOCK_BIT) {
      /* do not recurse into closures */
      if(C_header_bits(x) == C_CLOSURE_TYPE)
	return !C_memcmp((void *)x, (void *)y, n * sizeof(C_word));
      else if(C_block_item(x, 0) != C_block_item(y, 0)) return 0;
      else ++i;

      if(n == 1) return 1;
    }

    if(--n < 0) return 1;

    while(i < n)
      if(!C_equalp(C_block_item(x, i), C_block_item(y, i))) return 0;
      else ++i;

    x = C_block_item(x, i);
    y = C_block_item(y, i);
    goto loop;
  }
}


C_regparm C_word C_fcall C_set_gc_report(C_word flag)
{
  if(flag == C_SCHEME_FALSE) gc_report_flag = 0;
  else if(flag == C_SCHEME_TRUE) gc_report_flag = 2;
  else gc_report_flag = 1;

  return C_SCHEME_UNDEFINED;
}


C_regparm C_word C_fcall C_start_timer(void)
{
  tracked_mutation_count = 0;
  mutation_count = 0;
  gc_count_1_total = 0;
  gc_count_2 = 0;
  timer_start_ms = C_cpu_milliseconds();
  gc_ms = 0;
  return C_SCHEME_UNDEFINED;
}


void C_ccall C_stop_timer(C_word c, C_word closure, C_word k)
{
  double t0 = C_cpu_milliseconds() - timer_start_ms;
  C_word 
    ab[ WORDS_PER_FLONUM * 2 + C_SIZEOF_VECTOR(6) ],
    *a = ab,
    elapsed = C_flonum(&a, t0 / 1000.0),
    gc_time = C_flonum(&a, gc_ms / 1000.0),
    info;

  info = C_vector(&a, 6, elapsed, gc_time, C_fix(mutation_count),
                  C_fix(tracked_mutation_count), C_fix(gc_count_1_total),
		  C_fix(gc_count_2));
  C_kontinue(k, info);
}


C_word C_exit_runtime(C_word code)
{
  exit(C_unfix(code));
  return 0;			/* to please the compiler... */
}


C_regparm C_word C_fcall C_set_print_precision(C_word n)
{
  flonum_print_precision = C_unfix(n);
  return C_SCHEME_UNDEFINED;
}


C_regparm C_word C_fcall C_get_print_precision(void)
{
  return C_fix(flonum_print_precision);
}


C_regparm C_word C_fcall C_read_char(C_word port)
{
  C_FILEPTR fp = C_port_file(port);
  int c = C_getc(fp);

  if(c == EOF) {
    if(ferror(fp)) {
      clearerr(fp);
      return C_fix(-1);
    }
    /* Found here:
       http://mail.python.org/pipermail/python-bugs-list/2002-July/012579.html */
#if defined(_WIN32) && !defined(__CYGWIN__)
    else if(GetLastError() == ERROR_OPERATION_ABORTED) return C_fix(-1);
#endif
    else return C_SCHEME_END_OF_FILE;
  }

  return C_make_character(c);
}


C_regparm C_word C_fcall C_peek_char(C_word port)
{
  C_FILEPTR fp = C_port_file(port);
  int c = C_getc(fp);

  if(c == EOF) {
    if(ferror(fp)) {
      clearerr(fp);
      return C_fix(-1);
    }
    /* see above */
#if defined(_WIN32) && !defined(__CYGWIN__)
    else if(GetLastError() == ERROR_OPERATION_ABORTED) return C_fix(-1);
#endif
    else return C_SCHEME_END_OF_FILE;
  }

  C_ungetc(c, fp);
  return C_make_character(c);
}


C_regparm C_word C_fcall C_execute_shell_command(C_word string)
{
  int n = C_header_size(string);
  char *buf = buffer;

  /* Windows doc says to flush all output streams before calling system.
     Probably a good idea for all platforms. */
  (void)fflush(NULL);

  if(n >= STRING_BUFFER_SIZE) {
    if((buf = (char *)C_malloc(n + 1)) == NULL)
      barf(C_OUT_OF_MEMORY_ERROR, "system");
  }

  C_memcpy(buf, C_data_pointer(string), n);
  buf[ n ] = '\0';
  if (n != strlen(buf))
    barf(C_ASCIIZ_REPRESENTATION_ERROR, "get-environment-variable", string);

  n = C_system(buf);

  if(buf != buffer) C_free(buf);

  return C_fix(n);
}

/*
 * TODO: Implement something for Windows that supports selecting on
 * arbitrary fds (there, select() only works on network sockets and
 * poll() is not available at all).
 */
C_regparm int C_fcall C_check_fd_ready(int fd)
{
#ifdef NO_POSIX_POLL
  fd_set in;
  struct timeval tm;
  int rv;
  FD_ZERO(&in);
  FD_SET(fd, &in);
  tm.tv_sec = tm.tv_usec = 0;
  rv = select(fd + 1, &in, NULL, NULL, &tm);
  if(rv > 0) { rv = FD_ISSET(fd, &in) ? 1 : 0; }
  return rv;
#else
  struct pollfd ps;
  ps.fd = fd;
  ps.events = POLLIN;
  return poll(&ps, 1, 0);
#endif
}

C_regparm C_word C_fcall C_char_ready_p(C_word port)
{
#if defined(C_NONUNIX)
  /* The best we can currently do on Windows... */
  return C_SCHEME_TRUE;
#else
  int fd = C_fileno(C_port_file(port));
  return C_mk_bool(C_check_fd_ready(fd) == 1);
#endif
}


C_regparm C_word C_fcall C_fudge(C_word fudge_factor)
{
  int i, j;
  double tgc;

  switch(fudge_factor) {
  case C_fix(1): return C_SCHEME_END_OF_FILE; /* eof object */
  case C_fix(2):			      /* get time */
    panic(C_text("(##sys#fudge 2) [get time] not implemented"));

  case C_fix(3):		/* 64-bit system? */
#ifdef C_SIXTY_FOUR
    return C_SCHEME_TRUE;
#else
    return C_SCHEME_FALSE;
#endif

  case C_fix(4):		/* is this a console application? */
    return C_mk_bool(!C_gui_mode);

  case C_fix(5):		/* is this a GUI/console or Windows-GUI application? (silly) */
    if(C_gui_mode) {
#ifdef _WIN32
      return C_fix(1);
#else
      return C_SCHEME_FALSE;
#endif
    }

    return C_fix(0);

  case C_fix(6): 		/* milliseconds CPU */
    panic(C_text("(##sys#fudge 6) [current CPU milliseconds] not implemented"));

  case C_fix(7):		/* wordsize */
    return C_fix(sizeof(C_word));

  case C_fix(8):		/* words needed for double */
    return C_fix(C_wordsperdouble(1));

  case C_fix(9):		/* latency */
    return C_fix(last_interrupt_latency);

  case C_fix(10):		/* clocks per sec */
    return C_fix(CLOCKS_PER_SEC);

  case C_fix(11):		/* not a unix system? */
#if defined(C_NONUNIX) || defined(__CYGWIN__)
    return C_SCHEME_FALSE;
#else
    return C_SCHEME_TRUE;
#endif

  case C_fix(12):		/* tty forced? */
    return C_mk_bool(fake_tty_flag);

  case C_fix(13):		/* debug mode */
    return C_mk_bool(debug_mode);

  case C_fix(14):		/* interrupts enabled? */
    return C_mk_bool(C_interrupts_enabled);

  case C_fix(15):		/* symbol-gc enabled? */
    return C_mk_bool(C_enable_gcweak);

  case C_fix(16):		/* milliseconds (wall clock) */
    panic(C_text("(##sys#fudge 16) [current wall clock milliseconds] not implemented"));

  case C_fix(17):		/* fixed heap? */
    return(C_mk_bool(C_heap_size_is_fixed));

  case C_fix(18):		/* stack direction */
    return(C_fix(C_STACK_GROWS_DOWNWARD));

  case C_fix(19):		/* number of locatives */
    for(i = j = 0; i < locative_table_count; ++i)
      if(locative_table[ i ] != C_SCHEME_UNDEFINED) ++j;
    return C_fix(j);

  case C_fix(20):		/* unused */
    return C_SCHEME_FALSE;

  case C_fix(21):		/* largest fixnum */
    return C_fix(C_MOST_POSITIVE_FIXNUM);

  case C_fix(22):		/* does this process use a private egg-repository? */
    return C_mk_bool(private_repository != NULL);

  case C_fix(23):		/* seconds since process startup */
    return C_fix(C_startup_time_seconds);

  case C_fix(24):		/* dynamic loading available? */
#ifdef NO_DLOAD2
    return C_SCHEME_FALSE;
#else
    return C_SCHEME_TRUE;
#endif

  case C_fix(25):		/* REPL on error? XXX Is this used anywhere? */
    return C_mk_bool(C_enable_repl);

  case C_fix(26):		/* number of untriggered finalizers */
    return C_fix(live_finalizer_count);

  case C_fix(27):		/* total number of finalizers used and unused */
    return C_fix(allocated_finalizer_count);

  case C_fix(28):		/* are procedure-tabled enabled? */
#ifdef C_ENABLE_PTABLES
    return C_SCHEME_TRUE;
#else
    return C_SCHEME_FALSE;
#endif

  case C_fix(29):		/* size of ring-buffer used to hold trace entries */
    return C_fix(C_trace_buffer_size);

  case C_fix(30):		/* unused */
    return C_SCHEME_FALSE;

  case C_fix(31):		/* GC time since last invocation */
    tgc = timer_accumulated_gc_ms;
    timer_accumulated_gc_ms = 0;
    return C_fix(tgc);

  case C_fix(32):		/* are GC-hooks enabled? */
#ifdef C_GC_HOOKS
    return C_SCHEME_TRUE;
#else
    return C_SCHEME_FALSE;
#endif

  case C_fix(33):		/* return process-ID */
    return C_fix(C_getpid());

  case C_fix(34):		/* effective maximum for procedure arguments */
#ifdef C_HACKED_APPLY
    return C_fix(TEMPORARY_STACK_SIZE);
#else
    return C_fix(126);
#endif

  case C_fix(35):		/* unused */
    /* used to be apply-hook indicator */
    return C_SCHEME_FALSE;
    
  case C_fix(36):		/* toggle debug mode */
    debug_mode = !debug_mode;
    return C_mk_bool(debug_mode);

  case C_fix(37):		/* heap-dump enabled? */
    return C_mk_bool(dump_heap_on_exit);

  case C_fix(38):		/* SVN revision of built sources */
#ifdef C_SVN_REVISION
    return C_fix(C_SVN_REVISION);
#else
    return C_fix(0);
#endif

  case C_fix(39):		/* is this a cross-chicken? */
#if defined(C_CROSS_CHICKEN) && C_CROSS_CHICKEN
    return C_SCHEME_TRUE;
#else
    return C_SCHEME_FALSE;
#endif

  case C_fix(40):		/* assembly stub for "apply" available? */
#if defined(C_HACKED_APPLY)
    return C_SCHEME_TRUE;
#else
    return C_SCHEME_FALSE;
#endif

  case C_fix(41):		/* major CHICKEN version */
    return C_fix(C_MAJOR_VERSION);

  case C_fix(42):		/* binary version number */
#ifdef C_BINARY_VERSION
    return C_fix(C_BINARY_VERSION);
#else
    return C_fix(0);
#endif

  case C_fix(43):		/* minor CHICKEN version */
    return C_fix(C_MINOR_VERSION);

  default: return C_SCHEME_UNDEFINED;
  }
}


C_regparm void C_fcall C_paranoid_check_for_interrupt(void)
{
  if(--C_timer_interrupt_counter <= 0)
    C_raise_interrupt(C_TIMER_INTERRUPT_NUMBER);
}


C_regparm void C_fcall C_raise_interrupt(int reason)
{
  if(C_interrupts_enabled) {
    if(pending_interrupts_count == 0 && !handling_interrupts) {
      /* Force the next stack check to fail by faking a "full" stack.
         That causes save_and_reclaim() to be called, which will
         invoke handle_interrupt() (which restores the stack limit). */
      saved_stack_limit = C_stack_limit;

#if C_STACK_GROWS_DOWNWARD
      C_stack_limit = C_stack_pointer + 1000;
#else
      C_stack_limit = C_stack_pointer - 1000;
#endif
      interrupt_time = C_cpu_milliseconds();
      pending_interrupts[ pending_interrupts_count++ ] = reason;
    } else if(pending_interrupts_count < MAX_PENDING_INTERRUPTS) {
      int i;
      /*
       * Drop signals if too many, but don't queue up multiple entries
       * for the same signal.
       */
      for (i = 0; i < pending_interrupts_count; ++i) {
        if (pending_interrupts[i] == reason)
          return;
      }
      pending_interrupts[ pending_interrupts_count++ ] = reason;
    }
  }
}


C_regparm C_word C_fcall C_enable_interrupts(void)
{
  C_timer_interrupt_counter = C_initial_timer_interrupt_period;
  /* assert(C_timer_interrupt_counter > 0); */
  C_interrupts_enabled = 1;
  return C_SCHEME_UNDEFINED;
}


C_regparm C_word C_fcall C_disable_interrupts(void)
{
  C_interrupts_enabled = 0;
  return C_SCHEME_UNDEFINED;
}


C_regparm C_word C_fcall C_establish_signal_handler(C_word signum, C_word reason)
{
  int sig = C_unfix(signum);
#if defined(HAVE_SIGACTION)
  struct sigaction newsig;
#endif

  if(reason == C_SCHEME_FALSE) C_signal(sig, SIG_IGN);
  else {
    signal_mapping_table[ sig ] = C_unfix(reason);
#if defined(HAVE_SIGACTION)
    newsig.sa_flags = 0;
    /* The global signal handler is used for all signals, and
       manipulates a single queue.  Don't allow other signals to
       concurrently arrive while it's doing this, to avoid races. */
    sigfillset(&newsig.sa_mask);
    newsig.sa_handler = global_signal_handler;
    C_sigaction(sig, &newsig, NULL);
#else
    C_signal(sig, global_signal_handler);
#endif
  }

  return C_SCHEME_UNDEFINED;
}


/* Copy blocks into collected or static memory: */

C_regparm C_word C_fcall C_copy_block(C_word from, C_word to)
{
  int n = C_header_size(from);
  C_long bytes;

  if(C_header_bits(from) & C_BYTEBLOCK_BIT) {
    bytes = n;
    C_memcpy((C_SCHEME_BLOCK *)to, (C_SCHEME_BLOCK *)from, bytes + sizeof(C_header));
  }
  else {
    bytes = C_wordstobytes(n);
    C_memcpy((C_SCHEME_BLOCK *)to, (C_SCHEME_BLOCK *)from, bytes + sizeof(C_header));
  }

  return to;
}


C_regparm C_word C_fcall C_evict_block(C_word from, C_word ptr)
{
  int n = C_header_size(from);
  C_long bytes;
  C_word *p = (C_word *)C_pointer_address(ptr);

  if(C_header_bits(from) & C_BYTEBLOCK_BIT) bytes = n;
  else bytes = C_wordstobytes(n);

  C_memcpy(p, (C_SCHEME_BLOCK *)from, bytes + sizeof(C_header));
  return (C_word)p;
}


/* Inline versions of some standard procedures: */

C_regparm C_word C_fcall C_i_listp(C_word x)
{
  C_word fast = x, slow = x;

  while(fast != C_SCHEME_END_OF_LIST)
    if(!C_immediatep(fast) && C_block_header(fast) == C_PAIR_TAG) {
      fast = C_u_i_cdr(fast);
      
      if(fast == C_SCHEME_END_OF_LIST) return C_SCHEME_TRUE;
      else if(!C_immediatep(fast) && C_block_header(fast) == C_PAIR_TAG) {
	fast = C_u_i_cdr(fast);
	slow = C_u_i_cdr(slow);

	if(fast == slow) return C_SCHEME_FALSE;
      }
      else return C_SCHEME_FALSE;
    }
    else return C_SCHEME_FALSE;

  return C_SCHEME_TRUE;
}


C_regparm C_word C_fcall C_i_string_equal_p(C_word x, C_word y)
{
  C_word n;

  if(C_immediatep(x) || C_header_bits(x) != C_STRING_TYPE)
    barf(C_BAD_ARGUMENT_TYPE_ERROR, "string=?", x);

  if(C_immediatep(y) || C_header_bits(y) != C_STRING_TYPE)
    barf(C_BAD_ARGUMENT_TYPE_ERROR, "string=?", y);

  n = C_header_size(x);

  return C_mk_bool(n == C_header_size(y)
                   && !C_memcmp((char *)C_data_pointer(x), (char *)C_data_pointer(y), n));
}


C_regparm C_word C_fcall C_i_string_ci_equal_p(C_word x, C_word y)
{
  C_word n;
  char *p1, *p2;

  if(C_immediatep(x) || C_header_bits(x) != C_STRING_TYPE)
    barf(C_BAD_ARGUMENT_TYPE_ERROR, "string-ci=?", x);

  if(C_immediatep(y) || C_header_bits(y) != C_STRING_TYPE)
    barf(C_BAD_ARGUMENT_TYPE_ERROR, "string-ci=?", y);

  n = C_header_size(x);

  if(n != C_header_size(y)) return C_SCHEME_FALSE;

  p1 = (char *)C_data_pointer(x);
  p2 = (char *)C_data_pointer(y);

  while(n--) {
    if(C_tolower((int)(*(p1++))) != C_tolower((int)(*(p2++))))
      return C_SCHEME_FALSE;
  }

  return C_SCHEME_TRUE;
}


C_word C_a_i_list(C_word **a, int c, ...)
{
  va_list v;
  C_word x, last, current,
         first = C_SCHEME_END_OF_LIST;

  va_start(v, c);

  for(last = C_SCHEME_UNDEFINED; c--; last = current) {
    x = va_arg(v, C_word);
    current = C_a_pair(a, x, C_SCHEME_END_OF_LIST);

    if(last != C_SCHEME_UNDEFINED)
      C_set_block_item(last, 1, current);
    else first = current;
  }

  va_end(v);
  return first;
}


C_word C_a_i_string(C_word **a, int c, ...)
{
  va_list v;
  C_word x, s = (C_word)(*a);
  char *p;

  *a = (C_word *)((C_word)(*a) + sizeof(C_header) + C_align(c));
  C_block_header_init(s, C_STRING_TYPE | c);
  p = (char *)C_data_pointer(s);
  va_start(v, c);

  while(c--) {
    x = va_arg(v, C_word);

    if((x & C_IMMEDIATE_TYPE_BITS) == C_CHARACTER_BITS)
      *(p++) = C_character_code(x);
    else barf(C_BAD_ARGUMENT_TYPE_ERROR, "string", x);
  }

  return s;
}


C_word C_a_i_record(C_word **ptr, int n, ...)
{
  va_list v;
  C_word *p = *ptr,
         *p0 = p; 

  *(p++) = C_STRUCTURE_TYPE | n;
  va_start(v, n);

  while(n--)
    *(p++) = va_arg(v, C_word);

  *ptr = p;
  va_end(v);
  return (C_word)p0;
}


C_word C_a_i_port(C_word **ptr, int n)
{
  C_word 
    *p = *ptr,
    *p0 = p; 
  int i;

  *(p++) = C_PORT_TYPE | (C_SIZEOF_PORT - 1);
  *(p++) = (C_word)NULL;
  
  for(i = 0; i < C_SIZEOF_PORT - 2; ++i)
    *(p++) = C_SCHEME_FALSE;

  *ptr = p;
  return (C_word)p0;
}


C_regparm C_word C_fcall C_a_i_bytevector(C_word **ptr, int c, C_word num)
{
  C_word *p = *ptr,
         *p0;
  int n = C_unfix(num);

#ifndef C_SIXTY_FOUR
  /* Align on 8-byte boundary: */
  if(C_aligned8(p)) ++p;
#endif

  p0 = p;
  *(p++) = C_BYTEVECTOR_TYPE | C_wordstobytes(n);
  *ptr = p + n;
  return (C_word)p0;
}


C_word C_fcall C_a_i_smart_mpointer(C_word **ptr, int c, C_word x)
{
  C_word 
    *p = *ptr,
    *p0 = p;
  void *mp;

  if(C_immediatep(x)) mp = NULL;
  else if((C_header_bits(x) & C_SPECIALBLOCK_BIT) != 0) mp = C_pointer_address(x);
  else mp = C_data_pointer(x);

  *(p++) = C_POINTER_TYPE | 1;
  *((void **)p) = mp;
  *ptr = p + 1;
  return (C_word)p0;
}

C_regparm C_word C_fcall C_i_nanp(C_word x)
{
  if (x & C_FIXNUM_BIT) {
    return C_SCHEME_FALSE;
  } else if (C_immediatep(x)) {
    barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, "nan?", x);
  } else if (C_block_header(x) == C_FLONUM_TAG) {
    return C_u_i_flonum_nanp(x);
  } else if (C_header_bits(x) == C_BIGNUM_TYPE) {
    return C_SCHEME_FALSE;
  } else if (C_block_header(x) == C_STRUCTURE3_TAG) {
    if (C_block_item(x, 0) == C_ratnum_type_tag)
      return C_SCHEME_FALSE;
    else if (C_block_item(x, 0) == C_cplxnum_type_tag)
      return C_mk_bool(C_truep(C_i_nanp(C_block_item(x, 1))) ||
		       C_truep(C_i_nanp(C_block_item(x, 2))));
    else
      barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, "nan?", x);
  } else {
    barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, "nan?", x);
  }
}

C_regparm C_word C_fcall C_i_finitep(C_word x)
{
  if (x & C_FIXNUM_BIT) {
    return C_SCHEME_TRUE;
  } else if (C_immediatep(x)) {
    barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, "finite?", x);
  } else if (C_block_header(x) == C_FLONUM_TAG) {
    return C_u_i_flonum_finitep(x);
  } else if (C_header_bits(x) == C_BIGNUM_TYPE) {
    return C_SCHEME_TRUE;
  } else if (C_block_header(x) == C_STRUCTURE3_TAG) {
    if (C_block_item(x, 0) == C_ratnum_type_tag)
      return C_SCHEME_TRUE;
    else if (C_block_item(x, 0) == C_cplxnum_type_tag)
      return C_and(C_i_finitep(C_block_item(x, 1)),
		   C_i_finitep(C_block_item(x, 2)));
    else
      barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, "finite?", x);
  } else {
    barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, "finite?", x);
  }
}

C_regparm C_word C_fcall C_i_infinitep(C_word x)
{
  if (x & C_FIXNUM_BIT) {
    return C_SCHEME_FALSE;
  } else if (C_immediatep(x)) {
    barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, "infinite?", x);
  } else if (C_block_header(x) == C_FLONUM_TAG) {
    return C_u_i_flonum_infinitep(x);
  } else if (C_header_bits(x) == C_BIGNUM_TYPE) {
    return C_SCHEME_FALSE;
  } else if (C_block_header(x) == C_STRUCTURE3_TAG) {
    if (C_block_item(x, 0) == C_ratnum_type_tag)
      return C_SCHEME_FALSE;
    else if (C_block_item(x, 0) == C_cplxnum_type_tag)
      return C_mk_bool(C_truep(C_i_infinitep(C_block_item(x, 1))) ||
                       C_truep(C_i_infinitep(C_block_item(x, 2))));
    else
      barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, "infinite?", x);
  } else {
    barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, "infinite?", x);
  }
}

C_regparm C_word C_fcall C_i_exactp(C_word x)
{
  if (x & C_FIXNUM_BIT) {
    return C_SCHEME_TRUE;
  } else if (C_immediatep(x)) {
    barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, "exact?", x);
  } else if (C_block_header(x) == C_FLONUM_TAG) {
    return C_SCHEME_FALSE;
  } else if (C_header_bits(x) == C_BIGNUM_TYPE) {
    return C_SCHEME_TRUE;
  } else if (C_block_header(x) == C_STRUCTURE3_TAG) {
    if (C_block_item(x, 0) == C_ratnum_type_tag)
      return C_SCHEME_TRUE;
    else if (C_block_item(x, 0) == C_cplxnum_type_tag)
      return C_i_exactp(C_block_item(x, 1)); /* Exactness of i and r matches */
    else
      barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, "exact?", x);
  } else {
    barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, "exact?", x);
  }
}


C_regparm C_word C_fcall C_i_inexactp(C_word x)
{
  if (x & C_FIXNUM_BIT) {
    return C_SCHEME_FALSE;
  } else if (C_immediatep(x)) {
    barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, "inexact?", x);
  } else if (C_block_header(x) == C_FLONUM_TAG) {
    return C_SCHEME_TRUE;
  } else if (C_header_bits(x) == C_BIGNUM_TYPE) {
    return C_SCHEME_FALSE;
  } else if (C_block_header(x) == C_STRUCTURE3_TAG) {
    if (C_block_item(x, 0) == C_ratnum_type_tag)
      return C_SCHEME_FALSE;
    else if (C_block_item(x, 0) == C_cplxnum_type_tag)
      return C_i_inexactp(C_block_item(x, 1)); /* Exactness of i and r matches */
    else
      barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, "inexact?", x);
  } else {
    barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, "inexact?", x);
  }
}


C_regparm C_word C_fcall C_i_zerop(C_word x)
{
  if (x & C_FIXNUM_BIT) {
    return C_mk_bool(x == C_fix(0));
  } else if (C_immediatep(x)) {
    barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, "zero?", x);
  } else if (C_block_header(x) == C_FLONUM_TAG) {
    return C_mk_bool(C_flonum_magnitude(x) == 0.0);
  } else if (C_header_bits(x) == C_BIGNUM_TYPE ||
	     (C_block_header(x) == C_STRUCTURE3_TAG &&
	      (C_block_item(x, 0) == C_ratnum_type_tag ||
	       C_block_item(x, 0) == C_cplxnum_type_tag))) {
    return C_SCHEME_FALSE;
  } else {
    barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, "zero?", x);
  }
}

/* I */
C_regparm C_word C_fcall C_u_i_zerop(C_word x)
{
  return C_mk_bool(x == C_fix(0) ||
                   (!C_immediatep(x) &&
                    C_block_header(x) == C_FLONUM_TAG &&
                    C_flonum_magnitude(x) == 0.0));
}


C_regparm C_word C_fcall C_i_positivep(C_word x)
{
  if (x & C_FIXNUM_BIT)
    return C_i_fixnum_positivep(x);
  else if (C_immediatep(x))
    barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, "positive?", x);
  else if (C_block_header(x) == C_FLONUM_TAG)
    return C_mk_bool(C_flonum_magnitude(x) > 0.0);
  else if (C_header_bits(x) == C_BIGNUM_TYPE)
    return C_mk_nbool(C_bignum_negativep(x));
  else if (C_block_header(x) == C_STRUCTURE3_TAG &&
           (C_block_item(x, 0) == C_ratnum_type_tag))
    return C_i_integer_positivep(C_block_item(x, 1));
  else if (C_block_header(x) == C_STRUCTURE3_TAG &&
           (C_block_item(x, 0) == C_cplxnum_type_tag))
    barf(C_BAD_ARGUMENT_TYPE_NO_REAL_ERROR, "positive?", x);
  else
    barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, "positive?", x);
}

C_regparm C_word C_fcall C_i_integer_positivep(C_word x)
{
  if (x & C_FIXNUM_BIT) return C_i_fixnum_positivep(x);
  else return C_mk_nbool(C_bignum_negativep(x));
}

/* XXX TODO OBSOLETE: This can be removed after recompiling c-platform.scm */
C_regparm C_word C_fcall C_u_i_positivep(C_word x)
{
  return C_i_positivep(x);
}


C_regparm C_word C_fcall C_i_negativep(C_word x)
{
  if (x & C_FIXNUM_BIT)
    return C_i_fixnum_negativep(x);
  else if (C_immediatep(x))
    barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, "negative?", x);
  else if (C_block_header(x) == C_FLONUM_TAG)
    return C_mk_bool(C_flonum_magnitude(x) < 0.0);
  else if (C_header_bits(x) == C_BIGNUM_TYPE)
    return C_mk_bool(C_bignum_negativep(x));
  else if (C_block_header(x) == C_STRUCTURE3_TAG &&
           (C_block_item(x, 0) == C_ratnum_type_tag))
    return C_i_integer_negativep(C_block_item(x, 1));
  else if (C_block_header(x) == C_STRUCTURE3_TAG &&
           (C_block_item(x, 0) == C_cplxnum_type_tag))
    barf(C_BAD_ARGUMENT_TYPE_NO_REAL_ERROR, "negative?", x);
  else
    barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, "negative?", x);
}


/* XXX TODO OBSOLETE: This can be removed after recompiling c-platform.scm */
C_regparm C_word C_fcall C_u_i_negativep(C_word x)
{
  return C_i_negativep(x);
}

C_regparm C_word C_fcall C_i_integer_negativep(C_word x)
{
  if (x & C_FIXNUM_BIT) return C_i_fixnum_negativep(x);
  else return C_mk_bool(C_bignum_negativep(x));
}


C_regparm C_word C_fcall C_i_evenp(C_word x)
{
  if(x & C_FIXNUM_BIT) {
    return C_i_fixnumevenp(x);
  } else if(C_immediatep(x)) {
    barf(C_BAD_ARGUMENT_TYPE_NO_INTEGER_ERROR, "even?", x);
  } else if (C_block_header(x) == C_FLONUM_TAG) {
    double val, dummy;
    val = C_flonum_magnitude(x);
    if(C_isnan(val) || C_isinf(val) || C_modf(val, &dummy) != 0.0)
      barf(C_BAD_ARGUMENT_TYPE_NO_INTEGER_ERROR, "even?", x);
    else
      return C_mk_bool(fmod(val, 2.0) == 0.0);
  } else if (C_header_bits(x) == C_BIGNUM_TYPE) {
    return C_mk_nbool(C_bignum_digits(x)[0] & 1);
  } else { /* No need to try extended number */
    barf(C_BAD_ARGUMENT_TYPE_NO_INTEGER_ERROR, "even?", x);
  }
}

/* XXX TODO OBSOLETE: This can be removed after recompiling c-platform.scm */
C_regparm C_word C_fcall C_u_i_evenp(C_word x)
{
  return C_i_evenp(x);
}

C_regparm C_word C_fcall C_i_integer_evenp(C_word x)
{
  if (x & C_FIXNUM_BIT) return C_i_fixnumevenp(x);
  return C_mk_nbool(C_bignum_digits(x)[0] & 1);
}


C_regparm C_word C_fcall C_i_oddp(C_word x)
{
  if(x & C_FIXNUM_BIT) {
    return C_i_fixnumoddp(x);
  } else if(C_immediatep(x)) {
    barf(C_BAD_ARGUMENT_TYPE_NO_INTEGER_ERROR, "odd?", x);
  } else if(C_block_header(x) == C_FLONUM_TAG) {
    double val, dummy;
    val = C_flonum_magnitude(x);
    if(C_isnan(val) || C_isinf(val) || C_modf(val, &dummy) != 0.0)
      barf(C_BAD_ARGUMENT_TYPE_NO_INTEGER_ERROR, "odd?", x);
    else
      return C_mk_bool(fmod(val, 2.0) != 0.0);
  } else if (C_header_bits(x) == C_BIGNUM_TYPE) {
    return C_mk_bool(C_bignum_digits(x)[0] & 1);
  } else {
    barf(C_BAD_ARGUMENT_TYPE_NO_INTEGER_ERROR, "odd?", x);
  }
}


/* XXX TODO OBSOLETE: This can be removed after recompiling c-platform.scm */
C_regparm C_word C_fcall C_u_i_oddp(C_word x)
{
  return C_i_oddp(x);
}

C_regparm C_word C_fcall C_i_integer_oddp(C_word x)
{
  if (x & C_FIXNUM_BIT) return C_i_fixnumoddp(x);
  return C_mk_bool(C_bignum_digits(x)[0] & 1);
}


C_regparm C_word C_fcall C_i_car(C_word x)
{
  if(C_immediatep(x) || C_block_header(x) != C_PAIR_TAG)
    barf(C_BAD_ARGUMENT_TYPE_ERROR, "car", x);

  return C_u_i_car(x);
}


C_regparm C_word C_fcall C_i_cdr(C_word x)
{
  if(C_immediatep(x) || C_block_header(x) != C_PAIR_TAG)
    barf(C_BAD_ARGUMENT_TYPE_ERROR, "cdr", x);

  return C_u_i_cdr(x);
}


C_regparm C_word C_fcall C_i_caar(C_word x)
{
  if(C_immediatep(x) || C_block_header(x) != C_PAIR_TAG) {
  bad:
    barf(C_BAD_ARGUMENT_TYPE_ERROR, "caar", x);
  }

  x = C_u_i_car(x);

  if(C_immediatep(x) || C_block_header(x) != C_PAIR_TAG) goto bad;

  return C_u_i_car(x);
}


C_regparm C_word C_fcall C_i_cadr(C_word x)
{
  if(C_immediatep(x) || C_block_header(x) != C_PAIR_TAG) {
  bad:
    barf(C_BAD_ARGUMENT_TYPE_ERROR, "cadr", x);
  }

  x = C_u_i_cdr(x);

  if(C_immediatep(x) || C_block_header(x) != C_PAIR_TAG) goto bad;

  return C_u_i_car(x);
}


C_regparm C_word C_fcall C_i_cdar(C_word x)
{
  if(C_immediatep(x) || C_block_header(x) != C_PAIR_TAG) {
  bad:
    barf(C_BAD_ARGUMENT_TYPE_ERROR, "cdar", x);
  }

  x = C_u_i_car(x);

  if(C_immediatep(x) || C_block_header(x) != C_PAIR_TAG) goto bad;

  return C_u_i_cdr(x);
}


C_regparm C_word C_fcall C_i_cddr(C_word x)
{
  if(C_immediatep(x) || C_block_header(x) != C_PAIR_TAG) {
  bad:
    barf(C_BAD_ARGUMENT_TYPE_ERROR, "cddr", x);
  }

  x = C_u_i_cdr(x);
  if(C_immediatep(x) || C_block_header(x) != C_PAIR_TAG) goto bad;

  return C_u_i_cdr(x);
}


C_regparm C_word C_fcall C_i_caddr(C_word x)
{
  if(C_immediatep(x) || C_block_header(x) != C_PAIR_TAG) {
  bad:
    barf(C_BAD_ARGUMENT_TYPE_ERROR, "caddr", x);
  }

  x = C_u_i_cdr(x);
  if(C_immediatep(x) || C_block_header(x) != C_PAIR_TAG) goto bad;
  x = C_u_i_cdr(x);
  if(C_immediatep(x) || C_block_header(x) != C_PAIR_TAG) goto bad;

  return C_u_i_car(x);
}


C_regparm C_word C_fcall C_i_cdddr(C_word x)
{
  if(C_immediatep(x) || C_block_header(x) != C_PAIR_TAG) {
  bad:
    barf(C_BAD_ARGUMENT_TYPE_ERROR, "cdddr", x);
  }

  x = C_u_i_cdr(x);
  if(C_immediatep(x) || C_block_header(x) != C_PAIR_TAG) goto bad;
  x = C_u_i_cdr(x);
  if(C_immediatep(x) || C_block_header(x) != C_PAIR_TAG) goto bad;

  return C_u_i_cdr(x);
}


C_regparm C_word C_fcall C_i_cadddr(C_word x)
{
  if(C_immediatep(x) || C_block_header(x) != C_PAIR_TAG) {
  bad:
    barf(C_BAD_ARGUMENT_TYPE_ERROR, "cadddr", x);
  }

  x = C_u_i_cdr(x);
  if(C_immediatep(x) || C_block_header(x) != C_PAIR_TAG) goto bad;
  x = C_u_i_cdr(x);
  if(C_immediatep(x) || C_block_header(x) != C_PAIR_TAG) goto bad;
  x = C_u_i_cdr(x);
  if(C_immediatep(x) || C_block_header(x) != C_PAIR_TAG) goto bad;

  return C_u_i_car(x);
}


C_regparm C_word C_fcall C_i_cddddr(C_word x)
{
  if(C_immediatep(x) || C_block_header(x) != C_PAIR_TAG) {
  bad:
    barf(C_BAD_ARGUMENT_TYPE_ERROR, "cddddr", x);
  }

  x = C_u_i_cdr(x);
  if(C_immediatep(x) || C_block_header(x) != C_PAIR_TAG) goto bad;
  x = C_u_i_cdr(x);
  if(C_immediatep(x) || C_block_header(x) != C_PAIR_TAG) goto bad;
  x = C_u_i_cdr(x);
  if(C_immediatep(x) || C_block_header(x) != C_PAIR_TAG) goto bad;

  return C_u_i_cdr(x);
}


C_regparm C_word C_fcall C_i_list_tail(C_word lst, C_word i)
{
  C_word lst0 = lst;
  int n;

  if(lst != C_SCHEME_END_OF_LIST && 
     (C_immediatep(lst) || C_block_header(lst) != C_PAIR_TAG))
    barf(C_BAD_ARGUMENT_TYPE_ERROR, "list-tail", lst);

  if(i & C_FIXNUM_BIT) n = C_unfix(i);
  else barf(C_BAD_ARGUMENT_TYPE_ERROR, "list-tail", i);

  while(n--) {
    if(C_immediatep(lst) || C_block_header(lst) != C_PAIR_TAG)
      barf(C_OUT_OF_RANGE_ERROR, "list-tail", lst0, i);
    
    lst = C_u_i_cdr(lst);
  }

  return lst;
}


C_regparm C_word C_fcall C_i_vector_ref(C_word v, C_word i)
{
  int j;

  if(C_immediatep(v) || C_header_bits(v) != C_VECTOR_TYPE)
    barf(C_BAD_ARGUMENT_TYPE_ERROR, "vector-ref", v);

  if(i & C_FIXNUM_BIT) {
    j = C_unfix(i);

    if(j < 0 || j >= C_header_size(v)) barf(C_OUT_OF_RANGE_ERROR, "vector-ref", v, i);

    return C_block_item(v, j);
  }
  
  barf(C_BAD_ARGUMENT_TYPE_ERROR, "vector-ref", i);
  return C_SCHEME_UNDEFINED;
}


C_regparm C_word C_fcall C_i_block_ref(C_word x, C_word i)
{
  int j;

  if(C_immediatep(x) || (C_header_bits(x) & C_BYTEBLOCK_BIT) != 0)
    barf(C_BAD_ARGUMENT_TYPE_NO_BLOCK_ERROR, "##sys#block-ref", x);

  if(i & C_FIXNUM_BIT) {
    j = C_unfix(i);

    if(j < 0 || j >= C_header_size(x)) barf(C_OUT_OF_RANGE_ERROR, "##sys#block-ref", x, i);

    return C_block_item(x, j);
  }
  
  barf(C_BAD_ARGUMENT_TYPE_ERROR, "##sys#block-ref", i);
  return C_SCHEME_UNDEFINED;
}


C_regparm C_word C_fcall C_i_string_set(C_word s, C_word i, C_word c)
{
  int j;

  if(C_immediatep(s) || C_header_bits(s) != C_STRING_TYPE)
    barf(C_BAD_ARGUMENT_TYPE_ERROR, "string-set!", s);

  if(!C_immediatep(c) || (c & C_IMMEDIATE_TYPE_BITS) != C_CHARACTER_BITS)
    barf(C_BAD_ARGUMENT_TYPE_ERROR, "string-set!", c);

  if(i & C_FIXNUM_BIT) {
    j = C_unfix(i);

    if(j < 0 || j >= C_header_size(s)) barf(C_OUT_OF_RANGE_ERROR, "string-set!", s, i);

    return C_setsubchar(s, i, c);
  }

  barf(C_BAD_ARGUMENT_TYPE_ERROR, "string-set!", i);
  return C_SCHEME_UNDEFINED;
}


C_regparm C_word C_fcall C_i_string_ref(C_word s, C_word i)
{
  int j;

  if(C_immediatep(s) || C_header_bits(s) != C_STRING_TYPE)
    barf(C_BAD_ARGUMENT_TYPE_ERROR, "string-ref", s);

  if(i & C_FIXNUM_BIT) {
    j = C_unfix(i);

    if(j < 0 || j >= C_header_size(s)) barf(C_OUT_OF_RANGE_ERROR, "string-ref", s, i);

    return C_subchar(s, i);
  }
  
  barf(C_BAD_ARGUMENT_TYPE_ERROR, "string-ref", i);
  return C_SCHEME_UNDEFINED;
}


C_regparm C_word C_fcall C_i_vector_length(C_word v)
{
  if(C_immediatep(v) || C_header_bits(v) != C_VECTOR_TYPE)
    barf(C_BAD_ARGUMENT_TYPE_ERROR, "vector-length", v);

  return C_fix(C_header_size(v));
}


C_regparm C_word C_fcall C_i_string_length(C_word s)
{
  if(C_immediatep(s) || C_header_bits(s) != C_STRING_TYPE)
    barf(C_BAD_ARGUMENT_TYPE_ERROR, "string-length", s);

  return C_fix(C_header_size(s));
}


C_regparm C_word C_fcall C_i_length(C_word lst)
{
  C_word fast = lst, slow = lst;
  int n = 0;

  while(slow != C_SCHEME_END_OF_LIST) {
    if(fast != C_SCHEME_END_OF_LIST) {
      if(!C_immediatep(fast) && C_block_header(fast) == C_PAIR_TAG) {
	fast = C_u_i_cdr(fast);
      
	if(fast != C_SCHEME_END_OF_LIST) {
	  if(!C_immediatep(fast) && C_block_header(fast) == C_PAIR_TAG) {
	    fast = C_u_i_cdr(fast);
	  }
	  else barf(C_NOT_A_PROPER_LIST_ERROR, "length", lst);
	}

	if(fast == slow) 
	  barf(C_BAD_ARGUMENT_TYPE_CYCLIC_LIST_ERROR, "length", lst);
      }
    }

    if(C_immediatep(slow) || C_block_header(lst) != C_PAIR_TAG)
      barf(C_NOT_A_PROPER_LIST_ERROR, "length", lst);

    slow = C_u_i_cdr(slow);
    ++n;
  }

  return C_fix(n);
}


C_regparm C_word C_fcall C_u_i_length(C_word lst)
{
  int n = 0;

  while(!C_immediatep(lst) && C_block_header(lst) == C_PAIR_TAG) {
    lst = C_u_i_cdr(lst);
    ++n;
  }

  return C_fix(n);
}

/* XXX TODO OBSOLETE: This can be removed after recompiling c-platform.scm */
C_regparm C_word maybe_inexact_to_exact(C_word n)
{
  double m;
  C_word r;
  
  if(modf(C_flonum_magnitude(n), &m) == 0.0) {
    r = (C_word)m;
    
    if(r == m && C_fitsinfixnump(r))
      return C_fix(r);
  }
  return C_SCHEME_FALSE;
}

/* XXX TODO OBSOLETE: This can be removed after recompiling c-platform.scm */
C_regparm C_word C_fcall C_i_inexact_to_exact(C_word n)
{
  C_word r;
  
  if(n & C_FIXNUM_BIT) return n;
  else if(C_immediatep(n) || C_block_header(n) != C_FLONUM_TAG)
    barf(C_BAD_ARGUMENT_TYPE_ERROR, "inexact->exact", n);

  r = maybe_inexact_to_exact(n);
  if (r != C_SCHEME_FALSE) return r;

  barf(C_CANT_REPRESENT_INEXACT_ERROR, "inexact->exact", n);
  return 0;
}


C_regparm C_word C_fcall C_i_set_car(C_word x, C_word val)
{
  if(C_immediatep(x) || C_block_header(x) != C_PAIR_TAG)
    barf(C_BAD_ARGUMENT_TYPE_ERROR, "set-car!", x);

  C_mutate2(&C_u_i_car(x), val);
  return C_SCHEME_UNDEFINED;
}


C_regparm C_word C_fcall C_i_set_cdr(C_word x, C_word val)
{
  if(C_immediatep(x) || C_block_header(x) != C_PAIR_TAG)
    barf(C_BAD_ARGUMENT_TYPE_ERROR, "set-cdr!", x);

  C_mutate2(&C_u_i_cdr(x), val);
  return C_SCHEME_UNDEFINED;
}


C_regparm C_word C_fcall C_i_vector_set(C_word v, C_word i, C_word x)
{
  int j;

  if(C_immediatep(v) || C_header_bits(v) != C_VECTOR_TYPE)
    barf(C_BAD_ARGUMENT_TYPE_ERROR, "vector-set!", v);

  if(i & C_FIXNUM_BIT) {
    j = C_unfix(i);

    if(j < 0 || j >= C_header_size(v)) barf(C_OUT_OF_RANGE_ERROR, "vector-set!", v, i);

    C_mutate2(&C_block_item(v, j), x);
  }
  else barf(C_BAD_ARGUMENT_TYPE_ERROR, "vector-set!", i);

  return C_SCHEME_UNDEFINED;
}

void C_ccall C_abs(C_word c, C_word self, C_word k, C_word x)
{
  if (c != 3) {
    C_bad_argc_2(c, 3, self);
  } else if (x & C_FIXNUM_BIT) {
    C_word *a = C_alloc(C_SIZEOF_FIX_BIGNUM);
    C_kontinue(k, C_a_i_fixnum_abs(&a, 1, x));
  } else if (C_immediatep(x)) {
    barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, "abs", x);
  } else if (C_block_header(x) == C_FLONUM_TAG) {
    C_word *a = C_alloc(C_SIZEOF_FLONUM);
    C_kontinue(k, C_a_i_flonum_abs(&a, 1, x));
  } else if (C_header_bits(x) == C_BIGNUM_TYPE) {
    C_u_integer_abs(3, (C_word)NULL, k, x);
  } else {
    try_extended_number("\003sysextended-abs", 2, k, x);
  }
}

void C_ccall C_u_integer_abs(C_word c, C_word self, C_word k, C_word x)
{
  if (x & C_FIXNUM_BIT) {
    C_word *a = C_alloc(C_SIZEOF_FIX_BIGNUM);
    C_kontinue(k, C_a_i_fixnum_abs(&a, 1, x));
  } else if (C_bignum_negativep(x)) {
    C_u_integer_negate(3, (C_word)NULL, k, x);
  } else {
    C_kontinue(k, x);
  }
}

/* XXX TODO OBSOLETE: This can be removed after recompiling c-platform.scm */
C_regparm C_word C_fcall C_a_i_abs(C_word **a, int c, C_word x)
{
  if(x & C_FIXNUM_BIT) return C_fix(labs(C_unfix(x)));

  if(C_immediatep(x) || C_block_header(x) != C_FLONUM_TAG)
    barf(C_BAD_ARGUMENT_TYPE_ERROR, "abs", x);

  return C_flonum(a, fabs(C_flonum_magnitude(x)));
}

void C_ccall C_negate(C_word c, C_word self, C_word k, C_word x)
{
  if (x & C_FIXNUM_BIT) {
    C_word *a = C_alloc(C_SIZEOF_FIX_BIGNUM);
    C_kontinue(k, C_a_i_fixnum_negate(&a, 1, x));
  } else if (C_immediatep(x)) {
    barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, "-", x);
  } else if (C_block_header(x) == C_FLONUM_TAG) {
    C_word *a = C_alloc(C_SIZEOF_FLONUM);
    C_kontinue(k, C_a_i_flonum_negate(&a, 1, x));
  } else if (C_header_bits(x) == C_BIGNUM_TYPE) {
    C_u_integer_negate(3, (C_word)NULL, k, x);
  } else {
    try_extended_number("\003sysextended-negate", 2, k, x);
  }
}

void C_ccall C_u_integer_negate(C_word c, C_word self, C_word k, C_word x)
{
  if (x & C_FIXNUM_BIT) {
    C_word *a = C_alloc(C_SIZEOF_FIX_BIGNUM);
    C_kontinue(k, C_a_i_fixnum_negate(&a, 1, x));
  } else {
    if (C_bignum_negated_fitsinfixnump(x)) {
      C_kontinue(k, C_fix(C_MOST_NEGATIVE_FIXNUM));
    } else {
      C_word *ka, k2, negp = C_mk_nbool(C_bignum_negativep(x)),
             size = C_fix(C_bignum_size(x));
      ka = C_alloc(C_SIZEOF_CLOSURE(3));
      k2 = C_closure(&ka, 3, (C_word)bignum_negate_2, k, x);
      C_allocate_bignum(5, (C_word)NULL, k2, size, negp, C_SCHEME_FALSE);
    }
  }
}

/* Copy all the digits from source to target, obliterating what was
 * there.  If target is larger than source, the most significant
 * digits will remain untouched.
 */
C_inline void bignum_digits_destructive_copy(C_word target, C_word source)
{
  C_memcpy(C_bignum_digits(target), C_bignum_digits(source),
           C_wordstobytes(C_bignum_size(source)));
}

static void bignum_negate_2(C_word c, C_word self, C_word new_big)
{
  bignum_digits_destructive_copy(new_big, C_block_item(self, 2) /* old_big */);
  C_kontinue(C_block_item(self, 1), C_bignum_simplify(new_big));
}

/* XXX TODO OBSOLETE: This can be removed after recompiling c-platform.scm */
C_regparm C_word C_fcall C_a_i_bitwise_and(C_word **a, int c, C_word n1, C_word n2)
{
  double f1, f2;
  C_uword nn1, nn2;

  C_check_uint(n1, f1, nn1, "bitwise-and");
  C_check_uint(n2, f2, nn2, "bitwise-and");
  nn1 = C_limit_fixnum(nn1 & nn2);

  if(C_ufitsinfixnump(nn1)) return C_fix(nn1);
  else return C_flonum(a, nn1);
}


/* XXX TODO OBSOLETE: This can be removed after recompiling c-platform.scm */
C_regparm C_word C_fcall C_a_i_bitwise_ior(C_word **a, int c, C_word n1, C_word n2)
{
  double f1, f2;
  C_uword nn1, nn2;

  C_check_uint(n1, f1, nn1, "bitwise-ior");
  C_check_uint(n2, f2, nn2, "bitwise-ior");
  nn1 = C_limit_fixnum(nn1 | nn2);

  if(C_ufitsinfixnump(nn1)) return C_fix(nn1);
  else return C_flonum(a, nn1);
}


/* XXX TODO OBSOLETE: This can be removed after recompiling c-platform.scm */
C_regparm C_word C_fcall C_a_i_bitwise_xor(C_word **a, int c, C_word n1, C_word n2)
{
  double f1, f2;
  C_uword nn1, nn2;

  C_check_uint(n1, f1, nn1, "bitwise-xor");
  C_check_uint(n2, f2, nn2, "bitwise-xor");
  nn1 = C_limit_fixnum(nn1 ^ nn2);

  if(C_ufitsinfixnump(nn1)) return C_fix(nn1);
  else return C_flonum(a, nn1);
}

C_regparm C_word C_fcall C_u_i_integer_length(C_word x)
{
  if (x & C_FIXNUM_BIT) {
    return C_i_fixnum_length(x);
  } else {
    C_uword result = (C_bignum_size(x) - 1) * C_BIGNUM_DIGIT_LENGTH,
            *last_digit = C_bignum_digits(x) + C_bignum_size(x) - 1,
            last_digit_length = C_ilen(*last_digit);

    /* If *only* the highest bit is set, negating will give one less bit */
    if (C_bignum_negativep(x) &&
        *last_digit == ((C_uword)1 << (last_digit_length-1))) {
      C_uword *startx = C_bignum_digits(x);
      while (startx < last_digit && *startx == 0) ++startx;
      if (startx == last_digit) result--;
    }
    return C_fix(result + last_digit_length);
  }
}

/* XXX TODO OBSOLETE: This can be removed after recompiling c-platform.scm */
C_regparm C_word C_fcall C_i_bit_setp(C_word n, C_word i)
{
  double f1;
  C_uword nn1;
  int index;

  if((i & C_FIXNUM_BIT) == 0) 
    barf(C_BAD_ARGUMENT_TYPE_NO_FIXNUM_ERROR, "bit-set?", i);

  index = C_unfix(i);

  if(index < 0 || index >= C_WORD_SIZE)
    barf(C_OUT_OF_RANGE_ERROR, "bit-set?", n, i);

  C_check_uint(n, f1, nn1, "bit-set?");
  return C_mk_bool((nn1 & (1 << index)) != 0);
}

/* This returns a tmp bignum negated copy of X (must be freed!) when
 * the number is negative, or #f if it doesn't need to be negated.
 * The size can be larger or smaller than X (it may be 1-padded).
 */
C_inline C_word maybe_negate_bignum_for_bitwise_op(C_word x, C_word size)
{
  C_word nx = C_SCHEME_FALSE, xsize;
  if (C_bignum_negativep(x)) {
    nx = allocate_tmp_bignum(C_fix(size), C_SCHEME_FALSE, C_SCHEME_FALSE);
    xsize = C_bignum_size(x);
    /* Copy up until requested size, and init any remaining upper digits */
    C_memcpy(C_bignum_digits(nx), C_bignum_digits(x),
             C_wordstobytes(nmin(size, xsize)));
    if (size > xsize)
      C_memset(C_bignum_digits(nx)+xsize, 0, C_wordstobytes(size-xsize));
    bignum_digits_destructive_negate(nx);
  }
  return nx;
}

C_regparm C_word C_fcall
C_u_i_integer_bit_setp(C_word n, C_word i)
{
  if (!(i & C_FIXNUM_BIT)) { /* A bit silly, but might be useful */
    return C_i_integer_negativep(n);
  } else if (i & C_INT_SIGN_BIT) {
    barf(C_BAD_ARGUMENT_TYPE_NO_UINTEGER_ERROR, "bit-set?", n, i);
  } else {
    i = C_unfix(i);
    if (n & C_FIXNUM_BIT) {
      if (i >= C_WORD_SIZE) return C_mk_bool(n & C_INT_SIGN_BIT);
      else return C_mk_bool((C_unfix(n) & ((C_word)1 << i)) != 0);
    } else {
      C_word nn, d;
      d = i / C_BIGNUM_DIGIT_LENGTH;
      if (d >= C_bignum_size(n)) return C_mk_bool(C_bignum_negativep(n));

      /* TODO: this isn't necessary, is it? */
      if (C_truep(nn = maybe_negate_bignum_for_bitwise_op(n, d))) n = nn;

      i %= C_BIGNUM_DIGIT_LENGTH;
      d = C_mk_bool((C_bignum_digits(n)[d] & (C_uword)1 << i) != 0);
      if (C_truep(nn)) free_tmp_bignum(nn);
      return d;
    }
  }
}

/* XXX TODO OBSOLETE: This can be removed after recompiling c-platform.scm */
C_regparm C_word C_fcall C_a_i_bitwise_not(C_word **a, int c, C_word n)
{
  double f;
  C_uword nn;

  C_check_uint(n, f, nn, "bitwise-not");
  nn = C_limit_fixnum(~nn);

  if(C_ufitsinfixnump(nn)) return C_fix(nn);
  else return C_flonum(a, nn);
}

void C_ccall
C_u_2_integer_bitwise_and(C_word c, C_word self, C_word k, C_word x, C_word y)
{
  if ((x & y) & C_FIXNUM_BIT) {
    C_kontinue(k, C_u_fixnum_and(x, y));
  } else {
    C_word kab[C_SIZEOF_FIX_BIGNUM*2], *ka = kab, negp, size, k2;
    if (x & C_FIXNUM_BIT) x = C_a_u_i_fix_to_big(&ka, x);
    if (y & C_FIXNUM_BIT) y = C_a_u_i_fix_to_big(&ka, y);

    negp = C_mk_bool(C_bignum_negativep(x) && C_bignum_negativep(y));
    /* Allow negative 1-bits to propagate */
    if (C_bignum_negativep(x) || C_bignum_negativep(y))
      size = C_fix(nmax(C_bignum_size(x), C_bignum_size(y)) + 1);
    else
      size = C_fix(nmin(C_bignum_size(x), C_bignum_size(y)));

    ka = C_alloc(C_SIZEOF_CLOSURE(4)); /* Why faster than static alloc? */
    k2 = C_closure(&ka, 4, (C_word)bignum_bitwise_and_2, k, x, y);
    C_allocate_bignum(5, (C_word)NULL, k2, C_fix(size), negp, C_SCHEME_FALSE);
  }
}

static void bignum_bitwise_and_2(C_word c, C_word self, C_word result)
{
  C_word k = C_block_item(self, 1),
	 x = C_block_item(self, 2),
	 y = C_block_item(self, 3),
         size = C_bignum_size(result), nx, ny;
  C_uword *scanr = C_bignum_digits(result),
          *endr = scanr + C_bignum_size(result),
          *scans1, *ends1, *scans2;

  if (C_truep(nx = maybe_negate_bignum_for_bitwise_op(x, size))) x = nx;
  if (C_truep(ny = maybe_negate_bignum_for_bitwise_op(y, size))) y = ny;

  if (C_bignum_size(x) < C_bignum_size(y)) {
    scans1 = C_bignum_digits(x); ends1 = scans1 + C_bignum_size(x);
    scans2 = C_bignum_digits(y);
  } else {
    scans1 = C_bignum_digits(y); ends1 = scans1 + C_bignum_size(y);
    scans2 = C_bignum_digits(x);
  }

  while (scans1 < ends1) *scanr++ = *scans1++ & *scans2++;
  C_memset(scanr, 0, C_wordstobytes(endr - scanr));

  if (C_truep(nx)) free_tmp_bignum(nx);
  if (C_truep(ny)) free_tmp_bignum(ny);
  if (C_bignum_negativep(result)) bignum_digits_destructive_negate(result);

  C_kontinue(k, C_bignum_simplify(result));
}

void C_ccall
C_u_2_integer_bitwise_ior(C_word c, C_word self, C_word k, C_word x, C_word y)
{
  if ((x & y) & C_FIXNUM_BIT) {
    C_kontinue(k, C_u_fixnum_or(x, y));
  } else {
    C_word kab[C_SIZEOF_FIX_BIGNUM*2], *ka = kab, negp, size, k2;
    if (x & C_FIXNUM_BIT) x = C_a_u_i_fix_to_big(&ka, x);
    if (y & C_FIXNUM_BIT) y = C_a_u_i_fix_to_big(&ka, y);

    ka = C_alloc(C_SIZEOF_CLOSURE(4)); /* Why faster than static alloc? */
    k2 = C_closure(&ka, 4, (C_word)bignum_bitwise_ior_2, k, x, y);
    size = C_fix(nmax(C_bignum_size(x), C_bignum_size(y)) + 1);
    negp = C_mk_bool(C_bignum_negativep(x) || C_bignum_negativep(y));
    C_allocate_bignum(5, (C_word)NULL, k2, size, negp, C_SCHEME_FALSE);
  }
}

static void bignum_bitwise_ior_2(C_word c, C_word self, C_word result)
{
  C_word k = C_block_item(self, 1),
	 x = C_block_item(self, 2),
	 y = C_block_item(self, 3),
         size = C_bignum_size(result), nx, ny;
  C_uword *scanr = C_bignum_digits(result),
          *endr = scanr + C_bignum_size(result),
          *scans1, *ends1, *scans2, *ends2;

  if (C_truep(nx = maybe_negate_bignum_for_bitwise_op(x, size))) x = nx;
  if (C_truep(ny = maybe_negate_bignum_for_bitwise_op(y, size))) y = ny;

  if (C_bignum_size(x) < C_bignum_size(y)) {
    scans1 = C_bignum_digits(x); ends1 = scans1 + C_bignum_size(x);
    scans2 = C_bignum_digits(y); ends2 = scans2 + C_bignum_size(y);
  } else {
    scans1 = C_bignum_digits(y); ends1 = scans1 + C_bignum_size(y);
    scans2 = C_bignum_digits(x); ends2 = scans2 + C_bignum_size(x);
  }

  while (scans1 < ends1) *scanr++ = *scans1++ | *scans2++;
  while (scans2 < ends2) *scanr++ = *scans2++;
  if (scanr < endr) *scanr++ = 0; /* Only done when result is positive */
  assert(scanr == endr);

  if (C_truep(nx)) free_tmp_bignum(nx);
  if (C_truep(ny)) free_tmp_bignum(ny);
  if (C_bignum_negativep(result)) bignum_digits_destructive_negate(result);

  C_kontinue(k, C_bignum_simplify(result));
}

void C_ccall
C_u_2_integer_bitwise_xor(C_word c, C_word self, C_word k, C_word x, C_word y)
{
  if ((x & y) & C_FIXNUM_BIT) {
    C_kontinue(k, C_fixnum_xor(x, y));
  } else {
    C_word kab[C_SIZEOF_FIX_BIGNUM*2], *ka = kab, size, k2, negp;
    if (x & C_FIXNUM_BIT) x = C_a_u_i_fix_to_big(&ka, x);
    if (y & C_FIXNUM_BIT) y = C_a_u_i_fix_to_big(&ka, y);

    ka = C_alloc(C_SIZEOF_CLOSURE(4)); /* Why faster than static alloc? */
    k2 = C_closure(&ka, 4, (C_word)bignum_bitwise_xor_2, k, x, y);
    size = C_fix(nmax(C_bignum_size(x), C_bignum_size(y)) + 1);
    negp = C_mk_bool(C_bignum_negativep(x) != C_bignum_negativep(y));
    C_allocate_bignum(5, (C_word)NULL, k2, size, negp, C_SCHEME_FALSE);
  }
}

static void bignum_bitwise_xor_2(C_word c, C_word self, C_word result)
{
  C_word k = C_block_item(self, 1),
	 x = C_block_item(self, 2),
	 y = C_block_item(self, 3),
         size = C_bignum_size(result), nx, ny;
  C_uword *scanr = C_bignum_digits(result),
          *endr = scanr + C_bignum_size(result),
          *scans1, *ends1, *scans2, *ends2;

  if (C_truep(nx = maybe_negate_bignum_for_bitwise_op(x, size))) x = nx;
  if (C_truep(ny = maybe_negate_bignum_for_bitwise_op(y, size))) y = ny;

  if (C_bignum_size(x) < C_bignum_size(y)) {
    scans1 = C_bignum_digits(x); ends1 = scans1 + C_bignum_size(x);
    scans2 = C_bignum_digits(y); ends2 = scans2 + C_bignum_size(y);
  } else {
    scans1 = C_bignum_digits(y); ends1 = scans1 + C_bignum_size(y);
    scans2 = C_bignum_digits(x); ends2 = scans2 + C_bignum_size(x);
  }

  while (scans1 < ends1) *scanr++ = *scans1++ ^ *scans2++;
  while (scans2 < ends2) *scanr++ = *scans2++;
  if (scanr < endr) *scanr++ = 0; /* Only done when result is positive */
  assert(scanr == endr);

  if (C_truep(nx)) free_tmp_bignum(nx);
  if (C_truep(ny)) free_tmp_bignum(ny);
  if (C_bignum_negativep(result)) bignum_digits_destructive_negate(result);

  C_kontinue(k, C_bignum_simplify(result));
}

/* XXX TODO OBSOLETE: This can be removed after recompiling c-platform.scm */
C_regparm C_word C_fcall C_a_i_arithmetic_shift(C_word **a, int c, C_word n1, C_word n2)
{
  C_word nn;
  C_uword unn;
  C_word s;
  int sgn = 1;

  if((n1 & C_FIXNUM_BIT) != 0) {
    nn = C_unfix(n1);

    if((sgn = nn < 0 ? -1 : 1) >= 0) unn = nn;
  }
  else if(C_immediatep(n1) || C_block_header(n1) != C_FLONUM_TAG)
    barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, "arithmetic-shift", n1);
  else { 
    double m, f;

    f = C_flonum_magnitude(n1);
    
    if(C_isnan(f) || C_isinf(f) || C_modf(f, &m) != 0.0)
      barf(C_BAD_ARGUMENT_TYPE_NO_INTEGER_ERROR, "arithmetic-shift", n1);

    if(f < C_WORD_MIN || f > C_UWORD_MAX)
      barf(C_BAD_ARGUMENT_TYPE_NO_INTEGER_ERROR, "arithmetic-shift", n1);
    else if(f < 0) {
      if(f > C_WORD_MAX)
	barf(C_BAD_ARGUMENT_TYPE_NO_INTEGER_ERROR, "arithmetic-shift", n1);
      else {
	sgn = -1;
	nn = (C_word)f;
      }
    }
    else if(f > C_WORD_MAX) unn = (C_uword)f;
    else {
      nn = (C_word)f;
      sgn = -1;
    }
  }

  if((n2 & C_FIXNUM_BIT) != 0) s = C_unfix(n2);
  else barf(C_BAD_ARGUMENT_TYPE_NO_FIXNUM_ERROR, "arithmetic-shift", n2);

  if(sgn < 0) {
    if(s < 0) nn >>= -s;
    else nn <<= s;

    if(C_fitsinfixnump(nn)) return C_fix(nn);
    else return C_flonum(a, nn);
  } 
  else {
    if(s < 0) unn >>= -s;
    else unn <<= s;
  
    if(C_ufitsinfixnump(unn)) return C_fix(unn);
    else return C_flonum(a, unn);
  }
}

void C_ccall /* x is any exact integer but y is _always_ a fixnum */
C_u_integer_shift(C_word c, C_word self, C_word k, C_word x, C_word y)
{
  C_word ab[C_SIZEOF_FIX_BIGNUM], *a = ab;

  y = C_unfix(y);
  if (y == 0 || x == C_fix(0)) { /* Done (no shift) */
    C_kontinue(k, x);
  } else if (x & C_FIXNUM_BIT) {
    if (y < 0) {
      /* Don't shift more than a word's length (that's undefined in C!) */
      if (-y < C_WORD_SIZE) {
        C_kontinue(k, C_fix(C_unfix(x) >> -y));
      } else {
        C_kontinue(k, (x < 0) ? C_fix(-1) : C_fix(0));
      }
    } else if (y > 0 && y < C_WORD_SIZE-2 &&
               /* After shifting, the length still fits a fixnum */
               (C_ilen(C_unfix(x)) + y) < C_WORD_SIZE-2) {
      C_kontinue(k, C_fix(C_unfix(x) << y));
    } else {
      x = C_a_u_i_fix_to_big(&a, x);
    }
  }

  {
    C_word ab[C_SIZEOF_CLOSURE(6)], *a = ab,
           k2, size, negp, digit_offset, bit_offset;

    negp = C_mk_bool(C_bignum_negativep(x));
  
    if (y > 0) { /* y is guaranteed not to be 0 here */
      digit_offset = y / C_BIGNUM_DIGIT_LENGTH;
      bit_offset =   y % C_BIGNUM_DIGIT_LENGTH;

      k2 = C_closure(&a, 6, (C_word)bignum_actual_shift, k,
                     x, C_SCHEME_TRUE, C_fix(digit_offset), C_fix(bit_offset));
      size = C_fix(C_bignum_size(x) + digit_offset + 1);
      C_allocate_bignum(5, (C_word)NULL, k2, size, negp, C_SCHEME_FALSE);
    } else if (-y >= C_bignum_size(x) * (C_word)C_BIGNUM_DIGIT_LENGTH) {
      /* All bits are shifted out, just return 0 or -1 */
      C_kontinue(k, C_truep(negp) ? C_fix(-1) : C_fix(0));
    } else {
      digit_offset = -y / C_BIGNUM_DIGIT_LENGTH;
      bit_offset =   -y % C_BIGNUM_DIGIT_LENGTH;
    
      k2 = C_closure(&a, 6, (C_word)bignum_actual_shift, k,
                     x, C_SCHEME_FALSE, C_fix(digit_offset), C_fix(bit_offset));

      size = C_fix(C_bignum_size(x) - digit_offset);
      C_allocate_bignum(5, (C_word)NULL, k2, size, negp, C_SCHEME_FALSE);
    }
  }
}

static void bignum_actual_shift(C_word c, C_word self, C_word result)
{
  C_word k = C_block_item(self, 1),
         x = C_block_item(self, 2),
         shift_left = C_truep(C_block_item(self, 3)),
         digit_offset = C_unfix(C_block_item(self, 4)),
         bit_offset = C_unfix(C_block_item(self, 5));
  C_uword *startr = C_bignum_digits(result),
          *startx = C_bignum_digits(x),
          *endx = startx + C_bignum_size(x),
          *endr = startr + C_bignum_size(result);

  if (shift_left) {
    /* Initialize only the lower digits we're skipping and the MSD */
    C_memset(startr, 0, C_wordstobytes(digit_offset));
    *(endr-1) = 0;
    startr += digit_offset;
    /* Can't use bignum_digits_destructive_copy because it assumes
     * we want to copy from the start.
     */
    C_memcpy(startr, startx, C_wordstobytes(endx-startx));
    if(bit_offset > 0)
      bignum_digits_destructive_shift_left(startr, endr, bit_offset);
  } else {
    C_word nx, size = C_bignum_size(x) + 1;
    if (C_truep(nx = maybe_negate_bignum_for_bitwise_op(x, size))) {
      startx = C_bignum_digits(nx); /* update startx; x and endx are unused */
    }

    startx += digit_offset;
    /* Can't use bignum_digits_destructive_copy because that assumes
     * target is at least as big as source.
     */
    C_memcpy(startr, startx, C_wordstobytes(endr-startr));
    if(bit_offset > 0)
      bignum_digits_destructive_shift_right(startr,endr,bit_offset,C_truep(nx));

    if (C_truep(nx)) {
      free_tmp_bignum(nx);
      bignum_digits_destructive_negate(result);
    }
  }
  C_kontinue(k, C_bignum_simplify(result));
}


/* I */
C_regparm C_word C_fcall C_a_i_exp(C_word **a, int c, C_word n)
{
  double f;

  C_check_real(n, "exp", f);
  return C_flonum(a, exp(f));
}


/* I */
C_regparm C_word C_fcall C_a_i_log(C_word **a, int c, C_word n)
{
  double f;

  C_check_real(n, "log", f);
  return C_flonum(a, log(f));
}


/* I */
C_regparm C_word C_fcall C_a_i_sin(C_word **a, int c, C_word n)
{
  double f;

  C_check_real(n, "sin", f);
  return C_flonum(a, sin(f));
}


/* I */
C_regparm C_word C_fcall C_a_i_cos(C_word **a, int c, C_word n)
{
  double f;

  C_check_real(n, "cos", f);
  return C_flonum(a, cos(f));
}


/* I */
C_regparm C_word C_fcall C_a_i_tan(C_word **a, int c, C_word n)
{
  double f;

  C_check_real(n, "tan", f);
  return C_flonum(a, tan(f));
}


/* I */
C_regparm C_word C_fcall C_a_i_asin(C_word **a, int c, C_word n)
{
  double f;

  C_check_real(n, "asin", f);
  return C_flonum(a, asin(f));
}


/* I */
C_regparm C_word C_fcall C_a_i_acos(C_word **a, int c, C_word n)
{
  double f;

  C_check_real(n, "acos", f);
  return C_flonum(a, acos(f));
}


/* I */
C_regparm C_word C_fcall C_a_i_atan(C_word **a, int c, C_word n)
{
  double f;

  C_check_real(n, "atan", f);
  return C_flonum(a, atan(f));
}


/* I */
C_regparm C_word C_fcall C_a_i_atan2(C_word **a, int c, C_word n1, C_word n2)
{
  double f1, f2;

  C_check_real(n1, "atan", f1);
  C_check_real(n2, "atan", f2);
  return C_flonum(a, atan2(f1, f2));
}


/* I */
C_regparm C_word C_fcall C_a_i_sqrt(C_word **a, int c, C_word n)
{
  double f;

  C_check_real(n, "sqrt", f);
  return C_flonum(a, sqrt(f));
}


C_regparm C_word C_fcall C_i_assq(C_word x, C_word lst)
{
  C_word a;

  while(!C_immediatep(lst) && C_block_header(lst) == C_PAIR_TAG) {
    a = C_u_i_car(lst);

    if(!C_immediatep(a) && C_block_header(a) == C_PAIR_TAG) {
      if(C_u_i_car(a) == x) return a;
    }
    else barf(C_BAD_ARGUMENT_TYPE_ERROR, "assq", a);
  
    lst = C_u_i_cdr(lst);
  }

  if(lst!=C_SCHEME_END_OF_LIST)
    barf(C_BAD_ARGUMENT_TYPE_ERROR, "assq", lst);

  return C_SCHEME_FALSE;
}


C_regparm C_word C_fcall C_i_assv(C_word x, C_word lst)
{
  C_word a;

  while(!C_immediatep(lst) && C_block_header(lst) == C_PAIR_TAG) {
    a = C_u_i_car(lst);

    if(!C_immediatep(a) && C_block_header(a) == C_PAIR_TAG) {
      if(C_truep(C_i_eqvp(C_u_i_car(a), x))) return a;
    }
    else barf(C_BAD_ARGUMENT_TYPE_ERROR, "assv", a);
  
    lst = C_u_i_cdr(lst);
  }

  if(lst!=C_SCHEME_END_OF_LIST)
    barf(C_BAD_ARGUMENT_TYPE_ERROR, "assv", lst);

  return C_SCHEME_FALSE;
}


C_regparm C_word C_fcall C_i_assoc(C_word x, C_word lst)
{
  C_word a;

  while(!C_immediatep(lst) && C_block_header(lst) == C_PAIR_TAG) {
    a = C_u_i_car(lst);

    if(!C_immediatep(a) && C_block_header(a) == C_PAIR_TAG) {
      if(C_equalp(C_u_i_car(a), x)) return a;
    }
    else barf(C_BAD_ARGUMENT_TYPE_ERROR, "assoc", a);
  
    lst = C_u_i_cdr(lst);
  }

  if(lst!=C_SCHEME_END_OF_LIST)
    barf(C_BAD_ARGUMENT_TYPE_ERROR, "assoc", lst);

  return C_SCHEME_FALSE;
}


C_regparm C_word C_fcall C_i_memq(C_word x, C_word lst)
{
  while(!C_immediatep(lst) && C_block_header(lst) == C_PAIR_TAG) {
    if(C_u_i_car(lst) == x) return lst;
    else lst = C_u_i_cdr(lst);
  }

  if(lst!=C_SCHEME_END_OF_LIST)
    barf(C_BAD_ARGUMENT_TYPE_ERROR, "memq", lst);

  return C_SCHEME_FALSE;
}


C_regparm C_word C_fcall C_u_i_memq(C_word x, C_word lst)
{
  while(!C_immediatep(lst)) {
    if(C_u_i_car(lst) == x) return lst;
    else lst = C_u_i_cdr(lst);
  }

  return C_SCHEME_FALSE;
}


C_regparm C_word C_fcall C_i_memv(C_word x, C_word lst)
{
  while(!C_immediatep(lst) && C_block_header(lst) == C_PAIR_TAG) {
    if(C_truep(C_i_eqvp(C_u_i_car(lst), x))) return lst;
    else lst = C_u_i_cdr(lst);
  }

  if(lst!=C_SCHEME_END_OF_LIST)
    barf(C_BAD_ARGUMENT_TYPE_ERROR, "memv", lst);
  
  return C_SCHEME_FALSE;
}


C_regparm C_word C_fcall C_i_member(C_word x, C_word lst)
{
  while(!C_immediatep(lst) && C_block_header(lst) == C_PAIR_TAG) {
    if(C_equalp(C_u_i_car(lst), x)) return lst;
    else lst = C_u_i_cdr(lst);
  }

  if(lst!=C_SCHEME_END_OF_LIST)
    barf(C_BAD_ARGUMENT_TYPE_ERROR, "member", lst);
  
  return C_SCHEME_FALSE;
}


/* Inline routines for extended bindings: */

C_regparm C_word C_fcall C_i_check_closure_2(C_word x, C_word loc)
{
  if(C_immediatep(x) || (C_header_bits(x) != C_CLOSURE_TYPE)) {
    error_location = loc;
    barf(C_BAD_ARGUMENT_TYPE_NO_CLOSURE_ERROR, NULL, x);
  }

  return C_SCHEME_UNDEFINED;
}


C_regparm C_word C_fcall C_i_check_exact_2(C_word x, C_word loc)
{
  if(C_u_i_exactp(x) == C_SCHEME_FALSE) {
    error_location = loc;
    barf(C_BAD_ARGUMENT_TYPE_NO_EXACT_ERROR, NULL, x);
  }

  return C_SCHEME_UNDEFINED;
}


C_regparm C_word C_fcall C_i_check_inexact_2(C_word x, C_word loc)
{
  if(C_immediatep(x) || C_block_header(x) != C_FLONUM_TAG) {
    error_location = loc;
    barf(C_BAD_ARGUMENT_TYPE_NO_INEXACT_ERROR, NULL, x);
  }

  return C_SCHEME_UNDEFINED;
}


C_regparm C_word C_fcall C_i_check_char_2(C_word x, C_word loc)
{
  if((x & C_IMMEDIATE_TYPE_BITS) != C_CHARACTER_BITS) {
    error_location = loc;
    barf(C_BAD_ARGUMENT_TYPE_NO_CHAR_ERROR, NULL, x);
  }

  return C_SCHEME_UNDEFINED;
}


C_regparm C_word C_fcall C_i_check_number_2(C_word x, C_word loc)
{
  if (C_i_numberp(x) == C_SCHEME_FALSE) {
    error_location = loc;
    barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, NULL, x);
  }

  return C_SCHEME_UNDEFINED;
}


C_regparm C_word C_fcall C_i_check_string_2(C_word x, C_word loc)
{
  if(C_immediatep(x) || C_header_bits(x) != C_STRING_TYPE) {
    error_location = loc;
    barf(C_BAD_ARGUMENT_TYPE_NO_STRING_ERROR, NULL, x);
  }

  return C_SCHEME_UNDEFINED;
}


C_regparm C_word C_fcall C_i_check_bytevector_2(C_word x, C_word loc)
{
  if(C_immediatep(x) || C_header_bits(x) != C_BYTEVECTOR_TYPE) {
    error_location = loc;
    barf(C_BAD_ARGUMENT_TYPE_NO_BYTEVECTOR_ERROR, NULL, x);
  }

  return C_SCHEME_UNDEFINED;
}


C_regparm C_word C_fcall C_i_check_vector_2(C_word x, C_word loc)
{
  if(C_immediatep(x) || C_header_bits(x) != C_VECTOR_TYPE) {
    error_location = loc;
    barf(C_BAD_ARGUMENT_TYPE_NO_VECTOR_ERROR, NULL, x);
  }

  return C_SCHEME_UNDEFINED;
}


C_regparm C_word C_fcall C_i_check_structure_2(C_word x, C_word st, C_word loc)
{
  if(C_immediatep(x) || C_header_bits(x) != C_STRUCTURE_TYPE || C_block_item(x,0) != st) {
    error_location = loc;
    barf(C_BAD_ARGUMENT_TYPE_BAD_STRUCT_ERROR, NULL, x, st);
  }

  return C_SCHEME_UNDEFINED;
}


C_regparm C_word C_fcall C_i_check_pair_2(C_word x, C_word loc)
{
  if(C_immediatep(x) || C_block_header(x) != C_PAIR_TAG) {
    error_location = loc;
    barf(C_BAD_ARGUMENT_TYPE_NO_PAIR_ERROR, NULL, x);
  }

  return C_SCHEME_UNDEFINED;
}


C_regparm C_word C_fcall C_i_check_boolean_2(C_word x, C_word loc)
{
  if((x & C_IMMEDIATE_TYPE_BITS) != C_BOOLEAN_BITS) {
    error_location = loc;
    barf(C_BAD_ARGUMENT_TYPE_NO_BOOLEAN_ERROR, NULL, x);
  }

  return C_SCHEME_UNDEFINED;
}


C_regparm C_word C_fcall C_i_check_locative_2(C_word x, C_word loc)
{
  if(C_immediatep(x) || C_block_header(x) != C_LOCATIVE_TAG) {
    error_location = loc;
    barf(C_BAD_ARGUMENT_TYPE_NO_LOCATIVE_ERROR, NULL, x);
  }

  return C_SCHEME_UNDEFINED;
}


C_regparm C_word C_fcall C_i_check_symbol_2(C_word x, C_word loc)
{
  if(C_immediatep(x) || C_block_header(x) != C_SYMBOL_TAG) {
    error_location = loc;
    barf(C_BAD_ARGUMENT_TYPE_NO_SYMBOL_ERROR, NULL, x);
  }

  return C_SCHEME_UNDEFINED;
}


C_regparm C_word C_fcall C_i_check_list_2(C_word x, C_word loc)
{
  if(x != C_SCHEME_END_OF_LIST && (C_immediatep(x) || C_block_header(x) != C_PAIR_TAG)) {
    error_location = loc;
    barf(C_BAD_ARGUMENT_TYPE_NO_LIST_ERROR, NULL, x);
  }

  return C_SCHEME_UNDEFINED;
}


C_regparm C_word C_fcall C_i_check_port_2(C_word x, C_word input, C_word open, C_word loc)
{
  int inp;

  if(C_immediatep(x) || C_header_bits(x) != C_PORT_TYPE) {
    error_location = loc;
    barf(C_BAD_ARGUMENT_TYPE_NO_PORT_ERROR, NULL, x);
  }

  inp = C_block_item(x, 1) == C_SCHEME_TRUE;	/* slot #1: I/O flag */

  switch(input) {
  case C_SCHEME_TRUE:
    if(!inp) {
      error_location = loc;
      barf(C_BAD_ARGUMENT_TYPE_NO_INPUT_PORT_ERROR, NULL, x);
    }

    break;

  case C_SCHEME_FALSE:
    if(inp) {
      error_location = loc;
      barf(C_BAD_ARGUMENT_TYPE_NO_OUTPUT_PORT_ERROR, NULL, x);
    }

    break;

    /* any other value: omit direction check */
  }

  if(open == C_SCHEME_TRUE) {
    if(C_block_item(x, 8) != C_SCHEME_FALSE) {	/* slot #8: closed flag */
      error_location = loc;
      barf(C_PORT_CLOSED_ERROR, NULL, x);
    }
  }

  return C_SCHEME_UNDEFINED;
}


/*XXX these are not correctly named */
C_regparm C_word C_fcall C_i_foreign_char_argumentp(C_word x)
{
  if((x & C_IMMEDIATE_TYPE_BITS) != C_CHARACTER_BITS)
    barf(C_BAD_ARGUMENT_TYPE_NO_CHAR_ERROR, NULL, x);

  return x;
}


C_regparm C_word C_fcall C_i_foreign_fixnum_argumentp(C_word x)
{
  if((x & C_FIXNUM_BIT) == 0)
    barf(C_BAD_ARGUMENT_TYPE_NO_FIXNUM_ERROR, NULL, x);

  return x;
}


C_regparm C_word C_fcall C_i_foreign_flonum_argumentp(C_word x)
{
  if((x & C_FIXNUM_BIT) != 0) return x;

  if(C_immediatep(x) || C_block_header(x) != C_FLONUM_TAG)
    barf(C_BAD_ARGUMENT_TYPE_NO_FLONUM_ERROR, NULL, x);

  return x;
}


C_regparm C_word C_fcall C_i_foreign_block_argumentp(C_word x)
{
  if(C_immediatep(x))
    barf(C_BAD_ARGUMENT_TYPE_NO_BLOCK_ERROR, NULL, x);

  return x;
}


C_regparm C_word C_fcall C_i_foreign_struct_wrapper_argumentp(C_word t, C_word x)
{
  if(C_immediatep(x) || C_header_bits(x) != C_STRUCTURE_TYPE || C_block_item(x, 0) != t)
    barf(C_BAD_ARGUMENT_TYPE_BAD_STRUCT_ERROR, NULL, t, x);

  return x;
}


C_regparm C_word C_fcall C_i_foreign_string_argumentp(C_word x)
{
  if(C_immediatep(x) || C_header_bits(x) != C_STRING_TYPE)
    barf(C_BAD_ARGUMENT_TYPE_NO_STRING_ERROR, NULL, x);

  return x;
}


C_regparm C_word C_fcall C_i_foreign_symbol_argumentp(C_word x)
{
  if(C_immediatep(x) || C_header_bits(x) != C_SYMBOL_TYPE)
    barf(C_BAD_ARGUMENT_TYPE_NO_SYMBOL_ERROR, NULL, x);

  return x;
}


C_regparm C_word C_fcall C_i_foreign_pointer_argumentp(C_word x)
{
  if(C_immediatep(x) || 
     (C_header_bits(x) != C_SWIG_POINTER_TYPE &&
      (C_header_bits(x) & C_SPECIALBLOCK_BIT) == 0) )
    barf(C_BAD_ARGUMENT_TYPE_NO_POINTER_ERROR, NULL, x);

  return x;
}


C_regparm C_word C_fcall C_i_foreign_scheme_or_c_pointer_argumentp(C_word x)
{
  if(C_immediatep(x) || 
     (C_header_bits(x) != C_SWIG_POINTER_TYPE &&
      (C_header_bits(x) & C_SPECIALBLOCK_BIT) == 0) )
    barf(C_BAD_ARGUMENT_TYPE_NO_POINTER_ERROR, NULL, x);

  return x;
}


C_regparm C_word C_fcall C_i_foreign_tagged_pointer_argumentp(C_word x, C_word t)
{
  if(C_immediatep(x) || (C_header_bits(x) & C_SPECIALBLOCK_BIT) == 0
     || (t != C_SCHEME_FALSE && !C_equalp(C_block_item(x, 1), t)))
    barf(C_BAD_ARGUMENT_TYPE_NO_TAGGED_POINTER_ERROR, NULL, x, t);

  return x;
}


C_regparm C_word C_fcall C_i_foreign_integer_argumentp(C_word x)
{
  double m;

  if((x & C_FIXNUM_BIT) != 0) return x;

  if(!C_immediatep(x) && C_block_header(x) == C_FLONUM_TAG) {
    m = C_flonum_magnitude(x);

    if(m >= C_WORD_MIN && m <= C_WORD_MAX) return x;
  }

  barf(C_BAD_ARGUMENT_TYPE_NO_INTEGER_ERROR, NULL, x);
  return C_SCHEME_UNDEFINED;
}


C_regparm C_word C_fcall C_i_foreign_integer64_argumentp(C_word x)
{
  double m, r;

  if((x & C_FIXNUM_BIT) != 0) return x;
  
  if(!C_immediatep(x) && C_block_header(x) == C_FLONUM_TAG) {
    m = C_flonum_magnitude(x);

    if(m >= C_S64_MIN && m <= C_S64_MAX && C_modf(m, &r) == 0.0) return x;
  }

  barf(C_BAD_ARGUMENT_TYPE_NO_INTEGER_ERROR, NULL, x);
  return C_SCHEME_UNDEFINED;
}


C_regparm C_word C_fcall C_i_foreign_unsigned_integer_argumentp(C_word x)
{
  double m ,r;

  if((x & C_FIXNUM_BIT) != 0) return x;

  if(!C_immediatep(x) && C_block_header(x) == C_FLONUM_TAG) {
    m = C_flonum_magnitude(x);

    if(m >= 0 && m <= C_UWORD_MAX && C_modf(m, &r) == 0.0) return x;
  }

  barf(C_BAD_ARGUMENT_TYPE_NO_UINTEGER_ERROR, NULL, x);
  return C_SCHEME_UNDEFINED;
}


C_regparm C_word C_fcall C_i_foreign_unsigned_integer64_argumentp(C_word x)
{
  double m, r;

  if((x & C_FIXNUM_BIT) != 0) return x;

  if(!C_immediatep(x) && C_block_header(x) == C_FLONUM_TAG) {
    m = C_flonum_magnitude(x);

    if(m >= 0 && m <= C_U64_MAX && C_modf(m, &r) == 0.0) return x;
  }

  barf(C_BAD_ARGUMENT_TYPE_NO_UINTEGER_ERROR, NULL, x);
  return C_SCHEME_UNDEFINED;
}


/* I */
C_regparm C_word C_fcall C_i_not_pair_p_2(C_word x)
{
  return C_mk_bool(C_immediatep(x) || C_block_header(x) != C_PAIR_TAG);
}


C_regparm C_word C_fcall C_i_null_list_p(C_word x)
{
  if(x == C_SCHEME_END_OF_LIST) return C_SCHEME_TRUE;
  else if(!C_immediatep(x) && C_block_header(x) == C_PAIR_TAG) return C_SCHEME_FALSE;
  else {
    barf(C_BAD_ARGUMENT_TYPE_NO_LIST_ERROR, "null-list?", x);
    return C_SCHEME_FALSE;
  }
}


C_regparm C_word C_fcall C_i_string_null_p(C_word x)
{
  if(!C_immediatep(x) && C_header_bits(x) == C_STRING_TYPE)
    return C_zero_length_p(x);
  else {
    barf(C_BAD_ARGUMENT_TYPE_NO_STRING_ERROR, "string-null?", x);
    return C_SCHEME_FALSE;
  }
}


C_regparm C_word C_fcall C_i_null_pointerp(C_word x)
{
  if(!C_immediatep(x) && (C_header_bits(x) & C_SPECIALBLOCK_BIT) != 0)
    return C_null_pointerp(x);

  barf(C_BAD_ARGUMENT_TYPE_ERROR, "null-pointer?", x);
  return C_SCHEME_FALSE;
}


/* Primitives: */

void C_ccall C_apply(C_word c, C_word closure, C_word k, C_word fn, ...)
{
  va_list v;
  int i, n = c - 3;
  C_word x, skip;
#ifdef C_HACKED_APPLY
  C_word *buf = C_temporary_stack_limit;
  void *proc;
#endif

  if(c < 4) C_bad_min_argc(c, 4);

  if(C_immediatep(fn) || C_header_bits(fn) != C_CLOSURE_TYPE) {
    barf(C_NOT_A_CLOSURE_ERROR, "apply", fn);
  }

  va_start(v, fn);

  for(i = n; i > 1; --i) {
    x = va_arg(v, C_word);
#ifdef C_HACKED_APPLY
    *(buf++) = x;
#else
    C_save(x);
#endif
  }

  x = va_arg(v, C_word);

  if(x != C_SCHEME_END_OF_LIST && (C_immediatep(x) || C_block_header(x) != C_PAIR_TAG))
    barf(C_BAD_ARGUMENT_TYPE_ERROR, "apply", x);

  for(skip = x; !C_immediatep(skip) && C_block_header(skip) == C_PAIR_TAG; skip = C_u_i_cdr(skip)) {
    x = C_u_i_car(skip);

#ifdef C_HACKED_APPLY
    if(buf >= C_temporary_stack_bottom) barf(C_TOO_MANY_PARAMETERS_ERROR, "apply");

    *(buf++) = x;
#else
    C_save(x);

    if(C_temporary_stack < C_temporary_stack_limit)
      barf(C_TOO_MANY_PARAMETERS_ERROR, "apply");
#endif
    ++n;
  }

  va_end(v);
  --n;

#ifdef C_HACKED_APPLY
  /* 3 additional args + 1 slot for stack-pointer + two for stack-alignment to 16 bytes */
  buf = alloca((n + 6) * sizeof(C_word));
# ifdef __x86_64__
  /* XXX Shouldn't this check for C_SIXTY_FOUR in general? */
  buf = (void *)C_align16((C_uword)buf);
# endif
  buf[ 0 ] = n + 2;
  buf[ 1 ] = fn;
  buf[ 2 ] = k;
  C_memcpy(&buf[ 3 ], C_temporary_stack_limit, n * sizeof(C_word));
  proc = (void *)C_block_item(fn, 0);
  C_do_apply_hack(proc, buf, n + 3);
#else
  C_do_apply(n, fn, k);
#endif
}


void C_ccall C_do_apply(C_word n, C_word fn, C_word k)
{
  void *pr = (void *)C_block_item(fn, 0);
  C_word *ptr = C_temporary_stack = C_temporary_stack_bottom;

/* PTR_O_p<P>_<B>(o): list of COUNT = ((2 ** P) * B) '*(ptr-I)' arguments,
 * with offset I in range [o, o+COUNT-1].
 */
#define PTR_O_p0_0(o)
#define PTR_O_p1_0(o)
#define PTR_O_p2_0(o)
#define PTR_O_p3_0(o)
#define PTR_O_p4_0(o)
#define PTR_O_p5_0(o)
#define PTR_O_p6_0(o)
#define PTR_O_p7_0(o)
#define PTR_O_p0_1(o)   , *(ptr-(o))
#define PTR_O_p1_1(o)   , *(ptr-(o)), *(ptr-(o+1))
#define PTR_O_p2_1(o)   PTR_O_p1_1(o) PTR_O_p1_1(o+2)
#define PTR_O_p3_1(o)   PTR_O_p2_1(o) PTR_O_p2_1(o+4)
#define PTR_O_p4_1(o)   PTR_O_p3_1(o) PTR_O_p3_1(o+8)
#define PTR_O_p5_1(o)   PTR_O_p4_1(o) PTR_O_p4_1(o+16)
#define PTR_O_p6_1(o)   PTR_O_p5_1(o) PTR_O_p5_1(o+32)
#define PTR_O_p7_1(o)   PTR_O_p6_1(o) PTR_O_p6_1(o+64)

/* CASE_C_PROC_p0 (n0,  p6,p5,p4,p3,p2,p1,p0):
 *  let's note <N> = <n0> - 2; the macro inserts:
 *      case <N>: ((C_cproc<n0>)pr) (<n0>, fn, k, <rest>);
 *  where <rest> is:    *(ptr-1), ..., *(ptr-<N>)
 *  (<rest> is empty for <n0> == 2).
 *  We must have:   n0 = SUM (i = 7 to 0, p<i> * (1 << i)).
 * CASE_C_PROC_p<N+1> (...):
 *  like CASE_C_PROC_p<N>, but with doubled output...
 */
#define CASE_C_PROC_p0(n0,  p6,p5,p4,p3,p2,p1,p0) \
    case (n0-2): ((C_proc##n0)pr)(n0, fn, k \
PTR_O_p6_##p6(((n0-2)&0x80)+1)\
PTR_O_p5_##p5(((n0-2)&0xC0)+1)\
PTR_O_p4_##p4(((n0-2)&0xE0)+1)\
PTR_O_p3_##p3(((n0-2)&0xF0)+1)\
PTR_O_p2_##p2(((n0-2)&0xF8)+1)\
PTR_O_p1_##p1(((n0-2)&0xFC)+1)\
PTR_O_p0_##p0(((n0-2)&0xFE)+1));
#define CASE_C_PROC_p1( n0,n1,  p6,p5,p4,p3,p2,p1) \
        CASE_C_PROC_p0 (n0,  p6,p5,p4,p3,p2,p1,0) \
        CASE_C_PROC_p0 (n1,  p6,p5,p4,p3,p2,p1,1)
#define CASE_C_PROC_p2( n0,n1,n2,n3,  p6,p5,p4,p3,p2) \
        CASE_C_PROC_p1 (n0,n1,  p6,p5,p4,p3,p2,0) \
        CASE_C_PROC_p1 (n2,n3,  p6,p5,p4,p3,p2,1)
#define CASE_C_PROC_p3( n0,n1,n2,n3,n4,n5,n6,n7,  p6,p5,p4,p3) \
        CASE_C_PROC_p2 (n0,n1,n2,n3,  p6,p5,p4,p3,0) \
        CASE_C_PROC_p2 (n4,n5,n6,n7,  p6,p5,p4,p3,1)

  switch(n) {
    CASE_C_PROC_p3 (2,3,4,5,6,7,8,9,  0,0,0,0)
    CASE_C_PROC_p3 (10,11,12,13,14,15,16,17,  0,0,0,1)
    CASE_C_PROC_p3 (18,19,20,21,22,23,24,25,  0,0,1,0)
    CASE_C_PROC_p3 (26,27,28,29,30,31,32,33,  0,0,1,1)
    CASE_C_PROC_p3 (34,35,36,37,38,39,40,41,  0,1,0,0)
    CASE_C_PROC_p3 (42,43,44,45,46,47,48,49,  0,1,0,1)
    CASE_C_PROC_p3 (50,51,52,53,54,55,56,57,  0,1,1,0)
    CASE_C_PROC_p3 (58,59,60,61,62,63,64,65,  0,1,1,1)
    CASE_C_PROC_p0 (66,  1,0,0,0,0,0,0)
    CASE_C_PROC_p0 (67,  1,0,0,0,0,0,1)
    CASE_C_PROC_p1 (68,69,  1,0,0,0,0,1)
    CASE_C_PROC_p2 (70,71,72,73,  1,0,0,0,1)
    CASE_C_PROC_p3 (74,75,76,77,78,79,80,81,  1,0,0,1)
    CASE_C_PROC_p3 (82,83,84,85,86,87,88,89,  1,0,1,0)
    CASE_C_PROC_p3 (90,91,92,93,94,95,96,97,  1,0,1,1)
    CASE_C_PROC_p3 (98,99,100,101,102,103,104,105,  1,1,0,0)
    CASE_C_PROC_p3 (106,107,108,109,110,111,112,113,  1,1,0,1)
    CASE_C_PROC_p3 (114,115,116,117,118,119,120,121,  1,1,1,0)
    CASE_C_PROC_p2 (122,123,124,125,  1,1,1,1,0)
    CASE_C_PROC_p1 (126,127,  1,1,1,1,1,0)
    CASE_C_PROC_p0 (128,  1,1,1,1,1,1,0)
  default: barf(C_TOO_MANY_PARAMETERS_ERROR, "apply");
  }
}


void C_ccall C_call_cc(C_word c, C_word closure, C_word k, C_word cont)
{
  C_word *a = C_alloc(C_SIZEOF_CLOSURE(2)),
         wrapper;
  void *pr = (void *)C_block_item(cont,0);

  if(C_immediatep(cont) || C_header_bits(cont) != C_CLOSURE_TYPE)
    barf(C_BAD_ARGUMENT_TYPE_ERROR, "call-with-current-continuation", cont);

  /* Check for values-continuation: */
  if(C_block_item(k,0) == (C_word)values_continuation)
    wrapper = C_closure(&a, 2, (C_word)call_cc_values_wrapper, k);
  else wrapper = C_closure(&a, 2, (C_word)call_cc_wrapper, k);

  ((C_proc3)pr)(3, cont, k, wrapper);
}


void C_ccall call_cc_wrapper(C_word c, C_word closure, C_word k, C_word result)
{
  C_word cont = C_block_item(closure,1);

  if(c != 3) C_bad_argc(c, 3);

  C_kontinue(cont, result);
}


void C_ccall call_cc_values_wrapper(C_word c, C_word closure, C_word k, ...)
{
  va_list v;
  C_word cont = C_block_item(closure,1),
         x1;
  int n = c;

  va_start(v, k);

  if(c > 2) {
    x1 = va_arg(v, C_word);
    --n;
    
    while(--c > 2) C_save(va_arg(v, C_word));
  }
  else x1 = C_SCHEME_UNBOUND;

  va_end(v);
  C_do_apply(n - 2, cont, x1);
}


/* I */
void C_ccall C_continuation_graft(C_word c, C_word self, C_word k, C_word kk, C_word proc)
{
  ((C_proc2)C_fast_retrieve_proc(proc))(2, proc, C_block_item(kk, 1));
}


void C_ccall C_values(C_word c, C_word closure, C_word k, ...)
{
  va_list v;
  C_word n = c;

  if(c < 2) C_bad_min_argc(c, 2);

  va_start(v, k);

  /* Check continuation whether it receives multiple values: */
  if(C_block_item(k, 0) == (C_word)values_continuation) {
    while(c-- > 2)
      C_save(va_arg(v, C_word));

    va_end(v);
    C_do_apply(n - 2, k, C_SCHEME_UNBOUND); /* unbound value marks direct invocation */
  }
  
  if(c != 3) {
#ifdef RELAX_MULTIVAL_CHECK
    if(c == 2) n = C_SCHEME_UNDEFINED;
    else n = va_arg(v, C_word);
#else
    barf(C_CONTINUATION_CANT_RECEIVE_VALUES_ERROR, "values", k);
#endif
  }
  else n = va_arg(v, C_word);

  va_end(v);
  C_kontinue(k, n);
}


void C_ccall C_apply_values(C_word c, C_word closure, C_word k, C_word lst)
{
  C_word n;

  if(c != 3) C_bad_argc(c, 3);

  /* Check continuation wether it receives multiple values: */
  if(C_block_item(k, 0) == (C_word)values_continuation) {
    for(n = 0; !C_immediatep(lst) && C_block_header(lst) == C_PAIR_TAG; ++n) {
      C_save(C_u_i_car(lst));
      lst = C_u_i_cdr(lst);
    }

    C_do_apply(n, k, C_SCHEME_UNBOUND); /* unbound value marks direct invocation */
  }
  
  if(C_immediatep(lst) || (C_block_header(lst) == C_PAIR_TAG && C_u_i_cdr(lst) == C_SCHEME_END_OF_LIST)) {
#ifdef RELAX_MULTIVAL_CHECK
    if(C_immediatep(lst)) n = C_SCHEME_UNDEFINED;
    else n = C_u_i_car(lst);
#else
    barf(C_CONTINUATION_CANT_RECEIVE_VALUES_ERROR, "values", k);
#endif
  }
  else n = C_u_i_car(lst);

  C_kontinue(k, n);
}


void C_ccall C_call_with_values(C_word c, C_word closure, C_word k, C_word thunk, C_word kont)
{
  C_word *a = C_alloc(C_SIZEOF_CLOSURE(3)),
         kk;

  if(c != 4) C_bad_argc(c, 4);

  if(C_immediatep(thunk) || C_header_bits(thunk) != C_CLOSURE_TYPE)
    barf(C_BAD_ARGUMENT_TYPE_ERROR, "call-with-values", thunk);

  if(C_immediatep(kont) || C_header_bits(kont) != C_CLOSURE_TYPE)
    barf(C_BAD_ARGUMENT_TYPE_ERROR, "call-with-values", kont);

  kk = C_closure(&a, 3, (C_word)values_continuation, kont, k);
  C_do_apply(0, thunk, kk);
}


void C_ccall C_u_call_with_values(C_word c, C_word closure, C_word k, C_word thunk, C_word kont)
{
  C_word *a = C_alloc(C_SIZEOF_CLOSURE(3)),
         kk;

  kk = C_closure(&a, 3, (C_word)values_continuation, kont, k);
  C_do_apply(0, thunk, kk);
}


void C_ccall values_continuation(C_word c, C_word closure, C_word arg0, ...)
{
  C_word kont = C_block_item(closure, 1),
         k = C_block_item(closure, 2),
         n = c,
         *ptr;
  va_list v;

  if(arg0 == C_SCHEME_UNBOUND) { /* This continuation was called by 'values'... */
    va_start(v, arg0);

    for(; c-- > 2; C_save(va_arg(v, C_word)));

    va_end(v);
  }
  else {			/* This continuation was captured and called explicity... */
    ++n;
    c -= 1;

    /* move temporary-stack contents upwards one slot: */
    for(ptr = C_temporary_stack - c; --c; ++ptr) *ptr = ptr[ 1 ];

    C_save(arg0);
  }

  C_do_apply(n - 2, kont, k);
}


void C_ccall
C_2_basic_times(C_word c, C_word self, C_word k, C_word x, C_word y)
{
  if (x & C_FIXNUM_BIT) {
    if (y & C_FIXNUM_BIT) {
      C_word *a = C_alloc(C_SIZEOF_BIGNUM(2));
      C_kontinue(k, C_a_i_fixnum_times(&a, 2, x, y));
    } else if (C_immediatep(y)) {
      barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, "*", y);
    } else if (C_block_header(y) == C_FLONUM_TAG) {
      C_word *a = C_alloc(C_SIZEOF_FLONUM);
      C_kontinue(k, C_flonum(&a, (double)C_unfix(x) * C_flonum_magnitude(y)));
    } else if (C_header_bits(y) == C_BIGNUM_TYPE) {
      C_u_2_integer_times(4, (C_word)NULL, k, x, y);
    } else {
      try_extended_number("\003sysextended-times", 3, k, x, y);
    }
  } else if (C_immediatep(x)) {
    barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, "*", x);
  } else if (C_block_header(x) == C_FLONUM_TAG) {
    C_word *a = C_alloc(C_SIZEOF_FLONUM);
    if (y & C_FIXNUM_BIT) {
      C_kontinue(k, C_flonum(&a, C_flonum_magnitude(x) * (double)C_unfix(y)));
    } else if (C_immediatep(y)) {
      barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, "*", y);
    } else if (C_block_header(y) == C_FLONUM_TAG) {
      C_kontinue(k, C_a_i_flonum_times(&a, 2, x, y));
    } else if (C_header_bits(y) == C_BIGNUM_TYPE) {
      C_kontinue(k, C_flonum(&a, C_flonum_magnitude(x)*C_bignum_to_double(y)));
    } else {
      try_extended_number("\003sysextended-times", 3, k, x, y);
    }
  } else if (C_header_bits(x) == C_BIGNUM_TYPE) {
    if (y & C_FIXNUM_BIT) {
      C_u_2_integer_times(4, (C_word)NULL, k, x, y);
    } else if (C_immediatep(y)) {
      barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, "*", x);
    } else if (C_block_header(y) == C_FLONUM_TAG) {
      C_word *a = C_alloc(C_SIZEOF_FLONUM);
      C_kontinue(k, C_flonum(&a, C_bignum_to_double(x)*C_flonum_magnitude(y)));
    } else if (C_header_bits(y) == C_BIGNUM_TYPE) {
      C_u_2_integer_times(4, (C_word)NULL, k, x, y);
    } else {
      try_extended_number("\003sysextended-times", 3, k, x, y);
    }
  } else {
    try_extended_number("\003sysextended-times", 3, k, x, y);
  }
}

void C_ccall
C_u_2_integer_times(C_word c, C_word self, C_word k, C_word x, C_word y)
{
  if (x & C_FIXNUM_BIT) {
    if (y & C_FIXNUM_BIT) {
      C_word *a = C_alloc(C_SIZEOF_BIGNUM(2));
      C_kontinue(k, C_a_i_fixnum_times(&a, 2, x, y));
    } else {
      C_word tmp = x; /* swap to ensure x is a bignum and y a fixnum */
      x = y;
      y = tmp;
    }
  }
  /* Here, we know for sure that X is a bignum */
  if (y == C_fix(0)) {
    C_kontinue(k, C_fix(0));
  } else if (y == C_fix(1)) {
    C_kontinue(k, x);
  } else if (y == C_fix(-1)) {
    C_u_integer_negate(3, (C_word)NULL, k, x);
  } else if (y & C_FIXNUM_BIT) { /* Any other fixnum */
    C_word absy = (y & C_INT_SIGN_BIT) ? -C_unfix(y) : C_unfix(y),
           negp = C_mk_bool((y & C_INT_SIGN_BIT) ?
                            !C_bignum_negativep(x) :
                            C_bignum_negativep(x));
  
    if (C_fitsinbignumhalfdigitp(absy) ||
        (((C_uword)1 << (C_ilen(absy)-1)) == absy && C_fitsinfixnump(absy))) {
      C_word size, k2, *a = C_alloc(C_SIZEOF_CLOSURE(4));
      k2 = C_closure(&a, 4, (C_word)integer_times_2, k, x, C_fix(absy));
      size = C_fix(C_bignum_size(x) + 1); /* Needs _at most_ one more digit */
      C_allocate_bignum(5, (C_word)NULL, k2, size, negp, C_SCHEME_FALSE);
    } else {
      C_word *a = C_alloc(C_SIZEOF_FIX_BIGNUM);
      y = C_a_u_i_fix_to_big(&a, y);
      bignum_times_bignum_unsigned(k, x, y, negp);
    }
  } else {
    C_word negp = C_bignum_negativep(x) ?
                  !C_bignum_negativep(y) :
                  C_bignum_negativep(y);
    bignum_times_bignum_unsigned(k, x, y, C_mk_bool(negp));
  }
}

static void integer_times_2(C_word c, C_word self, C_word new_big)
{
  C_word k = C_block_item(self, 1),
	 old_bigx = C_block_item(self, 2),
	 absy = C_unfix(C_block_item(self, 3));
  C_uword *digits = C_bignum_digits(new_big),
	  *end_digit = digits + C_bignum_size(old_bigx);
  int shift;

  bignum_digits_destructive_copy(new_big, old_bigx);

  /* Scale up, and sanitise the result. */
  shift = C_ilen(absy) - 1;
  if (((C_uword)1 << shift) == absy) { /* Power of two? */
    *end_digit = bignum_digits_destructive_shift_left(digits, end_digit, shift);
  } else {
    *end_digit =
      bignum_digits_destructive_scale_up_with_carry(digits, end_digit, absy, 0);
  }
  C_kontinue(k, C_bignum_simplify(new_big));
}

static void
bignum_times_bignum_unsigned(C_word k, C_word x, C_word y, C_word negp)
{
  if (C_bignum_size(y) < C_bignum_size(x)) { /* Ensure size(x) <= size(y) */
    C_word z = x;
    x = y;
    y = z;
  }

  /* TODO: Activate later */
  /* if (C_bignum_size(x) < C_KARATSUBA_THRESHOLD) {  /\* Gradebook *\/ */
    C_word kab[C_SIZEOF_CLOSURE(4)], *ka = kab, k2, size;
    k2 = C_closure(&ka, 4, (C_word)bignum_times_bignum_unsigned_2, k, x, y);
    size = C_fix(C_bignum_size(x) + C_bignum_size(y));
    C_allocate_bignum(5, (C_word)NULL, k2, size, negp, C_SCHEME_TRUE);
  /* } else { */
  /*   try_extended_number("numbers#@bignum-2-times-karatsuba", 3, k, x, y); */
  /* } */
}

static void bignum_times_bignum_unsigned_2(C_word c, C_word self, C_word result)
{
  bignum_digits_multiply(C_block_item(self, 2), C_block_item(self, 3), result);
  C_kontinue(C_block_item(self, 1), C_bignum_simplify(result));
}


/* XXX TODO OBSOLETE: This can be removed after recompiling c-platform.scm */
void C_ccall C_times(C_word c, C_word closure, C_word k, ...)
{
  va_list v;
  C_word x, y;
  C_word iresult = C_fix(1);
  double fresult;
  C_alloc_flonum;

  va_start(v, k);
  c -= 2;

  while(c--) {
    x = va_arg(v, C_word);
    
    if(x & C_FIXNUM_BIT) {
      y = C_i_o_fixnum_times(iresult, x);

      if(y == C_SCHEME_FALSE) {
	fresult = (double)C_unfix(iresult) * (double)C_unfix(x);
	goto flonum_result;
      }
      else iresult = y;
    }
    else if(!C_immediatep(x) && C_block_header(x) == C_FLONUM_TAG) {
      fresult = (double)C_unfix(iresult) * C_flonum_magnitude(x);
      goto flonum_result;
    }
    else barf(C_BAD_ARGUMENT_TYPE_ERROR, "*", x);
  }

  va_end(v);
  C_kontinue(k, iresult);

 flonum_result:
  while(c--) {
    x = va_arg(v, C_word);

    if(x & C_FIXNUM_BIT)
      fresult *= (double)C_unfix(x);
    else if(!C_immediatep(x) && C_block_header(x) == C_FLONUM_TAG)
      fresult *= C_flonum_magnitude(x);
    else barf(C_BAD_ARGUMENT_TYPE_ERROR, "*", x);
  }

  va_end(v);
  C_kontinue_flonum(k, fresult);
}


/* XXX TODO OBSOLETE: This can be removed after recompiling c-platform.scm */
C_regparm C_word C_fcall C_2_times(C_word **ptr, C_word x, C_word y)
{
  C_word iresult;

  if(x & C_FIXNUM_BIT) {
    if(y & C_FIXNUM_BIT) {
      iresult = C_i_o_fixnum_times(x, y);

      if(iresult == C_SCHEME_FALSE)
	return C_flonum(ptr, (double)C_unfix(x) * (double)C_unfix(y));
      else return iresult;
    }
    else if(!C_immediatep(y) && C_block_header(y) == C_FLONUM_TAG)
      return C_flonum(ptr, (double)C_unfix(x) * C_flonum_magnitude(y));
    else barf(C_BAD_ARGUMENT_TYPE_ERROR, "*", y);
  }
  else if(!C_immediatep(x) && C_block_header(x) == C_FLONUM_TAG) {
    if(y & C_FIXNUM_BIT) 
      return C_flonum(ptr, C_flonum_magnitude(x) * (double)C_unfix(y));
    else if(!C_immediatep(y) && C_block_header(y) == C_FLONUM_TAG)
      return C_flonum(ptr, C_flonum_magnitude(x) * C_flonum_magnitude(y));
    else barf(C_BAD_ARGUMENT_TYPE_ERROR, "*", y);
  }
  else barf(C_BAD_ARGUMENT_TYPE_ERROR, "*", x);
  /* shutup compiler */
  return C_flonum(ptr, 0.0/0.0);
}

static void bignum_plus_unsigned(C_word k, C_word x, C_word y, C_word negp)
{
  C_word kab[C_SIZEOF_CLOSURE(4)], *ka = kab, k2, size;

  if (C_bignum_size(y) > C_bignum_size(x)) {  /* Ensure size(y) <= size(x) */
    C_word z = x;
    x = y;
    y = z;
  }

  k2 = C_closure(&ka, 4, (C_word)bignum_plus_unsigned_2, k, x, y);
  
  size = C_fix(C_bignum_size(x) + 1); /* One more digit, for possible carry. */
  C_allocate_bignum(5, (C_word)NULL, k2, size, negp, C_SCHEME_FALSE);
}

static void bignum_plus_unsigned_2(C_word c, C_word self, C_word result)
{
  C_word k = C_block_item(self, 1),
         x = C_block_item(self, 2),
         y = C_block_item(self, 3);
  C_uword *scan_y = C_bignum_digits(y),
          *end_y = scan_y + C_bignum_size(y),
          *scan_r = C_bignum_digits(result),
          *end_r = scan_r + C_bignum_size(result),
          sum, digit;
  int carry = 0;

  /* Copy x into r so we can operate on two pointers, which is faster
   * than three, and we can stop earlier after adding y.  It's slower
   * if x and y have equal length.  On average it's slightly faster.
   */
  bignum_digits_destructive_copy(result, x);
  *(end_r-1) = 0; /* Ensure most significant digit is initialised */

  /* Move over x and y simultaneously, destructively adding digits w/ carry. */
  while (scan_y < end_y) {
    digit = *scan_r;
    if (carry) {
      sum = digit + *scan_y++ + 1;
      carry = sum <= digit;
    } else {
      sum = digit + *scan_y++;
      carry = sum < digit;
    }
    (*scan_r++) = sum;
  }
  
  /* The end of y, the smaller number.  Propagate carry into the rest of x. */
  while (carry) {
    sum = (*scan_r) + 1;
    carry = (sum == 0);
    (*scan_r++) = sum;
  }
  assert(scan_r <= end_r);

  C_kontinue(k, C_bignum_simplify(result));
}

void C_ccall
C_2_basic_plus(C_word c, C_word self, C_word k, C_word x, C_word y)
{
  if (x & C_FIXNUM_BIT) {
    if (y & C_FIXNUM_BIT) {
      C_word *a = C_alloc(C_SIZEOF_FIX_BIGNUM);
      C_kontinue(k, C_a_i_fixnum_plus(&a, 2, x, y));
    } else if (C_immediatep(y)) {
      barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, "+", y);
    } else if (C_block_header(y) == C_FLONUM_TAG) {
      C_word *a = C_alloc(C_SIZEOF_FLONUM);
      C_kontinue(k, C_flonum(&a, (double)C_unfix(x) + C_flonum_magnitude(y)));
    } else if (C_header_bits(y) == C_BIGNUM_TYPE) {
      C_u_2_integer_plus(4, (C_word)NULL, k, x, y);
    } else {
      try_extended_number("\003sysextended-plus", 3, k, x, y);
    }
  } else if (C_immediatep(x)) {
    barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, "+", x);
  } else if (C_block_header(x) == C_FLONUM_TAG) {
    C_word *a = C_alloc(C_SIZEOF_FLONUM);
    if (y & C_FIXNUM_BIT) {
      C_kontinue(k, C_flonum(&a, C_flonum_magnitude(x) + (double)C_unfix(y)));
    } else if (C_immediatep(y)) {
      barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, "+", y);
    } else if (C_block_header(y) == C_FLONUM_TAG) {
      C_kontinue(k, C_a_i_flonum_plus(&a, 2, x, y));
    } else if (C_header_bits(y) == C_BIGNUM_TYPE) {
      C_kontinue(k, C_flonum(&a, C_flonum_magnitude(x)+C_bignum_to_double(y)));
    } else {
      try_extended_number("\003sysextended-plus", 3, k, x, y);
    }
  } else if (C_header_bits(x) == C_BIGNUM_TYPE) {
    if (y & C_FIXNUM_BIT) {
      C_u_2_integer_plus(4, (C_word)NULL, k, x, y);
    } else if (C_immediatep(y)) {
      barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, "+", y);
    } else if (C_block_header(y) == C_FLONUM_TAG) {
      C_word *a = C_alloc(C_SIZEOF_FLONUM);
      C_kontinue(k, C_flonum(&a, C_bignum_to_double(x)+C_flonum_magnitude(y)));
    } else if (C_header_bits(y) == C_BIGNUM_TYPE) {
      C_u_2_integer_plus(4, (C_word)NULL, k, x, y);
    } else {
      try_extended_number("\003sysextended-plus", 3, k, x, y);
    }
  } else {
    try_extended_number("\003sysextended-plus", 3, k, x, y);
  }
}

void C_ccall
C_u_2_integer_plus(C_word c, C_word self, C_word k, C_word x, C_word y)
{
  if ((x & y) & C_FIXNUM_BIT) {
    C_word ab[C_SIZEOF_FIX_BIGNUM], *a = ab;
    C_kontinue(k, C_a_i_fixnum_plus(&a, 2, x, y));
  } else {
    C_word ab[C_SIZEOF_FIX_BIGNUM * 2], *a = ab;
    if (x & C_FIXNUM_BIT) x = C_a_u_i_fix_to_big(&a, x);
    if (y & C_FIXNUM_BIT) y = C_a_u_i_fix_to_big(&a, y);

    if (C_bignum_negativep(x)) {
      if (C_bignum_negativep(y)) bignum_plus_unsigned(k, x, y, C_SCHEME_TRUE);
      else bignum_minus_unsigned(k, y, x);
    } else {
      if (C_bignum_negativep(y)) bignum_minus_unsigned(k, x, y);
      else bignum_plus_unsigned(k, x, y, C_SCHEME_FALSE);
    }
  }
}

/* XXX TODO OBSOLETE: This can be removed after recompiling c-platform.scm */
void C_ccall C_plus(C_word c, C_word closure, C_word k, ...)
{
  va_list v;
  C_word x, y;
  C_word iresult = C_fix(0);
  double fresult;
  C_alloc_flonum;

  va_start(v, k);
  c -= 2;

  while(c--) {
    x = va_arg(v, C_word);
    
    if(x & C_FIXNUM_BIT) {
      y = C_i_o_fixnum_plus(iresult, x);

      if(y == C_SCHEME_FALSE) {
	fresult = (double)C_unfix(iresult) + (double)C_unfix(x);
	goto flonum_result;
      }
      else iresult = y;
    }
    else if(!C_immediatep(x) && C_block_header(x) == C_FLONUM_TAG) {
      fresult = (double)C_unfix(iresult) + C_flonum_magnitude(x);
      goto flonum_result;
    }
    else barf(C_BAD_ARGUMENT_TYPE_ERROR, "+", x);
  }

  va_end(v);
  C_kontinue(k, iresult);

 flonum_result:
  while(c--) {
    x = va_arg(v, C_word);

    if(x & C_FIXNUM_BIT)
      fresult += (double)C_unfix(x);
    else if(!C_immediatep(x) && C_block_header(x) == C_FLONUM_TAG)
      fresult += C_flonum_magnitude(x);
    else barf(C_BAD_ARGUMENT_TYPE_ERROR, "+", x);
  }

  va_end(v);
  C_kontinue_flonum(k, fresult);
}


/* XXX TODO OBSOLETE: This can be removed after recompiling c-platform.scm */
C_regparm C_word C_fcall C_2_plus(C_word **ptr, C_word x, C_word y)
{
  C_word iresult;

  if(x & C_FIXNUM_BIT) {
    if(y & C_FIXNUM_BIT) {
      iresult = C_i_o_fixnum_plus(x, y);

      if(iresult == C_SCHEME_FALSE)
	return C_flonum(ptr, (double)C_unfix(x) + (double)C_unfix(y));
      else return iresult;
    }
    else if(!C_immediatep(y) && C_block_header(y) == C_FLONUM_TAG)
      return C_flonum(ptr, (double)C_unfix(x) + C_flonum_magnitude(y));
    else barf(C_BAD_ARGUMENT_TYPE_ERROR, "+", y);
  }
  else if(!C_immediatep(x) && C_block_header(x) == C_FLONUM_TAG) {
    if(y & C_FIXNUM_BIT) 
      return C_flonum(ptr, C_flonum_magnitude(x) + (double)C_unfix(y));
    else if(!C_immediatep(y) && C_block_header(y) == C_FLONUM_TAG)
      return C_flonum(ptr, C_flonum_magnitude(x) + C_flonum_magnitude(y));
    else barf(C_BAD_ARGUMENT_TYPE_ERROR, "+", y);
  }
  else barf(C_BAD_ARGUMENT_TYPE_ERROR, "+", x);
  /* shutup compiler */
  return C_flonum(ptr, 0.0/0.0);
}

static void bignum_minus_unsigned(C_word k, C_word x, C_word y)
{
  C_word kab[C_SIZEOF_CLOSURE(4)], *ka = kab, k2, size;

  switch(bignum_cmp_unsigned(x, y)) {
  case 0:	      /* x = y, return 0 */
    C_kontinue(k, C_fix(0));
  case -1:	      /* abs(x) < abs(y), return -(abs(y) - abs(x)) */
    k2 = C_closure(&ka, 4, (C_word)bignum_minus_unsigned_2, k, y, x);
    
    size = C_fix(C_bignum_size(y)); /* Maximum size of result is length of y. */
    C_allocate_bignum(5, (C_word)NULL, k2, size, C_SCHEME_TRUE, C_SCHEME_FALSE);
  case 1:	      /* abs(x) > abs(y), return abs(x) - abs(y) */
  default:
    k2 = C_closure(&ka, 4, (C_word)bignum_minus_unsigned_2, k, x, y);
    
    size = C_fix(C_bignum_size(x)); /* Maximum size of result is length of x. */
    C_allocate_bignum(5, (C_word)NULL, k2, size, C_SCHEME_FALSE, C_SCHEME_FALSE);
    break;
  }
}

static void bignum_minus_unsigned_2(C_word c, C_word self, C_word result)
{
  C_word k = C_block_item(self, 1),
         x = C_block_item(self, 2),
         y = C_block_item(self, 3);
  C_uword *scan_r = C_bignum_digits(result),
          *end_r = scan_r + C_bignum_size(result),
          *scan_y = C_bignum_digits(y),
          *end_y = scan_y + C_bignum_size(y),
          difference, digit;
  int borrow = 0;

  bignum_digits_destructive_copy(result, x); /* See bignum_plus_unsigned_2 */

  /* Destructively subtract y's digits w/ borrow from and back into r. */
  while (scan_y < end_y) {
    digit = *scan_r;
    if (borrow) {
      difference = digit - *scan_y++ - 1;
      borrow = difference >= digit;
    } else {
      difference = digit - *scan_y++;
      borrow = difference > digit;
    }
    (*scan_r++) = difference;
  }

  /* The end of y, the smaller number.  Propagate borrow into the rest of x. */
  while (borrow) {
    digit = *scan_r;
    difference = digit - borrow;
    borrow = difference >= digit;
    (*scan_r++) = difference;
  }

  assert(scan_r <= end_r);

  C_kontinue(k, C_bignum_simplify(result));
}

void C_ccall
C_2_basic_minus(C_word c, C_word self, C_word k, C_word x, C_word y)
{
  if (x & C_FIXNUM_BIT) {
    if (y & C_FIXNUM_BIT) {
      C_word *a = C_alloc(C_SIZEOF_FIX_BIGNUM);
      C_kontinue(k, C_a_i_fixnum_difference(&a, 2, x, y));
    } else if (C_immediatep(y)) {
      barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, "-", y);
    } else if (C_block_header(y) == C_FLONUM_TAG) {
      C_word *a = C_alloc(C_SIZEOF_FLONUM);
      C_kontinue(k, C_flonum(&a, (double)C_unfix(x) - C_flonum_magnitude(y)));
    } else if (C_header_bits(y) == C_BIGNUM_TYPE) {
      C_u_2_integer_minus(4, (C_word)NULL, k, x, y);
    } else {
      try_extended_number("\003sysextended-minus", 3, k, x, y);
    }
  } else if (C_immediatep(x)) {
    barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, "-", x);
  } else if (C_block_header(x) == C_FLONUM_TAG) {
    C_word *a = C_alloc(C_SIZEOF_FLONUM);
    if (y & C_FIXNUM_BIT) {
      C_kontinue(k, C_flonum(&a, C_flonum_magnitude(x) - (double)C_unfix(y)));
    } else if (C_immediatep(y)) {
      barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, "-", y);
    } else if (C_block_header(y) == C_FLONUM_TAG) {
      C_kontinue(k, C_a_i_flonum_difference(&a, 2, x, y)); /* XXX NAMING! */
    } else if (C_header_bits(y) == C_BIGNUM_TYPE) {
      C_kontinue(k, C_flonum(&a, C_flonum_magnitude(x)-C_bignum_to_double(y)));
    } else {
      try_extended_number("\003sysextended-minus", 3, k, x, y);
    }
  } else if (C_header_bits(x) == C_BIGNUM_TYPE) {
    if (y & C_FIXNUM_BIT) {
      C_u_2_integer_minus(4, (C_word)NULL, k, x, y);
    } else if (C_immediatep(y)) {
      barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, "-", y);
    } else if (C_block_header(y) == C_FLONUM_TAG) {
      C_word *a = C_alloc(C_SIZEOF_FLONUM);
      C_kontinue(k, C_flonum(&a, C_bignum_to_double(x)-C_flonum_magnitude(y)));
    } else if (C_header_bits(y) == C_BIGNUM_TYPE) {
      C_u_2_integer_minus(4, (C_word)NULL, k, x, y);
    } else {
      try_extended_number("\003sysextended-minus", 3, k, x, y);
    }
  } else {
    try_extended_number("\003sysextended-minus", 3, k, x, y);
  }
}

void C_ccall
C_u_2_integer_minus(C_word c, C_word self, C_word k, C_word x, C_word y)
{
  if ((x & y) & C_FIXNUM_BIT) {
    C_word ab[C_SIZEOF_FIX_BIGNUM], *a = ab;
    C_kontinue(k, C_a_i_fixnum_difference(&a, 2, x, y));
  } else {
    C_word ab[C_SIZEOF_FIX_BIGNUM * 2], *a = ab;
    if (x & C_FIXNUM_BIT) x = C_a_u_i_fix_to_big(&a, x);
    if (y & C_FIXNUM_BIT) y = C_a_u_i_fix_to_big(&a, y);

    if (C_bignum_negativep(x)) {
      if (C_bignum_negativep(y)) bignum_minus_unsigned(k, y, x);
      else bignum_plus_unsigned(k, x, y, C_SCHEME_TRUE);
    } else {
      if (C_bignum_negativep(y)) bignum_plus_unsigned(k, x, y, C_SCHEME_FALSE);
      else bignum_minus_unsigned(k, x, y);
    }
  }
}

/* XXX TODO OBSOLETE: This can be removed after recompiling c-platform.scm */
void C_ccall C_minus(C_word c, C_word closure, C_word k, C_word n1, ...)
{
  va_list v;
  C_word x, y;
  C_word iresult;
  double fresult;
  int ff = 0;
  C_alloc_flonum;

  if(c < 3) C_bad_min_argc(c, 3);

  if(n1 & C_FIXNUM_BIT) iresult = n1;
  else if(!C_immediatep(n1) && C_block_header(n1) == C_FLONUM_TAG) {
    fresult = C_flonum_magnitude(n1);
    ff = 1;
  }
  else barf(C_BAD_ARGUMENT_TYPE_ERROR, "-", n1);

  if(c == 3) {
    if(!ff) C_kontinue(k, C_fix(-C_unfix(n1)));
    else C_kontinue_flonum(k, -fresult);
  }

  va_start(v, n1);
  c -= 3;

  if(ff) goto flonum_result;

  while(c--) {
    x = va_arg(v, C_word);
    
    if(x & C_FIXNUM_BIT) {
      y = C_i_o_fixnum_difference(iresult, x);

      if(y == C_SCHEME_FALSE) {
	fresult = (double)C_unfix(iresult) - (double)C_unfix(x);
	goto flonum_result;
      }
      else iresult = y;
    }
    else if(!C_immediatep(x) && C_block_header(x) == C_FLONUM_TAG) {
      fresult = (double)C_unfix(iresult) - C_flonum_magnitude(x);
      goto flonum_result;
    }
    else barf(C_BAD_ARGUMENT_TYPE_ERROR, "-", x);
  }

  va_end(v);
  C_kontinue(k, iresult);

 flonum_result:
  while(c--) {
    x = va_arg(v, C_word);

    if(x & C_FIXNUM_BIT)
      fresult -= (double)C_unfix(x);
    else if(!C_immediatep(x) && C_block_header(x) == C_FLONUM_TAG)
      fresult -= C_flonum_magnitude(x);
    else barf(C_BAD_ARGUMENT_TYPE_ERROR, "-", x);
  }

  va_end(v);
  C_kontinue_flonum(k, fresult);
}


/* XXX TODO OBSOLETE: This can be removed after recompiling c-platform.scm */
C_regparm C_word C_fcall C_2_minus(C_word **ptr, C_word x, C_word y)
{
  C_word iresult;

  if(x & C_FIXNUM_BIT) {
    if(y & C_FIXNUM_BIT) {
      iresult = C_i_o_fixnum_difference(x, y);

      if(iresult == C_SCHEME_FALSE)
	return C_flonum(ptr, (double)C_unfix(x) - (double)C_unfix(y));
      else return iresult;
    }
    else if(!C_immediatep(y) && C_block_header(y) == C_FLONUM_TAG)
      return C_flonum(ptr, (double)C_unfix(x) - C_flonum_magnitude(y));
    else barf(C_BAD_ARGUMENT_TYPE_ERROR, "-", y);
  }
  else if(!C_immediatep(x) && C_block_header(x) == C_FLONUM_TAG) {
    if(y & C_FIXNUM_BIT) 
      return C_flonum(ptr, C_flonum_magnitude(x) - (double)C_unfix(y));
    else if(!C_immediatep(y) && C_block_header(y) == C_FLONUM_TAG)
      return C_flonum(ptr, C_flonum_magnitude(x) - C_flonum_magnitude(y));
    else barf(C_BAD_ARGUMENT_TYPE_ERROR, "-", y);
  }
  else barf(C_BAD_ARGUMENT_TYPE_ERROR, "-", x);
  /* shutup compiler */
  return C_flonum(ptr, 0.0/0.0);
}



/* XXX TODO OBSOLETE: This can be removed after recompiling c-platform.scm */
void C_ccall C_divide(C_word c, C_word closure, C_word k, C_word n1, ...)
{
  va_list v;
  C_word n2;
  C_word iresult, n3;
  int fflag;
  double fresult, f2;
  C_alloc_flonum;

  if(c < 3) C_bad_min_argc(c, 3);

  if(n1 & C_FIXNUM_BIT) {
    iresult = C_unfix(n1);
    fflag = 0;
  }
  else if(!C_immediatep(n1) && C_block_header(n1) == C_FLONUM_TAG) {
    fresult = C_flonum_magnitude(n1);
    fflag = 1;
  }
  else barf(C_BAD_ARGUMENT_TYPE_ERROR, "/", n1);

  if(c == 3) {
    if(fflag) {
      if(fresult == 0) barf(C_DIVISION_BY_ZERO_ERROR, "/");

      fresult = 1.0 / fresult;
    }
    else {
      if(iresult == 0) barf(C_DIVISION_BY_ZERO_ERROR, "/");
      else if(iresult == 1) C_kontinue(k, C_fix(1));

      fresult = 1.0 / (double)iresult;
      fflag = 1;
    }

    goto cont;
  }

  va_start(v, n1);
  c -= 3;

  while(c--) {
    n1 = va_arg(v, C_word);
    
    if(n1 & C_FIXNUM_BIT) {
      if(fflag) {
	if((n1 = C_unfix(n1)) == 0) 
	  barf(C_DIVISION_BY_ZERO_ERROR, "/");

	fresult /= n1;
      }
      else {
	if((n2 = C_unfix(n1)) == 0)
	  barf(C_DIVISION_BY_ZERO_ERROR, "/");

	n3 = iresult / n2;

	if((fresult = (double)iresult / (double)n2) != n3)
	  fflag = 1;
	else iresult = n3;
      }
    }
    else if(!C_immediatep(n1) && C_block_header(n1) == C_FLONUM_TAG) {
      if(fflag) {
	if((f2 = C_flonum_magnitude(n1)) == 0)
	  barf(C_DIVISION_BY_ZERO_ERROR, "/");

	fresult /= f2;
      }
      else {
	fflag = 1;

	if((f2 = C_flonum_magnitude(n1)) == 0)
	  barf(C_DIVISION_BY_ZERO_ERROR, "/");

	fresult = (double)iresult / f2;
      }
    }
    else barf(C_BAD_ARGUMENT_TYPE_ERROR, "/", n1);
  }

  va_end(v);
  
 cont:
  if(fflag) {
    C_kontinue_flonum(k, fresult);
  }
  else n1 = C_fix(iresult);

  C_kontinue(k, n1);
}


/* This is ugly but really cleans up the code below */
#define RETURN_Q_AND_OR_R(calc_q, calc_r)                 \
  if (C_truep(C_and(return_q, return_r))) {               \
    C_values(4, C_SCHEME_UNDEFINED, k, calc_q, calc_r);   \
  } else if (C_truep(return_r)) {                         \
    C_kontinue(k, calc_r);                                \
  } else {                                                \
    C_kontinue(k, calc_q);                                \
  }

/* Lossy; we could be in "quotient&remainder" or "modulo" */
#define DIVREM_LOC ((C_truep(C_and(return_q, return_r))) ? "/" :	\
                    (C_truep(return_q) ? "quotient" : "remainder"))

/* Another huge and ugly dispatch function.  This is the fundamental
 * division function.  It decides what functions to call depending on
 * whether we want to see the quotient and/or the remainder.  It only
 * knows about the "basic" types: fixnums, bignums and flonums.  The
 * Scheme "##sys#/" procedure handles ratnums and cplxnums.
 */
static C_regparm void
basic_divrem(C_word c, C_word self, C_word k, C_word x, C_word y, C_word return_q, C_word return_r)
{
  if (x & C_FIXNUM_BIT) {
    if (y & C_FIXNUM_BIT) {
      C_word ab[C_SIZEOF_FIX_BIGNUM], *a = ab;
      if (y == C_fix(0)) C_div_by_zero_error(DIVREM_LOC);

      RETURN_Q_AND_OR_R(C_a_i_fixnum_quotient_checked(&a, 2, x, y),
                        C_i_fixnum_remainder_checked(x, y));
    } else if (C_immediatep(y)) {
      barf(C_BAD_ARGUMENT_TYPE_NO_INTEGER_ERROR, DIVREM_LOC, y);
    } else if (C_block_header(y) == C_FLONUM_TAG) {
      C_word ab[C_SIZEOF_FLONUM*3], *a = ab;
      if (C_flonum_magnitude(y) == 0.0) C_div_by_zero_error(DIVREM_LOC);

      x = C_a_i_fix_to_flo(&a, 1, x);
      RETURN_Q_AND_OR_R(C_a_i_flonum_actual_quotient_checked(&a, 2, x, y),
                        C_a_i_flonum_remainder_checked(&a, 2, x, y));
    } else if (C_header_bits(y) == C_BIGNUM_TYPE) {
      integer_divrem(6, (C_word)NULL, k, x, y, return_q, return_r);
    } else {
      barf(C_BAD_ARGUMENT_TYPE_NO_INTEGER_ERROR, DIVREM_LOC, y);
    }
  } else if (C_immediatep(x)) {
    barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, DIVREM_LOC, x);
  } else if (C_block_header(x) == C_FLONUM_TAG) {
    if (!C_truep(C_u_i_fpintegerp(x))) {
      barf(C_BAD_ARGUMENT_TYPE_NO_INTEGER_ERROR, DIVREM_LOC, x);
    } else if (y & C_FIXNUM_BIT) {
      C_word ab[C_SIZEOF_FLONUM*3], *a = ab;
      if (y == C_fix(0)) C_div_by_zero_error(DIVREM_LOC);

      y = C_a_i_fix_to_flo(&a, 1, y);
      RETURN_Q_AND_OR_R(C_a_i_flonum_actual_quotient_checked(&a, 2, x, y),
                        C_a_i_flonum_remainder_checked(&a, 2, x, y));
    } else if (C_immediatep(y)) {
      barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, DIVREM_LOC, y);
    } else if (C_block_header(y) == C_FLONUM_TAG) {
      C_word ab[C_SIZEOF_FLONUM*3], *a = ab;
      if (C_flonum_magnitude(y) == 0.0) C_div_by_zero_error(DIVREM_LOC);

      RETURN_Q_AND_OR_R(C_a_i_flonum_actual_quotient_checked(&a, 2, x, y),
                        C_a_i_flonum_remainder_checked(&a, 2, x, y));
    } else if (C_header_bits(y) == C_BIGNUM_TYPE) {
      C_word k2, ab[C_SIZEOF_CLOSURE(3)], *a = ab;
      x = flo_to_tmp_bignum(x);
      k2 = C_closure(&a, 3, (C_word)divrem_intflo_2, k, x);
      integer_divrem(6, (C_word)NULL, k2, x, y, return_q, return_r);
    } else {
      barf(C_BAD_ARGUMENT_TYPE_NO_INTEGER_ERROR, DIVREM_LOC, y);
    }
  } else if (C_header_bits(x) == C_BIGNUM_TYPE) {
    if (y & C_FIXNUM_BIT) {
      integer_divrem(6, (C_word)NULL, k, x, y, return_q, return_r);
    } else if (C_immediatep(y)) {
      barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, DIVREM_LOC, y);
    } else if (C_block_header(y) == C_FLONUM_TAG) {
      if (!C_truep(C_u_i_fpintegerp(y))) {
        barf(C_BAD_ARGUMENT_TYPE_NO_INTEGER_ERROR, DIVREM_LOC, y);
      } else if (C_flonum_magnitude(y) == 0.0) {
        C_div_by_zero_error(DIVREM_LOC);
      } else {
        C_word k2, ab[C_SIZEOF_CLOSURE(3)], *a = ab;
        y = flo_to_tmp_bignum(y);
        k2 = C_closure(&a, 3, (C_word)divrem_intflo_2, k, y);
        integer_divrem(6, (C_word)NULL, k2, x, y, return_q, return_r);
      }
    } else if (C_header_bits(y) == C_BIGNUM_TYPE) {
      bignum_divrem(6, (C_word)NULL, k, x, y, return_q, return_r);
    } else {
      barf(C_BAD_ARGUMENT_TYPE_NO_INTEGER_ERROR, DIVREM_LOC, y);
    }
  } else {
    barf(C_BAD_ARGUMENT_TYPE_NO_INTEGER_ERROR, DIVREM_LOC, x);
  }
}

static void divrem_intflo_2(C_word c, C_word self, ...)
{
  C_word k = C_block_item(self, 1), x, y;
  va_list v;

  va_start(v, self);
  if (c == 2) {
    C_word ab[C_SIZEOF_FLONUM], *a = ab;
    x = va_arg(v, C_word);
    va_end(v);
    if (x & C_FIXNUM_BIT) x = C_a_i_fix_to_flo(&a, 1, x);
    else x = C_a_u_i_big_to_flo(&a, 1, x);
    free_tmp_bignum(C_block_item(self, 2));
    C_kontinue(k, x);
  } else { /* c == 3 */
    C_word ab[C_SIZEOF_FLONUM*2], *a = ab;
    x = va_arg(v, C_word);
    y = va_arg(v, C_word);
    va_end(v);

    if (x & C_FIXNUM_BIT) x = C_a_i_fix_to_flo(&a, 1, x);
    else x = C_a_u_i_big_to_flo(&a, 1, x);
    if (y & C_FIXNUM_BIT) y = C_a_i_fix_to_flo(&a, 1, y);
    else y = C_a_u_i_big_to_flo(&a, 1, y);
    free_tmp_bignum(C_block_item(self, 2));
    C_values(4, C_SCHEME_UNDEFINED, k, x, y);
  }
}

static void bignum_divrem_fixnum_2(C_word c, C_word self, C_word negated_big)
{
   C_word k = C_block_item(self, 1),
          return_q = C_block_item(self, 2),
          return_r = C_block_item(self, 3);
   RETURN_Q_AND_OR_R(negated_big, C_fix(0));
}

static C_regparm void
integer_divrem(C_word c, C_word self, C_word k, C_word x, C_word y, C_word return_q, C_word return_r)
{
  if (!(y & C_FIXNUM_BIT)) { /* y is bignum. */
    if (x & C_FIXNUM_BIT) {
      /* abs(x) < abs(y), so it will always be [0, x] except for this case: */
      if (x == C_fix(C_MOST_NEGATIVE_FIXNUM) &&
          C_bignum_negated_fitsinfixnump(y)) {
	RETURN_Q_AND_OR_R(C_fix(-1), C_fix(0));
      } else {
	RETURN_Q_AND_OR_R(C_fix(0), x);
      }
    } else {
      bignum_divrem(4, (C_word)NULL, k, x, y, return_q, return_r);
    }
  } else if (x & C_FIXNUM_BIT) { /* both x and y are fixnum. */
    C_word ab[C_SIZEOF_FIX_BIGNUM], *a = ab;
    if (y == C_fix(0)) C_div_by_zero_error(DIVREM_LOC);

    RETURN_Q_AND_OR_R(C_a_i_fixnum_quotient_checked(&a, 2, x, y),
                      C_i_fixnum_remainder_checked(x, y));
  } else { /* x is bignum, y is fixnum. */
    C_word absy = (y & C_INT_SIGN_BIT) ? -C_unfix(y) : C_unfix(y);

    if (y == C_fix(1)) {
      RETURN_Q_AND_OR_R(x, C_fix(0));
    } else if (y == C_fix(0)) {
      C_div_by_zero_error(DIVREM_LOC);
    } else if (y == C_fix(-1)) {
      C_word *ka, k2;
      ka = C_alloc(C_SIZEOF_CLOSURE(4));      
      k2 = C_closure(&ka, 4, (C_word)bignum_divrem_fixnum_2,
                     k, return_q, return_r);
      C_u_integer_negate(3, (C_word)NULL, k2, x);
    } else if (C_fitsinbignumhalfdigitp(absy) ||
               ((((C_uword)1 << (C_ilen(absy)-1)) == absy) &&
                C_fitsinfixnump(absy))) {
      if (C_truep(return_q)) {
        C_word q_negp = C_mk_bool((y & C_INT_SIGN_BIT) ?
                                  !(C_bignum_negativep(x)) :
                                  C_bignum_negativep(x)),
                r_negp = C_mk_bool(C_bignum_negativep(x)),
                *ka, k2, size;
	ka = C_alloc(C_SIZEOF_CLOSURE(9));
        size = C_fix(C_bignum_size(x));
        k2 = C_closure(&ka, 7,
                       (C_word)bignum_destructive_divide_unsigned_small,
                       k, x, C_fix(absy),
                       return_q, return_r, C_SCHEME_FALSE);
        C_allocate_bignum(5, (C_word)NULL, k2, size, q_negp, C_SCHEME_FALSE);
      } else {
        C_word rem;
	C_uword next_power = (C_uword)1 << (C_ilen(absy)-1);

	if (next_power == absy) { /* Is absy a power of two? */
          rem = *(C_bignum_digits(x)) & (next_power - 1);
        } else { /* Too bad, we have to do some real work */
          rem = bignum_remainder_unsigned_halfdigit(x, absy);
	}
        C_kontinue(k, C_bignum_negativep(x) ? C_fix(-rem) : C_fix(rem));
      }
    } else {			/* Just divide it as two bignums */
      C_word ab[C_SIZEOF_FIX_BIGNUM], *a = ab;
      bignum_divrem(6, (C_word)NULL, k, x, C_a_u_i_fix_to_big(&a, y),
                    return_q, return_r);
    }
  }
}

static C_regparm void
bignum_divrem(C_word c, C_word self, C_word k, C_word x, C_word y, C_word return_q, C_word return_r)
{
  C_word q_negp = C_mk_bool(C_bignum_negativep(y) ?
                            !C_bignum_negativep(x) :
                            C_bignum_negativep(x)),
         r_negp = C_mk_bool(C_bignum_negativep(x)), size;

  switch(bignum_cmp_unsigned(x, y)) {
  case 0:
    RETURN_Q_AND_OR_R(C_truep(q_negp) ? C_fix(-1) : C_fix(1), C_fix(0));
  case -1:
    RETURN_Q_AND_OR_R(C_fix(0), x);
  case 1:
  default:
    size = C_bignum_size(y);
    /* TODO: Activate later */
    /* if (size > C_BURNIKEL_ZIEGLER_THRESHOLD && */
    /*     /\* This avoids endless recursion for odd Ns just above threshold *\/ */
    /*     !(size & 1 && size < (C_BURNIKEL_ZIEGLER_THRESHOLD << 1))) { */
    /*   try_extended_number("\003sysbignum-divrem-burnikel-ziegler", 5, */
    /*     		  k, x, y, return_q, return_r); */
    /* } else */ if (C_truep(return_q)) {
      C_word kab[C_SIZEOF_FIX_BIGNUM+C_SIZEOF_CLOSURE(9)], *ka = kab, k2;
      k2 = C_closure(&ka, 9, (C_word)bignum_divide_2_unsigned, k, x, y,
                     return_q, return_r, r_negp,
                     /* Will be filled in later */
                     C_SCHEME_UNDEFINED, C_SCHEME_UNDEFINED);
      size = C_fix(C_bignum_size(x) + 1 - C_bignum_size(y));
      C_allocate_bignum(5, (C_word)NULL, k2, size, q_negp, C_SCHEME_FALSE);
    } else { /* We can skip bignum_divide_2_unsigned if we need no quotient */
      C_word kab[C_SIZEOF_FIX_BIGNUM+C_SIZEOF_CLOSURE(9)], *ka = kab, k2;
      k2 = C_closure(&ka, 9, (C_word)bignum_divide_2_unsigned_2, k, x, y,
                     return_q, return_r, r_negp,
                     /* Will be filled in later */
                     C_SCHEME_UNDEFINED, C_SCHEME_UNDEFINED);
      size = C_fix(C_bignum_size(x) + 1); /* May need to be normalized */
      C_allocate_bignum(5, (C_word)NULL, k2, size, r_negp, C_SCHEME_FALSE);
    }
  }
}

static C_word bignum_remainder_unsigned_halfdigit(C_word num, C_word den)
{
  C_uword *start = C_bignum_digits(num),
          *scan = start + C_bignum_size(num),
          rem = 0, two_digits;

  assert((den > 1) && (C_fitsinbignumhalfdigitp(den)));
  while (start < scan) {
    two_digits = (*--scan);
    rem = C_BIGNUM_DIGIT_COMBINE(rem, C_BIGNUM_DIGIT_HI_HALF(two_digits)) % den;
    rem = C_BIGNUM_DIGIT_COMBINE(rem, C_BIGNUM_DIGIT_LO_HALF(two_digits)) % den;
  }
  return rem;
}

/* External interface for the above internal divrem functions */
void C_ccall
C_basic_divrem(C_word c, C_word self, C_word k, C_word x, C_word y)
{
  if (c != 4) C_bad_argc_2(c, 4, self);
  basic_divrem(6, (C_word)NULL, k, x, y, C_SCHEME_TRUE, C_SCHEME_TRUE);
}

void C_ccall
C_u_integer_divrem(C_word c, C_word self, C_word k, C_word x, C_word y)
{
  integer_divrem(6, (C_word)NULL, k, x, y, C_SCHEME_TRUE, C_SCHEME_TRUE);
}

void C_ccall
C_basic_remainder(C_word c, C_word self, C_word k, C_word x, C_word y)
{
  if (c != 4) C_bad_argc_2(c, 4, self);
  basic_divrem(6, (C_word)NULL, k, x, y, C_SCHEME_FALSE, C_SCHEME_TRUE);
}

void C_ccall
C_u_integer_remainder(C_word c, C_word self, C_word k, C_word x, C_word y)
{
  integer_divrem(6, (C_word)NULL, k, x, y, C_SCHEME_FALSE, C_SCHEME_TRUE);
}

void C_ccall
C_basic_quotient(C_word c, C_word self, C_word k, C_word x, C_word y)
{
  if (c != 4) C_bad_argc_2(c, 4, self);
  basic_divrem(6, (C_word)NULL, k, x, y, C_SCHEME_TRUE, C_SCHEME_FALSE);
}

void C_ccall
C_u_integer_quotient(C_word c, C_word self, C_word k, C_word x, C_word y)
{
  integer_divrem(6, (C_word)NULL, k, x, y, C_SCHEME_TRUE, C_SCHEME_FALSE);
}


static void bignum_divide_2_unsigned(C_word c, C_word self, C_word quotient)
{
  C_word numerator = C_block_item(self, 2),
         remainder_negp = C_block_item(self, 6),
	 size = C_fix(C_bignum_size(numerator) + 1);

  /* Nice: We can recycle the current closure */
  C_set_block_item(self, 0, (C_word)bignum_divide_2_unsigned_2);
  C_set_block_item(self, 7, quotient);
  C_allocate_bignum(5, (C_word)NULL, self, size, remainder_negp, C_SCHEME_FALSE);
}

/* XXX TODO OBSOLETE: This can be removed after recompiling c-platform.scm */
C_regparm C_word C_fcall C_2_divide(C_word **ptr, C_word x, C_word y)
{
  C_word iresult;
  double fresult;
  int fflag = 0;

  if(x & C_FIXNUM_BIT) {
    if(y & C_FIXNUM_BIT) {
      if((iresult = C_unfix(y)) == 0) barf(C_DIVISION_BY_ZERO_ERROR, "/");

      fresult = (double)C_unfix(x) / (double)iresult;
      iresult = C_unfix(x) / iresult;
    }
    else if(!C_immediatep(y) && C_block_header(y) == C_FLONUM_TAG) {
      if((fresult = C_flonum_magnitude(y)) == 0.0)
	barf(C_DIVISION_BY_ZERO_ERROR, "/");

      fresult = (double)C_unfix(x) / fresult;
      fflag = 1;
    }
    else barf(C_BAD_ARGUMENT_TYPE_ERROR, "/", y);
  }
  else if(!C_immediatep(x) && C_block_header(x) == C_FLONUM_TAG) {
    fflag = 1;

    if(y & C_FIXNUM_BIT) {
      fresult = C_flonum_magnitude(x);

      if((iresult = C_unfix(y)) == 0) barf(C_DIVISION_BY_ZERO_ERROR, "/");

      fresult = fresult / (double)iresult;
    }
    else if(!C_immediatep(y) && C_block_header(y) == C_FLONUM_TAG) {
      if((fresult = C_flonum_magnitude(y)) == 0.0) barf(C_DIVISION_BY_ZERO_ERROR, "/");
      
      fresult = C_flonum_magnitude(x) / fresult;
    }
    else barf(C_BAD_ARGUMENT_TYPE_ERROR, "/", y);
  }
  else barf(C_BAD_ARGUMENT_TYPE_ERROR, "/", x);

  if(fflag || (double)iresult != fresult) return C_flonum(ptr, fresult);
  else return C_fix(iresult);
}

/* Compare two numbers as ratnums.  Either may be rat-, fix- or bignums */
static C_word rat_cmp(C_word x, C_word y)
{
  C_word ab[C_SIZEOF_FIX_BIGNUM*4], *a = ab, x1, x2, y1, y2,
         s, t, ssize, tsize, result, negp;
  C_uword *scan;

  /* Check for 1 or 0; if x or y is this, the other must be the ratnum */
  if (x == C_fix(0)) {	      /* Only the sign of y1 matters */
    return basic_cmp(x, C_block_item(y, 1), "ratcmp", 0);
  } else if (x == C_fix(1)) { /* x1*y1 <> x2*y2 --> y2 <> y1 | x1/x2 = 1/1 */
    return basic_cmp(C_block_item(y, 2), C_block_item(y, 1), "ratcmp", 0);
  } else if (y == C_fix(0)) { /* Only the sign of x1 matters */
    return basic_cmp(C_block_item(x, 1), y, "ratcmp", 0);
  } else if (y == C_fix(1)) { /* x1*y1 <> x2*y2 --> x1 <> x2 | y1/y2 = 1/1 */
    return basic_cmp(C_block_item(x, 1), C_block_item(x, 2), "ratcmp", 0);
  }

  /* Extract components x=x1/x2 and y=y1/y2 */
  if (x & C_FIXNUM_BIT || (C_header_bits(x) == C_BIGNUM_TYPE)) {
    x1 = x;
    x2 = C_fix(1);
  } else {
    x1 = C_block_item(x, 1);
    x2 = C_block_item(x, 2);
  }

  if (y & C_FIXNUM_BIT || (C_header_bits(y) == C_BIGNUM_TYPE)) {
    y1 = y;
    y2 = C_fix(1);
  } else {
    y1 = C_block_item(y, 1);
    y2 = C_block_item(y, 2);
  }

  /* We only want to deal with bignums (this is tricky enough) */
  if (x1 & C_FIXNUM_BIT) x1 = C_a_u_i_fix_to_big(&a, x1);
  if (x2 & C_FIXNUM_BIT) x2 = C_a_u_i_fix_to_big(&a, x2);
  if (y1 & C_FIXNUM_BIT) y1 = C_a_u_i_fix_to_big(&a, y1);
  if (y2 & C_FIXNUM_BIT) y2 = C_a_u_i_fix_to_big(&a, y2);

  /* We multiply using schoolbook method, so this will be very slow in
   * extreme cases.  This is a tradeoff we make so that comparisons
   * are inlineable, which makes a big difference for the common case.
   */
  ssize = C_fix(C_bignum_size(x1) + C_bignum_size(y2));
  negp = C_mk_bool(C_bignum_negativep(x1));
  s = allocate_tmp_bignum(C_fix(ssize), negp, C_SCHEME_TRUE);
  bignum_digits_multiply(x1, y2, s); /* Swap args if x1 < y2? */

  tsize = C_fix(C_bignum_size(y1) + C_bignum_size(x2));
  negp = C_mk_bool(C_bignum_negativep(y1));
  t = allocate_tmp_bignum(C_fix(tsize), negp, C_SCHEME_TRUE);
  bignum_digits_multiply(y1, x2, t); /* Swap args if y1 < x2? */

  /* Shorten the numbers if needed */
  for (scan = C_bignum_digits(s)+ssize-1; *scan == 0; scan--) ssize--;
  C_bignum_mutate_size(s, ssize);
  for (scan = C_bignum_digits(t)+tsize-1; *scan == 0; scan--) tsize--;
  C_bignum_mutate_size(t, tsize);

  result = C_i_bignum_cmp(s, t);

  free_tmp_bignum(t);
  free_tmp_bignum(s);
  return result;
}

C_regparm double C_fcall C_bignum_to_double(C_word bignum)
{
  double accumulator = 0;
  C_uword *start = C_bignum_digits(bignum),
          *scan = start + C_bignum_size(bignum);
  while (start < scan) {
    accumulator *= (C_word)1 << C_BIGNUM_HALF_DIGIT_LENGTH;
    accumulator *= (C_word)1 << C_BIGNUM_HALF_DIGIT_LENGTH;
    accumulator += (*--scan);
  }
  return(C_bignum_negativep(bignum) ? -accumulator : accumulator);
}

void C_ccall C_u_flo_to_int(C_word c, C_word self, C_word k, C_word x)
{
  int exponent;
  double significand = frexp(C_flonum_magnitude(x), &exponent);

  assert(C_truep(C_u_i_fpintegerp(x)));

  if (exponent <= 0) {
    C_kontinue(k, C_fix(0));
  } else if (exponent == 1) { /* TODO: check significand * 2^exp fits fixnum? */
    C_kontinue(k, significand < 0.0 ? C_fix(-1) : C_fix(1));
  } else {
    C_word kab[C_SIZEOF_CLOSURE(4) + C_SIZEOF_FLONUM], *ka = kab, k2, size,
           negp = C_mk_bool(C_flonum_magnitude(x) < 0.0),
           sign = C_flonum(&ka, fabs(significand));

    k2 = C_closure(&ka, 4, (C_word)flo_to_int_2, k, C_fix(exponent), sign);

    size = C_fix(C_BIGNUM_BITS_TO_DIGITS(exponent));
    C_allocate_bignum(5, (C_word)NULL, k2, size, negp, C_SCHEME_FALSE);
  }
}

static void flo_to_int_2(C_word c, C_word self, C_word result)
{
  C_word k = C_block_item(self, 1);
  C_uword exponent = C_unfix(C_block_item(self, 2)),
          *start = C_bignum_digits(result),
          *scan = start + C_bignum_size(result);
  double significand = C_flonum_magnitude(C_block_item(self, 3));

  fabs_frexp_to_digits(exponent, significand, start, scan);
  C_kontinue(k, C_bignum_simplify(result));
}

static void
fabs_frexp_to_digits(C_uword exp, double sign, C_uword *start, C_uword *scan)
{
  C_uword digit, odd_bits = exp % C_BIGNUM_DIGIT_LENGTH;

  assert(C_isfinite(sign));
  assert(0.5 <= sign && sign < 1); /* Guaranteed by frexp() and fabs() */
  assert((scan - start) == C_BIGNUM_BITS_TO_DIGITS(exp));
  
  if (odd_bits > 0) { /* Handle most significant digit first */
    sign *= (C_uword)1 << odd_bits;
    digit = (C_uword)sign;
    (*--scan) = digit;
    sign -= (double)digit;
  }

  while (start < scan && sign > 0) {
    sign *= pow(2.0, C_BIGNUM_DIGIT_LENGTH);
    digit = (C_uword)sign;
    (*--scan) = digit;
    sign -= (double)digit;
  }

  /* Finish up by clearing any remaining, lower, digits */
  while (start < scan)
    (*--scan) = 0;
}

static C_word flo_to_tmp_bignum(C_word x)
{
  /* TODO: allocating and initialising the bignum is pointless if we
   * already know the number of limbs in the comparand.  In fact,
   * bignum_cmp will first check the number of limbs and *then*
   * compare.  Instead, we can check beforehand and check the limbs
   * directly against the generated limbs, without allocating at all!
   */
  C_word tmp_big, negp = C_mk_bool(C_flonum_magnitude(x) < 0.0);
  int exponent;
  double significand = frexp(C_flonum_magnitude(x), &exponent);

  assert(C_u_i_fpintegerp(x));

  if (exponent <= 0) {
    tmp_big = allocate_tmp_bignum(C_fix(0), C_SCHEME_FALSE, C_SCHEME_FALSE);
  } else if (exponent == 1) { /* TODO: check significand * 2^exp fits fixnum? */
    /* Don't use fix_to_big to simplify caller code: it can just free this */
    tmp_big = allocate_tmp_bignum(C_fix(1), negp, C_SCHEME_FALSE);
    C_bignum_digits(tmp_big)[0] = 1;
  } else {
    C_uword size, *start, *end;

    size = C_fix(C_BIGNUM_BITS_TO_DIGITS(exponent));

    tmp_big = allocate_tmp_bignum(size, negp, C_SCHEME_FALSE);
    start = C_bignum_digits(tmp_big);
    end = start + C_bignum_size(tmp_big);

    fabs_frexp_to_digits(exponent, fabs(significand), start, end);
  }
  return tmp_big;
}

/* This is a bit weird: We have to compare flonums as bignums due to
 * precision loss on 64-bit platforms.  For simplicity, we convert
 * fixnums to bignums here.
 */
static C_word int_flo_cmp(C_word intnum, C_word flonum)
{
  C_word ab[C_SIZEOF_FIX_BIGNUM + C_SIZEOF_FLONUM], *a = ab, x, y, res;
  double i, f;

  f = C_flonum_magnitude(flonum);

  if (C_isnan(f)) {
    return C_SCHEME_FALSE; /* "mu" */
  } else if (C_isinf(f)) {
    return C_fix((f > 0.0) ? -1 : 1); /* x is smaller if f is +inf.0 */
  } else {
    f = modf(f, &i);

    x = (intnum & C_FIXNUM_BIT) ? C_a_u_i_fix_to_big(&a, intnum) : intnum;
    y = flo_to_tmp_bignum(C_flonum(&a, i));

    res = C_i_bignum_cmp(x, y);
    free_tmp_bignum(y);

    if (res == C_fix(0)) /* Use fraction to break tie. If f > 0, x is smaller */
      return C_fix((f > 0.0) ? -1 : ((f < 0.0) ? 1 : 0));
    else
      return res;
  }
}

/* For convenience (ie, to reduce the degree of mindfuck) */
static C_word flo_int_cmp(C_word flonum, C_word intnum)
{
  C_word res = int_flo_cmp(intnum, flonum);
  switch(res) {
  case C_fix(1): return C_fix(-1);
  case C_fix(-1): return C_fix(1);
  default: return res; /* Can be either C_fix(0) or C_SCHEME_FALSE(!) */
  }
}

/* This code is completely braindead, but at least it allows us to do
 * inline comparisons!
 */
static C_word rat_flo_cmp(C_word ratnum, C_word flonum)
{
  C_word ab[C_SIZEOF_FIX_BIGNUM * 2 + C_SIZEOF_FLONUM], *a = ab,
         num, denom, ibig, res, nscaled, iscaled, negp;
  C_uword *scan;
  int shift_amount, ilen, nlen;
  double i, f;

  f = C_flonum_magnitude(flonum);

  if (C_isnan(f)) {
    return C_SCHEME_FALSE; /* "mu" */
  } else if (C_isinf(f)) {
    return C_fix((f > 0.0) ? -1 : 1); /* x is smaller if f is +inf.0 */
  } else {
    /* Scale up the floating-point number to become a whole integer,
     * and remember power of two (# of bits) to shift the numerator.
     */
    shift_amount = 0;

    /* TODO: This doesn't work for denormalized flonums! */
    while (modf(f, &i) != 0.0) {
      f = ldexp(f, 1);
      shift_amount++;
    }

    i = f; /* TODO: split i and f so it'll work for denormalized flonums */

    num = C_block_item(ratnum, 1);
    num = (num & C_FIXNUM_BIT) ? C_a_u_i_fix_to_big(&a, num) : num;

    if (C_bignum_negativep(num) && i >= 0.0) { /* Save time if signs differ */
      return C_fix(-1);
    } else if (!C_bignum_negativep(num) && i <= 0.0) { /* num is never 0 */
      return C_fix(1);
    } else {
      negp = C_mk_bool(C_bignum_negativep(num));

      denom = C_block_item(ratnum, 2);
      denom = (denom & C_FIXNUM_BIT) ? C_a_u_i_fix_to_big(&a, denom) : denom;

      ibig = flo_to_tmp_bignum(C_flonum(&a, i));

      nlen = C_bignum_size(num) + C_bignum_size(denom);
      ilen = C_bignum_size(ibig) + C_bignum_size(denom);

      /* Now, multiply the scaled flonum by the denominator, so we can
       * compare it directly to the scaled numerator.  Unfortunately,
       * this won't use Karatsuba multiplication, so for large numbers
       * it will be slower than it could be if comparisons were done
       * in CPS context.
       */
      iscaled = allocate_tmp_bignum(C_fix(ilen), negp, C_SCHEME_TRUE);
      bignum_digits_multiply(denom, ibig, iscaled); /* Swap args if i < d? */
      free_tmp_bignum(ibig);

      nlen += C_BIGNUM_BITS_TO_DIGITS(shift_amount);
      nscaled = allocate_tmp_bignum(C_fix(nlen), negp, C_SCHEME_TRUE);

      scan = C_bignum_digits(nscaled) + shift_amount / C_BIGNUM_DIGIT_LENGTH;
      C_memcpy(scan, C_bignum_digits(num), C_wordstobytes(C_bignum_size(num)));
      shift_amount = shift_amount % C_BIGNUM_DIGIT_LENGTH;
      if(shift_amount > 0) {
        bignum_digits_destructive_shift_left(
         scan, C_bignum_digits(nscaled) + nlen, shift_amount);
      }

      /* Shorten the numbers if needed */
      for (scan = C_bignum_digits(iscaled)+ilen-1; *scan == 0; scan--) ilen--;
      C_bignum_mutate_size(iscaled, ilen);
      for (scan = C_bignum_digits(nscaled)+nlen-1; *scan == 0; scan--) nlen--;
      C_bignum_mutate_size(nscaled, nlen);

      /* Finally, we're ready to compare them! */
      res = C_i_bignum_cmp(nscaled, iscaled);
      free_tmp_bignum(nscaled);
      free_tmp_bignum(iscaled);

      return res;
    }
  }
}

static C_word flo_rat_cmp(C_word flonum, C_word ratnum)
{
  C_word res = rat_flo_cmp(ratnum, flonum);
  switch(res) {
  case C_fix(1): return C_fix(-1);
  case C_fix(-1): return C_fix(1);
  default: return res; /* Can be either C_fix(0) or C_SCHEME_FALSE(!) */
  }
}

/* The primitive comparison operator.  eqp should be 1 if we're only
 * interested in equality testing (can speed things up and in case of
 * compnums, equality checking is the only available operation).  This
 * may return #f, in case there is no answer (for NaNs) or as a quick
 * and dirty non-zero answer when eqp is true.  Ugly but effective :)
 */
static C_word basic_cmp(C_word x, C_word y, char *loc, int eqp)
{
  if (x & C_FIXNUM_BIT) {
    if (y & C_FIXNUM_BIT) {
      return C_fix((x < y) ? -1 : ((x > y) ? 1 : 0));
    } else if (C_immediatep(y)) {
      barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, loc, y);
    } else if (C_block_header(y) == C_FLONUM_TAG) {
      return int_flo_cmp(x, y);
    } else if (C_header_bits(y) == C_BIGNUM_TYPE) {
      C_word ab[C_SIZEOF_FIX_BIGNUM], *a = ab;
      return C_i_bignum_cmp(C_a_u_i_fix_to_big(&a, x), y);
    } else if (C_block_header(y) == C_STRUCTURE3_TAG) {
      if (C_block_item(y, 0) == C_ratnum_type_tag) {
        if (eqp) return C_SCHEME_FALSE;
        else return rat_cmp(x, y);
      } else if (C_block_item(y, 0) == C_cplxnum_type_tag) {
        if (eqp) return C_SCHEME_FALSE;
        else barf(C_BAD_ARGUMENT_TYPE_COMPLEX_NO_ORDERING_ERROR, loc, y);
      } else {
        barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, loc, y);
      }
    } else {
      barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, loc, y);
    }
  } else if (C_immediatep(x)) {
    barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, loc, x);
  } else if (C_block_header(x) == C_FLONUM_TAG) {
    if (y & C_FIXNUM_BIT) {
      return flo_int_cmp(x, y);
    } else if (C_immediatep(y)) {
      barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, loc, y);
    } else if (C_block_header(y) == C_FLONUM_TAG) {
      double a = C_flonum_magnitude(x), b = C_flonum_magnitude(y);
      if (C_isnan(a) || C_isnan(b)) return C_SCHEME_FALSE; /* "mu" */
      else return C_fix((a < b) ? -1 : ((a > b) ? 1 : 0));
    } else if (C_header_bits(y) == C_BIGNUM_TYPE) {
      return flo_int_cmp(x, y);
    } else if (C_block_header(y) == C_STRUCTURE3_TAG) {
      if (C_block_item(y, 0) == C_ratnum_type_tag) {
        return flo_rat_cmp(x, y);
      } else if (C_block_item(y, 0) == C_cplxnum_type_tag) {
	if (eqp) return C_SCHEME_FALSE;
        else barf(C_BAD_ARGUMENT_TYPE_COMPLEX_NO_ORDERING_ERROR, loc, y);
      } else {
        barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, loc, y);
      }
    } else {
      barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, loc, y);
    }
  } else if (C_header_bits(x) == C_BIGNUM_TYPE) {
    if (y & C_FIXNUM_BIT) {
      C_word ab[C_SIZEOF_FIX_BIGNUM], *a = ab;
      return C_i_bignum_cmp(x, C_a_u_i_fix_to_big(&a, y));
    } else if (C_immediatep(y)) {
      barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, loc, y);
    } else if (C_block_header(y) == C_FLONUM_TAG) {
      return int_flo_cmp(x, y);
    } else if (C_header_bits(y) == C_BIGNUM_TYPE) {
      return C_i_bignum_cmp(x, y);
    } else if (C_block_header(y) == C_STRUCTURE3_TAG) {
      if (C_block_item(y, 0) == C_ratnum_type_tag) {
        if (eqp) return C_SCHEME_FALSE;
        else return rat_cmp(x, y);
      } else if (C_block_item(y, 0) == C_cplxnum_type_tag) {
        if (eqp) return C_SCHEME_FALSE;
        else barf(C_BAD_ARGUMENT_TYPE_COMPLEX_NO_ORDERING_ERROR, loc, y);
      } else {
        barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, loc, y);
      }
    } else {
      barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, loc, y);
    }
  } else if (C_block_header(x) == C_STRUCTURE3_TAG &&
             (C_block_item(x, 0) == C_ratnum_type_tag)) {
    if (y & C_FIXNUM_BIT) {
      if (eqp) return C_SCHEME_FALSE;
      else return rat_cmp(x, y);
    } else if (C_immediatep(y)) {
      barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, loc, y);
    } else if (C_block_header(y) == C_FLONUM_TAG) {
      return rat_flo_cmp(x, y);
    } else if (C_header_bits(y) == C_BIGNUM_TYPE) {
      if (eqp) return C_SCHEME_FALSE;
      else return rat_cmp(x, y);
    } else if (C_block_header(y) == C_STRUCTURE3_TAG &&
               (C_block_item(y, 0) == C_ratnum_type_tag)) {
      if (eqp) {
        return C_and(C_and(C_i_integer_equalp(C_block_item(x, 1),
                                              C_block_item(y, 1)),
                           C_i_integer_equalp(C_block_item(x, 2),
                                              C_block_item(y, 2))),
                     C_fix(0));
      } else {
        return rat_cmp(x, y);
      }
    } else if (C_block_header(y) == C_STRUCTURE3_TAG &&
               (C_block_item(y, 0) == C_cplxnum_type_tag)) {
      if (eqp) return C_SCHEME_FALSE;
      else barf(C_BAD_ARGUMENT_TYPE_COMPLEX_NO_ORDERING_ERROR, loc, y);
    } else {
      barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, loc, y);
    }
  } else if (C_block_header(x) == C_STRUCTURE3_TAG &&
             (C_block_item(x, 0) == C_cplxnum_type_tag)) {
    if (!eqp) {
      barf(C_BAD_ARGUMENT_TYPE_COMPLEX_NO_ORDERING_ERROR, loc, x);
    } else if (y & C_FIXNUM_BIT) {
      return C_SCHEME_FALSE;
    } else if (C_immediatep(y)) {
      barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, loc, y);
    } else if (C_block_header(y) == C_FLONUM_TAG ||
               C_header_bits(y) == C_BIGNUM_TYPE ||
               (C_block_header(y) == C_STRUCTURE3_TAG &&
                (C_block_item(y, 0) == C_ratnum_type_tag))) {
      return C_SCHEME_FALSE;
    } else if (C_block_header(y) == C_STRUCTURE3_TAG &&
               (C_block_item(y, 0) == C_cplxnum_type_tag)) {
      return C_and(C_and(C_i_nequalp(C_block_item(x, 1), C_block_item(y, 1)),
                         C_i_nequalp(C_block_item(x, 2), C_block_item(y, 2))),
                   C_fix(0));
    } else {
      barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, loc, y);
    }
  } else {
    barf(C_BAD_ARGUMENT_TYPE_NO_NUMBER_ERROR, loc, x);
  }
}

static int bignum_cmp_unsigned(C_word x, C_word y)
{
  C_word xlen = C_bignum_size(x), ylen = C_bignum_size(y);

  if (xlen < ylen) {
    return -1;
  } else if (xlen > ylen) {
    return 1;
  } else if (x == y) {
    return 0;
  } else {
    C_uword *startx = C_bignum_digits(x),
            *scanx = startx + xlen,
            *scany = C_bignum_digits(y) + ylen;

    while (startx < scanx) {
      C_uword xdigit = (*--scanx), ydigit = (*--scany);
      if (xdigit < ydigit)
        return -1;
      if (xdigit > ydigit)
        return 1;
    }
    return 0;
  }
}

C_regparm C_word C_fcall C_i_bignum_cmp(C_word x, C_word y)
{
  if (C_bignum_negativep(x)) {
    if (C_bignum_negativep(y)) { /* Largest negative number is smallest */
      return C_fix(bignum_cmp_unsigned(y, x));
    } else {
      return C_fix(-1);
    }
  } else {
    if (C_bignum_negativep(y)) {
      return C_fix(1);
    } else {
      return C_fix(bignum_cmp_unsigned(x, y));
    }
  }
}

void C_ccall C_nequalp(C_word c, C_word closure, C_word k, ...)
{
  C_word x, y, result;
  va_list v;

  if (c < 4) C_bad_argc_2(c, 4, closure);

  c -= 2; 
  va_start(v, k);

  x = va_arg(v, C_word);
  while(--c) {
    y = va_arg(v, C_word);
    result = C_i_nequalp(x, y);
    if (result == C_SCHEME_FALSE) break;
  }

  va_end(v);
  C_kontinue(k, result);
}

C_regparm C_word C_fcall C_i_nequalp(C_word x, C_word y)
{
   return C_mk_bool(basic_cmp(x, y, "=", 1) == C_fix(0));
}

C_regparm C_word C_fcall C_i_integer_equalp(C_word x, C_word y)
{
  if (x & C_FIXNUM_BIT)
    return C_mk_bool(x == y);
  else if (y & C_FIXNUM_BIT)
    return C_SCHEME_FALSE;
  else
    return C_mk_bool(C_i_bignum_cmp(x, y) == C_fix(0));
}


void C_ccall C_greaterp(C_word c, C_word closure, C_word k, ...)
{
  C_word x, y, result;
  va_list v;

  if (c < 4) C_bad_argc_2(c, 4, closure);

  c -= 2; 
  va_start(v, k);

  x = va_arg(v, C_word);
  while(--c) {
    y = va_arg(v, C_word);
    result = C_i_greaterp(x, y);
    if (result == C_SCHEME_FALSE) break;
    x = y;
  }

  va_end(v);
  C_kontinue(k, result);
}


C_regparm C_word C_fcall C_i_greaterp(C_word x, C_word y)
{
   return C_mk_bool(basic_cmp(x, y, ">", 0) == C_fix(1));
}

C_regparm C_word C_fcall C_i_integer_greaterp(C_word x, C_word y)
{
  if (x & C_FIXNUM_BIT) {
    if (y & C_FIXNUM_BIT) {
      return C_mk_bool(C_unfix(x) > C_unfix(y));
    } else {
      return C_mk_bool(C_bignum_negativep(y));
    }
  } else if (y & C_FIXNUM_BIT) {
    return C_mk_nbool(C_bignum_negativep(x));
  } else {
    return C_mk_bool(C_i_bignum_cmp(x, y) == C_fix(1));
  }
}

void C_ccall C_lessp(C_word c, C_word closure, C_word k, ...)
{
  C_word x, y, result;
  va_list v;

  if (c < 4) C_bad_argc_2(c, 4, closure);

  c -= 2; 
  va_start(v, k);

  x = va_arg(v, C_word);
  while(--c) {
    y = va_arg(v, C_word);
    result = C_i_lessp(x, y);
    if (result == C_SCHEME_FALSE) break;
    x = y;
  }

  va_end(v);
  C_kontinue(k, result);
}


C_regparm C_word C_fcall C_i_lessp(C_word x, C_word y)
{
   return C_mk_bool(basic_cmp(x, y, "<", 0) == C_fix(-1));
}

C_regparm C_word C_fcall C_i_integer_lessp(C_word x, C_word y)
{
  if (x & C_FIXNUM_BIT) {
    if (y & C_FIXNUM_BIT) {
      return C_mk_bool(C_unfix(x) < C_unfix(y));
    } else {
      return C_mk_nbool(C_bignum_negativep(y));
    }
  } else if (y & C_FIXNUM_BIT) {
    return C_mk_bool(C_bignum_negativep(x));
  } else {
    return C_mk_bool(C_i_bignum_cmp(x, y) == C_fix(-1));
  }
}

void C_ccall C_greater_or_equal_p(C_word c, C_word closure, C_word k, ...)
{
  C_word x, y, result;
  va_list v;

  if (c < 4) C_bad_argc_2(c, 4, closure);

  c -= 2; 
  va_start(v, k);

  x = va_arg(v, C_word);
  while(--c) {
    y = va_arg(v, C_word);
    result = C_i_greater_or_equalp(x, y);
    if (result == C_SCHEME_FALSE) break;
    x = y;
  }

  va_end(v);
  C_kontinue(k, result);
}


C_regparm C_word C_fcall C_i_greater_or_equalp(C_word x, C_word y)
{
   C_word res = basic_cmp(x, y, ">=", 0);
   return C_mk_bool(res == C_fix(0) || res == C_fix(1));
}

C_regparm C_word C_fcall C_i_integer_greater_or_equalp(C_word x, C_word y)
{
  if (x & C_FIXNUM_BIT) {
    if (y & C_FIXNUM_BIT) {
      return C_mk_bool(C_unfix(x) >= C_unfix(y));
    } else {
      return C_mk_bool(C_bignum_negativep(y));
    }
  } else if (y & C_FIXNUM_BIT) {
    return C_mk_nbool(C_bignum_negativep(x));
  } else {
    C_word res = C_i_bignum_cmp(x, y);
    return C_mk_bool(res == C_fix(0) || res == C_fix(1));
  }
}

void C_ccall C_less_or_equal_p(C_word c, C_word closure, C_word k, ...)
{
  C_word x, y, result;
  va_list v;

  if (c < 4) C_bad_argc_2(c, 4, closure);

  c -= 2; 
  va_start(v, k);

  x = va_arg(v, C_word);
  while(--c) {
    y = va_arg(v, C_word);
    result = C_i_less_or_equalp(x, y);
    if (result == C_SCHEME_FALSE) break;
    x = y;
  }

  va_end(v);
  C_kontinue(k, result);
}


C_regparm C_word C_fcall C_i_less_or_equalp(C_word x, C_word y)
{
   C_word res = basic_cmp(x, y, "<=", 0);
   return C_mk_bool(res == C_fix(0) || res == C_fix(-1));
}


C_regparm C_word C_fcall C_i_integer_less_or_equalp(C_word x, C_word y)
{
  if (x & C_FIXNUM_BIT) {
    if (y & C_FIXNUM_BIT) {
      return C_mk_bool(C_unfix(x) <= C_unfix(y));
    } else {
      return C_mk_nbool(C_bignum_negativep(y));
    }
  } else if (y & C_FIXNUM_BIT) {
    return C_mk_bool(C_bignum_negativep(x));
  } else {
    C_word res = C_i_bignum_cmp(x, y);
    return C_mk_bool(res == C_fix(0) || res == C_fix(-1));
  }
}

void C_ccall C_gc(C_word c, C_word closure, C_word k, ...)
{
  int f;
  C_word arg;
  C_long size = 0;
  va_list v;

  va_start(v, k);

  if(c == 3) {
    arg = va_arg(v, C_word);
    f = C_truep(arg);
  }
  else if(c != 2) C_bad_min_argc(c, 2);
  else f = 1;

  C_save(k);
  va_end(v);

  if(c == 3) {
    if((arg & C_FIXNUM_BIT) != 0) size = C_unfix(arg);
    else if(arg == C_SCHEME_END_OF_LIST) size = percentage(heap_size, C_heap_growth);
  }

  if(size && !C_heap_size_is_fixed) {
    C_rereclaim2(size, 0);
    gc_2(NULL);
  }
  else if(f) C_fromspace_top = C_fromspace_limit;

  C_reclaim((void *)gc_2, NULL);
}


void gc_2(void *dummy)
{
  C_word k = C_restore;
  
  C_kontinue(k, C_fix((C_uword)C_fromspace_limit - (C_uword)C_fromspace_top));
}


void C_ccall C_open_file_port(C_word c, C_word closure, C_word k, C_word port, C_word channel, C_word mode)
{
  C_FILEPTR fp = (C_FILEPTR)NULL;
  C_char fmode[ 4 ];
  C_word n;
  char *buf;

  switch(channel) {
  case C_fix(0): fp = C_stdin; break;
  case C_fix(1): fp = C_stdout; break;
  case C_fix(2): fp = C_stderr; break;
  default:
    n = C_header_size(channel);
    buf = buffer;

    if(n >= STRING_BUFFER_SIZE) {
      if((buf = (char *)C_malloc(n + 1)) == NULL)
	barf(C_OUT_OF_MEMORY_ERROR, "open");
    }

    C_strncpy(buf, C_c_string(channel), n);
    buf[ n ] = '\0';
    if (n != strlen(buf))
      barf(C_ASCIIZ_REPRESENTATION_ERROR, "open", channel);
    n = C_header_size(mode);
    if (n >= sizeof(fmode)) n = sizeof(fmode) - 1;
    C_strncpy(fmode, C_c_string(mode), n);
    fmode[ n ] = '\0';
    if (n != strlen(fmode)) /* Shouldn't happen, but never hurts */
      barf(C_ASCIIZ_REPRESENTATION_ERROR, "open", mode);
    fp = C_fopen(buf, fmode);

    if(buf != buffer) C_free(buf);
  }
  
  C_set_block_item(port, 0, (C_word)fp);
  C_kontinue(k, C_mk_bool(fp != NULL));
}


void C_ccall C_allocate_vector(C_word c, C_word closure, C_word k, C_word size, C_word bvecf, C_word init, C_word align8)
{
  C_uword bytes, n = C_unfix(size);

  if(c != 6) C_bad_argc(c, 6);

  if(n > C_HEADER_SIZE_MASK)
    barf(C_OUT_OF_RANGE_ERROR, NULL, size, C_fix(C_HEADER_SIZE_MASK));

  if(!C_truep(bvecf)) bytes = C_wordstobytes(n) + sizeof(C_word);
  else bytes = n + sizeof(C_word);

  if(C_truep(align8)) bytes += sizeof(C_word);

  C_save(k);
  C_save(size);
  C_save(init);
  C_save(bvecf);
  C_save(align8);
  C_save(C_fix(bytes));

  if(!C_demand(C_bytestowords(bytes))) {
    /* Allocate on heap: */
    if((C_uword)(C_fromspace_limit - C_fromspace_top) < (bytes + stack_size * 2))
      C_fromspace_top = C_fromspace_limit; /* trigger major GC */
  
    C_save(C_SCHEME_TRUE);
    C_reclaim((void *)allocate_vector_2, NULL);
  }

  C_save(C_SCHEME_FALSE);
  allocate_vector_2(NULL);
}


void allocate_vector_2(void *dummy)
{
  C_word  mode = C_restore;
  C_uword bytes = C_unfix(C_restore);
  C_word  align8 = C_restore,
          bvecf = C_restore,
          init = C_restore;
  C_word  size = C_unfix(C_restore);
  C_word  k = C_restore,
          *v0, v;

  if(C_truep(mode)) {
    while((C_uword)(C_fromspace_limit - C_fromspace_top) < (bytes + stack_size)) {
      if(C_heap_size_is_fixed)
	panic(C_text("out of memory - cannot allocate vector (heap resizing disabled)"));

      C_save(init);
      C_save(k);
      C_rereclaim2(percentage(heap_size, C_heap_growth) + (C_uword)bytes, 0);
      k = C_restore;
      init = C_restore;
    }

    v0 = (C_word *)C_align((C_word)C_fromspace_top);
    C_fromspace_top += C_align(bytes);
  }
  else v0 = C_alloc(C_bytestowords(bytes));

#ifndef C_SIXTY_FOUR
  if(C_truep(align8) && C_aligned8(v0)) ++v0;
#endif

  v = (C_word)v0;

  if(!C_truep(bvecf)) {
    *(v0++) = C_VECTOR_TYPE | size | (C_truep(align8) ? C_8ALIGN_BIT : 0);
  
    while(size--) *(v0++) = init;
  }
  else {
    *(v0++) = C_STRING_TYPE | size;

    if(C_truep(init))
      C_memset(v0, C_character_code(init), size);
  }

  C_kontinue(k, v);
}


void C_ccall
C_allocate_bignum(C_word c, C_word self, C_word k, C_word size, C_word negp, C_word initp)
{
  C_uword bytes = C_wordstobytes(C_unfix(size) + 1); /* add slot for negp */

  if(bytes > C_HEADER_SIZE_MASK)
    barf(C_OUT_OF_RANGE_ERROR, NULL, size, C_fix(C_HEADER_SIZE_MASK));

  bytes += sizeof(C_word); /* header slot */

  C_save(k);
  C_save(negp);
  C_save(initp);
  C_save(C_fix(bytes));

  if(!C_demand(C_bytestowords(bytes))) {
    /* Allocate on heap: */
    if((C_uword)(C_fromspace_limit - C_fromspace_top) < (bytes + stack_size * 2))
      C_fromspace_top = C_fromspace_limit; /* trigger major GC */
  
    C_save(C_SCHEME_TRUE);
    C_reclaim((void *)allocate_bignum_2, NULL);
  }

  C_save(C_SCHEME_FALSE);
  allocate_bignum_2(NULL);
}

static void allocate_bignum_2(void *dummy)
{
  C_word  mode = C_restore;
  C_uword bytes = C_unfix(C_restore);
  C_word  initp = C_restore;
  C_word  negp = C_restore;
  C_word  k = C_restore;
  C_word  *v0, v;

  if(C_truep(mode)) {
    while((C_uword)(C_fromspace_limit - C_fromspace_top) < (bytes + stack_size)) {
      if(C_heap_size_is_fixed)
	panic(C_text("out of memory - cannot allocate bignum (heap resizing disabled)"));

      C_save(k);
      C_rereclaim2(percentage(heap_size, C_heap_growth) + (C_uword)bytes, 0);
      k = C_restore;
    }

    v0 = (C_word *)C_align((C_word)C_fromspace_top);
    C_fromspace_top += C_align(bytes);
  }
  else v0 = C_alloc(C_bytestowords(bytes));

  v = (C_word)v0;

  *(v0++) = C_BIGNUM_TYPE | (bytes-sizeof(C_word)); /* subtract header again */

  *(v0++) = C_truep(negp);
  if(C_truep(initp)) C_memset(v0, '\0', bytes - sizeof(C_word));

  C_kontinue(k, v);
}

static C_word allocate_tmp_bignum(C_word size, C_word negp, C_word initp)
{
  C_word *mem = C_malloc(C_wordstobytes(C_SIZEOF_BIGNUM(C_unfix(size)))),
          bignum = (C_word)mem;
  if (mem == NULL) abort();     /* TODO: panic */
  
  C_block_header_init(bignum, C_BIGNUM_TYPE | C_wordstobytes(C_unfix(size)+1));
  C_set_block_item(bignum, 0, C_truep(negp));

  if (C_truep(initp)) {
    C_memset(((C_uword *)C_data_pointer(bignum))+1,
             0, C_wordstobytes(C_unfix(size)));
  }

  return bignum;
}

/* Simplification: scan trailing zeroes, then return a fixnum if the
 * value fits, or trim the bignum's length. */
C_regparm C_word C_fcall C_bignum_simplify(C_word big)
{
  C_uword *start = C_bignum_digits(big),
          *last_digit = start + C_bignum_size(big) - 1,
          *scan = last_digit, tmp;
  int length;

  while (scan >= start && *scan == 0)
    scan--;
  length = scan - start + 1;
  
  switch(length) {
  case 0:
    return C_fix(0);
  case 1:
    tmp = *start;
    if (C_bignum_negativep(big) ?
        !(tmp & C_INT_SIGN_BIT) && C_fitsinfixnump(-(C_word)tmp) :
        C_ufitsinfixnump(tmp))
      return C_bignum_negativep(big) ? C_fix(-(C_word)tmp) : C_fix(tmp);
    /* FALLTHROUGH */
  default:
    if (scan < last_digit) C_bignum_mutate_size(big, length);
    return big;
  }
}

static void bignum_digits_destructive_negate(C_word result)
{
  C_uword *scan, *end, digit, sum;

  scan = C_bignum_digits(result);
  end = scan + C_bignum_size(result);

  do {
    digit = ~*scan;
    sum = digit + 1;
    *scan++ = sum;
  } while (sum == 0 && scan < end);

  for (; scan < end; scan++) {
    *scan = ~*scan;
  }
}

static C_uword
bignum_digits_destructive_scale_up_with_carry(C_uword *start, C_uword *end, C_uword factor, C_uword carry)
{
  C_uword digit, p;

  assert(C_fitsinbignumhalfdigitp(carry));
  assert(C_fitsinbignumhalfdigitp(factor));

  /* See fixnum_times.  Substitute xlo = factor, xhi = 0, y = digit
   * and simplify the result to reduce variable usage.
   */
  while (start < end) {
    digit = (*start);

    p = factor * C_BIGNUM_DIGIT_LO_HALF(digit) + carry;
    carry = C_BIGNUM_DIGIT_LO_HALF(p);

    p = factor * C_BIGNUM_DIGIT_HI_HALF(digit) + C_BIGNUM_DIGIT_HI_HALF(p);
    (*start++) = C_BIGNUM_DIGIT_COMBINE(C_BIGNUM_DIGIT_LO_HALF(p), carry);
    carry = C_BIGNUM_DIGIT_HI_HALF(p);
  }
  return carry;
}

static C_uword
bignum_digits_destructive_scale_down(C_uword *start, C_uword *end, C_uword denominator)
{
  C_uword digit, k = 0;
  C_uhword q_j_hi, q_j_lo;

  /* Single digit divisor case from Hacker's Delight, Figure 9-1,
   * adapted to modify u[] in-place instead of writing to q[].
   */
  while (start < end) {
    digit = (*--end);

    k = C_BIGNUM_DIGIT_COMBINE(k, C_BIGNUM_DIGIT_HI_HALF(digit)); /* j */
    q_j_hi = k / denominator;
    k -= q_j_hi * denominator;

    k = C_BIGNUM_DIGIT_COMBINE(k, C_BIGNUM_DIGIT_LO_HALF(digit)); /* j-1 */
    q_j_lo = k / denominator;
    k -= q_j_lo * denominator;
    
    *end = C_BIGNUM_DIGIT_COMBINE(q_j_hi, q_j_lo);
  }
  return k;
}

static C_uword
bignum_digits_destructive_shift_right(C_uword *start, C_uword *end, int shift_right, int negp)
{
  int shift_left = C_BIGNUM_DIGIT_LENGTH - shift_right;
  C_uword digit, carry = negp ? ((~(C_uword)0) << shift_left) : 0;

  assert(shift_right < C_BIGNUM_DIGIT_LENGTH);

  while (start < end) {
    digit = *(--end);
    *end = (digit >> shift_right) | carry;
    carry = digit << shift_left;
  }
  return carry >> shift_left; /* The bits that were shifted out to the right */
}

static C_uword
bignum_digits_destructive_shift_left(C_uword *start, C_uword *end, int shift_left)
{
  C_uword carry = 0, digit;
  int shift_right = C_BIGNUM_DIGIT_LENGTH - shift_left;

  assert(shift_left < C_BIGNUM_DIGIT_LENGTH);

  while (start < end) {
    digit = *start;
    (*start++) = (digit << shift_left) | carry;
    carry = digit >> shift_right;
  }
  return carry;	 /* This would end up as most significant digit if it fit */
}

static C_regparm void
bignum_digits_multiply(C_word x, C_word y, C_word result)
{
  C_uword product,
          *xd = C_bignum_digits(x),
          *yd = C_bignum_digits(y),
          *rd = C_bignum_digits(result);
  C_uhword carry, yj;
  /* Lengths in halfwords */
  int i, j, length_x = C_bignum_size(x) * 2, length_y = C_bignum_size(y) * 2;

  /* From Hacker's Delight, Figure 8-1 (top part) */
  for (j = 0; j < length_y; ++j) {
    yj = C_uhword_ref(yd, j);
    if (yj == 0) continue;
    carry = 0;
    for (i = 0; i < length_x; ++i) {
      product = (C_uword)C_uhword_ref(xd, i) * yj +
                (C_uword)C_uhword_ref(rd, i + j) + carry;
      C_uhword_set(rd, i + j, product);
      carry = C_BIGNUM_DIGIT_HI_HALF(product);
    }
    C_uhword_set(rd, j + length_x, carry);
  }
}

/* For help understanding this algorithm, see:
   Knuth, Donald E., "The Art of Computer Programming",
   volume 2, "Seminumerical Algorithms"
   section 4.3.1, "Multiple-Precision Arithmetic".

   [Yeah, that's a nice book but that particular section is not
   helpful at all, which is also pointed out by P. Brinch Hansen's
   "Multiple-Length Division Revisited: A Tour Of The Minefield".
   That's a more down-to-earth step-by-step explanation of the
   algorithm.  Add to this the C implementation in Hacker's Delight
   (section 9-2, p141--142) and you may be able to grok this...
   ...barely, if you're as math-challenged as I am -- sjamaan]
*/

static void bignum_divide_2_unsigned_2(C_word c, C_word self, C_word remainder)
{
  C_word k = C_block_item(self, 1),
         numerator = C_block_item(self, 2),
         denominator = C_block_item(self, 3),
         return_quotient = C_block_item(self, 4),
         return_remainder = C_block_item(self, 5),
         /* This one may be overwritten with the remainder */
         /* remainder_negp = C_block_item(self, 6), */
         quotient = C_block_item(self, 7),
	 length = C_bignum_size(denominator);
  C_uword d1 = *(C_bignum_digits(denominator) + length - 1),
          *startr = C_bignum_digits(remainder),
          *endr = startr + C_bignum_size(remainder);
  int shift;

  shift = C_BIGNUM_DIGIT_LENGTH - C_ilen(d1); /* nlz */

  /* We have to work on halfdigits, so we shift out only the necessary
   * amount in order fill out that halfdigit (base is halved).
   * This trick is shamelessly stolen from Gauche :)
   * See below for part 2 of the trick.
   */
  if (shift >= C_BIGNUM_HALF_DIGIT_LENGTH)
    shift -= C_BIGNUM_HALF_DIGIT_LENGTH;

  /* Code below won't always set high halfdigit of quotient, so do it here. */
  if (quotient != C_SCHEME_UNDEFINED)
    C_bignum_digits(quotient)[C_bignum_size(quotient)-1] = 0;

  bignum_digits_destructive_copy(remainder, numerator);
  *(endr-1) = 0;    /* Ensure most significant digit is initialised */
  if (shift == 0) { /* Already normalized */
    bignum_destructive_divide_normalized(remainder, denominator, quotient);
  } else { /* Requires normalisation; allocate scratch denominator for this */
    C_uword *startnd;
    C_word ndenom;

    bignum_digits_destructive_shift_left(startr, endr, shift);

    ndenom = allocate_tmp_bignum(C_fix(length), C_SCHEME_FALSE, C_SCHEME_FALSE);
    startnd = C_bignum_digits(ndenom);
    bignum_digits_destructive_copy(ndenom, denominator);
    bignum_digits_destructive_shift_left(startnd, startnd+length, shift);

    bignum_destructive_divide_normalized(remainder, ndenom, quotient);
    if (C_truep(return_remainder)) /* Otherwise, don't bother shifting back */
      bignum_digits_destructive_shift_right(startr, endr, shift, 0);

    free_tmp_bignum(ndenom);
  }

  if (C_truep(return_remainder)) {
    if (C_truep(return_quotient)) {
      C_values(4, C_SCHEME_UNDEFINED, k,
               C_bignum_simplify(quotient), C_bignum_simplify(remainder));
    } else {
      C_kontinue(k, C_bignum_simplify(remainder));
    }
  } else {
    assert(C_truep(return_quotient));
    C_kontinue(k, C_bignum_simplify(quotient));
  }
}

/* "small" is either a number that fits a halfdigit, or a power of two */
static void
bignum_destructive_divide_unsigned_small(C_word c, C_word self, C_word quotient)
{
  C_word k = C_block_item(self, 1),
         numerator = C_block_item(self, 2),
         denominator = C_unfix(C_block_item(self, 3)),
         /* return_quotient = C_block_item(self, 4), */
         return_remainder = C_block_item(self, 5),
         remainder_negp = C_block_item(self, 6);
  C_uword *start = C_bignum_digits(quotient),
          *end = start + C_bignum_size(quotient),
          remainder;
  int shift_amount;

  bignum_digits_destructive_copy(quotient, numerator);

  shift_amount = C_ilen(denominator)-1;
  if (((C_uword)1 << shift_amount) == denominator) { /* Power of two?  Shift! */
    remainder = bignum_digits_destructive_shift_right(start,end,shift_amount,0);
    assert(C_ufitsinfixnump(remainder));
  } else {
    remainder = bignum_digits_destructive_scale_down(start, end, denominator);
    assert(C_fitsinbignumhalfdigitp(remainder));
  }

  quotient = C_bignum_simplify(quotient);

  if (C_truep(return_remainder)) {
    remainder = C_truep(remainder_negp) ? -remainder : remainder;
    C_values(4, C_SCHEME_UNDEFINED, k, quotient, C_fix(remainder));
  } else {
    C_kontinue(k, quotient);
  }
}

static C_regparm void
bignum_destructive_divide_normalized(C_word big_u, C_word big_v, C_word big_q)
{
  C_uword *v = C_bignum_digits(big_v),
          *u = C_bignum_digits(big_u),
          *q = big_q == C_SCHEME_UNDEFINED ? NULL : C_bignum_digits(big_q),
           p,               /* product of estimated quotient & "denominator" */
           hat, qhat, rhat, /* estimated quotient and remainder digit */
           vn_1, vn_2;      /* "cached" values v[n-1], v[n-2] */
  C_word t, k;              /* Two helpers: temp/final remainder and "borrow" */
  /* We use plain ints here, which theoretically may not be enough on
   * 64-bit for an insanely huge number, but it is a _lot_ faster.
   */
  int n = C_bignum_size(big_v) * 2,       /* in halfwords */
      m = (C_bignum_size(big_u) * 2) - 2; /* Correct for extra digit */
  int i, j;		   /* loop  vars */

  /* Part 2 of Gauche's aforementioned trick: */
  if (C_uhword_ref(v, n-1) == 0) n--;

  /* These won't change during the loop, but are used in every step. */
  vn_1 = C_uhword_ref(v, n-1);
  vn_2 = C_uhword_ref(v, n-2);

  /* See also Hacker's Delight, Figure 9-1.  This is almost exactly that. */
  for (j = m - n; j >= 0; j--) {
    hat = C_BIGNUM_DIGIT_COMBINE(C_uhword_ref(u, j+n), C_uhword_ref(u, j+n-1));
    if (hat == 0) {
      if (q != NULL) C_uhword_set(q, j, 0);
      continue;
    }
    qhat = hat / vn_1;
    rhat = hat % vn_1;

    /* Two whiles is faster than one big check with an OR.  Thanks, Gauche! */
    while(qhat >= (1L << C_BIGNUM_HALF_DIGIT_LENGTH)) { qhat--; rhat += vn_1; }
    while(qhat * vn_2 > C_BIGNUM_DIGIT_COMBINE(rhat, C_uhword_ref(u, j+n-2))
	  && rhat < (1L << C_BIGNUM_HALF_DIGIT_LENGTH)) {
      qhat--;
      rhat += vn_1;
    }

    /* Multiply and subtract */
    k = 0;
    for (i = 0; i < n; i++) {
      p = qhat * C_uhword_ref(v, i);
      t = C_uhword_ref(u, i+j) - k - C_BIGNUM_DIGIT_LO_HALF(p);
      C_uhword_set(u, i+j, t);
      k = C_BIGNUM_DIGIT_HI_HALF(p) - (t >> C_BIGNUM_HALF_DIGIT_LENGTH);
    }
    t = C_uhword_ref(u,j+n) - k;
    C_uhword_set(u, j+n, t);

    if (t < 0) {		/* Subtracted too much? */
      qhat--;
      k = 0;
      for (i = 0; i < n; i++) {
        t = (C_uword)C_uhword_ref(u, i+j) + C_uhword_ref(v, i) + k;
        C_uhword_set(u, i+j, t);
	k = t >> C_BIGNUM_HALF_DIGIT_LENGTH;
      }
      C_uhword_set(u, j+n, (C_uhword_ref(u, j+n) + k));
    }
    if (q != NULL) C_uhword_set(q, j, qhat);
  } /* end j */
}


void C_ccall C_string_to_symbol(C_word c, C_word closure, C_word k, C_word string)
{
  int len, key;
  C_word s, *a = C_alloc(C_SIZEOF_SYMBOL + C_SIZEOF_BUCKET);
  C_char *name;

  if(c != 3) C_bad_argc(c, 3);

  if(C_immediatep(string) || C_header_bits(string) != C_STRING_TYPE)
    barf(C_BAD_ARGUMENT_TYPE_ERROR, "string->symbol", string);
    
  len = C_header_size(string);
  name = (C_char *)C_data_pointer(string);
  key = hash_string(len, name, symbol_table->size, symbol_table->rand, 0);

  if(!C_truep(s = lookup(key, len, name, symbol_table))) 
    s = add_symbol(&a, key, string, symbol_table);

  C_kontinue(k, s);
}


/* XXX TODO OBSOLETE: This can be removed after recompiling c-platform.scm */
C_regparm C_word C_fcall 
C_a_i_exact_to_inexact(C_word **a, int c, C_word n)
{
  if(n & C_FIXNUM_BIT) 
    return C_flonum(a, (double)C_unfix(n));
  else if(C_immediatep(n) || C_block_header(n) != C_FLONUM_TAG)
    barf(C_BAD_ARGUMENT_TYPE_ERROR, "exact->inexact", n);

  return n;
}


/* this is different from C_a_i_flonum_round, for R5RS compatibility */
C_regparm C_word C_fcall C_a_i_flonum_round_proper(C_word **ptr, int c, C_word n)
{
  double fn, i, f, i2, r;

  fn = C_flonum_magnitude(n);
  if(fn < 0.0) {
    f = modf(-fn, &i);
    if(f < 0.5 || (f == 0.5 && modf(i * 0.5, &i2) == 0.0))
      r = -i;
    else
      r = -(i + 1.0);
  }
  else if(fn == 0.0/* || fn == -0.0*/)
    r = fn;
  else {
    f = modf(fn, &i);
    if(f < 0.5 || (f == 0.5 && modf(i * 0.5, &i2) == 0.0))
      r = i;
    else
      r = i + 1.0;
  }

  return C_flonum(ptr, r);
}

C_regparm C_word C_fcall
C_a_i_flonum_gcd(C_word **p, C_word n, C_word x, C_word y)
{
   double xub, yub, r;

   if (!C_truep(C_u_i_fpintegerp(x)))
     barf(C_BAD_ARGUMENT_TYPE_NO_INTEGER_ERROR, "gcd", x);
   if (!C_truep(C_u_i_fpintegerp(y)))
     barf(C_BAD_ARGUMENT_TYPE_NO_INTEGER_ERROR, "gcd", y);

   xub = C_flonum_magnitude(x);
   yub = C_flonum_magnitude(y);

   if (xub < 0.0) xub = -xub;
   if (yub < 0.0) yub = -yub;
   
   while(yub != 0.0) {
     r = fmod(xub, yub);
     xub = yub;
     yub = r;
   }
   return C_flonum(p, xub);
}


/* XXX TODO OBSOLETE: This can be removed after recompiling c-platform.scm */
void C_ccall C_quotient(C_word c, C_word closure, C_word k, C_word n1, C_word n2)
{
  double f1, f2, r;
  C_word result;
  C_alloc_flonum;

  if(c != 4) C_bad_argc(c, 4);

  if(n1 &C_FIXNUM_BIT) {
    if(n2 &C_FIXNUM_BIT) {
      if((n2 = C_unfix(n2)) == 0)
	barf(C_DIVISION_BY_ZERO_ERROR, "quotient");
      
      result = C_fix(C_unfix(n1) / n2);
      C_kontinue(k, result);
    }
    else if(!C_immediatep(n2) && C_block_header(n2) == C_FLONUM_TAG) {
      f1 = (double)C_unfix(n1);
      f2 = C_flonum_magnitude(n2);
      if(C_isnan(f2) || C_isinf(f2) || C_modf(f2, &r) != 0.0)
        barf(C_BAD_ARGUMENT_TYPE_NO_INTEGER_ERROR, "quotient", n2);
    }
    else barf(C_BAD_ARGUMENT_TYPE_ERROR, "quotient", n2);
  }
  else if(!C_immediatep(n1) && C_block_header(n1) == C_FLONUM_TAG) {
    f1 = C_flonum_magnitude(n1);
    if(C_isnan(f1) || C_isinf(f1) || C_modf(f1, &r) != 0.0)
      barf(C_BAD_ARGUMENT_TYPE_NO_INTEGER_ERROR, "quotient", n1);

    if(n2 &C_FIXNUM_BIT)
      f2 = (double)C_unfix(n2);
    else if(!C_immediatep(n2) && C_block_header(n2) == C_FLONUM_TAG) {
      f2 = C_flonum_magnitude(n2);
      if(C_isnan(f2) || C_isinf(f2) || C_modf(f2, &r) != 0.0)
        barf(C_BAD_ARGUMENT_TYPE_NO_INTEGER_ERROR, "quotient", n2);
    }
    else barf(C_BAD_ARGUMENT_TYPE_ERROR, "quotient", n2);
  }
  else barf(C_BAD_ARGUMENT_TYPE_ERROR, "quotient", n1);

  if(f2 == 0)
    barf(C_DIVISION_BY_ZERO_ERROR, "quotient");

  modf(f1 / f2, &r);
  C_kontinue_flonum(k, r);
}


/* TODO OBSOLETE XXX: This needs to go, but still translated by c-platform */
C_regparm C_word C_fcall
C_a_i_string_to_number(C_word **a, int c, C_word str, C_word radix0)
{
  int radix, radixpf = 0, sharpf = 0, ratf = 0, exactf = 0, exactpf = 0, periodf = 0, expf = 0;
  C_word n1, n;
  C_char *sptr, *eptr, *rptr;
  double fn1, fn;

  if(radix0 & C_FIXNUM_BIT) radix = C_unfix(radix0);
  else barf(C_BAD_ARGUMENT_TYPE_BAD_BASE_ERROR, "string->number", radix0);

  if(C_immediatep(str) || C_header_bits(str) != C_STRING_TYPE)
    barf(C_BAD_ARGUMENT_TYPE_ERROR, "string->number", str);

  if((n = C_header_size(str)) == 0) {
  fail:
    n = C_SCHEME_FALSE;
    goto fini;
  }

  if(n >= STRING_BUFFER_SIZE - 1) goto fail;

  C_memcpy(sptr = buffer, C_c_string(str), n > (STRING_BUFFER_SIZE - 1) ? STRING_BUFFER_SIZE : n);
  buffer[ n ] = '\0';
  if (n != strlen(buffer)) /* Don't barf; this is simply invalid number syntax */
    goto fail;
  
  while(*sptr == '#') {
    switch(C_tolower((int)*(++sptr))) {
    case 'b': if(radixpf) goto fail; else { radix = 2; radixpf = 1; } break;
    case 'o': if(radixpf) goto fail; else { radix = 8; radixpf = 1; } break;
    case 'd': if(radixpf) goto fail; else { radix = 10; radixpf = 1; } break;
    case 'x': if(radixpf) goto fail; else { radix = 16; radixpf = 1; } break;
    case 'e': if(exactpf) goto fail; else { exactf = 1; exactpf = 1; } break;
    case 'i': if(exactpf) goto fail; else { exactf = 0; exactpf = 1; } break;
    default: goto fail;  /* Unknown prefix type */
    }

    ++sptr;
  }
  
  /* Scan for embedded special characters and do basic sanity checking: */
  for(eptr = sptr, rptr = sptr; *eptr != '\0'; ++eptr) {
    switch(C_tolower((int)*eptr)) {
    case '.': 
      if(periodf || ratf || expf) goto fail;
      
      periodf = 1;
      break;

    case '#':
      if (expf || (eptr == rptr) ||
	  (!sharpf && (eptr == rptr+1) && (C_strchr("+-.", *rptr) != NULL)))
        goto fail;
      
      sharpf = 1;
      *eptr = '0';
      
      break;
    case '/':
      if(periodf || ratf || expf || eptr == sptr) goto fail;
      
      sharpf = 0; /* Allow sharp signs in the denominator */
      ratf = 1;
      rptr = eptr+1;
      break;
    case 'e':
    case 'd':
    case 'f':
    case 'l':
    case 's':
      /* Don't set exp flag if we see the "f" in "inf.0" (preceded by 'n') */
      /* Other failure modes are handled elsewhere. */
      if(radix == 10 && eptr > sptr && C_tolower((int)*(eptr-1)) != 'n') {
        if (ratf) goto fail;
	
        expf = 1;
	sharpf = 0;
	*eptr = 'e'; /* strtod() normally only understands 'e', not dfls */
      }
      break;
    default:
      if(sharpf) goto fail;
      break;
    }
  }
  if (eptr == rptr) goto fail; /* Disallow "empty" numbers like "#x" and "1/" */
  
  /* check for rational representation: */
  if(rptr != sptr) {
    if (*(rptr) == '-' || *(rptr) == '+') {
      n = C_SCHEME_FALSE;
      goto fini;
    }
    *(rptr-1) = '\0';

    switch(convert_string_to_number(sptr, radix, &n1, &fn1)) {
    case 0:
      n = C_SCHEME_FALSE;
      goto fini;

    case 1:
      fn1 = (double)n1;
      break;

      /* case 2: nop */
    }

    sptr = rptr;
  }    
  
  /* convert number and return result: */
  switch(convert_string_to_number(sptr, radix, &n, &fn)) {
  case 0: 			/* failed */
    n = C_SCHEME_FALSE;
    break;

  case 1:			/* fixnum */
    if(sharpf || ratf || (exactpf && !exactf)) {
      n = C_flonum(a, ratf ? fn1 / (double)n : (double)n);

      if(exactpf && exactf) n = maybe_inexact_to_exact(n);
    }
    else n = C_fix(n);

    break;

  case 2:			/* flonum */
    n = C_flonum(a, ratf ? fn1 / fn : fn);

    if(exactpf && exactf) n = maybe_inexact_to_exact(n);

    break;
  }

 fini:
  return n;
}

void C_ccall
C_digits_to_integer(C_word c, C_word self, C_word k, C_word str,
                    C_word start, C_word end, C_word radix, C_word negp)
{
  assert((C_unfix(radix) > 1) && C_fitsinbignumhalfdigitp(C_unfix(radix)));
  
  if (start == end) {
    C_kontinue(k, C_SCHEME_FALSE);
  } else {
    C_word kab[C_SIZEOF_CLOSURE(6)], *ka = kab, k2, size;
    size_t nbits;
    k2 = C_closure(&ka, 6, (C_word)digits_to_integer_2, k, str, start, end, radix);

    nbits = (C_unfix(end) - C_unfix(start)) * C_ilen(C_unfix(radix)-1);
    size = C_fix(C_BIGNUM_BITS_TO_DIGITS(nbits));
    C_allocate_bignum(5, (C_word)NULL, k2, size, negp, C_SCHEME_FALSE);
  }
}

C_inline int hex_char_to_digit(int ch)
{
  if (ch == (int)'#') return 0; /* Hash characters in numbers are mapped to 0 */
  else if (ch >= (int)'a') return ch - (int)'a' + 10; /* lower hex */
  else if (ch >= (int)'A') return ch - (int)'A' + 10; /* upper hex */
  else return ch - (int)'0'; /* decimal (OR INVALID; handled elsewhere) */
}

static void
digits_to_integer_2(C_word c, C_word self, C_word result)
{
  C_word k = C_block_item(self, 1),
         str = C_block_item(self, 2),
         start = C_unfix(C_block_item(self, 3)),
         end = C_unfix(C_block_item(self, 4)),
         radix = C_unfix(C_block_item(self, 5));
  char *s = C_c_string(str);

  C_kontinue(k, str_to_bignum(result, s + start, s + end, radix));
}

/* Write from digit character stream to bignum.  Bignum does not need
 * to be initialised.  Returns the bignum, or a fixnum.  Assumes the
 * string contains only digits that fit within radix (checked by
 * string->number).
 */
static C_regparm C_word
str_to_bignum(C_word bignum, char *str, char *str_end, int radix)
{
  int radix_shift, str_digit;
  C_uword *digits = C_bignum_digits(bignum),
          *end_digits = digits + C_bignum_size(bignum), big_digit = 0;

  /* Below, we try to save up as much as possible in big_digit, and
   * only when it exceeds what we would be able to multiply easily, we
   * scale up the bignum and add what we saved up.
   */
  radix_shift = C_ilen(radix) - 1;
  if (((C_uword)1 << radix_shift) == radix) { /* Power of two? */
    int n = 0; /* Number of bits read so far into current big digit */

    /* Read from least to most significant digit to avoid shifting or scaling */
    while (str_end > str) {
      str_digit = hex_char_to_digit((int)*--str_end);

      big_digit |= (C_uword)str_digit << n;
      n += radix_shift;

      if (n >= C_BIGNUM_DIGIT_LENGTH) {
	n -= C_BIGNUM_DIGIT_LENGTH;
	*digits++ = big_digit;
	big_digit = str_digit >> (radix_shift - n);
      }
    }
    assert(n < C_BIGNUM_DIGIT_LENGTH);
    /* If radix isn't an exact divisor of digit length, write final digit */
    if (n > 0) *digits++ = big_digit;
    assert(digits == end_digits);
  } else {			  /* Not a power of two */
    C_uword *last_digit = digits, factor;  /* bignum starts as zero */

    do {
      factor = radix;
      while (str < str_end && C_fitsinbignumhalfdigitp(factor)) {
        str_digit = hex_char_to_digit((int)*str++);
	factor *= radix;
	big_digit = radix * big_digit + str_digit;
      }

      big_digit = bignum_digits_destructive_scale_up_with_carry(
                   digits, last_digit, factor / radix, big_digit);

      if (big_digit) {
	(*last_digit++) = big_digit; /* Move end */
        big_digit = 0;
      }
    } while (str < str_end);

    /* Set remaining digits to zero so bignum_simplify can do its work */
    assert(last_digit <= end_digits);
    while (last_digit < end_digits) *last_digit++ = 0;
  }

  return C_bignum_simplify(bignum);
}

static int from_n_nary(C_char *str, int base, double *r)
{
  double n = 0;
  C_char *ptr = str;

  while(*ptr != '\0') {
    int c = C_tolower((int)(*(ptr++)));

    if(c < '0') return 0;
    else if(c >= '0' + base) {
      if(base < 10) return 0;
      else if(c < 'a') return 0;
      else if(c >= 'a' + base - 10) return 0;
      else n = n * base + c - 'a' + 10;
    }
    else n = n * base + c - '0';
  }

  *r = n;
  return 1;
}


/* TODO OBSOLETE XXX: This needs to go, but still used in decode_literal */
C_regparm C_word C_fcall convert_string_to_number(C_char *str, int radix, C_word *fix, double *flo)
{
  C_ulong ln;
  C_word n;
  C_char *eptr, *eptr2;
  double fn;
  int len = C_strlen(str);

  if(radix == 10) {
    if (len == 6) {
      if((*str == '+' || *str == '-') &&
         C_strchr("inIN", *(str+1)) != NULL &&
         C_strchr("naNA", *(str+2)) != NULL &&
         C_strchr("fnFN", *(str+3)) != NULL &&
         *(str+4) == '.' && *(str+5) == '0') {
        if (*(str+1) == 'i' || *(str+1) == 'I')   /* Inf */
          *flo = 1.0/0.0;
        else                                      /* NaN */
          *flo = 0.0/0.0;
        if (*str == '-')
          *flo *= -1.0;
        return 2;
      }
    }
    /* Prevent C parser from accepting things like "-inf" on its own... */
    for(n = 0; n < len; ++n) {
      if (C_strchr("+-0123456789e.", *(str+n)) == NULL)
        return 0;
    }
  }

  if(C_strpbrk(str, "xX\0") != NULL) return 0;

  errno = 0;
  n = C_strtow(str, &eptr, radix);
  
  if(((n == C_LONG_MAX || n == C_LONG_MIN) && errno == ERANGE) || *eptr != '\0') {
    if(radix != 10)
      return from_n_nary(str, radix, flo) ? 2 : 0;

    errno = 0;
    fn = C_strtod(str, &eptr2);

    if(fn == HUGE_VAL && errno == ERANGE) return 0;
    else if(eptr2 == str) return 0;
    else if(*eptr2 == '\0' || (eptr != eptr2 && !C_strncmp(eptr2, ".0", C_strlen(eptr2)))) {
      *flo = fn;
      return 2;
    }

    return 0;
  }
  else if((n & C_INT_SIGN_BIT) != ((n << 1) & C_INT_SIGN_BIT)) { /* doesn't fit into fixnum? */
    if(*eptr == '\0' || !C_strncmp(eptr, ".0", C_strlen(eptr))) {
      *flo = (double)n;
      return 2;
    }
    else return 0;
  }
  else {
    *fix = n;
    return 1;
  }
}


static char *to_n_nary(C_uword num, C_uword base, int negp, int as_flonum)
{
  static char *digits = "0123456789abcdef";
  char *p;
  C_uword shift = C_ilen(base) - 1;
  int mask = (1 << shift) - 1;
  if (as_flonum) {
    buffer[68] = '\0';
    buffer[67] = '0';
    buffer[66] = '.';
  } else {
    buffer[66] = '\0';
  }
  p = buffer + 66;
  if (mask == base - 1) {
    do {
      *(--p) = digits [ num & mask ];
      num >>= shift;
    } while (num);
  } else {
    do {
      *(--p) = digits [ num % base ];
      num /= base;
    } while (num);
  }
  if (negp) *(--p) = '-';
  return p;
}


void C_ccall C_number_to_string(C_word c, C_word closure, C_word k, C_word num, ...)
{
  C_word radix;

  if(c == 3) {
    radix = C_fix(10);
  } else if(c == 4) {
    va_list v;

    va_start(v, num);
    radix = va_arg(v, C_word);
    va_end(v);
    
    if(!(radix & C_FIXNUM_BIT))
      barf(C_BAD_ARGUMENT_TYPE_BAD_BASE_ERROR, "number->string", radix);
  } else {
    C_bad_argc(c, 3);
  }

  if(num & C_FIXNUM_BIT) {
    C_fixnum_to_string(4, (C_word)NULL, k, num, radix);
  } else if (C_immediatep(num)) {
    barf(C_BAD_ARGUMENT_TYPE_ERROR, "number->string", num);
  } else if(C_block_header(num) == C_FLONUM_TAG) {
    C_flonum_to_string(4, (C_word)NULL, k, num, radix);
  } else if (C_header_bits(num) == C_BIGNUM_TYPE) {
    C_integer_to_string(4, (C_word)NULL, k, num, radix);
  } else {
    try_extended_number("\003sysextended-number->string", 3, k, num, radix);
  }
}

void C_ccall 
C_fixnum_to_string(C_word c, C_word self, C_word k, C_word num, C_word radix)
{
  C_char *p;
  C_word *a, neg = num & C_INT_SIGN_BIT ? 1 : 0;

  radix = C_unfix(radix);
  if (radix < 2 || radix > 16) {
    barf(C_BAD_ARGUMENT_TYPE_BAD_BASE_ERROR, "number->string", C_fix(radix));
  }

  num = neg ? -C_unfix(num) : C_unfix(num);
  p = to_n_nary(num, radix, neg, 0);

  num = C_strlen(p);
  a = C_alloc((C_bytestowords(num) + 1));
  C_kontinue(k, C_string(&a, num, p));
}

void C_ccall
C_flonum_to_string(C_word c, C_word self, C_word k, C_word num, C_word radix)
{
  C_word *a;
  C_char *p;
  double f;

  radix = C_unfix(radix);
  f = C_flonum_magnitude(num);

  /* XXX TODO: Should inexacts be printable in other bases than 10?
   * Perhaps output a string starting with #i?
   * Right now something like (number->string 1e40 16) results in
   * a string that can't be read back using string->number.
   */
  if((radix < 2) || (radix > 16)){
    barf(C_BAD_ARGUMENT_TYPE_BAD_BASE_ERROR, "number->string", C_fix(radix));
  }

  if(C_fits_in_unsigned_int_p(num) == C_SCHEME_TRUE) { /* Use fast int code */
    if(f < 0) {
      p = to_n_nary((C_uword)-f, radix, 1, 1);
    } else {
      p = to_n_nary((C_uword)f, radix, 0, 1);
    }
  } else if(C_isnan(f)) {
    p = "+nan.0";
  } else if(C_isinf(f)) {
    p = f > 0 ? "+inf.0" : "-inf.0";
  } else { /* Doesn't fit an unsigned int and not "special"; use system libc */
    C_snprintf(buffer, STRING_BUFFER_SIZE, C_text("%.*g"),
               /* XXX: flonum_print_precision */
               (int)C_unfix(C_get_print_precision()), f);
    buffer[STRING_BUFFER_SIZE-1] = '\0';

    if((p = C_strpbrk(buffer, C_text(".eE"))) == NULL) {
      /* Already checked for these, so shouldn't happen */
      assert(*buffer != 'i'); /* "inf" */
      assert(*buffer != 'n'); /* "nan" */
      /* Ensure integral flonums w/o expt are always terminated by .0 */
#if defined(HAVE_STRLCAT) || !defined(C_strcat)
      C_strlcat(buffer, C_text(".0"), sizeof(buffer));
#else
      C_strcat(buffer, C_text(".0"));
#endif
    }
    p = buffer;
  }

  radix = C_strlen(p);
  a = C_alloc((C_bytestowords(radix) + 1));
  radix = C_string(&a, radix, p);
  C_kontinue(k, radix);
}

void C_ccall
C_integer_to_string(C_word c, C_word self, C_word k, C_word num, C_word radix)
{
  if (num & C_FIXNUM_BIT) {
    C_fixnum_to_string(4, (C_word)NULL, k, num, radix);
  } else {
    int len, radix_shift;
    size_t nbits;

    if ((C_unfix(radix) < 2) || (C_unfix(radix) > 16)) {
      barf(C_BAD_ARGUMENT_TYPE_BAD_BASE_ERROR, "number->string", radix);
    }

    /* Approximation of the number of radix digits we'll need.  We try
     * to be as precise as possible to avoid memmove overhead at the end
     * of the non-powers of two part of the conversion procedure, which
     * we may need to do because we write strings back-to-front, and
     * pointers must be aligned (even for byte blocks).
     */
    len = C_bignum_size(num)-1;

    nbits  = (size_t)len * C_BIGNUM_DIGIT_LENGTH;
    nbits += C_ilen(C_bignum_digits(num)[len]);

    len = C_ilen(C_unfix(radix))-1;
    len = (nbits + len - 1) / len;
    len += C_bignum_negativep(num) ? 1 : 0; /* Add space for negative sign */
    
    radix_shift = C_ilen(C_unfix(radix)) - 1;
    /* TODO: Activate later */
    /* if (len > C_RECURSIVE_TO_STRING_THRESHOLD && */
    /*     /\* The power of two fast path is much faster than recursion *\/ */
    /*     ((C_uword)1 << radix_shift) != C_unfix(radix)) { */
    /*   try_extended_number("numbers#@integer->string/recursive", */
    /*                       4, k, num, radix, C_fix(len)); */
    /* } else { */
      C_word k2, negp = C_mk_bool(C_bignum_negativep(num)), *ka;
      ka = C_alloc(C_SIZEOF_CLOSURE(4));
      k2 = C_closure(&ka, 4, (C_word)bignum_to_str_2, k, num, radix);
      C_allocate_vector(6, (C_word)NULL, k2, C_fix(len),
                        /* Byte vec, no initialization, align at 8 bytes */
                        C_SCHEME_TRUE, C_SCHEME_FALSE, C_SCHEME_FALSE);
    /* } */
  }
}

static void
bignum_to_str_2(C_word c, C_word self, C_word string)
{
  static char *characters = "0123456789abcdef";
  C_word k = C_block_item(self, 1),
         bignum = C_block_item(self, 2),
         radix = C_unfix(C_block_item(self, 3));
  char *buf = C_c_string(string), *index = buf + C_header_size(string) - 1;
  int radix_shift, negp = (C_bignum_negativep(bignum) ? 1 : 0);

  radix_shift = C_ilen(radix) - 1;
  if (((C_uword)1 << radix_shift) == radix) { /* Power of two? */
    int radix_mask = radix - 1, big_digit_len = 0, radix_digit;
    C_uword *scan, *end, big_digit = 0;

    scan = C_bignum_digits(bignum);
    end = scan + C_bignum_size(bignum);

    while (scan < end) {
      /* If radix isn't an exact divisor of digit length, handle overlap */
      if (big_digit_len == 0) {
        big_digit = *scan++;
        big_digit_len = C_BIGNUM_DIGIT_LENGTH;
      } else {
        assert(index >= buf);
	radix_digit = big_digit;
        big_digit = *scan++;
	radix_digit |= (big_digit << big_digit_len) & radix_mask;
	big_digit >>= (radix_shift - big_digit_len);
        big_digit_len = C_BIGNUM_DIGIT_LENGTH - big_digit_len;
      }

      while(big_digit_len >= radix_shift && index >= buf) {
	radix_digit = big_digit & radix_mask;
        *index-- = characters[radix_digit];
	big_digit >>= radix_shift;
	big_digit_len -= radix_shift;
      }
    }

    assert(big_digit < radix);

    /* Final digit (like overlap at start of while loop) */
    if (big_digit) *index-- = characters[big_digit];

    if (negp) {
      /* Loop above might've overwritten sign position with a zero */
      if (*(index+1) == '0') *(index+1) = '-';
      else *index-- = '-';
    }

    /* Length calculation is always precise for radix powers of two. */
    assert(index == buf-1);
  } else {
    C_uword base, *start, *scan, big_digit;
    C_word working_copy;
    int steps, i;

    working_copy = allocate_tmp_bignum(C_fix(C_bignum_size(bignum)),
                                       C_mk_bool(negp), C_SCHEME_FALSE);
    bignum_digits_destructive_copy(working_copy, bignum);

    start = C_bignum_digits(working_copy);

    scan = start + C_bignum_size(bignum);
    /* Calculate the largest power of radix that fits a halfdigit:
     * steps = log10(2^halfdigit_bits), base = 10^steps
     */
    for(steps = 0, base = radix; C_fitsinbignumhalfdigitp(base); base *= radix)
      steps++;

    base /= radix; /* Back down: we overshot in the loop */

    while (scan > start) {
      big_digit = bignum_digits_destructive_scale_down(start, scan, base);

      if (*(scan-1) == 0) scan--; /* Adjust if we exhausted the highest digit */

      for(i = 0; i < steps && index >= buf; ++i) {
	C_word tmp = big_digit / radix;
        *index-- = characters[big_digit - (tmp*radix)]; /* big_digit % radix */
        big_digit = tmp;
      }
    }
    assert(index >= buf-1);
    free_tmp_bignum(working_copy);

    /* Move index onto first nonzero digit.  We're writing a bignum
       here: it can't consist of only zeroes. */
    while(*++index == '0');
  
    if (negp) *--index = '-';
  
    /* Shorten with distance between start and index. */
    if (buf != index) {
      i = C_header_size(string) - (index - buf);
      C_memmove(buf, index, i); /* Move start of number to beginning. */
      C_block_header(string) = C_STRING_TYPE | i; /* Mutate strlength. */
    }
  }

  C_kontinue(k, string);
}

void C_ccall C_make_structure(C_word c, C_word closure, C_word k, C_word type, ...)
{
  va_list v;
  int i;

  va_start(v, type);

  for(i = c - 3; i--; C_save(va_arg(v, C_word)));

  va_end(v);
  C_save(type);
  C_save(k);

  if(!C_demand(c - 1)) 
    C_reclaim((void *)make_structure_2, NULL);

  make_structure_2(NULL);
}


void make_structure_2(void *dummy)
{
  C_word k = C_restore,
      type = C_restore,
      size = C_rest_count(0),
      *a = C_alloc(C_SIZEOF_STRUCTURE(size+1)),
      *s = a,
      s0 = (C_word)s;

  *(s++) = C_STRUCTURE_TYPE | (size + 1);
  *(s++) = type;
  s += size;

  while(size--)
    *(--s) = C_restore;

  C_kontinue(k, s0);
}


void C_ccall C_make_symbol(C_word c, C_word closure, C_word k, C_word name)
{
  C_word ab[ C_SIZEOF_SYMBOL ], *a = ab,
         s0 = (C_word)a;

  *(a++) = C_SYMBOL_TYPE | (C_SIZEOF_SYMBOL - 1);
  *(a++) = C_SCHEME_UNBOUND;
  *(a++) = name;
  *a = C_SCHEME_END_OF_LIST;
  C_kontinue(k, s0);
}


void C_ccall C_make_pointer(C_word c, C_word closure, C_word k)
{
  C_word ab[ 2 ], *a = ab,
         p;

  p = C_mpointer(&a, NULL);
  C_kontinue(k, p);
}


void C_ccall C_make_tagged_pointer(C_word c, C_word closure, C_word k, C_word tag)
{
  C_word ab[ 3 ], *a = ab,
         p;

  p = C_taggedmpointer(&a, tag, NULL);
  C_kontinue(k, p);
}


void C_ccall C_ensure_heap_reserve(C_word c, C_word closure, C_word k, C_word n)
{
  C_save(k);

  if(!C_demand(C_bytestowords(C_unfix(n))))
    C_reclaim((void *)generic_trampoline, NULL);

  generic_trampoline(NULL);
}


void generic_trampoline(void *dummy)
{
  C_word k = C_restore;

  C_kontinue(k, C_SCHEME_UNDEFINED);
}


void C_ccall C_return_to_host(C_word c, C_word closure, C_word k)
{
  return_to_host = 1;
  C_save(k);
  C_reclaim((void *)generic_trampoline, NULL);
}


void C_ccall C_get_symbol_table_info(C_word c, C_word closure, C_word k)
{
  double d1, d2;
  int n = 0, total;
  C_SYMBOL_TABLE *stp;
  C_word x, y,
         ab[ WORDS_PER_FLONUM * 2 + C_SIZEOF_VECTOR(4) ],
         *a = ab;

  for(stp = symbol_table_list; stp != NULL; stp = stp->next)
    ++n;
  
  d1 = compute_symbol_table_load(&d2, &total);
  x = C_flonum(&a, d1);		/* load */
  y = C_flonum(&a, d2);		/* avg bucket length */
  C_kontinue(k, C_vector(&a, 4, x, y, C_fix(total), C_fix(n)));
}


void C_ccall C_get_memory_info(C_word c, C_word closure, C_word k)
{
  C_word ab[ C_SIZEOF_VECTOR(2) ], *a = ab;

  C_kontinue(k, C_vector(&a, 2, C_fix(heap_size), C_fix(stack_size)));
}


void C_ccall C_context_switch(C_word c, C_word closure, C_word k, C_word state)
{
  C_word n = C_header_size(state) - 1,
    adrs = C_block_item(state, 0);
  TRAMPOLINE trampoline;

  C_temporary_stack = C_temporary_stack_bottom - n;
  C_memcpy(C_temporary_stack, (C_word *)state + 2, n * sizeof(C_word));
  trampoline = (TRAMPOLINE)C_block_item(adrs,0);
  trampoline((void *)C_block_item(adrs,1));
}


void C_ccall C_peek_signed_integer(C_word c, C_word closure, C_word k, C_word v, C_word index)
{
  C_word x = C_block_item(v, C_unfix(index));
  C_alloc_flonum;

  if((x & C_INT_SIGN_BIT) != ((x << 1) & C_INT_SIGN_BIT)) {
    C_kontinue_flonum(k, (double)x);
  }

  C_kontinue(k, C_fix(x));
}


void C_ccall C_peek_unsigned_integer(C_word c, C_word closure, C_word k, C_word v, C_word index)
{
  C_word x = C_block_item(v, C_unfix(index));
  C_alloc_flonum;

  if((x & C_INT_SIGN_BIT) || ((x << 1) & C_INT_SIGN_BIT)) {
    C_kontinue_flonum(k, (double)(C_uword)x);
  }

  C_kontinue(k, C_fix(x));
}


void C_ccall C_decode_seconds(C_word c, C_word closure, C_word k, C_word secs, C_word mode)
{
  time_t tsecs;
  struct tm *tmt;
  C_word ab[ C_SIZEOF_VECTOR(10) ], *a = ab,
         info;

  tsecs = (time_t)((secs & C_FIXNUM_BIT) != 0 ? C_unfix(secs) : C_flonum_magnitude(secs));
  
  if(mode == C_SCHEME_FALSE) tmt = C_localtime(&tsecs);
  else tmt = C_gmtime(&tsecs);

  if(tmt  == NULL)
    C_kontinue(k, C_SCHEME_FALSE);
  
  info = C_vector(&a, 10, C_fix(tmt->tm_sec), C_fix(tmt->tm_min), C_fix(tmt->tm_hour),
		  C_fix(tmt->tm_mday), C_fix(tmt->tm_mon), C_fix(tmt->tm_year),
		  C_fix(tmt->tm_wday), C_fix(tmt->tm_yday),
		  tmt->tm_isdst > 0 ? C_SCHEME_TRUE : C_SCHEME_FALSE,
#ifdef C_GNU_ENV
                  /* negative for west of UTC, but we want positive */
		  C_fix(-tmt->tm_gmtoff)
#elif defined(__CYGWIN__) || defined(__MINGW32__) || defined(_WIN32) || defined(__WINNT__)
                  C_fix(mode == C_SCHEME_FALSE ? _timezone : 0) /* does not account for DST */
#else
                  C_fix(mode == C_SCHEME_FALSE ? timezone : 0)  /* does not account for DST */
#endif
		  );
  C_kontinue(k, info);
}


void C_ccall C_machine_byte_order(C_word c, C_word closure, C_word k)
{
  char *str;
  C_word *a, s;

  if(c != 2) C_bad_argc(c, 2);

#if defined(C_MACHINE_BYTE_ORDER)
  str = C_MACHINE_BYTE_ORDER;
#else
  C_cblock
    static C_word one_two_three = 123;
    str = (*((C_char *)&one_two_three) != 123) ? "big-endian" : "little-endian";
  C_cblockend;
#endif

  a = C_alloc(2 + C_bytestowords(strlen(str)));
  s = C_string2(&a, str);

  C_kontinue(k, s);
}


void C_ccall C_machine_type(C_word c, C_word closure, C_word k)
{
  C_word *a, s;

  if(c != 2) C_bad_argc(c, 2);

  a = C_alloc(2 + C_bytestowords(strlen(C_MACHINE_TYPE)));
  s = C_string2(&a, C_MACHINE_TYPE);
  
  C_kontinue(k, s);
}


void C_ccall C_software_type(C_word c, C_word closure, C_word k)
{
  C_word *a, s;

  if(c != 2) C_bad_argc(c, 2);

  a = C_alloc(2 + C_bytestowords(strlen(C_SOFTWARE_TYPE)));
  s = C_string2(&a, C_SOFTWARE_TYPE);

 C_kontinue(k, s);
}


void C_ccall C_build_platform(C_word c, C_word closure, C_word k)
{
  C_word *a, s;

  if(c != 2) C_bad_argc(c, 2);

  a = C_alloc(2 + C_bytestowords(strlen(C_BUILD_PLATFORM)));
  s = C_string2(&a, C_BUILD_PLATFORM);

 C_kontinue(k, s);
}


void C_ccall C_software_version(C_word c, C_word closure, C_word k)
{
  C_word *a, s;

  if(c != 2) C_bad_argc(c, 2);

  a = C_alloc(2 + C_bytestowords(strlen(C_SOFTWARE_VERSION)));
  s = C_string2(&a, C_SOFTWARE_VERSION);

 C_kontinue(k, s);
}


/* Register finalizer: */

void C_ccall C_register_finalizer(C_word c, C_word closure, C_word k, C_word x, C_word proc)
{
  if(C_immediatep(x) || (!C_in_stackp(x) && !C_in_heapp(x))) /* not GCable? */
    C_kontinue(k, x);

  C_do_register_finalizer(x, proc);
  C_kontinue(k, x);
}


void C_ccall C_do_register_finalizer(C_word x, C_word proc)
{
  C_word *ptr;
  int n, i;
  FINALIZER_NODE *flist;

  if(finalizer_free_list == NULL) {
    if((flist = (FINALIZER_NODE *)C_malloc(sizeof(FINALIZER_NODE))) == NULL)
      panic(C_text("out of memory - cannot allocate finalizer node"));

    ++allocated_finalizer_count;
  }
  else {
    flist = finalizer_free_list;
    finalizer_free_list = flist->next;
  }

  if(finalizer_list != NULL) finalizer_list->previous = flist;

  flist->previous = NULL;
  flist->next = finalizer_list;
  finalizer_list = flist;

  if(C_in_stackp(x)) C_mutate_slot(&flist->item, x);
  else flist->item = x;

  if(C_in_stackp(proc)) C_mutate_slot(&flist->finalizer, proc);
  else flist->finalizer = proc;

  ++live_finalizer_count;
}


int C_do_unregister_finalizer(C_word x)
{
  int n;
  FINALIZER_NODE *flist;

  for(flist = finalizer_list; flist != NULL; flist = flist->next) {
    if(flist->item == x) {
      if(flist->previous == NULL) finalizer_list = flist->next;
      else flist->previous->next = flist->next;

      return 1;
    }
  }

  return 0;
}


/* Dynamic loading of shared objects: */

void C_ccall C_set_dlopen_flags(C_word c, C_word closure, C_word k, C_word now, C_word global)
{
#if !defined(NO_DLOAD2) && defined(HAVE_DLFCN_H)
  dlopen_flags = (C_truep(now) ? RTLD_NOW : RTLD_LAZY) | (C_truep(global) ? RTLD_GLOBAL : RTLD_LOCAL);
#endif
  C_kontinue(k, C_SCHEME_UNDEFINED);
}


void C_ccall C_dload(C_word c, C_word closure, C_word k, C_word name, C_word entry)
{
#if !defined(NO_DLOAD2) && (defined(HAVE_DLFCN_H) || defined(HAVE_DL_H) || (defined(HAVE_LOADLIBRARY) && defined(HAVE_GETPROCADDRESS)))
  /* Force minor GC: otherwise the lf may contain pointers to stack-data
     (stack allocated interned symbols, for example) */
  C_save_and_reclaim((void *)dload_2, NULL, 3, k, name, entry);
#endif

  C_kontinue(k, C_SCHEME_FALSE);
}


#ifdef DLOAD_2_DEFINED
# undef DLOAD_2_DEFINED
#endif

#if !defined(NO_DLOAD2) && defined(HAVE_DL_H) && !defined(DLOAD_2_DEFINED)
# ifdef __hpux__
#  define DLOAD_2_DEFINED
void dload_2(void *dummy)
{
  void *handle, *p;
  C_word entry = C_restore,
         name = C_restore,
         k = C_restore;
  C_char *mname = (C_char *)C_data_pointer(name);

  /*
   * C_fprintf(C_stderr,
   *   "shl_loading %s : %s\n",
   *   (char *) C_data_pointer(name),
   *   (char *) C_data_pointer(entry));
   */

  if ((handle = (void *) shl_load(mname,
				  BIND_IMMEDIATE | DYNAMIC_PATH,
				  0L)) != NULL) {
    shl_t shl_handle = (shl_t) handle;

    /*** This version does not check for C_dynamic_and_unsafe. Fix it. */
    if (shl_findsym(&shl_handle, (char *) C_data_pointer(entry), TYPE_PROCEDURE, &p) == 0) {
      current_module_name = C_strdup(mname);
      current_module_handle = handle;

      if(debug_mode) {
	C_dbg(C_text("debug"), C_text("loading compiled module `%s' (handle is " UWORD_FORMAT_STRING ")\n"),
	      current_module_name, (C_uword)current_module_handle);
      }

      ((C_proc2)p)(2, C_SCHEME_UNDEFINED, k);
    } else {
      C_dlerror = (char *) C_strerror(errno);
      shl_unload(shl_handle);
    }
  } else {
    C_dlerror = (char *) C_strerror(errno);
  }

  C_kontinue(k, C_SCHEME_FALSE);
}
# endif
#endif


#if !defined(NO_DLOAD2) && defined(HAVE_DLFCN_H) && !defined(DLOAD_2_DEFINED)
# ifndef __hpux__
#  define DLOAD_2_DEFINED
void dload_2(void *dummy)
{
  void *handle, *p, *p2;
  C_word 
    entry = C_restore,
    name = C_restore,
    k = C_restore;
  C_char *topname = (C_char *)C_data_pointer(entry);
  C_char *mname = (C_char *)C_data_pointer(name);
  C_char *tmp;
  int tmp_len = 0;

  if((handle = C_dlopen(mname, dlopen_flags)) != NULL) {
    if((p = C_dlsym(handle, topname)) == NULL) {
      tmp_len = C_strlen(topname) + 2;
      tmp = (C_char *)C_malloc(tmp_len);

      if(tmp == NULL)
	panic(C_text("out of memory - cannot allocate toplevel name string"));

      C_strlcpy(tmp, C_text("_"), tmp_len);
      C_strlcat(tmp, topname, tmp_len);
      p = C_dlsym(handle, tmp);
      C_free(tmp);
    }

    if(p != NULL) {
      current_module_name = C_strdup(mname);
      current_module_handle = handle;

      if(debug_mode) {
	C_dbg(C_text("debug"), C_text("loading compiled module `%s' (handle is " UWORD_FORMAT_STRING ")\n"),
	      current_module_name, (C_uword)current_module_handle);
      }

      ((C_proc2)p)(2, C_SCHEME_UNDEFINED, k); /* doesn't return */
    }

    C_dlclose(handle);
  }
  
  C_dlerror = (char *)dlerror();
  C_kontinue(k, C_SCHEME_FALSE);
}
# endif
#endif


#if !defined(NO_DLOAD2) && (defined(HAVE_LOADLIBRARY) && defined(HAVE_GETPROCADDRESS)) && !defined(DLOAD_2_DEFINED)
# define DLOAD_2_DEFINED
void dload_2(void *dummy)
{
  HINSTANCE handle;
  FARPROC p = NULL, p2;
  C_word
    entry = C_restore,
    name = C_restore,
    k = C_restore;
  C_char *topname = (C_char *)C_data_pointer(entry);
  C_char *mname = (C_char *)C_data_pointer(name);

  /* cannot use LoadLibrary on non-DLLs, so we use extension checking */
  if (C_header_size(name) >= 5) {
    char *n = (char*) C_data_pointer(name);
    int l = C_header_size(name);
    if (C_strncasecmp(".dll", n+l-5, 4) && 
	C_strncasecmp(".so", n+l-4, 3))
      C_kontinue(k, C_SCHEME_FALSE);
  }

  if((handle = LoadLibrary(mname)) != NULL) {
    if ((p = GetProcAddress(handle, topname)) != NULL) {
      current_module_name = C_strdup(mname);
      current_module_handle = handle;

      if(debug_mode) {
	C_dbg(C_text("debug"), C_text("loading compiled module `%s' (handle is " UWORD_FORMAT_STRING ")\n"),
	      current_module_name, (C_uword)current_module_handle);
      }

      ((C_proc2)p)(2, C_SCHEME_UNDEFINED, k);
    }
    else FreeLibrary(handle);
  }

  C_dlerror = (char *) C_strerror(errno);
  C_kontinue(k, C_SCHEME_FALSE);
}
#endif


void C_ccall C_become(C_word c, C_word closure, C_word k, C_word table)
{
  C_word tp, x, old, neu, i, *p;

  i = forwarding_table_size;
  p = forwarding_table;

  for(tp = table; tp != C_SCHEME_END_OF_LIST; tp = C_u_i_cdr(tp)) {
    x = C_u_i_car(tp);
    old = C_u_i_car(x);
    neu = C_u_i_cdr(x);

    if(i == 0) {
      if((forwarding_table = (C_word *)realloc(forwarding_table, (forwarding_table_size + 1) * 4 * sizeof(C_word))) == NULL)
	panic(C_text("out of memory - cannot re-allocate forwarding table"));
	
      i = forwarding_table_size;
      p = forwarding_table + forwarding_table_size * 2;
      forwarding_table_size *= 2;
    }

    *(p++) = old;
    *(p++) = neu;
    --i;
  }

  *p = 0;
  C_fromspace_top = C_fromspace_limit;
  C_save_and_reclaim((void *)become_2, NULL, 1, k);
}


void become_2(void *dummy)
{
  C_word k = C_restore;
  *forwarding_table = 0;
  C_kontinue(k, C_SCHEME_UNDEFINED);
}


C_regparm C_word C_fcall
C_a_i_cpu_time(C_word **a, int c, C_word buf)
{
  C_word u, s = C_fix(0);

#if defined(C_NONUNIX) || defined(__CYGWIN__)
  if(CLOCKS_PER_SEC == 1000) u = clock();
  else u = C_number(a, C_floor(((double)clock() / (double)CLOCKS_PER_SEC) * 1000));
#else
  struct rusage ru;

  if(C_getrusage(RUSAGE_SELF, &ru) == -1) u = 0;
  else {
    u = C_number(a, C_floor((double)ru.ru_utime.tv_sec * 1000 + ru.ru_utime.tv_usec / 1000));
    s = C_number(a, C_floor((double)ru.ru_stime.tv_sec * 1000 + ru.ru_stime.tv_usec / 1000));
  }
#endif

  /* buf must not be in nursery */
  C_set_block_item(buf, 0, u);
  C_set_block_item(buf, 1, s);
  return buf;
}


C_regparm C_word C_fcall C_a_i_make_locative(C_word **a, int c, C_word type, C_word object, C_word index, C_word weak)
{
  C_word *loc = *a;
  int offset, i, in = C_unfix(index);
  *a = loc + C_SIZEOF_LOCATIVE;

  loc[ 0 ] = C_LOCATIVE_TAG;

  switch(C_unfix(type)) {
  case C_SLOT_LOCATIVE: in *= sizeof(C_word); break;
  case C_U16_LOCATIVE:
  case C_S16_LOCATIVE: in *= 2; break;
  case C_U32_LOCATIVE:
  case C_F32_LOCATIVE:
  case C_S32_LOCATIVE: in *= 4; break;
  case C_F64_LOCATIVE: in *= 8; break;
  }

  offset = in + sizeof(C_header);
  loc[ 1 ] = object + offset;
  loc[ 2 ] = C_fix(offset);
  loc[ 3 ] = type;
  loc[ 4 ] = C_truep(weak) ? C_SCHEME_FALSE : object;

  for(i = 0; i < locative_table_count; ++i)
    if(locative_table[ i ] == C_SCHEME_UNDEFINED) {
      locative_table[ i ] = (C_word)loc;
      return (C_word)loc;
    }

  if(locative_table_count >= locative_table_size) {
    if(debug_mode == 2)
      C_dbg(C_text("debug"), C_text("resizing locative table from %d to %d (count is %d)\n"), 
	    locative_table_size, locative_table_size * 2, locative_table_count);

    locative_table = (C_word *)C_realloc(locative_table, locative_table_size * 2 * sizeof(C_word));

    if(locative_table == NULL) 
      panic(C_text("out of memory - cannot resize locative table"));

    locative_table_size *= 2;
  }

  locative_table[ locative_table_count++ ] = (C_word)loc;
  return (C_word)loc;
}


void C_ccall C_locative_ref(C_word c, C_word closure, C_word k, C_word loc)
{
  C_word *ptr, val;
  C_alloc_flonum;

  if(c != 3) C_bad_argc(c, 3);

  if(C_immediatep(loc) || C_block_header(loc) != C_LOCATIVE_TAG)
    barf(C_BAD_ARGUMENT_TYPE_ERROR, "locative-set!", loc);

  ptr = (C_word *)C_block_item(loc, 0);

  if(ptr == NULL) barf(C_LOST_LOCATIVE_ERROR, "locative-ref", loc);

  switch(C_unfix(C_block_item(loc, 2))) {
  case C_SLOT_LOCATIVE: C_kontinue(k, *ptr);
  case C_CHAR_LOCATIVE: C_kontinue(k, C_make_character(*((char *)ptr)));
  case C_U8_LOCATIVE: C_kontinue(k, C_fix(*((unsigned char *)ptr)));
  case C_S8_LOCATIVE: C_kontinue(k, C_fix(*((char *)ptr)));
  case C_U16_LOCATIVE: C_kontinue(k, C_fix(*((unsigned short *)ptr)));
  case C_S16_LOCATIVE: C_kontinue(k, C_fix(*((short *)ptr)));
  case C_U32_LOCATIVE: C_peek_unsigned_integer(0, 0, k, (C_word)(ptr - 1), 0);
  case C_S32_LOCATIVE: C_peek_signed_integer(0, 0, k, (C_word)(ptr - 1), 0);
  case C_F32_LOCATIVE: C_kontinue_flonum(k, *((float *)ptr));
  case C_F64_LOCATIVE: C_kontinue_flonum(k, *((double *)ptr));
  default: panic(C_text("bad locative type"));
  }
}


C_regparm C_word C_fcall C_i_locative_set(C_word loc, C_word x)
{
  C_word *ptr, val;

  if(C_immediatep(loc) || C_block_header(loc) != C_LOCATIVE_TAG)
    barf(C_BAD_ARGUMENT_TYPE_ERROR, "locative-set!", loc);

  ptr = (C_word *)C_block_item(loc, 0);

  if(ptr == NULL)
    barf(C_LOST_LOCATIVE_ERROR, "locative-set!", loc);

  switch(C_unfix(C_block_item(loc, 2))) {
  case C_SLOT_LOCATIVE: C_mutate2(ptr, x); break;

  case C_CHAR_LOCATIVE:
    if((x & C_IMMEDIATE_TYPE_BITS) != C_CHARACTER_BITS)
      barf(C_BAD_ARGUMENT_TYPE_ERROR, "locative-set!", x);
      
    *((char *)ptr) = C_character_code(x); 
    break;

  case C_U8_LOCATIVE: 
    if((x & C_FIXNUM_BIT) == 0)
      barf(C_BAD_ARGUMENT_TYPE_ERROR, "locative-set!", x);

    *((unsigned char *)ptr) = C_unfix(x); 
    break;

  case C_S8_LOCATIVE: 
    if((x & C_FIXNUM_BIT) == 0)
      barf(C_BAD_ARGUMENT_TYPE_ERROR, "locative-set!", x);
    
    *((char *)ptr) = C_unfix(x); 
    break;

  case C_U16_LOCATIVE: 
    if((x & C_FIXNUM_BIT) == 0)
      barf(C_BAD_ARGUMENT_TYPE_ERROR, "locative-set!", x);

    *((unsigned short *)ptr) = C_unfix(x); 
    break;

  case C_S16_LOCATIVE: 
    if((x & C_FIXNUM_BIT) == 0)
      barf(C_BAD_ARGUMENT_TYPE_ERROR, "locative-set!", x);

    *((short *)ptr) = C_unfix(x); 
    break;

  case C_U32_LOCATIVE: 
    if((x & C_FIXNUM_BIT) == 0 && (C_immediatep(x) || C_block_header(x) != C_FLONUM_TAG))
      barf(C_BAD_ARGUMENT_TYPE_ERROR, "locative-set!", x);

    *((C_u32 *)ptr) = C_num_to_unsigned_int(x); 
    break;

  case C_S32_LOCATIVE: 
    if((x & C_FIXNUM_BIT) == 0 && (C_immediatep(x) || C_block_header(x) != C_FLONUM_TAG))
      barf(C_BAD_ARGUMENT_TYPE_ERROR, "locative-set!", x);
    
    *((C_s32 *)ptr) = C_num_to_int(x); 
    break;

  case C_F32_LOCATIVE: 
    if(C_immediatep(x) || C_block_header(x) != C_FLONUM_TAG)
      barf(C_BAD_ARGUMENT_TYPE_ERROR, "locative-set!", x);
    
    *((float *)ptr) = C_flonum_magnitude(x); 
    break;

  case C_F64_LOCATIVE: 
    if(C_immediatep(x) || C_block_header(x) != C_FLONUM_TAG)
      barf(C_BAD_ARGUMENT_TYPE_ERROR, "locative-set!", x);
    
    *((double *)ptr) = C_flonum_magnitude(x); 
    break;

  default: panic(C_text("bad locative type"));
  }

  return C_SCHEME_UNDEFINED;
}


C_regparm C_word C_fcall C_i_locative_to_object(C_word loc)
{
  C_word *ptr;

  if(C_immediatep(loc) || C_block_header(loc) != C_LOCATIVE_TAG)
    barf(C_BAD_ARGUMENT_TYPE_ERROR, "locative->object", loc);

  ptr = (C_word *)C_block_item(loc, 0);

  if(ptr == NULL) return C_SCHEME_FALSE;
  else return (C_word)ptr - C_unfix(C_block_item(loc, 1));
}


/* GC protection of user-variables: */

C_regparm void C_fcall C_gc_protect(C_word **addr, int n)
{
  int k;

  if(collectibles_top + n >= collectibles_limit) {
    k = collectibles_limit - collectibles;
    collectibles = (C_word **)C_realloc(collectibles, sizeof(C_word *) * k * 2);

    if(collectibles == NULL)
      panic(C_text("out of memory - cannot allocate GC protection vector"));
    
    collectibles_top = collectibles + k;
    collectibles_limit = collectibles + k * 2;
  }

  C_memcpy(collectibles_top, addr, n * sizeof(C_word *));
  collectibles_top += n;
}


C_regparm void C_fcall C_gc_unprotect(int n)
{
  collectibles_top -= n;
}


/* Map procedure-ptr to id or id to ptr: */

C_char *C_lookup_procedure_id(void *ptr)
{
  LF_LIST *lfl;
  C_PTABLE_ENTRY *pt;

  for(lfl = lf_list; lfl != NULL; lfl = lfl->next) {
    pt = lfl->ptable;

    if(pt != NULL) {
      while(pt->id != NULL) {
	if(pt->ptr == ptr) return pt->id;
	else ++pt;
      }
    }
  }

  return NULL;
}


void *C_lookup_procedure_ptr(C_char *id)
{
  LF_LIST *lfl;
  C_PTABLE_ENTRY *pt;

  for(lfl = lf_list; lfl != NULL; lfl = lfl->next) {
    pt = lfl->ptable;

    if(pt != NULL) {
      while(pt->id != NULL) {
	if(!C_strcmp(id, pt->id)) return pt->ptr;
	else ++pt;
      }
    }
  }

  return NULL;
}


void C_ccall C_copy_closure(C_word c, C_word closure, C_word k, C_word proc)
{
  int n = C_header_size(proc);

  if(!C_demand(n + 1)) C_save_and_reclaim((void *)copy_closure_2, NULL, 2, proc, k);
  else {
    C_save(proc);
    C_save(k);
    copy_closure_2(NULL);
  }
}


static void copy_closure_2(void *dummy)
{
  C_word 
    k = C_restore,
    proc = C_restore;
  int cells = C_header_size(proc);
  C_word
    *ptr = C_alloc(C_SIZEOF_CLOSURE(cells)),
    *p = ptr;

  *(p++) = C_CLOSURE_TYPE | cells;
  /* this is only allowed because the storage is freshly allocated: */
  C_memcpy_slots(p, C_data_pointer(proc), cells);
  C_kontinue(k, (C_word)ptr);
}


/* Creating black holes: */

void C_call_with_cthulhu(C_word c, C_word self, C_word k, C_word proc)
{
  C_word *a = C_alloc(C_SIZEOF_CLOSURE(1));
  
  k = C_closure(&a, 1, (C_word)termination_continuation);
  C_apply(4, C_SCHEME_UNDEFINED, k, proc, C_SCHEME_END_OF_LIST);
}


/* fixnum arithmetic with overflow detection (from "Hacker's Delight" by Hank Warren) 
   These routines return #f if the operation failed due to overflow. 
 */

C_regparm C_word C_fcall C_i_o_fixnum_plus(C_word n1, C_word n2)
{
  C_word x1, x2, s;
  
  if((n1 & C_FIXNUM_BIT) == 0 || (n2 & C_FIXNUM_BIT) == 0) return C_SCHEME_FALSE;

  x1 = C_unfix(n1);
  x2 = C_unfix(n2);
  s = x1 + x2;

#ifdef C_SIXTY_FOUR
  if((((s ^ x1) & (s ^ x2)) >> 62) != 0) return C_SCHEME_FALSE;
#else
  if((((s ^ x1) & (s ^ x2)) >> 30) != 0) return C_SCHEME_FALSE;
#endif
  else return C_fix(s);
}


C_regparm C_word C_fcall C_i_o_fixnum_difference(C_word n1, C_word n2)
{
  C_word x1, x2, s;

  if((n1 & C_FIXNUM_BIT) == 0 || (n2 & C_FIXNUM_BIT) == 0) return C_SCHEME_FALSE;

  x1 = C_unfix(n1);
  x2 = C_unfix(n2);
  s = x1 - x2;
  
#ifdef C_SIXTY_FOUR
  if((((s ^ x1) & ~(s ^ x2)) >> 62) != 0) return C_SCHEME_FALSE;
#else
  if((((s ^ x1) & ~(s ^ x2)) >> 30) != 0) return C_SCHEME_FALSE;
#endif
  else return C_fix(s);
}


C_regparm C_word C_fcall C_i_o_fixnum_times(C_word n1, C_word n2)
{
  C_word x1, x2;
  C_uword x1u, x2u;
#ifdef C_SIXTY_FOUR
# ifdef C_LLP
  C_uword c = 1ULL<<63ULL;
# else
  C_uword c = 1UL<<63UL;
# endif
#else
  C_uword c = 1UL<<31UL;
#endif

  if((n1 & C_INT_SIGN_BIT) == (n2 & C_INT_SIGN_BIT)) --c;

  x1 = C_unfix(n1);
  x2 = C_unfix(n2);
  x1u = x1 < 0 ? -x1 : x1;
  x2u = x2 < 0 ? -x2 : x2;

  if(x2u != 0 && x1u > (c / x2u)) return C_SCHEME_FALSE;
  
  x1 = x1 * x2;

  if(C_fitsinfixnump(x1)) return C_fix(x1);
  else return C_SCHEME_FALSE;
}


C_regparm C_word C_fcall C_i_o_fixnum_quotient(C_word n1, C_word n2)
{
  C_word x1, x2;

  if((n1 & C_FIXNUM_BIT) == 0 || (n2 & C_FIXNUM_BIT) == 0) return C_SCHEME_FALSE;

  x1 = C_unfix(n1);
  x2 = C_unfix(n2);

  if(x2 == 0)
    barf(C_DIVISION_BY_ZERO_ERROR, "fx/?");

#ifdef C_SIXTY_FOUR
  if(x1 == 0x8000000000000000L && x2 == -1) return C_SCHEME_FALSE;
#else
  if(x1 == 0x80000000L && x2 == -1) return C_SCHEME_FALSE;
#endif

  x1 = x1 / x2;

  if(C_fitsinfixnump(x1)) return C_fix(x1);
  else return C_SCHEME_FALSE;
}


C_regparm C_word C_fcall C_i_o_fixnum_and(C_word n1, C_word n2)
{
  C_uword x1, x2, r;

  if((n1 & C_FIXNUM_BIT) == 0 || (n2 & C_FIXNUM_BIT) == 0) return C_SCHEME_FALSE;

  x1 = C_unfix(n1);
  x2 = C_unfix(n2);
  r = x1 & x2;
  
  if(((r & C_INT_SIGN_BIT) >> 1) != (r & C_INT_TOP_BIT)) return C_SCHEME_FALSE;
  else return C_fix(r);
}


C_regparm C_word C_fcall C_i_o_fixnum_ior(C_word n1, C_word n2)
{
  C_uword x1, x2, r;

  if((n1 & C_FIXNUM_BIT) == 0 || (n2 & C_FIXNUM_BIT) == 0) return C_SCHEME_FALSE;

  x1 = C_unfix(n1);
  x2 = C_unfix(n2);
  r = x1 | x2;
  
  if(((r & C_INT_SIGN_BIT) >> 1) != (r & C_INT_TOP_BIT)) return C_SCHEME_FALSE;
  else return C_fix(r);
}


C_regparm C_word C_fcall C_i_o_fixnum_xor(C_word n1, C_word n2)
{
  C_uword x1, x2, r;

  if((n1 & C_FIXNUM_BIT) == 0 || (n2 & C_FIXNUM_BIT) == 0) return C_SCHEME_FALSE;

  x1 = C_unfix(n1);
  x2 = C_unfix(n2);
  r = x1 ^ x2;
  
  if(((r & C_INT_SIGN_BIT) >> 1) != (r & C_INT_TOP_BIT)) return C_SCHEME_FALSE;
  else return C_fix(r);
}


/* decoding of literals in compressed format */

static C_regparm C_uword C_fcall decode_size(C_char **str)
{
  C_uchar **ustr = (C_uchar **)str;
  C_uword size = (*((*ustr)++) & 0xff) << 16; /* always big endian */

  size |= (*((*ustr)++) & 0xff) << 8;
  size |= (*((*ustr)++) & 0xff);
  return size;
}


static C_regparm C_word C_fcall decode_literal2(C_word **ptr, C_char **str,
						C_word *dest)
{
  C_ulong bits = *((*str)++) & 0xff;
  C_word *data, *dptr, val;
  C_uword size;

  /* vvv this can be taken out at a later stage (once it works reliably) vvv */
  if(bits != 0xfe)
    panic(C_text("invalid encoded literal format"));

  bits = *((*str)++) & 0xff;
  /* ^^^ */

#ifdef C_SIXTY_FOUR
  bits <<= 24 + 32;
#else
  bits <<= 24;
#endif

  if(bits == C_HEADER_BITS_MASK) {		/* special/immediate */
    switch(0xff & *((*str)++)) {
    case C_BOOLEAN_BITS: 
      return C_mk_bool(*((*str)++));

    case C_CHARACTER_BITS: 
      return C_make_character(decode_size(str));

    case C_SCHEME_END_OF_LIST:
    case C_SCHEME_UNDEFINED:
    case C_SCHEME_END_OF_FILE:
      return (C_word)(*(*str - 1));

    case C_FIXNUM_BIT:
      val = *((*str)++) << 24; /* always big endian */
      val |= (*((*str)++) & 0xff) << 16;
      val |= (*((*str)++) & 0xff) << 8;
      val |= (*((*str)++) & 0xff);
      return C_fix(val); 

#ifdef C_SIXTY_FOUR
    case (C_FLONUM_TYPE >> (24 + 32)) & 0xff:
#else
    case (C_FLONUM_TYPE >> 24) & 0xff:
#endif
      bits = C_FLONUM_TYPE;
      break;

    default: 
      panic(C_text("invalid encoded special literal"));
    }
  }

#ifndef C_SIXTY_FOUR
  if((bits & C_8ALIGN_BIT) != 0) {
    /* Align _data_ on 8-byte boundary: */
    if(C_aligned8(*ptr)) ++(*ptr);
  }
#endif

  val = (C_word)(*ptr);

  if(bits == C_FLONUM_TYPE) {
    C_word ln;
    double fn;

    switch (convert_string_to_number(*str, 10, &ln, &fn)) {
    case 0: 			/* failed */
      panic(C_text("invalid encoded numeric literal"));
      break;

    case 1:			/* fixnum */
      val = C_fix(ln);
      break;

    case 2:			/* flonum */
      val = C_flonum(ptr, fn);
      break;
    }

    while(*((*str)++) != '\0');      /* skip terminating '\0' */
    return val;
  }

  if((bits & C_SPECIALBLOCK_BIT) != 0)
    panic(C_text("literals with special bit cannot be decoded"));

  size = decode_size(str);

  switch(bits) {
  case C_STRING_TYPE:
    /* strings are always allocated statically */
    val = C_static_string(ptr, size, *str);
    *str += size;
    break;
    
  case C_BYTEVECTOR_TYPE:
    /* ... as are bytevectors (blobs) */
    val = C_static_bytevector(ptr, size, *str);
    *str += size;
    break;
    
  case C_SYMBOL_TYPE:
    if(dest == NULL) 
      panic(C_text("invalid literal symbol destination"));

    val = C_h_intern(dest, size, *str);
    *str += size;
    break;

  case C_LAMBDA_INFO_TYPE:
    /* lambda infos are always allocated statically */
    val = C_static_lambda_info(ptr, size, *str);
    *str += size;
    break;

  default:
    *((*ptr)++) = C_make_header(bits, size);
    data = *ptr;

    if((bits & C_BYTEBLOCK_BIT) != 0) {
      C_memcpy(data, *str, size);
      size = C_align(size);
      *str += size;
      *ptr = (C_word *)C_align((C_word)(*ptr) + size);
    }
    else {
      C_word *dptr = *ptr;
      *ptr += size;

      while(size--) {
	*dptr = decode_literal2(ptr, str, dptr);
	++dptr;
      }
    }
  }

  return val;
}


C_regparm C_word C_fcall
C_decode_literal(C_word **ptr, C_char *str)
{
  return decode_literal2(ptr, &str, NULL);
}


void
C_use_private_repository(C_char *path)
{
  private_repository = path == NULL ? NULL : C_strdup(path);
}


C_char *
C_private_repository_path()
{
  return private_repository;
}


C_regparm C_word C_fcall
C_i_getprop(C_word sym, C_word prop, C_word def)
{
  C_word pl = C_block_item(sym, 2);

  while(pl != C_SCHEME_END_OF_LIST) {
    if(C_block_item(pl, 0) == prop)
      return C_u_i_car(C_u_i_cdr(pl));
    else pl = C_u_i_cdr(C_u_i_cdr(pl));
  }

  return def;
}


C_regparm C_word C_fcall
C_putprop(C_word **ptr, C_word sym, C_word prop, C_word val)
{
  C_word pl = C_block_item(sym, 2);

  while(pl != C_SCHEME_END_OF_LIST) {
    if(C_block_item(pl, 0) == prop) {
      C_mutate2(&C_u_i_car(C_u_i_cdr(pl)), val);
      return val;
    }
    else pl = C_u_i_cdr(C_u_i_cdr(pl));
  }

  pl = C_a_pair(ptr, val, C_block_item(sym, 2));
  pl = C_a_pair(ptr, prop, pl);
  C_mutate_slot(&C_block_item(sym, 2), pl);
  return val;
}


C_regparm C_word C_fcall
C_i_get_keyword(C_word kw, C_word args, C_word def)
{
  while(!C_immediatep(args)) {
    if(C_block_header(args) == C_PAIR_TAG) {
      if(kw == C_u_i_car(args)) {
	args = C_u_i_cdr(args);

	if(C_immediatep(args) || C_block_header(args) != C_PAIR_TAG)
	  return def;
	else return C_u_i_car(args);
      }
      else {
	args = C_u_i_cdr(args);

	if(C_immediatep(args) || C_block_header(args) != C_PAIR_TAG)
	  return def;
	else args = C_u_i_cdr(args);
      }
    }
  }

  return def;
}


void C_ccall
C_dump_heap_state(C_word c, C_word closure, C_word k)
{
  /* make sure heap is compacted */
  C_save(k);
  C_fromspace_top = C_fromspace_limit; /* force major GC */
  C_reclaim((void *)dump_heap_state_2, NULL);
}


static C_ulong
hdump_hash(C_word key)
{
  return (C_ulong)key % HDUMP_TABLE_SIZE;
}


static void
hdump_count(C_word key, int n, int t)
{
  HDUMP_BUCKET **bp = hdump_table + hdump_hash(key);
  HDUMP_BUCKET *b = *bp;

  while(b != NULL) {
    if(b->key == key) {
      b->count += n;
      b->total += t;
      return;
    }
    else b = b->next;
  }

  b = (HDUMP_BUCKET *)C_malloc(sizeof(HDUMP_BUCKET));
  
  if(b == 0)
    panic(C_text("out of memory - can not allocate heap-dump table-bucket"));

  b->next = *bp;
  b->key = key;
  *bp = b;
  b->count = n;
  b->total = t;
}


static void 
dump_heap_state_2(void *dummy)
{
  C_word k = C_restore;
  HDUMP_BUCKET *b, *b2, **bp;
  int n, bytes;
  C_byte *scan;
  C_SCHEME_BLOCK *sbp;
  C_header h;
  C_word x, key, *p;
  int imm = 0, blk = 0;
  
  hdump_table = (HDUMP_BUCKET **)C_malloc(HDUMP_TABLE_SIZE * sizeof(HDUMP_BUCKET *));

  if(hdump_table == NULL)
    panic(C_text("out of memory - can not allocate heap-dump table"));

  C_memset(hdump_table, 0, sizeof(HDUMP_BUCKET *) * HDUMP_TABLE_SIZE);

  scan = fromspace_start;

  while(scan < C_fromspace_top) {
    ++blk;
    sbp = (C_SCHEME_BLOCK *)scan;

    if(*((C_word *)sbp) == ALIGNMENT_HOLE_MARKER) 
      sbp = (C_SCHEME_BLOCK *)((C_word *)sbp + 1);

    n = C_header_size(sbp);
    h = sbp->header;
    bytes = (h & C_BYTEBLOCK_BIT) ? n : n * sizeof(C_word);
    key = (C_word)(h & C_HEADER_BITS_MASK);
    p = sbp->data;

    if(key == C_STRUCTURE_TYPE) key = *p;

    hdump_count(key, 1, bytes);

    if(n > 0 && (h & C_BYTEBLOCK_BIT) == 0) {
      if((h & C_SPECIALBLOCK_BIT) != 0) {
	--n;
	++p;
      }

      while(n--) {
	x = *(p++);

	if(C_immediatep(x)) {
	  ++imm;

	  if((x & C_FIXNUM_BIT) != 0) key = C_fix(1);
	  else {
	    switch(x & C_IMMEDIATE_TYPE_BITS) {
	    case C_BOOLEAN_BITS: key = C_SCHEME_TRUE; break;
	    case C_CHARACTER_BITS: key = C_make_character('A'); break;
	    default: key = x;
	    }
	  }

	  hdump_count(key, 1, 0);
	}
      }
    }

    scan = (C_byte *)sbp + C_align(bytes) + sizeof(C_word);
  }

  bp = hdump_table;
  
  for(n = 0; n < HDUMP_TABLE_SIZE; ++n) {
    for(b = bp[ n ]; b != NULL; b = b2) {
      b2 = b->next;

      switch(b->key) {
      case C_fix(1): C_fprintf(C_stderr,              C_text("fixnum         ")); break;
      case C_SCHEME_TRUE: C_fprintf(C_stderr,         C_text("boolean        ")); break;
      case C_SCHEME_END_OF_LIST: C_fprintf(C_stderr,  C_text("null           ")); break;
      case C_SCHEME_UNDEFINED  : C_fprintf(C_stderr,  C_text("void           ")); break;
      case C_make_character('A'): C_fprintf(C_stderr, C_text("character      ")); break;
      case C_SCHEME_END_OF_FILE: C_fprintf(C_stderr,  C_text("eof            ")); break;
      case C_SCHEME_UNBOUND: C_fprintf(C_stderr,      C_text("unbound        ")); break;
      case C_SYMBOL_TYPE: C_fprintf(C_stderr,         C_text("symbol         ")); break;
      case C_STRING_TYPE: C_fprintf(C_stderr,         C_text("string         ")); break;
      case C_PAIR_TYPE: C_fprintf(C_stderr,           C_text("pair           ")); break;
      case C_CLOSURE_TYPE: C_fprintf(C_stderr,        C_text("closure        ")); break;
      case C_FLONUM_TYPE: C_fprintf(C_stderr,         C_text("flonum         ")); break;
      case C_PORT_TYPE: C_fprintf(C_stderr,           C_text("port           ")); break;
      case C_POINTER_TYPE: C_fprintf(C_stderr,        C_text("pointer        ")); break;
      case C_LOCATIVE_TYPE: C_fprintf(C_stderr,       C_text("locative       ")); break;
      case C_TAGGED_POINTER_TYPE: C_fprintf(C_stderr, C_text("tagged pointer ")); break;
      case C_SWIG_POINTER_TYPE: C_fprintf(C_stderr,   C_text("swig pointer   ")); break;
      case C_LAMBDA_INFO_TYPE: C_fprintf(C_stderr,    C_text("lambda info    ")); break;
      case C_BUCKET_TYPE: C_fprintf(C_stderr,         C_text("bucket         ")); break;
      case C_VECTOR_TYPE: C_fprintf(C_stderr,         C_text("vector         ")); break;
      case C_BYTEVECTOR_TYPE: C_fprintf(C_stderr,     C_text("bytevector     ")); break;
	/* XXX this is sort of funny: */
      case C_BYTEBLOCK_BIT: C_fprintf(C_stderr,        C_text("blob           ")); break;
      default:
	x = b->key;

	if(!C_immediatep(x) && C_header_bits(x) == C_SYMBOL_TYPE) {
	  x = C_block_item(x, 1);
	  C_fprintf(C_stderr, C_text("`%.*s'"), (int)C_header_size(x), C_c_string(x));
	}
	else C_fprintf(C_stderr, C_text("unknown key " UWORD_FORMAT_STRING), (C_uword)b->key);
      }

      C_fprintf(C_stderr, C_text("\t" UWORD_COUNT_FORMAT_STRING), b->count);

      if(b->total > 0) 
	C_fprintf(C_stderr, C_text("\t" UWORD_COUNT_FORMAT_STRING " bytes"), 
		  (unsigned int)b->total);

      C_fputc('\n', C_stderr);
      C_free(b);
    }
  }

  C_fprintf(C_stderr, C_text("\ntotal number of blocks: " UWORD_COUNT_FORMAT_STRING
			     ", immediates: " UWORD_COUNT_FORMAT_STRING "\n"),
	    (unsigned int)blk, (unsigned int)imm);
  C_free(hdump_table);
  C_kontinue(k, C_SCHEME_UNDEFINED);
}


static void 
filter_heap_objects_2(void *dummy)
{
  void *func = C_pointer_address(C_restore);
  C_word userarg = C_restore;
  C_word vector = C_restore;
  C_word k = C_restore;
  int n, bytes;
  C_byte *scan;
  C_SCHEME_BLOCK *sbp;
  C_header h;
  C_word *p;
  int vecsize = C_header_size(vector);
  typedef int (*filterfunc)(C_word x, C_word userarg);
  filterfunc ff = (filterfunc)func;
  int vcount = 0;

  scan = fromspace_start;

  while(scan < C_fromspace_top) {
    sbp = (C_SCHEME_BLOCK *)scan;

    if(*((C_word *)sbp) == ALIGNMENT_HOLE_MARKER) 
      sbp = (C_SCHEME_BLOCK *)((C_word *)sbp + 1);

    n = C_header_size(sbp);
    h = sbp->header;
    bytes = (h & C_BYTEBLOCK_BIT) ? n : n * sizeof(C_word);
    p = sbp->data;

    if(ff((C_word)sbp, userarg)) {
      if(vcount < vecsize) {
	C_set_block_item(vector, vcount, (C_word)sbp);
	++vcount;
      }
      else {
	C_kontinue(k, C_fix(-1));
      }
    }

    scan = (C_byte *)sbp + C_align(bytes) + sizeof(C_word);
  }

  C_kontinue(k, C_fix(vcount));
}


void C_ccall
C_filter_heap_objects(C_word c, C_word closure, C_word k, C_word func, C_word vector, 
		      C_word userarg)
{
  /* make sure heap is compacted */
  C_save(k);
  C_save(vector);
  C_save(userarg);
  C_save(func);
  C_fromspace_top = C_fromspace_limit; /* force major GC */
  C_reclaim((void *)filter_heap_objects_2, NULL);
}


C_regparm C_word C_fcall
C_i_file_exists_p(C_word name, C_word file, C_word dir)
{
  struct stat buf;
  int res;

  res = stat(C_c_string(name), &buf);

  if(res != 0) {
    switch(errno) {
    case ENOENT: return C_SCHEME_FALSE;
    case EOVERFLOW: return C_truep(dir) ? C_SCHEME_FALSE : C_SCHEME_TRUE;
    case ENOTDIR: return C_SCHEME_FALSE;
    default: return C_fix(res);
    }
  }

  switch(buf.st_mode & S_IFMT) {
  case S_IFDIR: return C_truep(file) ? C_SCHEME_FALSE : C_SCHEME_TRUE;
  default: return C_truep(dir) ? C_SCHEME_FALSE : C_SCHEME_TRUE;
  }
}


C_regparm C_word C_fcall
C_i_pending_interrupt(C_word dummy)
{
  if(pending_interrupts_count > 0) {
    handling_interrupts = 1; /* Lock out further forced GCs until we're done */
    return C_fix(pending_interrupts[ --pending_interrupts_count ]);
  } else {
    handling_interrupts = 0; /* OK, can go on */
    return C_SCHEME_FALSE;
  }
}
