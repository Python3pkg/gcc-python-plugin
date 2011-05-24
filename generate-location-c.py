from cpybuilder import *

cu = CompilationUnit()
cu.add_include('gcc-python.h')
cu.add_include('gcc-python-wrappers.h')
cu.add_include('gcc-plugin.h')
cu.add_include("tree.h")

modinit_preinit = ''
modinit_postinit = ''

def generate_location():
    #
    # Generate the gcc.Location class:
    #
    global modinit_preinit
    global modinit_postinit

    cu.add_defn("""
static PyObject *
gcc_Location_get_file(struct PyGccLocation *self, void *closure)
{
    return gcc_python_string_from_string(LOCATION_FILE(self->loc));
}
""")

    cu.add_defn("""
static PyObject *
gcc_Location_get_line(struct PyGccLocation *self, void *closure)
{
    return gcc_python_int_from_long(LOCATION_LINE(self->loc));
}
""")

    cu.add_defn("""
static PyObject *
gcc_Location_get_column(struct PyGccLocation *self, void *closure)
{
    expanded_location exploc = expand_location(self->loc);

    return gcc_python_int_from_long(exploc.column);
}
""")

    getsettable = PyGetSetDefTable('gcc_Location_getset_table',
                                   [PyGetSetDef('file', 'gcc_Location_get_file', None, 'Name of the source file'),
                                    PyGetSetDef('line', 'gcc_Location_get_line', None, 'Line number within source file'),
                                    PyGetSetDef('column', 'gcc_Location_get_column', None, 'Column number within source file'),
                                    ])
    cu.add_defn(getsettable.c_defn())

    pytype = PyTypeObject(identifier = 'gcc_LocationType',
                          localname = 'Location',
                          tp_name = 'gcc.Location',
                          struct_name = 'struct PyGccLocation',
                          tp_new = 'PyType_GenericNew',
                          tp_getset = getsettable.identifier,
                          tp_repr = '(reprfunc)gcc_Location_repr',
                          tp_str = '(reprfunc)gcc_Location_str',
                          tp_richcompare = 'gcc_Location_richcompare')
    cu.add_defn(pytype.c_defn())
    modinit_preinit += pytype.c_invoke_type_ready()
    modinit_postinit += pytype.c_invoke_add_to_module()

generate_location()

cu.add_defn("""
int autogenerated_location_init_types(void)
{
""" + modinit_preinit + """
    return 1;

error:
    return 0;
}
""")

cu.add_defn("""
void autogenerated_location_add_types(PyObject *m)
{
""" + modinit_postinit + """
}
""")



print(cu.as_str())
