/* C11 syntax makes compiling this DPI driver as C++ a regression failure. */
int arcilator_c_driver(int value) {
    return _Generic(value, int: value ^ 0x13579bdf, default: 0);
}
