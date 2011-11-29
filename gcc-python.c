/*
   Copyright 2011 David Malcolm <dmalcolm@redhat.com>
   Copyright 2011 Red Hat, Inc.

   This is free software: you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see
   <http://www.gnu.org/licenses/>.
*/

#include <Python.h>
#include <structseq.h>
#include "gcc-python.h"

#include "gcc-python-closure.h"
#include "gcc-python-wrappers.h"

int plugin_is_GPL_compatible;

#include "plugin-version.h"

#include "tree.h"
#include "function.h"
#include "diagnostic.h"
#include "cgraph.h"
#include "opts.h"

#include "c-family/c-pragma.h" /* for parse_in */

#if 0
#define LOG(msg) \
    (void)fprintf(stderr, "%s:%i:%s\n", __FILE__, __LINE__, (msg))
#else
#define LOG(msg) ((void)0);
#endif


#define GCC_PYTHON_TRACE_ALL_EVENTS 0
#if GCC_PYTHON_TRACE_ALL_EVENTS
static const char* event_name[] = {
#define DEFEVENT(NAME) \
  #NAME, 
# include "plugin.def"
# undef DEFEVENT
};

/*
  Notes on the passes

  As of 2011-03-30, http://gcc.gnu.org/onlinedocs/gccint/Plugins.html doesn't
  seem to document the type of the gcc_data passed to each callback.

  For reference, with gcc-4.6.0-0.15.fc15.x86_64 the types seem to be as
  follows:

  PLUGIN_ATTRIBUTES:
    gcc_data=0x0
    Called from: init_attributes () at ../../gcc/attribs.c:187
    However, it seems at this point to have initialized these:
      static const struct attribute_spec *attribute_tables[4];
      static htab_t attribute_hash;

  PLUGIN_PRAGMAS:
    gcc_data=0x0
    Called from: c_common_init () at ../../gcc/c-family/c-opts.c:1052

  PLUGIN_START_UNIT:
    gcc_data=0x0
    Called from: compile_file () at ../../gcc/toplev.c:573

  PLUGIN_PRE_GENERICIZE
    gcc_data is:  tree fndecl;
    Called from: finish_function () at ../../gcc/c-decl.c:8323

  PLUGIN_OVERRIDE_GATE
    gcc_data:
      &gate_status
      bool gate_status;
    Called from : execute_one_pass (pass=0x1011340) at ../../gcc/passes.c:1520

  PLUGIN_PASS_EXECUTION
    gcc_data: struct opt_pass *pass
    Called from: execute_one_pass (pass=0x1011340) at ../../gcc/passes.c:1530

  PLUGIN_ALL_IPA_PASSES_START
    gcc_data=0x0
    Called from: ipa_passes () at ../../gcc/cgraphunit.c:1779

  PLUGIN_EARLY_GIMPLE_PASSES_START
    gcc_data=0x0
    Called from: execute_ipa_pass_list (pass=0x1011fa0) at ../../gcc/passes.c:1927

  PLUGIN_EARLY_GIMPLE_PASSES_END
    gcc_data=0x0
    Called from: execute_ipa_pass_list (pass=0x1011fa0) at ../../gcc/passes.c:1930

  PLUGIN_ALL_IPA_PASSES_END
    gcc_data=0x0
    Called from: ipa_passes () at ../../gcc/cgraphunit.c:1821

  PLUGIN_ALL_PASSES_START
    gcc_data=0x0
    Called from: tree_rest_of_compilation (fndecl=0x7ffff16b1f00) at ../../gcc/tree-optimize.c:420

  PLUGIN_ALL_PASSES_END
    gcc_data=0x0
    Called from: tree_rest_of_compilation (fndecl=0x7ffff16b1f00) at ../../gcc/tree-optimize.c:425

  PLUGIN_FINISH_UNIT
    gcc_data=0x0
    Called from: compile_file () at ../../gcc/toplev.c:668

  PLUGIN_FINISH
    gcc_data=0x0
    Called from: toplev_main (argc=17, argv=0x7fffffffdfc8) at ../../gcc/toplev.c:1970

  PLUGIN_FINISH_TYPE
    gcc_data=tree
    Called from c_parser_declspecs (parser=0x7fffef559730, specs=0x15296d0, scspec_ok=1 '\001', typespec_ok=1 '\001', start_attr_ok=<optimized out>, la=cla_nonabstract_decl) at ../../gcc/c-parser.c:2111

  PLUGIN_PRAGMA
    gcc_data=0x0
    Called from: init_pragma at ../../gcc/c-family/c-pragma.c:1321
    to  "Allow plugins to register their own pragmas."
*/

static void
trace_callback(enum plugin_event event, void *gcc_data, void *user_data)
{
    fprintf(stderr,
	    "%s:%i:trace_callback(%s, %p, %p)\n",
	    __FILE__, __LINE__, event_name[event], gcc_data, user_data);
    fprintf(stderr, "  cfun:%p\n", cfun);
}

#define DEFEVENT(NAME) \
static void trace_callback_for_##NAME(void *gcc_data, void *user_data) \
{ \
     trace_callback(NAME, gcc_data, user_data); \
}
# include "plugin.def"
# undef DEFEVENT
#endif /* GCC_PYTHON_TRACE_ALL_EVENTS */

static enum plugin_event current_event = GCC_PYTHON_PLUGIN_BAD_EVENT;

int gcc_python_is_within_event(enum plugin_event *out_event)
{
    if (current_event != GCC_PYTHON_PLUGIN_BAD_EVENT) {
        if (out_event) {
            *out_event = current_event;
        }
        return 1;
    } else {
        return 0;
    }
}


static void
gcc_python_finish_invoking_callback(PyGILState_STATE gstate,
                                    int expect_wrapped_data, PyObject *wrapped_gcc_data,
                                    void *user_data)
    CPYCHECKER_STEALS_REFERENCE_TO_ARG(3) /* wrapped_gcc_data */ ;

static void
gcc_python_finish_invoking_callback(PyGILState_STATE gstate,
                                    int expect_wrapped_data, PyObject *wrapped_gcc_data,
                                    void *user_data)
{
    struct callback_closure *closure = (struct callback_closure *)user_data;
    PyObject *args = NULL;
    PyObject *result = NULL;
    location_t saved_loc = input_location;
    enum plugin_event saved_event;

    assert(closure);
    /* We take ownership of wrapped_gcc_data.
       For some callbacks types it will always be NULL; for others, it's only
       NULL if an error has occurred: */
    if (expect_wrapped_data && !wrapped_gcc_data) {
        goto cleanup;
    }

    if (cfun) {
        /* Temporarily override input_location to the top of the function: */
        input_location = cfun->function_start_locus;
    }

    args = gcc_python_closure_make_args(closure, 1, wrapped_gcc_data);
    if (!args) {
        goto cleanup;
    }

    saved_event = current_event;
    current_event = closure->event;

    result = PyObject_Call(closure->callback, args, closure->kwargs);

    current_event = saved_event;

    if (!result) {
        /* Treat an unhandled Python error as a compilation error: */
        gcc_python_print_exception("Unhandled Python exception raised within callback");
    }

    // FIXME: the result is ignored

cleanup:
    Py_XDECREF(wrapped_gcc_data);
    Py_XDECREF(args);
    Py_XDECREF(result);

    PyGILState_Release(gstate);
    input_location = saved_loc;
}

/*
  C-level callbacks for each event ID follow, thunking into the registered
  Python callable.

  There's some repetition here, but it can be easier to debug if you have
  separate breakpoint locations for each event ID.
 */

static void
gcc_python_callback_for_tree(void *gcc_data, void *user_data)
{
    PyGILState_STATE gstate;
    tree t = (tree)gcc_data;

    gstate = PyGILState_Ensure();

    gcc_python_finish_invoking_callback(gstate, 
					1, gcc_python_make_wrapper_tree(t),
					user_data);
}


static void
gcc_python_callback_for_PLUGIN_ATTRIBUTES(void *gcc_data, void *user_data)
{
    PyGILState_STATE gstate;

    //printf("%s:%i:(%p, %p)\n", __FILE__, __LINE__, gcc_data, user_data);
    assert(pass);

    gstate = PyGILState_Ensure();

    gcc_python_finish_invoking_callback(gstate,
                                        0, NULL,
                                        user_data);
}

static void
gcc_python_callback_for_PLUGIN_PASS_EXECUTION(void *gcc_data, void *user_data)
{
    PyGILState_STATE gstate;
    struct opt_pass *pass = (struct opt_pass *)gcc_data;

    //printf("%s:%i:(%p, %p)\n", __FILE__, __LINE__, gcc_data, user_data);
    assert(pass);

    gstate = PyGILState_Ensure();

    gcc_python_finish_invoking_callback(gstate, 
					1, gcc_python_make_wrapper_pass(pass),
					user_data);
}



static void
gcc_python_callback_for_FINISH_UNIT(void *gcc_data, void *user_data)
{
    PyGILState_STATE gstate;

    gstate = PyGILState_Ensure();

    gcc_python_finish_invoking_callback(gstate,
					0, NULL,
					user_data);
}



static PyObject*
gcc_python_register_callback(PyObject *self, PyObject *args, PyObject *kwargs)
{
    int event;
    PyObject *callback = NULL;
    PyObject *extraargs = NULL;
    struct callback_closure *closure;

    if (!PyArg_ParseTuple(args, "iO|O:register_callback", &event, &callback, &extraargs)) {
        return NULL;
    }

    //printf("%s:%i:gcc_python_register_callback\n", __FILE__, __LINE__);

    closure = gcc_python_closure_new_for_plugin_event(callback, extraargs, kwargs,
                                                      (enum plugin_event)event);
    if (!closure) {
        return PyErr_NoMemory();
    }

    switch ((enum plugin_event)event) {
    case PLUGIN_ATTRIBUTES:
        register_callback("python", // FIXME
			  (enum plugin_event)event,
			  gcc_python_callback_for_PLUGIN_ATTRIBUTES,
			  closure);
	break;

    case PLUGIN_PRE_GENERICIZE:
        register_callback("python", // FIXME
			  (enum plugin_event)event,
			  gcc_python_callback_for_tree,
			  closure);
	break;
	
    case PLUGIN_PASS_EXECUTION:
        register_callback("python", // FIXME
			  (enum plugin_event)event,
			  gcc_python_callback_for_PLUGIN_PASS_EXECUTION,
			  closure);
	break;

    case PLUGIN_FINISH_UNIT:
        register_callback("python", // FIXME
			  (enum plugin_event)event,
			  gcc_python_callback_for_FINISH_UNIT,
			  closure);
	break;

    case PLUGIN_FINISH_TYPE:
        register_callback("python", // FIXME
			  (enum plugin_event)event,
			  gcc_python_callback_for_tree,
			  closure);
	break;

    default:
        PyErr_Format(PyExc_ValueError, "event type %i invalid (or not wired up yet)", event);
	return NULL;
    }
    
    Py_RETURN_NONE;
}

static PyObject*
gcc_python_define_macro(PyObject *self,
                        PyObject *args, PyObject *kwargs)
{
    const char *macro;
    char *keywords[] = {"macro",
                        NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwargs,
                                     "s:define_preprocessor_name", keywords,
                                     &macro)) {
        return NULL;
    }

    if (0) {
        fprintf(stderr, "gcc.define_macro(\"%s\")\n", macro);
    }

    if (!parse_in) {
        return PyErr_Format(PyExc_ValueError,
                            "gcc.define_macro(\"%s\") called without a compilation unit",
                            macro);
    }

    if (!gcc_python_is_within_event(NULL)) {
        return PyErr_Format(PyExc_ValueError,
                            "gcc.define_macro(\"%s\") called from outside an event callback",
                            macro);
    }

    cpp_define (parse_in, macro);

    Py_RETURN_NONE;
}

/*
  I initially attempted to directly wrap gcc's:
    diagnostic_report_diagnostic()
  given that that seems to be the abstraction within gcc/diagnostic.h: all
  instances of (struct diagnostic_info) within the gcc source tree seem to
  be allocated on the stack, within functions exposed in gcc/diagnostic.h

  However, diagnostic_report_diagnostic() ultimately calls into the
  pretty-printing routines, trying to format varargs, which doesn't make much
  sense for us: we have first-class string objects and string formatting at
  the python level.

  Thus we instead just wrap "error_at" and its analogs
*/

static PyObject*
gcc_python_permerror(PyObject *self, PyObject *args)
{
    PyGccLocation *loc_obj = NULL;
    const char *msgid = NULL;
    PyObject *result_obj = NULL;
    bool result_b;

    if (!PyArg_ParseTuple(args,
			  "O!"
			  "s"
			  ":permerror",
			  &gcc_LocationType, &loc_obj,
			  &msgid)) {
        return NULL;
    }

    /* Invoke the GCC function: */
    result_b = permerror(loc_obj->loc, "%s", msgid);

    result_obj = PyBool_FromLong(result_b);

    return result_obj;
}

static PyObject *
gcc_python_error(PyObject *self, PyObject *args, PyObject *kwargs)
{
    PyGccLocation *loc_obj;
    const char *msg;
    char *keywords[] = {"location",
                        "message",
                        NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwargs,
                                     "O!s:error", keywords,
                                     &gcc_LocationType, &loc_obj,
                                     &msg)) {
        return NULL;
    }

    error_at(loc_obj->loc, "%s", msg);

    Py_RETURN_NONE;
}

static PyObject *
gcc_python_warning(PyObject *self, PyObject *args, PyObject *kwargs)
{
    PyGccLocation *loc_obj;
    PyGccOption *opt_obj;
    const char *msg;
    char *keywords[] = {"location",
                        "option",
                        "message",
                        NULL};
    bool was_reported;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs,
                                     "O!O!s:warning", keywords,
                                     &gcc_LocationType, &loc_obj,
                                     &gcc_OptionType, &opt_obj,
                                     &msg)) {
        return NULL;
    }

    /* Ugly workaround; see this function: */
    if (0 == gcc_python_option_is_enabled(opt_obj->opt_code)) {
        return PyBool_FromLong(0);
    }

    was_reported = warning_at(loc_obj->loc, opt_obj->opt_code, "%s", msg);

    return PyBool_FromLong(was_reported);
}

static PyObject *
gcc_python_inform(PyObject *self, PyObject *args, PyObject *kwargs)
{
    PyGccLocation *loc_obj;
    const char *msg;
    char *keywords[] = {"location",
                        "message",
                        NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwargs,
                                     "O!s:inform", keywords,
                                     &gcc_LocationType, &loc_obj,
                                     &msg)) {
        return NULL;
    }

    inform(loc_obj->loc, "%s", msg);

    Py_RETURN_NONE;
}

static PyObject *
gcc_python_set_location(PyObject *self, PyObject *args)
{
    PyGccLocation *loc_obj;
    if (!PyArg_ParseTuple(args,
                          "O!:set_location",
                          &gcc_LocationType, &loc_obj)) {
        return NULL;
    }

    input_location = loc_obj->loc;

    Py_RETURN_NONE;
}

static PyObject *
gcc_python_get_option_list(PyObject *self, PyObject *args)
{
    PyObject *result;
    int i;

    result = PyList_New(0);
    if (!result) {
	goto error;
    }

    for (i = 0; i < cl_options_count; i++) {
	PyObject *opt_obj = gcc_python_make_wrapper_opt_code((enum opt_code)i);
	if (!opt_obj) {
	    goto error;
	}
	if (-1 == PyList_Append(result, opt_obj)) {
	    Py_DECREF(opt_obj);
	    goto error;
	}
    }

    return result;

 error:
    Py_XDECREF(result);
    return NULL;
}

static PyObject *
gcc_python_get_option_dict(PyObject *self, PyObject *args)
{
    PyObject *dict;
    size_t i;

    dict = PyDict_New();
    if (!dict) {
	goto error;
    }

    for (i = 0; i < cl_options_count; i++) {
	PyObject *opt_obj = gcc_python_make_wrapper_opt_code((enum opt_code)i);
        if (!opt_obj) {
	    goto error;
        }
        if (-1 == PyDict_SetItemString(dict,
                                       cl_options[i].opt_text,
                                       opt_obj)) {
	    Py_DECREF(opt_obj);
	    goto error;
	}
    }

    return dict;

 error:
    Py_XDECREF(dict);
    return NULL;
}

static PyObject *
gcc_python_get_parameters(PyObject *self, PyObject *args)
{
    PyObject *dict;
    size_t i;

    dict = PyDict_New();
    if (!dict) {
	goto error;
    }

    for (i = 0; i < get_num_compiler_params(); i++) {
        PyObject *param_obj = gcc_python_make_wrapper_param_num(i);
        if (!param_obj) {
	    goto error;
        }
        if (-1 == PyDict_SetItemString(dict,
                                       compiler_params[i].option,
                                       param_obj)) {
	    Py_DECREF(param_obj);
	    goto error;
	}
    }

    return dict;

 error:
    Py_XDECREF(dict);
    return NULL;
}

static PyObject *
gcc_python_get_variables(PyObject *self, PyObject *args)
{
    PyObject *result;
    struct varpool_node *n;

    result = PyList_New(0);
    if (!result) {
	goto error;
    }

    for (n = varpool_nodes; n; n = n->next) {
	PyObject *obj_var = gcc_python_make_wrapper_variable(n);
	if (!obj_var) {
	    goto error;
	}
	if (-1 == PyList_Append(result, obj_var)) {
	    Py_DECREF(obj_var);
	    goto error;
	}
        Py_DECREF(obj_var);
    }

    return result;

 error:
    Py_XDECREF(result);
    return NULL;
}

static PyObject *
gcc_python_maybe_get_identifier(PyObject *self, PyObject *args)
{
    const char *str;
    tree t;

    if (!PyArg_ParseTuple(args,
			  "s:maybe_get_identifier",
			  &str)) {
	return NULL;
    }

    t = maybe_get_identifier(str);
    return gcc_python_make_wrapper_tree(t);
}

/*
  get_translation_units was made globally visible in gcc revision 164331:
    http://gcc.gnu.org/ml/gcc-cvs/2010-09/msg00625.html
    http://gcc.gnu.org/viewcvs?view=revision&revision=164331
*/
static PyObject *
gcc_python_get_translation_units(PyObject *self, PyObject *args)
{
    return VEC_tree_as_PyList(all_translation_units);
}

/* Version handling: */

/* Define a gcc.Version type, as a structseq */
static struct PyStructSequence_Field gcc_version_fields[] = {
    {"basever", NULL},
    {"datestamp", NULL},
    {"devphase", NULL},
    {"revision", NULL},
    {"configuration_arguments", NULL},
    {0}
};

static struct PyStructSequence_Desc gcc_version_desc = {
    "gcc.Version", /* name */
    NULL, /* doc */
    gcc_version_fields,
    5
};

PyTypeObject GccVersionType;

static PyObject *
gcc_version_to_object(struct plugin_gcc_version *version)
{
    PyObject *obj = PyStructSequence_New(&GccVersionType);
    if (!obj) {
        return NULL;
    }

#define SET_ITEM(IDX, FIELD) \
    PyStructSequence_SET_ITEM(obj, (IDX), gcc_python_string_or_none(version->FIELD));

    SET_ITEM(0, basever);
    SET_ITEM(1, datestamp);
    SET_ITEM(2, devphase);
    SET_ITEM(3, revision);
    SET_ITEM(4, configuration_arguments);

#undef SET_ITEM

    return obj;
}

static PyObject *
gcc_python_get_plugin_gcc_version(PyObject *self, PyObject *args)
{
    /*
       "gcc_version" is compiled in to the plugin, as part of
       plugin-version.h:
    */
    return gcc_version_to_object(&gcc_version);
}

static struct plugin_gcc_version *actual_gcc_version;

static PyObject *
gcc_python_get_gcc_version(PyObject *self, PyObject *args)
{
    /*
       "actual_gcc_version" is passed in when the plugin is initialized
    */
    return gcc_version_to_object(actual_gcc_version);
}

static PyObject *
gcc_python_get_callgraph_nodes(PyObject *self, PyObject *args)
{
    PyObject *result;
    struct cgraph_node *node;

    /* For debugging, see GCC's dump of things: */
    if (0) {
        fprintf(stderr, "----------------BEGIN----------------\n");
        dump_cgraph (stderr);
        fprintf(stderr, "---------------- END ----------------\n");
    }

    result = PyList_New(0);
    if (!result) {
	goto error;
    }

    for (node = cgraph_nodes; node; node = node->next) {
	PyObject *obj_var = gcc_python_make_wrapper_cgraph_node(node);
	if (!obj_var) {
	    goto error;
	}
	if (-1 == PyList_Append(result, obj_var)) {
	    Py_DECREF(obj_var);
	    goto error;
	}
        Py_DECREF(obj_var);
    }

    return result;

 error:
    Py_XDECREF(result);
    return NULL;
}

/* Dump files */

static PyObject *
gcc_python_dump(PyObject *self, PyObject *arg)
{
    PyObject *str_obj;
    /*
       gcc/output.h: declares:
           extern FILE *dump_file;
       This is NULL when not defined.
    */
    if (!dump_file) {
        /* The most common case; make it fast */
        Py_RETURN_NONE;
    }

    str_obj = PyObject_Str(arg);
    if (!str_obj) {
        return NULL;
    }

    /* FIXME: encoding issues */
    /* FIXME: GIL */
    if (!fwrite(gcc_python_string_as_string(str_obj),
                strlen(gcc_python_string_as_string(str_obj)),
                1,
                dump_file)) {
        Py_DECREF(str_obj);
        return PyErr_SetFromErrnoWithFilename(PyExc_IOError, dump_file_name);
    }

    Py_DECREF(str_obj);

    Py_RETURN_NONE;
}

static PyObject *
gcc_python_get_dump_file_name(PyObject *self, PyObject *noargs)
{
    /* gcc/tree-pass.h declares:
        extern const char *dump_file_name;
    */
    return gcc_python_string_or_none(dump_file_name);
}

static PyObject *
gcc_python_get_dump_base_name(PyObject *self, PyObject *noargs)
{
    /*
      The generated gcc/options.h has:
          #ifdef GENERATOR_FILE
          extern const char *dump_base_name;
          #else
            const char *x_dump_base_name;
          #define dump_base_name global_options.x_dump_base_name
          #endif
    */
    return gcc_python_string_or_none(dump_base_name);
}

static PyMethodDef GccMethods[] = {
    {"register_attribute",
     (PyCFunction)gcc_python_register_attribute,
     (METH_VARARGS | METH_KEYWORDS),
     "Register an attribute."},

    {"register_callback",
     (PyCFunction)gcc_python_register_callback,
     (METH_VARARGS | METH_KEYWORDS),
     "Register a callback, to be called when various GCC events occur."},

    {"define_macro",
     (PyCFunction)gcc_python_define_macro,
     (METH_VARARGS | METH_KEYWORDS),
     "Pre-define a named value in the preprocessor."},

    /* Diagnostics: */
    {"permerror", gcc_python_permerror, METH_VARARGS,
     NULL},
    {"error",
     (PyCFunction)gcc_python_error,
     (METH_VARARGS | METH_KEYWORDS),
     ("Report an error\n"
      "FIXME\n")},
    {"warning",
     (PyCFunction)gcc_python_warning,
     (METH_VARARGS | METH_KEYWORDS),
     ("Report a warning\n"
      "FIXME\n")},
    {"inform",
     (PyCFunction)gcc_python_inform,
     (METH_VARARGS | METH_KEYWORDS),
     ("Report an information message\n"
      "FIXME\n")},
    {"set_location",
     (PyCFunction)gcc_python_set_location,
     METH_VARARGS,
     ("Temporarily set the default location for error reports\n")},

    /* Options: */
    {"get_option_list",
     gcc_python_get_option_list,
     METH_VARARGS,
     "Get all command-line options, as a list of gcc.Option instances"},

    {"get_option_dict",
     gcc_python_get_option_dict,
     METH_VARARGS,
     ("Get all command-line options, as a dict from command-line text strings "
      "to gcc.Option instances")},

    {"get_parameters", gcc_python_get_parameters, METH_VARARGS,
     "Get all tunable GCC parameters.  Returns a dictionary, mapping from"
     "option name -> gcc.Parameter instance"},

    {"get_variables", gcc_python_get_variables, METH_VARARGS,
     "Get all variables in this compilation unit as a list of gcc.Variable"},

    {"maybe_get_identifier", gcc_python_maybe_get_identifier, METH_VARARGS,
     "Get the gcc.IdentifierNode with this name, if it exists, otherwise None"},

    {"get_translation_units", gcc_python_get_translation_units, METH_VARARGS,
     "Get a list of all gcc.TranslationUnitDecl"},

    /* Version handling: */
    {"get_plugin_gcc_version", gcc_python_get_plugin_gcc_version, METH_VARARGS,
     "Get the gcc.Version that this plugin was compiled with"},

    {"get_gcc_version", gcc_python_get_gcc_version, METH_VARARGS,
     "Get the gcc.Version for this version of GCC"},

    {"get_callgraph_nodes", gcc_python_get_callgraph_nodes, METH_VARARGS,
     "Get a list of all gcc.CallgraphNode instances"},

    /* Dump files */
    {"dump", gcc_python_dump, METH_O,
     "Dump str() of the argument to the current dump file (or silently discard it when no dump file is open)"},

    {"get_dump_file_name", gcc_python_get_dump_file_name, METH_NOARGS,
     "Get the name of the current dump file (or None)"},

    {"get_dump_base_name", gcc_python_get_dump_base_name, METH_NOARGS,
     "Get the base name used when writing dump files"},

    /* Sentinel: */
    {NULL, NULL, 0, NULL}
};

static struct 
{
    PyObject *module;
    PyObject *argument_dict;
    PyObject *argument_tuple;
} gcc_python_globals;

#if PY_MAJOR_VERSION == 3
static struct PyModuleDef gcc_module_def = {
    PyModuleDef_HEAD_INIT,
    "gcc",   /* name of module */
    NULL,
    -1,
    GccMethods
};
#endif

static PyMODINIT_FUNC PyInit_gcc(void)
{
#if PY_MAJOR_VERSION == 3
    PyObject *m;
    m = PyModule_Create(&gcc_module_def);
#else
    Py_InitModule("gcc", GccMethods);
#endif

#if PY_MAJOR_VERSION == 3
    return m;
#endif
}

static int
gcc_python_init_gcc_module(struct plugin_name_args *plugin_info)
{
    int i;

    if (!gcc_python_globals.module) {
        return 0;
    }

    /* Set up int constants for each of the enum plugin_event values: */
    #define DEFEVENT(NAME) \
       PyModule_AddIntMacro(gcc_python_globals.module, NAME);
    # include "plugin.def"
    # undef DEFEVENT

    gcc_python_globals.argument_dict = PyDict_New();
    if (!gcc_python_globals.argument_dict) {
        return 0;
    }

    gcc_python_globals.argument_tuple = PyTuple_New(plugin_info->argc);
    if (!gcc_python_globals.argument_tuple) {
        return 0;
    }

    /* PySys_SetArgvEx(plugin_info->argc, plugin_info->argv, 0); */
    for (i=0; i<plugin_info->argc; i++) {
	struct plugin_argument *arg = &plugin_info->argv[i];
        PyObject *key;
        PyObject *value;
	PyObject *pair;
      
	key = gcc_python_string_from_string(arg->key);
	if (arg->value) {
            value = gcc_python_string_from_string(plugin_info->argv[i].value);
	} else {
  	    value = Py_None;
	}
        PyDict_SetItem(gcc_python_globals.argument_dict, key, value);
	// FIXME: ref counts?

	pair = Py_BuildValue("(s, s)", arg->key, arg->value);
	if (!pair) {
  	    return 1;
	}
        PyTuple_SetItem(gcc_python_globals.argument_tuple, i, pair);

    }
    PyModule_AddObject(gcc_python_globals.module, "argument_dict", gcc_python_globals.argument_dict);
    PyModule_AddObject(gcc_python_globals.module, "argument_tuple", gcc_python_globals.argument_tuple);

    /* Pass properties: */
    PyModule_AddIntMacro(gcc_python_globals.module, PROP_gimple_any);
    PyModule_AddIntMacro(gcc_python_globals.module, PROP_gimple_lcf);
    PyModule_AddIntMacro(gcc_python_globals.module, PROP_gimple_leh);
    PyModule_AddIntMacro(gcc_python_globals.module, PROP_cfg);
    PyModule_AddIntMacro(gcc_python_globals.module, PROP_referenced_vars);
    PyModule_AddIntMacro(gcc_python_globals.module, PROP_ssa);
    PyModule_AddIntMacro(gcc_python_globals.module, PROP_no_crit_edges);
    PyModule_AddIntMacro(gcc_python_globals.module, PROP_rtl);
    PyModule_AddIntMacro(gcc_python_globals.module, PROP_gimple_lomp);
    PyModule_AddIntMacro(gcc_python_globals.module, PROP_cfglayout);
    PyModule_AddIntMacro(gcc_python_globals.module, PROP_gimple_lcx);

    /* Success: */
    return 1;
}

static void gcc_python_run_any_command(void)
{
    PyObject* command_obj; /* borrowed ref */
    int result;
    const char *command_str;

    command_obj = PyDict_GetItemString(gcc_python_globals.argument_dict, "command");
    if (!command_obj) {
        return;
    }

    command_str = gcc_python_string_as_string(command_obj);

    if (0) {
        fprintf(stderr, "Running: %s\n", command_str);
    }

    result = PyRun_SimpleString(command_str);
    if (-1 == result) {
        /* Error running the python command */
        Py_Finalize();
        exit(1);
    }
}

static void gcc_python_run_any_script(void)
{
    PyObject* script_name;
    FILE *fp;
    int result;
  
    script_name = PyDict_GetItemString(gcc_python_globals.argument_dict, "script");
    if (!script_name) {
        return;
    }

    fp = fopen(gcc_python_string_as_string(script_name), "r");
    if (!fp) {
        fprintf(stderr,
		"Unable to read python script: %s\n",
                gcc_python_string_as_string(script_name));
	exit(1);
    }
    result = PyRun_SimpleFile(fp, gcc_python_string_as_string(script_name));
    fclose(fp);
    if (-1 == result) {
        /* Error running the python script */
        Py_Finalize();
        exit(1);
    }
}

int
setup_sys(struct plugin_name_args *plugin_info)
{
    /*

     * Sets up "sys.plugin_full_name" as plugin_info->full_name.  This is the
     path to the plugin (as specified with -fplugin=)

     * Sets up "sys.plugin_base_name" as plugin_info->base_name.  This is the
     short name, of the plugin (filename without .so suffix)

     * Add the directory containing the plugin to "sys.path", so that it can
     find modules relative to itself without needing PYTHONPATH to be set up.
     (sys.path has already been initialized by the call to Py_Initialize)

     * If PLUGIN_PYTHONPATH is defined, add it to "sys.path"

    */
    int result = 0; /* failure */
    PyObject *full_name = NULL;
    PyObject *base_name = NULL;
    const char *program =
      "import sys;\n"
      "import os;\n"
      "sys.path.append(os.path.abspath(os.path.dirname(sys.plugin_full_name)))\n";

    /* Setup "sys.plugin_full_name" */
    full_name = gcc_python_string_from_string(plugin_info->full_name);
    if (!full_name) {
        goto error;
    }
    if (-1 == PySys_SetObject("plugin_full_name", full_name)) {
        goto error;
    }

    /* Setup "sys.plugin_base_name" */
    base_name = gcc_python_string_from_string(plugin_info->base_name);
    if (!base_name) {
        goto error;
    }
    if (-1 == PySys_SetObject("plugin_base_name", base_name)) {
        goto error;
    }

    /* Add the plugin's path to sys.path */
    if (-1 == PyRun_SimpleString(program)) {
        goto error;
    }

#ifdef PLUGIN_PYTHONPATH
    {
        /*
           Support having multiple builds of the plugin installed independently
           of each other, by supporting each having a directory for support
           files e.g. gccutils, libcpychecker, etc

           We do this by seeing if PLUGIN_PYTHONPATH was defined in the
           compile, and if so, adding it to sys.path:
        */
        const char *program2 =
            "import sys;\n"
            "import os;\n"
            "sys.path.append('" PLUGIN_PYTHONPATH "')\n";

        if (-1 == PyRun_SimpleString(program2)) {
            goto error;
        }
    }
#endif

    /* Success: */
    result = 1;

 error:
    Py_XDECREF(full_name);
    Py_XDECREF(base_name);
    return result;
}

/*
  Wired up to PLUGIN_FINISH, this callback handles finalization for the plugin:
*/
void
on_plugin_finish(void *gcc_data, void *user_data)
{
    /*
       Clean up the python runtime.

       For python 3, this flushes buffering of sys.stdout and sys.stderr
    */
    Py_Finalize();
}

extern int
plugin_init (struct plugin_name_args *plugin_info,
             struct plugin_gcc_version *version) __attribute__((nonnull));

int
plugin_init (struct plugin_name_args *plugin_info,
             struct plugin_gcc_version *version)
{
    LOG("plugin_init started");

    if (!plugin_default_version_check (version, &gcc_version)) {
        return 1;
    }
    actual_gcc_version = version;

#if PY_MAJOR_VERSION >= 3
    /*
      Python 3 added internal buffering to sys.stdout and sys.stderr, but this
      leads to unpredictable interleavings of messages from gcc, such as from calling
      gcc.warning() vs those from python scripts, such as from print() and
      sys.stdout.write()

      Suppress the buffering, to better support mixed gcc/python output:
    */
    Py_UnbufferedStdioFlag = 1;
#endif

    PyImport_AppendInittab("gcc", PyInit_gcc);

    LOG("calling Py_Initialize...");

    Py_Initialize();

    LOG("Py_Initialize finished");

    gcc_python_globals.module = PyImport_ImportModule("gcc");

    PyEval_InitThreads();
  
    if (!gcc_python_init_gcc_module(plugin_info)) {
        return 1;
    }

    if (!setup_sys(plugin_info)) {
        return 1;
    }

    /* Init other modules */
    /* FIXME: properly integrate them within the module hierarchy */

    PyStructSequence_InitType(&GccVersionType, &gcc_version_desc);

    autogenerated_callgraph_init_types();  /* FIXME: error checking! */
    autogenerated_cfg_init_types();  /* FIXME: error checking! */
    autogenerated_function_init_types();  /* FIXME: error checking! */
    autogenerated_gimple_init_types();  /* FIXME: error checking! */
    autogenerated_location_init_types();  /* FIXME: error checking! */
    autogenerated_option_init_types();  /* FIXME: error checking! */
    autogenerated_parameter_init_types();  /* FIXME: error checking! */
    autogenerated_pass_init_types();  /* FIXME: error checking! */
    autogenerated_pretty_printer_init_types();  /* FIXME: error checking! */
    autogenerated_rtl_init_types(); /* FIXME: error checking! */
    autogenerated_tree_init_types(); /* FIXME: error checking! */
    autogenerated_variable_init_types(); /* FIXME: error checking! */



    autogenerated_callgraph_add_types(gcc_python_globals.module);
    autogenerated_cfg_add_types(gcc_python_globals.module);
    autogenerated_function_add_types(gcc_python_globals.module);
    autogenerated_gimple_add_types(gcc_python_globals.module);
    autogenerated_location_add_types(gcc_python_globals.module);
    autogenerated_option_add_types(gcc_python_globals.module);
    autogenerated_parameter_add_types(gcc_python_globals.module);
    autogenerated_pass_add_types(gcc_python_globals.module);
    autogenerated_pretty_printer_add_types(gcc_python_globals.module);
    autogenerated_rtl_add_types(gcc_python_globals.module);
    autogenerated_tree_add_types(gcc_python_globals.module);
    autogenerated_variable_add_types(gcc_python_globals.module);


    /* Register at-exit finalization for the plugin: */
    register_callback(plugin_info->base_name, PLUGIN_FINISH,
                      on_plugin_finish, NULL);

    gcc_python_run_any_command();
    gcc_python_run_any_script();

    //printf("%s:%i:got here\n", __FILE__, __LINE__);

#if GCC_PYTHON_TRACE_ALL_EVENTS
#define DEFEVENT(NAME) \
    if (NAME != PLUGIN_PASS_MANAGER_SETUP &&         \
        NAME != PLUGIN_INFO &&                       \
	NAME != PLUGIN_REGISTER_GGC_ROOTS &&         \
	NAME != PLUGIN_REGISTER_GGC_CACHES) {        \
    register_callback(plugin_info->base_name, NAME,  \
		      trace_callback_for_##NAME, NULL); \
    }
# include "plugin.def"
# undef DEFEVENT
#endif /* GCC_PYTHON_TRACE_ALL_EVENTS */

    LOG("init_plugin finished");

    return 0;
}

PyObject *
gcc_python_string_or_none(const char *str_or_null)
{
    if (str_or_null) {
	return gcc_python_string_from_string(str_or_null);
    } else {
	Py_RETURN_NONE;
    }
}

/*
  "double_int" is declared in gcc/double-int.h as a pair of HOST_WIDE_INT.
  These in turn are defined in gcc/hwint.h as a #define to one of "long",
  "long long", or "__int64".

  It appears that they can be interpreted as either "unsigned" or "signed"

  How to convert this to other types?
  We "cheat", and take it through the decimal representation, then convert
  from decimal.  This is probably slow, but is (I hope) at least correct.
*/
void
gcc_python_double_int_as_text(double_int di, bool is_unsigned,
                              char *out, int bufsize)
{
    FILE *f;
    assert(out);
    assert(bufsize > 256); /* FIXME */

    out[0] = '\0';
    f = fmemopen(out, bufsize, "w");
    dump_double_int (f, di, is_unsigned);
    fclose(f);
}

PyObject *
gcc_python_int_from_double_int(double_int di, bool is_unsigned)
{
    PyObject *long_obj;
#if PY_MAJOR_VERSION < 3
    long long_val;
    int overflow;
#endif
    char buf[512]; /* FIXME */
    gcc_python_double_int_as_text(di, is_unsigned, buf, sizeof(buf));

    long_obj = PyLong_FromString(buf, NULL, 10);
    if (!long_obj) {
        return NULL;
    }
#if PY_MAJOR_VERSION >= 3
    return long_obj;
#else
    long_val = PyLong_AsLongAndOverflow(long_obj, &overflow);
    if (overflow) {
        /* Doesn't fit in a PyIntObject; use the PyLongObject: */
        return long_obj;
    } else {
        /* Fits in a PyIntObject: use that */
        PyObject *int_obj = PyInt_FromLong(long_val);
        if (!int_obj) {
            return long_obj;
        }
        Py_DECREF(long_obj);
        return int_obj;
    }
#endif
}

/*
   GCC's headers "poison" strdup to make it unavailable, so we provide our own.

   The buffer is allocated using PyMem_Malloc
*/
char *
gcc_python_strdup(const char *str)
{
    char *result;
    char *dst;

    result = (char*)PyMem_Malloc(strlen(str) + 1);

    if (!result) {
        return NULL;
    }

    dst = result;
    while (*str) {
        *(dst++) = *(str++);
    }
    *dst = '\0';

    return result;
}

void gcc_python_print_exception(const char *msg)
{
    /* Handler for Python exceptions */
    assert(msg);

    /*
       Emit a gcc error, using GCC's "input_location"

       Typically, by the time our code is running, that's generally just the
       end of the source file.

       The value is saved and restored whenever calling into Python code, and
       within passes is initialized to the top of the function; it can be
       temporarily overridden using gcc.set_location()
    */
    error_at(input_location, "%s", msg);

    /* Print the traceback: */
    PyErr_PrintEx(1);
}

/*
  PEP-7
Local variables:
c-basic-offset: 4
indent-tabs-mode: nil
End:
*/
