/* Version strings exported by the module. */
#ifndef JSON_BUILD_H
#define JSON_BUILD_H

#ifndef JSON_BUILD_REV
#define JSON_BUILD_REV "4"
#endif
#ifndef JSON_BUILD_NAME
#define JSON_BUILD_NAME "prod"
#endif
#define OXJSON_VERSION "0.4.0"

static int json_build_export(PyObject *m)
{
    if (PyModule_AddStringConstant(m, "__version__", OXJSON_VERSION) < 0) {
        return -1;
    }
    if (PyModule_AddStringConstant(m, "build_rev", JSON_BUILD_REV) < 0) {
        return -1;
    }
    if (PyModule_AddStringConstant(m, "build_name", JSON_BUILD_NAME) < 0) {
        return -1;
    }
    return 0;
}

#endif
