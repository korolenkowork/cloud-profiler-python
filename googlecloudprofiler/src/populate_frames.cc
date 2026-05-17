#include "populate_frames.h"

#include <Python.h>

#include "stacktraces.h"

// Python version definitions
#define PY_311 0x030B0000  // 3.11
#define PY_312 0x030C0000  // 3.12
#define PY_313 0x030D0000  // 3.13

#if PY_VERSION_HEX >= PY_313

/**
 * Python 3.13 introduced significant changes to the frame structure:
 * - f_code renamed to f_executable (now PyObject* instead of PyCodeObject*)
 * - prev_instr renamed to instr_ptr
 * - Must use _PyFrame_GetCode() helper to access code object
 * 
 * The PyFrameObject structure members have been removed from the public C API
 * in 3.11:
 * https://docs.python.org/3/whatsnew/3.11.html#pyframeobject-3-11-hiding.
 *
 * Since this code runs as part of the SIGPROF handler, it cannot modify Python
 * objects (including their refcounts) and standard getters can't be used.
 * We expose the internal _PyInterpreterFrame and use that directly.
 */

#define Py_BUILD_CORE
#include "internal/pycore_frame.h"
#undef Py_BUILD_CORE

// Modified from CPython 3.13 source for async-signal-safe access
// Python 3.13 flattened cframe->current_frame to just current_frame
// 
// IMPORTANT: This can be called from a signal handler (SIGPROF), so we must
// be defensive about race conditions where the interpreter is in the middle
// of setting up frames. The current_frame pointer might be NULL or partially
// initialized if we interrupt during _PyEval_EvalFrameDefault setup.
static inline _PyInterpreterFrame *unsafe_PyThreadState_GetInterpreterFrame(
    PyThreadState *tstate) {
  if (tstate == NULL) {
    return NULL;
  }
  
  _PyInterpreterFrame *f = tstate->current_frame;
  
  // Handle race condition: current_frame might be NULL or uninitialized
  // if we interrupted during frame setup
  if (f == NULL) {
    return NULL;
  }
  
  while (f && _PyFrame_IsIncomplete(f)) {
    f = f->previous;
  }
  return f;
}

// In Python 3.13, f_code became f_executable and is now a PyObject*
// This helper safely extracts the code object
static inline PyCodeObject *unsafe_PyInterpreterFrame_GetCode(
    _PyInterpreterFrame *frame) {
  if (frame == NULL || _PyFrame_IsIncomplete(frame)) {
    return NULL;
  }
  
  PyObject *executable = frame->f_executable;
  if (executable == NULL) {
    return NULL;
  }
  
  // f_executable can be a code object or other types; verify it's a code object
  // PyCode_Check uses type pointer, which should be safe to check in signal handler
  if (!PyCode_Check(executable)) {
    return NULL;
  }
  
  return (PyCodeObject *)executable;
}

static inline _PyInterpreterFrame *unsafe_PyInterpreterFrame_GetBack(
    _PyInterpreterFrame *frame) {
  if (frame == NULL || _PyFrame_IsIncomplete(frame)) {
    return NULL;
  }
  
  _PyInterpreterFrame *prev = frame->previous;
  while (prev && _PyFrame_IsIncomplete(prev)) {
    prev = prev->previous;
  }
  return prev;
}

// Python 3.13 uses instr_ptr instead of prev_instr
int _PyInterpreterFrame_GetLine(_PyInterpreterFrame *frame) {
  if (frame == NULL) {
    return -1;
  }
  
  PyCodeObject *code = unsafe_PyInterpreterFrame_GetCode(frame);
  if (code == NULL) {
    return -1;
  }
  
  int addr = (int)(frame->instr_ptr - _PyCode_CODE(code)) * sizeof(_Py_CODEUNIT);
  return PyCode_Addr2Line(code, addr);
}

int PopulateFrames(CallFrame *frames, PyThreadState *ts) {
  if (ts == nullptr) {
    frames[0].lineno = kNoPyState;
    frames[0].py_code = nullptr;
    return 1;
  }

  _PyInterpreterFrame *frame = unsafe_PyThreadState_GetInterpreterFrame(ts);
  int num_frames = 0;
  while (frame != nullptr && num_frames < kMaxFramesToCapture) {
    // Get code object and line number - might be NULL/-1 if we hit a race condition
    PyCodeObject *code = unsafe_PyInterpreterFrame_GetCode(frame);
    int lineno = _PyInterpreterFrame_GetLine(frame);
    
    // Only record frames where we successfully got valid data
    // This handles race conditions where frame is partially initialized
    if (code != NULL && lineno >= 0) {
      frames[num_frames].lineno = lineno;
      frames[num_frames].py_code = code;
      num_frames++;
    }
    
    frame = unsafe_PyInterpreterFrame_GetBack(frame);
  }
  return num_frames;
}

#elif PY_VERSION_HEX >= PY_312

/**
 * Python 3.12 changes to the frame structure:
 * - f_code moved to first position in the struct
 * - f_func renamed to f_funcobj
 * - is_entry field removed
 * - return_offset field added
 *
 * The PyFrameObject structure members have been removed from the public C API
 * in 3.11:
 * https://docs.python.org/3/whatsnew/3.11.html#pyframeobject-3-11-hiding.
 *
 * Since this code runs as part of the SIGPROF handler, it cannot modify Python
 * objects (including their refcounts) and standard getters can't be used.
 * We expose the internal _PyInterpreterFrame and use that directly.
 */

#define Py_BUILD_CORE
#include "internal/pycore_frame.h"
#undef Py_BUILD_CORE

// Modified from CPython 3.12 source for async-signal-safe access
//
// IMPORTANT: This can be called from a signal handler (SIGPROF), which can
// interrupt the Python interpreter at ANY point, including during frame setup
// in _PyEval_EvalFrameDefault. Specifically, the signal can fire after:
//   tstate->cframe = &cframe;  
// but before:
//   cframe.current_frame = frame;
// This creates a race condition where cframe is set but current_frame is
// uninitialized, causing segfaults when we dereference it. We must check
// for NULL at every step.
static inline _PyInterpreterFrame *unsafe_PyThreadState_GetInterpreterFrame(
    PyThreadState *tstate) {
  if (tstate == NULL) {
    return NULL;
  }
  
  // Check if cframe is set - might be NULL during initialization
  _PyCFrame *cframe = tstate->cframe;
  if (cframe == NULL) {
    return NULL;
  }
  
  // CRITICAL: Check if current_frame is set - might be uninitialized
  // if we interrupted during _PyEval_EvalFrameDefault setup
  _PyInterpreterFrame *f = cframe->current_frame;
  if (f == NULL) {
    return NULL;
  }
  
  while (f && _PyFrame_IsIncomplete(f)) {
    f = f->previous;
  }
  return f;
}

// In Python 3.12, f_code is still PyCodeObject* but moved to first position
static inline PyCodeObject *unsafe_PyInterpreterFrame_GetCode(
    _PyInterpreterFrame *frame) {
  if (frame == NULL || _PyFrame_IsIncomplete(frame)) {
    return NULL;
  }
  
  PyCodeObject *code = frame->f_code;
  if (code == NULL) {
    return NULL;
  }
  
  return code;
}

static inline _PyInterpreterFrame *unsafe_PyInterpreterFrame_GetBack(
    _PyInterpreterFrame *frame) {
  if (frame == NULL || _PyFrame_IsIncomplete(frame)) {
    return NULL;
  }
  
  _PyInterpreterFrame *prev = frame->previous;
  while (prev && _PyFrame_IsIncomplete(prev)) {
    prev = prev->previous;
  }
  return prev;
}

// Python 3.12 still uses prev_instr (not renamed yet)
int _PyInterpreterFrame_GetLine(_PyInterpreterFrame *frame) {
  if (frame == NULL) {
    return -1;
  }
  
  PyCodeObject *code = frame->f_code;
  if (code == NULL) {
    return -1;
  }
  
  int addr = _PyInterpreterFrame_LASTI(frame) * sizeof(_Py_CODEUNIT);
  return PyCode_Addr2Line(code, addr);
}

int PopulateFrames(CallFrame *frames, PyThreadState *ts) {
  if (ts == nullptr) {
    frames[0].lineno = kNoPyState;
    frames[0].py_code = nullptr;
    return 1;
  }

  _PyInterpreterFrame *frame = unsafe_PyThreadState_GetInterpreterFrame(ts);
  int num_frames = 0;
  while (frame != nullptr && num_frames < kMaxFramesToCapture) {
    // Get code object and line number - might be NULL/-1 if we hit a race condition
    PyCodeObject *code = unsafe_PyInterpreterFrame_GetCode(frame);
    int lineno = _PyInterpreterFrame_GetLine(frame);
    
    // Only record frames where we successfully got valid data
    // This handles race conditions where frame is partially initialized
    if (code != NULL && lineno >= 0) {
      frames[num_frames].lineno = lineno;
      frames[num_frames].py_code = code;
      num_frames++;
    }
    
    frame = unsafe_PyInterpreterFrame_GetBack(frame);
  }
  return num_frames;
}

#elif PY_VERSION_HEX >= PY_311

/**
 * Python 3.11 frame structure baseline.
 * 
 * The PyFrameObject structure members have been removed from the public C API
 * in 3.11:
 * https://docs.python.org/3/whatsnew/3.11.html#pyframeobject-3-11-hiding.
 *
 * Since this code runs as part of the SIGPROF handler, it cannot modify Python
 * objects (including their refcounts) and standard getters can't be used.
 * We expose the internal _PyInterpreterFrame and use that directly.
 */

#define Py_BUILD_CORE
#include "internal/pycore_frame.h"
#undef Py_BUILD_CORE

// Modified from
// https://github.com/python/cpython/blob/v3.11.4/Python/pystate.c#L1278-L1285
//
// IMPORTANT: This can be called from a signal handler (SIGPROF), which can
// interrupt the Python interpreter during frame setup, creating race conditions.
// See Python 3.12 comments above for details on the race condition in
// _PyEval_EvalFrameDefault where cframe is set before current_frame.
static inline _PyInterpreterFrame *unsafe_PyThreadState_GetInterpreterFrame(
    PyThreadState *tstate) {
  if (tstate == NULL) {
    return NULL;
  }
  
  // Check if cframe is set - might be NULL during initialization
  _PyCFrame *cframe = tstate->cframe;
  if (cframe == NULL) {
    return NULL;
  }
  
  // Check if current_frame is set - might be uninitialized
  _PyInterpreterFrame *f = cframe->current_frame;
  if (f == NULL) {
    return NULL;
  }
  
  while (f && _PyFrame_IsIncomplete(f)) {
    f = f->previous;
  }
  return f;
}

// Modified from
// https://github.com/python/cpython/blob/v3.11.4/Objects/frameobject.c#L1310-L1315
// with refcounting removed and additional NULL checks for signal safety
static inline PyCodeObject *unsafe_PyInterpreterFrame_GetCode(
    _PyInterpreterFrame *frame) {
  if (frame == NULL || _PyFrame_IsIncomplete(frame)) {
    return NULL;
  }
  
  PyCodeObject *code = frame->f_code;
  if (code == NULL) {
    return NULL;
  }
  
  return code;
}

// Modified from
// https://github.com/python/cpython/blob/v3.11.4/Objects/frameobject.c#L1326-L1329
// with refcounting removed and additional NULL checks for signal safety
static inline _PyInterpreterFrame *unsafe_PyInterpreterFrame_GetBack(
    _PyInterpreterFrame *frame) {
  if (frame == NULL || _PyFrame_IsIncomplete(frame)) {
    return NULL;
  }
  
  _PyInterpreterFrame *prev = frame->previous;
  while (prev && _PyFrame_IsIncomplete(prev)) {
    prev = prev->previous;
  }
  return prev;
}

// Copied from
// https://github.com/python/cpython/blob/v3.11.4/Python/frame.c#L165-L170 as
// this function is not available in libpython
// Added NULL checks for signal safety
int _PyInterpreterFrame_GetLine(_PyInterpreterFrame *frame) {
  if (frame == NULL) {
    return -1;
  }
  
  PyCodeObject *code = frame->f_code;
  if (code == NULL) {
    return -1;
  }
  
  int addr = _PyInterpreterFrame_LASTI(frame) * sizeof(_Py_CODEUNIT);
  return PyCode_Addr2Line(code, addr);
}

int PopulateFrames(CallFrame *frames, PyThreadState *ts) {
  if (ts == nullptr) {
    frames[0].lineno = kNoPyState;
    frames[0].py_code = nullptr;
    return 1;
  }

  // We are running in the context of the thread interrupted by the signal
  // so the frame object for the current thread is stable.
  // Unfortunately, we can't use PyFrameObjects because they are initialized
  // lazily and will not have the info we need directly.
  _PyInterpreterFrame *frame = unsafe_PyThreadState_GetInterpreterFrame(ts);
  int num_frames = 0;
  while (frame != nullptr && num_frames < kMaxFramesToCapture) {
    // Get code object and line number - might be NULL/-1 if we hit a race condition
    PyCodeObject *code = unsafe_PyInterpreterFrame_GetCode(frame);
    int lineno = _PyInterpreterFrame_GetLine(frame);
    
    // Only record frames where we successfully got valid data
    // This handles race conditions where frame is partially initialized
    if (code != NULL && lineno >= 0) {
      frames[num_frames].lineno = lineno;
      frames[num_frames].py_code = code;
      num_frames++;
    }
    
    frame = unsafe_PyInterpreterFrame_GetBack(frame);
  }
  return num_frames;
}

#else
// python versions before 3.11

int PopulateFrames(CallFrame *frames, PyThreadState *ts) {
  if (ts == nullptr) {
    frames[0].lineno = kNoPyState;
    frames[0].py_code = nullptr;
    return 1;
  }
  // We are running in the context of the thread interrupted by the signal
  // so the frame object for the current thread is stable.
  PyFrameObject *frame = ts->frame;
  int num_frames = 0;
  while (frame != nullptr && num_frames < kMaxFramesToCapture) {
    frames[num_frames].lineno = frame->f_lineno;
    frames[num_frames].py_code = frame->f_code;
    num_frames++;
    frame = frame->f_back;
  }
  return num_frames;
}

#endif  // PY_VERSION_HEX >= PY_311

