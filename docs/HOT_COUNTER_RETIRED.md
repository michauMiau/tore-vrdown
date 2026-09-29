// Why the hot-path counter has to go, and what replaces it.
//
// v19 installed 15 call-site patches and Teardown produced a fatal sentry
// event four minutes later. That is a correlation, not a proof, but the code is
// unsafe on its face and its purpose has been overtaken:
//
//   1. Each patch is a 5-byte non-atomic write, applied from the init thread
//      while the game is already rendering. A core executing those bytes during
//      the write sees a torn instruction. x64 gives no way to make a 5-byte
//      patch atomic; a real one uses int3 padding and a second thread.
//   2. The stub clobbers rax before tail-jumping. Legal under the ABI, but it
//      is exactly the kind of thing that is only safe when the target does not
//      read rax on entry — an assumption about a function we have not read.
//   3. The table answers "what runs per frame", and the frame path turned out
//      not to contain the SDB at all (docs/SDB_NOT_ON_FRAME_PATH.md). So the
//      answer would have been "none of these" — which we already established
//      statically, at the cost of a crash.
//
// The replacement does not need to know where the frame path is. It walks the
// renderer's own heap pointers and looks for a plausible SceneDynamicBuffer by
// content. Teardown ships packed (entropy 8.0), so static RE keeps producing
// wrong answers; the process does not.
