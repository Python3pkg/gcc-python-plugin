#include <Python.h>
#include "gcc-python.h"
#include "gcc-python-wrappers.h"
#include "gimple.h"

/*
  "location_t" is the type used throughout.  Might be nice to expose this directly.

  input.h has: 
    typedef source_location location_t;

  line-map.h has:
      A logical line/column number, i.e. an "index" into a line_map:
          typedef unsigned int source_location;

*/

PyObject *
gcc_Location_repr(struct PyGccLocation * self)
{
     return PyString_FromFormat("gcc.Location(file='%s', line=%i)",
				LOCATION_FILE(self->loc),
				LOCATION_LINE(self->loc));
}

PyObject *
gcc_Location_str(struct PyGccLocation * self)
{
     return PyString_FromFormat("%s:%i",
				LOCATION_FILE(self->loc),
				LOCATION_LINE(self->loc));
}

PyObject *
gcc_python_make_wrapper_location(location_t loc)
{
    struct PyGccLocation *location_obj = NULL;
  
    location_obj = PyObject_New(struct PyGccLocation, &gcc_LocationType);
    if (!location_obj) {
        goto error;
    }

    location_obj->loc = loc;
    /* FIXME: do we need to do something for the GCC GC? */

    return (PyObject*)location_obj;
      
error:
    return NULL;
}

PyObject*
gcc_python_make_wrapper_gimple(gimple stmt)
{
    struct PyGccGimple *gimple_obj = NULL;
  
    gimple_obj = PyObject_New(struct PyGccGimple, &gcc_GimpleType);
    if (!gimple_obj) {
        goto error;
    }

    gimple_obj->stmt = stmt;
    /* FIXME: do we need to do something for the GCC GC? */

    return (PyObject*)gimple_obj;
      
error:
    return NULL;
}


//#include "rtl.h"
/*
  "struct rtx_def" is declarted within rtl.h, c.f:
    struct GTY((chain_next ("RTX_NEXT (&%h)"),
	    chain_prev ("RTX_PREV (&%h)"), variable_size)) rtx_def {
           ... snip ...
    }
    
  and seems to be the fundamental instruction type
    
  Seems to use RTX_NEXT() and RTX_PREV()
*/

#include "basic-block.h"

/*
  "struct edge_def" is declared in basic-block.h, c.f:
      struct GTY(()) edge_def {
           ... snip ...
      }
  and there are these typedefs to pointers defined in coretypes.h:
      typedef struct edge_def *edge;
      typedef const struct edge_def *const_edge;
 */
PyObject *
gcc_python_make_wrapper_edge(edge e)
{
    struct PyGccEdge *obj;

    if (!e) {
	Py_RETURN_NONE;
    }

    obj = PyObject_New(struct PyGccEdge, &gcc_EdgeType);
    if (!obj) {
        goto error;
    }

    obj->e = e;
    /* FIXME: do we need to do something for the GCC GC? */

    return (PyObject*)obj;
      
error:
    return NULL;
}

/*
  "struct basic_block_def" is declared in basic-block.h, c.f:
      struct GTY((chain_next ("%h.next_bb"), chain_prev ("%h.prev_bb"))) basic_block_def {
           ... snip ...
      }
  and there are these typedefs to pointers defined in coretypes.h:
      typedef struct basic_block_def *basic_block;
      typedef const struct basic_block_def *const_basic_block;
 */
PyObject *    
VEC_edge_as_PyList(VEC(edge,gc) *vec_edges)
{
    PyObject *result = NULL;
    int i;
    edge e;
    
    result = PyList_New(VEC_length(edge, vec_edges));
    if (!result) {
	goto error;
    }

    FOR_EACH_VEC_ELT(edge, vec_edges, i, e) {
	PyObject *item;
	item = gcc_python_make_wrapper_edge(e);
	if (!item) {
	    goto error;
	}
	PyList_SetItem(result, i, item);
    }

    return result;

 error:
    Py_XDECREF(result);
    return NULL;
}


PyObject *
gcc_BasicBlock_get_preds(PyGccBasicBlock *self, void *closure)
{
    return VEC_edge_as_PyList(self->bb->preds);
}

PyObject *
gcc_BasicBlock_get_succs(PyGccBasicBlock *self, void *closure)
{
    return VEC_edge_as_PyList(self->bb->succs);
}

PyObject *
gcc_BasicBlock_get_gimple(PyGccBasicBlock *self, void *closure)
{
    gimple_stmt_iterator gsi;
    PyObject *result = NULL;

    assert(self);
    assert(self->bb);

    printf("gcc_BasicBlock_get_gimple\n");
    
    if (self->bb->flags & BB_RTL) {
	Py_RETURN_NONE;
    }

    if (NULL == self->bb->il.gimple) {
	Py_RETURN_NONE;
    }

    /* FIXME: what about phi_nodes? */
    result = PyList_New(0);
    if (!result) {
	goto error;
    }

    for (gsi = gsi_start (self->bb->il.gimple->seq);
	 !gsi_end_p (gsi);
	 gsi_next (&gsi)) {

	gimple stmt = gsi_stmt(gsi);
	PyObject *obj_stmt;

	obj_stmt = gcc_python_make_wrapper_gimple(stmt);
	if (!obj_stmt) {
	    goto error;
	}

	if (PyList_Append(result, obj_stmt)) {
	    goto error;
	}

	printf("  gimple: %p code: %s (%i) %s:%i num_ops=%i\n", 
	       stmt,
	       gimple_code_name[gimple_code(stmt)],
	       gimple_code(stmt),
	       gimple_filename(stmt),
	       gimple_lineno(stmt),
	       gimple_num_ops(stmt));
    }

    return result;

 error:
    Py_XDECREF(result);
    return NULL;    
}

/*
  Force a 1-1 mapping between pointer values and wrapper objects
 */
PyObject *
gcc_python_lazily_create_wrapper(PyObject **cache,
				 void *ptr,
				 PyObject *(*ctor)(void *ptr))
{
    PyObject *key = NULL;
    PyObject *oldobj = NULL;
    PyObject *newobj = NULL;

    /* printf("gcc_python_lazily_create_wrapper(&%p, %p, %p)\n", *cache, ptr, ctor); */

    assert(cache);
    /* ptr is allowed to be NULL */
    assert(ctor);

    /* The cache is lazily created: */
    if (!*cache) {
	*cache = PyDict_New();
	if (!*cache) {
	    return NULL;
	}
    }

    key = PyLong_FromVoidPtr(ptr);
    if (!key) {
	return NULL;
    }

    oldobj = PyDict_GetItem(*cache, key);
    if (oldobj) {
	/* The cache already contains an object wrapping "ptr": reuse t */
	/* printf("reusing %p for %p\n", oldobj, ptr); */
	Py_INCREF(oldobj); /* it was a borrowed ref */
	Py_DECREF(key);
	return oldobj;
    }

    /* 
       Not in the cache: we don't yet have a wrapper object for this pointer
    */
       
    assert(NULL != key); /* we own a ref */
    assert(NULL == oldobj);
    assert(NULL == newobj);

    /* Construct a wrapper : */

    newobj = (*ctor)(ptr);
    if (!newobj) {
	Py_DECREF(key);
	return NULL;
    }

    /* printf("created %p for %p\n", newobj, ptr); */

    if (PyDict_SetItem(*cache, key, newobj)) {
	Py_DECREF(newobj);
	Py_DECREF(key);
	return NULL;
    }

    Py_DECREF(key);
    return newobj;
}


static PyObject *
real_make_basic_block_wrapper(void *ptr)
{
    basic_block bb = (basic_block)ptr;
    struct PyGccBasicBlock *obj;

    if (!bb) {
	Py_RETURN_NONE;
    }

    obj = PyObject_New(struct PyGccBasicBlock, &gcc_BasicBlockType);
    if (!obj) {
        goto error;
    }

#if 1
    printf("bb: %p\n", bb);
    printf("bb->flags: 0x%x\n", bb->flags);
    printf("bb->flags & BB_RTL: %i\n", bb->flags & BB_RTL);
    if (bb->flags & BB_RTL) {
	printf("bb->il.rtl: %p\n", bb->il.rtl);
    } else {
	printf("bb->il.gimple: %p\n", bb->il.gimple);
	if (bb->il.gimple) {
	    /* 
	       See http://gcc.gnu.org/onlinedocs/gccint/GIMPLE.html
	       and also gimple-pretty-print.c

	       coretypes.h has:
	           struct gimple_seq_d;
		   typedef struct gimple_seq_d *gimple_seq;

	       and gimple.h has:
   	           "A double-linked sequence of gimple statements."
		   struct GTY ((chain_next ("%h.next_free"))) gimple_seq_d {
                        ... snip ...
		   }
               and:
		   struct gimple_seq_node_d;
		   typedef struct gimple_seq_node_d *gimple_seq_node;
	       and:
	           struct GTY((chain_next ("%h.next"), chain_prev ("%h.prev"))) gimple_seq_node_d {
		       gimple stmt;
		       struct gimple_seq_node_d *prev;
		       struct gimple_seq_node_d *next;
		   };
	       and coretypes.h has:
	           union gimple_statement_d;
		   typedef union gimple_statement_d *gimple;
	       and gimple.h has the "union gimple_statement_d", and another
	       set of codes for this
	    */

	    printf("bb->il.gimple->seq: %p\n", bb->il.gimple->seq);
	    printf("bb->il.gimple->phi_nodes: %p\n", bb->il.gimple->phi_nodes);

	    {
		gimple_stmt_iterator i;
		
		for (i = gsi_start (bb->il.gimple->seq); !gsi_end_p (i); gsi_next (&i)) {
		    gimple stmt = gsi_stmt(i);
		    printf("  gimple: %p code: %s (%i) %s:%i num_ops=%i\n", 
			   stmt,
			   gimple_code_name[gimple_code(stmt)],
			   gimple_code(stmt),
			   gimple_filename(stmt),
			   gimple_lineno(stmt),
			   gimple_num_ops(stmt));
		    //print_generic_stmt (stderr, stmt, 0);
		}
	    }

	}
    }
#endif

    obj->bb = bb;
    /* FIXME: do we need to do something for the GCC GC? */

    return (PyObject*)obj;
      
error:
    return NULL;
}

static PyObject *basic_block_wrapper_cache = NULL;
PyObject *
gcc_python_make_wrapper_basic_block(basic_block bb)
{
    return gcc_python_lazily_create_wrapper(&basic_block_wrapper_cache,
					    bb,
					    real_make_basic_block_wrapper);
}

/*
  "struct control_flow_graph" is declared in basic-block.h, c.f.:
      struct GTY(()) control_flow_graph {
           ... snip ...
      }
*/
PyObject *
gcc_Cfg_get_basic_blocks(PyGccCfg *self, void *closure)
{
    PyObject *result = NULL;
    int i;
    
    result = PyList_New(self->cfg->x_n_basic_blocks);
    if (!result) {
	goto error;
    }

    for (i = 0; i < self->cfg->x_n_basic_blocks; i++) {
	PyObject *item;
	item = gcc_python_make_wrapper_basic_block(VEC_index(basic_block, self->cfg->x_basic_block_info, i));
	if (!item) {
	    goto error;
	}
	PyList_SetItem(result, i, item);
    }

    return result;

 error:
    Py_XDECREF(result);
    return NULL;
}

PyObject *
gcc_python_make_wrapper_cfg(struct control_flow_graph *cfg)
{
    struct PyGccCfg *obj;

    if (!cfg) {
	Py_RETURN_NONE;
    }

    obj = PyObject_New(struct PyGccCfg, &gcc_CfgType);
    if (!obj) {
        goto error;
    }

    obj->cfg = cfg;
    /* FIXME: do we need to do something for the GCC GC? */

    return (PyObject*)obj;
      
error:
    return NULL;
}


/*
  "struct function" is declared in function.h, c.f.:
      struct GTY(()) function {
           ... snip ...
      };
*/

#include "function.h"

PyObject *
gcc_Function_repr(struct PyGccFunction * self)
{
     PyObject *name = NULL;
     PyObject *result = NULL;
     tree decl;

     assert(self->fun);
     decl = self->fun->decl;
     if (DECL_NAME(decl)) {
	 name = PyString_FromString(IDENTIFIER_POINTER (DECL_NAME(decl)));
     } else {
	 name = PyString_FromString("(unnamed)");
     }

     if (!name) {
         goto error;
     }

     result = PyString_FromFormat("gcc.Function('%s')",
				  PyString_AsString(name));
     Py_DECREF(name);

     return result;
error:
     Py_XDECREF(name);
     Py_XDECREF(result);
     return NULL;
}

PyObject *
gcc_python_make_wrapper_function(struct function *fun)
{
    struct PyGccFunction *obj;

    if (!fun) {
	Py_RETURN_NONE;
    }

#if 0
    printf("gcc_python_make_wrapper_function(%p)\n", fun);
    
    printf("struct eh_status *eh: %p\n", fun->eh);
    printf("struct control_flow_graph *cfg: %p\n", fun->cfg);
    printf("struct gimple_seq_d *gimple_body: %p\n", fun->gimple_body);
    printf("struct gimple_df *gimple_df: %p\n", fun->gimple_df);
    printf("struct loops *x_current_loops: %p\n", fun->x_current_loops);
    printf("struct stack_usage *su: %p\n", fun->su);
 #if 0
    printf("htab_t GTY((skip)) value_histogram\n");
    printf("tree decl;\n");
    printf("tree static_chain_decl;\n");
    printf("tree nonlocal_goto_save_area;\n");
    printf("VEC(tree,gc) *local_decls: local_decls;\n");
    printf("struct machine_function * GTY ((maybe_undef)) machine;\n");
    printf("struct language_function * language;\n");
    printf("htab_t GTY ((param_is (union tree_node))) used_types_hash;\n");
    printf("int last_stmt_uid;\n");
    printf("int funcdef_no;\n");
    printf("location_t function_start_locus;\n");
    printf("location_t function_end_locus;\n");
    printf("unsigned int curr_properties;\n");
    printf("unsigned int last_verified;\n");
    printf("const char * GTY((skip)) cannot_be_copied_reason;\n");

    printf("unsigned int va_list_gpr_size : 8;\n");
    printf("unsigned int va_list_fpr_size : 8;\n");
    printf("unsigned int calls_setjmp : 1;\n");
    printf("unsigned int calls_alloca : 1;\n");
    printf("unsigned int has_nonlocal_label : 1;\n");
    printf("unsigned int cannot_be_copied_set : 1;\n");
    printf("unsigned int stdarg : 1;\n");
    printf("unsigned int dont_save_pending_sizes_p : 1;\n");
    printf("unsigned int after_inlining : 1;\n");
    printf("unsigned int always_inline_functions_inlined : 1;\n");
    printf("unsigned int can_throw_non_call_exceptions : 1;\n");

    printf("unsigned int returns_struct : 1;\n");
    printf("unsigned int returns_pcc_struct : 1;\n");
    printf("unsigned int after_tree_profile : 1;\n");
    printf("unsigned int has_local_explicit_reg_vars : 1;\n");
    printf("unsigned int is_thunk : 1;\n");
 #endif
#endif

    obj = PyObject_New(struct PyGccFunction, &gcc_FunctionType);
    if (!obj) {
        goto error;
    }

    obj->fun = fun;
    /* FIXME: do we need to do something for the GCC GC? */

    return (PyObject*)obj;
      
error:
    return NULL;
}

/*
    Code for various tree types
 */
PyObject *
gcc_Declaration_repr(struct PyGccTree * self)
{
     PyObject *name = NULL;
     PyObject *result = NULL;

     name = gcc_Declaration_get_name(self, NULL);
     if (!name) {
         goto error;
     }

     result = PyString_FromFormat("gcc.Declaration('%s')",
				  PyString_AsString(name));
     Py_DECREF(name);

     return result;
error:
     Py_XDECREF(name);
     Py_XDECREF(result);
     return NULL;
     
}

/* 
   GCC's debug_tree is implemented in:
     gcc/print-tree.c
   e.g. in:
     /usr/src/debug/gcc-4.6.0-20110321/gcc/print-tree.c
   and appears to be a good place to look when figuring out how the tree data
   works.

   FIXME: do we want a unique PyGccTree per tree address? (e.g. by maintaining a dict?)
   (what about lifetimes?)
*/
PyObject *
gcc_python_make_wrapper_tree(tree t)
{
    struct PyGccTree *tree_obj = NULL;
    PyTypeObject* tp;
  
    tp = gcc_python_autogenerated_tree_type_for_tree(t);
    assert(tp);
    printf("tp:%p\n", tp);
    
    tree_obj = PyObject_New(struct PyGccTree, tp);
    if (!tree_obj) {
        goto error;
    }

    tree_obj->t = t;
    /* FIXME: do we need to do something for the GCC GC? */

    return (PyObject*)tree_obj;
      
error:
    return NULL;
}

/*
  PEP-7  
Local variables:
c-basic-offset: 4
End:
*/
