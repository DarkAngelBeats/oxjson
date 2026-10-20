#ifndef JSON_DUMPS_WALK_H
#define JSON_DUMPS_WALK_H

#include <Python.h>

#ifdef __cplusplus
extern "C" {
#endif

/* dumps(obj, default=None, option=None, *, allow_nan=True) -> bytes */
PyObject *JsonDumps_py_dumps(PyObject *self, PyObject *args, PyObject *kwargs);
/* Same, METH_FASTCALL | METH_KEYWORDS. */
PyObject *JsonDumps_py_dumps_fast(PyObject *self, PyObject *const *args, Py_ssize_t nargs,
                                  PyObject *kwnames);

#ifdef __cplusplus
}
#endif

#endif
