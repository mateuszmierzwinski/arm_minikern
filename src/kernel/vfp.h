/* vfp — moving the floating-point state only when a thread uses it.
 *
 * Saving thirty-two 64-bit registers and the status word on every context
 * switch costs the same whether the thread has ever done arithmetic or not
 * — and many threads never do: a goroutine waiting on a channel, the
 * netpoller, anything doing I/O.
 *
 * So the state is left in the registers and the unit is switched **off**
 * for the thread being switched to. The first floating-point instruction it
 * executes traps as an undefined instruction, and that trap is where the
 * work happens: the previous owner's registers are saved, this thread's are
 * restored, the unit is switched on, and the instruction is retried. A
 * thread that never does arithmetic never pays.
 *
 * The owner is per core, because the registers are.
 */
#ifndef MINIKERN_KERNEL_VFP_H
#define MINIKERN_KERNEL_VFP_H

#include <stdbool.h>
#include <stdint.h>

struct task;
struct vfp_state;

/* Called when a thread is switched to: the unit goes off, and the state
 * stays where it is. */
void vfp_switch_away(void);

/* Called from the undefined-instruction trap. True when the instruction was
 * floating-point on a thread whose unit was off — the state has now been
 * moved and the instruction should be retried. False when it was a
 * genuinely undefined instruction, which is the caller's business.
 */
bool vfp_claim(struct task *t);

/* Copies the unit's registers into a thread's structure, if that is where
 * they are. Needed when something other than a switch has to read a
 * thread's state — a clone copying it to a new thread — because the state
 * may be live in the hardware and not in the structure at all. */
void vfp_capture(struct task *t);

/* Reads a thread's floating-point state wherever it is, and replaces it.
 *
 * Signal delivery needs both: the frame it builds must carry what the
 * thread had, and `rt_sigreturn` must put back what the frame holds — and
 * with the state moved lazily, "wherever it is" is the whole point. Reading
 * the hardware directly was right when every switch restored it, and is
 * now a way to read somebody else's registers. */
void vfp_read(struct task *t, struct vfp_state *out);
void vfp_write(struct task *t, const struct vfp_state *in);

/* How many times the state has been moved between threads. The number to
 * compare it with is how many context switches there were: moving it on
 * every switch is what this replaces. */
uint64_t vfp_moves(void);

/* Called when a thread ends, so that its registers are not saved into a
 * freed structure the next time someone wants the unit. */
void vfp_forget(const struct task *t);

#endif
