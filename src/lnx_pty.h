#ifndef ACE_LNX_PTY_H
#define ACE_LNX_PTY_H

/*
 * RunCommand() will set this private marker only when all three selected
 * command streams are the live ACE console. Chunk 1's isolated test harness
 * sets it directly; later chunks move the production decision into the
 * command runner. LNX removes it before execing the Linux target.
 */
#define ACE_LNX_PTY_VARIABLE "ACE_LNX_PTY"
#define ACE_LNX_PTY_VALUE "1"
#define ACE_LNX_TARGET_TERM_VARIABLE "ACE_LNX_TARGET_TERM"
#define ACE_LNX_PTY_TERM "xterm-256color"

#endif
