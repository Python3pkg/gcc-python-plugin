#   Copyright 2011 David Malcolm <dmalcolm@redhat.com>
#   Copyright 2011 Red Hat, Inc.
#
#   This is free software: you can redistribute it and/or modify it
#   under the terms of the GNU General Public License as published by
#   the Free Software Foundation, either version 3 of the License, or
#   (at your option) any later version.
#
#   This program is distributed in the hope that it will be useful, but
#   WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
#   General Public License for more details.
#
#   You should have received a copy of the GNU General Public License
#   along with this program.  If not, see
#   <http://www.gnu.org/licenses/>.

# Attempt to check that C code is implementing CPython's reference-counting
# rules.  See:
#   http://docs.python.org/c-api/intro.html#reference-counts
# for a description of how such code is meant to be written

import sys
import gcc

from gccutils import cfg_to_dot, invoke_dot, get_src_for_loc, check_isinstance

from libcpychecker.absinterp import *
from libcpychecker.diagnostics import Reporter, Annotator, Note
from libcpychecker.PyArg_ParseTuple import log
from libcpychecker.types import is_py3k, is_debug_build

def stmt_is_assignment_to_count(stmt):
    if hasattr(stmt, 'lhs'):
        if stmt.lhs:
            if isinstance(stmt.lhs, gcc.ComponentRef):
                # print 'stmt.lhs.target: %s' % stmt.lhs.target
                # print 'stmt.lhs.target.type: %s' % stmt.lhs.target.type
                # (presumably we need to filter these to structs that are
                # PyObject, or subclasses)
                if stmt.lhs.field.name == 'ob_refcnt':
                    return True

def type_is_pyobjptr(t):
    assert t is None or isinstance(t, gcc.Type)
    if str(t) == 'struct PyObject *':
        return True

def type_is_pyobjptr_subclass(t):
    assert t is None or isinstance(t, gcc.Type)
    # It must be a pointer:
    if not isinstance(t, gcc.PointerType):
        return False

    # ...to a struct:
    if not isinstance(t.dereference, gcc.RecordType):
        return False

    fieldnames = [f.name for f in t.dereference.fields]
    if is_py3k():
        # For Python 3, the first field must be "ob_base", or it must be "PyObject":
        if str(t) == 'struct PyObject *':
            return True
        if fieldnames[0] != 'ob_base':
            return False
    else:
        # For Python 2, the first two fields must be "ob_refcnt" and "ob_type".
        # (In a debug build, these are preceded by _ob_next and _ob_prev)
        # FIXME: debug builds!
        if is_debug_build():
            if fieldnames[:4] != ['_ob_next', '_ob_prev',
                                  'ob_refcnt', 'ob_type']:
                return False
        else:
            if fieldnames[:2] != ['ob_refcnt', 'ob_type']:
                return False

    # Passed all tests:
    return True

def stmt_is_assignment_to_objptr(stmt):
    if hasattr(stmt, 'lhs'):
        if stmt.lhs:
            if type_is_pyobjptr(stmt.lhs.type):
                return True

def stmt_is_return_of_objptr(stmt):
    if isinstance(stmt, gcc.GimpleReturn):
        if stmt.retval:
            if type_is_pyobjptr(stmt.retval.type):
                return True

class RefcountValue(AbstractValue):
    """
    Value for an ob_refcnt field.

    'relvalue' is all of the references owned within this function.

    'min_external' is a lower bound on all references owned outside the
    scope of this function.

    The actual value of ob_refcnt >= (relvalue + min_external)

    Examples:

      - an argument passed in a a borrowed ref starts with (0, 1), in that
      the function doesn't own any refs on it, but it has a refcount of at
      least 1, due to refs we know nothing about.

      - a newly constructed object gets (1, 0): we own a reference on it,
      and we don't know if there are any external refs on it.
    """
    def __init__(self, relvalue, min_external):
        self.relvalue = relvalue
        self.min_external = min_external

    @classmethod
    def new_ref(cls):
        return RefcountValue(relvalue=1,
                             min_external=0)

    @classmethod
    def borrowed_ref(cls):
        return RefcountValue(relvalue=0,
                             min_external=1)

    def __str__(self):
        return 'refs: %i + N where N >= %i' % (self.relvalue, self.min_external)

    def __repr__(self):
        return 'RefcountValue(%i, %i)' % (self.relvalue, self.min_external)

class GenericTpDealloc(AbstractValue):
    """
    A function pointer that points to a "typical" tp_dealloc callback
    i.e. one that frees up the underlying memory
    """
    def get_transitions_for_function_call(self, state, stmt):
        check_isinstance(state, State)
        check_isinstance(stmt, gcc.GimpleCall)
        returntype = stmt.fn.type.dereference.type

        # Mark the arg as being deallocated:
        value = state.eval_rvalue(stmt.args[0], stmt.loc)
        check_isinstance(value, PointerToRegion)
        region = value.region
        check_isinstance(region, Region)
        log('generic tp_dealloc called for %s', region)

        # Get the description of the region before trashing it:
        desc = 'calling tp_dealloc on %s' % region
        result = state.make_assignment(stmt.lhs,
                                       UnknownValue(returntype, stmt.loc),
                                       'calling tp_dealloc on %s' % region)
        new = state.copy()
        new.loc = state.loc.next_loc()

        # Mark the region as deallocated
        # Since regions are shared with other states, we have to set this up
        # for this state by assigning it with a special "DeallocatedMemory"
        # value
        # Clear the value for any fields within the region:
        for k, v in region.fields.items():
            if v in new.value_for_region:
                del new.value_for_region[v]
        # Set the default value for the whole region to be "DeallocatedMemory"
        new.region_for_var[region] = region
        new.value_for_region[region] = DeallocatedMemory(None, stmt.loc)

        return [Transition(state, new, desc)]

class MyState(State):
    def __init__(self, loc, region_for_var, value_for_region, return_rvalue, owned_refs, resources, exception_rvalue):
        State.__init__(self, loc, region_for_var, value_for_region, return_rvalue)
        self.owned_refs = owned_refs
        self.resources = resources
        self.exception_rvalue = exception_rvalue

    def copy(self):
        c = self.__class__(self.loc,
                           self.region_for_var.copy(),
                           self.value_for_region.copy(),
                           self.return_rvalue,
                           self.owned_refs[:],
                           self.resources.copy(),
                           self.exception_rvalue)
        if hasattr(self, 'fun'):
            c.fun = self.fun
        return c

    def _extra(self):
        return ' %s' % self.owned_refs

    def acquire(self, resource):
        self.resources.acquire(resource)

    def release(self, resource):
        self.resources.release(resource)

    def init_for_function(self, fun):
        log('MyState.init_for_function(%r)', fun)
        State.init_for_function(self, fun)

        # Initialize PyObject* arguments to sane values
        # (assume that they're non-NULL)
        nonnull_args = get_nonnull_arguments(fun.decl.type)
        for idx, parm in enumerate(fun.decl.arguments):
            region = self.eval_lvalue(parm, None)
            if type_is_pyobjptr_subclass(parm.type):
                # We have a PyObject* (or a derived class)
                log('got python obj arg: %r', region)
                # Assume it's a non-NULL ptr:
                objregion = Region('region-for-arg-%s' % parm, None)
                self.region_for_var[objregion] = objregion
                self.value_for_region[region] = PointerToRegion(parm.type,
                                                                parm.location,
                                                                objregion)
                # Assume we have a borrowed reference:
                ob_refcnt = self.make_field_region(objregion, 'ob_refcnt') # FIXME: this should be a memref and fieldref
                self.value_for_region[ob_refcnt] = RefcountValue.borrowed_ref()

                # Assume it has a non-NULL ob_type:
                ob_type = self.make_field_region(objregion, 'ob_type')
                typeobjregion = Region('region-for-type-of-arg-%s' % parm, None)
                self.value_for_region[ob_type] = PointerToRegion(get_PyTypeObject().pointer,
                                                                 parm.location,
                                                                 typeobjregion)
        self.verify()

    def make_assignment(self, key, value, desc, additional_ptr=None):
        if desc:
            check_isinstance(desc, str)
        transition = State.make_assignment(self, key, value, desc)
        if additional_ptr:
            transition.dest.owned_refs.append(additional_ptr)
        return transition

    def get_transitions(self):
        # Return a list of Transition instances, based on input State
        stmt = self.loc.get_stmt()
        if stmt:
            return self._get_transitions_for_stmt(stmt)
        else:
            result = []
            for loc in self.loc.next_locs():
                newstate = self.copy()
                newstate.loc = loc
                result.append(Transition(self, newstate, ''))
            log('result: %s', result)
            return result

    def _get_transitions_for_stmt(self, stmt):
        log('_get_transitions_for_stmt: %r %s', stmt, stmt)
        log('dir(stmt): %s', dir(stmt))
        if stmt.loc:
            gcc.set_location(stmt.loc)
        if isinstance(stmt, gcc.GimpleCall):
            return self._get_transitions_for_GimpleCall(stmt)
        elif isinstance(stmt, (gcc.GimpleDebug, gcc.GimpleLabel)):
            return [Transition(self,
                               self.use_next_loc(),
                               None)]
        elif isinstance(stmt, gcc.GimpleCond):
            return self._get_transitions_for_GimpleCond(stmt)
        elif isinstance(stmt, gcc.GimpleReturn):
            return self._get_transitions_for_GimpleReturn(stmt)
        elif isinstance(stmt, gcc.GimpleAssign):
            return self._get_transitions_for_GimpleAssign(stmt)
        elif isinstance(stmt, gcc.GimpleSwitch):
            return self._get_transitions_for_GimpleSwitch(stmt)
        else:
            raise NotImplementedError("Don't know how to cope with %r (%s) at %s"
                                      % (stmt, stmt, stmt.loc))

    def set_exception(self, exc_name):
        """
        Given the name of a (PyObject*) global for an exception class, such as
        the string "PyExc_MemoryError", set the exception state to the
        (PyObject*) for said exception class.

        The list of standard exception classes can be seen at:
          http://docs.python.org/c-api/exceptions.html#standard-exceptions
        """
        check_isinstance(exc_name, str)
        exc_decl = gccutils.get_global_vardecl_by_name(exc_name)
        check_isinstance(exc_decl, gcc.VarDecl)
        exc_region = self.var_region(exc_decl)
        self.exception_rvalue = exc_region

    def impl_object_ctor(self, stmt, typename, typeobjname):
        """
        Given a gcc.GimpleCall to a Python API function that returns a
        PyObject*, generate a
           (newobj, success, failure)
        triple, where newobj is a region, and success/failure are Transitions
        """
        check_isinstance(stmt, gcc.GimpleCall)
        check_isinstance(stmt.fn.operand, gcc.FunctionDecl)
        check_isinstance(typename, str)
        # the C struct for the type

        check_isinstance(typeobjname, str)
        # the C identifier of the global PyTypeObject for the type

        # Get the gcc.VarDecl for the global PyTypeObject
        typeobjdecl = gccutils.get_global_vardecl_by_name(typeobjname)
        check_isinstance(typeobjdecl, gcc.VarDecl)

        fnname = stmt.fn.operand.name

        # Allocation and assignment:
        success = self.copy()
        success.loc = self.loc.next_loc()

        # Set up type object:
        typeobjregion = success.var_region(typeobjdecl)
        tp_dealloc = success.make_field_region(typeobjregion, 'tp_dealloc')
        type_of_tp_dealloc = gccutils.get_field_by_name(get_PyTypeObject().type,
                                                        'tp_dealloc').type
        success.value_for_region[tp_dealloc] = GenericTpDealloc(type_of_tp_dealloc,
                                                                stmt.loc)

        nonnull = success.make_heap_region(typename, stmt)
        ob_refcnt = success.make_field_region(nonnull, 'ob_refcnt') # FIXME: this should be a memref and fieldref
        success.value_for_region[ob_refcnt] = RefcountValue.new_ref()
        ob_type = success.make_field_region(nonnull, 'ob_type')
        success.value_for_region[ob_type] = PointerToRegion(get_PyTypeObject().pointer,
                                                            stmt.loc,
                                                            typeobjregion)
        success.assign(stmt.lhs,
                       PointerToRegion(stmt.lhs.type,
                                       stmt.loc,
                                       nonnull),
                       stmt.loc)
        success = Transition(self,
                             success,
                             '%s() succeeds' % fnname)
        failure = self.make_assignment(stmt.lhs,
                                       ConcreteValue(stmt.lhs.type, stmt.loc, 0),
                                       '%s() fails' % fnname)
        failure.dest.set_exception('PyExc_MemoryError')
        return (nonnull, success, failure)

    def make_concrete_return_of(self, stmt, value):
        """
        Clone this state (at a function call), updating the location, and
        setting the result of the call to the given concrete value
        """
        newstate = self.copy()
        newstate.loc = self.loc.next_loc()
        if stmt.lhs:
            newstate.assign(stmt.lhs,
                            ConcreteValue(stmt.lhs.type, stmt.loc, value),
                            stmt.loc)
        return newstate

    def steal_reference(self, region):
        log('steal_reference(%r)', region)
        check_isinstance(region, Region)
        ob_refcnt = self.make_field_region(region, 'ob_refcnt')
        value = self.value_for_region[ob_refcnt]
        if isinstance(value, RefcountValue):
            # We have a value known relative to all of the refs owned by the
            # rest of the program.  Given that the rest of the program is
            # stealing a ref, that is increasing by one, hence our value must
            # go down by one:
            self.value_for_region[ob_refcnt] = RefcountValue(value.relvalue - 1,
                                                             value.min_external + 1)

    def make_borrowed_ref(self, stmt, name):
        """Make a new state, giving a borrowed ref to some object"""
        newstate = self.copy()
        newstate.loc = self.loc.next_loc()

        nonnull = newstate.make_heap_region(name, stmt)
        ob_refcnt = newstate.make_field_region(nonnull, 'ob_refcnt') # FIXME: this should be a memref and fieldref
        newstate.value_for_region[ob_refcnt] = RefcountValue.borrowed_ref()
        #ob_type = newstate.make_field_region(nonnull, 'ob_type')
        #newstate.value_for_region[ob_type] = PointerToRegion(get_PyTypeObject().pointer,
        #                                                    stmt.loc,
        #                                                    typeobjregion)
        newstate.assign(stmt.lhs,
                       PointerToRegion(stmt.lhs.type,
                                       stmt.loc,
                                       nonnull),
                       stmt.loc)
        return newstate

    def make_exception(self, stmt, fnname):
        """Make a new state, giving NULL and some exception"""
        failure = self.make_assignment(stmt.lhs,
                                       ConcreteValue(stmt.lhs.type, stmt.loc, 0),
                                       None)
        failure.dest.set_exception('PyExc_MemoryError')
        return failure.dest


    def make_transitions_for_fncall(self, stmt, success, failure):
        check_isinstance(stmt, gcc.GimpleCall)
        check_isinstance(success, State)
        check_isinstance(failure, State)

        fnname = stmt.fn.operand.name

        return [Transition(self, success, '%s() succeeds' % fnname),
                Transition(self, failure, '%s() fails' % fnname)]

    # Specific Python API function implementations:
    def impl_PyList_New(self, stmt):
        # Decl:
        #   PyObject* PyList_New(Py_ssize_t len)
        # Returns a new reference, or raises MemoryError
        lenarg = self.eval_rvalue(stmt.args[0], stmt.loc)
        check_isinstance(lenarg, AbstractValue)
        newobj, success, failure = self.impl_object_ctor(stmt,
                                                         'PyListObject', 'PyList_Type')
        # Set ob_size:
        ob_size = success.dest.make_field_region(newobj, 'ob_size')
        success.dest.value_for_region[ob_size] = lenarg

        # "Allocate" ob_item, and set it up so that all of the array is
        # treated as NULL:
        ob_item_region = success.dest.make_heap_region(
            'ob_item array for PyListObject',
            stmt)
        success.dest.value_for_region[ob_item_region] = \
            ConcreteValue(get_PyObjectPtr(),
                          stmt.loc, 0)

        ob_item = success.dest.make_field_region(newobj, 'ob_item')
        success.dest.value_for_region[ob_item] = PointerToRegion(get_PyObjectPtr().pointer,
                                                                 stmt.loc,
                                                                 ob_item_region)

        return [success, failure]

    def impl_PyLong_FromLong(self, stmt):
        newobj, success, failure = self.impl_object_ctor(stmt,
                                                         'PyLongObject', 'PyLong_Type')
        return [success, failure]

    def impl_PyList_SetItem(self, stmt):
        # Decl:
        #   int PyList_SetItem(PyObject *list, Py_ssize_t index, PyObject *item)
        fnname = stmt.fn.operand.name

        result = []

        arg_list, arg_index, arg_item = [self.eval_rvalue(arg, stmt.loc)
                                         for arg in stmt.args]

        # Is it really a list?
        if 0: # FIXME: check
            not_a_list = self.make_concrete_return_of(stmt, -1)
            result.append(Transition(self,
                           not_a_list,
                           '%s() fails (not a list)' % fnname))

        # Index out of range?
        if 0: # FIXME: check
            out_of_range = self.make_concrete_return_of(stmt, -1)
            result.append(Transition(self,
                           out_of_range,
                           '%s() fails (index out of range)' % fnname))

        if 1:
            success  = self.make_concrete_return_of(stmt, 0)
            # FIXME: update refcounts
            # "Steal" a reference to item:
            if isinstance(arg_item, PointerToRegion):
                check_isinstance(arg_item.region, Region)
                success.steal_reference(arg_item.region)

            # and discards a
            # reference to an item already in the list at the affected position.
            result.append(Transition(self,
                                     success,
                                     '%s() succeeds' % fnname))

        return result

    def impl_PyArg_ParseTuple(self, stmt):
        # Decl:
        #   PyAPI_FUNC(int) PyArg_ParseTuple(PyObject *, const char *, ...) Py_FORMAT_PARSETUPLE(PyArg_ParseTuple, 2, 3);
        # Also:
        #   #define PyArg_ParseTuple		_PyArg_ParseTuple_SizeT

        success = self.make_concrete_return_of(stmt, 1)

        failure = self.make_concrete_return_of(stmt, 0)
        # Various errors are possible, but a TypeError is always possible
        # e.g. for the case of the wrong number of arguments:
        failure.set_exception('PyExc_TypeError')

        return self.make_transitions_for_fncall(stmt, success, failure)

    def impl_Py_InitModule4_64(self, stmt):
        # Decl:
        #   PyAPI_FUNC(PyObject *) Py_InitModule4(const char *name, PyMethodDef *methods,
        #                                         const char *doc, PyObject *self,
        #                                         int apiver);
        #  Returns a borrowed reference
        #
        # FIXME:
        #  On 64-bit:
        #    #define Py_InitModule4 Py_InitModule4_64
        #  with tracerefs:
        #    #define Py_InitModule4 Py_InitModule4TraceRefs_64
        #    #define Py_InitModule4 Py_InitModule4TraceRefs
        success = self.make_borrowed_ref(stmt, 'output from Py_InitModule4')
        failure = self.make_exception(stmt, 'Py_InitModule4')
        return self.make_transitions_for_fncall(stmt, success, failure)

    def _get_transitions_for_GimpleCall(self, stmt):
        log('stmt.lhs: %s %r', stmt.lhs, stmt.lhs)
        log('stmt.fn: %s %r', stmt.fn, stmt.fn)
        log('dir(stmt.fn): %s', dir(stmt.fn))
        if hasattr(stmt.fn, 'operand'):
            log('stmt.fn.operand: %s', stmt.fn.operand)
        returntype = stmt.fn.type.dereference.type
        log('returntype: %s', returntype)

        if stmt.noreturn:
            # The function being called does not return e.g. "exit(0);"
            # Transition to a special noreturn state:
            newstate = self.copy()
            newstate.not_returning = True
            return [Transition(self,
                               newstate,
                               'not returning from %s' % stmt.fn)]

        if isinstance(stmt.fn, gcc.VarDecl):
            # Calling through a function pointer:
            val = self.eval_rvalue(stmt.fn, stmt.loc)
            log('val: %s',  val)
            check_isinstance(val, AbstractValue)
            return val.get_transitions_for_function_call(self, stmt)

        if isinstance(stmt.fn.operand, gcc.FunctionDecl):
            log('dir(stmt.fn.operand): %s', dir(stmt.fn.operand))
            log('stmt.fn.operand.name: %r', stmt.fn.operand.name)
            fnname = stmt.fn.operand.name

            # Hand off to impl_* methods, where these exist:
            methname = 'impl_%s' % fnname
            if hasattr(self, methname):
                meth = getattr(self, 'impl_%s' % fnname)
                return meth(stmt)

            #from libcpychecker.c_stdio import c_stdio_functions, handle_c_stdio_function

            #if fnname in c_stdio_functions:
            #    return handle_c_stdio_function(self, fnname, stmt)

            # Unknown function:
            log('Invocation of unknown function: %r', fnname)
            return [self.make_assignment(stmt.lhs,
                                         UnknownValue(returntype, stmt.loc),
                                         None)]

        log('stmt.args: %s %r', stmt.args, stmt.args)
        for i, arg in enumerate(stmt.args):
            log('args[%i]: %s %r', i, arg, arg)

    def _get_transitions_for_GimpleCond(self, stmt):
        def make_transition_for_true(stmt):
            e = true_edge(self.loc.bb)
            assert e
            nextstate = self.update_loc(Location.get_block_start(e.dest))
            nextstate.prior_bool = True
            return Transition(self, nextstate, 'taking True path')

        def make_transition_for_false(stmt):
            e = false_edge(self.loc.bb)
            assert e
            nextstate = self.update_loc(Location.get_block_start(e.dest))
            nextstate.prior_bool = False
            return Transition(self, nextstate, 'taking False path')

        log('stmt.exprcode: %s', stmt.exprcode)
        log('stmt.exprtype: %s', stmt.exprtype)
        log('stmt.lhs: %r %s', stmt.lhs, stmt.lhs)
        log('stmt.rhs: %r %s', stmt.rhs, stmt.rhs)
        log('dir(stmt.lhs): %s', dir(stmt.lhs))
        log('dir(stmt.rhs): %s', dir(stmt.rhs))
        boolval = self.eval_condition(stmt)
        if boolval is True:
            log('taking True edge')
            nextstate = make_transition_for_true(stmt)
            return [nextstate]
        elif boolval is False:
            log('taking False edge')
            nextstate = make_transition_for_false(stmt)
            return [nextstate]
        else:
            check_isinstance(boolval, UnknownValue)
            # We don't have enough information; both branches are possible:
            return [make_transition_for_true(stmt),
                    make_transition_for_false(stmt)]

    def eval_condition(self, stmt):
        def is_equal(lhs, rhs):
            check_isinstance(lhs, AbstractValue)
            check_isinstance(rhs, AbstractValue)
            if isinstance(rhs, ConcreteValue):
                if isinstance(lhs, PointerToRegion) and rhs.value == 0:
                    log('ptr to region vs 0: %s is definitely not equal to %s', lhs, rhs)
                    return False
                if isinstance(lhs, ConcreteValue):
                    log('comparing concrete values: %s %s', lhs, rhs)
                    return lhs.value == rhs.value
                if isinstance(lhs, RefcountValue):
                    log('comparing refcount value %s with concrete value: %s', lhs, rhs)
                    # The actual value of ob_refcnt >= lhs.relvalue
                    if lhs.relvalue > rhs.value:
                        # (Equality is thus not possible for this case)
                        return False
            if isinstance(rhs, PointerToRegion):
                if isinstance(lhs, PointerToRegion):
                    log('comparing regions: %s %s', lhs, rhs)
                    return lhs.region == rhs.region
            # We don't know:
            return None

        log('eval_condition: %s', stmt)
        lhs = self.eval_rvalue(stmt.lhs, stmt.loc)
        rhs = self.eval_rvalue(stmt.rhs, stmt.loc)
        log('eval of lhs: %r', lhs)
        log('eval of rhs: %r', rhs)
        log('stmt.exprcode: %r', stmt.exprcode)
        if stmt.exprcode == gcc.EqExpr:
            result = is_equal(lhs, rhs)
            if result is not None:
                return result
        elif stmt.exprcode == gcc.NeExpr:
            result = is_equal(lhs, rhs)
            if result is not None:
                return not result

        # Specialcasing: comparison of unknown ptr with NULL:
        if (isinstance(stmt.lhs, gcc.VarDecl)
            and isinstance(stmt.rhs, gcc.IntegerCst)
            and isinstance(stmt.lhs.type, gcc.PointerType)):
            # Split the ptr variable immediately into NULL and non-NULL
            # versions, so that we can evaluate the true and false branch with
            # explicitly data
            log('splitting %s into non-NULL/NULL pointers', stmt.lhs)
            self.raise_split_value(lhs, stmt.loc)

        log('unable to compare %r with %r', lhs, rhs)
        return UnknownValue(stmt.lhs.type, stmt.loc)

    def eval_rhs(self, stmt):
        log('eval_rhs(%s): %s', stmt, stmt.rhs)
        rhs = stmt.rhs
        if stmt.exprcode == gcc.PlusExpr:
            a = self.eval_rvalue(rhs[0], stmt.loc)
            b = self.eval_rvalue(rhs[1], stmt.loc)
            log('a: %r', a)
            log('b: %r', b)
            if isinstance(a, ConcreteValue) and isinstance(b, ConcreteValue):
                return ConcreteValue(stmt.lhs.type, stmt.loc, a.value + b.value)
            if isinstance(a, RefcountValue) and isinstance(b, ConcreteValue):
                return RefcountValue(a.relvalue + b.value, a.min_external)

            return UnknownValue(stmt.lhs.type, stmt.loc)

            raise NotImplementedError("Don't know how to cope with addition of\n  %r\nand\n  %r\nat %s"
                                      % (a, b, stmt.loc))
        elif stmt.exprcode == gcc.MinusExpr:
            a = self.eval_rvalue(rhs[0], stmt.loc)
            b = self.eval_rvalue(rhs[1], stmt.loc)
            log('a: %r', a)
            log('b: %r', b)
            if isinstance(a, RefcountValue) and isinstance(b, ConcreteValue):
                return RefcountValue(a.relvalue - b.value, a.min_external)
            raise NotImplementedError("Don't know how to cope with subtraction of\n  %r\nand\n  %rat %s"
                                      % (a, b, stmt.loc))
        elif stmt.exprcode == gcc.ComponentRef:
            return self.eval_rvalue(rhs[0], stmt.loc)
        elif stmt.exprcode == gcc.VarDecl:
            return self.eval_rvalue(rhs[0], stmt.loc)
        elif stmt.exprcode == gcc.ParmDecl:
            return self.eval_rvalue(rhs[0], stmt.loc)
        elif stmt.exprcode == gcc.IntegerCst:
            return self.eval_rvalue(rhs[0], stmt.loc)
        elif stmt.exprcode == gcc.AddrExpr:
            return self.eval_rvalue(rhs[0], stmt.loc)
        elif stmt.exprcode == gcc.NopExpr:
            return self.eval_rvalue(rhs[0], stmt.loc)
        elif stmt.exprcode == gcc.ArrayRef:
            return self.eval_rvalue(rhs[0], stmt.loc)
        elif stmt.exprcode == gcc.MemRef:
            return self.eval_rvalue(rhs[0], stmt.loc)
        elif stmt.exprcode == gcc.PointerPlusExpr:
            region = self.pointer_plus_region(stmt)
            return PointerToRegion(stmt.lhs.type, stmt.loc, region)
        else:
            raise NotImplementedError("Don't know how to cope with exprcode: %r (%s) at %s"
                                      % (stmt.exprcode, stmt.exprcode, stmt.loc))

    def _get_transitions_for_GimpleAssign(self, stmt):
        log('stmt.lhs: %r %s', stmt.lhs, stmt.lhs)
        log('stmt.rhs: %r %s', stmt.rhs, stmt.rhs)
        log('stmt: %r %s', stmt, stmt)
        log('stmt.exprcode: %r', stmt.exprcode)

        value = self.eval_rhs(stmt)
        log('value from eval_rhs: %r', value)
        check_isinstance(value, AbstractValue)

        if isinstance(value, DeallocatedMemory):
            raise ReadFromDeallocatedMemory(stmt, value)

        nextstate = self.use_next_loc()
        """
        if isinstance(stmt.lhs, gcc.MemRef):
            log('value: %s %r', value, value)
            # We're writing a value to memory; if it's a PyObject*
            # then we're surrending a reference on it:
            if value in nextstate.owned_refs:
                log('removing ownership of %s', value)
                nextstate.owned_refs.remove(value)
        """
        return [self.make_assignment(stmt.lhs,
                                     value,
                                     None)]

    def _get_transitions_for_GimpleReturn(self, stmt):
        #log('stmt.lhs: %r %s', stmt.lhs, stmt.lhs)
        #log('stmt.rhs: %r %s', stmt.rhs, stmt.rhs)
        log('stmt: %r %s', stmt, stmt)
        log('stmt.retval: %r', stmt.retval)

        nextstate = self.copy()

        if stmt.retval:
            rvalue = self.eval_rvalue(stmt.retval, stmt.loc)
            log('rvalue from eval_rvalue: %r', rvalue)
            nextstate.return_rvalue = rvalue
        nextstate.has_returned = True
        return [Transition(self, nextstate, 'returning')]

    def _get_transitions_for_GimpleSwitch(self, stmt):
        def get_labels_for_rvalue(self, stmt, rvalue):
            # Gather all possible labels for the given rvalue
            result = []
            for label in stmt.labels:
                # FIXME: for now, treat all labels as possible:
                result.append(label)
            return result
        log('stmt.indexvar: %r', stmt.indexvar)
        log('stmt.labels: %r', stmt.labels)
        indexval = self.eval_rvalue(stmt.indexvar, stmt.loc)
        log('indexval: %r', indexval)
        labels = get_labels_for_rvalue(self, stmt, indexval)
        log('labels: %r', labels)
        result = []
        for label in labels:
            newstate = self.copy()
            bb = self.fun.cfg.get_block_for_label(label.target)
            newstate.loc = Location(bb, 0)
            if label.low:
                check_isinstance(label.low, gcc.IntegerCst)
                if label.high:
                    check_isinstance(label.high, gcc.IntegerCst)
                    desc = 'following cases %i...%i' % (label.low.constant, label.high.constant)
                else:
                    desc = 'following case %i' % label.low.constant
            else:
                desc = 'following default'
            result.append(Transition(self,
                                     newstate,
                                     desc))
        return result

    def get_persistent_refs_for_region(self, dst_region):
        # Locate all regions containing pointers that point at the given region
        # that are either on the heap or are globals (not locals)
        check_isinstance(dst_region, Region)
        result = []
        for src_region in self.get_all_refs_for_region(dst_region):
            if src_region.is_on_stack():
                continue
            result.append(src_region)
        return result

    def get_all_refs_for_region(self, dst_region):
        # Locate all regions containing pointers that point at the given region
        check_isinstance(dst_region, Region)
        result = []
        for src_region in self.value_for_region:
            v = self.value_for_region[src_region]
            if isinstance(v, PointerToRegion):
                if v.region == dst_region:
                    result.append(src_region)
        return result

def get_traces(fun):
    return list(iter_traces(fun, MyState))

def dump_traces_to_stdout(traces):
    """
    For use in selftests: dump the traces to stdout, in a form that (hopefully)
    will allow usable comparisons against "gold" output ( not embedding
    anything that changes e.g. names of temporaries, address of wrapper
    objects, etc)
    """
    def dump_object(rvalue, title):
        check_isinstance(rvalue, AbstractValue)
        print('  %s:' % title)
        print('    repr(): %r' % rvalue)
        print('    str(): %s' % rvalue)
        if isinstance(rvalue, PointerToRegion):
            print('    r->ob_refcnt: %s'
                  % endstate.get_value_of_field_by_region(rvalue.region, 'ob_refcnt'))
            print('    r->ob_type: %r'
                  % endstate.get_value_of_field_by_region(rvalue.region, 'ob_type'))

    def dump_region(region, title):
        check_isinstance(region, Region)
        print('  %s:' % title)
        print('    repr(): %r' % region)
        print('    str(): %s' % region)
        print('    r->ob_refcnt: %s'
              % endstate.get_value_of_field_by_region(region, 'ob_refcnt'))
        print('    r->ob_type: %r'
              % endstate.get_value_of_field_by_region(region, 'ob_type'))

    for i, trace in enumerate(traces):
        print('Trace %i:' % i)

        # Emit the "interesting transitions" i.e. those with descriptions:
        print('  Transitions:')
        for trans in trace.transitions:
            if trans.desc:
                print('    %r' % trans.desc)

        # Emit information about the end state:
        endstate = trace.states[-1]

        if trace.err:
            print('  error: %r' % trace.err)
            print('  error: %s' % trace.err)

        if endstate.return_rvalue:
            dump_object(endstate.return_rvalue, 'Return value')

        # Other affected PyObject instances:
        for k in endstate.region_for_var:
            if not isinstance(endstate.region_for_var[k], Region):
                continue
            region = endstate.region_for_var[k]

            # Consider those for which we know something about an "ob_refcnt"
            # field:
            if 'ob_refcnt' not in region.fields:
                continue

            if (isinstance(endstate.return_rvalue, PointerToRegion)
                and region == endstate.return_rvalue.region):
                # (We did the return value above)
                continue

            dump_region(region, str(region))

        # Exception state:
        print('  Exception:')
        print('    %s' % endstate.exception_rvalue)

        if i + 1 < len(traces):
            sys.stdout.write('\n')

class RefcountAnnotator(Annotator):
    """
    Annotate a trace with information on the reference count of a particular
    object
    """
    def __init__(self, region):
        check_isinstance(region, Region)
        self.region = region

    def get_notes(self, transition):
        """
        Add a note to every transition that affects reference-counting for
        our target object
        """
        loc = transition.src.get_gcc_loc_or_none()
        if loc is None:
            # (we can't add a note without a valid location)
            return []

        result = []

        # Add a note when the ob_refcnt of the object changes:
        src_refcnt = transition.src.get_value_of_field_by_region(self.region,
                                                                 'ob_refcnt')
        dest_refcnt = transition.dest.get_value_of_field_by_region(self.region,
                                                                   'ob_refcnt')
        if src_refcnt != dest_refcnt:
            log('src_refcnt: %r', src_refcnt)
            log('dest_refcnt: %r', dest_refcnt)
            result.append(Note(loc,
                               ('ob_refcnt is now %s' % dest_refcnt)))

        # Add a note when there's a change to the set of persistent storage
        # locations referencing this object:
        src_refs = transition.src.get_persistent_refs_for_region(self.region)
        dest_refs = transition.dest.get_persistent_refs_for_region(self.region)
        if src_refs != dest_refs:
            result.append(Note(loc,
                               ('%s is now referenced by %i non-stack value(s): %s'
                                % (self.region.name,
                                   len(dest_refs),
                                   ', '.join([ref.name for ref in dest_refs])))))

        if 0:
            # For debugging: show the history of all references to the given
            # object:
            src_refs = transition.src.get_all_refs_for_region(self.region)
            dest_refs = transition.dest.get_all_refs_for_region(self.region)
            if src_refs != dest_refs:
                result.append(Note(loc,
                                   ('all refs: %s' % dest_refs)))
        return result

def check_refcounts(fun, dump_traces=False, show_traces=False):
    """
    The top-level function of the refcount checker, checking the refcounting
    behavior of a function

    fun: the gcc.Function to be checked

    dump_traces: bool: if True, dump information about the traces through
    the function to stdout (for self tests)

    show_traces: bool: if True, display a diagram of the state transition graph
    """
    # Abstract interpretation:
    # Walk the CFG, gathering the information we're interested in

    log('check_refcounts(%r, %r, %r)', fun, dump_traces, show_traces)

    check_isinstance(fun, gcc.Function)

    if show_traces:
        from libcpychecker.visualizations import StateGraphPrettyPrinter
        sg = StateGraph(fun, log, MyState)
        sgpp = StateGraphPrettyPrinter(sg)
        dot = sgpp.to_dot()
        #dot = sgpp.extra_items()
        # print(dot)
        invoke_dot(dot)

    traces = iter_traces(fun, MyState)
    if dump_traces:
        traces = list(traces)
        dump_traces_to_stdout(traces)

    rep = Reporter()

    for i, trace in enumerate(traces):
        trace.log(log, 'TRACE %i' % i)
        if trace.err:
            # This trace bails early with a fatal error; it probably doesn't
            # have a return value
            log('trace.err: %s %r', trace.err, trace.err)
            err = rep.make_error(fun, trace.err.loc, str(trace.err))
            err.add_trace(trace)
            # FIXME: in our example this ought to mention where the values came from
            continue
        # Otherwise, the trace proceeds normally
        return_value = trace.return_value()
        log('trace.return_value(): %s', trace.return_value())

        # Ideally, we should "own" exactly one reference, and it should be
        # the return value.  Anything else is an error (and there are other
        # kinds of error...)

        # Locate all PyObject that we touched
        endstate = trace.states[-1]
        endstate.log(log)
        log('return_value: %r', return_value)
        log('endstate.region_for_var: %r', endstate.region_for_var)
        log('endstate.value_for_region: %r', endstate.value_for_region)

        # Consider all regions of memory we know about:
        for k in endstate.region_for_var:
            if not isinstance(endstate.region_for_var[k], Region):
                continue
            region = endstate.region_for_var[k]

            log('considering ob_refcnt of %r', region)
            check_isinstance(region, Region)

            # Consider those for which we know something about an "ob_refcnt"
            # field:
            if 'ob_refcnt' not in region.fields:
                continue

            ob_refcnt = endstate.get_value_of_field_by_region(region,
                                                              'ob_refcnt')
            log('ob_refcnt: %r', ob_refcnt)

            # If it's the return value, it should have a net refcnt delta of
            # 1; all other PyObject should have a net delta of 0:
            if isinstance(return_value, PointerToRegion) and region == return_value.region:
                desc = 'return value'
                exp_refs = ['return value']
            else:
                desc = 'PyObject'
                # We may have a more descriptive name within the region:
                if isinstance(region, RegionOnHeap):
                    desc = region.name
                exp_refs = []

            # The reference count should also reflect any non-stack pointers
            # that point at this object:
            exp_refs += [ref.name
                         for ref in endstate.get_persistent_refs_for_region(region)]
            exp_refcnt = len(exp_refs)
            log('exp_refs: %r', exp_refs)

            # Helper function for when ob_refcnt is wrong:
            def emit_refcount_error(msg):
                err = rep.make_error(fun, endstate.get_gcc_loc(fun), msg)
                err.add_note(endstate.get_gcc_loc(fun),
                             ('was expecting final ob_refcnt to be N + %i (for some unknown N)'
                              % exp_refcnt))
                if exp_refcnt > 0:
                    err.add_note(endstate.get_gcc_loc(fun),
                                 ('due to object being referenced by: %s'
                                  % ', '.join(exp_refs)))
                err.add_note(endstate.get_gcc_loc(fun),
                             ('but final ob_refcnt is N + %i'
                              % ob_refcnt.relvalue))
                # For dynamically-allocated objects, indicate where they
                # were allocated:
                if isinstance(region, RegionOnHeap):
                    alloc_loc = region.alloc_stmt.loc
                    if alloc_loc:
                        err.add_note(region.alloc_stmt.loc,
                                     ('%s allocated at: %s'
                                      % (region.name,
                                         get_src_for_loc(alloc_loc))))

                # Summarize the control flow we followed through the function:
                if 1:
                    annotator = RefcountAnnotator(region)
                else:
                    # Debug help:
                    from libcpychecker.diagnostics import TestAnnotator
                    annotator = TestAnnotator()
                err.add_trace(trace, annotator)

                if 0:
                    # Handy for debugging:
                    err.add_note(endstate.get_gcc_loc(fun),
                                 'this was trace %i' % i)
                return err

            # Here's where we verify the refcount:
            if isinstance(ob_refcnt, RefcountValue):
                if ob_refcnt.relvalue > exp_refcnt:
                    # Refcount is too high:
                    err = emit_refcount_error('ob_refcnt of %s is %i too high'
                                              % (desc, ob_refcnt.relvalue - exp_refcnt))
                elif ob_refcnt.relvalue < exp_refcnt:
                    # Refcount is too low:
                    err = emit_refcount_error('ob_refcnt of %s is %i too low'
                                              % (desc, exp_refcnt - ob_refcnt.relvalue))
                    # Special-case hint for when None has too low a refcount:
                    if isinstance(return_value.region, RegionForGlobal):
                        if return_value.region.vardecl.name == '_Py_NoneStruct':
                            err.add_note(endstate.get_gcc_loc(fun),
                                         'consider using "Py_RETURN_NONE;"')

        # Detect returning a deallocated object:
        if return_value:
            if isinstance(return_value, PointerToRegion):
                rvalue = endstate.value_for_region.get(return_value.region, None)
                if isinstance(rvalue, DeallocatedMemory):
                    err = rep.make_error(fun,
                                         endstate.get_gcc_loc(fun),
                                         'returning pointer to deallocated memory')
                    err.add_trace(trace)
                    err.add_note(rvalue.loc,
                                 'memory deallocated here')

        # Detect failure to set exceptions when returning NULL:
        if not trace.err:
            if (isinstance(return_value, ConcreteValue)
                and return_value.value == 0
                and str(return_value.gcctype)=='struct PyObject *'):

                if (isinstance(endstate.exception_rvalue,
                              ConcreteValue)
                    and endstate.exception_rvalue.value == 0):
                    err = rep.make_error(fun,
                                         endstate.get_gcc_loc(fun),
                                         'returning (PyObject*)NULL without setting an exception')
                    err.add_trace(trace)

    # (all traces analysed)

    if rep.got_errors():
        filename = ('%s.%s-refcount-errors.html'
                    % (gcc.get_dump_base_name(), fun.decl.name))
        rep.dump_html(fun, filename)
        gcc.inform(fun.start,
                   ('graphical error report for function %r written out to %r'
                    % (fun.decl.name, filename)))

    if 0:
        dot = cfg_to_dot(fun.cfg)
        invoke_dot(dot)


