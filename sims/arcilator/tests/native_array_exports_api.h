// Public DPI C types, independently checked against generated DPI headers.
#pragma once
#include <svdpi.h>
#ifdef __cplusplus
extern "C" {
#endif
int array_atoms(const int *, char *, short *);
void array_vectors(const svLogicVecVal *, svLogicVecVal *, svBitVecVal *);
int array_pointers(const char **, const char **, const void **, void **);
void array_saved_pointers(const char **, void **);
int array_bits(const svLogic *, svBit *, svLogic *);
int array_saved();
void array_zero(svBitVecVal *);
long long signed_atoms(char, short, int, long long, char *, long long *);
unsigned long long unsigned_atoms(unsigned char, unsigned short, unsigned int,
                                  unsigned long long, unsigned char *,
                                  unsigned long long *);
void array_longs(const unsigned long long *, long long *, unsigned long long *);
#ifdef __cplusplus
}
#endif
