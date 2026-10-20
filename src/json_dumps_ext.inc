/* Part of src/json_dumps_walk.c: the types and options orjson.dumps has beyond
 * the JSON-native ones. Everything here is off the hot path: it runs for a value
 * whose type is not an exact builtin, or for a whole document when an option
 * changes the layout (indent, sorted keys, non-str keys, strict integers).
 *
 * Option bits are the orjson OPT_* values.
 */
#include <datetime.h>

#define JOPT_INDENT_2 1u
#define JOPT_NAIVE_UTC 2u
#define JOPT_NON_STR_KEYS 4u
#define JOPT_OMIT_MICROSECONDS 8u
#define JOPT_SERIALIZE_NUMPY 16u
#define JOPT_SORT_KEYS 32u
#define JOPT_STRICT_INTEGER 64u
#define JOPT_UTC_Z 128u
#define JOPT_PASSTHROUGH_SUBCLASS 256u
#define JOPT_PASSTHROUGH_DATETIME 512u
#define JOPT_APPEND_NEWLINE 1024u
#define JOPT_PASSTHROUGH_DATACLASS 2048u
#define JOPT_ALL 4095u
/* options that need the general writer for the whole document */
#define JOPT_GENERAL (JOPT_INDENT_2 | JOPT_NON_STR_KEYS | JOPT_SORT_KEYS | JOPT_STRICT_INTEGER)
#define OPT_DEPTH_LIMIT 254

static PyObject *g_ext_enum = NULL;     /* enum.Enum */
static PyObject *g_ext_uuid = NULL;     /* uuid.UUID */
static PyObject *g_ext_dc_field = NULL; /* dataclasses._FIELD */

static PyObject *ext_import(const char *mod, const char *name)
{
    PyObject *m = PyImport_ImportModule(mod);
    PyObject *o;
    if (!m) {
        return NULL;
    }
    o = PyObject_GetAttrString(m, name);
    Py_DECREF(m);
    return o;
}

static int ext_init(void)
{
    if (g_ext_enum) {
        return 0;
    }
    if (!PyDateTimeAPI) {
        PyDateTime_IMPORT;
        if (!PyDateTimeAPI) {
            return -1;
        }
    }
    if (!g_ext_uuid && !(g_ext_uuid = ext_import("uuid", "UUID"))) {
        return -1;
    }
    if (!g_ext_dc_field && !(g_ext_dc_field = ext_import("dataclasses", "_FIELD"))) {
        return -1;
    }
    g_ext_enum = ext_import("enum", "Enum");
    return g_ext_enum ? 0 : -1;
}

static inline char *ext_d2(char *p, int v)
{
    p[0] = (char)('0' + v / 10);
    p[1] = (char)('0' + v % 10);
    return p + 2;
}

static char *ext_fmt_date(char *p, int y, int m, int d)
{
    p = ext_d2(p, y / 100);
    p = ext_d2(p, y % 100);
    *p++ = '-';
    p = ext_d2(p, m);
    *p++ = '-';
    return ext_d2(p, d);
}

static char *ext_fmt_clock(char *p, int h, int mi, int s, int us, unsigned opt)
{
    p = ext_d2(p, h);
    *p++ = ':';
    p = ext_d2(p, mi);
    *p++ = ':';
    p = ext_d2(p, s);
    if (us && !(opt & JOPT_OMIT_MICROSECONDS)) {
        int i;
        *p++ = '.';
        for (i = 5; i >= 0; i--) {
            p[i] = (char)('0' + us % 10);
            us /= 10;
        }
        p += 6;
    }
    return p;
}

/* Text of a datetime / date / time into buf (at least 40 bytes).
 * Returns the length, 0 when o is none of them, -1 on error. */
static int ext_fmt_temporal(PyObject *o, unsigned opt, char *buf)
{
    char *p = buf;
    if (PyDateTime_Check(o)) {
        PyObject *tz = PyDateTime_DATE_GET_TZINFO(o);
        int has_off = 0;
        long off = 0;
        if (tz != Py_None) {
            PyObject *d = PyObject_CallMethod(o, "utcoffset", NULL);
            if (!d) {
                return -1;
            }
            if (d != Py_None) {
                if (!PyDelta_Check(d)) {
                    Py_DECREF(d);
                    PyErr_SetString(PyExc_TypeError, "datetime's timezone library is not supported: use datetime.timezone.utc, pendulum, pytz, or dateutil");
                    return -1;
                }
                off = (long)PyDateTime_DELTA_GET_DAYS(d) * 86400L + PyDateTime_DELTA_GET_SECONDS(d);
                has_off = 1;
            }
            Py_DECREF(d);
        } else if (opt & JOPT_NAIVE_UTC) {
            has_off = 1;
        }
        p = ext_fmt_date(p, PyDateTime_GET_YEAR(o), PyDateTime_GET_MONTH(o), PyDateTime_GET_DAY(o));
        *p++ = 'T';
        p = ext_fmt_clock(p, PyDateTime_DATE_GET_HOUR(o), PyDateTime_DATE_GET_MINUTE(o), PyDateTime_DATE_GET_SECOND(o),
                          PyDateTime_DATE_GET_MICROSECOND(o), opt);
        if (has_off) {
            if (off == 0 && (opt & JOPT_UTC_Z)) {
                *p++ = 'Z';
            } else {
                long a = off < 0 ? -off : off;
                *p++ = off < 0 ? '-' : '+';
                /* seconds round to the nearest minute, the way orjson does it */
                p = ext_d2(p, (int)(a / 3600));
                *p++ = ':';
                p = ext_d2(p, (int)(a % 3600 / 60) + (a % 60 >= 30));
            }
        }
        return (int)(p - buf);
    }
    if (PyDate_Check(o)) {
        p = ext_fmt_date(p, PyDateTime_GET_YEAR(o), PyDateTime_GET_MONTH(o), PyDateTime_GET_DAY(o));
        return (int)(p - buf);
    }
    if (PyTime_Check(o)) {
        if (PyDateTime_TIME_GET_TZINFO(o) != Py_None) {
            PyErr_SetString(PyExc_TypeError, "datetime.time must not have tzinfo set");
            return -1;
        }
        p = ext_fmt_clock(p, PyDateTime_TIME_GET_HOUR(o), PyDateTime_TIME_GET_MINUTE(o), PyDateTime_TIME_GET_SECOND(o),
                          PyDateTime_TIME_GET_MICROSECOND(o), opt);
        return (int)(p - buf);
    }
    return 0;
}

static int ext_put_quoted(Ctx *restrict c, const char *s, size_t n)
{
    RESERVE(c, n + 2);
    *c->cur++ = '"';
    memcpy(c->cur, s, n);
    c->cur += n;
    *c->cur++ = '"';
    return 0;
}

/* dataclass instance -> new dict of its fields, in declaration order */
static PyObject *ext_dataclass_dict(PyObject *o, PyObject *fields)
{
    PyObject *d = PyDict_New();
    PyObject *name;
    PyObject *field;
    Py_ssize_t pos = 0;
    if (!d) {
        return NULL;
    }
    while (PyDict_Next(fields, &pos, &name, &field)) {
        PyObject *ft = PyObject_GetAttrString(field, "_field_type");
        PyObject *v;
        int rc;
        if (!ft) {
            Py_DECREF(d);
            return NULL;
        }
        Py_DECREF(ft);
        if (ft != g_ext_dc_field) {
            continue; /* ClassVar / InitVar */
        }
        if (PyUnicode_Check(name) && PyUnicode_GET_LENGTH(name) > 0 && PyUnicode_READ_CHAR(name, 0) == '_') {
            continue; /* orjson leaves out names with a leading underscore */
        }
        v = PyObject_GetAttr(o, name);
        if (!v) {
            Py_DECREF(d);
            return NULL;
        }
        rc = PyDict_SetItem(d, name, v);
        Py_DECREF(v);
        if (rc < 0) {
            Py_DECREF(d);
            return NULL;
        }
    }
    return d;
}

/* numpy array or scalar -> a new plain Python object with the same content.
 * Returns NULL with an exception, or with none set when o is not numpy. */
static PyObject *ext_numpy(PyObject *o)
{
    PyTypeObject *t = Py_TYPE(o);
    PyObject *dtype;
    PyObject *kind;
    PyObject *res = NULL;
    Py_UCS4 k;
    long size = 0;
    int is_array;
    if (strncmp(t->tp_name, "numpy.", 6) != 0) {
        return NULL;
    }
    is_array = strcmp(t->tp_name, "numpy.ndarray") == 0;
    dtype = PyObject_GetAttrString(o, "dtype");
    if (!dtype) {
        PyErr_Clear();
        return NULL;
    }
    kind = PyObject_GetAttrString(dtype, "kind");
    if (kind) {
        PyObject *isz = PyObject_GetAttrString(dtype, "itemsize");
        if (isz) {
            size = PyLong_AsLong(isz);
            Py_DECREF(isz);
        }
    }
    Py_DECREF(dtype);
    if (!kind || !PyUnicode_Check(kind) || PyUnicode_GET_LENGTH(kind) != 1 || PyErr_Occurred()) {
        Py_XDECREF(kind);
        PyErr_Clear();
        return NULL;
    }
    k = PyUnicode_READ_CHAR(kind, 0);
    Py_DECREF(kind);
    if (is_array) {
        PyObject *nd = PyObject_GetAttrString(o, "ndim");
        long ndim = nd ? PyLong_AsLong(nd) : -1;
        Py_XDECREF(nd);
        if (ndim == 0) {
            PyErr_SetString(PyExc_TypeError, "numpy array is not C contiguous; use ndarray.tolist() in default");
            return NULL;
        }
    }
    if (is_array) {
        PyObject *flags = PyObject_GetAttrString(o, "flags");
        PyObject *cc = flags ? PyObject_GetAttrString(flags, "c_contiguous") : NULL;
        int ok = cc ? PyObject_IsTrue(cc) : -1;
        Py_XDECREF(flags);
        Py_XDECREF(cc);
        if (ok < 0) {
            return NULL;
        }
        if (!ok) {
            PyErr_SetString(PyExc_TypeError, "numpy array is not C contiguous; use ndarray.tolist() in default");
            return NULL;
        }
    }
    if (k == 'b' || k == 'i' || k == 'u' || (k == 'f' && size == 8)) {
        res = PyObject_CallMethod(o, is_array ? "tolist" : "item", NULL);
    } else if (k == 'f' && (size == 4 || size == 2)) {
        /* shortest text of the narrow float, read back as a double */
        PyObject *s = PyObject_CallMethod(o, "astype", "s", "U32");
        if (s) {
            PyObject *d = PyObject_CallMethod(s, "astype", "s", "float64");
            Py_DECREF(s);
            if (d) {
                res = PyObject_CallMethod(d, is_array ? "tolist" : "item", NULL);
                Py_DECREF(d);
            }
        }
    } else if (k == 'M') {
        PyObject *us = PyObject_CallMethod(o, "astype", "s", "datetime64[us]");
        if (us) {
            res = PyObject_CallMethod(us, is_array ? "tolist" : "item", NULL);
            Py_DECREF(us);
        }
    } else {
        PyErr_SetString(PyExc_TypeError, "unsupported datatype in numpy array");
    }
    return res;
}

/* A value whose type is not an exact builtin.
 * 1 = written; 2 = *conv is a new reference to serialize in its place;
 * 3 = same, a dict of dataclass fields (never sorted); 4 = same, from numpy
 * (strict-integer and datetime passthrough do not apply to it);
 * 0 = not one of ours; -1 = error. */
static int ext_try(Ctx *restrict c, PyObject *o, PyObject **conv)
{
    PyTypeObject *t = Py_TYPE(o);
    char buf[48];
    int n;
    if (ext_init() < 0) {
        return -1;
    }
    if (!(c->opt & JOPT_PASSTHROUGH_DATETIME)) {
        n = ext_fmt_temporal(o, c->opt, buf);
        if (n < 0) {
            return -1;
        }
        if (n > 0) {
            return ext_put_quoted(c, buf, (size_t)n) < 0 ? -1 : 1;
        }
    }
    n = PyObject_IsInstance(o, g_ext_enum);
    if (n < 0) {
        return -1;
    }
    if (n) {
        *conv = PyObject_GetAttrString(o, "value");
        return *conv ? 2 : -1;
    }
    if (t == (PyTypeObject *)g_ext_uuid || PyObject_IsInstance(o, g_ext_uuid) == 1) {
        PyObject *s = PyObject_Str(o);
        const char *u;
        Py_ssize_t un;
        int rc;
        if (!s) {
            return -1;
        }
        u = PyUnicode_AsUTF8AndSize(s, &un);
        rc = u ? ext_put_quoted(c, u, (size_t)un) : -1;
        Py_DECREF(s);
        return rc < 0 ? -1 : 1;
    }
    PyErr_Clear();
    if (!(c->opt & JOPT_PASSTHROUGH_DATACLASS) && !PyType_Check(o)) {
        PyObject *fields = PyObject_GetAttrString((PyObject *)t, "__dataclass_fields__");
        if (fields) {
            if (PyDict_Check(fields)) {
                *conv = ext_dataclass_dict(o, fields);
                Py_DECREF(fields);
                return *conv ? 3 : -1;
            }
            Py_DECREF(fields);
        } else {
            PyErr_Clear();
        }
    }
    if (c->opt & JOPT_SERIALIZE_NUMPY) {
        *conv = ext_numpy(o);
        if (*conv) {
            return 4;
        }
        if (PyErr_Occurred()) {
            return -1;
        }
    }
    return 0;
}

static int opt_lit(Ctx *restrict c, PyObject *o)
{
    RESERVE(c, 8);
    if (o == Py_True) {
        memcpy(c->cur, "true", 4);
        c->cur += 4;
    } else if (o == Py_False) {
        memcpy(c->cur, "false", 5);
        c->cur += 5;
    } else {
        memcpy(c->cur, "null", 4);
        c->cur += 4;
    }
    return 0;
}

/* ------------------------------------------------------------ general writer
 * Recursive, one value at a time. Used for the whole document when an option
 * in JOPT_GENERAL is set. */

static int opt_value(Ctx *restrict c, PyObject *o, int depth);

static int opt_newline(Ctx *restrict c, int depth)
{
    size_t n = 1u + 2u * (size_t)depth;
    RESERVE(c, n);
    *c->cur++ = '\n';
    memset(c->cur, ' ', n - 1u);
    c->cur += n - 1u;
    return 0;
}

static int opt_int(Ctx *restrict c, PyObject *o)
{
    if (c->opt & JOPT_STRICT_INTEGER) {
        int overflow = 0;
        long long v = PyLong_AsLongLongAndOverflow(o, &overflow);
        if (v == -1 && !overflow && PyErr_Occurred()) {
            return -1;
        }
        if (overflow || v > 9007199254740991LL || v < -9007199254740991LL) {
            PyErr_SetString(PyExc_TypeError, "Integer exceeds 53-bit range");
            return -1;
        }
    }
    return write_int(c, o);
}

/* dict key as a new str for OPT_NON_STR_KEYS; NULL with an exception otherwise */
static PyObject *opt_key_str(Ctx *restrict c, PyObject *k, int depth)
{
    char buf[48];
    int n;
    if (PyUnicode_CheckExact(k)) {
        return Py_NewRef(k);
    }
    if (!(c->opt & JOPT_NON_STR_KEYS)) {
        PyErr_SetString(PyExc_TypeError, "Dict key must be str");
        return NULL;
    }
    if (k == Py_None) {
        return PyUnicode_FromString("null");
    }
    if (k == Py_True || k == Py_False) {
        return PyUnicode_FromString(k == Py_True ? "true" : "false");
    }
    if (ext_init() < 0) {
        return NULL;
    }
    n = PyObject_IsInstance(k, g_ext_enum);
    if (n < 0) {
        return NULL;
    }
    if (n) {
        PyObject *v = PyObject_GetAttrString(k, "value");
        PyObject *s;
        if (!v) {
            return NULL;
        }
        if (depth >= OPT_DEPTH_LIMIT) {
            Py_DECREF(v);
            PyErr_SetString(PyExc_TypeError, "Recursion limit reached");
            return NULL;
        }
        s = opt_key_str(c, v, depth + 1);
        Py_DECREF(v);
        return s;
    }
    if (PyUnicode_Check(k)) {
        return PyUnicode_FromObject(k);
    }
    if (PyLong_Check(k)) {
        int overflow = 0;
        long long v = PyLong_AsLongLongAndOverflow(k, &overflow);
        if (v == -1 && !overflow && PyErr_Occurred()) {
            return NULL;
        }
        if (overflow < 0 || (overflow > 0 && PyLong_AsUnsignedLongLong(k) == (unsigned long long)-1 && PyErr_Occurred())) {
            PyErr_Clear();
            PyErr_SetString(PyExc_TypeError, "Dict integer key must be within 64-bit range");
            return NULL;
        }
        return PyObject_Str(k);
    }
    if (PyFloat_Check(k)) {
        double d = PyFloat_AS_DOUBLE(k);
        if (d != d || d - d != 0) {
            return PyUnicode_FromString("null");
        }
        n = jf64_write(buf, d);
        return PyUnicode_FromStringAndSize(buf, n);
    }
    n = ext_fmt_temporal(k, c->opt, buf);
    if (n < 0) {
        return NULL;
    }
    if (n > 0) {
        return PyUnicode_FromStringAndSize(buf, n);
    }
    if (PyObject_IsInstance(k, g_ext_uuid) == 1) {
        return PyObject_Str(k);
    }
    PyErr_Clear();
    PyErr_SetString(PyExc_TypeError, "Dict key must a type serializable with OPT_NON_STR_KEYS");
    return NULL;
}

typedef struct {
    PyObject *key; /* str, owned */
    PyObject *val; /* owned */
    const char *u;
    Py_ssize_t n;
    Py_ssize_t idx; /* insertion order: equal keys keep it */
} OptPair;

static int opt_pair_cmp(const void *a, const void *b)
{
    const OptPair *x = (const OptPair *)a;
    const OptPair *y = (const OptPair *)b;
    Py_ssize_t m = x->n < y->n ? x->n : y->n;
    int r = memcmp(x->u, y->u, (size_t)m);
    if (r) {
        return r;
    }
    if (x->n != y->n) {
        return x->n < y->n ? -1 : 1;
    }
    return x->idx < y->idx ? -1 : (x->idx > y->idx);
}

static int opt_dict(Ctx *restrict c, PyObject *o, int depth, int sort)
{
    Py_ssize_t n = PyDict_GET_SIZE(o);
    Py_ssize_t cnt = 0;
    Py_ssize_t pos = 0;
    Py_ssize_t i;
    PyObject *k;
    PyObject *v;
    OptPair *ps;
    int rc = -1;
    RESERVE(c, 2);
    if (n == 0) {
        *c->cur++ = '{';
        *c->cur++ = '}';
        return 0;
    }
    /* a snapshot first: default= may run arbitrary code while we write */
    ps = (OptPair *)PyMem_Malloc((size_t)n * sizeof(OptPair));
    if (!ps) {
        PyErr_NoMemory();
        return -1;
    }
    while (cnt < n && PyDict_Next(o, &pos, &k, &v)) {
        PyObject *ks = opt_key_str(c, k, depth);
        if (!ks) {
            goto done;
        }
        ps[cnt].key = ks;
        ps[cnt].val = Py_NewRef(v);
        ps[cnt].u = PyUnicode_AsUTF8AndSize(ks, &ps[cnt].n);
        ps[cnt].idx = cnt;
        cnt++;
        if (!ps[cnt - 1].u) {
            if (PyErr_ExceptionMatches(PyExc_UnicodeEncodeError)) {
                PyErr_Clear();
                PyErr_SetString(PyExc_TypeError, "str is not valid UTF-8: surrogates not allowed");
            }
            goto done;
        }
    }
    if (sort && (c->opt & JOPT_SORT_KEYS)) {
        qsort(ps, (size_t)cnt, sizeof(OptPair), opt_pair_cmp);
    }
    *c->cur++ = '{';
    for (i = 0; i < cnt; i++) {
        if (i) {
            RESERVE_OR(c, 1, goto done);
            *c->cur++ = ',';
        }
        if ((c->opt & JOPT_INDENT_2) && opt_newline(c, depth + 1) < 0) {
            goto done;
        }
        if (write_str(c, ps[i].key) < 0) {
            goto done;
        }
        RESERVE_OR(c, 2, goto done);
        *c->cur++ = ':';
        if (c->opt & JOPT_INDENT_2) {
            *c->cur++ = ' ';
        }
        if (opt_value(c, ps[i].val, depth + 1) < 0) {
            goto done;
        }
    }
    if ((c->opt & JOPT_INDENT_2) && opt_newline(c, depth) < 0) {
        goto done;
    }
    RESERVE_OR(c, 1, goto done);
    *c->cur++ = '}';
    rc = 0;
done:
    for (i = 0; i < cnt; i++) {
        Py_DECREF(ps[i].key);
        Py_DECREF(ps[i].val);
    }
    PyMem_Free(ps);
    return rc;
}

static int opt_seq(Ctx *restrict c, PyObject *o, int is_list, int depth)
{
    Py_ssize_t i;
    RESERVE(c, 2);
    if ((is_list ? PyList_GET_SIZE(o) : PyTuple_GET_SIZE(o)) == 0) {
        *c->cur++ = '[';
        *c->cur++ = ']';
        return 0;
    }
    *c->cur++ = '[';
    /* the size is read again each time: default= may change a list under us */
    for (i = 0; i < (is_list ? PyList_GET_SIZE(o) : PyTuple_GET_SIZE(o)); i++) {
        PyObject *v = is_list ? PyList_GET_ITEM(o, i) : PyTuple_GET_ITEM(o, i);
        int rc;
        if (i) {
            RESERVE(c, 1);
            *c->cur++ = ',';
        }
        if ((c->opt & JOPT_INDENT_2) && opt_newline(c, depth + 1) < 0) {
            return -1;
        }
        Py_INCREF(v);
        rc = opt_value(c, v, depth + 1);
        Py_DECREF(v);
        if (rc < 0) {
            return -1;
        }
    }
    if ((c->opt & JOPT_INDENT_2) && opt_newline(c, depth) < 0) {
        return -1;
    }
    RESERVE(c, 1);
    *c->cur++ = ']';
    return 0;
}

static int opt_value(Ctx *restrict c, PyObject *o, int depth)
{
    int n_default = 0;
    PyObject *held = NULL; /* a converted value we own */
    int rc;
    int sort = 1;
    if (depth > OPT_DEPTH_LIMIT) {
        PyErr_SetString(PyExc_TypeError, "Recursion limit reached");
        return -1;
    }
    for (;;) {
        PyTypeObject *t = Py_TYPE(o);
        int exact = t == &PyUnicode_Type || t == &PyLong_Type || t == &PyFloat_Type || t == &PyBool_Type ||
                    o == Py_None || t == &PyList_Type || t == &PyTuple_Type || t == &PyDict_Type;
        int sub = exact || !(c->opt & JOPT_PASSTHROUGH_SUBCLASS);
        PyObject *conv = NULL;
        if (!exact) {
            rc = ext_try(c, o, &conv);
            if (rc == 1) {
                rc = 0;
                break;
            }
            if (rc == 2 || rc == 3) {
                sort = rc == 2;
                Py_XDECREF(held);
                held = o = conv;
                continue;
            }
            if (rc == 4) {
                unsigned save = c->opt;
                c->opt &= ~(JOPT_STRICT_INTEGER | JOPT_PASSTHROUGH_DATETIME);
                rc = opt_value(c, conv, depth);
                c->opt = save;
                Py_DECREF(conv);
                break;
            }
            if (rc < 0) {
                break;
            }
        }
        if (t == &PyUnicode_Type || (sub && PyUnicode_Check(o))) {
            rc = write_str(c, o);
        } else if (o == Py_True || o == Py_False || o == Py_None) {
            rc = opt_lit(c, o);
        } else if (t == &PyLong_Type || (sub && PyLong_Check(o))) {
            rc = opt_int(c, o);
        } else if (t == &PyFloat_Type) {
            rc = write_float(c, PyFloat_AS_DOUBLE(o));
        } else if (t == &PyList_Type || (sub && PyList_Check(o))) {
            rc = opt_seq(c, o, 1, depth);
        } else if (t == &PyTuple_Type) {
            rc = opt_seq(c, o, 0, depth);
        } else if (t == &PyDict_Type || (sub && PyDict_Check(o))) {
            rc = opt_dict(c, o, depth, sort);
        } else if (!c->default_fn) {
            type_not_serializable(o);
            rc = -1;
        } else if (n_default >= DEFAULT_RECURSE_LIMIT) {
            PyErr_SetString(PyExc_TypeError, "default serializer exceeds recursion limit");
            rc = -1;
        } else {
            conv = PyObject_CallOneArg(c->default_fn, o);
            if (!conv) {
                rc = -1;
                break;
            }
            Py_XDECREF(held);
            held = o = conv;
            n_default++;
            continue;
        }
        break;
    }
    Py_XDECREF(held);
    return rc;
}
