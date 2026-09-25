#ifndef RVUOS_WORK_H
#define RVUOS_WORK_H

/*
 * What bounds a loop, said in the source; see DESIGN.md, "Bounded work".
 * Every loop a trap can run says it with one of these, a statement of its body,
 * and tools/loop-bounds.py fails the link on a loop of the image whose source does not.
 * Each is a _Static_assert, so it emits nothing and changes no code the compiler makes;
 * the tool reads them from clang's AST and finds a loop's source through the debug information.
 *
 * LOOP_BOUND(n)         at most n iterations, n a constant of the kernel or the machine.
 * LOOP_PAID(name, unit, what)
 *                       each iteration takes away what, which an earlier call made,
 *                       counted as a unit: a node, a link (a node below another),
 *                       an object, or a waiter (a thread waiting).
 * LOOP_WALK(name)       a walk the table in DESIGN.md, "Bounded work", lists as name.
 * LOOP_ARG(fn, arg)     bounded by the argument arg of fn, the function it stands in,
 *                       so every call to fn says what bounds it, with a CALL_ annotation.
 * LOOP_WAIT(what)       waits on the hardware for what, doing no work of its own.
 *
 * A CALL_ annotation stands just before the statement that calls such a function:
 * CALL_BOUND(n) and CALL_WALK(name) as for a loop,
 * CALL_LITERAL(s) when the bound is the length of s, which must be a string literal,
 * and CALL_ARG(fn, arg) when it is the argument arg of fn, the caller,
 * which passes the question on to fn's callers.
 *
 * In the host build's harness fuzz-work, RVUOS_WORK makes each loop annotation count too,
 * and the harness checks the claims after every call: see host/work.c.
 */

#define WORK_STR_(x) #x
#define WORK_STR(x) WORK_STR_(x)
#define WORK_SITE __FILE__ ":" WORK_STR(__LINE__)

/*
 * WORK_STEP starts with the semicolon that ends the _Static_assert,
 * so an annotation is still written as one statement.
 * Every loop annotation steps, not only the checked kinds,
 * so that a loop inside it is entered anew at each of its iterations.
 */
#if defined(RVUOS_HOST) && defined(RVUOS_WORK)
struct work_site {
    char kind; /* 'b' bound, 'p' paid, 'o' any other */
    unsigned bound;
    const char *unit;
    const char *site;
};
void work_step(const struct work_site *site);
#define WORK_STEP(kind, n, unit)                                                   \
    ;                                                                               \
    do {                                                                            \
        static const struct work_site work_site_ = { kind, n, unit, WORK_SITE };   \
        work_step(&work_site_);                                                     \
    } while (0)
#else
#define WORK_STEP(kind, n, unit)
#endif

#define LOOP_BOUND(n) _Static_assert((n) > 0, "loop bound " #n) WORK_STEP('b', (n), "")
#define LOOP_PAID(name, unit, what) \
    _Static_assert(1, "loop paid " #name " " #unit " " what) WORK_STEP('p', 0, #unit)
#define LOOP_WALK(name) _Static_assert(1, "loop walk " #name) WORK_STEP('o', 0, "")
#define LOOP_ARG(fn, arg) _Static_assert(1, "loop arg " #fn " " #arg) WORK_STEP('o', 0, "")
#define LOOP_WAIT(what) _Static_assert(1, "loop wait " what) WORK_STEP('o', 0, "")

#define CALL_BOUND(n) _Static_assert((n) > 0, "call bound " #n)
#define CALL_WALK(name) _Static_assert(1, "call walk " #name)
#define CALL_LITERAL(s) _Static_assert(sizeof("" s) > 0, "call literal")
#define CALL_ARG(fn, arg) _Static_assert(1, "call arg " #fn " " #arg)

#endif
