#ifndef ACE_AROS_BOOPSI_RUNTIME_H
#define ACE_AROS_BOOPSI_RUNTIME_H

/*
 * Host seam for the real AROS BOOPSI sources in rom/intuition and
 * compiler/alib.  ACE supplies the Exec services those sources call and the
 * one-time rootclass bootstrap that intuition_init.c performs on a real AROS
 * build.  The class system itself is entirely AROS code.
 */

/*
 * Creates the Intuition base, initialises the class list and installs the
 * rootclass, in the same order as InitRootClass() in rom/intuition.
 * Returns 0 on success.
 *
 * Calling it more than once does not install a second rootclass; it takes
 * another reference to the one that is there.  The class list is process-
 * wide, so every user has to be counted: a console device that closed used
 * to tear the whole thing down while another still held classes and objects
 * on it, which left the survivor dispatching methods through freed memory.
 * Each successful call is one ace_boopsi_cleanup().
 */
int ace_boopsi_init(void);

/*
 * Drops one reference.  On the last one, releases the class list and the
 * Intuition base: classes still registered are freed; objects still alive
 * are not, since only their class knows how.  Unbalanced calls, or calls
 * before any successful init, do nothing.
 */
void ace_boopsi_cleanup(void);

/* The rootclass installed by ace_boopsi_init(), or NULL before it runs. */
struct IClass *ace_boopsi_rootclass(void);

#endif
