/* Python module oxjson.oxjson: loads, load, dumps, dump, the error
 * types and the OPT_* constants.
 * loads: src/jl_kernel.asm + src/jl_loads.c (AVX2) or src/jl_portable.c.
 * dumps: src/json_dumps_walk.c (AVX2) or src/json_dumps_portable.c.
 */
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <string.h>
#include "json_dumps_walk.h"
#include "json_build.h"

void JsonDetectAvx2(void);
extern int g_jsonHasAvx2;

/* Two builds of loads and dumps live in the module: the AVX2 one (NASM kernel,
 * x86-64 only) and the portable C one. Module init picks one per process. */
PyObject *JlLoads_portable(PyObject *self, PyObject *obj);
void JsonLoads_set_error_type_portable(PyObject *exc_type);
void JsonLoads_set_json_error_type_portable(PyObject *exc_type);
PyObject *JsonDumps_py_dumps_portable(PyObject *self, PyObject *args, PyObject *kwargs);
PyObject *JsonDumps_py_dumps_fast_portable(PyObject *self, PyObject *const *args, Py_ssize_t nargs,
                                           PyObject *kwnames);
/* JSON_AVX2_BUILD: the build script linked the AVX2 objects too */
#ifdef JSON_AVX2_BUILD
#define JSON_HAVE_AVX2_BUILD 1
PyObject *JlLoads(PyObject *self, PyObject *obj);
void JsonLoads_set_error_type(PyObject *exc_type);
void JsonLoads_set_json_error_type(PyObject *exc_type);
#endif

static PyCFunction g_loads = JlLoads_portable;
static PyCFunctionWithKeywords g_dumps = JsonDumps_py_dumps_portable;

static PyObject *mod_loads(PyObject *self, PyObject *obj)
{
    return g_loads(self, obj);
}

static PyObject *JsonStdDecodeError = NULL;
static PyObject *StdJSONDecodeInit = NULL;

static int set_code_pos(PyObject *self, PyObject *code, PyObject *pos)
{
    PyObject *owned_c = NULL;
    PyObject *owned_p = NULL;
    int rc;
    if (!code) {
        owned_c = PyLong_FromLong(-5);
        if (!owned_c) {
            return -1;
        }
        code = owned_c;
    }
    if (!pos) {
        owned_p = PyLong_FromLong(0);
        if (!owned_p) {
            Py_XDECREF(owned_c);
            return -1;
        }
        pos = owned_p;
    }
    rc = PyObject_SetAttrString(self, "code", code);
    if (rc < 0) {
        Py_XDECREF(owned_c);
        Py_XDECREF(owned_p);
        return -1;
    }
    rc = PyObject_SetAttrString(self, "pos", pos);
    Py_XDECREF(owned_c);
    Py_XDECREF(owned_p);
    return rc;
}

static int json_error_tp_init(PyObject *self, PyObject *args, PyObject *kw)
{
    PyObject *message;
    PyObject *code = NULL;
    PyObject *pos = NULL;
    PyObject *sub;
    Py_ssize_t n;
    int rc;

    if (kw && PyDict_Size(kw) > 0) {
        PyErr_SetString(PyExc_TypeError, "JSONError() takes no keyword arguments");
        return -1;
    }
    n = PyTuple_GET_SIZE(args);
    if (n == 1) {
        message = PyTuple_GET_ITEM(args, 0);
    } else if (n == 2) {
        code = PyTuple_GET_ITEM(args, 0);
        message = PyTuple_GET_ITEM(args, 1);
    } else if (n == 3) {
        code = PyTuple_GET_ITEM(args, 0);
        message = PyTuple_GET_ITEM(args, 1);
        pos = PyTuple_GET_ITEM(args, 2);
    } else {
        PyErr_SetString(PyExc_TypeError, "JSONError() takes 1 to 3 arguments");
        return -1;
    }
    sub = PyTuple_Pack(1, message);
    if (!sub) {
        return -1;
    }
    rc = ((PyTypeObject *)PyExc_ValueError)->tp_init(self, sub, NULL);
    Py_DECREF(sub);
    if (rc < 0) {
        return -1;
    }
    return set_code_pos(self, code, pos);
}

static int json_decode_error_tp_init(PyObject *self, PyObject *args, PyObject *kw)
{
    PyObject *code = NULL;
    PyObject *msg;
    PyObject *doc;
    PyObject *pos;
    PyObject *ret;
    Py_ssize_t n;

    if (kw && PyDict_Size(kw) > 0) {
        PyErr_SetString(PyExc_TypeError, "JSONDecodeError() takes no keyword arguments");
        return -1;
    }
    n = PyTuple_GET_SIZE(args);
    if (n == 3) {
        msg = PyTuple_GET_ITEM(args, 0);
        doc = PyTuple_GET_ITEM(args, 1);
        pos = PyTuple_GET_ITEM(args, 2);
    } else if (n == 4) {
        code = PyTuple_GET_ITEM(args, 0);
        msg = PyTuple_GET_ITEM(args, 1);
        doc = PyTuple_GET_ITEM(args, 2);
        pos = PyTuple_GET_ITEM(args, 3);
    } else {
        PyErr_SetString(PyExc_TypeError, "JSONDecodeError() takes 3 or 4 arguments");
        return -1;
    }
    if (!JsonStdDecodeError || !StdJSONDecodeInit) {
        PyErr_SetString(PyExc_RuntimeError, "json.JSONDecodeError missing");
        return -1;
    }
    ret = PyObject_CallFunctionObjArgs(StdJSONDecodeInit, self, msg, doc, pos, NULL);
    if (!ret) {
        return -1;
    }
    Py_DECREF(ret);
    return set_code_pos(self, code, pos);
}

static PyType_Slot json_error_slots[] = {
    {Py_tp_init, json_error_tp_init},
    {Py_tp_doc, "Library error. Parse failures are JSONDecodeError (also a JSONError)."},
    {0, 0}
};

static PyType_Spec json_error_spec = {
    "oxjson.JSONError",
    0,
    0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    json_error_slots
};

static PyType_Slot json_decode_error_slots[] = {
    {Py_tp_init, json_decode_error_tp_init},
    {Py_tp_doc, "Parse error; .pos is a character index into .doc (json.JSONDecodeError compatible)."},
    {0, 0}
};

static PyType_Spec json_decode_error_spec = {
    "oxjson.JSONDecodeError",
    0,
    0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    json_decode_error_slots
};

static PyObject *mod_load(PyObject *self, PyObject *fp)
{
    PyObject *data;
    PyObject *out;
    data = PyObject_CallMethod(fp, "read", NULL);
    if (!data) {
        return NULL;
    }
    out = mod_loads(self, data);
    Py_DECREF(data);
    return out;
}

static PyObject *mod_dump(PyObject *self, PyObject *args, PyObject *kwargs)
{
    PyObject *obj;
    PyObject *fp;
    PyObject *dumps_args;
    PyObject *text;
    PyObject *wret;

    if (PyTuple_GET_SIZE(args) != 2) {
        PyErr_SetString(PyExc_TypeError, "dump() takes 2 positional arguments: obj, fp");
        return NULL;
    }
    obj = PyTuple_GET_ITEM(args, 0);
    fp = PyTuple_GET_ITEM(args, 1);
    dumps_args = PyTuple_Pack(1, obj);
    if (!dumps_args) {
        return NULL;
    }
    text = g_dumps(self, dumps_args, kwargs);
    Py_DECREF(dumps_args);
    if (!text) {
        return NULL;
    }
    wret = PyObject_CallMethod(fp, "write", "O", text);
    Py_DECREF(text);
    if (!wret) {
        return NULL;
    }
    Py_DECREF(wret);
    Py_RETURN_NONE;
}

static PyMethodDef methods[] = {
    {"loads", JlLoads_portable, METH_O,
     "loads(data) -> object. data is str, bytes, bytearray or memoryview."},
    {"load", mod_load, METH_O,
     "fp.read() then loads()."},
    {"dump", (PyCFunction)mod_dump, METH_VARARGS | METH_KEYWORDS,
     "Write dumps(obj) bytes to a file object. Keywords: default=, option=, allow_nan=."},
    {"dumps", (PyCFunction)(void (*)(void))JsonDumps_py_dumps_fast_portable, METH_FASTCALL | METH_KEYWORDS,
     "dumps(obj, default=None, option=None, *, allow_nan=True) -> bytes. option is OPT_* bits, as in orjson."},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef moduledef = {
    PyModuleDef_HEAD_INIT,
    "oxjson.oxjson",
    "JSON loads and dumps.",
    -1,
    methods,
    NULL,
    NULL,
    NULL,
    NULL
};

static int add_type(PyObject *m, const char *name, PyObject *type)
{
    if (!type) {
        return -1;
    }
    Py_INCREF(type);
    if (PyModule_AddObject(m, name, type) < 0) {
        Py_DECREF(type);
        return -1;
    }
    return 0;
}

PyMODINIT_FUNC PyInit_oxjson(void)
{
    PyObject *m;
    PyObject *json_mod = NULL;
    PyObject *bases = NULL;
    PyObject *JSONError = NULL;
    PyObject *JSONDecodeError = NULL;
    PyObject *all = NULL;

    JsonDetectAvx2();
#ifdef JSON_HAVE_AVX2_BUILD
    if (g_jsonHasAvx2) {
        PyMethodDef *m;
        g_loads = JlLoads;
        g_dumps = JsonDumps_py_dumps;
        for (m = methods; m->ml_name; m++) {
            if (strcmp(m->ml_name, "loads") == 0) {
                m->ml_meth = JlLoads;
            } else if (strcmp(m->ml_name, "dumps") == 0) {
                m->ml_meth = (PyCFunction)(void (*)(void))JsonDumps_py_dumps_fast;
            }
        }
    }
#endif

    json_mod = PyImport_ImportModule("json");
    if (!json_mod) {
        return NULL;
    }
    JsonStdDecodeError = PyObject_GetAttrString(json_mod, "JSONDecodeError");
    Py_DECREF(json_mod);
    json_mod = NULL;
    if (!JsonStdDecodeError) {
        return NULL;
    }
    StdJSONDecodeInit = PyObject_GetAttrString(JsonStdDecodeError, "__init__");
    if (!StdJSONDecodeInit) {
        return NULL;
    }

    bases = PyTuple_Pack(1, PyExc_ValueError);
    if (!bases) {
        goto fail;
    }
    JSONError = PyType_FromSpecWithBases(&json_error_spec, bases);
    Py_CLEAR(bases);
    if (!JSONError) {
        goto fail;
    }

    bases = PyTuple_Pack(2, JsonStdDecodeError, JSONError);
    if (!bases) {
        goto fail;
    }
    JSONDecodeError = PyType_FromSpecWithBases(&json_decode_error_spec, bases);
    Py_CLEAR(bases);
    if (!JSONDecodeError) {
        goto fail;
    }

    m = PyModule_Create(&moduledef);
    if (!m) {
        goto fail;
    }
    if (json_build_export(m) < 0) {
        Py_DECREF(m);
        goto fail;
    }
    if (add_type(m, "JSONError", JSONError) < 0) {
        Py_DECREF(m);
        goto fail;
    }
    if (add_type(m, "JSONDecodeError", JSONDecodeError) < 0) {
        Py_DECREF(m);
        goto fail;
    }
    if (PyModule_AddIntConstant(m, "JSON_ERROR_EMPTY_DOCUMENT", -18) < 0) {
        Py_DECREF(m);
        goto fail;
    }
    all = Py_BuildValue("[sssssss]",
        "JSONDecodeError",
        "JSONError",
        "JSON_ERROR_EMPTY_DOCUMENT",
        "dump",
        "dumps",
        "load",
        "loads");
    if (!all) {
        Py_DECREF(m);
        goto fail;
    }
    {
        /* the orjson option bits, same names and values */
        static const struct { const char *name; long v; } opts[] = {
            {"OPT_APPEND_NEWLINE", 1024}, {"OPT_INDENT_2", 1}, {"OPT_NAIVE_UTC", 2}, {"OPT_NON_STR_KEYS", 4},
            {"OPT_OMIT_MICROSECONDS", 8}, {"OPT_PASSTHROUGH_DATACLASS", 2048}, {"OPT_PASSTHROUGH_DATETIME", 512},
            {"OPT_PASSTHROUGH_SUBCLASS", 256}, {"OPT_SERIALIZE_DATACLASS", 0}, {"OPT_SERIALIZE_NUMPY", 16},
            {"OPT_SERIALIZE_UUID", 0}, {"OPT_SORT_KEYS", 32}, {"OPT_STRICT_INTEGER", 64}, {"OPT_UTC_Z", 128}};
        size_t i;
        for (i = 0; i < sizeof(opts) / sizeof(opts[0]); i++) {
            PyObject *nm = PyUnicode_FromString(opts[i].name);
            if (!nm || PyModule_AddIntConstant(m, opts[i].name, opts[i].v) < 0 || PyList_Append(all, nm) < 0) {
                Py_XDECREF(nm);
                Py_DECREF(all);
                Py_DECREF(m);
                return NULL;
            }
            Py_DECREF(nm);
        }
    }
    if (PyModule_AddObject(m, "__all__", all) < 0) {
        Py_DECREF(all);
        Py_DECREF(m);
        goto fail;
    }
    JsonLoads_set_error_type_portable(JSONDecodeError);
    JsonLoads_set_json_error_type_portable(JSONError);
#ifdef JSON_HAVE_AVX2_BUILD
    /* the AVX2 copy of src/jl_loads.c keeps its own references */
    JsonLoads_set_error_type(JSONDecodeError);
    JsonLoads_set_json_error_type(JSONError);
#endif
    Py_DECREF(JSONError);
    Py_DECREF(JSONDecodeError);
    return m;

fail:
    Py_XDECREF(json_mod);
    Py_XDECREF(bases);
    Py_XDECREF(JSONError);
    Py_XDECREF(JSONDecodeError);
    return NULL;
}
