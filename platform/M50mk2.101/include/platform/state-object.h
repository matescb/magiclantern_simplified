#ifndef __platform_state_object_h
#define __platform_state_object_h

/* EvfDevState, confirmed via serial-harness statespy: *(0x10108) is the state object
 * whose per-frame self-transition (input==6, old_state==3) drives LV vsync. The old
 * 0x6cec was an unverified guess (not even a StateObject). */
#define EVF_STATE (*(struct state_object **)0x10108)

/* EvfState (LiveView::EvfCapState.c), created at 0xE06EEC38 with matrix 0xE0B5050C,
 * 17 inputs x 8 states, dispatched by task EvfCap. Input 5 = evfReadOutDoneInterrupt.
 * Alternative per-frame hook source, see m50ii_vsync_src in src/state-object.c and
 * docs/offline/F1.md. Not yet probed on hardware. */
#define M50II_EVFCAP_STATE (*(struct state_object **)0x8E8C)

#endif // __platform_state_object_h
