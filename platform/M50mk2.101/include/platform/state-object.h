#ifndef __platform_state_object_h
#define __platform_state_object_h

/* EvfDevState, confirmed via serial-harness statespy: *(0x10108) is the state object
 * whose per-frame self-transition (input==6, old_state==3) drives LV vsync. The old
 * 0x6cec was an unverified guess (not even a StateObject). */
#define EVF_STATE (*(struct state_object **)0x10108)

#endif // __platform_state_object_h
